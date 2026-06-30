/*
 * ============================================================
 * Waveshare 4" Spectra 6 E-Paper Image Optimizer
 * Display: 600 x 400, 6-color (Black/White/Red/Green/Blue/Yellow)
 * Board:   ESP32-S3 + Waveshare HAT+ driver board
 * Protocol: SPI
 *
 * REQUIRED LIBRARIES (Arduino Library Manager):
 *   - ESPAsyncWebServer  (search "ESPAsyncWebServer" by lacamera)
 *   - AsyncTCP           (search "AsyncTCP" by ESP32Async)
 *
 * SETUP:
 *   1. Set WIFI_SSID and WIFI_PASS below
 *   2. Verify pin numbers match your HAT+ wiring
 *   3. Upload -> open Serial Monitor at 115200 -> note IP address
 *   4. Open that IP in your phone browser
 *   5. Upload any image -- everything processes in the browser,
 *      only the final 120 KB framebuffer is sent to the ESP32
 *
 * PALETTE CALIBRATION:
 *   The RGB values in the JS PALETTE array are typical measured
 *   values. If colors look wrong, adjust them to your panel's
 *   actual output -- they vary between production batches.
 *
 * SPECTRA 6 COLORS (6 total, no orange):
 *   0=Black  1=White  2=Green  3=Blue  4=Red  5=Yellow
 * ============================================================
 */

#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <SPI.h>

// ============================================================
// USER CONFIGURATION
// ============================================================
const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";

// Pins -- from Waveshare ESP32-S3 files
#define PIN_BUSY    4   // EPD_BUSY_PIN
#define PIN_RST    14   // EPD_RST_PIN
#define PIN_DC     13   // EPD_DC_PIN
#define PIN_CS     10   // EPD_CS_PIN
#define PIN_SCK    12   // EPD_SCK_PIN
#define PIN_MOSI   11   // EPD_MOSI_PIN

// Display resolution -- Waveshare 4" Spectra 6 HAT+
#define EPD_W   600
#define EPD_H   400

// ============================================================
// FRAMEBUFFER  (4 bits/pixel -> 2 pixels per byte)
// 600 x 400 / 2 = 120,000 bytes
// ============================================================
#define BUF_SZ  (EPD_W * EPD_H / 2)   // 120000

uint8_t* epd_buf = nullptr;
volatile bool g_show  = false;
volatile bool g_clear = false;
String g_status = "Ready -- open this page on your phone";

AsyncWebServer server(80);

// ============================================================
// EPD DRIVER -- Waveshare Spectra 6 (UC8179 / ACeP protocol)
// BUSY pin: LOW = idle, HIGH = busy (active-HIGH on this panel)
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

static void epd_wait_idle() {
  // Waveshare Spectra 6: BUSY LOW = busy, wait until HIGH
  delay(10);
  while (digitalRead(PIN_BUSY) == LOW) {
    delay(100);
  }
  delay(10);
}

static void epd_hw_reset() {
  digitalWrite(PIN_RST, HIGH); delay(20);
  digitalWrite(PIN_RST, LOW);  delay(4);
  digitalWrite(PIN_RST, HIGH); delay(20);
  epd_wait_idle();
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

  // Waveshare Spectra 6 (600x400) initialisation sequence
  // Based on Waveshare EPD_4in2_Spectra6 reference driver
  epd_cmd(0x00);                         // Panel Setting
    epd_dat(0xEF); epd_dat(0x08);

  epd_cmd(0x01);                         // Power Setting
    epd_dat(0x37); epd_dat(0x00);
    epd_dat(0x23); epd_dat(0x23);

  epd_cmd(0x03); epd_dat(0x00);         // Power off sequence

  epd_cmd(0x06);                         // Booster soft start
    epd_dat(0xC7); epd_dat(0xC7); epd_dat(0x1D);

  epd_cmd(0x30); epd_dat(0x3C);         // PLL control (50Hz)

  epd_cmd(0x41); epd_dat(0x00);         // Temperature sensor (internal)

  epd_cmd(0x50); epd_dat(0x37);         // VCOM and data interval

  epd_cmd(0x60); epd_dat(0x22);         // TCON setting

  epd_cmd(0x61);                         // Resolution: 600 x 400
    epd_dat((EPD_W >> 8) & 0xFF);        // 0x02
    epd_dat( EPD_W       & 0xFF);        // 0x58
    epd_dat((EPD_H >> 8) & 0xFF);        // 0x01
    epd_dat( EPD_H       & 0xFF);        // 0x90

  epd_cmd(0xE3); epd_dat(0xAA);         // Power saving

  delay(100);
  epd_cmd(0x50); epd_dat(0x37);         // Re-set VCOM after delay
}

// Write current epd_buf to the display and trigger refresh
void epd_show_buf() {
  Serial.println("[EPD] Starting refresh (~20-30s)...");

  epd_cmd(0x61);                         // Set resolution again before data
    epd_dat((EPD_W >> 8) & 0xFF);
    epd_dat( EPD_W       & 0xFF);
    epd_dat((EPD_H >> 8) & 0xFF);
    epd_dat( EPD_H       & 0xFF);

  epd_cmd(0x10);                         // Data start transmission
  digitalWrite(PIN_DC, HIGH);
  digitalWrite(PIN_CS, LOW);
  SPI.writeBytes(epd_buf, BUF_SZ);       // 120 000 bytes, 4-bit/pixel
  digitalWrite(PIN_CS, HIGH);

  epd_cmd(0x04); epd_wait_idle();        // Power on
  epd_cmd(0x12); epd_wait_idle();        // Display refresh
  epd_cmd(0x02); epd_wait_idle();        // Power off

  Serial.println("[EPD] Refresh complete.");
}

// Fill entire display with a single Spectra 6 color code (0-5)
void epd_fill(uint8_t colorCode) {
  uint8_t b = ((colorCode & 0x0F) << 4) | (colorCode & 0x0F);
  epd_cmd(0x61);
    epd_dat((EPD_W >> 8) & 0xFF);
    epd_dat( EPD_W       & 0xFF);
    epd_dat((EPD_H >> 8) & 0xFF);
    epd_dat( EPD_H       & 0xFF);
  epd_cmd(0x10);
  digitalWrite(PIN_DC, HIGH);
  digitalWrite(PIN_CS, LOW);
  for (size_t i = 0; i < BUF_SZ; i++) SPI.transfer(b);
  digitalWrite(PIN_CS, HIGH);
  epd_cmd(0x04); epd_wait_idle();
  epd_cmd(0x12); epd_wait_idle();
  epd_cmd(0x02); epd_wait_idle();
}

// ============================================================
// SETUP / LOOP
// ============================================================
void setup_server();   // forward declaration

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n[BOOT] Waveshare 4\" Spectra 6 Image Optimizer");

  // Allocate framebuffer in PSRAM (120 KB -- too large for internal RAM)
  epd_buf = (uint8_t*)ps_malloc(BUF_SZ);
  if (!epd_buf) {
    Serial.println("[ERROR] PSRAM allocation failed!");
    Serial.println("[ERROR] Enable PSRAM: Tools -> PSRAM -> OPI PSRAM");
    while (1) delay(1000);
  }
  memset(epd_buf, 0x11, BUF_SZ);  // pre-fill with white (0x1=white, packed)
  Serial.printf("[EPD]  Buffer: %d bytes in PSRAM (%dx%d @ 4bpp)\n", BUF_SZ, EPD_W, EPD_H);

  epd_init();
  Serial.println("[EPD] Init OK -- clearing to white...");
  epd_fill(0x01);   // 0x01 = White
  Serial.println("[EPD] Clear done.");

  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("[WiFi] Connecting");
  for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) {
    delay(500); Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n[WiFi] Connected!  ->  http://%s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("\n[WiFi] FAILED -- check SSID/password");
  }

  setup_server();
  server.begin();
  Serial.println("[HTTP] Server running");
}

void loop() {
  if (g_clear) {
    g_clear = false;
    g_status = "Clearing display to white...";
    epd_fill(0x01);
    g_status = "Display cleared. Ready.";
  }
  if (g_show) {
    g_show = false;
    g_status = "Refreshing display (~20-30s)...";
    epd_show_buf();
    g_status = "Done! Ready for next image.";
  }
  delay(20);
}

// ============================================================
// HTML PAGE  (served from PROGMEM in three chunks)
// ============================================================

// ---- CHUNK 1: head + CSS ----
const char HTML_HEAD[] PROGMEM = 
  "<!DOCTYPE html>\n"
  "<html lang=\"en\">\n"
  "<head>\n"
  "<meta charset=\"UTF-8\">\n"
  "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1,maximum-scale=1\">\n"
  "<title>Spectra 6 Optimizer</title>\n"
  "<style>\n"
  "*{box-sizing:border-box;margin:0;padding:0}\n"
  "body{background:#111;color:#eee;font-family:system-ui,sans-serif;font-size:14px;min-height:100vh}\n"
  "h1{font-size:1.1rem;font-weight:600;color:#fff;padding:12px 16px;background:#1a1a2e;border-bottom:1px solid #333;display:flex;align-items:center;gap:10px}\n"
  "#connStatus{font-size:.75rem;font-weight:400;color:#888;margin-left:auto}\n"
  ".main{display:flex;flex-direction:column}\n"
  ".upload-zone{border:2px dashed #444;border-radius:10px;margin:14px;padding:28px 16px;text-align:center;cursor:pointer;transition:.2s;background:#161616}\n"
  ".upload-zone:hover,.upload-zone.drag{border-color:#6c9bdf;background:#1a1f2e}\n"
  ".upload-zone p{color:#888;font-size:.85rem;margin-top:6px}\n"
  ".upload-zone input[type=file]{display:none}\n"
  ".btn{display:inline-flex;align-items:center;justify-content:center;gap:6px;padding:10px 18px;border-radius:7px;border:none;cursor:pointer;font-size:.85rem;font-weight:600;transition:.15s;white-space:nowrap}\n"
  ".btn-primary{background:#2e5fa3;color:#fff}.btn-primary:hover{background:#3d74c7}\n"
  ".btn-primary:disabled{background:#2a2a2a;color:#555;cursor:not-allowed}\n"
  ".btn-warn{background:#5a3a10;color:#ffc}.btn-warn:hover{background:#7a5018}\n"
  ".btn-sm{padding:6px 12px;font-size:.78rem}\n"
  ".row{display:flex;gap:8px;flex-wrap:wrap;padding:0 14px 10px;align-items:center}\n"
  ".preview-wrap{margin:0 14px 10px;border-radius:8px;overflow:hidden;background:#0a0a0a;text-align:center}\n"
  "#previewCanvas{max-width:100%;height:auto;display:block;margin:0 auto}\n"
  ".status-bar{background:#151f15;border:1px solid #2a3a2a;border-radius:6px;margin:0 14px 10px;padding:8px 12px;font-size:.8rem;color:#8fc08f;min-height:34px;line-height:1.4}\n"
  ".progress-wrap{margin:0 14px 10px;height:5px;background:#1e1e1e;border-radius:3px;overflow:hidden;display:none}\n"
  ".progress-bar{height:100%;background:#2e5fa3;width:0%;transition:width .15s;border-radius:3px}\n"
  ".section{border-top:1px solid #1e1e1e;padding:10px 14px}\n"
  ".section details summary{cursor:pointer;user-select:none;font-weight:600;color:#999;font-size:.85rem;padding:4px 0;list-style:none;display:flex;align-items:center;gap:6px}\n"
  ".section details summary::before{content:\"&#9654;\";font-size:.6rem}\n"
  ".section details[open] summary::before{content:\"&#9660;\"}\n"
  ".ctrl-grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(195px,1fr));gap:10px 18px;padding:10px 0}\n"
  ".ctrl{display:flex;flex-direction:column;gap:4px}\n"
  ".ctrl label{font-size:.74rem;color:#777;display:flex;justify-content:space-between}\n"
  ".ctrl label span{color:#ccc;font-weight:600;min-width:40px;text-align:right}\n"
  ".ctrl input[type=range]{width:100%;accent-color:#4a80d0;cursor:pointer}\n"
  ".ctrl select{background:#1a1a1a;color:#eee;border:1px solid #333;border-radius:5px;padding:5px 8px;font-size:.82rem;width:100%}\n"
  ".checks{display:flex;flex-direction:column;gap:6px;padding:8px 0}\n"
  ".checks label{display:flex;align-items:center;gap:8px;font-size:.82rem;cursor:pointer;color:#bbb}\n"
  ".checks input[type=checkbox]{accent-color:#4a80d0;width:14px;height:14px;cursor:pointer}\n"
  ".hist-wrap{padding:8px 0}\n"
  ".hist-controls{display:flex;gap:14px;margin-bottom:6px}\n"
  ".hist-controls label{display:flex;align-items:center;gap:5px;font-size:.75rem;color:#888;cursor:pointer}\n"
  "#histCanvas{width:100%;height:80px;background:#0c0c0c;border-radius:5px;display:block}\n"
  ".pal-bar{display:flex;height:22px;border-radius:4px;overflow:hidden;margin:10px 0 3px}\n"
  ".pal-swatch{transition:flex .3s;position:relative;min-width:2px}\n"
  ".pal-swatch::after{content:attr(data-pct);position:absolute;bottom:1px;left:50%;transform:translateX(-50%);font-size:9px;color:rgba(255,255,255,.75);white-space:nowrap;pointer-events:none}\n"
  ".pal-names{display:flex;font-size:.62rem;color:#555;margin-bottom:8px}\n"
  ".pal-names span{flex:1;text-align:center}\n"
  ".divider{height:1px;background:#1e1e1e;margin:8px 0}\n"
  ".badge{font-size:.68rem;padding:2px 7px;border-radius:10px;margin-left:4px}\n"
  ".badge-auto{background:#1a3a1a;color:#6c9b6c}\n"
  ".badge-man{background:#3a2a10;color:#c09050}\n"
  ".scene-info{text-align:center;padding:2px 14px 8px;display:flex;flex-wrap:wrap;gap:5px;justify-content:center}\n"
  ".sbadge{background:#1c2b3b;color:#6c8fb0;font-size:.7rem;padding:3px 9px;border-radius:10px}\n"
  "</style>\n"
  "</head>\n"
  "<body>\n"
  "<h1>&#127749; Spectra 6 Image Optimizer <span id=\"connStatus\">&#9679; connecting...</span></h1>\n"
  "<div class=\"main\">\n";


// ---- CHUNK 2: body controls ----
const char HTML_BODY[] PROGMEM = 
  "<div class=\"upload-zone\" id=\"dropZone\" onclick=\"document.getElementById('fileIn').click()\">\n"
  "  <svg width=\"34\" height=\"34\" fill=\"none\" stroke=\"#555\" stroke-width=\"1.5\" viewBox=\"0 0 24 24\">\n"
  "    <path d=\"M4 16v2a2 2 0 002 2h12a2 2 0 002-2v-2M12 12V4m0 0L8 8m4-4l4 4\"/>\n"
  "  </svg>\n"
  "  <p>Tap to select photo</p>\n"
  "  <p>or drag &amp; drop here</p>\n"
  "  <input type=\"file\" id=\"fileIn\" accept=\"image/*\">\n"
  "</div>\n"
  "\n"
  "<div class=\"row\">\n"
  "  <button class=\"btn btn-primary\" id=\"btnProcess\" disabled>&#9654; Process</button>\n"
  "  <button class=\"btn btn-primary\" id=\"btnSend\" disabled>&#8594; Send to Display</button>\n"
  "  <button class=\"btn btn-warn btn-sm\" id=\"btnClear\">Clear</button>\n"
  "</div>\n"
  "\n"
  "<div class=\"status-bar\" id=\"statusBar\">Upload an image to begin</div>\n"
  "<div class=\"progress-wrap\" id=\"progWrap\"><div class=\"progress-bar\" id=\"progBar\"></div></div>\n"
  "\n"
  "<div class=\"preview-wrap\">\n"
  "  <canvas id=\"previewCanvas\" width=\"600\" height=\"400\"></canvas>\n"
  "</div>\n"
  "<div class=\"scene-info\" id=\"sceneInfo\"></div>\n"
  "\n"
  "<div class=\"section\">\n"
  "<details open>\n"
  "<summary>Advanced Controls\n"
  "  <span class=\"badge\" id=\"autoModeBadge\">AUTO</span>\n"
  "</summary>\n"
  "<div class=\"ctrl-grid\">\n"
  "  <div class=\"ctrl\"><label>Brightness <span id=\"vBri\">0</span></label>\n"
  "    <input type=\"range\" id=\"sBri\" min=\"-100\" max=\"100\" value=\"0\"></div>\n"
  "  <div class=\"ctrl\"><label>Contrast <span id=\"vCon\">0</span></label>\n"
  "    <input type=\"range\" id=\"sCon\" min=\"-100\" max=\"100\" value=\"0\"></div>\n"
  "  <div class=\"ctrl\"><label>Gamma <span id=\"vGam\">1.00</span></label>\n"
  "    <input type=\"range\" id=\"sGam\" min=\"50\" max=\"200\" value=\"100\"></div>\n"
  "  <div class=\"ctrl\"><label>Exposure EV <span id=\"vExp\">0.00</span></label>\n"
  "    <input type=\"range\" id=\"sExp\" min=\"-200\" max=\"200\" value=\"0\"></div>\n"
  "  <div class=\"ctrl\"><label>Highlights <span id=\"vHi\">0</span></label>\n"
  "    <input type=\"range\" id=\"sHi\" min=\"-100\" max=\"100\" value=\"0\"></div>\n"
  "  <div class=\"ctrl\"><label>Shadows <span id=\"vSh\">0</span></label>\n"
  "    <input type=\"range\" id=\"sSh\" min=\"-100\" max=\"100\" value=\"0\"></div>\n"
  "  <div class=\"ctrl\"><label>White Point <span id=\"vWp\">255</span></label>\n"
  "    <input type=\"range\" id=\"sWp\" min=\"128\" max=\"255\" value=\"255\"></div>\n"
  "  <div class=\"ctrl\"><label>Black Point <span id=\"vBp\">0</span></label>\n"
  "    <input type=\"range\" id=\"sBp\" min=\"0\" max=\"64\" value=\"0\"></div>\n"
  "  <div class=\"ctrl\"><label>Temperature <span id=\"vTmp\">0</span></label>\n"
  "    <input type=\"range\" id=\"sTmp\" min=\"-100\" max=\"100\" value=\"0\"></div>\n"
  "  <div class=\"ctrl\"><label>Tint <span id=\"vTnt\">0</span></label>\n"
  "    <input type=\"range\" id=\"sTnt\" min=\"-100\" max=\"100\" value=\"0\"></div>\n"
  "  <div class=\"ctrl\"><label>Saturation <span id=\"vSat\">0</span></label>\n"
  "    <input type=\"range\" id=\"sSat\" min=\"-100\" max=\"100\" value=\"0\"></div>\n"
  "  <div class=\"ctrl\"><label>Vibrance <span id=\"vVib\">0</span></label>\n"
  "    <input type=\"range\" id=\"sVib\" min=\"-100\" max=\"100\" value=\"0\"></div>\n"
  "  <div class=\"ctrl\"><label>Sharpen <span id=\"vShp\">0</span></label>\n"
  "    <input type=\"range\" id=\"sShp\" min=\"0\" max=\"100\" value=\"0\"></div>\n"
  "  <div class=\"ctrl\"><label>Noise Reduction <span id=\"vNoi\">0</span></label>\n"
  "    <input type=\"range\" id=\"sNoi\" min=\"0\" max=\"100\" value=\"0\"></div>\n"
  "  <div class=\"ctrl\"><label>CLAHE Amount <span id=\"vCla\">50</span></label>\n"
  "    <input type=\"range\" id=\"sCla\" min=\"0\" max=\"100\" value=\"50\"></div>\n"
  "  <div class=\"ctrl\"><label>CLAHE Radius (tiles) <span id=\"vClR\">8</span></label>\n"
  "    <input type=\"range\" id=\"sClR\" min=\"2\" max=\"32\" value=\"8\"></div>\n"
  "  <div class=\"ctrl\"><label>CLAHE Clip Limit <span id=\"vClS\">3.0</span></label>\n"
  "    <input type=\"range\" id=\"sClS\" min=\"10\" max=\"80\" value=\"30\"></div>\n"
  "  <div class=\"ctrl\"><label>Edge Enhancement <span id=\"vEdg\">0</span></label>\n"
  "    <input type=\"range\" id=\"sEdg\" min=\"0\" max=\"100\" value=\"0\"></div>\n"
  "  <div class=\"ctrl\"><label>Local Contrast <span id=\"vLoc\">0</span></label>\n"
  "    <input type=\"range\" id=\"sLoc\" min=\"0\" max=\"100\" value=\"0\"></div>\n"
  "</div>\n"
  "\n"
  "<div class=\"divider\"></div>\n"
  "<div style=\"padding:4px 0 10px\">\n"
  "  <label style=\"font-size:.78rem;color:#777;display:block;margin-bottom:5px\">Dithering Algorithm</label>\n"
  "  <select id=\"selDither\">\n"
  "    <option value=\"auto\">Auto (scene-adaptive)</option>\n"
  "    <option value=\"none\">None (direct palette snap)</option>\n"
  "    <option value=\"ordered\">Ordered -- Bayer 4&#215;4</option>\n"
  "    <option value=\"ordered8\">Ordered -- Bayer 8&#215;8</option>\n"
  "    <option value=\"floyd\">Floyd-Steinberg</option>\n"
  "    <option value=\"jarvis\">Jarvis-Judice-Ninke</option>\n"
  "    <option value=\"stucki\">Stucki</option>\n"
  "    <option value=\"burkes\">Burkes</option>\n"
  "    <option value=\"atkinson\">Atkinson</option>\n"
  "  </select>\n"
  "</div>\n"
  "\n"
  "<div class=\"divider\"></div>\n"
  "<div class=\"checks\">\n"
  "  <label><input type=\"checkbox\" id=\"cAuto\" checked> Auto-select parameters per image</label>\n"
  "  <label><input type=\"checkbox\" id=\"cSkin\" checked> Protect skin tones (avoid mapping to blue/green)</label>\n"
  "  <label><input type=\"checkbox\" id=\"cSky\"  checked> Protect sky (bias toward blue)</label>\n"
  "  <label><input type=\"checkbox\" id=\"cVeg\"  checked> Protect vegetation (bias toward green)</label>\n"
  "  <label><input type=\"checkbox\" id=\"cSun\"  checked> Protect sunset / warm tones</label>\n"
  "</div>\n"
  "</details>\n"
  "</div>\n"
  "\n"
  "<div class=\"section\">\n"
  "<details>\n"
  "<summary>Histogram &amp; Palette Usage</summary>\n"
  "<div class=\"hist-wrap\">\n"
  "  <div class=\"hist-controls\">\n"
  "    <label><input type=\"checkbox\" id=\"cHL\" checked> Luminance</label>\n"
  "    <label><input type=\"checkbox\" id=\"cHR\" checked> RGB</label>\n"
  "  </div>\n"
  "  <canvas id=\"histCanvas\"></canvas>\n"
  "  <div class=\"divider\"></div>\n"
  "  <div style=\"font-size:.75rem;color:#666;margin-bottom:2px\">Palette usage</div>\n"
  "  <div class=\"pal-bar\" id=\"palBar\"></div>\n"
  "  <div class=\"pal-names\" id=\"palNames\"></div>\n"
  "</div>\n"
  "</details>\n"
  "</div>\n"
  "\n"
  "</div><!-- .main -->\n";


// ---- CHUNK 3a: JavaScript (color science, adjustments, CLAHE) ----
const char HTML_JS_A[] PROGMEM = 
  "<script>\n"
  "'use strict';\n"
  "// ================================================================\n"
  "// DISPLAY CONSTANTS\n"
  "// ================================================================\n"
  "const EPD_W = 600, EPD_H = 400;\n"
  "\n"
  "// Spectra 6 -- 6 colors, NO orange\n"
  "// Adjust RGB to match your panel's actual output\n"
  "const PALETTE = [\n"
  "  {name:'Black',  r:  0, g:  0, b:  0, code:0x0, hex:'#000000'},\n"
  "  {name:'White',  r:255, g:255, b:255, code:0x1, hex:'#ffffff'},\n"
  "  {name:'Green',  r:  0, g:155, b: 72, code:0x2, hex:'#009b48'},\n"
  "  {name:'Blue',   r:  0, g: 68, b:143, code:0x3, hex:'#00448f'},\n"
  "  {name:'Red',    r:228, g: 18, b: 18, code:0x4, hex:'#e41212'},\n"
  "  {name:'Yellow', r:255, g:218, b: 44, code:0x5, hex:'#ffda2c'},\n"
  "];\n"
  "\n"
  "// ================================================================\n"
  "// COLOR SCIENCE\n"
  "// ================================================================\n"
  "function srgbToLinear(c){\n"
  "  c/=255;\n"
  "  return c<=0.04045?c/12.92:Math.pow((c+0.055)/1.055,2.4);\n"
  "}\n"
  "function linearToSrgb(c){\n"
  "  c=Math.max(0,Math.min(1,c));\n"
  "  return Math.round((c<=0.0031308?12.92*c:1.055*Math.pow(c,1/2.4)-0.055)*255);\n"
  "}\n"
  "function rgbToXyz(r,g,b){\n"
  "  const rl=srgbToLinear(r),gl=srgbToLinear(g),bl=srgbToLinear(b);\n"
  "  return[\n"
  "    rl*0.4124564+gl*0.3575761+bl*0.1804375,\n"
  "    rl*0.2126729+gl*0.7151522+bl*0.0721750,\n"
  "    rl*0.0193339+gl*0.1191920+bl*0.9503041\n"
  "  ];\n"
  "}\n"
  "function xyzToLab(x,y,z){\n"
  "  // D65 reference white\n"
  "  const f=t=>t>0.008856?Math.cbrt(t):7.787*t+16/116;\n"
  "  return[116*f(y/1.00000)-16,500*(f(x/0.95047)-f(y/1.00000)),200*(f(y/1.00000)-f(z/1.08883))];\n"
  "}\n"
  "function rgbToLab(r,g,b){return xyzToLab(...rgbToXyz(r,g,b));}\n"
  "\n"
  "function labToXyz(L,a,b){\n"
  "  const fy=(L+16)/116,fx=a/500+fy,fz=fy-b/200;\n"
  "  const u=t=>t>0.206897?t*t*t:(t-16/116)/7.787;\n"
  "  return[u(fx)*0.95047,u(fy)*1.00000,u(fz)*1.08883];\n"
  "}\n"
  "function labToRgb(L,a,b){\n"
  "  const[x,y,z]=labToXyz(L,a,b);\n"
  "  return[\n"
  "    linearToSrgb( x*3.2404542-y*1.5371385-z*0.4985314),\n"
  "    linearToSrgb(-x*0.9692660+y*1.8760108+z*0.0415560),\n"
  "    linearToSrgb( x*0.0556434-y*0.2040259+z*1.0572252)\n"
  "  ];\n"
  "}\n"
  "function rgbToHsv(r,g,b){\n"
  "  r/=255;g/=255;b/=255;\n"
  "  const mx=Math.max(r,g,b),mn=Math.min(r,g,b),d=mx-mn;\n"
  "  let h=0;\n"
  "  if(d){\n"
  "    if(mx===r)h=((g-b)/d%6+6)%6;\n"
  "    else if(mx===g)h=(b-r)/d+2;\n"
  "    else h=(r-g)/d+4;\n"
  "    h*=60;\n"
  "  }\n"
  "  return[h,mx?d/mx:0,mx];\n"
  "}\n"
  "\n"
  "// Full CIEDE2000 formula\n"
  "function ciede2000(L1,a1,b1,L2,a2,b2){\n"
  "  const D2R=Math.PI/180,P7=25**7;\n"
  "  const C1=Math.hypot(a1,b1),C2=Math.hypot(a2,b2);\n"
  "  const Cb=(C1+C2)/2,C7=Cb**7;\n"
  "  const G=0.5*(1-Math.sqrt(C7/(C7+P7)));\n"
  "  const a1p=a1*(1+G),a2p=a2*(1+G);\n"
  "  const C1p=Math.hypot(a1p,b1),C2p=Math.hypot(a2p,b2);\n"
  "  let h1p=Math.atan2(b1,a1p)*180/Math.PI;if(h1p<0)h1p+=360;\n"
  "  let h2p=Math.atan2(b2,a2p)*180/Math.PI;if(h2p<0)h2p+=360;\n"
  "  const dLp=L2-L1,dCp=C2p-C1p;\n"
  "  let dhp=0;\n"
  "  if(C1p*C2p!==0){const dh=h2p-h1p;dhp=Math.abs(dh)<=180?dh:dh>180?dh-360:dh+360;}\n"
  "  const dHp=2*Math.sqrt(C1p*C2p)*Math.sin(dhp*D2R/2);\n"
  "  const Lb=(L1+L2)/2,Cbp=(C1p+C2p)/2;\n"
  "  const Hbp=C1p*C2p===0?h1p+h2p:Math.abs(h1p-h2p)<=180?(h1p+h2p)/2:h1p+h2p<360?(h1p+h2p+360)/2:(h1p+h2p-360)/2;\n"
  "  const T=1-0.17*Math.cos((Hbp-30)*D2R)+0.24*Math.cos(2*Hbp*D2R)+0.32*Math.cos((3*Hbp+6)*D2R)-0.20*Math.cos((4*Hbp-63)*D2R);\n"
  "  const SL=1+0.015*(Lb-50)**2/Math.sqrt(20+(Lb-50)**2);\n"
  "  const SC=1+0.045*Cbp,SH=1+0.015*Cbp*T;\n"
  "  const Cbp7=Cbp**7,RC=2*Math.sqrt(Cbp7/(Cbp7+P7));\n"
  "  const dTh=30*Math.exp(-((Hbp-275)/25)**2);\n"
  "  const RT=-Math.sin(2*dTh*D2R)*RC;\n"
  "  return Math.sqrt((dLp/SL)**2+(dCp/SC)**2+(dHp/SH)**2+RT*(dCp/SC)*(dHp/SH));\n"
  "}\n"
  "\n"
  "// Precompute LAB for each palette entry once\n"
  "const PAL_LAB=PALETTE.map(p=>rgbToLab(p.r,p.g,p.b));\n"
  "\n"
  "function nearestPalette(r,g,b,pSkin,pSky,pVeg,pSun){\n"
  "  const lab=rgbToLab(r,g,b);\n"
  "  const[h,s,v]=rgbToHsv(r,g,b);\n"
  "  let best=0,bestD=Infinity;\n"
  "  for(let i=0;i<PALETTE.length;i++){\n"
  "    let d=ciede2000(...lab,...PAL_LAB[i]);\n"
  "    // Color protection -- multiply distance to wrong-category targets\n"
  "    if(pSkin&&isSkin(r,g,b,h,s,v)){\n"
  "      if(i===2||i===3)d*=3.0;   // green & blue strongly penalised for skin\n"
  "    }\n"
  "    if(pSky&&isSky(r,g,b,h,s,v)){\n"
  "      if(i===2||i===4||i===5)d*=2.5;  // not green/red/yellow for sky\n"
  "    }\n"
  "    if(pVeg&&isVeg(r,g,b,h,s,v)){\n"
  "      if(i===3||i===4)d*=2.0;   // not blue/red for vegetation\n"
  "    }\n"
  "    if(pSun&&isSunset(r,g,b,h,s,v)){\n"
  "      if(i===2||i===3)d*=2.5;   // not green/blue for sunset\n"
  "    }\n"
  "    if(d<bestD){bestD=d;best=i;}\n"
  "  }\n"
  "  return best;\n"
  "}\n"
  "\n"
  "// ================================================================\n"
  "// SCENE DETECTION\n"
  "// ================================================================\n"
  "const isSkin=(r,g,b,h,s,v)=>h>=0&&h<=50&&s>.15&&s<.90&&v>.2&&v<.97&&r>60&&r>g&&r>b;\n"
  "const isSky =(r,g,b,h,s,v)=>h>=185&&h<=265&&s>.15&&v>.35&&b>80&&b>r;\n"
  "const isVeg =(r,g,b,h,s,v)=>h>=75&&h<=165&&s>.25&&v>.1&&g>50&&g>r*.8;\n"
  "const isSunset=(r,g,b,h,s,v)=>h>=5&&h<=55&&s>.35&&v>.4&&r>120;\n"
  "\n"
  "function analyzeScene(data){\n"
  "  const n=data.length/4;\n"
  "  const step=Math.max(1,Math.floor(n/5000));\n"
  "  let sk=0,sky=0,vg=0,su=0,dark=0,lite=0,tS=0,tV=0,tR=0,tG=0,tB=0,cnt=0;\n"
  "  for(let i=0;i<n;i+=step){\n"
  "    const r=data[i*4],g=data[i*4+1],b=data[i*4+2];\n"
  "    const[h,s,v]=rgbToHsv(r,g,b);\n"
  "    tS+=s;tV+=v;tR+=r;tG+=g;tB+=b;\n"
  "    if(isSkin(r,g,b,h,s,v))sk++;\n"
  "    if(isSky(r,g,b,h,s,v))sky++;\n"
  "    if(isVeg(r,g,b,h,s,v))vg++;\n"
  "    if(isSunset(r,g,b,h,s,v))su++;\n"
  "    if(v<.25)dark++;if(v>.8)lite++;\n"
  "    cnt++;\n"
  "  }\n"
  "  const t=cnt||1;\n"
  "  return{\n"
  "    skinR:sk/t,skyR:sky/t,vegR:vg/t,sunR:su/t,\n"
  "    avgSat:tS/t,avgVal:tV/t,avgR:tR/t,avgG:tG/t,avgB:tB/t,\n"
  "    darkR:dark/t,liteR:lite/t,\n"
  "    scene:sk/t>.25?'portrait':sky/t>.2?'sky':vg/t>.25?'nature':su/t>.12?'sunset':'general'\n"
  "  };\n"
  "}\n"
  "\n"
  "function autoParams(sc){\n"
  "  const P={bri:0,con:0,gam:1.0,sat:0,vib:0,cla:50,claR:8,claS:3.0,\n"
  "           hi:0,sh:0,tmp:0,tnt:0,exp:0,wp:255,bp:0,shp:0,noi:0,edg:0,loc:0};\n"
  "  // Tone: dark image\n"
  "  if(sc.avgVal<.35){P.bri=18;P.gam=.82;P.sh=22;P.cla=70;}\n"
  "  else if(sc.avgVal>.70){P.hi=-12;P.gam=1.05;P.cla=35;}\n"
  "  if(sc.darkR>.4){P.cla=78;P.claS=4.0;P.sh=28;}\n"
  "  if(sc.liteR>.5){P.cla=30;P.hi=-10;}\n"
  "  // Desaturated source\n"
  "  if(sc.avgSat<.15){P.sat+=22;P.vib+=18;}\n"
  "  // Per-scene tuning\n"
  "  switch(sc.scene){\n"
  "    case'portrait': P.sat=5;P.vib=12;P.gam=.96;P.cla=32;P.con=8;P.shp=15;break;\n"
  "    case'sky':      P.sat=18;P.con=12;P.cla=42;P.gam=1.04;P.tmp=-12;break;\n"
  "    case'nature':   P.sat=22;P.vib=16;P.con=14;P.cla=55;P.gam=.98;break;\n"
  "    case'sunset':   P.sat=28;P.con=16;P.tmp=22;P.gam=.90;P.cla=50;break;\n"
  "    default:        P.sat=10;P.con=6;P.cla=50;\n"
  "  }\n"
  "  return P;\n"
  "}\n"
  "\n"
  "// ================================================================\n"
  "// IMAGE ADJUSTMENTS\n"
  "// ================================================================\n"
  "function adjBrightnessContrast(data,bri,con){\n"
  "  const B=bri/100*255,F=(259*(con+255))/(255*(259-con));\n"
  "  for(let i=0;i<data.length;i+=4)\n"
  "    for(let c=0;c<3;c++)data[i+c]=Math.max(0,Math.min(255,F*(data[i+c]-128)+128+B));\n"
  "}\n"
  "function adjGamma(data,gam){\n"
  "  if(Math.abs(gam-1)<.005)return;\n"
  "  const lut=new Uint8ClampedArray(256);\n"
  "  for(let i=0;i<256;i++)lut[i]=Math.round(Math.pow(i/255,1/gam)*255);\n"
  "  for(let i=0;i<data.length;i+=4){data[i]=lut[data[i]];data[i+1]=lut[data[i+1]];data[i+2]=lut[data[i+2]];}\n"
  "}\n"
  "function adjExposure(data,ev){\n"
  "  if(ev===0)return;\n"
  "  const m=Math.pow(2,ev);\n"
  "  for(let i=0;i<data.length;i+=4){\n"
  "    data[i]=Math.min(255,data[i]*m);\n"
  "    data[i+1]=Math.min(255,data[i+1]*m);\n"
  "    data[i+2]=Math.min(255,data[i+2]*m);\n"
  "  }\n"
  "}\n"
  "function adjHiSh(data,hi,sh){\n"
  "  if(hi===0&&sh===0)return;\n"
  "  const hl=hi/100,sl=sh/100;\n"
  "  for(let i=0;i<data.length;i+=4)\n"
  "    for(let c=0;c<3;c++){\n"
  "      let v=data[i+c]/255;\n"
  "      if(v>.5)v+=hl*(v-.5)*2*(1-v)*2;\n"
  "      if(v<.5)v+=sl*(.5-v)*2*v*2;\n"
  "      data[i+c]=Math.max(0,Math.min(255,v*255));\n"
  "    }\n"
  "}\n"
  "function adjLevels(data,bp,wp){\n"
  "  if(bp===0&&wp===255)return;\n"
  "  const range=wp-bp||1;\n"
  "  for(let i=0;i<data.length;i+=4)\n"
  "    for(let c=0;c<3;c++)data[i+c]=Math.max(0,Math.min(255,((data[i+c]-bp)/range)*255));\n"
  "}\n"
  "function adjTemperature(data,tmp,tnt){\n"
  "  if(tmp===0&&tnt===0)return;\n"
  "  const T=tmp/100*28,Ti=tnt/100*14;\n"
  "  for(let i=0;i<data.length;i+=4){\n"
  "    data[i]=Math.max(0,Math.min(255,data[i]+T));\n"
  "    data[i+1]=Math.max(0,Math.min(255,data[i+1]+Ti));\n"
  "    data[i+2]=Math.max(0,Math.min(255,data[i+2]-T));\n"
  "  }\n"
  "}\n"
  "function adjSaturation(data,sat){\n"
  "  if(sat===0)return;\n"
  "  const s=1+sat/100;\n"
  "  for(let i=0;i<data.length;i+=4){\n"
  "    const l=.2126*data[i]/255+.7152*data[i+1]/255+.0722*data[i+2]/255;\n"
  "    data[i]=Math.max(0,Math.min(255,(l+(data[i]/255-l)*s)*255));\n"
  "    data[i+1]=Math.max(0,Math.min(255,(l+(data[i+1]/255-l)*s)*255));\n"
  "    data[i+2]=Math.max(0,Math.min(255,(l+(data[i+2]/255-l)*s)*255));\n"
  "  }\n"
  "}\n"
  "function adjVibrance(data,vib){\n"
  "  if(vib===0)return;\n"
  "  const v=vib/100;\n"
  "  for(let i=0;i<data.length;i+=4){\n"
  "    const r=data[i]/255,g=data[i+1]/255,b=data[i+2]/255;\n"
  "    const mx=Math.max(r,g,b),mn=Math.min(r,g,b);\n"
  "    const sat=mx?(mx-mn)/mx:0;\n"
  "    const boost=v*(1-sat)*1.8;\n"
  "    const l=.2126*r+.7152*g+.0722*b,s2=1+boost;\n"
  "    data[i]=Math.max(0,Math.min(255,(l+(r-l)*s2)*255));\n"
  "    data[i+1]=Math.max(0,Math.min(255,(l+(g-l)*s2)*255));\n"
  "    data[i+2]=Math.max(0,Math.min(255,(l+(b-l)*s2)*255));\n"
  "  }\n"
  "}\n"
  "function adjSharpen(data,W,H,amt){\n"
  "  if(amt<=0)return;\n"
  "  const k=amt/100*1.8,src=new Uint8ClampedArray(data);\n"
  "  const kn=[-k/8,-k/8,-k/8,-k/8,1+k,-k/8,-k/8,-k/8,-k/8];\n"
  "  for(let y=1;y<H-1;y++)for(let x=1;x<W-1;x++)\n"
  "    for(let c=0;c<3;c++){\n"
  "      let v=0;\n"
  "      for(let ky=-1;ky<=1;ky++)for(let kx=-1;kx<=1;kx++)\n"
  "        v+=src[((y+ky)*W+(x+kx))*4+c]*kn[(ky+1)*3+(kx+1)];\n"
  "      data[(y*W+x)*4+c]=Math.max(0,Math.min(255,v));\n"
  "    }\n"
  "}\n"
  "function adjNoise(data,W,H,amt){\n"
  "  if(amt<=0)return;\n"
  "  const a=amt/100,src=new Uint8ClampedArray(data);\n"
  "  for(let y=1;y<H-1;y++)for(let x=1;x<W-1;x++)\n"
  "    for(let c=0;c<3;c++){\n"
  "      let s=0;\n"
  "      for(let ky=-1;ky<=1;ky++)for(let kx=-1;kx<=1;kx++)s+=src[((y+ky)*W+(x+kx))*4+c];\n"
  "      data[(y*W+x)*4+c]=Math.round(src[(y*W+x)*4+c]*(1-a)+s/9*a);\n"
  "    }\n"
  "}\n"
  "function adjEdge(data,W,H,amt){\n"
  "  if(amt<=0)return;\n"
  "  const a=amt/200,src=new Uint8ClampedArray(data);\n"
  "  for(let y=1;y<H-1;y++)for(let x=1;x<W-1;x++)\n"
  "    for(let c=0;c<3;c++){\n"
  "      const gx=src[((y-1)*W+x+1)*4+c]-src[((y-1)*W+x-1)*4+c]\n"
  "               +2*src[(y*W+x+1)*4+c]-2*src[(y*W+x-1)*4+c]\n"
  "               +src[((y+1)*W+x+1)*4+c]-src[((y+1)*W+x-1)*4+c];\n"
  "      const gy=src[((y+1)*W+x-1)*4+c]+2*src[((y+1)*W+x)*4+c]+src[((y+1)*W+x+1)*4+c]\n"
  "               -src[((y-1)*W+x-1)*4+c]-2*src[((y-1)*W+x)*4+c]-src[((y-1)*W+x+1)*4+c];\n"
  "      data[(y*W+x)*4+c]=Math.max(0,Math.min(255,src[(y*W+x)*4+c]+a*Math.sqrt(gx*gx+gy*gy)*0.5));\n"
  "    }\n"
  "}\n"
  "function adjLocalContrast(data,W,H,amt){\n"
  "  if(amt<=0)return;\n"
  "  const a=amt/200,r=4,src=new Uint8ClampedArray(data);\n"
  "  for(let y=0;y<H;y++)for(let x=0;x<W;x++){\n"
  "    let sr=0,sg=0,sb=0,cnt=0;\n"
  "    for(let dy=-r;dy<=r;dy++)for(let dx=-r;dx<=r;dx++){\n"
  "      const ny=Math.min(H-1,Math.max(0,y+dy)),nx=Math.min(W-1,Math.max(0,x+dx));\n"
  "      sr+=src[(ny*W+nx)*4];sg+=src[(ny*W+nx)*4+1];sb+=src[(ny*W+nx)*4+2];cnt++;\n"
  "    }\n"
  "    const i=(y*W+x)*4;\n"
  "    data[i]=Math.max(0,Math.min(255,src[i]+(src[i]-sr/cnt)*a));\n"
  "    data[i+1]=Math.max(0,Math.min(255,src[i+1]+(src[i+1]-sg/cnt)*a));\n"
  "    data[i+2]=Math.max(0,Math.min(255,src[i+2]+(src[i+2]-sb/cnt)*a));\n"
  "  }\n"
  "}\n"
  "\n"
  "// ================================================================\n"
  "// CLAHE  (applied to L* channel in CIELab)\n"
  "// ================================================================\n"
  "function applyCLAHE(data,W,H,tileR,clipLim){\n"
  "  if(clipLim<=0.01)return;\n"
  "  const tw=Math.max(4,tileR*2),th=Math.max(4,tileR*2);\n"
  "  const nX=Math.ceil(W/tw),nY=Math.ceil(H/th);\n"
  "  // Extract L channel (rough mapping to 0-255)\n"
  "  const Lch=new Float32Array(W*H);\n"
  "  for(let i=0;i<W*H;i++){\n"
  "    const[L]=rgbToLab(data[i*4],data[i*4+1],data[i*4+2]);\n"
  "    Lch[i]=(L+0)*255/100;  // L is 0-100\n"
  "  }\n"
  "  // Build CLUTs per tile\n"
  "  const clut=new Float32Array(nX*nY*256);\n"
  "  for(let ty=0;ty<nY;ty++)for(let tx=0;tx<nX;tx++){\n"
  "    const x0=tx*tw,y0=ty*th,x1=Math.min(x0+tw,W),y1=Math.min(y0+th,H);\n"
  "    const hist=new Uint32Array(256);\n"
  "    let cnt=0;\n"
  "    for(let y=y0;y<y1;y++)for(let x=x0;x<x1;x++){\n"
  "      hist[Math.round(Math.min(255,Math.max(0,Lch[y*W+x])))]++;cnt++;\n"
  "    }\n"
  "    const clip=Math.max(1,Math.round(clipLim*cnt/256));\n"
  "    let ex=0;\n"
  "    for(let i=0;i<256;i++){if(hist[i]>clip){ex+=hist[i]-clip;hist[i]=clip;}}\n"
  "    const add=Math.floor(ex/256);\n"
  "    for(let i=0;i<256;i++)hist[i]+=add;\n"
  "    const cdf=new Float32Array(256);\n"
  "    cdf[0]=hist[0];for(let i=1;i<256;i++)cdf[i]=cdf[i-1]+hist[i];\n"
  "    const cmin=cdf.find(v=>v>0)||0;\n"
  "    const base=(ty*nX+tx)*256;\n"
  "    for(let i=0;i<256;i++)clut[base+i]=cnt>cmin?(cdf[i]-cmin)/(cnt-cmin)*255:i;\n"
  "  }\n"
  "  // Bilinear interpolation between tiles\n"
  "  for(let y=0;y<H;y++)for(let x=0;x<W;x++){\n"
  "    const tx=(x/W)*(nX-1),ty=(y/H)*(nY-1);\n"
  "    const tx0=Math.floor(tx),ty0=Math.floor(ty);\n"
  "    const tx1=Math.min(tx0+1,nX-1),ty1=Math.min(ty0+1,nY-1);\n"
  "    const fx=tx-tx0,fy=ty-ty0;\n"
  "    const Lv=Math.round(Math.min(255,Math.max(0,Lch[y*W+x])));\n"
  "    const c00=clut[(ty0*nX+tx0)*256+Lv],c10=clut[(ty0*nX+tx1)*256+Lv];\n"
  "    const c01=clut[(ty1*nX+tx0)*256+Lv],c11=clut[(ty1*nX+tx1)*256+Lv];\n"
  "    const newL=c00*(1-fx)*(1-fy)+c10*fx*(1-fy)+c01*(1-fx)*fy+c11*fx*fy;\n"
  "    const shift=newL-Lch[y*W+x];\n"
  "    const i=y*W+x;\n"
  "    data[i*4]=Math.max(0,Math.min(255,data[i*4]+shift));\n"
  "    data[i*4+1]=Math.max(0,Math.min(255,data[i*4+1]+shift));\n"
  "    data[i*4+2]=Math.max(0,Math.min(255,data[i*4+2]+shift));\n"
  "  }\n"
  "}\n"
  "\n"
  "// ================================================================\n"
  "// DITHERING\n"
  "// ================================================================\n";


// ---- CHUNK 3b: JavaScript (dithering, pipeline, UI) ----
const char HTML_JS_B[] PROGMEM = 
  "const B4=[0,8,2,10,12,4,14,6,3,11,1,9,15,7,13,5];\n"
  "const B8=[0,32,8,40,2,34,10,42,48,16,56,24,50,18,58,26,12,44,4,36,14,46,6,38,60,28,52,20,62,30,54,22,3,35,11,43,1,33,9,41,51,19,59,27,49,17,57,25,15,47,7,39,13,45,5,37,63,31,55,23,61,29,53,21];\n"
  "const K_FLOYD=[[1,0,7/16],[-1,1,3/16],[0,1,5/16],[1,1,1/16]];\n"
  "const K_JARVIS=[[1,0,7/48],[2,0,5/48],[-2,1,3/48],[-1,1,5/48],[0,1,7/48],[1,1,5/48],[2,1,3/48],[-2,2,1/48],[-1,2,3/48],[0,2,5/48],[1,2,3/48],[2,2,1/48]];\n"
  "const K_STUCKI=[[1,0,8/42],[2,0,4/42],[-2,1,2/42],[-1,1,4/42],[0,1,8/42],[1,1,4/42],[2,1,2/42],[-2,2,1/42],[-1,2,2/42],[0,2,4/42],[1,2,2/42],[2,2,1/42]];\n"
  "const K_BURKES=[[1,0,8/32],[2,0,4/32],[-2,1,2/32],[-1,1,4/32],[0,1,8/32],[1,1,4/32],[2,1,2/32]];\n"
  "const K_ATKINSON=[[1,0,1/8],[2,0,1/8],[-1,1,1/8],[0,1,1/8],[1,1,1/8],[0,2,1/8]];\n"
  "\n"
  "function ditherDirect(data,W,H,pS,pSk,pV,pSu){\n"
  "  const r=new Uint8Array(W*H);\n"
  "  for(let i=0;i<W*H;i++)r[i]=nearestPalette(data[i*4],data[i*4+1],data[i*4+2],pS,pSk,pV,pSu);\n"
  "  return r;\n"
  "}\n"
  "function ditherOrdered(data,W,H,pS,pSk,pV,pSu,bayer,sz){\n"
  "  const r=new Uint8Array(W*H);\n"
  "  for(let y=0;y<H;y++)for(let x=0;x<W;x++){\n"
  "    const t=(bayer[(y%sz)*sz+(x%sz)]/(sz*sz-1)-.5)*28;\n"
  "    const i=(y*W+x)*4;\n"
  "    r[y*W+x]=nearestPalette(\n"
  "      Math.max(0,Math.min(255,data[i]+t)),\n"
  "      Math.max(0,Math.min(255,data[i+1]+t)),\n"
  "      Math.max(0,Math.min(255,data[i+2]+t)),\n"
  "      pS,pSk,pV,pSu);\n"
  "  }\n"
  "  return r;\n"
  "}\n"
  "function ditherError(data,W,H,pS,pSk,pV,pSu,kern){\n"
  "  const buf=new Float32Array(W*H*3);\n"
  "  for(let i=0;i<W*H;i++){buf[i*3]=data[i*4];buf[i*3+1]=data[i*4+1];buf[i*3+2]=data[i*4+2];}\n"
  "  const r=new Uint8Array(W*H);\n"
  "  for(let y=0;y<H;y++)for(let x=0;x<W;x++){\n"
  "    const i=y*W+x;\n"
  "    const ri=Math.max(0,Math.min(255,buf[i*3]));\n"
  "    const gi=Math.max(0,Math.min(255,buf[i*3+1]));\n"
  "    const bi=Math.max(0,Math.min(255,buf[i*3+2]));\n"
  "    const ci=nearestPalette(ri,gi,bi,pS,pSk,pV,pSu);\n"
  "    r[i]=ci;\n"
  "    const er=ri-PALETTE[ci].r,eg=gi-PALETTE[ci].g,eb=bi-PALETTE[ci].b;\n"
  "    for(const[dx,dy,w]of kern){\n"
  "      const nx=x+dx,ny=y+dy;\n"
  "      if(nx>=0&&nx<W&&ny>=0&&ny<H){\n"
  "        buf[(ny*W+nx)*3]+=er*w;buf[(ny*W+nx)*3+1]+=eg*w;buf[(ny*W+nx)*3+2]+=eb*w;\n"
  "      }\n"
  "    }\n"
  "  }\n"
  "  return r;\n"
  "}\n"
  "\n"
  "function autoPickDither(sc){\n"
  "  switch(sc.scene){\n"
  "    case'portrait': return'atkinson';   // clean, minimal grain for faces\n"
  "    case'sky':      return'stucki';     // smooth gradient handling\n"
  "    case'nature':   return'floyd';\n"
  "    case'sunset':   return'jarvis';\n"
  "    default:        return sc.avgSat>.45?'floyd':'stucki';\n"
  "  }\n"
  "}\n"
  "\n"
  "function dither(data,W,H,method,sc,pS,pSk,pV,pSu){\n"
  "  const alg=method==='auto'?autoPickDither(sc):method;\n"
  "  switch(alg){\n"
  "    case'none':     return ditherDirect(data,W,H,pS,pSk,pV,pSu);\n"
  "    case'ordered':  return ditherOrdered(data,W,H,pS,pSk,pV,pSu,B4,4);\n"
  "    case'ordered8': return ditherOrdered(data,W,H,pS,pSk,pV,pSu,B8,8);\n"
  "    case'floyd':    return ditherError(data,W,H,pS,pSk,pV,pSu,K_FLOYD);\n"
  "    case'jarvis':   return ditherError(data,W,H,pS,pSk,pV,pSu,K_JARVIS);\n"
  "    case'stucki':   return ditherError(data,W,H,pS,pSk,pV,pSu,K_STUCKI);\n"
  "    case'burkes':   return ditherError(data,W,H,pS,pSk,pV,pSu,K_BURKES);\n"
  "    case'atkinson': return ditherError(data,W,H,pS,pSk,pV,pSu,K_ATKINSON);\n"
  "    default:        return ditherError(data,W,H,pS,pSk,pV,pSu,K_FLOYD);\n"
  "  }\n"
  "}\n"
  "\n"
  "// ================================================================\n"
  "// FRAMEBUFFER  (4 bits/pixel, high nibble = pixel n, low = n+1)\n"
  "// Spectra 6 color codes: 0=Black 1=White 2=Green 3=Blue 4=Red 5=Yellow\n"
  "// ================================================================\n"
  "function makeFramebuffer(indices,W,H){\n"
  "  const buf=new Uint8Array(W*H/2);\n"
  "  for(let i=0;i<W*H;i+=2)\n"
  "    buf[i>>1]=(PALETTE[indices[i]].code<<4)|PALETTE[indices[i+1]].code;\n"
  "  return buf;\n"
  "}\n"
  "\n"
  "// ================================================================\n"
  "// HISTOGRAM + PALETTE PREVIEW\n"
  "// ================================================================\n"
  "function drawHistogram(imageData){\n"
  "  const c=document.getElementById('histCanvas');\n"
  "  const CW=c.clientWidth||300,CH=80;\n"
  "  c.width=CW;c.height=CH;\n"
  "  const ctx=c.getContext('2d');\n"
  "  ctx.clearRect(0,0,CW,CH);\n"
  "  const d=imageData.data;\n"
  "  const showL=document.getElementById('cHL').checked;\n"
  "  const showRGB=document.getElementById('cHR').checked;\n"
  "  if(!showL&&!showRGB)return;\n"
  "  const hL=new Uint32Array(256),hR=new Uint32Array(256),hG=new Uint32Array(256),hB=new Uint32Array(256);\n"
  "  for(let i=0;i<d.length;i+=4){\n"
  "    const l=Math.round(.2126*d[i]+.7152*d[i+1]+.0722*d[i+2]);\n"
  "    hL[l]++;hR[d[i]]++;hG[d[i+1]]++;hB[d[i+2]]++;\n"
  "  }\n"
  "  const mx=Math.max(showL?Math.max(...hL):0,showRGB?Math.max(...hR,...hG,...hB):0)||1;\n"
  "  const draw=(h,col)=>{\n"
  "    ctx.fillStyle=col;\n"
  "    for(let i=0;i<256;i++){\n"
  "      const ht=h[i]/mx*(CH-1);\n"
  "      ctx.fillRect(Math.floor(i*CW/256),CH-ht,Math.ceil(CW/256)+1,ht);\n"
  "    }\n"
  "  };\n"
  "  if(showRGB){ctx.globalAlpha=.5;draw(hR,'#e06060');draw(hG,'#50b850');draw(hB,'#5090e0');ctx.globalAlpha=1;}\n"
  "  if(showL){ctx.globalAlpha=.75;draw(hL,'rgba(210,210,210,.9)');ctx.globalAlpha=1;}\n"
  "}\n"
  "\n"
  "function renderPaletteBar(counts,total){\n"
  "  const bar=document.getElementById('palBar'),names=document.getElementById('palNames');\n"
  "  bar.innerHTML='';names.innerHTML='';\n"
  "  for(let i=0;i<PALETTE.length;i++){\n"
  "    const pct=total?Math.round(counts[i]/total*100):Math.round(100/PALETTE.length);\n"
  "    const sw=document.createElement('div');\n"
  "    sw.className='pal-swatch';\n"
  "    sw.style.cssText=`background:${PALETTE[i].hex};flex:${Math.max(counts[i],1)}`;\n"
  "    sw.setAttribute('data-pct',pct+'%');\n"
  "    if(i===0)sw.style.color='#fff';\n"
  "    if(i===4||i===2)sw.style.color='#fff';\n"
  "    bar.appendChild(sw);\n"
  "    const nm=document.createElement('span');\n"
  "    nm.textContent=PALETTE[i].name.slice(0,3);\n"
  "    nm.title=`${PALETTE[i].name}: ${pct}%`;\n"
  "    names.appendChild(nm);\n"
  "  }\n"
  "}\n"
  "\n"
  "// ================================================================\n"
  "// MAIN PIPELINE\n"
  "// ================================================================\n"
  "let srcBmp=null, framebuf=null, lastScene=null;\n"
  "\n"
  "async function processImage(){\n"
  "  if(!srcBmp){setStatus('No image loaded.');return;}\n"
  "  setBtns(false);\n"
  "  setProgress(true,0);\n"
  "\n"
  "  // Step 1 -- Resize\n"
  "  setStatus('Resizing to 600x400...');await tick();\n"
  "  const oc=new OffscreenCanvas(EPD_W,EPD_H);\n"
  "  const octx=oc.getContext('2d');\n"
  "  octx.imageSmoothingEnabled=true;octx.imageSmoothingQuality='high';\n"
  "  octx.drawImage(srcBmp,0,0,EPD_W,EPD_H);\n"
  "  const id=octx.getImageData(0,0,EPD_W,EPD_H);\n"
  "  const d=id.data;\n"
  "  setProgress(true,5);\n"
  "\n"
  "  // Step 2 -- Scene analysis\n"
  "  setStatus('Analyzing scene...');await tick();\n"
  "  const sc=analyzeScene(d);\n"
  "  lastScene=sc;\n"
  "  renderSceneInfo(sc);\n"
  "  setProgress(true,10);\n"
  "\n"
  "  // Step 3 -- Parameters\n"
  "  const autoMode=document.getElementById('cAuto').checked;\n"
  "  let P;\n"
  "  if(autoMode){P=autoParams(sc);pushToSliders(P);}\n"
  "  else P=readSliders();\n"
  "\n"
  "  const pS=document.getElementById('cSkin').checked;\n"
  "  const pSk=document.getElementById('cSky').checked;\n"
  "  const pV=document.getElementById('cVeg').checked;\n"
  "  const pSu=document.getElementById('cSun').checked;\n"
  "  setProgress(true,15);\n"
  "\n"
  "  // Step 4 -- Tone adjustments\n"
  "  setStatus('Applying tone adjustments...');await tick();\n"
  "  adjExposure(d,P.exp);\n"
  "  adjHiSh(d,P.hi,P.sh);\n"
  "  adjLevels(d,P.bp,P.wp);\n"
  "  adjBrightnessContrast(d,P.bri,P.con);\n"
  "  adjTemperature(d,P.tmp,P.tnt);\n"
  "  adjSaturation(d,P.sat);\n"
  "  adjVibrance(d,P.vib);\n"
  "  setProgress(true,30);\n"
  "\n"
  "  // Step 5 -- Filters\n"
  "  setStatus('Sharpening / noise / edge / local contrast...');await tick();\n"
  "  if(P.noi>0)adjNoise(d,EPD_W,EPD_H,P.noi);\n"
  "  if(P.shp>0)adjSharpen(d,EPD_W,EPD_H,P.shp);\n"
  "  if(P.edg>0)adjEdge(d,EPD_W,EPD_H,P.edg);\n"
  "  if(P.loc>0)adjLocalContrast(d,EPD_W,EPD_H,P.loc);\n"
  "  setProgress(true,45);\n"
  "\n"
  "  // Step 6 -- CLAHE\n"
  "  setStatus('CLAHE...');await tick();\n"
  "  if(P.cla>0){\n"
  "    const clip=(P.claS)*(P.cla/100);\n"
  "    applyCLAHE(d,EPD_W,EPD_H,P.claR,clip);\n"
  "  }\n"
  "  setProgress(true,60);\n"
  "\n"
  "  // Step 7 -- Gamma\n"
  "  adjGamma(d,P.gam);\n"
  "  setProgress(true,65);\n"
  "\n"
  "  // Step 8 -- Palette mapping + dithering\n"
  "  setStatus('CIEDE2000 palette mapping + dithering...');await tick();\n"
  "  const method=document.getElementById('selDither').value;\n"
  "  const indices=dither(d,EPD_W,EPD_H,method,sc,pS,pSk,pV,pSu);\n"
  "  setProgress(true,90);\n"
  "\n"
  "  // Step 9 -- Preview\n"
  "  setStatus('Rendering preview...');await tick();\n"
  "  const pxBuf=new Uint8ClampedArray(EPD_W*EPD_H*4);\n"
  "  const palCnt=new Uint32Array(PALETTE.length);\n"
  "  for(let i=0;i<EPD_W*EPD_H;i++){\n"
  "    const ci=indices[i];palCnt[ci]++;\n"
  "    pxBuf[i*4]=PALETTE[ci].r;pxBuf[i*4+1]=PALETTE[ci].g;pxBuf[i*4+2]=PALETTE[ci].b;pxBuf[i*4+3]=255;\n"
  "  }\n"
  "  const pv=document.getElementById('previewCanvas');\n"
  "  pv.width=EPD_W;pv.height=EPD_H;\n"
  "  pv.getContext('2d').putImageData(new ImageData(pxBuf,EPD_W,EPD_H),0,0);\n"
  "  setProgress(true,95);\n"
  "\n"
  "  // Histogram + palette bar\n"
  "  drawHistogram(new ImageData(d,EPD_W,EPD_H));\n"
  "  renderPaletteBar(palCnt,EPD_W*EPD_H);\n"
  "\n"
  "  // Step 10 -- Framebuffer\n"
  "  framebuf=makeFramebuffer(indices,EPD_W,EPD_H);\n"
  "  setProgress(true,100);\n"
  "\n"
  "  const dithName=method==='auto'?`auto -> ${autoPickDither(sc)}`:method;\n"
  "  setStatus(`Ready! ${EPD_W}x${EPD_H}, ${(framebuf.length/1024).toFixed(0)} KB | scene: ${sc.scene} | dither: ${dithName}`);\n"
  "  setProgress(false,0);\n"
  "  setBtns(true);\n"
  "}\n"
  "\n"
  "// ================================================================\n"
  "// SEND TO DISPLAY  (chunked binary upload)\n"
  "// ================================================================\n"
  "async function sendDisplay(){\n"
  "  if(!framebuf){setStatus('Process an image first.');return;}\n"
  "  document.getElementById('btnSend').disabled=true;\n"
  "  setProgress(true,0);\n"
  "  const CHUNK=8192;\n"
  "  const total=framebuf.length;   // 120000 bytes for 600x400\n"
  "  const chunks=Math.ceil(total/CHUNK);\n"
  "  for(let c=0;c<chunks;c++){\n"
  "    const off=c*CHUNK;\n"
  "    const slice=framebuf.slice(off,Math.min(off+CHUNK,total));\n"
  "    setStatus(`Uploading chunk ${c+1}/${chunks}...`);\n"
  "    setProgress(true,(c/chunks)*88);\n"
  "    try{\n"
  "      const res=await fetch('/chunk?o='+off,{method:'POST',headers:{'Content-Type':'application/octet-stream'},body:slice});\n"
  "      if(!res.ok)throw new Error('HTTP '+res.status);\n"
  "    }catch(e){\n"
  "      setStatus('Upload error: '+e.message);\n"
  "      setProgress(false,0);\n"
  "      document.getElementById('btnSend').disabled=false;\n"
  "      return;\n"
  "    }\n"
  "  }\n"
  "  setProgress(true,95);\n"
  "  setStatus('Triggering display refresh...');\n"
  "  await fetch('/show',{method:'POST'});\n"
  "  setProgress(true,100);\n"
  "  setStatus('Sent! Display refreshing (~20-30 seconds) -- do not power off.');\n"
  "  setProgress(false,0);\n"
  "  document.getElementById('btnSend').disabled=false;\n"
  "}\n"
  "\n"
  "// ================================================================\n"
  "// SLIDER / PARAMS HELPERS\n"
  "// ================================================================\n"
  "const SLIDER_MAP=[\n"
  "  ['sBri','vBri',v=>v,     'bri'],\n"
  "  ['sCon','vCon',v=>v,     'con'],\n"
  "  ['sGam','vGam',v=>(v/100).toFixed(2),'gam',v=>v/100],\n"
  "  ['sExp','vExp',v=>(v/100).toFixed(2)+' EV','exp',v=>v/100],\n"
  "  ['sHi', 'vHi', v=>v,     'hi'],\n"
  "  ['sSh', 'vSh', v=>v,     'sh'],\n"
  "  ['sWp', 'vWp', v=>v,     'wp'],\n"
  "  ['sBp', 'vBp', v=>v,     'bp'],\n"
  "  ['sTmp','vTmp',v=>v,     'tmp'],\n"
  "  ['sTnt','vTnt',v=>v,     'tnt'],\n"
  "  ['sSat','vSat',v=>v,     'sat'],\n"
  "  ['sVib','vVib',v=>v,     'vib'],\n"
  "  ['sShp','vShp',v=>v,     'shp'],\n"
  "  ['sNoi','vNoi',v=>v,     'noi'],\n"
  "  ['sCla','vCla',v=>v,     'cla'],\n"
  "  ['sClR','vClR',v=>v,     'claR'],\n"
  "  ['sClS','vClS',v=>(v/10).toFixed(1),'claS',v=>v/10],\n"
  "  ['sEdg','vEdg',v=>v,     'edg'],\n"
  "  ['sLoc','vLoc',v=>v,     'loc'],\n"
  "];\n"
  "function readSliders(){\n"
  "  const P={};\n"
  "  for(const[sid,,fmt,key,xform]of SLIDER_MAP){\n"
  "    const v=+document.getElementById(sid).value;\n"
  "    P[key]=xform?xform(v):v;\n"
  "  }\n"
  "  return P;\n"
  "}\n"
  "function pushToSliders(P){\n"
  "  const pushes={bri:'sBri',con:'sCon',sat:'sSat',vib:'sVib',hi:'sHi',sh:'sSh',tmp:'sTmp',tnt:'sTnt',shp:'sShp',noi:'sNoi',edg:'sEdg',loc:'sLoc',cla:'sCla',claR:'sClR'};\n"
  "  for(const[k,id]of Object.entries(pushes)){\n"
  "    const el=document.getElementById(id);if(el&&P[k]!=null)el.value=String(P[k]);\n"
  "  }\n"
  "  if(P.gam!=null)document.getElementById('sGam').value=String(Math.round(P.gam*100));\n"
  "  if(P.claS!=null)document.getElementById('sClS').value=String(Math.round(P.claS*10));\n"
  "  if(P.wp!=null)document.getElementById('sWp').value=String(P.wp);\n"
  "  if(P.bp!=null)document.getElementById('sBp').value=String(P.bp);\n"
  "  if(P.exp!=null)document.getElementById('sExp').value=String(Math.round(P.exp*100));\n"
  "  refreshLabels();\n"
  "}\n"
  "function refreshLabels(){\n"
  "  for(const[sid,vid,fmt]of SLIDER_MAP){\n"
  "    const s=document.getElementById(sid),v=document.getElementById(vid);\n"
  "    if(s&&v)v.textContent=fmt(s.value);\n"
  "  }\n"
  "}\n"
  "\n"
  "// ================================================================\n"
  "// UI HELPERS\n"
  "// ================================================================\n"
  "function setStatus(m){document.getElementById('statusBar').textContent=m;}\n"
  "function setProgress(show,pct){\n"
  "  document.getElementById('progWrap').style.display=show?'block':'none';\n"
  "  document.getElementById('progBar').style.width=pct+'%';\n"
  "}\n"
  "function setBtns(ready){\n"
  "  document.getElementById('btnProcess').disabled=false;\n"
  "  document.getElementById('btnSend').disabled=!ready||!framebuf;\n"
  "}\n"
  "function tick(){return new Promise(r=>setTimeout(r,0));}\n"
  "\n"
  "function renderSceneInfo(sc){\n"
  "  const icons={portrait:'&#128100;',sky:'&#127780;',nature:'&#127807;',sunset:'&#127749;',general:'&#128444;'};\n"
  "  const parts=[`<span class=\"sbadge\">${icons[sc.scene]||''} ${sc.scene}</span>`];\n"
  "  if(sc.skinR>.08)parts.push(`<span class=\"sbadge\">Skin ${Math.round(sc.skinR*100)}%</span>`);\n"
  "  if(sc.skyR>.08) parts.push(`<span class=\"sbadge\">Sky ${Math.round(sc.skyR*100)}%</span>`);\n"
  "  if(sc.vegR>.08) parts.push(`<span class=\"sbadge\">Vegetation ${Math.round(sc.vegR*100)}%</span>`);\n"
  "  if(sc.sunR>.05) parts.push(`<span class=\"sbadge\">Sunset ${Math.round(sc.sunR*100)}%</span>`);\n"
  "  document.getElementById('sceneInfo').innerHTML=parts.join('');\n"
  "}\n"
  "\n"
  "// ================================================================\n"
  "// EVENT WIRING\n"
  "// ================================================================\n"
  "async function loadFile(file){\n"
  "  if(!file||!file.type.startsWith('image/'))return;\n"
  "  srcBmp=await createImageBitmap(file);\n"
  "  // Quick scaled preview\n"
  "  const pv=document.getElementById('previewCanvas');\n"
  "  pv.width=EPD_W;pv.height=EPD_H;\n"
  "  const ctx=pv.getContext('2d');\n"
  "  ctx.imageSmoothingEnabled=true;ctx.imageSmoothingQuality='high';\n"
  "  ctx.drawImage(srcBmp,0,0,EPD_W,EPD_H);\n"
  "  setStatus(`Loaded: ${srcBmp.width}x${srcBmp.height} px -- tap Process`);\n"
  "  document.getElementById('btnProcess').disabled=false;\n"
  "  framebuf=null;document.getElementById('btnSend').disabled=true;\n"
  "  document.getElementById('sceneInfo').innerHTML='';\n"
  "}\n"
  "\n"
  "document.getElementById('fileIn').addEventListener('change',e=>loadFile(e.target.files[0]));\n"
  "document.getElementById('btnProcess').addEventListener('click',processImage);\n"
  "document.getElementById('btnSend').addEventListener('click',sendDisplay);\n"
  "document.getElementById('btnClear').addEventListener('click',()=>{\n"
  "  fetch('/clear',{method:'POST'});\n"
  "  setStatus('Clear command sent -- display refreshing to white...');\n"
  "});\n"
  "\n"
  "// Drag & drop\n"
  "const dz=document.getElementById('dropZone');\n"
  "dz.addEventListener('dragover',e=>{e.preventDefault();dz.classList.add('drag');});\n"
  "dz.addEventListener('dragleave',()=>dz.classList.remove('drag'));\n"
  "dz.addEventListener('drop',e=>{e.preventDefault();dz.classList.remove('drag');loadFile(e.dataTransfer.files[0]);});\n"
  "\n"
  "// Sliders -- live labels\n"
  "document.querySelectorAll('input[type=range]').forEach(s=>s.addEventListener('input',refreshLabels));\n"
  "refreshLabels();\n"
  "\n"
  "// Auto-mode badge\n"
  "document.getElementById('cAuto').addEventListener('change',e=>{\n"
  "  const b=document.getElementById('autoModeBadge');\n"
  "  b.textContent=e.target.checked?'AUTO':'MANUAL';\n"
  "  b.className='badge '+(e.target.checked?'badge-auto':'badge-man');\n"
  "});\n"
  "\n"
  "// Histogram checkboxes\n"
  "['cHL','cHR'].forEach(id=>document.getElementById(id).addEventListener('change',()=>{\n"
  "  if(lastScene){/* redraw if we have data */}\n"
  "}));\n"
  "\n"
  "// Device status poll\n"
  "async function pollDevice(){\n"
  "  try{\n"
  "    await fetch('/status',{signal:AbortSignal.timeout(2000)});\n"
  "    document.getElementById('connStatus').innerHTML='&#9679; online';\n"
  "    document.getElementById('connStatus').style.color='#6c9b6c';\n"
  "  }catch{\n"
  "    document.getElementById('connStatus').innerHTML='&#9679; offline';\n"
  "    document.getElementById('connStatus').style.color='#9b5050';\n"
  "  }\n"
  "}\n"
  "pollDevice();setInterval(pollDevice,6000);\n"
  "\n"
  "// Initial palette bar (equal weights)\n"
  "renderPaletteBar(new Uint32Array(PALETTE.length).fill(1),PALETTE.length);\n"
  "</script>\n"
  "</body>\n"
  "</html>\n";


// ================================================================
// WEB SERVER
// ================================================================
void setup_server() {

  // Main page
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
    AsyncResponseStream* res = req->beginResponseStream("text/html");
    res->print(FPSTR(HTML_HEAD));
    res->print(FPSTR(HTML_BODY));
    res->print(FPSTR(HTML_JS_A));
    res->print(FPSTR(HTML_JS_B));
    req->send(res);
  });

  // Receive binary framebuffer chunk
  // POST /chunk?o=<byte_offset>   body: raw bytes
  server.on("/chunk", HTTP_POST,
    [](AsyncWebServerRequest* req){ req->send(200, "text/plain", "ok"); },
    nullptr,
    [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total){
      int off = req->hasParam("o") ? req->getParam("o")->value().toInt() : 0;
      size_t dest = (size_t)off + index;
      if (dest + len <= BUF_SZ) {
        memcpy(epd_buf + dest, data, len);
      }
    }
  );

  // Trigger display
  server.on("/show", HTTP_POST, [](AsyncWebServerRequest* req){
    g_show = true;
    req->send(200, "text/plain", "ok");
  });

  // Clear to white
  server.on("/clear", HTTP_POST, [](AsyncWebServerRequest* req){
    g_clear = true;
    req->send(200, "text/plain", "ok");
  });

  // Status
  server.on("/status", HTTP_GET, [](AsyncWebServerRequest* req){
    req->send(200, "text/plain", g_status);
  });

  server.onNotFound([](AsyncWebServerRequest* req){
    req->send(404, "text/plain", "not found");
  });
}
