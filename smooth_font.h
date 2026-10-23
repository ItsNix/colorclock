// Anti-aliased VLW font (see fonts.h / tools/make_fonts.py), parsed once with
// load() and then used via spr.setFont(&f.font).
// Lives in a header so Arduino's auto-generated prototypes can see the type.
#pragma once
#include "crowpanel7.h"   // LovyanGFX
#include <esp_heap_caps.h>

struct SmoothFont {
  lgfx::PointerWrapper data;
  lgfx::VLWfont        font;

  // Copies the font from flash into PSRAM first: flash and PSRAM share one bus,
  // and pulling big glyphs from (much slower) flash while drawing starved the
  // RGB panel's scan-out, showing as flashing lines on the digits.
  void load(const uint8_t* vlw) {
    size_t len = size(vlw);
    uint8_t* copy = (uint8_t*)heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
    if (copy) memcpy(copy, vlw, len);
    data.set(copy ? copy : vlw, len);
    font.loadFont(&data);
  }

  // Total VLW size: 24-byte header + 28 bytes per glyph + every glyph's w*h bitmap.
  static size_t size(const uint8_t* vlw) {
    auto be32 = [](const uint8_t* p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; };
    uint32_t count = be32(vlw);
    size_t len = 24 + count * 28;
    for (uint32_t i = 0; i < count; i++) {
      const uint8_t* g = vlw + 24 + i * 28;
      len += (size_t)be32(g + 4) * be32(g + 8);   // height * width
    }
    return len;
  }
};
