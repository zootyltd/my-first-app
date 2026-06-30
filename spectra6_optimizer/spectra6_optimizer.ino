/*
 * ============================================================
 * Spectra 6 / 7-Color E-Paper Image Optimizer
 * Waveshare 4.01" 7-Color HAT + ESP32-S3
 *
 * REQUIRED LIBRARIES (install via Arduino Library Manager):
 *   - ESPAsyncWebServer  (search "ESPAsyncWebServer" by lacamera)
 *   - AsyncTCP           (search "AsyncTCP" by ESP32Async)
 *
 * HOW TO USE:
 *   1. Set WIFI_SSID and WIFI_PASS below
 *   2. Adjust pin numbers if your wiring differs
 *   3. Upload to ESP32-S3
 *   4. Open Serial Monitor at 115200 baud — note the IP address
 *   5. Open that IP in your phone browser
 *   6. Upload any image — it processes entirely in the browser, then sends
 *
 * DISPLAY RESOLUTION:
 *   7-color ACeP (4.01" HAT): 640 x 400  ← default
 *   6-color Spectra 6:        600 x 448  ← uncomment below if needed
 *
 * PALETTE CALIBRATION:
 *   Adjust PALETTE_RGB values to match the actual colors your panel
 *   produces — they vary between batches. Use a colorimeter if available.
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

// Display pins — ESP32-S3 + Waveshare HAT+
#define PIN_BUSY  13
#define PIN_RST   12
#define PIN_DC     8
#define PIN_CS     9
#define PIN_SCK   18
#define PIN_MOSI  23

// Uncomment ONE block:
#define EPD_W  640   // 7-color ACeP 4.01"
#define EPD_H  400
//#define EPD_W  600   // 6-color Spectra 6
//#define EPD_H  448

// ============================================================
// FRAMEBUFFER  (4 bits/pixel, 2 pixels per byte)
// ============================================================
#define BUF_SZ  (EPD_W * EPD_H / 2)

uint8_t epd_buf[BUF_SZ];
volatile bool g_show = false;
volatile bool g_clear = false;
String g_status = "Ready — open this page on your phone";

AsyncWebServer server(80);

// ============================================================
// EPD DRIVER — Waveshare EPD_4in01f (7-color ACeP)
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
static void epd_wait() {
  delay(10);
  while (digitalRead(PIN_BUSY) == HIGH) delay(50);
}
static void epd_reset() {
  digitalWrite(PIN_RST, HIGH); delay(20);
  digitalWrite(PIN_RST, LOW);  delay(2);
  digitalWrite(PIN_RST, HIGH); delay(20);
  epd_wait();
}

void epd_init() {
  pinMode(PIN_BUSY, INPUT);
  pinMode(PIN_RST,  OUTPUT);
  pinMode(PIN_DC,   OUTPUT);
  pinMode(PIN_CS,   OUTPUT);
  SPI.begin(PIN_SCK, -1, PIN_MOSI, PIN_CS);
  SPI.setFrequency(4000000UL);
  SPI.setBitOrder(MSBFIRST);
  SPI.setDataMode(SPI_MODE0);
  epd_reset();

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
  Serial.println("[EPD] Starting refresh (~15-30s)...");
  epd_cmd(0x61);
    epd_dat((EPD_W >> 8) & 0xFF); epd_dat(EPD_W & 0xFF);
    epd_dat((EPD_H >> 8) & 0xFF); epd_dat(EPD_H & 0xFF);
  epd_cmd(0x10);
  digitalWrite(PIN_DC, HIGH);
  digitalWrite(PIN_CS, LOW);
  SPI.writeBytes(epd_buf, BUF_SZ);
  digitalWrite(PIN_CS, HIGH);
  epd_cmd(0x04); epd_wait();
  epd_cmd(0x12); epd_wait();
  epd_cmd(0x02); epd_wait();
  Serial.println("[EPD] Refresh done.");
}

void epd_fill(uint8_t color) {
  uint8_t b = (color << 4) | color;
  epd_cmd(0x61);
    epd_dat((EPD_W >> 8) & 0xFF); epd_dat(EPD_W & 0xFF);
    epd_dat((EPD_H >> 8) & 0xFF); epd_dat(EPD_H & 0xFF);
  epd_cmd(0x10);
  digitalWrite(PIN_DC, HIGH);
  digitalWrite(PIN_CS, LOW);
  for (size_t i = 0; i < BUF_SZ; i++) SPI.transfer(b);
  digitalWrite(PIN_CS, HIGH);
  epd_cmd(0x04); epd_wait();
  epd_cmd(0x12); epd_wait();
  epd_cmd(0x02); epd_wait();
}

// ============================================================
// WEB SERVER HANDLERS
// ============================================================
void setup_server();

// ============================================================
// SETUP / LOOP
// ============================================================
void setup() {
  Serial.begin(115200);
  Serial.println("\n[BOOT] Spectra 6 Image Optimizer");

  epd_init();
  Serial.println("[EPD] Initialized. Clearing to white...");
  epd_fill(0x01);  // white
  Serial.println("[EPD] Clear done.");

  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("[WiFi] Connecting");
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries++ < 40) {
    delay(500); Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n[WiFi] Connected! Open: http://%s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("\n[WiFi] Failed — check credentials");
  }

  setup_server();
  server.begin();
  Serial.println("[HTTP] Server started");
}

void loop() {
  if (g_clear) {
    g_clear = false;
    g_status = "Clearing display...";
    epd_fill(0x01);
    g_status = "Display cleared.";
  }
  if (g_show) {
    g_show = false;
    g_status = "Refreshing display (~15-30s)...";
    epd_show_buf();
    g_status = "Image displayed! Ready for next.";
  }
  delay(10);
}

// ============================================================
// HTML PAGE — split into PROGMEM chunks
// ============================================================

// ---- CHUNK 1: DOCTYPE, head, CSS ----
const char HTML_HEAD[] PROGMEM = R"EPAPER(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1">
<title>Spectra 6 Image Optimizer</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{background:#111;color:#eee;font-family:system-ui,sans-serif;font-size:14px;min-height:100vh}
h1{font-size:1.1rem;font-weight:600;color:#fff;padding:12px 16px;background:#1a1a2e;border-bottom:1px solid #333;letter-spacing:.5px}
h1 span{color:#6c9bdf;font-size:.8rem;font-weight:400;margin-left:8px}
.main{display:flex;flex-direction:column;gap:0}
.upload-zone{border:2px dashed #444;border-radius:10px;margin:14px;padding:28px 16px;text-align:center;cursor:pointer;transition:.2s;background:#161616}
.upload-zone:hover,.upload-zone.drag{border-color:#6c9bdf;background:#1a1f2e}
.upload-zone p{color:#888;font-size:.85rem;margin-top:6px}
.upload-zone input[type=file]{display:none}
.btn{display:inline-flex;align-items:center;justify-content:center;gap:6px;padding:10px 18px;border-radius:7px;border:none;cursor:pointer;font-size:.85rem;font-weight:600;transition:.15s}
.btn-primary{background:#2e5fa3;color:#fff}.btn-primary:hover{background:#3d74c7}
.btn-primary:disabled{background:#333;color:#666;cursor:not-allowed}
.btn-danger{background:#6b2020;color:#fff}.btn-danger:hover{background:#8b3030}
.btn-sm{padding:6px 12px;font-size:.78rem}
.row{display:flex;gap:8px;flex-wrap:wrap;padding:0 14px 10px}
.preview-wrap{position:relative;margin:0 14px 10px;border-radius:8px;overflow:hidden;background:#0a0a0a;text-align:center}
#previewCanvas{max-width:100%;height:auto;display:block;margin:0 auto;border-radius:8px}
.status-bar{background:#1a1f1a;border:1px solid #2a3a2a;border-radius:6px;margin:0 14px 10px;padding:8px 12px;font-size:.8rem;color:#8fc08f;min-height:34px}
.progress-wrap{margin:0 14px 10px;height:6px;background:#1e1e1e;border-radius:3px;overflow:hidden;display:none}
.progress-bar{height:100%;background:#2e5fa3;width:0%;transition:width .1s;border-radius:3px}
.section{border-top:1px solid #222;padding:10px 14px}
.section summary{cursor:pointer;user-select:none;font-weight:600;color:#aaa;font-size:.85rem;padding:4px 0;list-style:none;display:flex;align-items:center;gap:6px}
.section summary::before{content:"▶";font-size:.65rem;transition:.2s}
details[open] summary::before{content:"▼"}
.section summary:hover{color:#ddd}
.ctrl-grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(200px,1fr));gap:8px 16px;padding:10px 0}
.ctrl{display:flex;flex-direction:column;gap:3px}
.ctrl label{font-size:.75rem;color:#888;display:flex;justify-content:space-between}
.ctrl label span{color:#bbb;font-weight:600}
.ctrl input[type=range]{width:100%;accent-color:#4a80d0;height:4px}
.ctrl select{background:#1e1e1e;color:#eee;border:1px solid #333;border-radius:5px;padding:5px 8px;font-size:.82rem;width:100%}
.checks{display:flex;flex-direction:column;gap:5px;padding:6px 0}
.checks label{display:flex;align-items:center;gap:7px;font-size:.82rem;cursor:pointer;color:#bbb}
.checks input[type=checkbox]{accent-color:#4a80d0;width:15px;height:15px}
.hist-section{padding:10px 14px}
.hist-title{font-size:.78rem;color:#666;margin-bottom:4px}
#histCanvas{width:100%;height:80px;background:#0d0d0d;border-radius:6px;display:block}
.palette-preview{display:flex;height:24px;border-radius:5px;overflow:hidden;margin:8px 0}
.pal-swatch{flex:1;transition:.3s;position:relative}
.pal-swatch::after{content:attr(data-pct);position:absolute;bottom:2px;left:50%;transform:translateX(-50%);font-size:.55rem;color:rgba(255,255,255,.7);white-space:nowrap}
.pal-labels{display:flex;font-size:.62rem;color:#666;margin-bottom:6px}
.pal-labels span{flex:1;text-align:center}
.divider{height:1px;background:#222;margin:6px 0}
.info-tip{font-size:.72rem;color:#556;font-style:italic;padding:4px 0}
.auto-badge{background:#1c3b1c;color:#6c9b6c;font-size:.68rem;padding:2px 6px;border-radius:10px;margin-left:4px}
.scene-badge{background:#1c2b3b;color:#6c8fb0;font-size:.7rem;padding:3px 8px;border-radius:10px;display:inline-block;margin:4px 0}
.temp-row{display:flex;gap:6px;align-items:center;font-size:.75rem;color:#888;padding:4px 0}
</style>
</head>
<body>
<h1>Spectra 6 Image Optimizer <span id="connStatus">● connecting...</span></h1>
<div class="main">
)EPAPER";

// ---- CHUNK 2: body HTML (controls) ----
const char HTML_BODY[] PROGMEM = R"EPAPER(
<div class="upload-zone" id="dropZone" onclick="document.getElementById('fileIn').click()">
  <svg width="36" height="36" fill="none" stroke="#555" stroke-width="1.5" viewBox="0 0 24 24">
    <path d="M4 16v2a2 2 0 002 2h12a2 2 0 002-2v-2M12 12V4m0 0L8 8m4-4l4 4"/>
  </svg>
  <p>Tap to select image</p>
  <p>or drag &amp; drop here</p>
  <input type="file" id="fileIn" accept="image/*">
</div>

<div class="row">
  <button class="btn btn-primary" id="btnProcess" disabled>Process Image</button>
  <button class="btn btn-primary" id="btnSend" disabled>Send to Display</button>
  <button class="btn btn-danger btn-sm" id="btnClear">Clear Display</button>
</div>

<div class="status-bar" id="statusBar">Upload an image to begin</div>
<div class="progress-wrap" id="progWrap"><div class="progress-bar" id="progBar"></div></div>

<div class="preview-wrap">
  <canvas id="previewCanvas" width="640" height="400"></canvas>
</div>
<div id="sceneInfo" style="text-align:center;padding:0 14px 6px"></div>

<div class="section">
<details open>
<summary>Advanced Controls <span class="auto-badge" id="autoModeBadge">AUTO ON</span></summary>
<div class="ctrl-grid">

  <div class="ctrl"><label>Brightness <span id="vBrightness">0</span></label>
    <input type="range" id="sBrightness" min="-100" max="100" value="0"></div>

  <div class="ctrl"><label>Contrast <span id="vContrast">0</span></label>
    <input type="range" id="sContrast" min="-100" max="100" value="0"></div>

  <div class="ctrl"><label>Gamma <span id="vGamma">1.0</span></label>
    <input type="range" id="sGamma" min="50" max="200" value="100"></div>

  <div class="ctrl"><label>Exposure EV <span id="vExposure">0.0</span></label>
    <input type="range" id="sExposure" min="-200" max="200" value="0"></div>

  <div class="ctrl"><label>Highlights <span id="vHighlights">0</span></label>
    <input type="range" id="sHighlights" min="-100" max="100" value="0"></div>

  <div class="ctrl"><label>Shadows <span id="vShadows">0</span></label>
    <input type="range" id="sShadows" min="-100" max="100" value="0"></div>

  <div class="ctrl"><label>White Point <span id="vWhite">255</span></label>
    <input type="range" id="sWhite" min="128" max="255" value="255"></div>

  <div class="ctrl"><label>Black Point <span id="vBlack">0</span></label>
    <input type="range" id="sBlack" min="0" max="64" value="0"></div>

  <div class="ctrl"><label>Temperature <span id="vTemp">0</span></label>
    <input type="range" id="sTemp" min="-100" max="100" value="0"></div>

  <div class="ctrl"><label>Tint <span id="vTint">0</span></label>
    <input type="range" id="sTint" min="-100" max="100" value="0"></div>

  <div class="ctrl"><label>Saturation <span id="vSaturation">0</span></label>
    <input type="range" id="sSaturation" min="-100" max="100" value="0"></div>

  <div class="ctrl"><label>Vibrance <span id="vVibrance">0</span></label>
    <input type="range" id="sVibrance" min="-100" max="100" value="0"></div>

  <div class="ctrl"><label>Sharpen <span id="vSharpen">0</span></label>
    <input type="range" id="sSharpen" min="0" max="100" value="0"></div>

  <div class="ctrl"><label>Noise Reduction <span id="vNoise">0</span></label>
    <input type="range" id="sNoise" min="0" max="100" value="0"></div>

  <div class="ctrl"><label>CLAHE Amount <span id="vClahe">50</span></label>
    <input type="range" id="sClahe" min="0" max="100" value="50"></div>

  <div class="ctrl"><label>CLAHE Radius <span id="vClaheR">8</span></label>
    <input type="range" id="sClaheR" min="2" max="32" value="8"></div>

  <div class="ctrl"><label>CLAHE Strength <span id="vClaheS">3.0</span></label>
    <input type="range" id="sClaheS" min="10" max="80" value="30"></div>

  <div class="ctrl"><label>Edge Enhancement <span id="vEdge">0</span></label>
    <input type="range" id="sEdge" min="0" max="100" value="0"></div>

  <div class="ctrl"><label>Local Contrast <span id="vLocal">0</span></label>
    <input type="range" id="sLocal" min="0" max="100" value="0"></div>

</div>

<div class="divider"></div>
<div style="padding:6px 0">
<label style="font-size:.8rem;color:#888;margin-bottom:4px;display:block">Dithering Algorithm</label>
<select id="selDither">
  <option value="auto">Auto (scene-adaptive)</option>
  <option value="none">None</option>
  <option value="ordered">Ordered (Bayer 4×4)</option>
  <option value="ordered8">Ordered (Bayer 8×8)</option>
  <option value="floyd">Floyd-Steinberg</option>
  <option value="jarvis">Jarvis-Judice-Ninke</option>
  <option value="stucki">Stucki</option>
  <option value="burkes">Burkes</option>
  <option value="atkinson">Atkinson</option>
</select>
</div>

<div class="divider"></div>
<div class="checks">
  <label><input type="checkbox" id="cAutoParams" checked> Auto-select processing parameters per image</label>
  <label><input type="checkbox" id="cSkin" checked> Protect skin tones</label>
  <label><input type="checkbox" id="cSky" checked> Protect sky</label>
  <label><input type="checkbox" id="cVeg" checked> Protect vegetation / grass</label>
  <label><input type="checkbox" id="cSunset" checked> Protect sunset / warm tones</label>
</div>

</details>
</div>

<div class="section">
<details>
<summary>Histogram &amp; Palette</summary>
<div class="hist-section">
  <div style="display:flex;gap:8px;margin-bottom:6px">
    <label style="font-size:.75rem;color:#888;display:flex;align-items:center;gap:5px">
      <input type="checkbox" id="cHistBrightness" checked style="accent-color:#aaa"> Luminance
    </label>
    <label style="font-size:.75rem;color:#888;display:flex;align-items:center;gap:5px">
      <input type="checkbox" id="cHistRGB" checked style="accent-color:#e06060"> RGB
    </label>
  </div>
  <canvas id="histCanvas"></canvas>
  <div class="divider" style="margin:10px 0"></div>
  <div class="hist-title">Palette Usage</div>
  <div class="palette-preview" id="palPreview"></div>
  <div class="pal-labels" id="palLabels"></div>
</div>
</details>
</div>

</div><!-- .main -->
)EPAPER";

// ---- CHUNK 3: JavaScript ----
const char HTML_JS[] PROGMEM = R"EPAPER(
<script>
// ================================================================
// CONSTANTS
// ================================================================
const EPD_W = 640, EPD_H = 400;

// Measured display colors — adjust to your panel's actual output
const PALETTE = [
  {name:'Black',  r:0,   g:0,   b:0,   code:0x0, hex:'#000000'},
  {name:'White',  r:255, g:255, b:255, code:0x1, hex:'#ffffff'},
  {name:'Green',  r:0,   g:155, b:72,  code:0x2, hex:'#009b48'},
  {name:'Blue',   r:0,   g:68,  b:143, code:0x3, hex:'#00448f'},
  {name:'Red',    r:228, g:18,  b:18,  code:0x4, hex:'#e41212'},
  {name:'Yellow', r:255, g:218, b:44,  code:0x5, hex:'#ffda2c'},
  {name:'Orange', r:224, g:101, b:4,   code:0x6, hex:'#e06504'},
];

// ================================================================
// COLOR SCIENCE
// ================================================================
function srgbLinear(c) {
  c /= 255;
  return c <= 0.04045 ? c / 12.92 : Math.pow((c + 0.055) / 1.055, 2.4);
}
function linearSrgb(c) {
  c = Math.max(0, Math.min(1, c));
  return Math.round((c <= 0.0031308 ? 12.92*c : 1.055*Math.pow(c,1/2.4)-0.055)*255);
}
function rgbToXyz(r,g,b) {
  const rl=srgbLinear(r), gl=srgbLinear(g), bl=srgbLinear(b);
  return [
    rl*0.4124564+gl*0.3575761+bl*0.1804375,
    rl*0.2126729+gl*0.7151522+bl*0.0721750,
    rl*0.0193339+gl*0.1191920+bl*0.9503041
  ];
}
function xyzToLab(x,y,z) {
  const f=t=>t>0.008856?Math.cbrt(t):7.787*t+16/116;
  return [116*f(y/1.00000)-16, 500*(f(x/0.95047)-f(y/1.00000)), 200*(f(y/1.00000)-f(z/1.08883))];
}
function rgbToLab(r,g,b) { return xyzToLab(...rgbToXyz(r,g,b)); }

function labToXyz(L,a,b) {
  const fy=(L+16)/116, fx=a/500+fy, fz=fy-b/200;
  const u=t=>t>0.206897?t*t*t:(t-16/116)/7.787;
  return [u(fx)*0.95047, u(fy)*1.00000, u(fz)*1.08883];
}
function labToRgb(L,a,b) {
  const [x,y,z]=labToXyz(L,a,b);
  return [
    linearSrgb( x*3.2404542-y*1.5371385-z*0.4985314),
    linearSrgb(-x*0.9692660+y*1.8760108+z*0.0415560),
    linearSrgb( x*0.0556434-y*0.2040259+z*1.0572252)
  ];
}

function rgbToHsv(r,g,b) {
  r/=255; g/=255; b/=255;
  const mx=Math.max(r,g,b), mn=Math.min(r,g,b), d=mx-mn;
  let h=0;
  if(d){
    if(mx===r) h=(g-b)/d%6;
    else if(mx===g) h=(b-r)/d+2;
    else h=(r-g)/d+4;
    h=((h*60)+360)%360;
  }
  return [h, mx?d/mx:0, mx];
}
function rgbToHsl(r,g,b) {
  r/=255; g/=255; b/=255;
  const mx=Math.max(r,g,b), mn=Math.min(r,g,b), l=(mx+mn)/2, d=mx-mn;
  if(!d) return [0,0,l];
  const s=d/(l>0.5?2-mx-mn:mx+mn);
  let h;
  if(mx===r) h=(g-b)/d+(g<b?6:0);
  else if(mx===g) h=(b-r)/d+2;
  else h=(r-g)/d+4;
  return [h*60, s, l];
}
function hslToRgb(h,s,l) {
  const q=l<.5?l*(1+s):l+s-l*s, p=2*l-q;
  const hk=h=>{const t=((h%1)+1)%1; return t<1/6?p+(q-p)*6*t:t<1/2?q:t<2/3?p+(q-p)*(2/3-t)*6:p;};
  return [Math.round(hk((h/360+1/3))*255), Math.round(hk(h/360)*255), Math.round(hk((h/360-1/3))*255)];
}

// CIEDE2000
function ciede2000(L1,a1,b1, L2,a2,b2) {
  const D2R=Math.PI/180;
  const C1=Math.hypot(a1,b1), C2=Math.hypot(a2,b2);
  const Cb=(C1+C2)/2, C7=Cb**7, P7=25**7;
  const G=0.5*(1-Math.sqrt(C7/(C7+P7)));
  const a1p=a1*(1+G), a2p=a2*(1+G);
  const C1p=Math.hypot(a1p,b1), C2p=Math.hypot(a2p,b2);
  let h1p=Math.atan2(b1,a1p)*180/Math.PI; if(h1p<0)h1p+=360;
  let h2p=Math.atan2(b2,a2p)*180/Math.PI; if(h2p<0)h2p+=360;
  const dLp=L2-L1, dCp=C2p-C1p;
  let dhp=0;
  if(C1p*C2p!==0){
    const dh=h2p-h1p;
    dhp=Math.abs(dh)<=180?dh:dh>180?dh-360:dh+360;
  }
  const dHp=2*Math.sqrt(C1p*C2p)*Math.sin(dhp*D2R/2);
  const Lb=(L1+L2)/2, Cbp=(C1p+C2p)/2;
  let Hbp=(C1p*C2p===0)?h1p+h2p:Math.abs(h1p-h2p)<=180?(h1p+h2p)/2:h1p+h2p<360?(h1p+h2p+360)/2:(h1p+h2p-360)/2;
  const T=1-0.17*Math.cos((Hbp-30)*D2R)+0.24*Math.cos(2*Hbp*D2R)+0.32*Math.cos((3*Hbp+6)*D2R)-0.20*Math.cos((4*Hbp-63)*D2R);
  const SL=1+0.015*(Lb-50)**2/Math.sqrt(20+(Lb-50)**2);
  const SC=1+0.045*Cbp, SH=1+0.015*Cbp*T;
  const Cbp7=Cbp**7, RC=2*Math.sqrt(Cbp7/(Cbp7+P7));
  const dTh=30*Math.exp(-((Hbp-275)/25)**2);
  const RT=-Math.sin(2*dTh*D2R)*RC;
  return Math.sqrt((dLp/SL)**2+(dCp/SC)**2+(dHp/SH)**2+RT*(dCp/SC)*(dHp/SH));
}

// Precompute palette LAB
const PAL_LAB = PALETTE.map(p=>rgbToLab(p.r,p.g,p.b));

function nearestColor(r,g,b, protectSkin,protectSky,protectVeg,protectSunset) {
  const lab=rgbToLab(r,g,b);
  const [h,s,v]=rgbToHsv(r,g,b);
  let best=0, bestD=Infinity;
  for(let i=0;i<PALETTE.length;i++){
    let d=ciede2000(...lab,...PAL_LAB[i]);
    // Color protection: increase distance to wrong-category colors
    if(protectSkin && isSkinPixel(r,g,b,h,s,v)){
      // Skin should go to warm colors; penalize blue/green
      if(i===2||i===3) d*=2.5;  // green, blue
    }
    if(protectSky && isSkyPixel(r,g,b,h,s,v)){
      // Sky → blue; penalize warm colors
      if(i===4||i===5||i===6) d*=2.0;
    }
    if(protectVeg && isVegPixel(r,g,b,h,s,v)){
      // Vegetation → green
      if(i===0||i===3||i===4) d*=1.8;
    }
    if(protectSunset && isSunsetPixel(r,g,b,h,s,v)){
      // Sunset → orange/red/yellow
      if(i===2||i===3) d*=2.0;
    }
    if(d<bestD){bestD=d;best=i;}
  }
  return best;
}

// ================================================================
// SCENE DETECTION
// ================================================================
function isSkinPixel(r,g,b,h,s,v) {
  return h>=0&&h<=50&&s>0.15&&s<0.90&&v>0.20&&v<0.97&&r>60&&r>g&&r>b;
}
function isSkyPixel(r,g,b,h,s,v) {
  return h>=185&&h<=265&&s>0.15&&v>0.35&&b>80&&b>r;
}
function isVegPixel(r,g,b,h,s,v) {
  return h>=75&&h<=165&&s>0.25&&v>0.10&&g>50&&g>r*0.8;
}
function isSunsetPixel(r,g,b,h,s,v) {
  return h>=5&&h<=55&&s>0.35&&v>0.4&&r>120;
}

function analyzeScene(data) {
  const n=data.length/4;
  let skin=0,sky=0,veg=0,sunset=0,dark=0,light=0,totalS=0,totalV=0,totalR=0,totalG=0,totalB=0;
  const step=Math.max(1,Math.floor(n/4000));
  let cnt=0;
  for(let i=0;i<n;i+=step){
    const r=data[i*4],g=data[i*4+1],b=data[i*4+2];
    const [h,s,v]=rgbToHsv(r,g,b);
    totalS+=s; totalV+=v; totalR+=r; totalG+=g; totalB+=b;
    if(isSkinPixel(r,g,b,h,s,v)) skin++;
    if(isSkyPixel(r,g,b,h,s,v)) sky++;
    if(isVegPixel(r,g,b,h,s,v)) veg++;
    if(isSunsetPixel(r,g,b,h,s,v)) sunset++;
    if(v<0.25) dark++;
    if(v>0.80) light++;
    cnt++;
  }
  const t=cnt||1;
  return {
    skinRatio:skin/t, skyRatio:sky/t, vegRatio:veg/t, sunsetRatio:sunset/t,
    avgSat:totalS/t, avgVal:totalV/t,
    avgR:totalR/t, avgG:totalG/t, avgB:totalB/t,
    darkRatio:dark/t, lightRatio:light/t,
    dominant: skin/t>0.25?'portrait': sky/t>0.20?'sky': veg/t>0.25?'nature': sunset/t>0.12?'sunset': 'general'
  };
}

function autoParams(scene) {
  const p={brightness:0,contrast:0,gamma:1.0,saturation:0,vibrance:0,clahe:50,claheR:8,claheS:3.0,
           shadows:0,highlights:0,temp:0,tint:0,dither:'auto'};
  if(scene.avgVal<0.35){p.brightness=20;p.gamma=0.80;p.shadows=25;}
  else if(scene.avgVal>0.70){p.highlights=-15;p.gamma=1.05;}
  if(scene.darkRatio>0.4){p.clahe=75;p.claheS=4.0;p.shadows=30;}
  if(scene.lightRatio>0.5){p.clahe=30;p.highlights=-10;}
  switch(scene.dominant){
    case 'portrait':
      p.saturation=5; p.vibrance=10; p.gamma=0.95; p.clahe=35; p.contrast=8; break;
    case 'sky':
      p.saturation=15; p.contrast=10; p.clahe=45; p.gamma=1.05; p.temp=-10; break;
    case 'nature':
      p.saturation=20; p.vibrance=15; p.contrast=12; p.clahe=55; p.gamma=0.98; break;
    case 'sunset':
      p.saturation=25; p.contrast=15; p.temp=20; p.gamma=0.92; p.clahe=50; break;
    default:
      p.saturation=10; p.contrast=5; p.clahe=50; break;
  }
  if(scene.avgSat<0.15){p.saturation+=20;p.vibrance+=15;}
  return p;
}

// ================================================================
// IMAGE ADJUSTMENTS
// ================================================================
function applyBrightnessContrast(data, brightness, contrast) {
  const B=brightness/100*255, F=(259*(contrast+255))/(255*(259-contrast));
  for(let i=0;i<data.length;i+=4){
    for(let c=0;c<3;c++){
      let v=F*(data[i+c]-128)+128+B;
      data[i+c]=Math.max(0,Math.min(255,v));
    }
  }
}

function applyGamma(data, gamma) {
  if(Math.abs(gamma-1.0)<0.005) return;
  const lut=new Uint8ClampedArray(256);
  for(let i=0;i<256;i++) lut[i]=Math.round(Math.pow(i/255,1/gamma)*255);
  for(let i=0;i<data.length;i+=4){
    data[i]=lut[data[i]]; data[i+1]=lut[data[i+1]]; data[i+2]=lut[data[i+2]];
  }
}

function applyExposure(data, ev) {
  const m=Math.pow(2, ev/100);
  for(let i=0;i<data.length;i+=4){
    data[i]=Math.min(255,data[i]*m);
    data[i+1]=Math.min(255,data[i+1]*m);
    data[i+2]=Math.min(255,data[i+2]*m);
  }
}

function applyHighlightsShadows(data, highlights, shadows) {
  const hl=highlights/100, sh=shadows/100;
  for(let i=0;i<data.length;i+=4){
    for(let c=0;c<3;c++){
      let v=data[i+c]/255;
      // highlights: affect bright areas
      if(v>0.5) v += hl*(v-0.5)*2*(1-v)*2;
      // shadows: affect dark areas
      if(v<0.5) v += sh*(0.5-v)*2*v*2;
      data[i+c]=Math.max(0,Math.min(255,v*255));
    }
  }
}

function applyLevels(data, blackPt, whitePt) {
  if(blackPt===0 && whitePt===255) return;
  const range=whitePt-blackPt||1;
  for(let i=0;i<data.length;i+=4){
    for(let c=0;c<3;c++){
      data[i+c]=Math.max(0,Math.min(255,((data[i+c]-blackPt)/range)*255));
    }
  }
}

function applyTemperature(data, temp, tint) {
  const T=temp/100*30, Ti=tint/100*15;
  for(let i=0;i<data.length;i+=4){
    data[i]=Math.max(0,Math.min(255,data[i]+T));
    data[i+1]=Math.max(0,Math.min(255,data[i+1]+Ti));
    data[i+2]=Math.max(0,Math.min(255,data[i+2]-T));
  }
}

function applySaturation(data, saturation) {
  if(saturation===0) return;
  const s=1+saturation/100;
  for(let i=0;i<data.length;i+=4){
    const r=data[i]/255, g=data[i+1]/255, b=data[i+2]/255;
    const L=0.2126*r+0.7152*g+0.0722*b;
    data[i]=Math.max(0,Math.min(255,(L+(r-L)*s)*255));
    data[i+1]=Math.max(0,Math.min(255,(L+(g-L)*s)*255));
    data[i+2]=Math.max(0,Math.min(255,(L+(b-L)*s)*255));
  }
}

function applyVibrance(data, vibrance) {
  if(vibrance===0) return;
  const v=vibrance/100;
  for(let i=0;i<data.length;i+=4){
    const r=data[i]/255, g=data[i+1]/255, b=data[i+2]/255;
    const mx=Math.max(r,g,b), mn=Math.min(r,g,b), sat=mx?(mx-mn)/mx:0;
    const boost=v*(1-sat)*2;  // boost desaturated colors more
    const L=0.2126*r+0.7152*g+0.0722*b;
    const s2=1+boost;
    data[i]=Math.max(0,Math.min(255,(L+(r-L)*s2)*255));
    data[i+1]=Math.max(0,Math.min(255,(L+(g-L)*s2)*255));
    data[i+2]=Math.max(0,Math.min(255,(L+(b-L)*s2)*255));
  }
}

function applySharpen(data, W, H, amount) {
  if(amount<=0) return;
  const k=amount/100*1.5;
  const src=new Uint8ClampedArray(data);
  const kern=[-k/8,-k/8,-k/8,-k/8,1+k,-k/8,-k/8,-k/8,-k/8];
  for(let y=1;y<H-1;y++) for(let x=1;x<W-1;x++){
    for(let c=0;c<3;c++){
      let v=0;
      for(let ky=-1;ky<=1;ky++) for(let kx=-1;kx<=1;kx++)
        v+=src[((y+ky)*W+(x+kx))*4+c]*kern[(ky+1)*3+(kx+1)];
      data[(y*W+x)*4+c]=Math.max(0,Math.min(255,v));
    }
  }
}

function applyNoiseReduction(data, W, H, amount) {
  if(amount<=0) return;
  const sigma=amount/100*2+0.5;
  const r=Math.ceil(sigma*2), sz=(2*r+1)**2;
  const src=new Uint8ClampedArray(data);
  for(let y=r;y<H-r;y++) for(let x=r;x<W-r;x++){
    for(let c=0;c<3;c++){
      let s=0;
      for(let ky=-r;ky<=r;ky++) for(let kx=-r;kx<=r;kx++){
        const g=Math.exp(-(kx*kx+ky*ky)/(2*sigma*sigma));
        s+=src[((y+ky)*W+(x+kx))*4+c]*g;
      }
      // simplified: just box blur approximation
    }
    // Use faster box blur
    for(let c=0;c<3;c++){
      let s=0;
      for(let ky=-1;ky<=1;ky++) for(let kx=-1;kx<=1;kx++)
        s+=src[((y+ky)*W+(x+kx))*4+c];
      const orig=src[(y*W+x)*4+c];
      data[(y*W+x)*4+c]=Math.round(orig*(1-amount/100)+s/9*(amount/100));
    }
  }
}

function applyEdgeEnhancement(data, W, H, amount) {
  if(amount<=0) return;
  const a=amount/100;
  const src=new Uint8ClampedArray(data);
  for(let y=1;y<H-1;y++) for(let x=1;x<W-1;x++){
    for(let c=0;c<3;c++){
      const gx=src[((y-1)*W+(x+1))*4+c]-src[((y-1)*W+(x-1))*4+c]
               +2*src[(y*W+(x+1))*4+c]-2*src[(y*W+(x-1))*4+c]
               +src[((y+1)*W+(x+1))*4+c]-src[((y+1)*W+(x-1))*4+c];
      const gy=src[((y+1)*W+(x-1))*4+c]+2*src[((y+1)*W+x)*4+c]+src[((y+1)*W+(x+1))*4+c]
               -src[((y-1)*W+(x-1))*4+c]-2*src[((y-1)*W+x)*4+c]-src[((y-1)*W+(x+1))*4+c];
      const edge=Math.sqrt(gx*gx+gy*gy)/4;
      data[(y*W+x)*4+c]=Math.max(0,Math.min(255,src[(y*W+x)*4+c]+a*edge*0.5));
    }
  }
}

// ================================================================
// CLAHE (Contrast Limited Adaptive Histogram Equalization)
// Applied to L* channel in LAB space
// ================================================================
function applyCLAHE(data, W, H, tileR, clipLimit) {
  if(clipLimit<=0.01) return;
  const tileW=Math.max(2,tileR*2), tileH=Math.max(2,tileR*2);
  const nX=Math.ceil(W/tileW), nY=Math.ceil(H/tileH);
  // Compute L channel only
  const Lch=new Float32Array(W*H);
  for(let i=0;i<W*H;i++){
    const r=data[i*4],g=data[i*4+1],b=data[i*4+2];
    const [L]=rgbToLab(r,g,b);
    Lch[i]=(L+16)/116*255; // rough mapping to 0-255
  }
  // Build per-tile histograms and CLUTs
  const clut=new Float32Array(nX*nY*256);
  for(let ty=0;ty<nY;ty++) for(let tx=0;tx<nX;tx++){
    const x0=tx*tileW, y0=ty*tileH;
    const x1=Math.min(x0+tileW,W), y1=Math.min(y0+tileH,H);
    const hist=new Uint32Array(256);
    let cnt=0;
    for(let y=y0;y<y1;y++) for(let x=x0;x<x1;x++){
      hist[Math.round(Math.min(255,Math.max(0,Lch[y*W+x])))]++;
      cnt++;
    }
    // Clip and redistribute
    const clip=Math.max(1,Math.round(clipLimit*cnt/256));
    let excess=0;
    for(let i=0;i<256;i++){if(hist[i]>clip){excess+=hist[i]-clip;hist[i]=clip;}}
    const add=Math.floor(excess/256);
    for(let i=0;i<256;i++) hist[i]+=add;
    // Build CDF
    const cdf=new Float32Array(256);
    cdf[0]=hist[0];
    for(let i=1;i<256;i++) cdf[i]=cdf[i-1]+hist[i];
    const cdfMin=cdf.find(v=>v>0)||0;
    const base=(ty*nX+tx)*256;
    for(let i=0;i<256;i++)
      clut[base+i]=(cnt>cdfMin)?(cdf[i]-cdfMin)/(cnt-cdfMin)*255:i;
  }
  // Bilinear interpolation
  for(let y=0;y<H;y++) for(let x=0;x<W;x++){
    const tx=(x/W)*(nX-1), ty2=(y/H)*(nY-1);
    const tx0=Math.floor(tx), ty0=Math.floor(ty2);
    const tx1=Math.min(tx0+1,nX-1), ty1=Math.min(ty0+1,nY-1);
    const fx=tx-tx0, fy=ty2-ty0;
    const Lv=Math.round(Math.min(255,Math.max(0,Lch[y*W+x])));
    const c00=clut[(ty0*nX+tx0)*256+Lv];
    const c10=clut[(ty0*nX+tx1)*256+Lv];
    const c01=clut[(ty1*nX+tx0)*256+Lv];
    const c11=clut[(ty1*nX+tx1)*256+Lv];
    const newL=(c00*(1-fx)*(1-fy)+c10*fx*(1-fy)+c01*(1-fx)*fy+c11*fx*fy);
    const shift=newL-Lch[y*W+x];
    const i=y*W+x;
    data[i*4]=Math.max(0,Math.min(255,data[i*4]+shift));
    data[i*4+1]=Math.max(0,Math.min(255,data[i*4+1]+shift));
    data[i*4+2]=Math.max(0,Math.min(255,data[i*4+2]+shift));
  }
}

// ================================================================
// DITHERING ALGORITHMS
// ================================================================
function ditherNone(indices, W, H) { return indices; }

const BAYER4=[
   0, 8, 2,10,
  12, 4,14, 6,
   3,11, 1, 9,
  15, 7,13, 5
];
const BAYER8=[
   0,32, 8,40, 2,34,10,42,
  48,16,56,24,50,18,58,26,
  12,44, 4,36,14,46, 6,38,
  60,28,52,20,62,30,54,22,
   3,35,11,43, 1,33, 9,41,
  51,19,59,27,49,17,57,25,
  15,47, 7,39,13,45, 5,37,
  63,31,55,23,61,29,53,21
];

function ditherOrdered(data, W, H, protSkin, protSky, protVeg, protSunset, bayer, sz) {
  const res=new Uint8Array(W*H);
  for(let y=0;y<H;y++) for(let x=0;x<W;x++){
    const i=(y*W+x)*4;
    const t=bayer[(y%sz)*sz+(x%sz)]/(sz*sz-1)-0.5;
    const r=Math.max(0,Math.min(255,data[i]+t*32));
    const g=Math.max(0,Math.min(255,data[i+1]+t*32));
    const b=Math.max(0,Math.min(255,data[i+2]+t*32));
    res[y*W+x]=nearestColor(r,g,b,protSkin,protSky,protVeg,protSunset);
  }
  return res;
}

function ditherError(data, W, H, protSkin, protSky, protVeg, protSunset, kernel) {
  const buf=new Float32Array(W*H*3);
  for(let i=0;i<W*H;i++){buf[i*3]=data[i*4];buf[i*3+1]=data[i*4+1];buf[i*3+2]=data[i*4+2];}
  const res=new Uint8Array(W*H);
  for(let y=0;y<H;y++) for(let x=0;x<W;x++){
    const i=(y*W+x);
    const r=Math.max(0,Math.min(255,buf[i*3]));
    const g=Math.max(0,Math.min(255,buf[i*3+1]));
    const b=Math.max(0,Math.min(255,buf[i*3+2]));
    const ci=nearestColor(r,g,b,protSkin,protSky,protVeg,protSunset);
    res[i]=ci;
    const er=r-PALETTE[ci].r, eg=g-PALETTE[ci].g, eb=b-PALETTE[ci].b;
    for(const[dx,dy,w] of kernel){
      const nx=x+dx, ny=y+dy;
      if(nx>=0&&nx<W&&ny>=0&&ny<H){
        const ni=(ny*W+nx)*3;
        buf[ni]+=er*w; buf[ni+1]+=eg*w; buf[ni+2]+=eb*w;
      }
    }
  }
  return res;
}

const KERN_FLOYD=[[1,0,7/16],[-1,1,3/16],[0,1,5/16],[1,1,1/16]];
const KERN_JARVIS=[[1,0,7/48],[2,0,5/48],[-2,1,3/48],[-1,1,5/48],[0,1,7/48],[1,1,5/48],[2,1,3/48],[-2,2,1/48],[-1,2,3/48],[0,2,5/48],[1,2,3/48],[2,2,1/48]];
const KERN_STUCKI=[[1,0,8/42],[2,0,4/42],[-2,1,2/42],[-1,1,4/42],[0,1,8/42],[1,1,4/42],[2,1,2/42],[-2,2,1/42],[-1,2,2/42],[0,2,4/42],[1,2,2/42],[2,2,1/42]];
const KERN_BURKES=[[1,0,8/32],[2,0,4/32],[-2,1,2/32],[-1,1,4/32],[0,1,8/32],[1,1,4/32],[2,1,2/32]];
const KERN_ATKINSON=[[1,0,1/8],[2,0,1/8],[-1,1,1/8],[0,1,1/8],[1,1,1/8],[0,2,1/8]];

function selectDitherAuto(scene) {
  switch(scene.dominant){
    case 'portrait': return 'atkinson';   // clean faces, minimal grain
    case 'sky':      return 'stucki';     // smooth gradients
    case 'nature':   return 'floyd';
    case 'sunset':   return 'jarvis';
    default:         return scene.avgSat>0.45?'floyd':'stucki';
  }
}

function applyDither(data, W, H, method, scene, protSkin, protSky, protVeg, protSunset) {
  let alg=method;
  if(alg==='auto') alg=selectDitherAuto(scene);
  switch(alg){
    case 'none':     return ditherNone(mapPaletteDirect(data,W,H,protSkin,protSky,protVeg,protSunset),W,H);
    case 'ordered':  return ditherOrdered(data,W,H,protSkin,protSky,protVeg,protSunset,BAYER4,4);
    case 'ordered8': return ditherOrdered(data,W,H,protSkin,protSky,protVeg,protSunset,BAYER8,8);
    case 'floyd':    return ditherError(data,W,H,protSkin,protSky,protVeg,protSunset,KERN_FLOYD);
    case 'jarvis':   return ditherError(data,W,H,protSkin,protSky,protVeg,protSunset,KERN_JARVIS);
    case 'stucki':   return ditherError(data,W,H,protSkin,protSky,protVeg,protSunset,KERN_STUCKI);
    case 'burkes':   return ditherError(data,W,H,protSkin,protSky,protVeg,protSunset,KERN_BURKES);
    case 'atkinson': return ditherError(data,W,H,protSkin,protSky,protVeg,protSunset,KERN_ATKINSON);
    default:         return ditherError(data,W,H,protSkin,protSky,protVeg,protSunset,KERN_FLOYD);
  }
}

function mapPaletteDirect(data,W,H,pS,pSk,pV,pSu){
  const res=new Uint8Array(W*H);
  for(let i=0;i<W*H;i++)
    res[i]=nearestColor(data[i*4],data[i*4+1],data[i*4+2],pS,pSk,pV,pSu);
  return res;
}

// ================================================================
// GENERATE FRAMEBUFFER (4 bits/pixel)
// ================================================================
function generateFramebuffer(indices, W, H) {
  const buf=new Uint8Array(W*H/2);
  for(let i=0;i<W*H;i+=2){
    buf[i/2]=(PALETTE[indices[i]].code<<4)|PALETTE[indices[i+1]].code;
  }
  return buf;
}

// ================================================================
// HISTOGRAM
// ================================================================
function drawHistogram(imageData) {
  const c=document.getElementById('histCanvas');
  const CW=c.offsetWidth||300, CH=80;
  c.width=CW; c.height=CH;
  const ctx=c.getContext('2d');
  ctx.clearRect(0,0,CW,CH);
  const data=imageData.data;
  const showL=document.getElementById('cHistBrightness').checked;
  const showRGB=document.getElementById('cHistRGB').checked;
  if(!showL&&!showRGB) return;
  const hL=new Uint32Array(256), hR=new Uint32Array(256), hG=new Uint32Array(256), hB=new Uint32Array(256);
  for(let i=0;i<data.length;i+=4){
    const l=Math.round(0.2126*data[i]+0.7152*data[i+1]+0.0722*data[i+2]);
    hL[l]++; hR[data[i]]++; hG[data[i+1]]++; hB[data[i+2]]++;
  }
  const mx=Math.max(showL?Math.max(...hL):0, showRGB?Math.max(...hR,...hG,...hB):0)||1;
  const draw=(hist,col)=>{
    ctx.fillStyle=col;
    for(let i=0;i<256;i++){
      const h=hist[i]/mx*(CH-2);
      ctx.fillRect(Math.floor(i*CW/256),CH-h,Math.ceil(CW/256)+1,h);
    }
  };
  if(showRGB){
    ctx.globalAlpha=0.5;
    draw(hR,'#e06060'); draw(hG,'#60c060'); draw(hB,'#6090e0');
    ctx.globalAlpha=1;
  }
  if(showL){ ctx.globalAlpha=0.8; draw(hL,'rgba(220,220,220,0.8)'); ctx.globalAlpha=1; }
}

function updatePalettePreview(counts, total) {
  const pp=document.getElementById('palPreview');
  const pl=document.getElementById('palLabels');
  pp.innerHTML=''; pl.innerHTML='';
  for(let i=0;i<PALETTE.length;i++){
    const pct=total?Math.round(counts[i]/total*100):0;
    const sw=document.createElement('div');
    sw.className='pal-swatch';
    sw.style.background=PALETTE[i].hex;
    sw.style.flex=String(Math.max(counts[i],1));
    sw.setAttribute('data-pct',pct+'%');
    if(PALETTE[i].name==='White'||PALETTE[i].name==='Yellow')
      sw.style.color='#333';
    pp.appendChild(sw);
    const lb=document.createElement('span');
    lb.textContent=PALETTE[i].name[0];
    lb.title=`${PALETTE[i].name}: ${pct}%`;
    pl.appendChild(lb);
  }
}

// ================================================================
// MAIN PROCESSING PIPELINE
// ================================================================
let srcCanvas=null, resultBuf=null, lastScene=null;

async function processImage() {
  if(!srcCanvas){setStatus('No image loaded');return;}
  document.getElementById('btnProcess').disabled=true;
  document.getElementById('btnSend').disabled=true;
  setStatus('Resizing...');
  await tick();

  // Step 1: Resize to EPD dimensions
  const offC=new OffscreenCanvas(EPD_W,EPD_H);
  const offCtx=offC.getContext('2d');
  offCtx.imageSmoothingEnabled=true;
  offCtx.imageSmoothingQuality='high';
  offCtx.drawImage(srcCanvas,0,0,EPD_W,EPD_H);
  const imgData=offCtx.getImageData(0,0,EPD_W,EPD_H);
  const data=imgData.data;

  // Step 2: Scene analysis
  setStatus('Analyzing scene...');
  await tick();
  const scene=analyzeScene(data);
  lastScene=scene;
  updateSceneInfo(scene);

  // Step 3: Load or auto-compute parameters
  const autoMode=document.getElementById('cAutoParams').checked;
  let P={};
  if(autoMode){
    P=autoParams(scene);
    applyAutoToSliders(P);
  } else {
    P=readSliders();
  }

  const pSkin=document.getElementById('cSkin').checked;
  const pSky=document.getElementById('cSky').checked;
  const pVeg=document.getElementById('cVeg').checked;
  const pSunset=document.getElementById('cSunset').checked;

  // Step 4: Apply adjustments in sequence
  setStatus('Applying tone adjustments...'); await tick();
  applyExposure(data, P.exposure||0);
  applyHighlightsShadows(data, P.highlights||0, P.shadows||0);
  applyLevels(data, P.blackPt||0, P.whitePt||255);
  applyBrightnessContrast(data, P.brightness||0, P.contrast||0);
  applyTemperature(data, P.temp||0, P.tint||0);
  applySaturation(data, P.saturation||0);
  applyVibrance(data, P.vibrance||0);

  setStatus('Applying sharpening & noise reduction...'); await tick();
  if((P.noise||0)>0) applyNoiseReduction(data, EPD_W, EPD_H, P.noise||0);
  if((P.sharpen||0)>0) applySharpen(data, EPD_W, EPD_H, P.sharpen||0);
  if((P.edge||0)>0) applyEdgeEnhancement(data, EPD_W, EPD_H, P.edge||0);
  if((P.local||0)>0) applyLocalContrast(data, EPD_W, EPD_H, P.local||0);

  setStatus('Applying CLAHE...'); await tick();
  const claheAmount=P.clahe!=null?P.clahe:50;
  if(claheAmount>0){
    const clipStr=(P.claheS||3.0)*(claheAmount/100);
    applyCLAHE(data, EPD_W, EPD_H, P.claheR||8, clipStr);
  }

  setStatus('Applying gamma...'); await tick();
  applyGamma(data, P.gamma||1.0);

  // Step 5: Dithering + palette mapping (slow — most time here)
  setStatus('Mapping to palette with CIEDE2000...'); await tick();
  const ditherMethod=document.getElementById('selDither').value;
  const indices=applyDither(data, EPD_W, EPD_H, ditherMethod, scene, pSkin, pSky, pVeg, pSunset);

  // Step 6: Render preview
  setStatus('Rendering preview...'); await tick();
  const previewData=new Uint8ClampedArray(EPD_W*EPD_H*4);
  const palCounts=new Uint32Array(PALETTE.length);
  for(let i=0;i<EPD_W*EPD_H;i++){
    const ci=indices[i];
    palCounts[ci]++;
    previewData[i*4]=PALETTE[ci].r;
    previewData[i*4+1]=PALETTE[ci].g;
    previewData[i*4+2]=PALETTE[ci].b;
    previewData[i*4+3]=255;
  }
  const previewImgData=new ImageData(previewData,EPD_W,EPD_H);
  const pv=document.getElementById('previewCanvas');
  pv.width=EPD_W; pv.height=EPD_H;
  pv.getContext('2d').putImageData(previewImgData,0,0);

  // Step 7: Histogram
  drawHistogram(new ImageData(data,EPD_W,EPD_H));
  updatePalettePreview(palCounts, EPD_W*EPD_H);

  // Step 8: Generate framebuffer
  resultBuf=generateFramebuffer(indices, EPD_W, EPD_H);

  setStatus(`Done! ${EPD_W}×${EPD_H} — ${(resultBuf.length/1024).toFixed(1)}KB ready. Tap "Send to Display".`);
  document.getElementById('btnProcess').disabled=false;
  document.getElementById('btnSend').disabled=false;
}

function applyLocalContrast(data, W, H, amount) {
  if(amount<=0) return;
  const a=amount/200;
  const blur=new Uint8ClampedArray(data.length);
  const r=4;
  for(let y=0;y<H;y++) for(let x=0;x<W;x++){
    let sr=0,sg=0,sb=0,cnt=0;
    for(let dy=-r;dy<=r;dy++) for(let dx=-r;dx<=r;dx++){
      const ny=Math.min(H-1,Math.max(0,y+dy)), nx=Math.min(W-1,Math.max(0,x+dx));
      const i=(ny*W+nx)*4; sr+=data[i];sg+=data[i+1];sb+=data[i+2];cnt++;
    }
    const i=(y*W+x)*4;
    blur[i]=sr/cnt; blur[i+1]=sg/cnt; blur[i+2]=sb/cnt;
  }
  for(let i=0;i<data.length;i+=4){
    for(let c=0;c<3;c++)
      data[i+c]=Math.max(0,Math.min(255,data[i+c]+(data[i+c]-blur[i+c])*a));
  }
}

// ================================================================
// SEND TO DISPLAY (chunked binary upload)
// ================================================================
async function sendToDisplay() {
  if(!resultBuf){setStatus('Process an image first.');return;}
  document.getElementById('btnSend').disabled=true;
  const CHUNK=8192;
  const total=resultBuf.length;
  const chunks=Math.ceil(total/CHUNK);
  showProgress(true);
  for(let c=0;c<chunks;c++){
    const offset=c*CHUNK;
    const slice=resultBuf.slice(offset,Math.min(offset+CHUNK,total));
    setStatus(`Uploading ${c+1}/${chunks}...`);
    setProgress((c/chunks)*90);
    try{
      const r=await fetch('/chunk?o='+offset,{method:'POST',headers:{'Content-Type':'application/octet-stream'},body:slice});
      if(!r.ok) throw new Error('HTTP '+r.status);
    }catch(e){setStatus('Upload error: '+e.message);showProgress(false);document.getElementById('btnSend').disabled=false;return;}
  }
  setProgress(95);
  setStatus('Sending display command...');
  await fetch('/show',{method:'POST'});
  setProgress(100);
  setStatus('Sent! Display is refreshing (~15-30 seconds)...');
  showProgress(false);
  document.getElementById('btnSend').disabled=false;
}

// ================================================================
// SLIDER HELPERS
// ================================================================
function readSliders(){
  return {
    brightness:+document.getElementById('sBrightness').value,
    contrast:+document.getElementById('sContrast').value,
    gamma:+document.getElementById('sGamma').value/100,
    exposure:+document.getElementById('sExposure').value/100,
    highlights:+document.getElementById('sHighlights').value,
    shadows:+document.getElementById('sShadows').value,
    whitePt:+document.getElementById('sWhite').value,
    blackPt:+document.getElementById('sBlack').value,
    temp:+document.getElementById('sTemp').value,
    tint:+document.getElementById('sTint').value,
    saturation:+document.getElementById('sSaturation').value,
    vibrance:+document.getElementById('sVibrance').value,
    sharpen:+document.getElementById('sSharpen').value,
    noise:+document.getElementById('sNoise').value,
    clahe:+document.getElementById('sClahe').value,
    claheR:+document.getElementById('sClaheR').value,
    claheS:+document.getElementById('sClaheS').value/10,
    edge:+document.getElementById('sEdge').value,
    local:+document.getElementById('sLocal').value,
  };
}

function applyAutoToSliders(P){
  const set=(id,v)=>{const el=document.getElementById(id);if(el)el.value=String(v);};
  set('sBrightness',P.brightness||0);
  set('sContrast',P.contrast||0);
  set('sGamma',Math.round((P.gamma||1.0)*100));
  set('sExposure',Math.round((P.exposure||0)*100));
  set('sHighlights',P.highlights||0);
  set('sShadows',P.shadows||0);
  set('sWhite',P.whitePt||255);
  set('sBlack',P.blackPt||0);
  set('sTemp',P.temp||0);
  set('sTint',P.tint||0);
  set('sSaturation',P.saturation||0);
  set('sVibrance',P.vibrance||0);
  set('sClahe',P.clahe||50);
  set('sClaheR',P.claheR||8);
  set('sClaheS',Math.round((P.claheS||3.0)*10));
  updateSliderLabels();
}

function updateSliderLabels(){
  const sliders=[
    ['sBrightness','vBrightness',v=>v],
    ['sContrast','vContrast',v=>v],
    ['sGamma','vGamma',v=>(v/100).toFixed(2)],
    ['sExposure','vExposure',v=>(v/100).toFixed(2)+' EV'],
    ['sHighlights','vHighlights',v=>v],
    ['sShadows','vShadows',v=>v],
    ['sWhite','vWhite',v=>v],
    ['sBlack','vBlack',v=>v],
    ['sTemp','vTemp',v=>v],
    ['sTint','vTint',v=>v],
    ['sSaturation','vSaturation',v=>v],
    ['sVibrance','vVibrance',v=>v],
    ['sSharpen','vSharpen',v=>v],
    ['sNoise','vNoise',v=>v],
    ['sClahe','vClahe',v=>v],
    ['sClaheR','vClaheR',v=>v],
    ['sClaheS','vClaheS',v=>(v/10).toFixed(1)],
    ['sEdge','vEdge',v=>v],
    ['sLocal','vLocal',v=>v],
  ];
  for(const[sid,vid,fmt] of sliders){
    const s=document.getElementById(sid), v=document.getElementById(vid);
    if(s&&v) v.textContent=fmt(s.value);
  }
}

// ================================================================
// UI HELPERS
// ================================================================
function setStatus(msg){
  document.getElementById('statusBar').textContent=msg;
  console.log('[STATUS]',msg);
}
function showProgress(show){
  document.getElementById('progWrap').style.display=show?'block':'none';
}
function setProgress(pct){
  document.getElementById('progBar').style.width=pct+'%';
}
function tick(){return new Promise(r=>setTimeout(r,0));}

function updateSceneInfo(scene){
  const el=document.getElementById('sceneInfo');
  const icons={portrait:'👤',sky:'🌤',nature:'🌿',sunset:'🌅',general:'🖼'};
  const parts=[`<span class="scene-badge">${icons[scene.dominant]||'🖼'} ${scene.dominant}</span>`];
  if(scene.skinRatio>0.1) parts.push(`<span class="scene-badge">Skin ${Math.round(scene.skinRatio*100)}%</span>`);
  if(scene.skyRatio>0.1)  parts.push(`<span class="scene-badge">Sky ${Math.round(scene.skyRatio*100)}%</span>`);
  if(scene.vegRatio>0.1)  parts.push(`<span class="scene-badge">Vegetation ${Math.round(scene.vegRatio*100)}%</span>`);
  if(scene.sunsetRatio>0.05) parts.push(`<span class="scene-badge">Sunset ${Math.round(scene.sunsetRatio*100)}%</span>`);
  el.innerHTML=parts.join(' ');
}

// ================================================================
// EVENT LISTENERS
// ================================================================
async function loadFile(file){
  if(!file||!file.type.startsWith('image/')) return;
  const bmp=await createImageBitmap(file);
  srcCanvas=new OffscreenCanvas(bmp.width,bmp.height);
  srcCanvas.getContext('2d').drawImage(bmp,0,0);
  // Quick preview
  const pv=document.getElementById('previewCanvas');
  pv.width=EPD_W; pv.height=EPD_H;
  const pvCtx=pv.getContext('2d');
  pvCtx.imageSmoothingEnabled=true;
  pvCtx.imageSmoothingQuality='high';
  pvCtx.drawImage(bmp,0,0,EPD_W,EPD_H);
  setStatus(`Image loaded: ${bmp.width}×${bmp.height} → will resize to ${EPD_W}×${EPD_H}`);
  document.getElementById('btnProcess').disabled=false;
  resultBuf=null;
  document.getElementById('btnSend').disabled=true;
}

document.getElementById('fileIn').addEventListener('change',e=>{loadFile(e.target.files[0]);});
document.getElementById('btnProcess').addEventListener('click',processImage);
document.getElementById('btnSend').addEventListener('click',sendToDisplay);
document.getElementById('btnClear').addEventListener('click',()=>{
  fetch('/clear',{method:'POST'});
  setStatus('Clear command sent — display refreshing...');
});

// Drag and drop
const dz=document.getElementById('dropZone');
dz.addEventListener('dragover',e=>{e.preventDefault();dz.classList.add('drag');});
dz.addEventListener('dragleave',()=>dz.classList.remove('drag'));
dz.addEventListener('drop',e=>{e.preventDefault();dz.classList.remove('drag');loadFile(e.dataTransfer.files[0]);});

// Slider live labels
document.querySelectorAll('input[type=range]').forEach(s=>{
  s.addEventListener('input',updateSliderLabels);
});
updateSliderLabels();

// Auto-mode toggle visual feedback
document.getElementById('cAutoParams').addEventListener('change',e=>{
  document.getElementById('autoModeBadge').textContent=e.target.checked?'AUTO ON':'MANUAL';
});

// Histogram checkbox change
document.getElementById('cHistBrightness').addEventListener('change',()=>{if(lastScene){}});
document.getElementById('cHistRGB').addEventListener('change',()=>{});

// Poll status from device
async function pollStatus(){
  try{
    const r=await fetch('/status');
    const t=await r.text();
    document.getElementById('connStatus').textContent='● online';
    document.getElementById('connStatus').style.color='#6c9b6c';
  }catch(e){
    document.getElementById('connStatus').textContent='● offline';
    document.getElementById('connStatus').style.color='#9b6c6c';
  }
}
pollStatus();
setInterval(pollStatus,5000);

// Initialize palette preview empty state
updatePalettePreview(new Uint32Array(7).fill(1), 7);

</script>
</body>
</html>
)EPAPER";

// ================================================================
// WEB SERVER SETUP
// ================================================================
void setup_server() {

  // Serve the main page (chunked)
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* req){
    AsyncResponseStream* res = req->beginResponseStream("text/html");
    res->print(FPSTR(HTML_HEAD));
    res->print(FPSTR(HTML_BODY));
    res->print(FPSTR(HTML_JS));
    req->send(res);
  });

  // Receive a chunk of framebuffer binary data
  // URL: POST /chunk?o=<byte_offset>
  server.on("/chunk", HTTP_POST,
    [](AsyncWebServerRequest* req){ req->send(200, "text/plain", "ok"); },
    nullptr,
    [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total){
      int offset = 0;
      if(req->hasParam("o")) offset = req->getParam("o")->value().toInt();
      size_t dest = (size_t)offset + index;
      if(dest + len <= BUF_SZ){
        memcpy(epd_buf + dest, data, len);
      }
    }
  );

  // Trigger display refresh
  server.on("/show", HTTP_POST, [](AsyncWebServerRequest* req){
    g_show = true;
    req->send(200, "text/plain", "ok");
  });

  // Clear display to white
  server.on("/clear", HTTP_POST, [](AsyncWebServerRequest* req){
    g_clear = true;
    req->send(200, "text/plain", "ok");
  });

  // Status endpoint
  server.on("/status", HTTP_GET, [](AsyncWebServerRequest* req){
    req->send(200, "text/plain", g_status);
  });

  server.onNotFound([](AsyncWebServerRequest* req){
    req->send(404, "text/plain", "not found");
  });
}
