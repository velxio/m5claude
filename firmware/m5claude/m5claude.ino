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
    s->updateConnParams(info.getConnHandle(), 12, 24, 0, 400);
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

static void ble_begin() {
  ble_rx_buf = xStreamBufferCreate(8192, 1);
  char name[24];
  snprintf(name, sizeof name, "M5Claude-%s", device_id + 4);
  NimBLEDevice::init(name);
  NimBLEDevice::setMTU(247);
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);
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
  adv->start();
}

static void ble_write(const char* data, size_t len) {
  if (!ble_subscribed) return;
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
  lcd.setBrightness(170);
  Serial.printf("# lcd %dx%d rot=%d\n", lcd.width(), lcd.height(), lcd.getRotation());
  prefs.begin("m5claude", false);

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
    ui::toast("Paired computers cleared", 3000);
  }

  ble_begin();
  app::link_up(LINK_USB, true);
}

void loop() {
  static char buf[256];
  int n;
  while ((n = Serial.available()) > 0) {
    n = Serial.readBytes(buf, n < (int)sizeof buf ? n : sizeof buf);
    app::link_rx(LINK_USB, buf, n);
  }
  ble_poll();
  app::loop(millis());
  delay(5);
}
