# GPS RTK Base Station Project

## Hardware
- [project_hardware](project_hardware.md) — ESP32-P4-WIFI6-Touch-LCD-4B, UM980, wiring
- [project_structure](project_structure.md) — IDF file layout, key modules
- [project_display](project_display.md) — Display/touch implementation decisions

## Build & Deploy
- [project_build_deploy](project_build_deploy.md) — build (./idf.sh · Windows idf.ps1), OTA/USB flash, release.sh · release.ps1, bootstrap recovery, config.env, git push
- [project_deploy_test](project_deploy_test.md) — deploy + verify runbook: OTA via web, USB recovery flash from the monitoring Pi (esptool over /dev/ttyACM0), how to confirm a build landed + is crash-free

## Networking & Web
- [project_ntrip](project_ntrip.md) — correction-slot mailbox architecture, reconnect/backoff logic, net_health, Onocoy-vs-RTK2go/RTKdata behavior, bug history

## RINEX / Post-processing
- [project_rinex](project_rinex.md) — signal→band classification, Unicore UM980 signal codes, antenna metadata, OPUS/CSRS-PPP

## Reliability
- [project_wifi_rpc_crash](project_wifi_rpc_crash.md) — IWDT crash from UI/web blocking esp_wifi_sta_get_ap_info RPC to the C6; fixed + confirmed in ota95 by caching ssid/rssi
