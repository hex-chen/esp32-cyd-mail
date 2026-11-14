# esp32-cyd-mail

CYD 一体板（ESP32-2432S028R，2.8" ILI9341）上的邮件提醒器。

- 服务端在家里 Linux 服务器上轮询 Gmail IMAP，把最新邮件的**中文**发件人/主题用 Noto Sans CJK 渲染成 1bit 点阵
- 板子走 WiFi 独立供电，每 15 秒拉一次点阵并贴图，板子不需要任何中文字库
- 有新邮件 → 亮屏 10 秒 → 关背光（内容保留）
- 按板载 BOOT(GPIO0) → 亮屏 20 秒查看；亮屏期间再按一次立即熄灭

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

其他接口：`GET /api/health` 看状态；`POST /api/fake` 推一封假邮件（联调用）。

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
