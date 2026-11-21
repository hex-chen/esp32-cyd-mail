/* CYD (ESP32-2432S028R) 邮件提醒固件
 *
 * 每 15 秒向服务端拉一张 240x160 1bit 点阵卡片。
 * 新邮件 -> 亮屏 10 秒 -> 自动关背光（内容保留）。
 *
 * BOOT(GPIO0) 交互：
 *   灭屏短按        -> 亮屏看最新一封，常亮不自动熄
 *   亮屏短按        -> 循环切下一封，常亮不自动熄
 *   亮屏长按 >=1s   -> 立即熄屏
 * 只有新邮件（ALERT）和开机 splash 会到点自动熄屏。
 * 本地环形缓存最近 5 封位图；重启后只保留开机后收到的 + 最新一封。
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
#define POLL_MS    15000
#define HTTP_TIMEOUT_MS 6000
#define CACHE_N    5

TFT_eSPI tft = TFT_eSPI();
static uint8_t card[CARD_BYTES];                 // 收包临时缓冲
static uint8_t cards[CACHE_N][CARD_BYTES];       // 5 封位图环形缓存（24KB）
static uint32_t cardSeq[CACHE_N];
static uint8_t cardCnt = 0;                      // 已缓存封数
static uint8_t cardHead = 0;                     // 最新一封的索引
static int8_t viewPos = 0;                       // 0 = 最新

static uint32_t lastSeq = 0;                     // 0 = 首次同步
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

static void backlight(bool on) {
  digitalWrite(BL_PIN, on ? HIGH : LOW);
  Serial.printf("[screen] %s\n", on ? "ON" : "OFF");
}

static void store_card(uint32_t seq) {
  cardHead = (cardHead + 1) % CACHE_N;
  memcpy(cards[cardHead], card, CARD_BYTES);
  cardSeq[cardHead] = seq;
  if (cardCnt < CACHE_N) cardCnt++;
  viewPos = 0;
}

static void draw_card() {
  tft.fillScreen(TFT_NAVY);
  if (cardCnt == 0) {
    tft.setTextDatum(MC_DATUM);
    tft.setTextSize(2);
    tft.setTextColor(TFT_WHITE, TFT_NAVY);
    tft.drawString("No mail", tft.width() / 2, tft.height() / 2);
    return;
  }
  uint8_t idx = (cardHead - viewPos + CACHE_N) % CACHE_N;
  // 位图里的 1 画白，0 保持深蓝背景
  tft.drawBitmap(0, (tft.height() - CARD_H) / 2, cards[idx],
                 CARD_W, CARD_H, TFT_WHITE);
  tft.setTextDatum(BC_DATUM);
  tft.setTextSize(1);
  tft.setTextColor(TFT_CYAN, TFT_NAVY);
  char line[64];
  snprintf(line, sizeof(line), "mail %d/%d  seq %lu  tap:next hold:off",
           viewPos + 1, cardCnt, (unsigned long)cardSeq[idx]);
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
  const char *keys[] = {"X-Seq", "X-New", "X-Unread", "X-Poll"};
  http.collectHeaders(keys, 4);
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
  Serial.printf("[poll] seq=%lu new=%d unread=%lu every=%lus\n",
                (unsigned long)seq, isNew, (unsigned long)unread,
                (unsigned long)(pollEvery / 1000));

  if (changed || cardCnt == 0) store_card(seq);

  if (isNew) {
    mode = ALERT;
    modeUntil = millis() + ALERT_MS;
    autoOff = true;                    // 新邮件亮 10 秒自动熄屏
    backlight(true);
    draw_card();
  } else if (changed && mode != DARK) {
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
           SERVER_HOST, SERVER_PORT, kind, viewPos + 1, cardCnt, (int)mode);
  if (http.begin(c, url)) { http.GET(); http.end(); }
}

static void on_boot_short() {
  report_event("tap");
  autoOff = false;                     // 手动亮屏不自动熄屏，只能长按关
  if (mode == DARK) {
    mode = VIEW;
    viewPos = 0;
    backlight(true);
  } else {
    if (cardCnt > 1) viewPos = (viewPos + 1) % cardCnt;
    mode = VIEW;
  }
  draw_card();
  Serial.printf("[boot] tap -> VIEW %d/%d (stay on)\n",
                viewPos + 1, cardCnt);
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
  poll_server();  // 首次同步：seen=0，服务端不会判成新邮件
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
    }
  }
  // 按住达阈值：亮屏状态下长按立即熄屏
  if (pressing && !longFired && mode != DARK && (millis() - pressAt >= HOLD_MS)) {
    on_boot_long();
  }
  // 上升沿：松开（未触发长按则视为短按）
  if (lastBoot == LOW && b == HIGH && pressing) {
    pressing = false;
    if (!longFired) on_boot_short();
  }
  lastBoot = b;

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
