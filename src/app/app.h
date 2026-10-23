// Glue between the transports (USB serial, BLE, or a TCP socket in development builds), the
// dashboard model and the UI. Transport-agnostic: the platform feeds received bytes in
// and provides a write function for replies.
//
// Protocol (newline-delimited JSON, both directions):
//   bridge -> device  {"pc":"<host name>","pcid":"<stable id>"}      first line on a link
//                     snapshot, see bridge/m5claude_bridge/snapshot.py
//   device -> bridge  {"hello":"m5claude","id":"<device id>","fw":"<version>"}
//                     {"req":"refresh"}
//                     {"pair":"pending" | "ok" | "denied"}       untrusted links only
//
// Trusted links (a USB cable, a local TCP socket) take data right away. On an untrusted
// link (Bluetooth) a PC must be approved once on the device; approvals are remembered.
#pragma once
#include <LovyanGFX.hpp>
#include <stddef.h>
#include <stdint.h>

#include "../model/model.h"

namespace app {

constexpr const char* kFirmwareVersion = "0.1.0";
constexpr int kMaxLinks = 2;

struct Platform {
  void (*write)(int link, const char* data, size_t len) = nullptr;
  void (*drop)(int link) = nullptr;                    // close a link (denied pairing)
  bool (*is_trusted_pc)(const char* pcid) = nullptr;   // remembered approvals
  void (*trust_pc)(const char* pcid) = nullptr;
};

// `demo`: start with mock data instead of waiting for a bridge.
void begin(lgfx::LovyanGFX* lcd, const char* device_id, const Platform& p, bool demo = false);

void link_up(int link, bool trusted);
void link_down(int link);
void link_rx(int link, const char* data, size_t len);

// Call often. `host_epoch` is the platform's own wall clock if it has one (0 otherwise);
// without it, time comes from the bridge's snapshots.
void loop(uint32_t ms, int64_t host_epoch = 0);

const cu::Dashboard& dashboard();

}  // namespace app
