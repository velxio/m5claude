// Data model shared by the UI, the mock generator and the link parser.
// Mirrors the bridge's wire format (see bridge/m5claude_bridge/snapshot.py).
#pragma once
#include <stdint.h>

namespace cu {

// A rolling usage limit as reported by Claude (utilization 0..100 + reset time).
struct Limit {
  bool present = false;
  char label[12] = {};    // "Session", "Weekly", or a model name for scoped limits
  float pct = 0;          // 0..100
  int64_t resets_at = 0;  // unix epoch seconds
};

struct Tokens {
  uint64_t input = 0, output = 0, cache_read = 0, cache_write = 0;
  uint64_t total() const { return input + output + cache_read + cache_write; }
};

struct ModelShare {
  char name[12] = {};
  float pct = 0;  // share of the week's tokens, 0..100
};

enum class Kind : uint8_t { Subscription, Api };

constexpr int kMaxScoped = 2;

struct Account {
  char name[24] = {};
  char plan[16] = {};     // "Max 20x", "Pro", "API"...
  Kind kind = Kind::Subscription;
  bool ok = true;
  char error[32] = {};    // shown when !ok ("Login expired")

  Limit session;          // 5-hour window       (subscriptions)
  Limit weekly;           // 7-day, all models   (subscriptions)
  Limit scoped[kMaxScoped];  // per-model weekly limits

  uint64_t tokens_today = 0;
  Tokens week;
  float cost_week = 0;    // USD; an API-price estimate for subscriptions
  float cost_today = 0;
  uint64_t daily[7] = {}; // daily[6] = today
  ModelShare models[3];
  int model_count = 0;

  bool is_api() const { return kind == Kind::Api; }
  int scoped_count() const {
    int n = 0;
    while (n < kMaxScoped && scoped[n].present) ++n;
    return n;
  }
};

constexpr int kMaxAccounts = 6;

struct Dashboard {
  Account accounts[kMaxAccounts];
  int count = 0;
  int64_t fetched_at = 0;  // bridge timestamp of the last snapshot (epoch s)
  int32_t tz_offset = 0;   // bridge's local UTC offset in seconds (for clock/reset times)
  bool online = false;     // a bridge is connected
  bool received = false;   // at least one snapshot arrived
};

// Demo data so the UI can be developed without accounts/network.
void mock_fill(Dashboard& d, int64_t now);
// Small drift over time, for demos.
void mock_tick(Dashboard& d, int64_t now);

// Parse one snapshot line from the bridge into `d`. Returns false on bad input
// (in which case `d` is left untouched).
bool parse_snapshot(const char* json, Dashboard& d);

}  // namespace cu
