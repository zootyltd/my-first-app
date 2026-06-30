/*
 * Waveshare 4" Spectra 6 E-Paper — ESP32-S3 API server
 * Display: 400x600 portrait, 6-color (Black/White/Green/Blue/Red/Yellow)
 *
 * REQUIRED LIBRARIES:
 *   ESPAsyncWebServer + AsyncTCP (mathieucarbou or lacamera fork)
 *
 * SETUP:
 *   1. Set WIFI_SSID / WIFI_PASS below
 *   2. Flash, note IP from Serial Monitor
 *   3. Open optimizer.html on your phone, enter that IP
 *
 * COLORS: 0=Black 1=White 2=Green 3=Blue 4=Red 5=Yellow
 */

#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <SPI.h>
#include <esp_task_wdt.h>

// ============================================================
// USER CONFIGURATION
// ============================================================
const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";

#define PIN_BUSY    4
#define PIN_RST    14
#define PIN_DC     13
#define PIN_CS     10
#define PIN_SCK    12
#define PIN_MOSI   11

#define EPD_W   400
#define EPD_H   600
#define BUF_SZ  (EPD_W * EPD_H / 2)   // 120000 bytes

uint8_t* epd_buf = nullptr;
volatile bool g_show  = false;
volatile bool g_clear = false;
String g_status = "Ready";

AsyncWebServer server(80);

// ============================================================
// EPD DRIVER
// ============================================================
static void epd_cmd(uint8_t c) {
  digitalWrite(PIN_DC, LOW);
  digitalWrite(PIN_CS, LOW);
  SPI.transfer(c);
  digitalWrite(PIN_CS, HIGH);
}

static void epd_dat(uint8_t d) {
  digitalWrite(PIN_DC, HIGH);
  digitalWrite(PIN_CS, LOW);
  SPI.transfer(d);
  digitalWrite(PIN_CS, HIGH);
}

static void epd_wait_idle(uint32_t timeout_ms = 15000) {
  delay(10);
  if (digitalRead(PIN_BUSY) == LOW) { delay(10); return; }
  uint32_t t0 = millis();
  while (digitalRead(PIN_BUSY) == HIGH) {
    if (millis() - t0 > timeout_ms) {
      Serial.printf("[EPD] wait_idle timeout BUSY=%d\n", digitalRead(PIN_BUSY));
      break;
    }
    delay(100);
  }
  delay(10);
}

static void epd_hw_reset() {
  digitalWrite(PIN_RST, HIGH); delay(20);
  digitalWrite(PIN_RST, LOW);  delay(4);
  digitalWrite(PIN_RST, HIGH); delay(200);
}

void epd_init() {
  pinMode(PIN_BUSY, INPUT);
  pinMode(PIN_RST,  OUTPUT);
  pinMode(PIN_DC,   OUTPUT);
  pinMode(PIN_CS,   OUTPUT);
  digitalWrite(PIN_CS,  HIGH);
  digitalWrite(PIN_RST, HIGH);

  SPI.begin(PIN_SCK, -1, PIN_MOSI, PIN_CS);
  SPI.setFrequency(4000000UL);
  SPI.setBitOrder(MSBFIRST);
  SPI.setDataMode(SPI_MODE0);

  epd_hw_reset();

  epd_cmd(0x00); epd_dat(0xEF); epd_dat(0x08);
  epd_cmd(0x01); epd_dat(0x37); epd_dat(0x00); epd_dat(0x23); epd_dat(0x23);
  epd_cmd(0x03); epd_dat(0x00);
  epd_cmd(0x06); epd_dat(0xC7); epd_dat(0xC7); epd_dat(0x1D);
  epd_cmd(0x30); epd_dat(0x3C);
  epd_cmd(0x41); epd_dat(0x00);
  epd_cmd(0x50); epd_dat(0x37);
  epd_cmd(0x60); epd_dat(0x22);
  epd_cmd(0x61);
    epd_dat((EPD_W >> 8) & 0xFF); epd_dat(EPD_W & 0xFF);
    epd_dat((EPD_H >> 8) & 0xFF); epd_dat(EPD_H & 0xFF);
  epd_cmd(0xE3); epd_dat(0xAA);
  delay(100);
  epd_cmd(0x50); epd_dat(0x37);
}

void epd_show_buf() {
  Serial.println("[EPD] Refresh start...");
  epd_cmd(0x61);
    epd_dat((EPD_W >> 8) & 0xFF); epd_dat(EPD_W & 0xFF);
    epd_dat((EPD_H >> 8) & 0xFF); epd_dat(EPD_H & 0xFF);
  epd_cmd(0x10);
  digitalWrite(PIN_DC, HIGH);
  digitalWrite(PIN_CS, LOW);
  SPI.writeBytes(epd_buf, BUF_SZ);
  digitalWrite(PIN_CS, HIGH);
  epd_cmd(0x04); epd_wait_idle(5000);
  epd_cmd(0x12); epd_wait_idle(45000);
  epd_cmd(0x02); epd_wait_idle(5000);
  Serial.println("[EPD] Refresh complete.");
}

void epd_fill(uint8_t colorCode) {
  uint8_t b = ((colorCode & 0x0F) << 4) | (colorCode & 0x0F);
  epd_cmd(0x61);
    epd_dat((EPD_W >> 8) & 0xFF); epd_dat(EPD_W & 0xFF);
    epd_dat((EPD_H >> 8) & 0xFF); epd_dat(EPD_H & 0xFF);
  epd_cmd(0x10);
  digitalWrite(PIN_DC, HIGH);
  digitalWrite(PIN_CS, LOW);
  for (size_t i = 0; i < BUF_SZ; i++) SPI.transfer(b);
  digitalWrite(PIN_CS, HIGH);
  epd_cmd(0x04); epd_wait_idle(5000);
  epd_cmd(0x12); epd_wait_idle(45000);
  epd_cmd(0x02); epd_wait_idle(5000);
}

// ============================================================
// SETUP / LOOP
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(500);
  esp_task_wdt_deinit();
  Serial.println("\n[BOOT] Waveshare 4\" Spectra 6");

  epd_buf = (uint8_t*)ps_malloc(BUF_SZ);
  if (!epd_buf) {
    Serial.println("[ERROR] PSRAM alloc failed! Enable Tools->PSRAM->OPI PSRAM");
    while (1) delay(1000);
  }
  memset(epd_buf, 0x11, BUF_SZ);
  Serial.printf("[EPD] Buffer: %d bytes in PSRAM\n", BUF_SZ);

  epd_init();
  Serial.println("[EPD] Init OK -- clearing...");
  epd_fill(0x01);
  Serial.println("[EPD] Clear done.");

  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("[WiFi] Connecting");
  for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) {
    delay(500); Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n[WiFi] Connected! -> http://%s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("\n[WiFi] FAILED");
  }

  // CORS helper
  auto cors = [](AsyncWebServerResponse* r) {
    r->addHeader("Access-Control-Allow-Origin", "*");
    r->addHeader("Access-Control-Allow-Methods", "GET,POST,OPTIONS");
    r->addHeader("Access-Control-Allow-Headers", "Content-Type");
  };

  // Preflight handler for all OPTIONS requests
  server.onNotFound([cors](AsyncWebServerRequest* req) {
    if (req->method() == HTTP_OPTIONS) {
      AsyncWebServerResponse* r = req->beginResponse(204);
      cors(r);
      req->send(r);
    } else {
      req->send(404, "text/plain", "not found");
    }
  });

  // Status
  server.on("/status", HTTP_GET, [cors](AsyncWebServerRequest* req) {
    AsyncWebServerResponse* r = req->beginResponse(200, "text/plain", g_status);
    cors(r);
    req->send(r);
  });

  // Receive binary framebuffer chunk: POST /chunk?o=<offset>
  server.on("/chunk", HTTP_POST,
    [cors](AsyncWebServerRequest* req) {
      AsyncWebServerResponse* r = req->beginResponse(200, "text/plain", "ok");
      cors(r);
      req->send(r);
    },
    nullptr,
    [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total) {
      int off = req->hasParam("o") ? req->getParam("o")->value().toInt() : 0;
      size_t dest = (size_t)off + index;
      if (dest + len <= BUF_SZ) memcpy(epd_buf + dest, data, len);
    }
  );

  // Trigger display refresh
  server.on("/show", HTTP_POST, [cors](AsyncWebServerRequest* req) {
    g_show = true;
    AsyncWebServerResponse* r = req->beginResponse(200, "text/plain", "ok");
    cors(r);
    req->send(r);
  });

  // Clear to white
  server.on("/clear", HTTP_POST, [cors](AsyncWebServerRequest* req) {
    g_clear = true;
    AsyncWebServerResponse* r = req->beginResponse(200, "text/plain", "ok");
    cors(r);
    req->send(r);
  });

  server.begin();
  Serial.println("[HTTP] API server running");
  Serial.println("[HTTP] Open optimizer.html on your phone, enter the IP above");
}

void loop() {
  if (g_clear) {
    g_clear = false;
    g_status = "Clearing...";
    epd_fill(0x01);
    g_status = "Ready";
  }
  if (g_show) {
    g_show = false;
    g_status = "Refreshing display...";
    epd_show_buf();
    g_status = "Done";
  }
  delay(20);
}
