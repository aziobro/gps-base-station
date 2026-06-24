# Docker build environment

This project can be built in a Linux ESP-IDF container instead of the native
Windows ESP-IDF install. Use this when Windows path/toolchain behavior gets in
the way.

## Requirements

- Docker Desktop with the WSL2 backend enabled.
- The repository checked out on the Windows PC.
- The Raspberry Pi remains the preferred USB flasher/logger for the device.

## Build the container

From the project root:

```powershell
.\tools\docker-build.ps1
```

The first run builds a local image named `gps-base-station-idf:v6.0.1` from
`docker/Dockerfile`, then runs:

```bash
idf.py -B build-docker build
```

Build output is written to `build-docker/` so the native Windows `build/`
directory is not touched.

## Common commands

```powershell
.\tools\docker-build.ps1 build
.\tools\docker-build.ps1 fullclean
.\tools\docker-build.ps1 menuconfig
.\tools\docker-build.ps1 reconfigure
```

To rebuild the Docker image after changing `docker/Dockerfile`:

```powershell
docker build -t gps-base-station-idf:v6.0.1 -f docker\Dockerfile .
```

To use a different local image name:

```powershell
$env:GBS_DOCKER_IMAGE='gps-base-station-idf:test'
.\tools\docker-build.ps1 build
```

## Firmware outputs

After a successful build, the files normally needed for Pi flashing are:

```text
build-docker/gps_base_station.bin
build-docker/ota_data_initial.bin
build-docker/bootloader/bootloader.bin
build-docker/partition_table/partition-table.bin
```

For the usual OTA-slot recovery flash from the Pi, copy at least:

```text
build-docker/ota_data_initial.bin
build-docker/gps_base_station.bin
```

## Verify the embedded version

```powershell
python .\tools\fw_version.py .\build-docker\gps_base_station.bin
```

The reported version should match `version.txt`.

## Troubleshooting

If Docker reports access denied for `//./pipe/docker_engine` or
`C:\Users\<you>\.docker\config.json`, Docker Desktop is installed but this shell
does not have access to it. Start Docker Desktop and run the command from a
PowerShell session that can run:

```powershell
docker info
```

If that still fails, run PowerShell as Administrator once to confirm Docker
Desktop itself is healthy, then fix the Windows permissions for the Docker
config/pipe or add the user to the local `docker-users` group.

## Notes

- The container uses the official `espressif/idf:v6.0.1` base image.
- The ESP-IDF target is fixed to `esp32p4`.
- Do not commit `build-docker/`; it is generated output.
