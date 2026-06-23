I have everything needed. Here is the complete inventory.

---

# WEB UI INVENTORY â€” GPS Base Station (current state)

Source: `gps-base-station/main/web_server.cpp` (1982 lines) + `web_server.hpp` (75 lines). All HTML/CSS/JS is generated in C++ via `std::string` concatenation and raw-string literals (`R"HTML(...)"`, `R"JSEOF(...)"`); there is **no template engine and no static asset files** other than baked-in TLS certs.

## 1. Server architecture & transport

- **Two servers** started in `AdminWebServer::start()`:
  - **HTTPS server** on **port 443** (`httpd_ssl_start`). Carries the entire admin UI. TLS cert/key are embedded binaries (`_binary_server_cert_pem_*`, `_binary_server_key_pem_*`). Config: `max_uri_handlers=32`, `stack_size=12288`, `max_open_sockets=8` (sized for "up to seven dashboard tabs + one admin request"), keep-alive enabled (idle 15s, interval 5s, count 2), `recv/send_wait_timeout=30s`, `lru_purge_enable=true`.
  - **HTTP server** on **port 80** (`httpd_start`), tiny (`max_uri_handlers=3`, `max_open_sockets=1`, `backlog_conn=1`, `stack_size=4096`, wildcard URI match). Acts as a **redirect/provisioning gateway** only â€” see `http_gateway_handler`.
- **HTTPâ†’HTTPS behavior** (`http_gateway_handler`, lines 382-419):
  - `GET /ca.crt` over HTTP is always served (cert download must work before trust is established).
  - If **not** in AP mode: responds `308 Permanent Redirect` to `https://<Host>/<uri>` (strips any `:port` from Host; falls back to `gps-base.local`).
  - If in AP mode (provisioning): a small whitelist is served over plain HTTP â€” `GET /`, `/setup`, `/config`, `/wifi/scan`, `/status`, `/ca.crt`; `POST /setup`, `/config/wifi`. Anything else returns 404 "Only WiFi provisioning is available over HTTP".
- **Security headers** set by `send_page()`: `Cache-Control: no-store`, `Strict-Transport-Security: max-age=3600`. JSON/data endpoints set `Cache-Control: no-store`.

## 2. Authentication

- **HTTP Basic Auth**, fixed username `admin` (`kAdminUser`), password from `storage_->admin_password()`. `authorize()` (lines 1273-1299) base64-encodes `admin:<pw>` and compares to the `Authorization` header.
- `send_unauthorized()` returns `401` with `WWW-Authenticate: Basic realm="GPS Base Station"` and `Connection: close`.
- **First-run gate:** if `admin_password_set()` is false, `GET /` redirects (`303`) to `/setup`. `/setup` is the only page reachable before a password exists.
- **Every** handler except `setup_*`, `ca_certificate_handler`, and `http_gateway_handler` calls `authorize()` first.

## 3. Complete route table

| Path | Method | Auth | Returns | Handler |
|---|---|---|---|---|
| `/` | GET | yes (redirects to `/setup` if no pw) | HTML Status dashboard | `root_handler` |
| `/setup` | GET | no | HTML first-time password form | `setup_get_handler` |
| `/setup` | POST | no (403 if already set) | 303â†’`/` | `setup_post_handler` |
| `/config` | GET | yes | HTML config page (services/position/antenna/survey/wifi/AP) | `config_get_handler` |
| `/config` | POST | yes | 303â†’`/config` (saves NTRIP services) | `config_post_handler` |
| `/wifi/scan` | GET | yes (409 unless AP mode) | JSON array of networks | `wifi_scan_handler` |
| `/skyplot` | GET | yes | HTML canvas sky plot page | `skyplot_handler` |
| `/skyplot/data` | GET | yes | JSON satellite array | `skyplot_data_handler` |
| `/status` | GET | yes | JSON full status snapshot | `status_handler` |
| `/update` | GET | yes | HTML OTA upload page | `update_page_handler` |
| `/update` | POST | yes | text/plain result; restarts device | `update_upload_handler` |
| `/config/position` | POST | yes | 303â†’`/` (saves manual position) | `position_handler` |
| `/survey` | POST | yes | 303â†’`/` (starts new survey) | `survey_handler` |
| `/config/wifi` | POST | yes | text/plain; restarts device | `wifi_handler` |
| `/config/ap` | POST | yes | 303â†’`/config` (sets hotspot pw, live-applied) | `ap_password_handler` |
| `/config/antenna` | POST | yes | 303â†’`/config` (saves antenna model/radome/height) | `antenna_handler` |
| `/logs` | GET | yes | HTML console-log viewer | `logs_page_handler` |
| `/logs/data` | GET | yes | text/plain log text + cursor headers | `logs_data_handler` |
| `/ca.crt` | GET | **no** (HTTP+HTTPS) | x509 CA cert download | `ca_certificate_handler` |
| `/files` | GET | yes | HTML SD file browser | `files_page_handler` |
| `/files/list` | GET | yes | JSON dir listing (`?path=`) | `files_list_handler` |
| `/files/download` | GET | yes | binary stream (`?path=`) | `files_download_handler` |
| `/files/delete` | POST | yes | JSON `{"ok":bool}` | `files_delete_handler` |
| `/files/rename` | POST | yes | JSON `{"ok":bool}` | `files_rename_handler` |
| `/files/mkdir` | POST | yes | JSON `{"ok":bool[,"error"]}` | `files_mkdir_handler` |
| `/rinex/toggle` | POST | yes | JSON `{"ok":bool}` | `rinex_toggle_handler` |
| `/rinex/export` | GET | yes | HTML date-range export page | `rinex_export_page_handler` |
| `/rinex/export` | POST | yes | merged `.rnx` binary stream | `rinex_export_handler` |
| `/ntrip/toggle` | POST | yes | JSON `{"ok":true}` | `ntrip_toggle_handler` |
| `/*` | GET/POST (port 80 only) | n/a | redirect or provisioning whitelist | `http_gateway_handler` |

## 4. Shared chrome (`send_page`, lines 1309-1340)

All standard pages are wrapped by `send_page(request, title, content)`. It streams 5 parts in 1 KB chunks via `send_chunks` then a terminating zero-length chunk:

1. **kPrefix:** `<!doctype html><html><head><meta charset utf-8><meta viewport width=device-width,initial-scale=1><link rel=icon href="data:,"><title>`
2. **title** (the per-page title arg)
3. **kAfterTitle:** `</title><style>â€¦</style></head><body><h1>GPS Base Station</h1>` â€” the single shared CSS block.
4. **content** (the page body)
5. **kSuffix:** `</body></html>`

**Shared CSS theme tokens (the current "terminal" look to be replaced):**
- `body{font-family:monospace;background:#111;color:#cfc;padding:1em;max-width:720px;margin:auto}`
- `h1,h2{color:#0f0}` (bright green)
- `table{border-collapse:collapse;width:100%}` Â· `td{border:1px solid #333;padding:6px}`
- `input{width:100%;max-width:420px;padding:7px;background:#1a1a1a;color:#cfc;border:1px solid #444;box-sizing:border-box}`
- `button{padding:8px 16px;background:#1a1a1a;color:#0f0;border:1px solid #0f0}`
- **Status color classes:** `.ok{color:#0f0}` (green) Â· `.warn{color:#fa0}` (amber) Â· `.err{color:#f44}` (red) Â· `.dim{opacity:.6}`
- `a{color:#0d0}`

**IMPORTANT chrome inconsistency:** `/rinex/export` (GET) **does NOT use `send_page`** â€” it builds its own full HTML document with a **different inline stylesheet** (`body{background:#0d0d0d;color:#e0e0e0â€¦}`, headings `#0f0`, accent/buttons in **blue `#08f`** instead of green, `max-width:560px`). The `/files` page also injects heavy **inline styles** (blue `#08f` and amber `#fa0` accents, per-button colors) rather than relying on shared classes. So "shared chrome" is only partially shared today.

**Header byte for redesign:** there is **no shared nav bar**. Cross-links are ad-hoc `<a>` tags appended at the bottom (or top) of each page's content.

## 5. Page-by-page content

### `/` â€” Status dashboard (`root_handler`, 433-635)
- **Single `<table>`** of label/value rows, each value cell carrying an `id` so JS can live-update it:
  - Framework (ESP-IDF version), Application (firmware version), Uptime (`#st-uptime`), Last reset (`#st-reset`, human reason string), System health (`#st-health` ok/err), WiFi (`#st-wifi` connected/AP fallback/disconnected + SSID), IP (`#st-ip`, defaults `192.168.4.1`), WiFi signal (`#st-rssi`, dBm + excellent/good/fair/weak via `rssi_html`), Mode (`#st-mode` Base TX vs Survey), Position (lat/lon/height or "not set"), Survey (`#st-survey` â€” collecting w/ elapsedÂ·samplesÂ·blocksÂ·stabilityÂ·lat/lon/height, or complete, or idle), Satellites (`#st-sats` GPS/GLO/GAL/BDS + total), RTCM (`#st-rtcm` B/s + total bytes), Local NTRIP clients count (`#st-clients`), Local NTRIP client IPs (`#st-client-ips`), NTRIP push (`#st-ntrip` enabled/disabled + **Enable/Disable button** calling `toggleNtrip()`), RTK2go/Onocoy/RTKdata service rows (`#st-r2g`/`#st-onc`/`#st-rtk` via `service_html`: connected+uptime / bytes sent / dropped batches / reconnects / last-data age / last error), Free heap (`#st-heap` bytes + % + low-watermark, color by % thresholds 40/20), SD card (`#st-sd` used/total/% or not-mounted), RINEX collection (`#st-rinex` active+epochs+filename or inactive, with **Start/Stop button** calling `toggleRinex()`).
- **Footer links:** `/config`, `/skyplot`, `/files`, `/rinex/export`, `/logs`, `/status` (raw JSON), `/update`.
- **Client JS (lines 586-633):** Polls `GET /status` **every 15 s** (`setTimeout(refresh,15000)`, self-rescheduling, with a `statusRequest` reentrancy guard). Rebuilds every `#st-*` cell from JSON using local helpers `esc`, `bytes`, `set`, `upt`, `svc`, `ips` (these **duplicate the C++ formatting logic** â€” `service_html`, `human_bytes`, `uptime_str`, `rssi_html` are reimplemented in JS). `toggleRinex(start)` â†’ `POST /rinex/toggle` body `start=1|0`. `toggleNtrip(on)` â†’ `POST /ntrip/toggle` body `on=1|0`. Both re-poll after 500 ms.

### `/setup` â€” First-time setup (637-653)
- Form `POST /setup`: two password inputs (`password`, `confirm`, `minlength=8`, required), "Set Password" button. No other chrome links.
- POST validation: 8-64 chars, must match; saves via `storage_->save_admin_password`; 303â†’`/`. 403 if already set, 400 on mismatch.

### `/config` â€” Configuration (675-764) â€” the densest page
Multiple stacked `<form>`s + `<h2>` section headers:
1. **NTRIP Services** (`POST /config`): per service (RTK2go / Onocoy / RTKdata.online) an enable checkbox (`r2g_en`/`onc_en`/`rtk_en`), mountpoint text input (`*_mp`, prefilled from storage), password input (`*_pw`, blank = keep existing). "Save Services" button.
2. **Manual Position** (`POST /config/position`): lat / lon / hgt inputs (prefilled if valid). "Save Position".
3. **Antenna (RINEX)** (`POST /config/antenna`): explanatory `.dim` note about OPUS/CSRS-PPP phase-centre. Inputs `ant_model` (e.g. HXCGPS500), `ant_radome` (e.g. NONE), `ant_h` (ARP delta-H, m). "Save Antenna".
4. **Survey** (`POST /survey`): single "Start New Survey" button.
5. **WiFi** (`POST /config/wifi`): `ssid` (required, prefilled), `password` (blank = keep). "Save WiFi & Restart" + "Scan Networks" button. Inline JS: Scan button fetches `GET /wifi/scan`, renders a list of `<button>`s (each sets the SSID field) with ` <rssi> dBm secured|open`.
6. **Access Point (Hotspot)** (`POST /config/ap`): shows fixed AP SSID (`config::kAccessPointSsid` = `GPS-BaseStation`), one `ap_pw` input (minlength 8, maxlength 63, required). "Save Hotspot Password".
- **Footer links:** `/logs`, `/`.

### `/skyplot` â€” Sky plot (789-815)
- `<canvas id=sky 640x640>` (CSS-scaled, `max-width:640px`, `background:#080b08`), a `#count` line, "Back" link.
- JS draws polar plot: elevation rings at 0/30/60Â°, N/S/E/W labels, dots colored by constellation (`colors=['','#3af','#f86','#5d5','#fd5']` indexed by `sys`), PRN label above each dot. Fetches `GET /skyplot/data` on load and **every 10 s** (`setInterval(update,10000)`).

### `/update` â€” Firmware update (963-1027)
- File picker (`accept=.bin`, hidden + styled "Choose .bin" button + filename span), a hidden progress bar (`#otaBar`, green `#0f0` fill), status text, "Upload & Flash" submit.
- JS `startOTA`: confirm dialog, then `XMLHttpRequest POST /update` with `Content-Type: application/octet-stream`, raw file body; `upload.onprogress` drives the bar; on 200 shows response text, turns bar green, redirects to `/` after 9 s; on error turns bar red.

### `/logs` â€” Console logs (1209-1252)
- Auto-refresh checkbox (5 s, default on), "Refresh now", "Jump to end" buttons; `<pre id=log>` (`background:#080b08;color:#cde`, `max-height:70vh`, monospace). Footer links `/config`, `/`.
- JS: incremental fetch using a `cursor`; `GET /logs/data?since=<cursor>` (or full on first load/truncation). Reads `X-Log-Cursor` and `X-Log-Truncated` headers; appends text, trims client buffer to 16 KB, auto-scrolls if at bottom.

### `/files` â€” SD file browser (1463-1598)
- Top links: `â† Status` (`/`), `RINEX export â†’` (`/rinex/export`).
- **Storage usage bar** (server-rendered): colored progress bar (green/amber/red by 80/95% thresholds) with "used of total (pct%)"; or "calculatingâ€¦"/"not mounted".
- **Breadcrumb** (`#breadcrumb`), **"+ New Folder"** button.
- **Table** (Name / Size / Actions) populated by JS. `var SD_ROOT` injected from `SdManager::kMountPoint` (`/sdcard`).
- JS (`R"JSEOF"` block): `loadDir(path)` â†’ `GET /files/list?path=â€¦`, sorts dirs-first then name, renders `.. ` parent row, `[dir]`/`[file]` rows. Per-row action buttons: **dl** (files only â†’ `dlFile` sets `window.location` to `/files/download?path=`), **rn** (`renameEntry` â†’ `prompt`, `POST /files/rename` JSON `{from,to}`), **del** (`delEntry` â†’ `confirm`, `POST /files/delete` JSON `{path}`). `mkdirPrompt` â†’ `POST /files/mkdir` JSON `{path}`.
- **NOTE for redesign:** current delete/rename are **single-item only** (one `prompt`/`confirm` each). There is **no multi-select and no bulk/directory-delete UI** today â€” that is net-new work the redesign must add on both surfaces. (`delete_entry` in `SdManager` can delete a directory entry, but the web UI only deletes one path per call.)

### `/rinex/export` â€” RINEX export (1739-1847)
- **Self-contained HTML** (not `send_page`; blue `#08f` theme). Links `â† SD Files`, `â† Status`. Optional "No RINEX files found in /rawdata" note. Two `datetime-local` inputs (start/end UTC, prefilled from earliest/latest file found in `/sdcard/rawdata`), "Export & Download" button, `#status` line.
- JS `doExport()`: validates start<end, `POST /rinex/export` JSON `{start,end}`, streams the response body via `reader.read()`, accumulates chunks into a Blob, shows running received-bytes/elapsed, then triggers a client-side download named `rinex_<startcompact>.rnx`.

## 6. Data endpoint shapes (for rebuilding pages)

**`GET /status`** (875-961) â€” flat JSON object. Keys:
`framework, version, healthy(bool), uptime_sec, reset_reason, wifi_connected(bool), ap_active(bool), ssid, ip, rssi, free_heap, heap_total, min_free_heap, mode("base_tx"|"survey"), rtcm_bps, rtcm_total, gps, glonass, galileo, beidou, survey_state("collecting"|"done"|"error"|"idle"), survey_elapsed, survey_samples, survey_blocks, survey_stability, survey_valid(bool), survey_lat, survey_lon, survey_height, local_clients, local_client_ips(string[]), ntrip_enabled(bool), rtk2go{}, onocoy{}, rtkdata{}, position_valid(bool), sd_mounted(bool), sd_total, sd_used, rinex_active(bool), rinex_epochs, rinex_files, rinex_file`.
Each service object (`provider_json`): `{enabled, connected, message, bytes, dropped, reconnects, last_error, connected_sec, ever_sent, last_send_age}`.

**`GET /wifi/scan`** â†’ `[{ssid, rssi, secured(bool)}, â€¦]`. `409` if not in AP mode.

**`GET /skyplot/data`** â†’ `[{prn, el, az, snr, sys(int 1-4)}, â€¦]` (up to 64 sats).

**`GET /files/list?path=`** (from `SdManager::list_dir`) â†’ `[{name, path, is_dir(bool), size(int)}, â€¦]`. Returns `[]` if SD unmounted. `400` "Invalid path" if `safe_path` fails.

**`GET /files/download?path=`** â†’ `application/octet-stream`, `Content-Disposition: attachment`. 503 unmounted, 400 bad path, 404 if missing/dir.

**`GET /logs/data?since=<cursor>`** â†’ `text/plain`; headers `X-Log-Cursor`, `X-Log-Truncated`.

## 7. Write/action endpoints â€” request/response shapes

- **`POST /setup`** â€” form `password`, `confirm` â†’ 303â†’`/` (8-64 chars, must match).
- **`POST /config`** â€” form `r2g_en/onc_en/rtk_en` (checkbox presence), `*_mp`, `*_pw` â†’ calls `save_service`/`set_service_enabled` + `station_->reload_services()`; 303â†’`/config`. Body cap **2048**.
- **`POST /config/position`** â€” form `lat,lon,hgt` (validated âˆ’90..90 / âˆ’180..180 / âˆ’1000..20000 m) â†’ `station_->request_position`; 303â†’`/`. Body cap **512**.
- **`POST /survey`** â€” no body â†’ `station_->request_survey()`; 303â†’`/`.
- **`POST /config/wifi`** â€” form `ssid` (â‰¤32), `password` (â‰¤64, blank keeps saved pw if SSID unchanged) â†’ `save_wifi`; text/plain "WiFi credentials saved. Restarting." + **reboots after ~2.5 s** (`restart_task`). Body cap **512**.
- **`POST /config/ap`** â€” form `ap_pw` (8-63) â†’ `save_ap_password` + `wifi_->apply_ap_settings()` (live, drops current hotspot clients); 303â†’`/config`. Body cap **256**.
- **`POST /config/antenna`** â€” form `ant_model` (â‰¤16, default `HXCGPS500`), `ant_radome` (â‰¤4, default `NONE`), `ant_h` (atof) â†’ `save_antenna`; 303â†’`/config`. Body cap **512**.
- **`POST /update`** â€” raw binary body (octet-stream). Suspends streams, waits 1.1 s, validates size vs OTA partition, writes via `esp_ota_*` in 4 KB recv chunks (timeout-tolerant up to 20 retries), sets boot partition, replies "Update accepted. Restarting." with `Connection: close`, then **reboots after ~2.5 s**. Various 400/500 errors on failure.
- **`POST /rinex/toggle`** â€” form `start=1|0` (also accepts `true`) â†’ `station_->request_raw_collection`; JSON `{"ok":bool}`. Body cap **64**.
- **`POST /ntrip/toggle`** â€” form `on=1|0` (or `true`) â†’ `station_->set_streams_enabled` (persisted); JSON `{"ok":true}`. Body cap **64**.
- **`POST /files/delete`** â€” JSON `{"path":â€¦}` â†’ JSON `{"ok":bool}`. Body cap **512**. Single entry.
- **`POST /files/rename`** â€” JSON `{"from":â€¦,"to":â€¦}` â†’ JSON `{"ok":bool}`. Body cap **512**.
- **`POST /files/mkdir`** â€” JSON `{"path":â€¦}` â†’ JSON `{"ok":true}` or `{"ok":false,"error":â€¦}`. Body cap **512**. `mkdir(...,0755)`.
- **`POST /rinex/export`** â€” JSON `{"start":"YYYY-MM-DDTHH:MM","end":â€¦}` (UTC) â†’ merged `.rnx` octet-stream (`Content-Disposition: attachment; filename="export.rnx"`). Selects `/sdcard/rawdata/BASE_YYYYMMDD_HHMMSS.rnx` files overlapping the window, streams first file's full header (patching `TIME OF LAST OBS` from the last file), then concatenates observation bodies of subsequent files (skipping their headers). 400 bad range, 404 none in range. Body cap **256**.

## 8. Helper utilities available for new pages

Member helpers (in `web_server.hpp`/.cpp):
- `read_body(req, max_len)` â€” bounded POST body reader, timeout-tolerant; returns `{}` if over cap or on error.
- `form_value(body, name)` â€” extract one `application/x-www-form-urlencoded` field (url-decoded).
- `url_decode(value)` â€” `+`â†’space and `%XX` decoding.
- `html_escape(value)` â€” escapes `& < > " '`. (Note: a **duplicate** file-scope `escape_html()` exists at line 190 with identical behavior â€” used by `service_html`/`rssi_html`.)
- `json_escape(value)` â€” JSON string escaping (control chars, quotes, backslash).
- `query_param(req, key)` â€” parse one URL query parameter (url-decoded).
- `json_field(body, key)` â€” minimal `"key":"value"` extractor for **unescaped** JSON string values (paths only; no nesting/escape handling). `rinex_export_handler` has its own inline copy of the same logic.

File-scope formatting helpers (anonymous namespace): `send_chunks` (1 KB chunking), `human_bytes`, `uptime_str`, `ipv4_to_string`, `local_client_ips_html/json`, `service_html`, `rssi_html`, `reset_reason_str`, `parse_double`, `restart_task` (2.5 s delay + `esp_restart`), plus RINEX time helpers `utc_to_unix`, `parse_datetime_input`, `parse_rinex_filename_utc`, `select_rinex_files`.

External data sources a page can pull from: `storage_` (position, wifi creds, per-service creds + enabled, admin/ap pw, antenna model/radome/height), `wifi_` (connected/ap_active/ssid/rssi/ip/scan_networks/apply_ap_settings), `station_` (`status()` â†’ `BaseStationStatus` with survey snapshot, RTCM counters, NTRIP service statuses, local clients/IPs; `healthy()`, `streams_enabled()`, `rinex_status()`, `satellites()`, request_position/survey/raw_collection, set_streams_enabled/suspended, reload_services), `sd_` (`is_mounted`, `disk_stats` {total_bytes, used_bytes, valid}, `list_dir`, `delete_entry`, `rename_entry`, `safe_path`, `kMountPoint="/sdcard"`).

## 9. Constraints a redesign MUST respect

1. **No template engine / no bundler.** Every byte of HTML/CSS/JS is C++ string literals concatenated at request time, or raw-string blocks. A shared theme should be one reusable CSS block in `kAfterTitle` (or a single served `.css`), not per-page inline styles. **Today the theme is duplicated/diverged** across `send_page`, `/files`, and `/rinex/export` â€” unify it.
2. **Chunked streaming.** Pages are streamed in 1 KB chunks (`send_chunks`); binary downloads in 4 KB chunks with `vTaskDelay(1 tick)` pacing (~400 KB/s) to avoid esp_hosted SDIO/TCP overflow. Any large new asset must stream, not buffer whole.
3. **Tight heap.** `MALLOC_CAP_8BIT` free heap is a first-class status metric; OTA suspends streams and waits before flashing. Avoid building large `std::string`s; keep CSS/JS compact. App partition is 6 MB â€” keep flash footprint modest. Embedded fonts/images are discouraged.
4. **No client libraries.** All JS today is vanilla, inline, framework-free (fetch + XHR). No CDN â€” the device is often only reachable on its own AP/LAN. A redesign should stay self-contained (no external `<script src>`/`<link href>` to the internet).
5. **HTTPS-only admin + Basic Auth.** Every page/data/action endpoint must call `authorize()`; HTTP port 80 only redirects or serves the AP-provisioning whitelist. New endpoints must register in `register_secure_handlers()` (count must stay â‰¤ `max_uri_handlers=32` â€” there are currently 29 secure handlers, leaving ~3 slots; raise the cap if adding more) and, if needed during provisioning, in the HTTP gateway whitelist.
6. **Single-file C++.** Adding pages means adding `static esp_err_t *_handler` declarations to `web_server.hpp`, the registration entry, and the handler body. No file-based routing.
7. **Polling cadence already established:** Status 15 s, sky plot 10 s, logs 5 s. Keep `Cache-Control: no-store` on data endpoints.
8. **Body-size caps** are enforced per endpoint via `read_body(req, cap)` â€” new forms must keep within or raise their cap explicitly.
9. **Minimal JSON parsing** â€” `json_field` only handles flat `"k":"v"` string fields with no escaping; complex request bodies need a more robust parser or staying with form-encoding.
10. **Functionality to preserve** (per project decisions): everything above. Web-only capabilities that need not appear on the touchscreen: file download (`/files/download`), OTA upload (`/update`), bulk RINEX export (`/rinex/export`), full console log view (`/logs`), CA cert download (`/ca.crt`).

**Key gaps the redesign introduces vs. today:** (a) no unified nav/IA â€” links are ad-hoc; (b) three divergent visual themes; (c) bulk multi-select + whole-directory delete does **not** exist in the web file browser yet (single-item only); (d) JS reimplements C++ formatting (status cell rendering) â€” a unified design may want one source of truth.
