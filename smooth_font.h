// Anti-aliased VLW font held in flash (see fonts.h / tools/make_fonts.py),
// parsed once with load() and then used via spr.setFont(&f.font).
// Lives in a header so Arduino's auto-generated prototypes can see the type.
#pragma once
#include "crowpanel7.h"   // LovyanGFX

struct SmoothFont {
  lgfx::PointerWrapper data;
  lgfx::VLWfont        font;
  void load(const uint8_t* vlw) { data.set(vlw); font.loadFont(&data); }
};
