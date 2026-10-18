#include <Arduino.h>
#include "crowpanel7.h"
#include <esp_lcd_panel_ops.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#define LCD_BL_PIN     2
// 928 x 525 clocks per frame (incl. porches): 20 MHz ≈ 41 Hz refresh. Elecrow's
// reference 12 MHz (≈25 Hz) shows rolling flicker on colors the panel dithers.
// Must divide 160 MHz evenly (16, 20, 26.67 ...).
#define LCD_PCLK_HZ    (20 * 1000 * 1000)
#define BOUNCE_LINES   10   // two SRAM bounce buffers of SCREEN_W * BOUNCE_LINES px

// Panel data pins in color-bit order: B0..B4, G0..G5, R0..R4
static const int DATA_PINS[16] = {
  15, 7, 6, 5, 4,            // B0-B4
  9, 46, 3, 8, 16, 1,        // G0-G5
  14, 21, 47, 48, 45,        // R0-R4
};

static SemaphoreHandle_t frameDone;
static volatile uint32_t frameCount = 0;

// Fires each time a complete frame has been fed to the LCD DMA.
static bool IRAM_ATTR onFrameComplete(esp_lcd_panel_handle_t, const esp_lcd_rgb_panel_event_data_t*, void*) {
  frameCount++;
  BaseType_t woken = pdFALSE;
  xSemaphoreGiveFromISR(frameDone, &woken);
  return woken == pdTRUE;
}

bool CrowPanel7::begin() {
  pinMode(LCD_BL_PIN, OUTPUT);
  digitalWrite(LCD_BL_PIN, LOW);

  frameDone = xSemaphoreCreateBinary();

  esp_lcd_rgb_panel_config_t cfg = {};
  cfg.clk_src         = LCD_CLK_SRC_DEFAULT;
  cfg.timings.pclk_hz = LCD_PCLK_HZ;
  cfg.timings.h_res   = SCREEN_W;
  cfg.timings.v_res   = SCREEN_H;
  cfg.timings.hsync_pulse_width = 48;
  cfg.timings.hsync_back_porch  = 40;
  cfg.timings.hsync_front_porch = 40;
  cfg.timings.vsync_pulse_width = 31;
  cfg.timings.vsync_back_porch  = 13;
  cfg.timings.vsync_front_porch = 1;
  // Same register values LovyanGFX's Bus_RGB produced with Elecrow's config
  // (hsync/vsync_polarity = 0 → idle low; it never applied pclk_active_neg).
  cfg.timings.flags.hsync_idle_low  = 1;
  cfg.timings.flags.vsync_idle_low  = 1;
  cfg.timings.flags.de_idle_high    = 0;
  cfg.timings.flags.pclk_active_neg = 0;
  cfg.timings.flags.pclk_idle_high  = 0;

  cfg.data_width            = 16;
  cfg.bits_per_pixel        = 16;
  cfg.num_fbs               = 2;
  cfg.bounce_buffer_size_px = SCREEN_W * BOUNCE_LINES;
  cfg.dma_burst_size        = 64;
  cfg.hsync_gpio_num = 39;
  cfg.vsync_gpio_num = 40;
  cfg.de_gpio_num    = 41;
  cfg.pclk_gpio_num  = 0;    // GPIO 0 = BOOT strap; unusable as a button
  cfg.disp_gpio_num  = -1;
  // LovyanGFX stores RGB565 byte-swapped (big-endian), so cross the two data
  // bytes in hardware instead of swapping every pixel in software.
  for (int i = 0; i < 16; i++) cfg.data_gpio_nums[i ^ 8] = DATA_PINS[i];
  cfg.flags.fb_in_psram = 1;

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
  tc.pin_rst  = -1;            // reset is via PCA9557 on V3.0 boards; see resetTouchController()
  tc.i2c_port = I2C_NUM_1;
  tc.pin_sda  = TOUCH_SDA;
  tc.pin_scl  = TOUCH_SCL;
  tc.freq     = 400000;
  tc.i2c_addr = 0x14;          // driver falls back to 0x5D automatically
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

void CrowPanel7::setBacklight(bool on) {
  digitalWrite(LCD_BL_PIN, on ? HIGH : LOW);
}

float CrowPanel7::measureFps() {
  uint32_t f0 = frameCount, t0 = millis();
  delay(1000);
  return (frameCount - f0) * 1000.0f / (millis() - t0);
}

bool CrowPanel7::touched() {
  if (!_touchOk) return false;
  lgfx::touch_point_t tp;
  return _touch.getTouchRaw(&tp, 1) > 0;
}
