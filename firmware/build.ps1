# Build (and optionally flash) the firmware with arduino-cli.
#   .\firmware\build.ps1                 compile
#   .\firmware\build.ps1 -Upload         compile + flash (auto-detects the M5Stack's port)
#   .\firmware\build.ps1 -Upload -Port COM11
# Needs the esp32:esp32 core and three libraries:
#   arduino-cli core install esp32:esp32
#   arduino-cli lib install LovyanGFX NimBLE-Arduino ArduinoJson
# A LovyanGFX checkout in third_party/LovyanGFX, if present, takes precedence.
param([switch]$Upload, [string]$Port)
# 'Continue': arduino-cli writes progress to stderr, which PowerShell 5.1 would treat as fatal.
$ErrorActionPreference = 'Continue'
$root = Split-Path $PSScriptRoot -Parent
$sketch = Join-Path $PSScriptRoot 'm5claude'
$fqbn = 'esp32:esp32:m5stack-core-esp32:PartitionScheme=no_ota'

# The sketch sees the shared sources through a directory junction (no admin rights needed).
$link = Join-Path $sketch 'src'
if (-not (Test-Path $link)) {
  cmd /c mklink /J "$link" "$(Join-Path $root 'src')" | Out-Null
}

$build = Join-Path $root 'build\firmware'
$extra = @()
$lgfx = Join-Path $root 'third_party\LovyanGFX'
if (Test-Path $lgfx) { $extra += @('--library', $lgfx) }
arduino-cli compile --fqbn $fqbn @extra `
  --build-property 'compiler.cpp.extra_flags=-std=gnu++17' --build-path $build $sketch
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

if ($Upload) {
  if (-not $Port) {
    # M5Stack Core USB-UART chips: CH9102 (v2.6+), CP2104 (older).
    $dev = Get-CimInstance Win32_PnPEntity |
      Where-Object { $_.DeviceID -match 'VID_1A86&PID_55D4|VID_10C4&PID_EA60' -and $_.Name -match '\((COM\d+)\)' } |
      Select-Object -First 1
    if (-not $dev) { Write-Error 'No M5Stack found on USB. Plug it in or pass -Port COMx.'; exit 1 }
    $Port = [regex]::Match($dev.Name, 'COM\d+').Value
  }
  Write-Host "Flashing $Port ..."
  arduino-cli upload --fqbn $fqbn --input-dir $build -p $Port $sketch
  exit $LASTEXITCODE
}
