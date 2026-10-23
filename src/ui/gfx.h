// Thin drawing layer over an LGFX_Sprite that renders one horizontal band
// of the screen. The ESP32 (no PSRAM) can't hold a full 320x240x16 frame, so
// the UI is drawn several times into a small band sprite, each time shifted
// by the band's top `oy`. All coordinates here are *screen* coordinates.
#pragma once
#include <LovyanGFX.hpp>
#include <math.h>
#include <stdint.h>

namespace ui {

// Always pass colors as uint32_t RGB888 — LovyanGFX treats plain `int` as RGB565.
namespace C {
constexpr uint32_t bg = 0x141413;
constexpr uint32_t card = 0x1E1D1B;
constexpr uint32_t card_hi = 0x282724;
constexpr uint32_t track = 0x34322E;
constexpr uint32_t line = 0x2C2B28;
constexpr uint32_t text = 0xF0EEE6;
constexpr uint32_t text2 = 0xA6A39A;
constexpr uint32_t text3 = 0x6E6C66;
constexpr uint32_t accent = 0xD97757;  // Claude terracotta
constexpr uint32_t accent_dim = 0x6B3B2C;
constexpr uint32_t ok = 0x6CC495;
constexpr uint32_t warn = 0xE9B44C;
constexpr uint32_t bad = 0xEB6650;
constexpr uint32_t c_in = 0x7AA2F7;
constexpr uint32_t c_out = 0xD97757;
constexpr uint32_t c_cr = 0x6CC495;
constexpr uint32_t c_cw = 0xB490E6;
}  // namespace C

inline uint32_t mix(uint32_t a, uint32_t b, float t) {
  if (t <= 0) return a;
  if (t >= 1) return b;
  auto ch = [&](int s) {
    float x = ((a >> s) & 0xFF) * (1 - t) + ((b >> s) & 0xFF) * t;
    return (uint32_t)(x + 0.5f) << s;
  };
  return ch(16) | ch(8) | ch(0);
}

// Utilization → status color (calm / getting close / nearly out).
inline uint32_t level_color(float pct) {
  if (pct >= 85) return C::bad;
  if (pct >= 60) return C::warn;
  return C::ok;
}

struct G {
  LGFX_Sprite* s;
  int oy;  // screen y of the band's first row
  int bh;  // band height

  int Y(int y) const { return y - oy; }
  bool vis(int y, int h) const { return y + h > oy && y < oy + bh; }

  void fill(int x, int y, int w, int h, uint32_t c) {
    if (vis(y, h)) s->fillRect(x, Y(y), w, h, c);
  }
  void rrect(int x, int y, int w, int h, int r, uint32_t c) {
    if (vis(y, h)) s->fillSmoothRoundRect(x, Y(y), w, h, r, c);
  }
  void circle(int x, int y, int r, uint32_t c) {
    if (vis(y - r, 2 * r + 1)) s->fillSmoothCircle(x, Y(y), r, c);
  }
  void line(float x0, float y0, float x1, float y1, float w, uint32_t c) {
    int top = (int)fminf(y0, y1) - (int)w - 1, h = (int)fabsf(y1 - y0) + 2 * (int)w + 3;
    if (vis(top, h)) s->drawWideLine(x0, Y(y0), x1, Y(y1), w, c);
  }

  // Text: datum is LovyanGFX's textdatum (top_left, middle_center, ...).
  // Transparent background; the sprite blends anti-aliased edges with what's below.
  void text(const char* str, int x, int y, const lgfx::IFont* f, uint32_t c,
            lgfx::textdatum_t d = lgfx::middle_left) {
    if (!vis(y - 30, 60)) return;
    s->setFont(f);
    s->setTextDatum(d);
    s->setTextColor(c);
    s->drawString(str, x, Y(y));
  }
  int width(const char* str, const lgfx::IFont* f) {
    s->setFont(f);
    return s->textWidth(str);
  }

  // Rounded progress bar; pct 0..100.
  void bar(int x, int y, int w, int h, float pct, uint32_t c, uint32_t track = C::track) {
    if (!vis(y, h)) return;
    int r = h / 2;
    s->fillSmoothRoundRect(x, Y(y), w, h, r, track);
    if (pct <= 0) return;
    int fw = (int)(w * fminf(pct, 100) / 100.0f + 0.5f);
    if (fw < h) fw = h;
    s->fillSmoothRoundRect(x, Y(y), fw, h, r, c);
  }

  // Anti-aliased ring gauge with rounded caps, starting at 12 o'clock, clockwise.
  // `bg` is the color under the ring (edges are blended against it).
  void ring(int cx, int cy, int r, int thick, float pct, uint32_t c, uint32_t track, uint32_t bg) {
    if (!vis(cy - r - 1, 2 * r + 3)) return;
    const float ro = r, ri = r - thick;
    const float sweep = fmaxf(0, fminf(pct, 100)) / 100.0f * 6.2831853f;
    for (int y = -r - 1; y <= r + 1; ++y) {
      int sy = cy + y;
      if (sy < oy || sy >= oy + bh) continue;
      for (int x = -r - 1; x <= r + 1; ++x) {
        float d = sqrtf((float)(x * x + y * y));
        float cov = fminf(ro + 0.5f - d, d - ri + 0.5f);
        if (cov <= 0) continue;
        if (cov > 1) cov = 1;
        float a = atan2f((float)x, (float)-y);  // 0 at top, clockwise
        if (a < 0) a += 6.2831853f;
        uint32_t col = a <= sweep ? c : track;
        s->drawPixel(cx + x, Y(sy), mix(bg, col, cov));
      }
    }
    if (pct > 0) {  // round caps
      float rm = (ro + ri) / 2, cr = thick / 2.0f;
      s->fillSmoothCircle(cx, Y(cy - (int)roundf(rm)), (int)cr, c);
      float ex = cx + rm * sinf(sweep), ey = cy - rm * cosf(sweep);
      s->fillSmoothCircle((int)roundf(ex), Y((int)roundf(ey)), (int)cr, c);
    }
  }
};

}  // namespace ui
