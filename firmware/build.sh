#!/usr/bin/env sh
# Build (and optionally flash) the firmware with arduino-cli on Linux/macOS.
#   ./firmware/build.sh                  compile
#   ./firmware/build.sh /dev/ttyUSB0     compile + flash
# Needs: arduino-cli core install esp32:esp32
#        arduino-cli lib install LovyanGFX NimBLE-Arduino ArduinoJson
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(dirname "$here")
sketch="$here/m5claude"
fqbn='esp32:esp32:m5stack-core-esp32:PartitionScheme=no_ota'
[ -e "$sketch/src" ] || ln -s ../../src "$sketch/src"   # the sketch sees the shared sources
arduino-cli compile --fqbn "$fqbn" \
  --build-property 'compiler.cpp.extra_flags=-std=gnu++17' \
  --build-path "$root/build/firmware" "$sketch"
if [ -n "$1" ]; then
  arduino-cli upload --fqbn "$fqbn" --input-dir "$root/build/firmware" -p "$1" "$sketch"
fi
