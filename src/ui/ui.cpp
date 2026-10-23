#include "ui.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "fonts_gen.h"
#include "gfx.h"

namespace ui {

using lgfx::textdatum_t;
using cu::Account;
using cu::Limit;

// ---------------------------------------------------------------- fonts ---

struct Font {
  lgfx::PointerWrapper src;
  lgfx::VLWfont f;
  void load(const uint8_t* p) {
    src.set(p);
    f.loadFont(&src);
  }
};
static Font s_xs, s_sm, s_md, s_lg, s_xl, s_hero;
static const lgfx::IFont* F_XS = &s_xs.f;
static const lgfx::IFont* F_SM = &s_sm.f;
static const lgfx::IFont* F_MD = &s_md.f;
static const lgfx::IFont* F_LG = &s_lg.f;
static const lgfx::IFont* F_XL = &s_xl.f;
static const lgfx::IFont* F_HERO = &s_hero.f;

// ---------------------------------------------------------------- state ---

constexpr int W = 320, H = 240;
constexpr int kBandH = 60;  // 320x60x2 = 38 KB sprite, fits ESP32 heap with WiFi/TLS
constexpr uint32_t kAnimMs = 520;
constexpr uint32_t kLongMs = 650;

static lgfx::LovyanGFX* s_lcd = nullptr;
// Two band buffers: one is drawn while the other is still going out over SPI DMA.
static LGFX_Sprite s_band[2];
static int s_bands = 0;  // buffers actually allocated
static const cu::Dashboard* s_data = nullptr;

static int s_view = V_OVERVIEW;
static int s_sel = 0;
static uint32_t s_anim_start = 0;
static float s_anim = 1;
static bool s_dirty = true;
static uint32_t s_last_draw = 0;
static bool s_refresh_req = false;
static bool s_refreshing = false;
static char s_toast[40] = {};
static uint32_t s_toast_until = 0;
static uint32_t s_ms = 0;

// Yes/no question shown over everything (pairing a new computer).
static bool s_ask = false;
static char s_ask_title[32], s_ask_subject[40], s_ask_question[64];
static int s_answer = -1;

static const uint32_t kAvatar[] = {0xD97757, 0x7AA2F7, 0x6CC495, 0xB490E6, 0xE9B44C, 0x5EC4C9};

// --------------------------------------------------------------- format ---

static void fmt_tokens(char* b, size_t n, double v) {
  if (v < 1e3) snprintf(b, n, "%.0f", v);
  else if (v < 1e5) snprintf(b, n, "%.1fK", v / 1e3);
  else if (v < 1e6) snprintf(b, n, "%.0fK", v / 1e3);
  else if (v < 1e7) snprintf(b, n, "%.2fM", v / 1e6);
  else if (v < 1e8) snprintf(b, n, "%.1fM", v / 1e6);
  else if (v < 1e9) snprintf(b, n, "%.0fM", v / 1e6);
  else snprintf(b, n, "%.2fB", v / 1e9);
}

static void fmt_dur(char* b, size_t n, int64_t s) {
  if (s < 60) snprintf(b, n, "<1m");
  else if (s < 3600) snprintf(b, n, "%dm", (int)(s / 60));
  else if (s < 86400) snprintf(b, n, "%dh %dm", (int)(s / 3600), (int)(s % 3600 / 60));
  else snprintf(b, n, "%dd %dh", (int)(s / 86400), (int)(s % 86400 / 3600));
}

// Wall-clock fields in the bridge PC's timezone (the device has no tz database).
static void local_tm(int64_t t, struct tm* out) {
  time_t tt = (time_t)(t + (s_data ? s_data->tz_offset : 0));
#if defined(_WIN32)
  gmtime_s(out, &tt);
#else
  gmtime_r(&tt, out);
#endif
}

static const char* const kWday[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
static const char* const kWdayLetter[] = {"S", "M", "T", "W", "T", "F", "S"};

static void fmt_when(char* b, size_t n, int64_t at, int64_t now) {
  struct tm a, c;
  local_tm(at, &a);
  local_tm(now, &c);
  if (a.tm_yday == c.tm_yday && a.tm_year == c.tm_year)
    snprintf(b, n, "today %02d:%02d", a.tm_hour, a.tm_min);
  else
    snprintf(b, n, "%s %02d:%02d", kWday[a.tm_wday], a.tm_hour, a.tm_min);
}

static float ease(float t) {
  if (t >= 1) return 1;
  float u = 1 - t;
  return 1 - u * u * u;
}

// ----------------------------------------------------------- components ---

static void spark(G& g, int cx, int cy, int r, uint32_t c) {
  // Radial burst, a nod to the Claude mark.
  for (int i = 0; i < 12; ++i) {
    float a = i * 6.2831853f / 12 + 0.13f;
    float len = (i % 2) ? r * 0.72f : r;
    g.line(cx, cy, cx + sinf(a) * len, cy - cosf(a) * len, 1.25f, c);
  }
  g.circle(cx, cy, 2, c);
}

static void avatar(G& g, int cx, int cy, int r, int idx, const char* name) {
  g.circle(cx, cy, r, kAvatar[idx % 6]);
  char ini[2] = {name[0] ? (char)(name[0] >= 'a' && name[0] <= 'z' ? name[0] - 32 : name[0]) : '?', 0};
  g.text(ini, cx, cy, r >= 12 ? F_MD : F_SM, C::bg, lgfx::middle_center);
}

// Plan pill right after (x, y-center). Returns its width.
static int pill(G& g, int x, int cy, const char* label, uint32_t fg, uint32_t bg) {
  int w = g.width(label, F_XS) + 12;
  g.rrect(x, cy - 7, w, 14, 7, bg);
  g.text(label, x + w / 2, cy, F_XS, fg, lgfx::middle_center);
  return w;
}

static void chevron(G& g, int cx, int cy, int dir, uint32_t c) {
  g.line(cx + 2 * dir, cy - 4, cx - 2 * dir, cy, 0.9f, c);
  g.line(cx - 2 * dir, cy, cx + 2 * dir, cy + 4, 0.9f, c);
}

static void footer(G& g, const char* mid) {
  const int y = 229;
  const int xs[3] = {66, 160, 254};
  for (int i = 0; i < 3; ++i) g.rrect(xs[i] - 26, y - 8, 52, 17, 8, C::card);
  chevron(g, xs[0], y, 1, C::text2);
  g.text(mid, xs[1], y, F_XS, C::text2, lgfx::middle_center);
  chevron(g, xs[2], y, -1, C::text2);
}

// Green: bridge connected and data fresh. Amber: stale. Red: no bridge.
static uint32_t status_color(int64_t now) {
  if (!s_data || !s_data->online) return C::bad;
  if (now && s_data->fetched_at && now - s_data->fetched_at > 180) return C::warn;
  return C::ok;
}

static void clock_status(G& g, int64_t now) {
  char t[8] = "--:--";
  if (now > 0) {
    struct tm tm;
    local_tm(now, &tm);
    snprintf(t, sizeof t, "%02d:%02d", tm.tm_hour, tm.tm_min);
  }
  int w = g.width(t, F_MD);
  g.text(t, 311, 15, F_MD, C::text, lgfx::middle_right);
  g.circle(311 - w - 9, 15, 3, status_color(now));
}

// Header for per-account views: avatar, name, plan, page dots.
static void account_header(G& g, const Account& a, int idx, int view) {
  avatar(g, 18, 16, 10, idx, a.name);
  g.text(a.name, 35, 15, F_MD, C::text);
  int x = 35 + g.width(a.name, F_MD) + 7;
  pill(g, x, 16, a.plan, C::accent, mix(C::bg, C::accent, 0.18f));
  // API accounts have no plan limits, so their Limits page is skipped.
  const int pages = a.is_api() ? 2 : 3, cur = view - (a.is_api() ? V_TOKENS : V_LIMITS);
  for (int i = 0; i < pages; ++i)
    g.circle(310 - (pages - 1 - i) * 10, 16, i == cur ? 3 : 2, i == cur ? C::accent : C::track);
  if (s_data && s_data->count > 1) {
    char n[8];
    snprintf(n, sizeof n, "%d/%d", idx + 1, s_data->count);
    g.text(n, 278, 15, F_XS, C::text3, lgfx::middle_right);
  }
}

static void pct_str(char* b, size_t n, float pct) { snprintf(b, n, "%d%%", (int)(pct + 0.5f)); }

// ------------------------------------------------------------- overview ---

static void draw_waiting(G& g) {
  // Gentle pulse so it's clear the device is alive.
  float p = 0.5f + 0.5f * sinf(s_ms / 420.0f);
  spark(g, 160, 82, 22, mix(C::accent_dim, C::accent, p));
  g.text("Waiting for your PC", 160, 128, F_LG, C::text, lgfx::middle_center);
  g.text("Run m5claude-bridge on your computer.", 160, 152, F_SM, C::text2, lgfx::middle_center);
  g.text("It connects over USB or Bluetooth automatically.", 160, 170, F_XS, C::text3,
         lgfx::middle_center);
}

// Card body for an API organization: spend instead of plan limits.
static void api_card(G& g, const Account& a, int x, int y, int w, int tx, float e) {
  char b[32], v[16];
  snprintf(b, sizeof b, a.cost_week * e < 1000 ? "$%.2f" : "$%.0f", a.cost_week * e);
  g.text(b, x + w - 10, y + 15, F_LG, C::text, lgfx::middle_right);
  g.text("7d", x + w - 12 - g.width(b, F_LG) - 4, y + 16, F_XS, C::text3, lgfx::middle_right);
  fmt_tokens(v, sizeof v, (double)a.tokens_today * e);
  snprintf(b, sizeof b, "Today  $%.2f", a.cost_today * e);
  g.text(b, tx, y + 34, F_SM, C::text2);
  snprintf(b, sizeof b, "%s tokens", v);
  g.text(b, tx + 136, y + 34, F_SM, C::text2);
}

// Tiny 7-day bar chart; today's bar is highlighted.
static void mini_bars(G& g, int x, int bottom, int w, int h, const uint64_t* daily, float e,
                      uint32_t bg) {
  uint64_t mx = 1;
  for (int i = 0; i < 7; ++i) mx = daily[i] > mx ? daily[i] : mx;
  const int slot = w / 7, bw = slot - 3;
  for (int i = 0; i < 7; ++i) {
    int bh = (int)(daily[i] * (float)h / mx * e);
    if (daily[i] && bh < 2) bh = 2;
    g.rrect(x + i * slot, bottom - h, bw, h, 2, mix(bg, C::track, 0.5f));
    if (bh > 0) g.rrect(x + i * slot, bottom - bh, bw, bh, 2, i == 6 ? C::accent : mix(bg, C::accent, 0.45f));
  }
}

// One row of a meter: LABEL ............ 42%  /  bar  /  footnote.
static void meter(G& g, int x, int y, int w, const char* label, const Limit& l, int64_t now, float e) {
  char b[32], d[16];
  g.text(label, x, y, F_XS, C::text3);
  if (!l.present) {
    g.text("—", x + w, y, F_MD, C::text3, lgfx::middle_right);
    g.bar(x, y + 10, w, 6, 0, C::track);
    return;
  }
  const bool reset = l.resets_at && l.resets_at <= now;
  const float pct = reset ? 0 : l.pct;
  pct_str(b, sizeof b, pct * e);
  g.text(b, x + w, y, F_MD, level_color(pct), lgfx::middle_right);
  g.bar(x, y + 10, w, 6, pct * e, level_color(pct));
  if (reset) {
    snprintf(b, sizeof b, "reset, waiting for update");
  } else {
    fmt_dur(d, sizeof d, l.resets_at - now);
    snprintf(b, sizeof b, "resets in %s", d);
  }
  g.text(b, x, y + 25, F_XS, C::text2);
}

// A single account fills the whole screen.
static void hero_card(G& g, const Account& a, int64_t now, float e) {
  const int x = 8, y = 34, w = 304, h = 180;
  g.rrect(x, y, w, h, 12, C::card);
  avatar(g, x + 18, y + 18, 10, 0, a.name);
  g.text(a.name, x + 34, y + 17, F_MD, C::text);
  pill(g, x + 34 + g.width(a.name, F_MD) + 7, y + 18, a.plan, C::accent, mix(C::card, C::accent, 0.2f));
  char b[32], v[16];

  if (!a.ok) {
    g.text(a.error[0] ? a.error : "Error", 160, y + 96, F_MD, C::bad, lgfx::middle_center);
    return;
  }
  if (a.is_api()) {
    g.text("SPEND · 7 DAYS", x + 12, y + 46, F_XS, C::text3);
    snprintf(b, sizeof b, a.cost_week * e < 1000 ? "$%.2f" : "$%.0f", a.cost_week * e);
    g.text(b, x + 11, y + 74, F_XL, C::text);
    snprintf(b, sizeof b, "Today  $%.2f", a.cost_today * e);
    g.text(b, x + 12, y + 100, F_SM, C::text2);
    mini_bars(g, x + 168, y + 108, 126, 60, a.daily, e, C::card);
    g.fill(x + 12, y + 122, w - 24, 1, C::line);
    fmt_tokens(v, sizeof v, (double)a.tokens_today * e);
    g.text("TOKENS TODAY", x + 12, y + 140, F_XS, C::text3);
    g.text(v, x + 12, y + 160, F_MD, C::text);
    fmt_tokens(v, sizeof v, (double)a.week.total() * e);
    g.text("TOKENS · 7D", x + 112, y + 140, F_XS, C::text3);
    g.text(v, x + 112, y + 160, F_MD, C::text);
    if (a.model_count) {
      g.text("TOP MODEL", x + 212, y + 140, F_XS, C::text3);
      snprintf(b, sizeof b, "%s %d%%", a.models[0].name, (int)(a.models[0].pct + 0.5f));
      g.text(b, x + 212, y + 160, F_MD, C::text);
    }
    return;
  }

  // Left: weekly ring.
  const Limit& wk = a.weekly;
  const int cx = x + 66, cy = y + 94;
  g.ring(cx, cy, 48, 9, wk.present ? wk.pct * e : 0, wk.present ? level_color(wk.pct) : C::track,
         C::track, C::card);
  if (wk.present) {
    char num[8];
    snprintf(num, sizeof num, "%d", (int)(wk.pct * e + 0.5f));
    int nw = g.width(num, F_HERO), pw = g.width("%", F_SM);
    int x0 = cx - (nw + pw + 1) / 2;
    g.text(num, x0, cy - 5, F_HERO, C::text);
    g.text("%", x0 + nw + 1, cy - 11, F_SM, C::text2);
    g.text("WEEKLY", cx, cy + 19, F_XS, C::text3, lgfx::middle_center);
    fmt_when(v, sizeof v, wk.resets_at, now);
    snprintf(b, sizeof b, "Resets %s", v);
    g.text(b, cx, y + 162, F_XS, C::text2, lgfx::middle_center);
  } else {
    g.text("—", cx, cy - 4, F_XL, C::text3, lgfx::middle_center);
    snprintf(b, sizeof b, "Limits: %s", a.error[0] ? a.error : "unavailable");
    g.text(b, cx, y + 162, F_XS, C::warn, lgfx::middle_center);
  }

  // Right: session, first per-model limit, today.
  const int rx = x + 136, rw = w - 148;
  meter(g, rx, y + 46, rw, "SESSION · 5H", a.session, now, e);
  if (a.scoped[0].present) {
    char t[24];
    snprintf(t, sizeof t, "%s · 7D", a.scoped[0].label);
    for (char* p = t; *p; ++p)
      if (*p >= 'a' && *p <= 'z') *p -= 32;
    meter(g, rx, y + 92, rw, t, a.scoped[0], now, e);
  } else {
    g.text("API VALUE · 7D", rx, y + 92, F_XS, C::text3);
    snprintf(b, sizeof b, "$%.2f", a.cost_week * e);
    g.text(b, rx, y + 110, F_MD, C::text);
  }
  g.text("TODAY", rx, y + 138, F_XS, C::text3);
  fmt_tokens(v, sizeof v, (double)a.tokens_today * e);
  g.text(v, rx, y + 156, F_MD, C::text);
  snprintf(b, sizeof b, "$%.2f value", a.cost_today * e);
  g.text(b, rx, y + 171, F_XS, C::text2);
  mini_bars(g, rx + 84, y + 172, rw - 84, 34, a.daily, e, C::card);
}

static void account_card(G& g, int i, int x, int y, int w, int h, bool tall, int64_t now, float e) {
  const Account& a = s_data->accounts[i];
  const bool sel = i == s_sel;
  const uint32_t bg = sel ? C::card_hi : C::card;
  if (sel) g.rrect(x - 1, y - 1, w + 2, h + 2, 11, C::accent);
  g.rrect(x, y, w, h, 10, bg);
  avatar(g, x + 20, y + 28, 13, i, a.name);
  const int tx = x + 42;
  g.text(a.name, tx, y + 14, F_MD, C::text);
  pill(g, tx + g.width(a.name, F_MD) + 7, y + 15, a.plan, C::accent, mix(bg, C::accent, 0.2f));

  if (!a.ok) {
    g.text(a.error[0] ? a.error : "Error", tx, y + 38, F_SM, C::bad);
    return;
  }
  char b[40], v[16];
  if (tall) {  // extra row: today's usage and the week at a glance
    g.fill(tx, y + 56, w - (tx - x) - 10, 1, C::line);
    fmt_tokens(v, sizeof v, (double)a.tokens_today * e);
    if (a.is_api()) snprintf(b, sizeof b, "Today %s tokens", v);
    else snprintf(b, sizeof b, "Today %s · $%.2f value", v, a.cost_today * e);
    g.text(b, tx, y + 71, F_XS, C::text2);
    mini_bars(g, x + w - 80, y + 78, 70, 16, a.daily, e, bg);
  }
  if (a.is_api()) {
    api_card(g, a, x, y, w, tx, e);
    return;
  }
  if (!a.weekly.present && !a.session.present) {  // limits temporarily unavailable
    snprintf(b, sizeof b, "Limits: %s, retrying", a.error[0] ? a.error : "unavailable");
    g.text(b, tx, y + 38, F_SM, C::warn);
    return;
  }
  if (a.weekly.present) {
    pct_str(b, sizeof b, a.weekly.pct * e);
    g.text(b, x + w - 10, y + 15, F_LG, level_color(a.weekly.pct), lgfx::middle_right);
    g.text("week", x + w - 12 - g.width(b, F_LG) - 4, y + 16, F_XS, C::text3, lgfx::middle_right);
  }
  // Two mini meters: 5h session and 7d weekly.
  struct M { const char* lbl; const Limit* l; int x0, bw; } ms[2] = {
      {"5h", &a.session, tx, 108}, {"7d", &a.weekly, tx + 136, 108}};
  for (auto& m : ms) {
    g.text(m.lbl, m.x0, y + 32, F_XS, C::text3);
    int bx = m.x0 + 17;
    g.bar(bx, y + 29, m.bw - 17, 6, m.l->present ? m.l->pct * e : 0,
          m.l->present ? level_color(m.l->pct) : C::track);
    if (m.l->present && m.l->resets_at) {
      char d[16];
      if (m.l == &a.session) {
        fmt_dur(d, sizeof d, m.l->resets_at - now);
        snprintf(b, sizeof b, "resets in %s", d);
      } else {
        fmt_when(b, sizeof b, m.l->resets_at, now);
      }
      g.text(b, bx, y + 45, F_XS, C::text2);
    }
  }
}

static void draw_overview(G& g, int64_t now, float e) {
  spark(g, 17, 15, 8, C::accent);
  g.text("Claude", 32, 15, F_LG, C::text);
  g.text("Usage", 32 + g.width("Claude", F_LG) + 5, 15, F_LG, C::text3);
  clock_status(g, now);

  const int n = s_data->count;
  if (!s_data->received) {
    draw_waiting(g);
    return;
  }
  if (n == 0) {
    g.text("No accounts configured", 160, 112, F_MD, C::text2, lgfx::middle_center);
    g.text("Log in with Claude Code, or add an API key in the bridge", 160, 132, F_XS, C::text3,
           lgfx::middle_center);
    return;
  }
  if (n == 1) {
    hero_card(g, s_data->accounts[0], now, e);
    footer(g, "Details");
    return;
  }
  // Two accounts get taller cards with an extra row; three or more scroll.
  const bool tall = n == 2;
  const int h = tall ? 86 : 56, pitch = h + (tall ? 8 : 5);
  int first = s_sel - 1;
  if (first > n - 3) first = n - 3;
  if (first < 0) first = 0;
  for (int k = 0; k < 3 && first + k < n; ++k)
    account_card(g, first + k, 8, 34 + k * pitch, 304, h, tall, now, e);
  if (n > 3) {  // scroll indicator
    int th = 180, ty = 34, kh = th * 3 / n;
    int ky = ty + (th - kh) * first / (n - 3);
    g.rrect(314, ty, 3, th, 1, C::card);
    g.rrect(314, ky, 3, kh, 1, C::text3);
  }
  footer(g, "Details");
}

// --------------------------------------------------------------- limits ---

static void limit_row(G& g, int y, const char* title, const Limit& l, int64_t now, float e) {
  const int x = 156, w = 156, h = 58;
  g.rrect(x, y, w, h, 10, C::card);
  g.text(title, x + 10, y + 13, F_XS, C::text3);
  if (l.present && l.resets_at <= now) {
    g.text("0%", x + w - 10, y + 14, F_LG, C::ok, lgfx::middle_right);
    g.bar(x + 10, y + 27, w - 20, 7, 0, C::track);
    g.text("Reset, waiting for update", x + 10, y + 45, F_XS, C::text2);
    return;
  }
  if (!l.present) {
    g.text("—", x + w - 10, y + 14, F_LG, C::text3, lgfx::middle_right);
    g.bar(x + 10, y + 27, w - 20, 7, 0, C::track);
    g.text("Not on this plan", x + 10, y + 45, F_XS, C::text3);
    return;
  }
  char b[32], d[16];
  pct_str(b, sizeof b, l.pct * e);
  uint32_t c = level_color(l.pct);
  g.text(b, x + w - 10, y + 14, F_LG, c, lgfx::middle_right);
  g.bar(x + 10, y + 27, w - 20, 7, l.pct * e, c);
  fmt_dur(d, sizeof d, l.resets_at - now);
  snprintf(b, sizeof b, "Resets in %s", d);
  g.text(b, x + 10, y + 45, F_XS, C::text2);
}

// What this usage would cost at API prices; fills limit slots the plan doesn't use.
static void value_row(G& g, int y, bool first, const Account& a, float e) {
  const int x = 156, w = 156, h = 58;
  g.rrect(x, y, w, h, 10, C::card);
  char b[24];
  if (first) {
    g.text("API VALUE · 7D", x + 10, y + 13, F_XS, C::text3);
    snprintf(b, sizeof b, "$%.2f", a.cost_week * e);
    g.text(b, x + 10, y + 33, F_LG, C::text);
    g.text("at API list prices", x + 10, y + 49, F_XS, C::text3);
  } else {
    g.text("API VALUE · TODAY", x + 10, y + 13, F_XS, C::text3);
    snprintf(b, sizeof b, "$%.2f", a.cost_today * e);
    g.text(b, x + 10, y + 33, F_LG, C::text);
    char v[16];
    fmt_tokens(v, sizeof v, (double)a.tokens_today * e);
    snprintf(b, sizeof b, "%s tokens", v);
    g.text(b, x + 10, y + 49, F_XS, C::text3);
  }
}

static void draw_limits(G& g, const Account& a, int64_t now, float e) {
  account_header(g, a, s_sel, V_LIMITS);

  // Hero: weekly ring.
  const int cx = 78, cy = 100;
  g.rrect(8, 34, 140, 180, 10, C::card);
  const Limit& wk = a.weekly;
  uint32_t c = wk.present ? level_color(wk.pct) : C::track;
  g.ring(cx, cy, 52, 9, wk.present ? wk.pct * e : 0, c, C::track, C::card);
  char num[8], b[24], d[16];
  snprintf(num, sizeof num, "%d", (int)(wk.pct * e + 0.5f));
  int nw = g.width(num, F_HERO), pw = g.width("%", F_MD);
  int x0 = cx - (nw + pw + 1) / 2;
  g.text(wk.present ? num : "—", x0, cy - 3, F_HERO, C::text, lgfx::middle_left);
  if (wk.present) g.text("%", x0 + nw + 1, cy - 9, F_MD, C::text2, lgfx::middle_left);
  g.text("WEEKLY", cx, cy + 22, F_XS, C::text3, lgfx::middle_center);
  if (wk.present) {
    g.text("Resets", cx, 168, F_XS, C::text3, lgfx::middle_center);
    fmt_when(b, sizeof b, wk.resets_at, now);
    g.text(b, cx, 184, F_MD, C::text, lgfx::middle_center);
    fmt_dur(d, sizeof d, wk.resets_at - now);
    snprintf(b, sizeof b, "in %s", d);
    g.text(b, cx, 200, F_XS, C::text2, lgfx::middle_center);
  }

  limit_row(g, 34, "SESSION · 5H", a.session, now, e);
  // Per-model weekly limits carry whatever model names Claude reports.
  const int ys[2] = {95, 156};
  for (int i = 0; i < 2; ++i) {
    if (a.scoped[i].present) {
      char t[24];
      snprintf(t, sizeof t, "%s · 7D", a.scoped[i].label);
      for (char* p = t; *p; ++p)
        if (*p >= 'a' && *p <= 'z') *p -= 32;
      limit_row(g, ys[i], t, a.scoped[i], now, e);
    } else {
      value_row(g, ys[i], i == a.scoped_count(), a, e);
    }
  }
  footer(g, "Tokens");
}

// --------------------------------------------------------------- tokens ---

static void kpi(G& g, int x, const char* label, const char* value) {
  g.rrect(x, 34, 98, 54, 10, C::card);
  g.text(label, x + 10, 46, F_XS, C::text3);
  g.text(value, x + 9, 68, F_XL, C::text);
}

static void draw_tokens(G& g, const Account& a, int64_t now, float e) {
  account_header(g, a, s_sel, V_TOKENS);
  char b1[16], b2[16], b3[16];
  fmt_tokens(b1, sizeof b1, a.tokens_today * e);
  uint64_t wk = 0;
  for (uint64_t v : a.daily) wk += v;
  fmt_tokens(b2, sizeof b2, a.week.total() * e);
  snprintf(b3, sizeof b3, a.cost_week < 10 ? "$%.2f" : a.cost_week < 100 ? "$%.1f" : "$%.0f",
           a.cost_week * e);
  kpi(g, 8, "TODAY", b1);
  kpi(g, 111, "7 DAYS", b2);
  kpi(g, 214, a.is_api() ? "COST · 7D" : "API VALUE", b3);

  // Daily bars.
  const int cy0 = 94, ch = 120;
  g.rrect(8, cy0, 304, ch, 10, C::card);
  g.text("Tokens per day", 18, cy0 + 13, F_XS, C::text3);
  uint64_t mx = 1;
  for (uint64_t v : a.daily) mx = v > mx ? v : mx;
  const int base = cy0 + 96, top = cy0 + 30, span = base - top;
  const float scale = span / (mx * 1.08f);

  const double avg = wk / 7.0;
  char b[24], v[16];
  fmt_tokens(v, sizeof v, avg);
  snprintf(b, sizeof b, "avg %s", v);
  g.text(b, 302, cy0 + 13, F_XS, C::text2, lgfx::middle_right);
  int ay = base - (int)(avg * scale * e);
  for (int x = 18; x < 302; x += 6) g.fill(x, ay, 3, 1, C::text3);

  struct tm tm;
  local_tm(now, &tm);
  for (int i = 0; i < 7; ++i) {
    const int cx = 18 + 20 + i * 40;
    const bool today = i == 6;
    int bh = (int)(a.daily[i] * scale * e);
    if (a.daily[i] && bh < 3) bh = 3;
    if (bh > 0) g.rrect(cx - 11, base - bh, 22, bh, 4, today ? C::accent : mix(C::card, C::accent, 0.42f));
    int wd = ((tm.tm_wday - (6 - i)) % 7 + 7) % 7;
    g.text(kWdayLetter[wd], cx, base + 11, F_XS, today ? C::text : C::text3, lgfx::middle_center);
    if (today) {
      fmt_tokens(v, sizeof v, a.daily[i] * e);
      g.text(v, cx, base - bh - 8, F_XS, C::text, lgfx::middle_center);
    }
  }
  g.fill(18, base, 284, 1, C::line);
  footer(g, "Breakdown");
}

// ------------------------------------------------------------ breakdown ---

static void draw_breakdown(G& g, const Account& a, int64_t now, float e) {
  (void)now;
  account_header(g, a, s_sel, V_BREAKDOWN);
  const cu::Tokens& t = a.week;
  const double total = (double)t.total();
  char b[24], v[16];

  g.rrect(8, 34, 304, 104, 10, C::card);
  g.text("TOKENS · 7 DAYS", 18, 47, F_XS, C::text3);
  fmt_tokens(v, sizeof v, total * e);
  g.text(v, 302, 47, F_MD, C::text, lgfx::middle_right);

  struct Seg { const char* name; uint64_t val; uint32_t c; } segs[4] = {
      {"Input", t.input, C::c_in},
      {"Output", t.output, C::c_out},
      {"Cache read", t.cache_read, C::c_cr},
      {"Cache write", t.cache_write, C::c_cw}};

  // Stacked bar: draw cumulative rounded bars from widest to narrowest.
  const int bx = 18, bw = 284, by = 60;
  g.rrect(bx, by, bw, 9, 4, C::track);
  if (total > 0) {
    double cum[4], acc = 0;
    for (int i = 0; i < 4; ++i) cum[i] = (acc += segs[i].val);
    for (int i = 3; i >= 0; --i) {
      int w = (int)(bw * cum[i] / total * e + 0.5);
      if (w >= 9) g.rrect(bx, by, w, 9, 4, segs[i].c);
    }
  }
  for (int i = 0; i < 4; ++i) {
    int y = 84 + i * 15;
    g.circle(22, y, 3, segs[i].c);
    g.text(segs[i].name, 32, y, F_SM, C::text2);
    fmt_tokens(v, sizeof v, segs[i].val * e);
    g.text(v, 250, y, F_SM, C::text, lgfx::middle_right);
    snprintf(b, sizeof b, "%.0f%%", total > 0 ? segs[i].val * 100.0 / total : 0.0);
    g.text(b, 302, y, F_XS, C::text3, lgfx::middle_right);
  }

  g.rrect(8, 144, 304, 70, 10, C::card);
  g.text("MODELS", 18, 156, F_XS, C::text3);
  const uint32_t mc[3] = {C::accent, C::c_in, C::c_cr};
  if (a.model_count == 0) g.text("No data", 18, 182, F_SM, C::text3);
  for (int i = 0; i < a.model_count && i < 3; ++i) {
    int y = 172 + i * 16;
    g.text(a.models[i].name, 18, y, F_SM, C::text2);
    g.bar(80, y - 3, 180, 6, a.models[i].pct * e, mc[i]);
    snprintf(b, sizeof b, "%d%%", (int)(a.models[i].pct * e + 0.5f));
    g.text(b, 302, y, F_SM, C::text, lgfx::middle_right);
  }
  footer(g, "Home");
}

// -------------------------------------------------------------- overlay ---

static void draw_ask(G& g) {
  g.fill(0, 0, 320, 240, C::bg);
  g.rrect(16, 26, 288, 176, 14, C::card);
  spark(g, 160, 62, 14, C::accent);
  g.text(s_ask_title, 160, 98, F_XS, C::text3, lgfx::middle_center);
  g.text(s_ask_subject, 160, 122, F_LG, C::text, lgfx::middle_center);
  g.text(s_ask_question, 160, 152, F_SM, C::text2, lgfx::middle_center);
  g.text("Approve only computers you own.", 160, 176, F_XS, C::text3, lgfx::middle_center);
  const int y = 229;
  g.rrect(66 - 30, y - 9, 60, 19, 9, C::card_hi);
  g.text("Deny", 66, y, F_XS, C::text2, lgfx::middle_center);
  g.rrect(254 - 30, y - 9, 60, 19, 9, C::accent);
  g.text("Allow", 254, y, F_XS, C::bg, lgfx::middle_center);
}

static void draw_toast(G& g) {
  int w = g.width(s_toast, F_SM) + 30;
  int x = 160 - w / 2, y = 186;
  g.rrect(x - 1, y - 1, w + 2, 24, 12, C::line);
  g.rrect(x, y, w, 22, 11, C::card_hi);
  g.circle(x + 12, y + 11, 3, s_refreshing ? C::warn : C::ok);
  g.text(s_toast, x + 20, y + 11, F_SM, C::text);
}

// --------------------------------------------------------------- render ---

static void draw_all(G& g, int view, int64_t now, float e, bool with_toast) {
  if (!s_data) return;
  if (view == V_OVERVIEW || s_data->count == 0) {
    draw_overview(g, now, e);
  } else {
    const Account& a = s_data->accounts[s_sel];
    if (view == V_LIMITS && !a.is_api()) draw_limits(g, a, now, e);
    else if (view == V_TOKENS) draw_tokens(g, a, now, e);
    else draw_breakdown(g, a, now, e);
  }
  if (s_ask) draw_ask(g);
  if (with_toast) draw_toast(g);
}

static void alloc_bands() {
  if (s_bands) return;
  for (auto& b : s_band) {
    b.setColorDepth(16);
    if (!b.createSprite(W, kBandH)) break;
    ++s_bands;
  }
}

static void render(lgfx::LovyanGFX* dst, int view, int64_t now, float e, bool with_toast) {
  alloc_bands();
  if (!s_bands) return;
  dst->startWrite();
  for (int y = 0, k = 0; y < H; y += kBandH, k = (k + 1) % s_bands) {
    LGFX_Sprite& band = s_band[k];
    // With one buffer, the previous DMA transfer must finish before it is redrawn.
    if (s_bands == 1) dst->waitDMA();
    band.fillScreen(C::bg);
    G g{&band, y, kBandH};
    draw_all(g, view, now, e, with_toast);
    dst->waitDMA();  // the other buffer's transfer; this one stays untouched until its turn
    band.pushSprite(dst, 0, y);
  }
  dst->waitDMA();
  dst->endWrite();
}

void render_to(lgfx::LovyanGFX* dst, int view, int sel, int64_t now, float anim) {
  s_sel = sel;
  s_view = view;
  render(dst, view, now, ease(anim), false);
}

// ---------------------------------------------------------------- input ---

struct Btn {
  int pin;
  bool down = false;
  bool long_fired = false;
  uint32_t t0 = 0;
};
enum Ev { EV_NONE, EV_CLICK, EV_LONG };
static Btn s_btn[3] = {{kPinA}, {kPinB}, {kPinC}};

static Ev poll(Btn& b, uint32_t ms) {
  bool pressed = !lgfx::gpio_in(b.pin);
  Ev ev = EV_NONE;
  if (pressed && !b.down) {
    b.down = true;
    b.long_fired = false;
    b.t0 = ms;
  } else if (pressed && b.down && !b.long_fired && ms - b.t0 >= kLongMs) {
    b.long_fired = true;
    ev = EV_LONG;
  } else if (!pressed && b.down) {
    b.down = false;
    if (!b.long_fired && ms - b.t0 >= 25) ev = EV_CLICK;
  }
  return ev;
}

static void restart_anim() {
  s_anim_start = s_ms;
  s_anim = 0;
  s_dirty = true;
}

// API accounts have no plan limits: their Limits page is skipped.
static int fix_view(int view) {
  if (view == V_LIMITS && s_data && s_sel < s_data->count && s_data->accounts[s_sel].is_api())
    return V_TOKENS;
  return view;
}

static void handle_input(uint32_t ms) {
  Ev a = poll(s_btn[0], ms), b = poll(s_btn[1], ms), c = poll(s_btn[2], ms);
  if (s_ask) {
    if (a == EV_CLICK || c == EV_CLICK) {
      s_answer = c == EV_CLICK ? 1 : 0;
      s_ask = false;
      s_dirty = true;
    }
    return;
  }
  const int n = s_data && s_data->received ? s_data->count : 0;
  if (n > 0 && (a == EV_CLICK || c == EV_CLICK)) {
    s_sel = (s_sel + (c == EV_CLICK ? 1 : n - 1)) % n;
    s_view = fix_view(s_view);
    if (s_view == V_OVERVIEW) s_dirty = true;
    else restart_anim();
  }
  if (b == EV_CLICK && n > 0) {
    s_view = fix_view((s_view + 1) % V_COUNT);
    restart_anim();
  }
  if (b == EV_LONG) {
    s_refresh_req = true;
    s_refreshing = true;
    toast("Refreshing…", 15000);
  }
}

// ------------------------------------------------------------------ api ---

void begin(lgfx::LovyanGFX* lcd, const cu::Dashboard* data) {
  s_lcd = lcd;
  s_data = data;
  s_xs.load(font_xs_vlw);
  s_sm.load(font_sm_vlw);
  s_md.load(font_md_vlw);
  s_lg.load(font_lg_vlw);
  s_xl.load(font_xl_vlw);
  s_hero.load(font_hero_vlw);
  for (auto& b : s_btn) lgfx::pinMode(b.pin, lgfx::pin_mode_t::input);
  alloc_bands();
  restart_anim();
}

void loop(uint32_t ms, int64_t now) {
  s_ms = ms;
  handle_input(ms);
  bool toast_on = s_toast[0] && ms < s_toast_until;
  if (s_toast[0] && !toast_on) {
    s_toast[0] = 0;
    s_dirty = true;
  }
  if (s_anim < 1) {
    s_anim = (ms - s_anim_start) / (float)kAnimMs;
    if (s_anim > 1) s_anim = 1;
    s_dirty = true;
  }
  if (ms - s_last_draw >= 1000) s_dirty = true;  // clock & countdowns
  if (s_data && !s_data->received && ms - s_last_draw >= 90) s_dirty = true;  // waiting pulse
  if (!s_dirty) return;
  s_dirty = false;
  s_last_draw = ms;
  render(s_lcd, s_view, now, ease(s_anim), toast_on);
}

bool take_refresh_request() {
  bool r = s_refresh_req;
  s_refresh_req = false;
  return r;
}

void data_updated(bool first) {
  if (s_data && s_sel >= s_data->count) s_sel = 0;
  s_view = fix_view(s_view);
  if (s_refreshing) {
    s_refreshing = false;
    toast("Data updated");
  }
  if (first) restart_anim();
  s_dirty = true;
}

void refresh_failed(const char* why) {
  s_refreshing = false;
  toast(why);
}

void ask(const char* title, const char* subject, const char* question) {
  snprintf(s_ask_title, sizeof s_ask_title, "%s", title);
  snprintf(s_ask_subject, sizeof s_ask_subject, "%s", subject);
  snprintf(s_ask_question, sizeof s_ask_question, "%s", question);
  s_ask = true;
  s_answer = -1;
  s_dirty = true;
}

void ask_cancel() {
  s_ask = false;
  s_dirty = true;
}

int take_answer() {
  int a = s_answer;
  s_answer = -1;
  return a;
}

void toast(const char* msg, uint32_t duration_ms) {
  snprintf(s_toast, sizeof s_toast, "%s", msg);
  s_toast_until = s_ms + duration_ms;
  s_dirty = true;
}

}  // namespace ui
