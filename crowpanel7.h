/**
 * Display + touch driver for Elecrow CrowPanel 7.0" ESP32 boards (800x480 RGB).
 *
 * Supported boards (pick at compile time):
 *   BOARD_CROWPANEL_7          CrowPanel 7.0" HMI, SKU DIS08070H (TN), hardware V2.x / V3.0
 *   BOARD_CROWPANEL_ADVANCE_7  CrowPanel Advance 7.0" (IPS), hardware V1.3 / V1.4 / V1.5
 * Default is BOARD_CROWPANEL_7; for the Advance add
 *   --build-property compiler.cpp.extra_flags=-DBOARD_CROWPANEL_ADVANCE_7
 * (Advance V1.0 / V1.2 use different backlight / touch-reset hardware and are not supported.)
 *
 * Both boards use ESP-IDF's esp_lcd RGB driver with two PSRAM framebuffers
 * (tear-free swap at frame end) and SRAM bounce buffers (tolerates PSRAM stalls
 * from WiFi / flash writes), plus LovyanGFX's GT911 touch driver.
 *
 * Drawing is done with LovyanGFX: bind an LGFX_Sprite to backBuffer(), draw a
 * full frame, then call present().
 */
#pragma once

#if !defined(BOARD_CROWPANEL_7) && !defined(BOARD_CROWPANEL_ADVANCE_7)
#define BOARD_CROWPANEL_7
#endif
#if defined(BOARD_CROWPANEL_7) && defined(BOARD_CROWPANEL_ADVANCE_7)
#error "Define only one of BOARD_CROWPANEL_7 / BOARD_CROWPANEL_ADVANCE_7"
#endif

#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <esp_lcd_panel_rgb.h>

#define SCREEN_W  800
#define SCREEN_H  480

class CrowPanel7 {
public:
  // Powers up the board, panel and touch. Backlight stays off until setBacklight(true).
  bool begin();

  // Framebuffer that is not on screen; draw the next frame into it.
  void* backBuffer() const { return _fb[_back]; }

  // Shows the back buffer from the next frame on, and returns once the old
  // front buffer is no longer being scanned out (it becomes the new back buffer).
  void present();

  void setBacklight(bool on);
  float measureFps();   // blocks ~1 s
  // Change the pixel clock live (takes effect next frame). The real clock is
  // 160 MHz / integer, so the requested value is rounded by the driver.
  bool setPixelClock(uint32_t hz);
  bool touched();

  static const char* boardName();

private:
  esp_lcd_panel_handle_t _panel = nullptr;
  void*                  _fb[2] = {nullptr, nullptr};
  int                    _back  = 1;
  lgfx::Touch_GT911      _touch;
  bool                   _touchOk = false;
};
