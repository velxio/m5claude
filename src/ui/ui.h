// Claude usage dashboard UI for a 320x240 M5Stack Core (buttons A/B/C).
// Platform-agnostic: draws into any LovyanGFX target and reads the three buttons
// through lgfx::gpio_in, so the same code can run on other hosts.
#pragma once
#include <LovyanGFX.hpp>

#include "../model/model.h"
#include "i18n.h"

namespace ui {

enum View { V_OVERVIEW, V_LIMITS, V_TOKENS, V_BREAKDOWN, V_COUNT };

// Physical buttons on the M5Stack Core (active low).
constexpr int kPinA = 39, kPinB = 38, kPinC = 37;

void begin(lgfx::LovyanGFX* lcd, const cu::Dashboard* data);

// Call often. Polls buttons, runs animations and redraws when needed.
// `now` is wall-clock epoch seconds (0 if unknown yet).
void loop(uint32_t ms, int64_t now);

// Long-press B asks for a refresh; the app forwards it to the bridge.
bool take_refresh_request();
// A new snapshot was stored in the dashboard (`first`: the first one since boot).
void data_updated(bool first);
void refresh_failed(const char* why);

void toast(const char* msg, uint32_t duration_ms = 1600);

// User settings, persisted by the board (NVS) through Hooks::save.
struct Settings {
  uint8_t lang = LANG_EN;
  uint8_t brightness = 2;  // 0..3 → 25/50/75/100 %
  uint8_t sounds = 1;      // 0 off, 1 alerts, 2 alerts + clicks
  uint8_t alert_at = 1;    // 0 → 80 %, 1 → 90 %, 2 → off
  uint8_t screen_off = 1;  // on battery: 0 never, 1 after 1 min, 2 after 5 min
  uint8_t night = 0;       // dim 22:00–07:00
};

enum Sound : uint8_t { SND_CLICK, SND_ALERT, SND_RESET, SND_PAIR };

// What the UI needs from the board. All optional.
struct Hooks {
  void (*brightness)(uint8_t level) = nullptr;  // 0 = backlight off
  void (*sound)(Sound s) = nullptr;
  void (*save)(const Settings& s) = nullptr;
  int (*paired_count)() = nullptr;
  void (*forget_pcs)() = nullptr;
  const char* device_id = "";
  const char* version = "";
};

void set_hooks(const Hooks& h);
void apply_settings(const Settings& s);
const Settings& settings();
const Strings& tr();  // strings in the current language

// After a new snapshot: alerts for limits crossing the threshold, chimes for resets.
void check_alerts(const cu::Dashboard& before);
void wake();  // turn the screen back on (alerts, pairing)
bool screen_is_off();  // backlight off (idle on battery): the board may go to standby
void show_settings(bool open);
void play_sound(Sound s);

// Battery state from the board: level 0..100 (-1 hides the icon), USB power present,
// charge complete.
void set_battery(int level, bool plugged, bool full);

// Modal yes/no question (A = no, C = yes). take_answer(): -1 pending, 0 no, 1 yes.
void ask(const char* title, const char* subject, const char* question);
void ask_cancel();
int take_answer();

// Render a full frame for a given state to any target (used for screenshots).
void render_to(lgfx::LovyanGFX* dst, int view, int sel, int64_t now, float anim = 1.0f);

}  // namespace ui
