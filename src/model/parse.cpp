#include <ArduinoJson.h>
#include <string.h>

#include "model.h"

namespace cu {

static void copy(char* dst, size_t n, const char* src) {
  strncpy(dst, src ? src : "", n - 1);
  dst[n - 1] = 0;
}

static void limit(Limit& l, JsonArrayConst a, const char* label) {
  if (a.isNull() || a.size() < 2) return;
  l.present = true;
  copy(l.label, sizeof l.label, label);
  l.pct = a[0] | 0.0f;
  l.resets_at = a[1] | (int64_t)0;
}

bool parse_snapshot(const char* json, Dashboard& d) {
  JsonDocument doc;
  if (deserializeJson(doc, json) || (doc["v"] | 0) != 1) return false;

  static Dashboard next;  // static: Dashboard is large for an ESP32 task stack
  next = Dashboard{};
  next.fetched_at = doc["ts"] | (int64_t)0;
  next.tz_offset = doc["tz"] | 0;
  for (JsonObjectConst o : doc["acc"].as<JsonArrayConst>()) {
    if (next.count >= kMaxAccounts) break;
    Account& a = next.accounts[next.count++];
    copy(a.name, sizeof a.name, o["n"] | "?");
    copy(a.plan, sizeof a.plan, o["p"] | "");
    a.kind = strcmp(o["k"] | "sub", "api") == 0 ? Kind::Api : Kind::Subscription;
    a.ok = (o["ok"] | 1) != 0;
    copy(a.error, sizeof a.error, o["e"] | "");

    limit(a.session, o["s"], "Session");
    limit(a.weekly, o["w"], "Weekly");
    int i = 0;
    for (JsonArrayConst x : o["x"].as<JsonArrayConst>()) {
      if (i >= kMaxScoped || x.size() < 3) break;
      Limit& l = a.scoped[i++];
      l.present = true;
      copy(l.label, sizeof l.label, x[0] | "?");
      l.pct = x[1] | 0.0f;
      l.resets_at = x[2] | (int64_t)0;
    }

    a.tokens_today = o["td"] | (uint64_t)0;
    JsonArrayConst tk = o["tk"];
    if (tk.size() == 4) a.week = {tk[0] | (uint64_t)0, tk[1] | (uint64_t)0, tk[2] | (uint64_t)0, tk[3] | (uint64_t)0};
    a.cost_week = o["c"] | 0.0f;
    a.cost_today = o["ct"] | 0.0f;
    i = 0;
    for (JsonVariantConst v : o["d"].as<JsonArrayConst>()) {
      if (i >= 7) break;
      a.daily[i++] = v | (uint64_t)0;
    }
    for (JsonArrayConst m : o["m"].as<JsonArrayConst>()) {
      if (a.model_count >= 3 || m.size() < 2) break;
      ModelShare& s = a.models[a.model_count++];
      copy(s.name, sizeof s.name, m[0] | "?");
      s.pct = m[1] | 0.0f;
    }
  }
  next.online = true;
  next.received = true;
  d = next;
  return true;
}

}  // namespace cu
