// Claude usage dashboard UI for a 320x240 M5Stack Core (buttons A/B/C).
// Platform-agnostic: draws into any LovyanGFX target and reads the three buttons
// through lgfx::gpio_in, so the same code can run on other hosts.
#pragma once
#include <LovyanGFX.hpp>

#include "../model/model.h"

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

// Modal yes/no question (A = no, C = yes). take_answer(): -1 pending, 0 no, 1 yes.
void ask(const char* title, const char* subject, const char* question);
void ask_cancel();
int take_answer();

// Render a full frame for a given state to any target (used for screenshots).
void render_to(lgfx::LovyanGFX* dst, int view, int sel, int64_t now, float anim = 1.0f);

}  // namespace ui
