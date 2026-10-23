#include "app.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include <ArduinoJson.h>

#include "../ui/ui.h"

namespace app {

constexpr uint32_t kHelloEveryMs = 2000;      // until the bridge answers
constexpr uint32_t kOfflineAfterMs = 150000;  // bridge pushes every ~60 s
constexpr size_t kMaxLine = 4096;

enum class Pair : uint8_t { None, Pending, Asking, Ok };

struct Link {
  bool up = false;
  bool trusted = false;
  Pair pair = Pair::None;
  char pc[32] = {};
  char pcid[40] = {};
  char line[kMaxLine];
  size_t len = 0;
  bool overflow = false;
  uint32_t hello_ms = 0;
  uint32_t rx_ms = 0;  // last snapshot on this link
  bool got_data = false;
};

static cu::Dashboard s_dash;
static Platform s_p;
static char s_id[24] = "m5claude";
static bool s_demo = false;
static Link s_links[kMaxLinks];
static int s_asking = -1;  // link whose PC is being approved on screen

static uint32_t s_ms = 0;
static uint32_t s_rx_ms = 0;    // when the last snapshot arrived (any link)
static int64_t s_rx_epoch = 0;  // its timestamp

const cu::Dashboard& dashboard() { return s_dash; }

static void send(int link, const char* s) {
  if (s_p.write && s_links[link].up) s_p.write(link, s, strlen(s));
}

static void hello(int link) {
  char b[96];
  snprintf(b, sizeof b, "{\"hello\":\"m5claude\",\"id\":\"%s\",\"fw\":\"%s\"}\n", s_id, kFirmwareVersion);
  send(link, b);
  s_links[link].hello_ms = s_ms;
}

static bool accepted(const Link& l) { return l.trusted || l.pair == Pair::Ok; }

static void on_identity(int link, const char* pc, const char* pcid) {
  Link& l = s_links[link];
  snprintf(l.pc, sizeof l.pc, "%s", pc);
  snprintf(l.pcid, sizeof l.pcid, "%s", pcid);
  if (l.trusted) return;
  if (pcid[0] && s_p.is_trusted_pc && s_p.is_trusted_pc(pcid)) {
    l.pair = Pair::Ok;
    send(link, "{\"pair\":\"ok\"}\n");
  } else {
    l.pair = Pair::Pending;
    send(link, "{\"pair\":\"pending\"}\n");
  }
}

static void on_line(int link, const char* line) {
  if (line[0] != '{') return;
  Link& l = s_links[link];
  if (strstr(line, "\"pcid\"") && !strstr(line, "\"acc\"")) {
    JsonDocument doc;
    if (!deserializeJson(doc, line)) on_identity(link, doc["pc"] | "PC", doc["pcid"] | "");
    return;
  }
  if (!accepted(l)) return;  // snapshots from an unapproved PC are ignored
  const bool first = !s_dash.received;
  static cu::Dashboard before;  // static: too big for the loop task's stack
  before = s_dash;
  if (!cu::parse_snapshot(line, s_dash)) return;
  s_demo = false;
  l.got_data = true;
  l.rx_ms = s_rx_ms = s_ms;
  s_rx_epoch = s_dash.fetched_at;
  ui::data_updated(first);
  if (!first) ui::check_alerts(before);
}

void begin(lgfx::LovyanGFX* lcd, const char* device_id, const Platform& p, bool demo) {
  s_p = p;
  snprintf(s_id, sizeof s_id, "%s", device_id);
  s_demo = demo;
  ui::begin(lcd, &s_dash);
}

void link_up(int link, bool trusted) {
  Link& l = s_links[link];
  l = Link{};
  l.up = true;
  l.trusted = trusted;
  hello(link);
}

void link_down(int link) {
  s_links[link].up = false;
  if (s_asking == link) {
    s_asking = -1;
    ui::ask_cancel();
  }
}

void link_rx(int link, const char* data, size_t len) {
  Link& l = s_links[link];
  for (size_t i = 0; i < len; ++i) {
    char c = data[i];
    if (c == '\n') {
      if (!l.overflow && l.len) {
        l.line[l.len] = 0;
        on_line(link, l.line);
      }
      l.len = 0;
      l.overflow = false;
    } else if (c != '\r') {
      if (l.len < kMaxLine - 1) l.line[l.len++] = c;
      else l.overflow = true;
    }
  }
}

static void pairing() {
  if (s_asking < 0) {
    for (int i = 0; i < kMaxLinks; ++i) {
      if (s_links[i].up && s_links[i].pair == Pair::Pending) {
        s_asking = i;
        s_links[i].pair = Pair::Asking;
        ui::ask(ui::tr().new_computer, s_links[i].pc, ui::tr().allow_question);
        ui::play_sound(ui::SND_PAIR);
        break;
      }
    }
    return;
  }
  int answer = ui::take_answer();
  if (answer < 0) return;
  Link& l = s_links[s_asking];
  if (answer == 1) {
    l.pair = Pair::Ok;
    if (s_p.trust_pc && l.pcid[0]) s_p.trust_pc(l.pcid);
    send(s_asking, "{\"pair\":\"ok\"}\n");
    send(s_asking, "{\"req\":\"refresh\"}\n");
    ui::toast(ui::tr().paired);
  } else {
    send(s_asking, "{\"pair\":\"denied\"}\n");
    l.pair = Pair::None;
    if (s_p.drop) s_p.drop(s_asking);
  }
  s_asking = -1;
}

void loop(uint32_t ms, int64_t host_epoch) {
  s_ms = ms;
  if (s_demo && !s_dash.received && host_epoch) {  // demo data needs a real clock
    cu::mock_fill(s_dash, host_epoch);
    time_t t = (time_t)host_epoch;
    struct tm utc = *gmtime(&t);
    utc.tm_isdst = -1;
    s_dash.tz_offset = (int32_t)(t - mktime(&utc));
    ui::data_updated(true);
  }

  bool any_fresh = false;
  for (int i = 0; i < kMaxLinks; ++i) {
    Link& l = s_links[i];
    if (!l.up) continue;
    const bool fresh = l.got_data && ms - l.rx_ms < kOfflineAfterMs;
    any_fresh |= fresh;
    // Keep greeting until data flows, so a bridge that opens the port late still finds us.
    if (!fresh && l.pair != Pair::Asking && ms - l.hello_ms >= kHelloEveryMs) hello(i);
  }
  if (s_dash.received && !s_demo) s_dash.online = any_fresh;
  pairing();

  if (ui::take_refresh_request()) {
    bool sent = false;
    for (int i = 0; i < kMaxLinks; ++i) {
      if (s_links[i].up && accepted(s_links[i])) {
        send(i, "{\"req\":\"refresh\"}\n");
        sent = true;
      }
    }
    if (!sent) ui::refresh_failed(ui::tr().pc_not_connected);
  }

  int64_t now = host_epoch;
  if (!now && s_rx_epoch) now = s_rx_epoch + (ms - s_rx_ms) / 1000;
  ui::loop(ms, now);
}

}  // namespace app
