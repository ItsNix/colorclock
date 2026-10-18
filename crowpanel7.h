/**
 * Display + touch driver for the Elecrow CrowPanel 7.0" ESP32 HMI (DIS08070H).
 *   - 800x480 16-bit parallel RGB panel, driven by ESP-IDF's esp_lcd RGB driver
 *     with two PSRAM framebuffers (tear-free swap at frame end) and SRAM bounce
 *     buffers (tolerates PSRAM stalls from WiFi / flash writes)
 *   - Backlight on GPIO 2 (plain on/off)
 *   - GT911 capacitive touch on I2C SDA 19 / SCL 20 (via LovyanGFX)
 *
 * Drawing is done with LovyanGFX: bind an LGFX_Sprite to backBuffer(), draw a
 * full frame, then call present().
 *
 * Pin and timing values taken from Elecrow's reference gfx_conf.h (CrowPanel_70),
 * which is identical for hardware V2.x and V3.0.
 */
#pragma once

#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <esp_lcd_panel_rgb.h>

#define SCREEN_W  800
#define SCREEN_H  480

#define TOUCH_SDA 19
#define TOUCH_SCL 20

class CrowPanel7 {
public:
  // Creates the panel and touch driver. Backlight stays off until setBacklight(true).
  bool begin();

  // Framebuffer that is not on screen; draw the next frame into it.
  void* backBuffer() const { return _fb[_back]; }

  // Shows the back buffer from the next frame on, and returns once the old
  // front buffer is no longer being scanned out (it becomes the new back buffer).
  void present();

  void setBacklight(bool on);
  float measureFps();   // blocks ~1 s
  bool touched();

private:
  esp_lcd_panel_handle_t _panel = nullptr;
  void*                  _fb[2] = {nullptr, nullptr};
  int                    _back  = 1;
  lgfx::Touch_GT911      _touch;
  bool                   _touchOk = false;
};
