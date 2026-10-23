#include <string.h>

#include "model.h"

namespace cu {

static void set_str(char* dst, size_t n, const char* src) {
  strncpy(dst, src, n - 1);
  dst[n - 1] = 0;
}

static void lim(Limit& l, const char* label, float pct, int64_t resets_at) {
  l.present = true;
  set_str(l.label, sizeof l.label, label);
  l.pct = pct;
  l.resets_at = resets_at;
}

static void models(Account& a, float opus, float sonnet, float haiku) {
  const char* names[3] = {"Opus", "Sonnet", "Haiku"};
  const float pct[3] = {opus, sonnet, haiku};
  for (int i = 0; i < 3; ++i) {
    set_str(a.models[i].name, sizeof a.models[i].name, names[i]);
    a.models[i].pct = pct[i];
  }
  a.model_count = 3;
}

void mock_fill(Dashboard& d, int64_t now) {
  const int64_t H = 3600, D = 86400;
  d = Dashboard{};
  d.count = 3;
  d.online = true;
  d.received = true;
  d.fetched_at = now - 12;

  Account& a = d.accounts[0];
  set_str(a.name, sizeof a.name, "Personal");
  set_str(a.plan, sizeof a.plan, "Max 20x");
  lim(a.session, "Session", 42, now + 2 * H + 14 * 60);
  lim(a.weekly, "Weekly", 67, now + 3 * D + 4 * H);
  lim(a.scoped[0], "Opus", 54, now + 3 * D + 4 * H);
  lim(a.scoped[1], "Sonnet", 31, now + 3 * D + 4 * H);
  a.week = {2'310'000, 1'480'000, 15'900'000, 1'120'000};
  a.tokens_today = 2'840'000;
  a.cost_week = 84.20f;
  a.cost_today = 11.35f;
  const uint64_t da[7] = {1'900'000, 3'400'000, 2'600'000, 4'100'000, 3'300'000, 2'670'000, 2'840'000};
  memcpy(a.daily, da, sizeof da);
  models(a, 61, 34, 5);

  Account& b = d.accounts[1];
  set_str(b.name, sizeof b.name, "Work");
  set_str(b.plan, sizeof b.plan, "Max 5x");
  lim(b.session, "Session", 91, now + 38 * 60);
  lim(b.weekly, "Weekly", 88, now + 1 * D + 19 * H);
  lim(b.scoped[0], "Opus", 96, now + 1 * D + 19 * H);
  b.week = {3'050'000, 2'010'000, 22'400'000, 1'760'000};
  b.tokens_today = 4'920'000;
  b.cost_week = 131.75f;
  b.cost_today = 19.80f;
  const uint64_t db[7] = {3'800'000, 4'600'000, 4'200'000, 3'900'000, 5'300'000, 2'500'000, 4'920'000};
  memcpy(b.daily, db, sizeof db);
  models(b, 78, 20, 2);

  Account& c = d.accounts[2];
  set_str(c.name, sizeof c.name, "Acme API");
  set_str(c.plan, sizeof c.plan, "API");
  c.kind = Kind::Api;
  c.week = {4'100'000, 1'260'000, 9'900'000, 780'000};
  c.tokens_today = 2'120'000;
  c.cost_week = 212.40f;
  c.cost_today = 27.15f;
  const uint64_t dc[7] = {2'300'000, 1'100'000, 2'520'000, 2'610'000, 2'410'000, 2'980'000, 2'120'000};
  memcpy(c.daily, dc, sizeof dc);
  models(c, 12, 70, 18);
}

void mock_tick(Dashboard& d, int64_t now) {
  // Pretend the bridge refreshed and the active account kept working.
  Account& a = d.accounts[0];
  if (a.session.pct < 99.5f) a.session.pct += 0.5f;
  if (a.weekly.pct < 99.8f) a.weekly.pct += 0.1f;
  a.tokens_today += 18'000;
  a.daily[6] = a.tokens_today;
  a.week.cache_read += 15'000;
  a.week.output += 3'000;
  d.fetched_at = now;
}

}  // namespace cu
