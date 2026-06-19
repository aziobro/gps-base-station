#Requires -Version 5.1
<#
.SYNOPSIS
    Windows / PowerShell equivalent of idf.sh — activate ESP-IDF v6.0.1 and run idf.py.

.DESCRIPTION
    PowerShell does not persist shell state between invocations, so every build must
    activate ESP-IDF in the same process that runs idf.py. This wrapper dot-sources the
    framework's export.ps1 and then runs idf.py with the supplied arguments.

.EXAMPLE
    .\idf.ps1 build
    .\idf.ps1 flash monitor
    $env:PORT = 'COM5'; .\idf.ps1 flash
    . .\idf.ps1                 # dot-source with no args: just activate ESP-IDF in this shell

.NOTES
    If PowerShell blocks scripts (execution policy), launch with:
        powershell -NoProfile -ExecutionPolicy Bypass -File .\idf.ps1 build
    or, once per user:
        Set-ExecutionPolicy -Scope CurrentUser RemoteSigned

    Machine-specific values (ESP-IDF path, serial port) come from config.win.ps1
    (git-ignored; copy from config.win.sample.ps1). A real environment variable
    ($env:IDF_PATH, $env:PORT) always overrides config.win.ps1.
#>
[CmdletBinding()]
param(
    # Everything on the command line is passed straight through to idf.py.
    # The serial port is taken from $env:PORT / config.win.ps1, NOT a parameter,
    # so a bare "build" / "flash monitor" is never mistaken for a port value.
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$IdfArgs
)

$ScriptDir = $PSScriptRoot

# --- machine config: built-in default <- config.win.ps1 <- caller env ---
$IdfPath    = 'C:\esp\v6.0.1\esp-idf'
$SerialPort = ''
$cfg = Join-Path $ScriptDir 'config.win.ps1'
if (Test-Path $cfg) { . $cfg }                       # may set $IdfPath, $SerialPort
if ($env:IDF_PATH) { $IdfPath = $env:IDF_PATH }
$Port = if ($env:PORT) { $env:PORT } else { $SerialPort }

$export = Join-Path $IdfPath 'export.ps1'
if (-not (Test-Path $export)) {
    throw "ESP-IDF export.ps1 not found at '$export'. Set `$IdfPath in config.win.ps1 or `$env:IDF_PATH."
}

# Activate ESP-IDF (sets IDF_PATH, puts idf.py + the RISC-V toolchain on PATH).
. $export

if ($IdfArgs -and $IdfArgs.Count -gt 0) {
    Push-Location $ScriptDir
    try {
        if ($Port) { idf.py -p $Port @IdfArgs }
        else       { idf.py @IdfArgs }
    } finally { Pop-Location }
    exit $LASTEXITCODE
}
