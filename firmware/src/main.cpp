/* CYD (ESP32-2432S028R) 邮件提醒固件
 *
 * 每 15 秒向服务端拉一张 240x160 1bit 点阵卡片。
 * 服务端说 X-New=1 -> 亮屏 10 秒 -> 关背光（内容保留）。
 * 按 BOOT(GPIO0) -> 亮屏 20 秒查看；亮屏期间再按一次立即熄灭。
 */

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <TFT_eSPI.h>
#include "wifi_config.h"

#define CARD_W     240
#define CARD_H     160
#define CARD_BYTES (CARD_W * CARD_H / 8)   // 4800
#define BOOT_PIN   0
#define BL_PIN     21

#define ALERT_MS   10000
#define VIEW_MS    20000
#define POLL_MS    15000
#define HTTP_TIMEOUT_MS 6000

TFT_eSPI tft = TFT_eSPI();
static uint8_t card[CARD_BYTES];      // 静态缓冲，避免堆碎片
static uint32_t lastSeq = 0;          // 0 = 首次同步
static uint32_t pollEvery = POLL_MS;
static uint32_t lastPoll = 0;

enum Mode { DARK, ALERT, VIEW };
static Mode mode = DARK;
static uint32_t modeUntil = 0;
static bool lastBoot = HIGH;

static void backlight(bool on) {
  digitalWrite(BL_PIN, on ? HIGH : LOW);
  Serial.printf("[screen] %s\n", on ? "ON" : "OFF");
}

static void draw_card() {
  tft.fillScreen(TFT_NAVY);
  // 位图里的 1 画白，0 保持深蓝背景
  tft.drawBitmap(0, (tft.height() - CARD_H) / 2, card,
                 CARD_W, CARD_H, TFT_WHITE);
  tft.setTextDatum(BC_DATUM);
  tft.setTextSize(1);
  tft.setTextColor(TFT_CYAN, TFT_NAVY);
  char line[64];
  snprintf(line, sizeof(line), "seq %lu  |  BOOT to view",
           (unsigned long)lastSeq);
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

  if (isNew) {
    mode = ALERT;
    modeUntil = millis() + ALERT_MS;
    backlight(true);
    draw_card();
  } else if (changed && mode != DARK) {
    draw_card();  // 亮屏期间服务端换了卡片，跟着刷新
  }
}

static void on_boot_press() {
  if (mode == DARK) {
    mode = VIEW;
    modeUntil = millis() + VIEW_MS;
    backlight(true);
    draw_card();
    Serial.println("[boot] wake -> VIEW 20s");
  } else if (mode == ALERT) {
    mode = VIEW;
    modeUntil = millis() + VIEW_MS;
    Serial.println("[boot] ALERT -> VIEW 20s");
  } else {
    mode = DARK;
    backlight(false);
    Serial.println("[boot] sleep now");
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== CYD Mail Notify ===");

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
  backlight(true);
  lastPoll = millis();
  poll_server();  // 首次同步：seen=0，服务端不会判成新邮件
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) wifi_connect();

  bool b = digitalRead(BOOT_PIN);
  if (lastBoot == HIGH && b == LOW) {
    delay(40);                       // 简易消抖
    if (digitalRead(BOOT_PIN) == LOW) on_boot_press();
  }
  lastBoot = b;

  if (mode != DARK && (int32_t)(millis() - modeUntil) >= 0) {
    mode = DARK;
    backlight(false);
    Serial.println("[screen] timeout -> DARK");
  }

  if ((int32_t)(millis() - lastPoll) >= (int32_t)pollEvery) {
    lastPoll = millis();
    poll_server();
  }
  delay(20);
}
