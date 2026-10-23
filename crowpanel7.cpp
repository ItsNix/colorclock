#include <Arduino.h>
#include <Wire.h>
#include "crowpanel7.h"
#include <esp_lcd_panel_ops.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// ── Per-board configuration ─────────────────────────────────────────────────
// Pin and timing values from Elecrow's reference code for each board.
struct BoardConfig {
  const char* name;
  int data_pins[16];          // color-bit order: B0..B4, G0..G5, R0..R4
  int hsync, vsync, de, pclk;
  uint32_t pclk_hz;
  uint32_t hsync_pulse, hsync_back, hsync_front;
  uint32_t vsync_pulse, vsync_back, vsync_front;
  bool hsync_idle_low, vsync_idle_low, pclk_active_neg, pclk_idle_high;
  bool bb_invalidate_cache;
  int touch_sda, touch_scl;
};

#if defined(BOARD_CROWPANEL_7)
// Elecrow gfx_conf.h (CrowPanel_70), identical for V2.x and V3.0.
// 928 x 525 clocks per frame: 20 MHz ≈ 41 Hz. Elecrow's 12 MHz (≈25 Hz) showed
// rolling flicker; 26.67 MHz exceeds PSRAM bandwidth (artifacts).
static const BoardConfig BOARD = {
  "CrowPanel 7.0 (DIS08070H)",
  { 15, 7, 6, 5, 4,   9, 46, 3, 8, 16, 1,   14, 21, 47, 48, 45 },
  39, 40, 41, 0,               // GPIO 0 = BOOT strap; unusable as a button
  20 * 1000 * 1000,
  48, 40, 40,
  31, 13, 1,
  true, true, true, false,     // Elecrow's pclk_active_neg = 1 (LovyanGFX's Bus_RGB ignored it)
  false,
  19, 20,
};
#define LCD_BL_PIN       2
#define LCD_BL_PWM_HZ    5000  // backlight dimming via LEDC, 8-bit duty
#define PCA9557_ADDR     0x18  // V3.0: gates GT911 RST/INT; absent on V2.x (writes ignored)
#define PCA9557_OUTPUT   0x01
#define PCA9557_CONFIG   0x03  // bit = 1 → input
#define BOARD_IO38       38    // driven LOW by Elecrow's V3.0 reference code

#elif defined(BOARD_CROWPANEL_ADVANCE_7)
// Elecrow CrowPanel-Advance-7 repo, example/V1.3_and_V1.4_and_V1.5 (esp_lcd config).
// 820 x 500 clocks per frame: 18 MHz ≈ 44 Hz.
static const BoardConfig BOARD = {
  "CrowPanel Advance 7.0 (V1.3-V1.5)",
  { 21, 47, 48, 45, 38,   9, 10, 11, 12, 13, 14,   7, 17, 18, 3, 46 },
  40, 41, 42, 39,
  18 * 1000 * 1000,
  4, 8, 8,
  4, 8, 8,
  false, false, true, true,
  true,                        // as in Elecrow's example: frees cache after scan-out reads
  15, 16,
};
#define STC8_ADDR          0x30  // helper MCU: backlight, touch reset, buzzer
#define STC8_BL_MAX        0     // 0 = brightest … 244 = dimmest
#define STC8_BL_OFF        245
#define STC8_BUZZER_ON     246   // not in Elecrow's examples; from a V1.3 ESP-IDF demo
#define STC8_BUZZER_OFF    247
#define STC8_TOUCH_ENABLE  250
#define GT911_ADDR         0x5D
#define BM8563_ADDR        0x51  // RTC (PCF8563-compatible), CR1220 backup cell
#define TOUCH_RESET_PIN    1     // pulsed LOW by Elecrow's example during touch bring-up
#define BOARD_IO19         19    // set OUTPUT by Elecrow's example
#endif

#define TOUCH_I2C_PORT  I2C_NUM_1   // LovyanGFX owns the touch bus after begin()
// Two internal-SRAM bounce buffers of SCREEN_W * BOUNCE_LINES px (2 x 32 KB at 20).
// Larger = more slack before scan-out underruns when PSRAM/flash are busy.
#define BOUNCE_LINES    20

static SemaphoreHandle_t frameDone;
static volatile uint32_t frameCount = 0;

// Fires each time a complete frame has been fed to the LCD DMA.
static bool IRAM_ATTR onFrameComplete(esp_lcd_panel_handle_t, const esp_lcd_rgb_panel_event_data_t*, void*) {
  frameCount++;
  BaseType_t woken = pdFALSE;
  xSemaphoreGiveFromISR(frameDone, &woken);
  return woken == pdTRUE;
}

// ── Board power-up (runs before the panel and touch drivers start) ──────────
#if defined(BOARD_CROWPANEL_7)
static void pca9557Write(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(PCA9557_ADDR);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
}

static void boardPowerUp() {
  ledcAttach(LCD_BL_PIN, LCD_BL_PWM_HZ, 8);
  ledcWrite(LCD_BL_PIN, 0);
  pinMode(BOARD_IO38, OUTPUT);
  digitalWrite(BOARD_IO38, LOW);

  // GT911 reset sequence from Elecrow's V3.0 code: PCA9557 IO0 = RST, IO1 = INT.
  Wire.begin(BOARD.touch_sda, BOARD.touch_scl);
  pca9557Write(PCA9557_CONFIG, 0x00);  // all outputs
  pca9557Write(PCA9557_OUTPUT, 0x00);  // RST low, INT low
  delay(20);
  pca9557Write(PCA9557_OUTPUT, 0x01);  // release RST
  delay(100);
  pca9557Write(PCA9557_CONFIG, 0x02);  // INT back to input
  Wire.end();
}

#elif defined(BOARD_CROWPANEL_ADVANCE_7)
static bool i2cPresent(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

static void stc8WireCommand(uint8_t cmd) {
  Wire.beginTransmission(STC8_ADDR);
  Wire.write(cmd);
  Wire.endTransmission();
}

static void boardPowerUp() {
  pinMode(BOARD_IO19, OUTPUT);
  Wire.begin(BOARD.touch_sda, BOARD.touch_scl);
  delay(50);
  // Elecrow's bring-up: until both the STC8 and the GT911 answer, ask the STC8
  // to enable touch and pulse the touch reset line.
  for (int attempt = 0; attempt < 10; attempt++) {
    if (i2cPresent(STC8_ADDR) && i2cPresent(GT911_ADDR)) break;
    Serial.printf("Waiting for STC8 (0x%02X) / GT911 (0x%02X)...\n", STC8_ADDR, GT911_ADDR);
    stc8WireCommand(STC8_TOUCH_ENABLE);
    pinMode(TOUCH_RESET_PIN, OUTPUT);
    digitalWrite(TOUCH_RESET_PIN, LOW);
    delay(120);
    pinMode(TOUCH_RESET_PIN, INPUT);
    delay(100);
  }
  stc8WireCommand(STC8_BL_OFF);
  Wire.end();   // later STC8 writes go through LovyanGFX's I2C on TOUCH_I2C_PORT
}
#endif

const char* CrowPanel7::boardName() { return BOARD.name; }

bool CrowPanel7::begin() {
  boardPowerUp();

  frameDone = xSemaphoreCreateBinary();

  esp_lcd_rgb_panel_config_t cfg = {};
  cfg.clk_src         = LCD_CLK_SRC_DEFAULT;
  cfg.timings.pclk_hz = BOARD.pclk_hz;
  cfg.timings.h_res   = SCREEN_W;
  cfg.timings.v_res   = SCREEN_H;
  cfg.timings.hsync_pulse_width = BOARD.hsync_pulse;
  cfg.timings.hsync_back_porch  = BOARD.hsync_back;
  cfg.timings.hsync_front_porch = BOARD.hsync_front;
  cfg.timings.vsync_pulse_width = BOARD.vsync_pulse;
  cfg.timings.vsync_back_porch  = BOARD.vsync_back;
  cfg.timings.vsync_front_porch = BOARD.vsync_front;
  cfg.timings.flags.hsync_idle_low  = BOARD.hsync_idle_low;
  cfg.timings.flags.vsync_idle_low  = BOARD.vsync_idle_low;
  cfg.timings.flags.de_idle_high    = 0;
  cfg.timings.flags.pclk_active_neg = BOARD.pclk_active_neg;
  cfg.timings.flags.pclk_idle_high  = BOARD.pclk_idle_high;

  cfg.data_width            = 16;
  cfg.bits_per_pixel        = 16;
  cfg.num_fbs               = 2;
  cfg.bounce_buffer_size_px = SCREEN_W * BOUNCE_LINES;
  cfg.dma_burst_size        = 64;
  cfg.hsync_gpio_num = BOARD.hsync;
  cfg.vsync_gpio_num = BOARD.vsync;
  cfg.de_gpio_num    = BOARD.de;
  cfg.pclk_gpio_num  = BOARD.pclk;
  cfg.disp_gpio_num  = -1;
  // LovyanGFX stores RGB565 byte-swapped (big-endian), so cross the two data
  // bytes in hardware instead of swapping every pixel in software.
  for (int i = 0; i < 16; i++) cfg.data_gpio_nums[i ^ 8] = BOARD.data_pins[i];
  cfg.flags.fb_in_psram         = 1;
  cfg.flags.bb_invalidate_cache = BOARD.bb_invalidate_cache;

  if (esp_lcd_new_rgb_panel(&cfg, &_panel) != ESP_OK) return false;

  esp_lcd_rgb_panel_event_callbacks_t cbs = {};
  cbs.on_frame_buf_complete = onFrameComplete;
  esp_lcd_rgb_panel_register_event_callbacks(_panel, &cbs, nullptr);

  esp_lcd_panel_reset(_panel);
  esp_lcd_panel_init(_panel);
  esp_lcd_rgb_panel_get_frame_buffer(_panel, 2, &_fb[0], &_fb[1]);
  memset(_fb[0], 0, SCREEN_W * SCREEN_H * 2);
  memset(_fb[1], 0, SCREEN_W * SCREEN_H * 2);
  _back = 1;   // driver starts scanning out fb 0

  auto tc = _touch.config();
  tc.x_min = 0;  tc.x_max = SCREEN_W - 1;
  tc.y_min = 0;  tc.y_max = SCREEN_H - 1;
  tc.pin_int  = -1;
  tc.pin_rst  = -1;            // reset handled in boardPowerUp()
  tc.i2c_port = TOUCH_I2C_PORT;
  tc.pin_sda  = BOARD.touch_sda;
  tc.pin_scl  = BOARD.touch_scl;
  tc.freq     = 400000;
  tc.i2c_addr = 0x5D;          // driver falls back to 0x14 automatically
  _touch.config(tc);
  _touchOk = _touch.init();
  if (!_touchOk) Serial.println("GT911 touch not found");

  return true;
}

void CrowPanel7::present() {
  xSemaphoreTake(frameDone, 0);                       // drop any stale event
  // Passing one of the driver's own framebuffers makes it switch to that buffer
  // at the start of the next frame instead of copying.
  esp_lcd_panel_draw_bitmap(_panel, 0, 0, SCREEN_W, SCREEN_H, _fb[_back]);
  // Wait for the frame in progress to finish, then for one full frame from the
  // new buffer, so the old buffer is no longer being read.
  xSemaphoreTake(frameDone, pdMS_TO_TICKS(100));
  xSemaphoreTake(frameDone, pdMS_TO_TICKS(100));
  _back ^= 1;
}

#if defined(BOARD_CROWPANEL_ADVANCE_7)
static void stc8Command(uint8_t cmd) {
  if (lgfx::i2c::transactionWrite(TOUCH_I2C_PORT, STC8_ADDR, &cmd, 1).has_error()) {
    Serial.printf("STC8 command %u failed\n", cmd);
  }
}
#endif

void CrowPanel7::setBrightness(uint8_t percent) {
  percent = min<uint8_t>(percent, 100);
#if defined(BOARD_CROWPANEL_7)
  ledcWrite(LCD_BL_PIN, percent * 255 / 100);
#elif defined(BOARD_CROWPANEL_ADVANCE_7)
  // 1..100 % → 244 (dimmest) .. 0 (brightest); 0 % → off.
  stc8Command(percent == 0 ? STC8_BL_OFF
                           : STC8_BL_MAX + (100 - percent) * (STC8_BL_OFF - 1 - STC8_BL_MAX) / 99);
#endif
}

void CrowPanel7::setBuzzer(bool on) {
#if defined(BOARD_CROWPANEL_ADVANCE_7)
  stc8Command(on ? STC8_BUZZER_ON : STC8_BUZZER_OFF);
#endif
}

#if defined(BOARD_CROWPANEL_ADVANCE_7)
static uint8_t bcd(int v)       { return (v / 10) << 4 | (v % 10); }
static int     unbcd(uint8_t b) { return (b >> 4) * 10 + (b & 0x0F); }

// Days since 1970-01-01 for a Gregorian date (newlib has no timegm()).
static int64_t daysFromCivil(int y, int m, int d) {
  y -= m <= 2;
  int era = y / 400;                    // y >= 2000 here, so no negative-era fixup
  int yoe = y - era * 400;
  int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return (int64_t)era * 146097 + doe - 719468;
}
#endif

bool CrowPanel7::readRtc(time_t& utc) {
#if defined(BOARD_CROWPANEL_ADVANCE_7)
  uint8_t reg = 0x02, r[7];   // seconds, minutes, hours, day, weekday, month, year
  if (lgfx::i2c::transactionWriteRead(TOUCH_I2C_PORT, BM8563_ADDR, &reg, 1, r, 7).has_error()) {
    return false;
  }
  if (r[0] & 0x80) return false;   // VL: oscillator stopped since the last write (no / flat cell)
  int64_t days = daysFromCivil(2000 + unbcd(r[6]), unbcd(r[5] & 0x1F), unbcd(r[3] & 0x3F));
  utc = days * 86400 + unbcd(r[2] & 0x3F) * 3600 + unbcd(r[1] & 0x7F) * 60 + unbcd(r[0] & 0x7F);
  return true;
#else
  return false;
#endif
}

bool CrowPanel7::writeRtc(time_t utc) {
#if defined(BOARD_CROWPANEL_ADVANCE_7)
  struct tm t;
  gmtime_r(&utc, &t);
  const uint8_t w[] = {
    0x00,                           // start register
    0x00, 0x00,                     // control 1 (clock running), control 2 (no alarm/timer)
    bcd(t.tm_sec),                  // writing seconds also clears VL
    bcd(t.tm_min), bcd(t.tm_hour), bcd(t.tm_mday), (uint8_t)t.tm_wday,
    bcd(t.tm_mon + 1),              // century bit 0 = 20xx
    bcd(t.tm_year % 100),
  };
  return !lgfx::i2c::transactionWrite(TOUCH_I2C_PORT, BM8563_ADDR, w, sizeof(w)).has_error();
#else
  return false;
#endif
}

bool CrowPanel7::setPixelClock(uint32_t hz) {
  return esp_lcd_rgb_panel_set_pclk(_panel, hz) == ESP_OK;
}

float CrowPanel7::measureFps() {
  uint32_t f0 = frameCount, t0 = millis();
  delay(1000);
  return (frameCount - f0) * 1000.0f / (millis() - t0);
}

bool CrowPanel7::touched() {
  int x, y;
  return touchPoint(x, y);
}

bool CrowPanel7::touchPoint(int& x, int& y) {
  if (!_touchOk) return false;
  lgfx::touch_point_t tp;
  if (_touch.getTouchRaw(&tp, 1) == 0) return false;
  x = tp.x;
  y = tp.y;
  return true;
}
