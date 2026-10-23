// m5claude firmware for M5Stack Basic (ESP32, ILI9342C 320x240, buttons on GPIO 39/38/37).
//
// Two links to the PC bridge, both zero-config:
//   USB  — the cable's serial port (trusted: physical access).
//   BLE  — a GATT service the bridge finds by UUID; each new PC is approved once on screen.
// Nothing about the PC or the network is stored except the IDs of approved PCs.
//
// `src/` is a junction to the repo's shared src/ (see firmware/build.ps1).
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <driver/gpio.h>
#include <esp_sleep.h>
#include <freertos/stream_buffer.h>

#include "src/app/app.h"
#include "src/ui/ui.h"

enum { LINK_USB = 0, LINK_BLE = 1 };

// Custom 128-bit UUIDs (keep in sync with bridge/m5claude_bridge/transports/ble.py).
static const char* kSvcUuid = "8f1a0001-5c6e-4b7a-9d2e-6d35c1a0e001";
static const char* kRxUuid = "8f1a0002-5c6e-4b7a-9d2e-6d35c1a0e001";  // bridge -> device (write)
static const char* kTxUuid = "8f1a0003-5c6e-4b7a-9d2e-6d35c1a0e001";  // device -> bridge (notify)

constexpr int kMaxTrustedPcs = 4;

// Explicit M5Stack Basic display config (ILI9342C on VSPI). Autodetection probes
// several boards' pins at boot and can pick the wrong panel geometry.
class LGFX_M5StackBasic : public lgfx::LGFX_Device {
  struct Panel : public lgfx::Panel_ILI9342 {
    bool init(bool use_reset) override {
      // Newer Basic units (IPS) need inverted colors; they pull the reset line
      // differently, which is how the vendor libraries tell them apart.
      _cfg.invert = lgfx::gpio::command((const uint8_t[]){
          lgfx::gpio::command_mode_output, GPIO_NUM_33,
          lgfx::gpio::command_write_low, GPIO_NUM_33,
          lgfx::gpio::command_mode_input_pulldown, GPIO_NUM_33,
          lgfx::gpio::command_write_high, GPIO_NUM_33,
          lgfx::gpio::command_read, GPIO_NUM_33,
          lgfx::gpio::command_mode_output, GPIO_NUM_33,
          lgfx::gpio::command_end});
      return lgfx::Panel_ILI9342::init(use_reset);
    }
  };
  Panel panel_;
  lgfx::Bus_SPI bus_;
  lgfx::Light_PWM light_;

 public:
  LGFX_M5StackBasic() {
    auto b = bus_.config();
    b.spi_host = VSPI_HOST;
    b.spi_mode = 0;
    b.freq_write = 40000000;
    b.freq_read = 16000000;
    b.spi_3wire = true;
    b.use_lock = true;
    b.dma_channel = 1;
    b.pin_sclk = 18;
    b.pin_mosi = 23;
    b.pin_miso = 19;
    b.pin_dc = 27;
    bus_.config(b);
    panel_.setBus(&bus_);

    auto p = panel_.config();
    p.pin_cs = 14;
    p.pin_rst = 33;
    p.pin_busy = -1;
    p.memory_width = 320;
    p.memory_height = 240;
    p.panel_width = 320;
    p.panel_height = 240;
    p.offset_rotation = 3;
    p.readable = true;
    p.rgb_order = false;
    p.dlen_16bit = false;
    p.bus_shared = true;  // shares the SPI bus with the microSD slot
    panel_.config(p);

    auto l = light_.config();
    l.pin_bl = 32;
    l.invert = false;
    l.freq = 44100;
    l.pwm_channel = 7;
    light_.config(l);
    panel_.setLight(&light_);
    setPanel(&panel_);
  }
};

static LGFX_M5StackBasic lcd;
static Preferences prefs;
static char device_id[16];

// ------------------------------------------------------------------- BLE ---

static NimBLEServer* ble_server = nullptr;
static NimBLECharacteristic* ble_tx = nullptr;
static StreamBufferHandle_t ble_rx_buf;      // NimBLE task -> loop()
static volatile bool ble_subscribed = false;  // set from NimBLE callbacks
static volatile bool ble_changed = false;
static volatile uint16_t ble_conn = 0xFFFF;
static volatile uint16_t ble_mtu = 23;

class ServerCb : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* s, NimBLEConnInfo& info) override {
    ble_conn = info.getConnHandle();
    // A ~600-byte snapshot a minute needs little bandwidth: a 100–200 ms connection
    // interval with slave latency 2 keeps the radio mostly idle (units: 1.25 ms, 10 ms).
    s->updateConnParams(info.getConnHandle(), 80, 160, 2, 400);
  }
  void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int) override {
    ble_conn = 0xFFFF;
    ble_subscribed = false;
    ble_changed = true;
    NimBLEDevice::startAdvertising();
  }
  void onMTUChange(uint16_t mtu, NimBLEConnInfo&) override { ble_mtu = mtu; }
};

class RxCb : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo&) override {
    NimBLEAttValue v = c->getValue();
    xStreamBufferSend(ble_rx_buf, v.data(), v.length(), 0);
  }
};

class TxCb : public NimBLECharacteristicCallbacks {
  void onSubscribe(NimBLECharacteristic*, NimBLEConnInfo&, uint16_t sub) override {
    ble_subscribed = sub != 0;
    ble_changed = true;
  }
};

static bool ble_on = false;

static void ble_start() {
  if (ble_on) return;
  ble_on = true;
  xStreamBufferReset(ble_rx_buf);
  char name[24];
  snprintf(name, sizeof name, "M5Claude-%s", device_id + 4);
  NimBLEDevice::init(name);
  NimBLEDevice::setMTU(247);
  NimBLEDevice::setPower(ESP_PWR_LVL_N0);  // 0 dBm: plenty for a desk, saves current
  ble_server = NimBLEDevice::createServer();
  ble_server->setCallbacks(new ServerCb());
  NimBLEService* svc = ble_server->createService(kSvcUuid);
  svc->createCharacteristic(kRxUuid, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR)
      ->setCallbacks(new RxCb());
  ble_tx = svc->createCharacteristic(kTxUuid, NIMBLE_PROPERTY::NOTIFY);
  ble_tx->setCallbacks(new TxCb());
  svc->start();
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->addServiceUUID(kSvcUuid);
  adv->setName(name);
  adv->enableScanResponse(true);
  adv->setMinInterval(800);   // 500 ms (units of 0.625 ms); the bridge scans for 4 s at a time
  adv->setMaxInterval(1600);  // 1 s
  adv->start();
}

// Radio fully off (standby). NimBLE frees everything; ble_start() rebuilds it.
static void ble_stop() {
  if (!ble_on) return;
  ble_on = false;
  NimBLEDevice::deinit(true);
  ble_server = nullptr;
  ble_tx = nullptr;
  ble_subscribed = false;
  ble_conn = 0xFFFF;
  ble_changed = false;
  app::link_down(LINK_BLE);
}

static void ble_write(const char* data, size_t len) {
  if (!ble_on || !ble_subscribed) return;
  const size_t chunk = ble_mtu > 23 ? ble_mtu - 3 : 20;
  for (size_t off = 0; off < len; off += chunk) {
    size_t n = len - off < chunk ? len - off : chunk;
    ble_tx->notify((const uint8_t*)data + off, n);
  }
}

static void ble_poll() {
  if (ble_changed) {
    ble_changed = false;
    if (ble_subscribed) app::link_up(LINK_BLE, false);
    else app::link_down(LINK_BLE);
  }
  char buf[256];
  size_t n;
  while ((n = xStreamBufferReceive(ble_rx_buf, buf, sizeof buf, 0)) > 0) app::link_rx(LINK_BLE, buf, n);
}

// ------------------------------------------------------------ battery ---
// IP5306 power manager on the internal I2C bus (SDA 21, SCL 22), same registers as
// M5Stack's own libraries. Its fuel gauge reports four steps: 25/50/75/100 %.

// LovyanGFX's I2C helpers (the ones its board autodetection uses for power chips).
constexpr int kI2cPort = 1;
constexpr uint8_t kIp5306 = 0x75;
constexpr uint32_t kI2cFreq = 100000;

static bool ip5306_read(uint8_t reg, uint8_t& val) {
  return lgfx::i2c::transactionWriteRead(kI2cPort, kIp5306, &reg, 1, &val, 1, kI2cFreq).has_value();
}

static bool ip5306_write(uint8_t reg, uint8_t val) {
  const uint8_t buf[2] = {reg, val};
  return lgfx::i2c::transactionWrite(kI2cPort, kIp5306, buf, 2, kI2cFreq).has_value();
}

static void battery_begin() {
  lgfx::i2c::init(kI2cPort, 21, 22);
  // Keep the 5 V boost on at light load; otherwise the IP5306 cuts power on battery
  // when the board draws little (screen off, BLE idle).
  uint8_t ctl0;
  if (ip5306_read(0x00, ctl0)) ip5306_write(0x00, ctl0 | 0x02);
}

static bool s_on_battery = false;  // no USB power: save energy

static void battery_poll() {
  static uint32_t last = 0;
  if (last && millis() - last < 2000) return;
  last = millis();
  uint8_t level_reg, read0, read1;
  if (!ip5306_read(0x78, level_reg)) {
    ui::set_battery(-1, false, false);  // no gauge (or no battery base)
    return;
  }
  int level;
  switch (level_reg >> 4) {
    case 0x00: level = 100; break;
    case 0x08: level = 75; break;
    case 0x0C: level = 50; break;
    case 0x0E: level = 25; break;
    default: level = 0;
  }
  const bool plugged = ip5306_read(0x70, read0) && (read0 & 0x08);
  const bool full = plugged && ip5306_read(0x71, read1) && (read1 & 0x08);
  static int logged = -2;
  const int code = level * 4 + plugged * 2 + full;
  if (code != logged) {  // diagnostics on the serial port (non-JSON lines are ignored)
    logged = code;
    Serial.printf("# battery %d%% plugged=%d full=%d (0x78=%02X)\n", level, plugged, full, level_reg);
  }
  ui::set_battery(level, plugged, full);
  s_on_battery = !plugged;
}

// ------------------------------------------------------------ speaker ---
// The 1 W speaker on GPIO 25, driven with LEDC square waves at a low duty (volume).

constexpr int kSpeakerPin = 25, kSpeakerCh = 2;
constexpr uint32_t kVolume = 24;  // duty out of 1023

struct Note {
  uint16_t freq;  // 0 = rest
  uint16_t ms;
};
static const Note kClick[] = {{2400, 12}};
static const Note kAlert[] = {{880, 110}, {0, 60}, {880, 110}, {0, 60}, {1175, 220}};
static const Note kReset[] = {{523, 90}, {659, 90}, {784, 160}};
static const Note kPair[] = {{1047, 80}, {1319, 140}};
static const Note* s_tune = nullptr;
static size_t s_tune_len = 0, s_tune_pos = 0;
static uint32_t s_note_end = 0;

static void speaker_off() {
  ledcWrite(kSpeakerCh, 0);
  ledcDetachPin(kSpeakerPin);
  pinMode(kSpeakerPin, OUTPUT);
  digitalWrite(kSpeakerPin, LOW);  // no hiss between sounds
}

static void play_tune(ui::Sound snd) {
  switch (snd) {
    case ui::SND_CLICK: s_tune = kClick; s_tune_len = 1; break;
    case ui::SND_ALERT: s_tune = kAlert; s_tune_len = 5; break;
    case ui::SND_RESET: s_tune = kReset; s_tune_len = 3; break;
    case ui::SND_PAIR: s_tune = kPair; s_tune_len = 2; break;
  }
  s_tune_pos = 0;
  s_note_end = 0;
  ledcSetup(kSpeakerCh, 1000, 10);
  ledcAttachPin(kSpeakerPin, kSpeakerCh);
}

static void speaker_poll() {
  if (!s_tune || millis() < s_note_end) return;
  if (s_tune_pos >= s_tune_len) {
    s_tune = nullptr;
    speaker_off();
    return;
  }
  const Note& n = s_tune[s_tune_pos++];
  if (n.freq) {
    ledcWriteTone(kSpeakerCh, n.freq);
    ledcWrite(kSpeakerCh, kVolume);
  } else {
    ledcWrite(kSpeakerCh, 0);
  }
  s_note_end = millis() + n.ms;
}

// -------------------------------------------------------------- power ---
// The Basic's 110 mAh battery lasts well under an hour with the radio, CPU and
// backlight all on. On battery: CPU at 80 MHz, and once the screen has been off for a
// while, standby: radio off and the CPU in light sleep (RAM kept), woken by any button
// or by a timer that briefly turns the radio on to fetch fresh data, screen still off.

constexpr uint32_t kStandbyAfterMs = 15000;           // screen off this long → standby
constexpr uint64_t kSyncEveryUs = 10ULL * 60 * 1000000;  // background refresh period
constexpr uint32_t kSyncWindowMs = 45000;             // radio-on time per refresh

static const gpio_num_t kButtons[3] = {GPIO_NUM_39, GPIO_NUM_38, GPIO_NUM_37};

static void set_cpu_mhz(uint32_t mhz) {
  if (getCpuFrequencyMhz() != mhz) setCpuFrequencyMhz(mhz);  // BLE needs ≥ 80 MHz
}

static bool any_button_down() {
  for (gpio_num_t b : kButtons)
    if (!gpio_get_level(b)) return true;
  return false;
}

static void serve_links();  // defined with loop()

// Returns when a button is pressed, USB power appears, or an alert turns the screen on.
static void standby() {
  ble_stop();
  for (;;) {
    for (gpio_num_t b : kButtons) gpio_wakeup_enable(b, GPIO_INTR_LOW_LEVEL);
    esp_sleep_enable_gpio_wakeup();
    esp_sleep_enable_timer_wakeup(kSyncEveryUs);
    Serial.flush();
    esp_light_sleep_start();
    for (gpio_num_t b : kButtons) gpio_wakeup_disable(b);
    if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_TIMER) break;  // a button

    // Background refresh with the screen off.
    ble_start();
    const uint32_t seen = app::snapshots_received(), t0 = millis();
    while (millis() - t0 < kSyncWindowMs && app::snapshots_received() == seen) {
      serve_links();
      if (!s_on_battery || !ui::screen_is_off() || any_button_down()) {  // stay awake
        ui::wake();
        return;
      }
      delay(20);
    }
    serve_links();
    if (!ui::screen_is_off()) return;  // an alert from the new data lit the screen
    ble_stop();
  }
  while (any_button_down()) delay(10);  // the press that wakes it does nothing else
  ble_start();
  ui::wake();
}

// -------------------------------------------------------- trusted PCs ---

static bool is_trusted_pc(const char* pcid) {
  String list = prefs.getString("pcs", "");
  int start = 0;
  while (start < (int)list.length()) {
    int end = list.indexOf(',', start);
    if (end < 0) end = list.length();
    if (list.substring(start, end) == pcid) return true;
    start = end + 1;
  }
  return false;
}

static void trust_pc(const char* pcid) {
  if (is_trusted_pc(pcid)) return;
  String list = prefs.getString("pcs", "");
  String out = pcid;
  int kept = 1, start = 0;
  while (start < (int)list.length() && kept < kMaxTrustedPcs) {  // newest first, oldest dropped
    int end = list.indexOf(',', start);
    if (end < 0) end = list.length();
    out += "," + list.substring(start, end);
    ++kept;
    start = end + 1;
  }
  prefs.putString("pcs", out);
}

static int paired_count() {
  String list = prefs.getString("pcs", "");
  if (!list.length()) return 0;
  int n = 1;
  for (char c : list) n += c == ',';
  return n;
}

// ------------------------------------------------------------- arduino ---

void setup() {
  Serial.setRxBufferSize(4096);
  Serial.begin(115200);
  pinMode(25, OUTPUT);  // keep the speaker DAC quiet
  digitalWrite(25, LOW);

  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_BT);
  snprintf(device_id, sizeof device_id, "M5C-%02X%02X", mac[4], mac[5]);

  lcd.init();
  lcd.setRotation(1);
  lcd.setBrightness(160);
  Serial.printf("# lcd %dx%d rot=%d\n", lcd.width(), lcd.height(), lcd.getRotation());
  prefs.begin("m5claude", false);
  battery_begin();

  ui::Hooks hooks;
  hooks.brightness = [](uint8_t level) {
    if (level == 0) {
      lcd.setBrightness(0);
      lcd.sleep();
    } else {
      lcd.wakeup();
      lcd.setBrightness(level);
    }
  };
  hooks.sound = play_tune;
  hooks.save = [](const ui::Settings& st) { prefs.putBytes("set", &st, sizeof st); };
  hooks.paired_count = paired_count;
  hooks.forget_pcs = [] { prefs.remove("pcs"); };
  hooks.device_id = device_id;
  hooks.version = app::kFirmwareVersion;
  ui::set_hooks(hooks);
  ui::Settings st;
  if (prefs.getBytesLength("set") == sizeof st) prefs.getBytes("set", &st, sizeof st);
  ui::apply_settings(st);

  app::Platform p;
  p.write = [](int link, const char* d, size_t n) {
    if (link == LINK_USB) Serial.write((const uint8_t*)d, n);
    else ble_write(d, n);
  };
  p.drop = [](int link) {
    if (link == LINK_BLE && ble_conn != 0xFFFF) ble_server->disconnect(ble_conn);
  };
  p.is_trusted_pc = is_trusted_pc;
  p.trust_pc = trust_pc;
  app::begin(&lcd, device_id, p);

  // Holding A while powering on forgets every approved computer.
  if (!lgfx::gpio_in(ui::kPinA)) {
    prefs.remove("pcs");
    ui::toast(ui::tr().pcs_cleared, 3000);
  }

  ble_rx_buf = xStreamBufferCreate(8192, 1);
  ble_start();
  app::link_up(LINK_USB, true);
}

static void serve_links() {
  static char buf[256];
  int n;
  while ((n = Serial.available()) > 0) {
    n = Serial.readBytes(buf, n < (int)sizeof buf ? n : sizeof buf);
    app::link_rx(LINK_USB, buf, n);
  }
  if (ble_on) ble_poll();
  battery_poll();
  speaker_poll();
  app::loop(millis());
}

void loop() {
  serve_links();

  set_cpu_mhz(s_on_battery ? 80 : 240);
  static uint32_t screen_off_since = 0;
  if (!s_on_battery || !ui::screen_is_off()) {
    screen_off_since = 0;
  } else if (!screen_off_since) {
    screen_off_since = millis();
  } else if (millis() - screen_off_since > kStandbyAfterMs) {
    standby();
    screen_off_since = 0;
  }
  delay(s_on_battery ? 20 : 5);
}
