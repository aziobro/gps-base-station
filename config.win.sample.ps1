# GPS Base Station — Windows build/deploy configuration (SAMPLE)
#
# Copy this file to "config.win.ps1" (git-ignored) and edit it for your machine:
#
#     Copy-Item config.win.sample.ps1 config.win.ps1
#
# This is the Windows / PowerShell analog of config.sample.env. It is dot-sourced by
# idf.ps1 and tools/release.ps1. Every value is optional — the scripts fall back to
# built-in defaults — and a real environment variable set by the caller
# (e.g. $env:DEVICE_HOST, $env:PORT, $env:ADMIN_PASSWORD) always wins over the value here.

# ----------------------------------------------------------------------------
# Build toolchain
# ----------------------------------------------------------------------------
# Absolute path to the ESP-IDF v6.0.1 framework directory (the one containing
# export.ps1). Find it in %USERPROFILE%\.espressif\idf-env.json, or in the
# ESP-IDF VS Code extension setting "idf.espIdfPath".
$IdfPath = 'C:\esp\v6.0.1\esp-idf'

# ----------------------------------------------------------------------------
# Flashing / device
# ----------------------------------------------------------------------------
# Serial port for USB flashing (.\idf.ps1 flash, .\tools\release.ps1 usb), e.g. 'COM5'.
# Find it in Device Manager under "Ports (COM & LPT)".
$SerialPort = ''

# Device hostname or IP for OTA upload and live /status verification.
$DeviceHost = '192.168.8.186'

# Web UI / OTA admin username.
$AdminUser = 'admin'

# Admin password — prefer passing it at runtime rather than committing it:
#   $env:ADMIN_PASSWORD = 'secret'; .\tools\release.ps1 ota $DeviceHost
# config.win.ps1 is git-ignored, so you MAY uncomment and set it here for convenience:
# $AdminPassword = '...'
