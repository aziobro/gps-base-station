#Requires -Version 5.1
<#
.SYNOPSIS
    Windows / PowerShell equivalent of tools/release.sh — one command to ship a firmware
    update and prove it landed.

.DESCRIPTION
    Workflow:
      1. Bump the version (writes version.txt — the single source of truth).
      2. Build the firmware (delegated to ..\idf.ps1 build).
      3. Verify the freshly built binary embeds the target version.
      4. Push it to the device: over USB (idf.ps1 flash) or over the air (HTTPS POST /update).
      5. Verify the running device reports the target version at /status.

.EXAMPLE
    .\tools\release.ps1 build                              # bump + build + verify binary (no device)
    $env:PORT='COM5'; .\tools\release.ps1 usb              # bump + build + USB flash
    $env:ADMIN_PASSWORD='secret'; .\tools\release.ps1 ota 192.168.8.186
    .\tools\release.ps1 ota 192.168.8.186 2026.06.12-ota94 # explicit version (no auto-bump)

.NOTES
    VERSION is optional; when omitted the trailing integer of the current version is
    incremented (e.g. 2026.06.12-ota93 -> 2026.06.12-ota94).

    Machine config (serial port, device host, admin user/password) comes from
    config.win.ps1 (git-ignored; copy from config.win.sample.ps1). A real environment
    variable ($env:PORT, $env:DEVICE_HOST, $env:ADMIN_USER, $env:ADMIN_PASSWORD) overrides it.

    If PowerShell blocks scripts, launch with:
        powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\release.ps1 ota 192.168.8.186
#>
[CmdletBinding()]
param(
    [Parameter(Position = 0, Mandatory = $true)]
    [ValidateSet('build', 'usb', 'ota')]
    [string]$Mode,
    [Parameter(Position = 1)] [string]$Arg1,
    [Parameter(Position = 2)] [string]$Arg2
)

$ErrorActionPreference = 'Stop'
$ScriptDir   = $PSScriptRoot
$RepoRoot    = Split-Path -Parent $ScriptDir
$VersionFile = Join-Path $RepoRoot 'version.txt'
$AppBin      = Join-Path $RepoRoot 'build\gps_base_station.bin'
$IdfWrapper  = Join-Path $RepoRoot 'idf.ps1'
$FwVersionPy = Join-Path $ScriptDir 'fw_version.py'

# --- machine config: defaults <- config.win.ps1 <- caller env ---
$SerialPort    = ''
$DeviceHost    = ''
$AdminUser     = 'admin'
$AdminPassword = ''
$cfg = Join-Path $RepoRoot 'config.win.ps1'
if (Test-Path $cfg) { . $cfg }
if ($env:PORT)           { $SerialPort    = $env:PORT }
if ($env:DEVICE_HOST)    { $DeviceHost    = $env:DEVICE_HOST }
if ($env:ADMIN_USER)     { $AdminUser     = $env:ADMIN_USER }
if ($env:ADMIN_PASSWORD) { $AdminPassword = $env:ADMIN_PASSWORD }

function Die([string]$m)  { Write-Host "ERROR: $m" -ForegroundColor Red; exit 1 }
function Step([string]$m) { Write-Host ""; Write-Host "==> $m" -ForegroundColor Cyan }

# --- argument parsing (mirror release.sh: ota HOST [VERSION]; build|usb [VERSION]) ---
$OtaHost = ''
$ExplicitVersion = ''
if ($Mode -eq 'ota') {
    $OtaHost = $Arg1
    $ExplicitVersion = $Arg2
    if (-not $OtaHost) { $OtaHost = $DeviceHost }
    if (-not $OtaHost) { Die "ota mode requires a HOST argument (or set `$DeviceHost in config.win.ps1)" }
} else {
    $ExplicitVersion = $Arg1
}

function Get-CurrentVersion {
    if (Test-Path $VersionFile) { return ((Get-Content $VersionFile -TotalCount 1) -join '').Trim() }
    return ''
}
function BumpVersion([string]$cur) {
    if     ($cur -match '^(.*[^0-9])([0-9]+)$') { return ($Matches[1] + ([int64]$Matches[2] + 1)) }
    elseif ($cur -match '^[0-9]+$')             { return [string]([int64]$cur + 1) }
    else                                        { return "$cur-1" }
}

$CurVersion = Get-CurrentVersion
if     ($ExplicitVersion) { $Target = $ExplicitVersion }
elseif ($CurVersion)      { $Target = BumpVersion $CurVersion }
else                      { Die "no version.txt and no explicit VERSION given" }

# The app descriptor caps version at 31 chars (+ NUL).
if ($Target.Length -gt 31) { Die "version '$Target' exceeds 31 chars" }

$shown = if ($CurVersion) { $CurVersion } else { '<none>' }
Step "Version: $shown -> $Target"
# Single line, LF, no BOM — match the repo's version.txt exactly.
[System.IO.File]::WriteAllText($VersionFile, "$Target`n")

# --- build (delegate to idf.ps1 so ESP-IDF activation lives in one place) ---
Step "Building firmware"
& powershell -NoProfile -ExecutionPolicy Bypass -File $IdfWrapper build
if ($LASTEXITCODE -ne 0)      { Die "build failed (idf.py exit $LASTEXITCODE)" }
if (-not (Test-Path $AppBin)) { Die "build did not produce $AppBin" }

# --- verify the binary embeds the target version ---
Step "Verifying built binary"
$BinVersion = (& python $FwVersionPy $AppBin | Out-String).Trim()
Write-Host "    binary reports: $BinVersion"
if ($BinVersion -ne $Target) { Die "binary version '$BinVersion' != target '$Target'" }
Write-Host "    OK - binary matches target version"

# --- helpers for live (on-device) verification ---
# /update and /status are gated by AdminWebServer::authorize()
# (main/web_server.cpp), which only checks a session cookie set by
# POST /login -- there is no HTTP Basic Auth fallback. A bare
# `curl -u user:pass` silently fails auth on every request (hit this
# 2026-07-05: "Login required" on every call despite a correct password).
# Logs in fresh each time rather than reusing one cookie jar for the whole
# script run: a successful OTA upload reboots the device, which wipes its
# in-RAM session_token_, invalidating any cookie obtained before the reboot.
function New-LoginCookieJar([string]$h) {
    $jar = New-TemporaryFile
    $code = & curl.exe -k -s -m 8 -c $jar -o NUL -w '%{http_code}' `
        --data-urlencode "user=$AdminUser" --data-urlencode "password=$AdminPassword" `
        "https://$h/login"
    if ($code -ne '303') { Remove-Item $jar -ErrorAction SilentlyContinue; return $null }
    return $jar
}
function Get-DeviceVersion([string]$h) {
    $jar = New-LoginCookieJar $h
    if (-not $jar) { return '' }
    $r = & curl.exe -k -s -m 8 -b $jar "https://$h/status"
    Remove-Item $jar -ErrorAction SilentlyContinue
    if (-not $r) { return '' }
    try { return ($r | ConvertFrom-Json).version } catch { return '' }
}
function Require-Password {
    if (-not $AdminPassword) { Die "admin password required: set `$env:ADMIN_PASSWORD (or `$AdminPassword in config.win.ps1)" }
}
function Verify-Live([string]$h) {
    Step "Verifying live device at $h (waiting for reboot)"
    $deadline = (Get-Date).AddSeconds(120)
    $seen = ''
    while ((Get-Date) -lt $deadline) {
        $seen = Get-DeviceVersion $h
        if ($seen -eq $Target) { Write-Host "    OK - device reports $seen" -ForegroundColor Green; return }
        if ($seen) { Write-Host "    device reports '$seen' (want '$Target'), retrying..." }
        Start-Sleep -Seconds 5
    }
    Die "device never reported '$Target' (last saw: '$seen')"
}

# --- push ---
switch ($Mode) {
    'build' {
        Step "Done (build-only). version.txt = $Target"
    }
    'usb' {
        Step "Flashing over USB"
        if ($SerialPort) { $env:PORT = $SerialPort }
        & powershell -NoProfile -ExecutionPolicy Bypass -File $IdfWrapper flash
        if ($LASTEXITCODE -ne 0) { Die "flash failed (idf.py exit $LASTEXITCODE)" }
        if ($DeviceHost) { Require-Password; Verify-Live $DeviceHost }
        else { Write-Host "    Flashed. Set `$env:DEVICE_HOST to auto-verify /status, or open https://<device>/ and confirm '$Target'." }
    }
    'ota' {
        Require-Password
        # A successful upload reboots the device, so the HTTP response can be dropped as
        # the socket closes; a busy base station may also reject the first attempt. Retry,
        # but check /status first so we never re-upload a build the device already took.
        for ($attempt = 1; $attempt -le 3; $attempt++) {
            Step "Uploading over the air to $OtaHost (attempt $attempt)"
            $jar = New-LoginCookieJar $OtaHost
            if (-not $jar) { Die "login to $OtaHost failed - check ADMIN_PASSWORD" }
            $resp = & curl.exe -k -s -m 600 -b $jar `
                -H "Content-Type: application/octet-stream" `
                --data-binary "@$AppBin" "https://$OtaHost/update"
            Remove-Item $jar -ErrorAction SilentlyContinue
            if ($resp -match 'Update accepted') { Write-Host "    device: $resp"; break }
            Write-Host "    upload did not confirm (response: '$resp')"
            Start-Sleep -Seconds 6
            if ((Get-DeviceVersion $OtaHost) -eq $Target) { Write-Host "    device already reports $Target - upload took"; break }
            if ($attempt -lt 3) { Write-Host "    retrying..." }
        }
        # Verify-Live is authoritative: it polls /status and fails if the device never
        # reports the target version.
        Verify-Live $OtaHost
    }
}

Step "Release $Target complete"
