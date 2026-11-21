/* CYD (ESP32-2432S028R) 邮件提醒固件
 *
 * 服务端持有全部历史邮件（按索引取），板子本地滑动窗口缓存 3 封。
 * 新邮件 -> 亮屏 10 秒 -> 自动关背光（内容保留）。
 *
 * BOOT(GPIO0) 交互：
 *   灭屏短按        -> 亮屏看邮件，常亮不自动熄
 *   亮屏短按        -> 循环翻下一封未读邮件，常亮不自动熄
 *   双击(350ms内两次) -> 标记当前封已读（服务端 IMAP \Seen），从列表移除
 *   亮屏长按 >=1s   -> 立即熄屏
 * 只有新邮件（ALERT）和开机 splash 会到点自动熄屏。
 * 翻到窗口外才向服务端请求该封（翻一封发一封），并预取下一封凑满 3 封缓存。
 */

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Update.h>
#include <TFT_eSPI.h>
#include "wifi_config.h"
#include "esp_ota_ops.h"

#ifndef FW_VERSION
#define FW_VERSION 1
#endif

#define CARD_W     240
#define CARD_H     160
#define CARD_BYTES (CARD_W * CARD_H / 8)   // 4800
#define BOOT_PIN   0
#define BL_PIN     21

#define ALERT_MS   10000
#define VIEW_MS    20000
#define HOLD_MS    1000
#define DOUBLE_MS  350                 // 双击判定窗口
#define POLL_MS    15000
#define HTTP_TIMEOUT_MS 6000
#define WIN_N      3                             // 本地滑动窗口缓存封数

TFT_eSPI tft = TFT_eSPI();
static uint8_t card[CARD_BYTES];                 // 收包临时缓冲
// 滑动窗口：缓存当前及相邻的历史位图
static uint8_t winBmp[WIN_N][CARD_BYTES];        // 3 封位图（14.4KB）
static int32_t winIdx[WIN_N] = {-1, -1, -1};     // 每槽对应的历史索引，-1=空（必须初始化！）
static int8_t winNext = 0;                       // 下一个写入槽（环形）

static uint32_t lastSeq = 0;                     // 0 = 首次同步
static int32_t histIdx = 0;                      // 当前查看的历史索引（0=最新）
static int32_t histTotal = 1;                    // 服务端邮件总数
static bool histLoaded = false;                  // 是否已拿到历史卡片
static uint32_t pollEvery = POLL_MS;
static uint32_t lastPoll = 0;
static uint8_t pollCount = 0;                    // 每 4 次 poll 查一次 OTA

enum Mode { DARK, ALERT, VIEW };
static Mode mode = DARK;
static uint32_t modeUntil = 0;
static bool autoOff = false;                     // true=到点自动熄屏（新邮件/开机splash）

static bool lastBoot = HIGH;
static bool pressing = false;
static uint32_t pressAt = 0;
static bool longFired = false;
static bool tapPending = false;          // 短按已松开，等待可能的第二下
static uint32_t tapAt = 0;
static uint32_t toastUntil = 0;          // >0 表示正在显示提示，到点重绘卡片

static void backlight(bool on) {
  digitalWrite(BL_PIN, on ? HIGH : LOW);
  Serial.printf("[screen] %s\n", on ? "ON" : "OFF");
}

// 把索引 idx 的位图写进窗口缓存（环形覆盖最旧槽）
static void win_put(int32_t idx, const uint8_t *bmp) {
  winIdx[winNext] = idx;
  memcpy(winBmp[winNext], bmp, CARD_BYTES);
  winNext = (winNext + 1) % WIN_N;
}

// 查窗口里是否已有 idx，返回槽号或 -1
static int win_find(int32_t idx) {
  for (int i = 0; i < WIN_N; i++) {
    if (winIdx[i] == idx) return i;
  }
  return -1;
}

static void draw_card() {
  tft.fillScreen(TFT_NAVY);
  int slot = win_find(histIdx);
  if (slot < 0) {
    Serial.printf("[draw] Loading! histIdx=%d total=%d win=[%d,%d,%d]\n",
                  histIdx, histTotal, winIdx[0], winIdx[1], winIdx[2]);
    tft.setTextDatum(MC_DATUM);
    tft.setTextSize(2);
    tft.setTextColor(TFT_WHITE, TFT_NAVY);
    tft.drawString("Loading...", tft.width() / 2, tft.height() / 2);
    return;
  }
  tft.drawBitmap(0, (tft.height() - CARD_H) / 2, winBmp[slot],
                 CARD_W, CARD_H, TFT_WHITE);
  tft.setTextDatum(BC_DATUM);
  tft.setTextSize(1);
  tft.setTextColor(TFT_CYAN, TFT_NAVY);
  char line[64];
  snprintf(line, sizeof(line), "%d/%d  tap:next hold:off",
           histIdx + 1, histTotal);
  tft.drawString(line, tft.width() / 2, tft.height() - 8);
}

static void wifi_connect() {
  Serial.printf("[wifi] connecting %s ...\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) {
    delay(400);
    Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[wifi] OK  IP=%s  RSSI=%d\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
  } else {
    Serial.println("[wifi] FAILED, will retry");
  }
}

// 从服务端取第 idx 封历史位图，写入窗口。返回是否成功。
// show=true 时顺便把 histIdx 设为 idx（用于翻页后立即显示）。
static bool fetch_mail(int32_t idx, bool show) {
  // 不在这里卡 idx 上限：服务端会 404 兜底，且响应头能带回真实 X-Total
  if (idx < 0 || WiFi.status() != WL_CONNECTED) return false;
  WiFiClient c;
  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  const char *fk[] = {"X-Index", "X-Total"};
  http.collectHeaders(fk, 2);
  char url[160];
  snprintf(url, sizeof(url), "http://%s:%d/api/mail?idx=%d",
           SERVER_HOST, SERVER_PORT, (int)idx);
  if (!http.begin(c, url)) {
    Serial.printf("[mail] idx=%d begin 失败\n", idx);
    return false;
  }
  int code = http.GET();
  // 先取总数（404 时也有 X-Total），再判断状态
  int total = http.header("X-Total").toInt();
  if (total > 0) histTotal = total;
  if (code != 200) {
    Serial.printf("[mail] idx=%d HTTP %d\n", idx, code);
    http.end(); return false;
  }
  // 不用 http.header("X-Index")：HTTPClient 没 collectHeaders 时读不到，
  // 会返回 0 导致所有卡存进同一个槽。请求哪封就存哪封，最可靠。
  int rIdx = idx;
  int len = http.getSize();
  Serial.printf("[mail] idx=%d 响应 len=%d rIdx=%d\n", idx, len, rIdx);
  bool ok = false;
  if (len == CARD_BYTES) {
    WiFiClient *stream = http.getStreamPtr();
    size_t got = 0;
    uint32_t t0 = millis();
    while (got < CARD_BYTES && millis() - t0 < HTTP_TIMEOUT_MS) {
      size_t want = min((size_t)512, (size_t)(CARD_BYTES - got));
      int n = stream->readBytes(card + got, want);
      if (n <= 0) break;
      got += n;
    }
    ok = (got == CARD_BYTES);
  }
  http.end();
  if (ok) {
    win_put(rIdx >= 0 ? rIdx : idx, card);
    if (show) histIdx = idx;
    Serial.printf("[mail] 取回 %d/%d%s\n", idx + 1, histTotal,
                  show ? " (显示)" : " (预取)");
  }
  return ok;
}

// 确保 histIdx 及其相邻在窗口里（翻一封发一封，缓存3封）
static void ensure_window() {
  if (win_find(histIdx) < 0) fetch_mail(histIdx, false);
  // 预取下一封（循环回绕）
  int32_t nxt = (histIdx + 1) % histTotal;
  if (win_find(nxt) < 0) fetch_mail(nxt, false);
}

// 极简 JSON 取值：从 body 里找 "key": 后面的数字/字符串
static int json_int(const String &s, const char *key) {
  String pat = String("\"") + key + "\":";
  int i = s.indexOf(pat);
  if (i < 0) return -1;
  i += pat.length();
  while (i < s.length() && s[i] == ' ') i++;
  return s.substring(i).toInt();
}
static String json_str(const String &s, const char *key) {
  String pat = String("\"") + key + "\":";
  int i = s.indexOf(pat);
  if (i < 0) return "";
  i += pat.length();
  while (i < s.length() && s[i] == ' ') i++;   // 跳过冒号后空格
  if (i >= s.length() || s[i] != '"') return "";
  i++;
  int j = s.indexOf('"', i);
  if (j < 0) return "";
  return s.substring(i, j);
}

// OTA: 查服务端版本，有更新则流式下载 + MD5 校验 + 刷写 + 重启
static void check_ota() {
  if (WiFi.status() != WL_CONNECTED) return;
  WiFiClient c1;
  HTTPClient http;
  http.setTimeout(6000);
  char url[160];
  snprintf(url, sizeof(url), "http://%s:%d/api/fw", SERVER_HOST, SERVER_PORT);
  if (!http.begin(c1, url)) return;
  int code = http.GET();
  if (code != 200) { http.end(); return; }
  String body = http.getString();
  http.end();

  int ver = json_int(body, "version");
  if (ver <= FW_VERSION || ver <= 0) return;   // 无新版本

  int size = json_int(body, "size");
  String md5 = json_str(body, "md5");
  if (size <= 0 || size > 1200000 || md5.length() != 32) {
    Serial.printf("[ota] 元数据异常 ver=%d size=%d md5=%s\n",
                  ver, size, md5.c_str());
    return;
  }
  Serial.printf("[ota] 发现新版本 %d -> %d (%d bytes)\n", FW_VERSION, ver, size);

  // 刷写期间关背光，避免花屏
  backlight(false);
  WiFiClient c2;
  HTTPClient dl;
  dl.setTimeout(15000);
  snprintf(url, sizeof(url), "http://%s:%d/api/fw.bin", SERVER_HOST, SERVER_PORT);
  if (!dl.begin(c2, url)) { Serial.println("[ota] 下载连接失败"); return; }
  if (dl.GET() != 200) { Serial.println("[ota] 下载失败"); dl.end(); return; }

  if (!Update.begin(size, U_FLASH)) {
    Serial.printf("[ota] begin 失败: %d\n", Update.errorString());
    dl.end();
    return;
  }
  Update.setMD5(md5.c_str());

  WiFiClient *stream = dl.getStreamPtr();
  uint8_t buf[1024];
  size_t got = 0;
  uint32_t t0 = millis();
  while (got < (size_t)size && millis() - t0 < 60000) {
    size_t want = min((size_t)1024, (size_t)size - got);
    int n = stream->readBytes(buf, want);
    if (n <= 0) break;
    if (Update.write(buf, n) != (size_t)n) {
      Serial.println("[ota] 写入失败");
      Update.abort();
      dl.end();
      return;
    }
    got += n;
  }
  dl.end();

  if (got != (size_t)size || !Update.end(true)) {
    Serial.printf("[ota] 校验失败 got=%d err=%d\n", got, Update.errorString());
    Update.abort();
    return;
  }
  Serial.printf("[ota] 刷写成功 v%d，重启中（失败会自动回滚）...\n", ver);
  delay(200);
  ESP.restart();
}

static void poll_server() {
  if (WiFi.status() != WL_CONNECTED) return;

  WiFiClient client;
  HTTPClient http;
  const char *keys[] = {"X-Seq", "X-New", "X-Unread", "X-Poll", "X-Total"};
  http.collectHeaders(keys, 5);
  http.setTimeout(HTTP_TIMEOUT_MS);

  char url[160];
  snprintf(url, sizeof(url), "http://%s:%d/api/card?seen=%lu",
           SERVER_HOST, SERVER_PORT, (unsigned long)lastSeq);
  if (!http.begin(client, url)) {
    Serial.println("[http] begin failed");
    return;
  }
  int code = http.GET();
  if (code != 200) {
    Serial.printf("[http] GET -> %d\n", code);
    http.end();
    return;
  }

  uint32_t seq = http.header("X-Seq").toInt();
  bool isNew = http.header("X-New").toInt() == 1;
  uint32_t unread = http.header("X-Unread").toInt();
  uint32_t hint = http.header("X-Poll").toInt();
  int total = http.header("X-Total").toInt();
  if (total > 0) histTotal = total;
  if (hint >= 5 && hint <= 300) pollEvery = hint * 1000;
  int len = http.getSize();

  bool ok = false;
  if (len == CARD_BYTES) {
    WiFiClient *stream = http.getStreamPtr();
    size_t got = 0;
    uint32_t t0 = millis();
    while (got < CARD_BYTES && millis() - t0 < HTTP_TIMEOUT_MS) {
      size_t want = min((size_t)512, (size_t)(CARD_BYTES - got));
      int n = stream->readBytes(card + got, want);
      if (n <= 0) break;
      got += n;
    }
    ok = (got == CARD_BYTES);
  }
  http.end();

  if (!ok) {
    Serial.printf("[http] 位图不完整 len=%d\n", len);
    return;
  }

  bool changed = (seq != lastSeq);
  lastSeq = seq;
  Serial.printf("[poll] seq=%lu new=%d unread=%lu total=%d every=%lus\n",
                (unsigned long)seq, isNew, (unsigned long)unread,
                histTotal, (unsigned long)(pollEvery / 1000));

  // /api/card 返回的是最新一封，放进 histIdx=0
  if (!histLoaded) {
    win_put(0, card);
    histIdx = 0;
    histLoaded = true;
  }

  if (isNew) {
    // 新邮件：跳回最新，用刚收到的卡片
    win_put(0, card);
    histIdx = 0;
    mode = ALERT;
    modeUntil = millis() + ALERT_MS;
    autoOff = true;
    backlight(true);
    draw_card();
    ensure_window();           // 后台补齐相邻
  } else if (changed && histIdx == 0 && mode != DARK) {
    win_put(0, card);
    draw_card();
  }
}

// 按键上报：免 USB 诊断用，短超时不阻塞太久
static void report_event(const char *kind) {
  if (WiFi.status() != WL_CONNECTED) return;
  WiFiClient c;
  HTTPClient http;
  http.setTimeout(1000);
  char url[180];
  snprintf(url, sizeof(url),
           "http://%s:%d/api/event?k=%s&pos=%d&cnt=%d&mode=%d",
           SERVER_HOST, SERVER_PORT, kind, histIdx + 1, histTotal, (int)mode);
  if (http.begin(c, url)) { http.GET(); http.end(); }
}

// 双击：请求服务端把当前封标已读，成功后从窗口移除并显示下一封
static void on_boot_double() {
  report_event("double");
  if (WiFi.status() != WL_CONNECTED || histTotal <= 1) {
    // 空列表：友好提示，不当失败
    tft.fillScreen(TFT_NAVY);
    tft.setTextDatum(MC_DATUM);
    tft.setTextSize(2);
    tft.setTextColor(TFT_WHITE, TFT_NAVY);
    tft.drawString("No unread mail", tft.width() / 2, tft.height() / 2);
    toastUntil = millis() + 1200;
    Serial.println("[boot] double 空列表");
    return;
  }
  WiFiClient c;
  HTTPClient http;
  http.setTimeout(4000);
  char url[160];
  snprintf(url, sizeof(url), "http://%s:%d/api/read?idx=%d",
           SERVER_HOST, SERVER_PORT, (int)histIdx);
  bool ok = false;
  int total = histTotal;
  if (http.begin(c, url)) {
    int code = http.GET();
    if (code == 200) {
      String body = http.getString();
      ok = json_int(body, "ok") == 1;
      int t = json_int(body, "total");
      if (t > 0) total = t;
    }
    http.end();
  }
  if (ok) {
    // 服务端已删该封：窗口清掉，索引不前进（下一封顶上来）
    int slot = win_find(histIdx);
    if (slot >= 0) winIdx[slot] = -1;
    histTotal = total;
    if (histIdx >= histTotal) histIdx = 0;
    // 清窗口里失效的索引
    for (int i = 0; i < WIN_N; i++) {
      if (winIdx[i] >= histTotal) winIdx[i] = -1;
    }
    // 显示"已读"提示 1.2 秒
    tft.fillScreen(TFT_NAVY);
    tft.setTextDatum(MC_DATUM);
    tft.setTextSize(2);
    tft.setTextColor(TFT_WHITE, TFT_NAVY);
    tft.drawString("Marked read", tft.width() / 2, tft.height() / 2);
    Serial.printf("[boot] double -> read %d, total=%d\n", histIdx + 1, total);
    if (win_find(histIdx) < 0) fetch_mail(histIdx, true);
    ensure_window();
    backlight(true);
    toastUntil = millis() + 1200;   // 提示 1.2 秒后重绘卡片（不熄屏）
  } else {
    Serial.printf("[boot] double 标已读失败 idx=%d\n", histIdx);
    draw_card();
  }
}

static void on_boot_short() {
  report_event("tap");
  autoOff = false;                     // 手动亮屏不自动熄屏，只能长按关
  if (mode == DARK) {
    mode = VIEW;
    backlight(true);
  } else {
    // 循环回绕翻下一封
    histIdx = (histIdx + 1) % histTotal;
    mode = VIEW;
  }
  // 窗口没有就直接取（有短暂 Loading），取回后显示
  if (win_find(histIdx) < 0) fetch_mail(histIdx, true);
  draw_card();
  ensure_window();                     // 缓存当前+下一封
  Serial.printf("[boot] tap -> %d/%d (stay on)\n", histIdx + 1, histTotal);
}

static void on_boot_long() {
  report_event("hold");
  longFired = true;
  mode = DARK;
  autoOff = false;
  backlight(false);
  Serial.println("[boot] HOLD -> DARK");
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.printf("\n=== CYD Mail Notify v%d (OTA) ===\n", FW_VERSION);
  Serial.printf("[ota] 当前固件 v%d\n", FW_VERSION);
  // 新固件启动后确认运行正常，告诉 bootloader 不要回滚
  esp_ota_mark_app_valid_cancel_rollback();

  pinMode(BL_PIN, OUTPUT);
  backlight(false);
  pinMode(BOOT_PIN, INPUT_PULLUP);

  tft.init();
  tft.setRotation(0);
  tft.fillScreen(TFT_NAVY);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_NAVY);
  tft.setTextSize(2);
  tft.drawString("Mail Notify", tft.width() / 2, tft.height() / 2 - 20);
  tft.setTextSize(1);
  tft.setTextColor(TFT_CYAN, TFT_NAVY);
  tft.drawString("connecting WiFi...", tft.width() / 2, tft.height() / 2 + 12);

  wifi_connect();
  memset(card, 0, sizeof(card));
  // 开机亮 3 秒显示启动信息，之后进入黑屏待机
  mode = VIEW;
  modeUntil = millis() + 3000;
  autoOff = true;
  backlight(true);
  lastPoll = millis();
  poll_server();      // 首次同步：seen=0，服务端不会判成新邮件
  ensure_window();    // 预取下一封，凑满 3 封窗口
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) wifi_connect();

  bool b = digitalRead(BOOT_PIN);
  // 下降沿：按下开始计时
  if (lastBoot == HIGH && b == LOW) {
    delay(40);                       // 简易消抖
    if (digitalRead(BOOT_PIN) == LOW) {
      pressing = true;
      pressAt = millis();
      longFired = false;
      // 第二次按下且在双击窗口内 -> 双击，取消待处理的单击
      if (tapPending && (millis() - tapAt) <= DOUBLE_MS) {
        tapPending = false;
        on_boot_double();
      }
    }
  }
  // 按住达阈值：亮屏状态下长按立即熄屏
  if (pressing && !longFired && mode != DARK && (millis() - pressAt >= HOLD_MS)) {
    on_boot_long();
  }
  // 上升沿：松开（未触发长按则挂起单击，等 350ms 看有没有第二下）
  if (lastBoot == LOW && b == HIGH && pressing) {
    pressing = false;
    if (!longFired) {
      tapPending = true;
      tapAt = millis();
    }
  }
  lastBoot = b;

  // 单击超时：没等到第二下，执行翻页
  if (tapPending && (millis() - tapAt) > DOUBLE_MS) {
    tapPending = false;
    on_boot_short();
  }

  // 已读提示到点：重绘卡片（保持亮屏）
  if (toastUntil > 0 && (int32_t)(millis() - toastUntil) >= 0) {
    toastUntil = 0;
    draw_card();
  }

  if (mode != DARK && autoOff && (int32_t)(millis() - modeUntil) >= 0) {
    mode = DARK;
    autoOff = false;
    backlight(false);
    Serial.println("[screen] timeout -> DARK");
  }

  if ((int32_t)(millis() - lastPoll) >= (int32_t)pollEvery) {
    lastPoll = millis();
    poll_server();
    if (++pollCount % 4 == 0) check_ota();   // 约每 60 秒查一次固件版本
  }
  delay(20);
}
