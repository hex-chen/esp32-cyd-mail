# esp32-cyd-mail

CYD 一体板（ESP32-2432S028R，2.8" ILI9341）上的邮件提醒器。

- 服务端在家里 Linux 服务器上轮询 Gmail IMAP，把最新邮件的**中文**发件人/主题用 Noto Sans CJK 渲染成 1bit 点阵
- 板子走 WiFi 独立供电，每 15 秒拉一次点阵并贴图，板子不需要任何中文字库
- 有新邮件 → 亮屏 10 秒 → 自动关背光（内容保留）
- 服务端持有**未读邮件列表**（Gmail `UNSEEN`），按索引取；板子短按可循环翻未读
- 板子本地滑动窗口只缓存 **3 封**；翻到窗口外才向服务端请求该封（翻一封发一封），并预取下一封，减小卡顿
- BOOT(GPIO0) 三态操作：
  - **灭屏短按** → 亮屏看邮件，**常亮不自动熄**
  - **亮屏短按** → 循环翻下一封未读，**常亮不自动熄**（等 350ms 确认无第二下才翻）
  - **双击（350ms 内两次）** → 标记当前封已读（服务端 IMAP `\Seen`），从列表移除并显示「Marked read」提示
  - **亮屏长按 ≥1 秒** → 立即熄屏
- 只有**新邮件**（亮 10 秒）和开机 splash（3 秒）会自动熄屏；手动亮屏的只能长按关
- 无未读时显示「暂无未读邮件」占位卡
- 支持 **OTA 远程升级**：板子每约 60 秒查一次服务端固件版本，有新版自动下载、MD5 校验、刷写、重启；刷写失败 bootloader 自动回滚

```
Gmail IMAP --(代理 CONNECT)--> 服务器 mail_notify.py --(HTTP 点阵)--> CYD 板子
```

## 目录

| 路径 | 内容 |
|---|---|
| `server/` | 邮件检测、点阵渲染、HTTP 接口（部署在 `192.168.31.136:~/mail-notify/`） |
| `firmware/` | PlatformIO 固件工程 |
| `firmware/lib/TFT_eSPI` | 离线复用的屏幕驱动 2.5.43 |

## 通信协议

`GET /api/card?seen=<seq>`

响应头：

| 头 | 含义 |
|---|---|
| `X-Seq` | 当前卡片序号，单调递增 |
| `X-New` | `1` = 有新邮件，触发亮屏 10 秒。`seen=0`（板子首次同步）恒为 0，避免重启误报 |
| `X-Unread` | 未读邮件数 |
| `X-Poll` | 建议轮询间隔（秒），板子会采用 |

响应体：4800 字节 1bit 位图，240×160，MSB first（bit7 = 最左像素），行优先，每行 30 字节。`1` 画白，`0` 保持深蓝背景。格式与 TFT_eSPI `drawBitmap()` 一致，也与 Pillow `Image.new("1").tobytes()` 一致。

其他接口：`GET /api/health` 看状态；`POST /api/fake` 推一封假邮件（联调用）；`GET /api/event?k=tap&pos=&cnt=&mode=` 板子按键上报（免 USB 诊断，pm2 日志 `[event]` 可看）。

历史翻页接口：

| 接口 | 返回 |
|---|---|
| `GET /api/mail?idx=N` | 第 N 封的 4800 字节位图（0=最新未读，时间倒序），越界 404 |
| 响应头 | `X-Index` / `X-Total` / `X-Unread` |
| `GET /api/read?idx=N` | 标记第 N 封已读（IMAP `\Seen`），从列表移除，返回 `{ok,total}` |
| `GET /api/read/all` | 全部未读标已读，返回 `{ok,marked,total}` |

`/api/card` 也带 `X-Total`（历史总数）。服务端启动时会后台批量拉全部邮件 header 并渲染位图存内存（约 600KB），每 10 分钟重建一次；IMAP 访问用锁串行化，失败自动重试（Gmail 代理偶发 SSL EOF）。

OTA 接口（板子自动调用）：

| 接口 | 返回 |
|---|---|
| `GET /api/fw` | 紧凑 JSON `{"version":N,"size":B,"md5":"..."}`；无固件时 `{"version":0}` |
| `GET /api/fw.bin` | 固件二进制，长度与 `size` 一致 |

版本号在 `firmware/platformio.ini` 的 `FW_VERSION` 宏。板子只在服务端版本 **大于** 本地版本时才升级。紧凑 JSON（冒号后无空格）是为兼容旧版固件的简易解析器。

## OTA 升级流程

首次需 USB 烧录一次带 OTA 的固件（bootstrap），之后永久免 USB。升级只改版本号，不用插线：

```bash
# 1. 改 firmware/platformio.ini 的 FW_VERSION，例如 4 -> 5
# 2. 编译
~/.platformio/penv/bin/pio run -d firmware -e cyd
# 3. 上传固件 + 更新 fw.json（版本号与 FW_VERSION 一致）
scp firmware/.pio/build/cyd/firmware.bin chen@192.168.31.136:~/mail-notify/firmware/
ssh chen@192.168.31.136 'bash -s' <<'EOS'
cd ~/mail-notify
SZ=$(stat -c%s firmware/firmware.bin)
MD5=$(md5sum firmware/firmware.bin | awk '{print $1}')
printf '{"version":5,"size":%s,"md5":"%s"}' "$SZ" "$MD5" > firmware/fw.json
EOS
# 板子约 60 秒内自动检测 -> 下载 -> 校验 -> 刷写 -> 重启
# 串口可见 [ota] 日志；新固件启动后调用 esp_ota_mark_app_valid_cancel_rollback() 确认
```

注意：`fw.json` 必须是**紧凑格式**（无空格），否则旧版板子解析不出 md5。分区表已含 app0/app1 双分区 + otadata，支持回滚。

## 服务端部署

```bash
scp -r server/ chen@192.168.31.136:~/mail-notify/
ssh chen@192.168.31.136
cd ~/mail-notify
python3 -m pip install -r requirements.txt    # 只用 Pillow
cp config.example.json config.json && chmod 600 config.json
# 编辑 config.json：填 user / app_password，把 fake_mode 改成 false
pm2 start mail_notify.py --name mail-notify --interpreter python3
pm2 save
curl http://127.0.0.1:8087/api/health
```

Gmail 要用 **16 位应用专用密码**，不是登录密码。服务器在国内，`imap.gmail.com:993` 经 `127.0.0.1:7890` 代理的 HTTP CONNECT 隧道访问；Python 3.14 的 imaplib 通过覆盖 `_create_socket()` 接入隧道（`server/imap_source.py`）。IMAP 用 `select(readonly=True)` + `BODY.PEEK`，不会把邮件标成已读。

`fake_mode: true` 时不连 IMAP，可用 `POST /api/fake` 触发假邮件跑通全链路。

## 固件编译与烧录

```bash
cp firmware/include/wifi_config.example.h firmware/include/wifi_config.h
# 填 WIFI_SSID / WIFI_PASS / SERVER_HOST / SERVER_PORT
~/.platformio/penv/bin/pio run -d firmware
```

CH340 只能用 115200。不要用 `pio run -t upload`（重建会清理 `.pio/build`，可能被权限系统拦下），直接写 Flash：

```bash
P=~/.platformio/penv/bin/python
E=~/.platformio/packages/tool-esptoolpy/esptool.py
F=~/.platformio/packages/framework-arduinoespressif32@3.20014.231204/tools/partitions/boot_app0.bin
B=firmware/.pio/build/cyd
$P $E --chip esp32 --port /dev/cu.usbserial-10 --baud 115200 \
  --before default_reset --after hard_reset \
  write_flash -z --flash_mode dio --flash_freq 40m --flash_size 4MB \
  0x1000 $B/bootloader.bin 0x8000 $B/partitions.bin 0xe000 $F 0x10000 $B/firmware.bin
```

串口号 `/dev/cu.usbserial-*` 会随 USB 口变化，用前先 `ls /dev/cu.usbserial-*`。

## 屏幕引脚（CYD，已验证）

ILI9341_2_DRIVER + HSPI：MISO 12 / MOSI 13 / SCLK 14 / CS 15 / DC 2 / RST -1；背光 GPIO21 高电平点亮；240×320。全部通过 `build_flags` 宏配置，不改库里的 `User_Setup.h`。

## 安全

`server/config.json`（邮箱授权码）和 `firmware/include/wifi_config.h`（WiFi 密码）都在 `.gitignore` 里，权限 600，不会进任何仓库。
