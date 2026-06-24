#Requires -Version 5.1
<#
.SYNOPSIS
    Build the ESP32-P4 firmware inside a Linux ESP-IDF Docker container.

.EXAMPLE
    .\tools\docker-build.ps1
    .\tools\docker-build.ps1 menuconfig
    .\tools\docker-build.ps1 fullclean

.NOTES
    The container writes build output to build-docker/ so the Windows ESP-IDF
    build/ directory is left alone.
#>
[CmdletBinding()]
param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$IdfArgs
)

$ErrorActionPreference = 'Stop'

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$ImageName = if ($env:GBS_DOCKER_IMAGE) { $env:GBS_DOCKER_IMAGE } else { 'gps-base-station-idf:v6.0.1' }

if (-not (Get-Command docker -ErrorAction SilentlyContinue)) {
    Write-Error 'Docker was not found. Install Docker Desktop and enable the WSL2 backend.'
    exit 1
}

cmd /c "docker info >nul 2>nul"
if ($LASTEXITCODE -ne 0) {
    Write-Error 'Docker is installed, but this shell cannot reach the Docker daemon. Start Docker Desktop, then run this from a shell with Docker access.'
    exit $LASTEXITCODE
}

if (-not $IdfArgs -or $IdfArgs.Count -eq 0) {
    $IdfArgs = @('build')
}

cmd /c "docker image inspect $ImageName >nul 2>nul"
if ($LASTEXITCODE -ne 0) {
    docker build -t $ImageName -f (Join-Path $RepoRoot 'docker\Dockerfile') $RepoRoot
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

$dockerArgs = @(
    'run', '--rm',
    '-e', 'IDF_TARGET=esp32p4',
    '-v', "${RepoRoot}:/work",
    '-w', '/work',
    $ImageName,
    'idf.py', '-B', 'build-docker'
) + $IdfArgs

docker @dockerArgs
exit $LASTEXITCODE
