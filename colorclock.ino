/**
 * Elecrow CrowPanel 7.0" ESP32 HMI (DIS08070H) — Color Clock with Web Configuration
 *
 * Background color = RGB where:
 *   R = hours   (0-23)  mapped → 0-255
 *   G = minutes (0-59)  mapped → 0-255
 *   B = seconds (0-59)  mapped → 0-255
 *
 * Text color auto-selected via WCAG contrast ratio.
 * Double-buffered PSRAM framebuffers, swapped at frame end, for tear-free updates.
 *
 * Configurable via web UI at http://colorclock.local (or device IP):
 *   - NTP server
 *   - Time zone (POSIX TZ string, presets + custom)
 *   - 12h / 24h format
 *   - Show / hide seconds
 *   - Show / hide date
 *   - Color gamma (this panel renders mid-tones paler than the T-Display's IPS)
 *
 * WiFi setup via tzapu WiFiManager captive portal.
 * Touch & hold the screen during the boot splash to reset WiFi credentials.
 * (GPIO 0 / BOOT is the display pixel clock on this board, so it can't be used.)
 *
 * Libraries (install via Library Manager):
 *   - LovyanGFX   by lovyan03
 *   - WiFiManager by tzapu
 *   (Preferences, WebServer, ESPmDNS, Wire are built-in to the ESP32 core.)
 *
 * Board: Elecrow CrowPanel 7.0P — PSRAM: Enabled (required for the framebuffers)
 * Display / touch pin config lives in crowpanel7.h.
 */

#include <WiFi.h>
#include <WiFiManager.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <time.h>
#include <Wire.h>
#include "crowpanel7.h"

// ── Defaults ─────────────────────────────────────────────────────────────────
const char* DEFAULT_NTP        = "pool.ntp.org";
const char* DEFAULT_TZ         = "MST7MDT,M3.2.0,M11.1.0";   // Mountain Time w/ DST
const char* DEFAULT_TZ_LABEL   = "Mountain (MST/MDT)";
const char* AP_NAME            = "ColorClock-Setup";
const char* AP_PASSWORD        = "sopretty";      // min 8 chars for WPA2
const char* HOSTNAME           = "colorclock";    // DHCP hostname
const char* MDNS_NAME          = "colorclock";    // → http://colorclock.local
const float DEFAULT_GAMMA      = 1.8f;
const float MIN_GAMMA          = 1.0f;
const float MAX_GAMMA          = 3.0f;
// ─────────────────────────────────────────────────────────────────────────────

// V3.0 boards gate the GT911 reset/INT lines through a PCA9557 I/O expander.
// On V2.x boards there's no device at this address and the writes are ignored.
#define PCA9557_ADDR     0x18
#define PCA9557_OUTPUT   0x01
#define PCA9557_CONFIG   0x03   // bit = 1 → input
#define BOARD_IO38       38     // driven LOW by Elecrow's V3.0 reference code

#define WIFI_RESET_WINDOW_MS 3000   // how long the boot splash waits for a touch
#define WIFI_RESET_HOLD_MS   2000   // how long the touch must be held

CrowPanel7  display;
LGFX_Sprite spr;      // always bound to the display's off-screen framebuffer

void bindSprite() {
  spr.setBuffer(display.backBuffer(), SCREEN_W, SCREEN_H, 16);
}

// Put the finished frame on screen and point the sprite at the next back buffer.
void presentFrame() {
  display.present();
  bindSprite();
}
WebServer   server(80);
Preferences prefs;

// ── Settings (persisted in NVS) ──────────────────────────────────────────────
struct Settings {
  String ntp_server;
  String tz_string;
  bool   format_24h;
  bool   show_seconds;
  bool   show_date;
  float  gamma;          // applied to the background color; 1.0 = off
} settings;

void loadSettings() {
  prefs.begin("clock", true);
  settings.ntp_server   = prefs.getString("ntp",   DEFAULT_NTP);
  settings.tz_string    = prefs.getString("tz",    DEFAULT_TZ);
  settings.format_24h   = prefs.getBool  ("fmt24", true);
  settings.show_seconds = prefs.getBool  ("secs",  true);
  settings.show_date    = prefs.getBool  ("date",  true);
  settings.gamma        = prefs.getFloat ("gamma", DEFAULT_GAMMA);
  prefs.end();
}

void saveSettings() {
  prefs.begin("clock", false);
  prefs.putString("ntp",   settings.ntp_server);
  prefs.putString("tz",    settings.tz_string);
  prefs.putBool  ("fmt24", settings.format_24h);
  prefs.putBool  ("secs",  settings.show_seconds);
  prefs.putBool  ("date",  settings.show_date);
  prefs.putFloat ("gamma", settings.gamma);
  prefs.end();
}

void applyTimeSettings() {
  // ESP32 built-in SNTP — handles DST via POSIX TZ string
  configTzTime(settings.tz_string.c_str(), settings.ntp_server.c_str());
}

// ── Color mapping ─────────────────────────────────────────────────────────────
inline uint8_t mapH(int h) { return (uint8_t)((h * 255) / 23); }
inline uint8_t mapM(int m) { return (uint8_t)((m * 255) / 59); }
inline uint8_t mapS(int s) { return (uint8_t)((s * 255) / 59); }

// Background gamma correction: the CrowPanel's panel shows mid-tones much
// lighter than the T-Display's IPS, so darken them before display. The hex
// and R/G/B readouts still show the uncorrected time-mapped values.
uint8_t gammaLut[256];

void buildGammaLut() {
  for (int i = 0; i < 256; i++) {
    gammaLut[i] = (uint8_t)lroundf(powf(i / 255.0f, settings.gamma) * 255.0f);
  }
}

uint16_t rgb888to565(uint8_t r, uint8_t g, uint8_t b) {
  return ((uint16_t)(r & 0xF8) << 8) |
         ((uint16_t)(g & 0xFC) << 3) |
         ((uint16_t)(b >> 3));
}

// ── WCAG contrast / text color selection ─────────────────────────────────────
static uint16_t linearise(uint8_t c) {
  float cf  = c / 255.0f;
  float lin = (cf <= 0.04045f) ? (cf / 12.92f)
                                : powf((cf + 0.055f) / 1.055f, 2.4f);
  return (uint16_t)(lin * 1000.0f);
}

static uint32_t luminance1000(uint8_t r, uint8_t g, uint8_t b) {
  return (212UL * linearise(r) +
          715UL * linearise(g) +
           72UL * linearise(b)) / 1000UL;
}

static uint32_t contrastRatio100(uint32_t L1, uint32_t L2) {
  uint32_t hi = max(L1, L2) + 50;
  uint32_t lo = min(L1, L2) + 50;
  return (hi * 100UL) / lo;
}

uint16_t bestTextColor(uint8_t r, uint8_t g, uint8_t b) {
  uint32_t bgLum = luminance1000(r, g, b);
  struct Candidate { uint8_t r, g, b; uint16_t col565; };
  Candidate candidates[] = {
    {255, 255, 255, TFT_WHITE},
    {  0,   0,   0, TFT_BLACK},
    {180, 180, 180, rgb888to565(180, 180, 180)},
    { 80,  80,  80, rgb888to565( 80,  80,  80)},
  };

  uint32_t bestCR  = 0;
  uint16_t bestCol = TFT_WHITE;
  for (auto& c : candidates) {
    uint32_t lum = luminance1000(c.r, c.g, c.b);
    uint32_t cr  = contrastRatio100(bgLum, lum);
    if (cr > bestCR) { bestCR = cr; bestCol = c.col565; }
  }
  return bestCol;
}

// ── Calendar names (tm_wday: 0=Sun; tm_mon: 0=Jan) ──────────────────────────
const char* dayNames[]   = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
const char* monthNames[] = {"Jan","Feb","Mar","Apr","May","Jun",
                              "Jul","Aug","Sep","Oct","Nov","Dec"};

String pad2(int n) { return (n < 10 ? "0" : "") + String(n); }

// ── Format time string per settings ─────────────────────────────────────────
String formatTime(const struct tm& ti) {
  String out;
  if (settings.format_24h) {
    out = pad2(ti.tm_hour) + ":" + pad2(ti.tm_min);
    if (settings.show_seconds) out += ":" + pad2(ti.tm_sec);
  } else {
    int h12 = ti.tm_hour % 12; if (h12 == 0) h12 = 12;
    out = String(h12) + ":" + pad2(ti.tm_min);
    if (settings.show_seconds) out += ":" + pad2(ti.tm_sec);
    out += (ti.tm_hour < 12 ? " AM" : " PM");
  }
  return out;
}

// ── Status / boot screens ───────────────────────────────────────────────────
void useFont(const lgfx::IFont* font, float scale = 1.0f) {
  spr.setFont(font);
  spr.setTextSize(scale);
}

void drawStatus(const String& l1, const String& l2, uint16_t color) {
  spr.fillSprite(TFT_BLACK);
  spr.setTextDatum(MC_DATUM);
  spr.setTextColor(color, TFT_BLACK);
  useFont(&fonts::DejaVu56);
  if (l2.length() == 0) {
    spr.drawString(l1, SCREEN_W/2, SCREEN_H/2);
  } else {
    spr.drawString(l1, SCREEN_W/2, SCREEN_H/2 - 40);
    useFont(&fonts::DejaVu40);
    spr.setTextColor(TFT_WHITE, TFT_BLACK);
    spr.drawString(l2, SCREEN_W/2, SCREEN_H/2 + 40);
  }
}

void showStatus(const String& l1, const String& l2 = "", uint16_t color = TFT_CYAN) {
  drawStatus(l1, l2, color);
  presentFrame();
}

void portalCallback(WiFiManager* wm) {
  spr.fillSprite(TFT_BLUE);
  spr.setTextDatum(MC_DATUM);
  spr.setTextColor(TFT_WHITE, TFT_BLUE);
  useFont(&fonts::DejaVu56);
  spr.drawString("WiFi Setup", SCREEN_W/2, 55);
  useFont(&fonts::DejaVu40);
  spr.drawString("Connect phone to:", SCREEN_W/2, 140);
  useFont(&fonts::DejaVu72);
  spr.setTextColor(TFT_YELLOW, TFT_BLUE);
  spr.drawString(AP_NAME, SCREEN_W/2, 215);
  useFont(&fonts::DejaVu40);
  spr.setTextColor(TFT_WHITE, TFT_BLUE);
  spr.drawString(String("Password: ") + AP_PASSWORD, SCREEN_W/2, 300);
  spr.drawString("Then open: 192.168.4.1", SCREEN_W/2, 365);
  useFont(&fonts::DejaVu24);
  spr.drawString("(captive portal will appear)", SCREEN_W/2, 430);
  presentFrame();
}

// ── Touch-hold WiFi reset at boot ───────────────────────────────────────────
// Shows a splash for WIFI_RESET_WINDOW_MS; returns true if the screen is held
// for WIFI_RESET_HOLD_MS, with a progress bar while it's held.
bool wifiResetRequested() {
  const int barX = 100, barY = SCREEN_H - 60, barW = SCREEN_W - 200, barH = 24;
  uint32_t start = millis(), heldSince = 0;
  while (true) {
    uint32_t now = millis();
    bool touched = display.touched();
    if (touched && heldSince == 0) heldSince = now;
    if (!touched) heldSince = 0;
    uint32_t held = touched ? now - heldSince : 0;

    drawStatus("Color Clock", "Touch & hold to reset WiFi", TFT_CYAN);
    if (touched) {
      spr.drawRect(barX - 2, barY - 2, barW + 4, barH + 4, TFT_WHITE);
      spr.fillRect(barX, barY, min<uint32_t>(held, WIFI_RESET_HOLD_MS) * barW / WIFI_RESET_HOLD_MS,
                   barH, TFT_RED);
    }
    presentFrame();   // paced by the panel's frame rate

    if (touched && held >= WIFI_RESET_HOLD_MS) return true;
    if (!touched && now - start >= WIFI_RESET_WINDOW_MS) return false;
  }
}

// ── Web UI ──────────────────────────────────────────────────────────────────
const char SETTINGS_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html><html><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Color Clock</title>
<style>
*{box-sizing:border-box}
body{font-family:-apple-system,BlinkMacSystemFont,sans-serif;background:#111;color:#eee;padding:20px;max-width:500px;margin:auto;line-height:1.4}
h1{color:#6cf;margin-top:0}
h2{color:#6cf;font-size:16px;margin:25px 0 10px}
label{display:block;margin-top:14px;font-weight:600;font-size:14px}
input[type=text],select{width:100%;padding:10px;margin-top:5px;background:#222;color:#fff;border:1px solid #444;border-radius:4px;font-size:14px}
input[type=checkbox]{margin-right:8px;transform:scale(1.3);vertical-align:middle}
.cb{display:flex;align-items:center;margin-top:14px;font-weight:400}
button{margin-top:20px;padding:12px;background:#6cf;color:#000;border:0;font-size:15px;border-radius:4px;cursor:pointer;width:100%;font-weight:600}
button:hover{background:#8df}
button.danger{background:#822;color:#fff}
button.danger:hover{background:#a33}
.status{background:#1a1a1a;padding:15px;border-radius:6px;margin-bottom:20px;font-size:13px;border:1px solid #333}
.status div{margin:3px 0;display:flex;justify-content:space-between}
.status span:first-child{color:#888}
.status span:last-child{color:#fff;font-family:monospace}
input[type=range]{width:100%;margin-top:8px}
.hint{color:#888;font-size:12px;margin-top:4px}
.live{color:#6cf;font-size:18px;font-family:monospace}
hr{border:0;border-top:1px solid #333;margin:25px 0}
</style></head><body>
<h1>Color Clock</h1>

<div class="status">
<div><span>Current</span><span class="live" id="time">--:--:--</span></div>
<div><span>Hostname</span><span>%MDNS%.local</span></div>
<div><span>IP</span><span>%IP%</span></div>
<div><span>WiFi</span><span>%RSSI% dBm</span></div>
<div><span>Uptime</span><span>%UPTIME%</span></div>
</div>

<form action="/save" method="POST">

<h2>Time Source</h2>
<label>NTP Server</label>
<input type="text" name="ntp" value="%NTP%">
<div class="hint">e.g. pool.ntp.org, time.google.com, time.cloudflare.com</div>

<label>Time Zone Preset</label>
<select onchange="if(this.value)document.querySelector('[name=tz]').value=this.value">
<option value="">— pick preset or use custom below —</option>
<option value="UTC0">UTC</option>
<option value="PST8PDT,M3.2.0,M11.1.0">US Pacific (PST/PDT)</option>
<option value="MST7MDT,M3.2.0,M11.1.0">US Mountain (MST/MDT)</option>
<option value="MST7">US Arizona (MST, no DST)</option>
<option value="CST6CDT,M3.2.0,M11.1.0">US Central (CST/CDT)</option>
<option value="EST5EDT,M3.2.0,M11.1.0">US Eastern (EST/EDT)</option>
<option value="HST10">Hawaii (HST)</option>
<option value="AKST9AKDT,M3.2.0,M11.1.0">Alaska (AKST/AKDT)</option>
<option value="GMT0BST,M3.5.0/1,M10.5.0">UK (GMT/BST)</option>
<option value="CET-1CEST,M3.5.0,M10.5.0/3">Central Europe (CET/CEST)</option>
<option value="EET-2EEST,M3.5.0/3,M10.5.0/4">Eastern Europe (EET/EEST)</option>
<option value="JST-9">Japan (JST)</option>
<option value="CST-8">China (CST)</option>
<option value="IST-5:30">India (IST)</option>
<option value="AEST-10AEDT,M10.1.0,M4.1.0/3">Sydney (AEST/AEDT)</option>
<option value="NZST-12NZDT,M9.5.0,M4.1.0/3">New Zealand (NZST/NZDT)</option>
</select>

<label>POSIX TZ String</label>
<input type="text" name="tz" value="%TZ%">
<div class="hint">Auto-filled from preset, or enter your own. Handles DST automatically.</div>

<h2>Display Format</h2>
<label class="cb"><input type="checkbox" name="fmt24" %FMT24%> 24-hour format (off = 12-hour AM/PM)</label>
<label class="cb"><input type="checkbox" name="secs"  %SECS%> Show seconds</label>
<label class="cb"><input type="checkbox" name="date"  %DATE%> Show date line</label>

<label>Color richness (gamma): <span id="gv">%GAMMA%</span></label>
<input type="range" name="gamma" min="1.0" max="3.0" step="0.1" value="%GAMMA%"
       oninput="document.getElementById('gv').textContent=this.value">
<div class="hint">Higher darkens mid-tones for deeper colors on this panel. 1.0 = no correction.</div>

<button type="submit">Save &amp; Apply</button>
</form>

<hr>

<form action="/reset_wifi" method="POST"
      onsubmit="return confirm('Wipe saved WiFi credentials and reboot?')">
<button type="submit" class="danger">Reset WiFi Credentials</button>
</form>

<script>
function tick(){
  fetch('/time').then(r=>r.text()).then(t=>{
    document.getElementById('time').textContent=t;
  }).catch(()=>{});
}
setInterval(tick,1000); tick();
</script>
</body></html>
)HTML";

// ── Web handlers ────────────────────────────────────────────────────────────
String uptimeString() {
  long s = millis() / 1000;
  char buf[32];
  snprintf(buf, sizeof(buf), "%ldd %02ld:%02ld:%02ld",
           s/86400, (s%86400)/3600, (s%3600)/60, s%60);
  return String(buf);
}

void handleRoot() {
  String html = FPSTR(SETTINGS_HTML);
  html.replace("%MDNS%",   MDNS_NAME);
  html.replace("%IP%",     WiFi.localIP().toString());
  html.replace("%RSSI%",   String(WiFi.RSSI()));
  html.replace("%UPTIME%", uptimeString());
  html.replace("%NTP%",    settings.ntp_server);
  html.replace("%TZ%",     settings.tz_string);
  html.replace("%FMT24%",  settings.format_24h   ? "checked" : "");
  html.replace("%SECS%",   settings.show_seconds ? "checked" : "");
  html.replace("%DATE%",   settings.show_date    ? "checked" : "");
  html.replace("%GAMMA%",  String(settings.gamma, 1));
  server.send(200, "text/html", html);
}

void handleSave() {
  if (server.hasArg("ntp")) settings.ntp_server = server.arg("ntp");
  if (server.hasArg("tz"))  settings.tz_string  = server.arg("tz");
  settings.format_24h   = server.hasArg("fmt24");
  settings.show_seconds = server.hasArg("secs");
  settings.show_date    = server.hasArg("date");
  if (server.hasArg("gamma")) {
    settings.gamma = constrain(server.arg("gamma").toFloat(), MIN_GAMMA, MAX_GAMMA);
    buildGammaLut();
  }
  saveSettings();
  applyTimeSettings();
  Serial.printf("Saved: ntp=%s tz=%s 24h=%d secs=%d date=%d gamma=%.1f\n",
                settings.ntp_server.c_str(), settings.tz_string.c_str(),
                settings.format_24h, settings.show_seconds, settings.show_date,
                settings.gamma);
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleTime() {
  struct tm ti;
  if (!getLocalTime(&ti, 50)) { server.send(200, "text/plain", "--:--:--"); return; }
  server.send(200, "text/plain", formatTime(ti));
}

void handleResetWiFi() {
  server.send(200, "text/html",
    "<meta http-equiv='refresh' content='3'>"
    "<body style='font-family:sans-serif;background:#111;color:#eee;padding:30px'>"
    "<h1>Resetting WiFi…</h1><p>Device will reboot into setup mode.</p></body>");
  delay(800);
  WiFiManager wm;
  wm.resetSettings();
  delay(500);
  ESP.restart();
}

void handleNotFound() {
  server.send(404, "text/plain", "Not found");
}

void setupWebServer() {
  server.on("/",          handleRoot);
  server.on("/save",      HTTP_POST, handleSave);
  server.on("/time",      handleTime);
  server.on("/reset_wifi",HTTP_POST, handleResetWiFi);
  server.onNotFound(handleNotFound);
  server.begin();

  if (MDNS.begin(MDNS_NAME)) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("mDNS: http://%s.local\n", MDNS_NAME);
  }
}

// ── WiFi setup via WiFiManager ──────────────────────────────────────────────
void setupWiFi() {
  WiFiManager wm;
  wm.setConfigPortalTimeout(180);
  wm.setAPCallback(portalCallback);
  wm.setDebugOutput(false);

  // Set hostname BEFORE connecting so DHCP advertises it correctly
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);

  // Only offer the reset on the first call (boot), not on WiFi-loss reconnects
  static bool firstCall = true;
  if (firstCall && wifiResetRequested()) {
    showStatus("Resetting WiFi", "credentials...", TFT_RED);
    wm.resetSettings();
    delay(1500);
  }
  firstCall = false;

  showStatus("Connecting WiFi", "(saved creds)");
  if (!wm.autoConnect(AP_NAME, AP_PASSWORD)) {
    showStatus("Setup timeout", "Restarting...", TFT_RED);
    delay(2000);
    ESP.restart();
  }

  showStatus("Connected!", WiFi.localIP().toString(), TFT_GREEN);
  delay(2000);
  showStatus("Web UI at:", String(MDNS_NAME) + ".local", TFT_CYAN);
  delay(2500);
}

// ── State ───────────────────────────────────────────────────────────────────
int lastH = -1, lastM = -1, lastS = -1;
bool lastFmt24 = true, lastSecs = true, lastDate = true;  // detect setting changes
float lastGamma = 0;

// ── Pick largest font scale that fits the box (bitmap fonts scale in steps) ──
float pickTextScale(const String& s, int maxWidth, int maxHeight, float startScale) {
  for (float sc = startScale; sc > 1.0f; sc -= 0.25f) {
    spr.setTextSize(sc);
    if (spr.textWidth(s) <= maxWidth && spr.fontHeight() <= maxHeight) return sc;
  }
  return 1.0f;
}

// ── GT911 reset sequence (from Elecrow V3.0 reference code) ─────────────────
// PCA9557 IO0 = touch RST, IO1 = touch INT. Harmless on V2.x boards.
static void pca9557Write(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(PCA9557_ADDR);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
}

void resetTouchController() {
  pinMode(BOARD_IO38, OUTPUT);
  digitalWrite(BOARD_IO38, LOW);

  Wire.begin(TOUCH_SDA, TOUCH_SCL);
  pca9557Write(PCA9557_CONFIG, 0x00);  // all outputs
  pca9557Write(PCA9557_OUTPUT, 0x00);  // RST low, INT low
  delay(20);
  pca9557Write(PCA9557_OUTPUT, 0x01);  // release RST
  delay(100);
  pca9557Write(PCA9557_CONFIG, 0x02);  // INT back to input
  Wire.end();                          // LovyanGFX drives these pins on I2C_NUM_1
}

// ── Setup ───────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);

  resetTouchController();

  if (!display.begin()) {
    Serial.println("Display init failed — is OPI PSRAM enabled in the board options?");
    while (true) delay(1000);
  }
  bindSprite();
  spr.setTextDatum(MC_DATUM);

  loadSettings();
  buildGammaLut();

  Serial.printf("Display %dx%d, double-buffered, %.1f fps\n",
                SCREEN_W, SCREEN_H, display.measureFps());
  Serial.printf("Loaded: ntp=%s tz=%s 24h=%d secs=%d date=%d gamma=%.1f\n",
                settings.ntp_server.c_str(), settings.tz_string.c_str(),
                settings.format_24h, settings.show_seconds, settings.show_date,
                settings.gamma);

  // First frame is drawn into a framebuffer before the backlight comes on,
  // so the panel never shows uninitialized memory.
  showStatus("Color Clock", "Starting...", TFT_CYAN);
  display.setBacklight(true);

  setupWiFi();
  applyTimeSettings();
  setupWebServer();
}

// ── Loop ────────────────────────────────────────────────────────────────────
void loop() {
  server.handleClient();

  if (WiFi.status() != WL_CONNECTED) {
    showStatus("WiFi lost", "Reconnecting...", TFT_ORANGE);
    WiFi.reconnect();
    delay(2000);
    if (WiFi.status() != WL_CONNECTED) {
      setupWiFi();
      applyTimeSettings();
    }
  }

  struct tm ti;
  if (!getLocalTime(&ti, 10)) {
    delay(200);
    return;
  }

  int h = ti.tm_hour, m = ti.tm_min, s = ti.tm_sec;

  // Redraw if time changed OR settings changed
  bool settingsChanged = (settings.format_24h   != lastFmt24)
                      || (settings.show_seconds != lastSecs)
                      || (settings.show_date    != lastDate)
                      || (settings.gamma        != lastGamma);
  if (h == lastH && m == lastM && s == lastS && !settingsChanged) {
    delay(50);
    return;
  }
  lastH = h; lastM = m; lastS = s;
  lastFmt24 = settings.format_24h;
  lastSecs  = settings.show_seconds;
  lastDate  = settings.show_date;
  lastGamma = settings.gamma;

  // ── Color from time ────────────────────────────────────────────────────
  uint8_t  R  = mapH(h);
  uint8_t  G  = mapM(m);
  uint8_t  B  = mapS(s);
  uint8_t  dR = gammaLut[R], dG = gammaLut[G], dB = gammaLut[B];   // as displayed
  uint16_t bg = rgb888to565(dR, dG, dB);
  uint16_t fg = bestTextColor(dR, dG, dB);

  spr.fillSprite(bg);
  spr.setTextColor(fg, bg);
  spr.setTextDatum(MC_DATUM);

  // ── Hex code ─────────────────────────────────────────────────────────
  char hexBuf[8];
  snprintf(hexBuf, sizeof(hexBuf), "#%02X%02X%02X", R, G, B);
  useFont(&fonts::DejaVu56);
  spr.drawString(hexBuf, SCREEN_W/2, 50);

  // ── Time — dynamically sized to fit ──────────────────────────────────
  String timeStr = formatTime(ti);
  useFont(&fonts::DejaVu72);
  int timeBoxH = settings.show_date ? 230 : 300;
  spr.setTextSize(pickTextScale(timeStr, SCREEN_W - 40, timeBoxH, 4.0f));
  int timeY = settings.show_date ? 210 : 250;
  spr.drawString(timeStr, SCREEN_W/2, timeY);

  // ── Date (optional) ──────────────────────────────────────────────────
  if (settings.show_date) {
    String dateStr = String(dayNames[ti.tm_wday]) + "  " +
                     pad2(ti.tm_mday) + " " +
                     monthNames[ti.tm_mon] + " " +
                     String(1900 + ti.tm_year);
    useFont(&fonts::DejaVu56);
    spr.drawString(dateStr, SCREEN_W/2, 365);
  }

  // ── RGB strip ────────────────────────────────────────────────────────
  useFont(&fonts::DejaVu24);
  char rgbBuf[28];
  snprintf(rgbBuf, sizeof(rgbBuf), "R:%3d   G:%3d   B:%3d", R, G, B);
  spr.drawString(rgbBuf, SCREEN_W/2, 450);

  presentFrame();

  Serial.printf("%s  %s\n", timeStr.c_str(), hexBuf);
}
