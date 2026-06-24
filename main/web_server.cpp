#include "web_server.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <dirent.h>
#include <string_view>
#include <sys/stat.h>
#include <sys/types.h>
#include <vector>

#include "app_config.hpp"
#include "base_station.hpp"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_https_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "log_buffer.hpp"
#include "sd_delete.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "mbedtls/base64.h"

namespace {

constexpr char kTag[] = "web";
constexpr char kAdminUser[] = "admin";
constexpr char kRawDataDir[] = "/sdcard/rawdata";

// Convert a Gregorian UTC date/time to Unix seconds.  Accurate for 2000–2099.
time_t utc_to_unix(int Y, int Mo, int D, int h, int m, int s) {
    static const int moff[] = {0,31,59,90,120,151,181,212,243,273,304,334};
    int y = Y - 1970;
    int leaps = y > 0 ? (y - 1) / 4 + 1 : 0;
    bool isleap = (Y % 4 == 0 && (Y % 100 != 0 || Y % 400 == 0));
    long days = (long)y * 365 + leaps + moff[Mo - 1] + (Mo > 2 && isleap ? 1 : 0) + (D - 1);
    return (time_t)(days * 86400L + h * 3600L + m * 60L + s);
}

// Parse "YYYY-MM-DDTHH:MM" or "YYYY-MM-DDTHH:MM:SS" (UTC) → Unix time.
time_t parse_datetime_input(const char *s) {
    int Y = 0, Mo = 0, D = 0, h = 0, m = 0, sec = 0;
    if (sscanf(s, "%d-%d-%dT%d:%d:%d", &Y, &Mo, &D, &h, &m, &sec) < 5) return -1;
    if (Y < 2020 || Y > 2099 || Mo < 1 || Mo > 12 || D < 1 || D > 31) return -1;
    return utc_to_unix(Y, Mo, D, h, m, sec);
}

// Parse "BASE_YYYYMMDD_HHMMSS.rnx" (GPS time ≈ UTC for selection) → Unix time.
time_t parse_rinex_filename_utc(const char *name) {
    int Y, Mo, D, h, m, s;
    if (sscanf(name, "BASE_%4d%2d%2d_%2d%2d%2d.rnx", &Y, &Mo, &D, &h, &m, &s) != 6) return -1;
    if (Y < 2020) return -1;
    return utc_to_unix(Y, Mo, D, h, m, s);
}

// Return sorted list of rawdata .rnx paths whose 1-hour window overlaps [start, end].
std::vector<std::string> select_rinex_files(time_t start, time_t end) {
    DIR *dir = opendir(kRawDataDir);
    if (!dir) return {};
    constexpr time_t kFileSpan = 3600 + 120;  // file duration + 2-min margin
    constexpr time_t kMargin   = 120;
    std::vector<std::pair<time_t, std::string>> found;
    struct dirent *ent;
    while ((ent = readdir(dir)) != nullptr) {
        if (ent->d_type != DT_REG) continue;
        time_t ft = parse_rinex_filename_utc(ent->d_name);
        if (ft < 0) continue;
        if (ft < end + kMargin && ft + kFileSpan > start - kMargin) {
            char path[280];
            snprintf(path, sizeof(path), "%s/%s", kRawDataDir, ent->d_name);
            found.push_back({ft, path});
        }
    }
    closedir(dir);
    std::sort(found.begin(), found.end());
    std::vector<std::string> paths;
    paths.reserve(found.size());
    for (auto &p : found) paths.push_back(std::move(p.second));
    return paths;
}

extern const unsigned char server_cert_start[]
    asm("_binary_server_cert_pem_start");
extern const unsigned char server_cert_end[]
    asm("_binary_server_cert_pem_end");
extern const unsigned char server_key_start[]
    asm("_binary_server_key_pem_start");
extern const unsigned char server_key_end[]
    asm("_binary_server_key_pem_end");
extern const unsigned char ca_cert_start[]
    asm("_binary_ca_cert_pem_start");
extern const unsigned char ca_cert_end[]
    asm("_binary_ca_cert_pem_end");

esp_err_t send_chunks(httpd_req_t *request, std::string_view data) {
    constexpr size_t kChunkSize = 1024;
    for (size_t offset = 0; offset < data.size(); offset += kChunkSize) {
        const size_t length = std::min(kChunkSize, data.size() - offset);
        const esp_err_t result =
            httpd_resp_send_chunk(request, data.data() + offset, length);
        if (result != ESP_OK) return result;
    }
    return ESP_OK;
}

std::string human_bytes(uint64_t bytes) {
    char text[32];
    if (bytes >= 1024ULL * 1024ULL) {
        snprintf(text, sizeof(text), "%.1f MB",
                 static_cast<double>(bytes) / (1024.0 * 1024.0));
    } else if (bytes >= 1024) {
        snprintf(text, sizeof(text), "%.1f KB",
                 static_cast<double>(bytes) / 1024.0);
    } else {
        snprintf(text, sizeof(text), "%llu B",
                 static_cast<unsigned long long>(bytes));
    }
    return text;
}

const char *reset_reason_str(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_POWERON:   return "power-on";
        case ESP_RST_EXT:       return "external";
        case ESP_RST_SW:        return "software";
        case ESP_RST_PANIC:     return "panic/exception";
        case ESP_RST_INT_WDT:   return "interrupt watchdog";
        case ESP_RST_TASK_WDT:  return "task watchdog";
        case ESP_RST_WDT:       return "other watchdog";
        case ESP_RST_DEEPSLEEP: return "deep-sleep wake";
        case ESP_RST_BROWNOUT:  return "brownout";
        case ESP_RST_SDIO:      return "SDIO";
        default:                return "unknown";
    }
}

std::string ipv4_to_string(uint32_t ipv4) {
    if (!ipv4) return {};
    const uint32_t host = ntohl(ipv4);
    char text[16];
    snprintf(text, sizeof(text), "%u.%u.%u.%u",
             static_cast<unsigned>((host >> 24) & 0xff),
             static_cast<unsigned>((host >> 16) & 0xff),
             static_cast<unsigned>((host >> 8) & 0xff),
             static_cast<unsigned>(host & 0xff));
    return text;
}

std::string local_client_ips_json(
    const LocalCaster::ClientSnapshot &clients) {
    std::string out = "[";
    for (int i = 0; i < clients.count; ++i) {
        if (i) out += ",";
        out += "\"" + ipv4_to_string(clients.ipv4[i]) + "\"";
    }
    out += "]";
    return out;
}

bool parse_double(const std::string &text, double &value) {
    if (text.empty()) return false;
    errno = 0;
    char *end = nullptr;
    value = strtod(text.c_str(), &end);
    return errno == 0 && end != text.c_str() && *end == '\0' &&
           std::isfinite(value);
}

void restart_task(void *) {
    vTaskDelay(pdMS_TO_TICKS(2500));
    esp_restart();
}

// Wrap a C string as a double-quoted JS/JSON string literal (escapes ", \,
// control chars; passes UTF-8 bytes >=0x20 through unchanged). Used to inject the
// shared ui_sections::del::* wording — which contains multibyte UTF-8 (…, —) — into
// the page's inline <script> so the web matches the touch UI verbatim.
std::string json_string_literal(const char *text) {
    std::string out = "\"";
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(text);
         *p; ++p) {
        const unsigned char c = *p;
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c >= 0x20) out.push_back(static_cast<char>(c));
        }
    }
    out += "\"";
    return out;
}

}  // namespace

esp_err_t AdminWebServer::start(
    Storage &storage, WifiManager &wifi,
    BaseStation &station, SdManager &sd) {
    storage_ = &storage;
    wifi_ = &wifi;
    station_ = &station;
    sd_ = &sd;

    httpd_ssl_config_t tls_config = HTTPD_SSL_CONFIG_DEFAULT();
    // Raised from 32: the secure handler table grew past 30 with the bulk-delete
    // routes (/files/preview, /files/delete-batch, /files/delete-status). 40 gives
    // headroom for future routes without re-tuning.
    tls_config.httpd.max_uri_handlers = 40;
    tls_config.httpd.stack_size = 12288;
    tls_config.httpd.recv_wait_timeout = 30;
    tls_config.httpd.send_wait_timeout = 30;
    tls_config.httpd.lru_purge_enable = false;
    // Cap concurrent TLS sessions hard. On this board malloc()/std::string AND the
    // mbedtls/esp-aes DMA buffers all live in the SAME scarce internal SRAM, so every
    // extra simultaneous TLS handshake competes with page-building for it. At 8, a
    // browser's parallel connections + the /status poll + NTRIP exhausted internal
    // SRAM; with C++ exceptions off a failed alloc became abort() → a reboot loop
    // (esp-aes "Failed to allocate memory" / mbedtls -0x0084 just before each abort).
    // Two sockets are enough for the admin page plus one polling request. The ESP32-P4
    // AES/TLS path can still run out of internal DMA-capable memory during caster
    // reconnects, so keep browser handshake concurrency deliberately low.
    tls_config.httpd.max_open_sockets = 2;
    tls_config.httpd.keep_alive_enable = false;
    tls_config.servercert = server_cert_start;
    tls_config.servercert_len = server_cert_end - server_cert_start;
    tls_config.prvtkey_pem = server_key_start;
    tls_config.prvtkey_len = server_key_end - server_key_start;
    ESP_RETURN_ON_ERROR(
        httpd_ssl_start(&https_server_, &tls_config),
        kTag, "HTTPS start failed");
    ESP_RETURN_ON_ERROR(
        register_secure_handlers(), kTag, "HTTPS URI registration failed");

    httpd_config_t http_config = HTTPD_DEFAULT_CONFIG();
    http_config.server_port = 80;
    http_config.ctrl_port = 32768;
    http_config.max_uri_handlers = 3;
    http_config.max_open_sockets = 1;
    http_config.backlog_conn = 1;
    http_config.stack_size = 4096;
    http_config.lru_purge_enable = true;
    http_config.uri_match_fn = httpd_uri_match_wildcard;
    esp_err_t gateway_result = httpd_start(&http_server_, &http_config);
    if (gateway_result == ESP_OK) {
        const httpd_uri_t gateway_get{
            "/*", HTTP_GET, http_gateway_handler, this};
        const httpd_uri_t gateway_post{
            "/*", HTTP_POST, http_gateway_handler, this};
        gateway_result =
            httpd_register_uri_handler(http_server_, &gateway_get);
        if (gateway_result == ESP_OK) {
            gateway_result =
                httpd_register_uri_handler(http_server_, &gateway_post);
        }
    }
    if (gateway_result != ESP_OK) {
        ESP_LOGW(
            kTag, "HTTP recovery gateway unavailable: %s",
            esp_err_to_name(gateway_result));
        if (http_server_) {
            httpd_stop(http_server_);
            http_server_ = nullptr;
        }
    }

    ESP_LOGI(kTag, "HTTPS administration server listening on port 443");
    if (http_server_) {
        ESP_LOGI(
            kTag, "HTTP redirect/AP provisioning gateway listening on port 80");
    }
    return ESP_OK;
}

esp_err_t AdminWebServer::register_secure_handlers() {
    const httpd_uri_t handlers[] = {
        {"/", HTTP_GET, root_handler, this},
        {"/setup", HTTP_GET, setup_get_handler, this},
        {"/setup", HTTP_POST, setup_post_handler, this},
        {"/config", HTTP_GET, config_get_handler, this},
        {"/config", HTTP_POST, config_post_handler, this},
        {"/position", HTTP_GET, position_page_handler, this},
        {"/system", HTTP_GET, system_page_handler, this},
        {"/wifi/scan", HTTP_GET, wifi_scan_handler, this},
        {"/skyplot", HTTP_GET, skyplot_handler, this},
        {"/skyplot/data", HTTP_GET, skyplot_data_handler, this},
        {"/status", HTTP_GET, status_handler, this},
        {"/update", HTTP_GET, update_page_handler, this},
        {"/update", HTTP_POST, update_upload_handler, this},
        {"/config/position", HTTP_POST, position_handler, this},
        {"/survey", HTTP_POST, survey_handler, this},
        {"/config/wifi", HTTP_POST, wifi_handler, this},
        {"/config/ap", HTTP_POST, ap_password_handler, this},
        {"/config/antenna", HTTP_POST, antenna_handler, this},
        {"/logs", HTTP_GET, logs_page_handler, this},
        {"/logs/data", HTTP_GET, logs_data_handler, this},
        {"/ca.crt", HTTP_GET, ca_certificate_handler, this},
        {"/files", HTTP_GET, files_page_handler, this},
        {"/files/list", HTTP_GET, files_list_handler, this},
        {"/files/download", HTTP_GET, files_download_handler, this},
        {"/files/delete", HTTP_POST, files_delete_handler, this},
        {"/files/preview", HTTP_POST, files_preview_handler, this},
        {"/files/delete-batch", HTTP_POST, files_delete_batch_handler, this},
        {"/files/delete-status", HTTP_GET, files_delete_status_handler, this},
        {"/files/rename", HTTP_POST, files_rename_handler, this},
        {"/files/mkdir", HTTP_POST, files_mkdir_handler, this},
        {"/rinex/toggle", HTTP_POST, rinex_toggle_handler, this},
        {"/rinex/export", HTTP_GET,  rinex_export_page_handler, this},
        {"/rinex/export", HTTP_POST, rinex_export_handler, this},
        {"/ntrip/toggle", HTTP_POST, ntrip_toggle_handler, this},
        {"/theme", HTTP_POST, theme_post_handler, this},
    };
    for (const auto &handler : handlers) {
        ESP_RETURN_ON_ERROR(
            httpd_register_uri_handler(https_server_, &handler),
            kTag, "URI registration failed");
    }
    return ESP_OK;
}

void AdminWebServer::stop() {
    if (http_server_) {
        httpd_stop(http_server_);
        http_server_ = nullptr;
    }
    if (https_server_) {
        httpd_ssl_stop(https_server_);
        https_server_ = nullptr;
    }
}

esp_err_t AdminWebServer::http_gateway_handler(httpd_req_t *request) {
    AdminWebServer *server = self(request);
    if (request->method == HTTP_GET &&
        std::string(request->uri) == "/ca.crt") {
        return ca_certificate_handler(request);
    }
    if (!server->wifi_->access_point_active()) {
        char host[96]{};
        if (httpd_req_get_hdr_value_str(
                request, "Host", host, sizeof(host)) != ESP_OK ||
            host[0] == '\0') {
            strlcpy(host, "gps-base.local", sizeof(host));
        }
        if (char *port = strchr(host, ':')) *port = '\0';
        const std::string location =
            "https://" + std::string(host) + request->uri;
        httpd_resp_set_status(request, "308 Permanent Redirect");
        httpd_resp_set_hdr(request, "Location", location.c_str());
        httpd_resp_set_type(request, "text/plain");
        return httpd_resp_sendstr(request, "Redirecting to HTTPS");
    }

    const std::string uri = request->uri;
    if (request->method == HTTP_GET) {
        if (uri == "/") return root_handler(request);
        if (uri == "/setup") return setup_get_handler(request);
        if (uri == "/config") return config_get_handler(request);
        if (uri == "/wifi/scan") return wifi_scan_handler(request);
        if (uri == "/status") return status_handler(request);
        if (uri == "/ca.crt") return ca_certificate_handler(request);
    } else if (request->method == HTTP_POST) {
        if (uri == "/setup") return setup_post_handler(request);
        if (uri == "/config/wifi") return wifi_handler(request);
    }
    return httpd_resp_send_err(
        request, HTTPD_404_NOT_FOUND,
        "Only WiFi provisioning is available over HTTP");
}


esp_err_t AdminWebServer::ca_certificate_handler(httpd_req_t *request) {
    httpd_resp_set_type(request, "application/x-x509-ca-cert");
    httpd_resp_set_hdr(
        request, "Content-Disposition",
        "attachment; filename=\"gps-base-ca.crt\"");
    httpd_resp_set_hdr(request, "Cache-Control", "public, max-age=86400");
    return httpd_resp_send(
        request, reinterpret_cast<const char *>(ca_cert_start),
        ca_cert_end - ca_cert_start - 1);
}

esp_err_t AdminWebServer::root_handler(httpd_req_t *request) {
    AdminWebServer *server = self(request);
    if (!server->storage_->admin_password_set()) {
        httpd_resp_set_status(request, "303 See Other");
        httpd_resp_set_hdr(request, "Location", "/setup");
        return httpd_resp_send(request, nullptr, 0);
    }
    if (!server->authorize(request)) return server->send_unauthorized(request);

    std::string content =
        // Read-only deep-link tile grid mirroring the touch Dashboard. Every tile
        // is an <a> into its owning section; values are rebuilt from /status (15s).
        "<div class='grid'>"
        // Hero — RTCM output (spans 2 at wide). Links into the broadcast section.
        "<a class='tile hero' href='/config'>"
        "<p class='tlabel'>RTCM Output</p>"
        "<div><span id='d-rtcm' class='tval idle'>--</span>"
        "<span class='tunit'>B/s</span></div>"
        "<p class='tsub' id='d-rtcm-sub'>total --</p></a>"
        // Mode
        "<a class='tile' href='/skyplot'>"
        "<p class='tlabel'>Mode</p>"
        "<div><span id='d-mode' class='tval idle'>--</span></div>"
        "<p class='tsub'>Survey / Base TX</p></a>"
        // Satellites
        "<a class='tile' href='/skyplot'>"
        "<p class='tlabel'>Satellites</p>"
        "<div><span id='d-sats' class='tval'>--</span></div>"
        "<p class='tsub' id='d-sats-sub'>--</p></a>"
        // NTRIP push — three per-service lights (dot + name + reconnect count).
        // green=connected, red=enabled-but-down, dim=disabled. Links to Links.
        "<a class='tile' href='/config'>"
        "<p class='tlabel'>NTRIP Push</p>"
        "<div class='svclist'>"
        "<span class='svc'><span id='d-r2g-dot' class='dot idle'></span>"
        "RTK2go<span id='d-r2g-rc' class='svc-rc'></span></span>"
        "<span class='svc'><span id='d-onc-dot' class='dot idle'></span>"
        "Onocoy<span id='d-onc-rc' class='svc-rc'></span></span>"
        "<span class='svc'><span id='d-rtk-dot' class='dot idle'></span>"
        "RTKdata<span id='d-rtk-rc' class='svc-rc'></span></span>"
        "</div>"
        "<p class='tsub' id='d-ntrip-sub'>-- clients</p></a>"
        // Position
        "<a class='tile' href='/position'>"
        "<p class='tlabel'>Position</p>"
        "<div><span id='d-pos' class='tval' style='font-size:clamp(16px,3vw,22px)'>--</span></div>"
        "<p class='tsub' id='d-pos-sub'>fixed base</p></a>"
        // SD / RINEX
        "<a class='tile' href='/files'>"
        "<p class='tlabel'>SD / RINEX</p>"
        "<div><span id='d-sd' class='tval' style='font-size:clamp(16px,3.5vw,24px)'>--</span></div>"
        "<div class='track' style='margin-top:8px'><div id='d-sd-bar' "
        "style='width:0%;background:var(--good)'></div></div>"
        "<p class='tsub'><span id='d-rinex' class='pill idle'><span class='dot'></span>"
        "<span id='d-rinex-txt'>RINEX --</span></span></p></a>"
        // Link (WiFi / AP)
        "<a class='tile' href='/logs'>"
        "<p class='tlabel'>Link</p>"
        "<div><span id='d-link' class='tval' style='font-size:clamp(16px,3vw,22px)'>--</span></div>"
        "<p class='tsub' id='d-link-sub'>--</p></a>"
        "</div>"
        // Survey action — the operator's most common deliberate action.
        "<div style='margin:16px 0'>"
        "<button onclick='startSurvey()' style='background:var(--surface-hi)'>"
        "Start / Restart Survey</button></div>"
        "<footer>"
        "ESP-IDF " + std::string(esp_get_idf_version()) +
        " &middot; FW " + std::string(esp_app_get_description()->version) +
        " &middot; <a href='/status'>JSON status</a>"
        "</footer>"
        R"HTML(<script>
let statusRequest=false;
function bytes(v){return v>=1048576?(v/1048576).toFixed(1)+' MB':v>=1024?(v/1024).toFixed(1)+' KB':v+' B';}
function set(id,v){const e=document.getElementById(id);if(e)e.textContent=v;}
// Set the colored value class on a tile value (good/warn/crit/idle).
function sev(id,s){const e=document.getElementById(id);if(e){e.className='tval '+s;}}
function pill(id,txtId,s,txt){const e=document.getElementById(id);if(e)e.className='pill '+s;set(txtId,txt);}
// The global header pill is driven by the shared nav poller (navPoll in nav_html),
// which runs on every page. This dashboard poll only updates the tile readouts.
async function refresh(){
 if(statusRequest)return;statusRequest=true;
 try{
  const r=await fetch('/status',{cache:'no-store'});if(!r.ok)throw Error(r.status);
  const d=await r.json();
  // RTCM hero — green when transmitting, idle when not.
  const tx=d.mode==='base_tx';
  set('d-rtcm',d.rtcm_bps);sev('d-rtcm',tx&&d.rtcm_bps>0?'good':'idle');
  set('d-rtcm-sub','total '+bytes(d.rtcm_total));
  // Mode
  set('d-mode',tx?'Base TX':'Survey');sev('d-mode',tx?'good':'warn');
  // Satellites
  const total=d.gps+d.glonass+d.galileo+d.beidou;
  set('d-sats',total);
  set('d-sats-sub','G'+d.gps+' R'+d.glonass+' E'+d.galileo+' C'+d.beidou);
  // NTRIP push — three per-service lights + reconnect counts.
  // green=connected, red=enabled-but-down, dim=disabled (matches touch).
  function svclight(dotId,rcId,s){
   const dot=document.getElementById(dotId);
   let cls=s.connected?'good':s.enabled?'crit':'idle';
   if(dot)dot.className='dot '+cls;
   const rc=document.getElementById(rcId);
   if(rc)rc.textContent=(s.reconnects>0)?' ↻'+s.reconnects:'';
   return cls;
  }
  svclight('d-r2g-dot','d-r2g-rc',d.rtk2go);
  svclight('d-onc-dot','d-onc-rc',d.onocoy);
  svclight('d-rtk-dot','d-rtk-rc',d.rtkdata);
  set('d-ntrip-sub',d.local_clients+' clients');
  // Position
  if(d.position_valid){set('d-pos',d.position_lat.toFixed(6)+', '+d.position_lon.toFixed(6));}
  else set('d-pos','not set');
  set('d-pos-sub',d.position_valid?'fixed base':'no fixed position');
  // SD / disk
  if(!d.sd_mounted){set('d-sd','no card');sev('d-sd','crit');document.getElementById('d-sd-bar').style.width='0%';}
  else if(!d.sd_total){set('d-sd','mounted');sev('d-sd','good');}
  else{const sp=Math.round(d.sd_used*100/d.sd_total),sc=sp<80?'good':sp<95?'warn':'crit';
   set('d-sd',sp+'% used');sev('d-sd',sc);
   const bar=document.getElementById('d-sd-bar');bar.style.width=sp+'%';
   bar.style.background='var(--'+sc+')';}
  pill('d-rinex','d-rinex-txt',d.rinex_active?'good':'idle',d.rinex_active?'RINEX LIVE':'RINEX OFF');
  // Link
  if(d.wifi_connected){set('d-link',d.ssid||'WiFi');set('d-link-sub',(d.ip||'')+' · '+d.rssi+' dBm');sev('d-link',d.rssi>-75?'good':'warn');}
  else if(d.ap_active){set('d-link','AP mode');set('d-link-sub','GPS-BaseStation');sev('d-link','warn');}
  else{set('d-link','offline');set('d-link-sub','no network');sev('d-link','crit');}
 }catch(e){}finally{statusRequest=false;setTimeout(refresh,15000);}
}
async function startSurvey(){
 if(!confirm('Start a new survey-in? This leaves Base TX and stops RTCM output until the survey completes.'))return;
 try{await fetch('/survey',{method:'POST'});}catch(e){}
 setTimeout(refresh,500);
}
refresh();
</script>)HTML";
    return server->send_page(
        request, "GPS Base Station", content, "/");
}

esp_err_t AdminWebServer::setup_get_handler(httpd_req_t *request) {
    AdminWebServer *server = self(request);
    if (server->storage_->admin_password_set()) {
        httpd_resp_set_status(request, "303 See Other");
        httpd_resp_set_hdr(request, "Location", "/");
        return httpd_resp_send(request, nullptr, 0);
    }
    const std::string content =
        "<h2>First-time Setup</h2>"
        "<form method='post' action='/setup'>"
        "<p><input name='password' type='password' minlength='8' "
        "placeholder='Administration password' required></p>"
        "<p><input name='confirm' type='password' minlength='8' "
        "placeholder='Confirm password' required></p>"
        "<button>Set Password</button></form>";
    return server->send_page(request, "Setup", content);
}

esp_err_t AdminWebServer::setup_post_handler(httpd_req_t *request) {
    AdminWebServer *server = self(request);
    if (server->storage_->admin_password_set()) {
        return httpd_resp_send_err(request, HTTPD_403_FORBIDDEN, "Already set");
    }
    const std::string body = read_body(request, 512);
    const std::string password = form_value(body, "password");
    if (password.size() < 8 || password.size() > 64 ||
        password != form_value(body, "confirm")) {
        return httpd_resp_send_err(
            request, HTTPD_400_BAD_REQUEST, "Passwords do not match");
    }
    ESP_RETURN_ON_ERROR(
        server->storage_->save_admin_password(password),
        kTag, "Password save failed");
    httpd_resp_set_status(request, "303 See Other");
    httpd_resp_set_hdr(request, "Location", "/");
    return httpd_resp_send(request, nullptr, 0);
}

esp_err_t AdminWebServer::config_get_handler(httpd_req_t *request) {
    AdminWebServer *server = self(request);
    if (!server->authorize(request)) return server->send_unauthorized(request);
    const ServiceCredentials r2g = server->storage_->load_service("rtk2go");
    const ServiceCredentials onc = server->storage_->load_service("onocoy");
    const ServiceCredentials rtk = server->storage_->load_service("rtkdata");
    auto checked = [server](const char *name) {
        return server->storage_->service_enabled(name) ? " checked" : "";
    };
    const char *master_checked =
        server->station_->streams_enabled() ? " checked" : "";
    const std::string content =
        "<div class='sec'><h2>NTRIP Push</h2>"
        "<p class='dim'>Push your RTCM corrections to public/private NTRIP "
        "casters. Position &amp; antenna live in Position; WiFi &amp; hotspot in "
        "System.</p>"
        "<p><label><input style='width:auto' type='checkbox' id='ntrip_master'" +
        std::string(master_checked) +
        " onchange='toggleMaster(this)'> <b>Enable NTRIP push (master)</b></label></p>"
        "<p class='dim' id='ntrip_master_msg' style='min-height:1.2em'></p>"
        "</div>"
        "<div class='sec'><h2>NTRIP Services</h2>"
        "<form method='post' action='/config'>"
        "<p><label><input style='width:auto' type='checkbox' name='r2g_en'" +
        std::string(checked("rtk2go")) + "> Enable RTK2go</label></p>"
        "<p><input name='r2g_mp' placeholder='RTK2go mountpoint' value='" +
        html_escape(r2g.mountpoint) + "'></p>"
        "<p><input name='r2g_pw' type='password' "
        "placeholder='Leave blank to keep password'></p>"
        "<p><label><input style='width:auto' type='checkbox' name='onc_en'" +
        std::string(checked("onocoy")) + "> Enable Onocoy</label></p>"
        "<p><input name='onc_mp' placeholder='Onocoy source mountpoint' value='" +
        html_escape(onc.mountpoint) + "'></p>"
        "<p><input name='onc_pw' type='password' "
        "placeholder='Leave blank to keep password'></p>"
        "<p><label><input style='width:auto' type='checkbox' name='rtk_en'" +
        std::string(checked("rtkdata")) + "> Enable RTKdata.online</label></p>"
        "<p><input name='rtk_mp' placeholder='RTKdata mountpoint' value='" +
        html_escape(rtk.mountpoint) + "'></p>"
        "<p><input name='rtk_pw' type='password' "
        "placeholder='Leave blank to keep password'></p>"
        "<button>Save Services</button></form></div>"
        "<script>"
        "async function toggleMaster(cb){"
        "var m=document.getElementById('ntrip_master_msg');"
        "cb.disabled=true;m.textContent='Saving…';"
        "try{var r=await fetch('/ntrip/toggle',{method:'POST',"
        "headers:{'Content-Type':'application/x-www-form-urlencoded'},"
        "body:'on='+(cb.checked?'1':'0')});"
        "if(!r.ok)throw 0;"
        "m.textContent=cb.checked?'NTRIP push enabled.':'NTRIP push disabled.';}"
        "catch(e){cb.checked=!cb.checked;m.textContent='Save failed.';}"
        "cb.disabled=false;}"
        "</script>";
    return server->send_page(
        request, "Links", content, "/config");
}

// GET /position — Position Setup: manual position, survey, RINEX collection,
// antenna, and a link to RINEX export. Forms moved here from the old /config.
esp_err_t AdminWebServer::position_page_handler(httpd_req_t *request) {
    AdminWebServer *server = self(request);
    if (!server->authorize(request)) return server->send_unauthorized(request);
    const BasePosition position = server->storage_->load_position();
    char lat[32]{}, lon[32]{}, height[32]{};
    if (position.valid) {
        snprintf(lat, sizeof(lat), "%.8f", position.lat);
        snprintf(lon, sizeof(lon), "%.8f", position.lon);
        snprintf(height, sizeof(height), "%.4f", position.height);
    }
    const bool rinex_active = server->station_->rinex_status().active;
    // "Antenna in use" for the status header — read server-side (it isn't in
    // /status). The position/mode/survey readouts below are live from /status.
    std::string antenna_in_use = server->storage_->antenna_model();
    if (antenna_in_use.empty()) antenna_in_use = "not set";
    const std::string antenna_radome = server->storage_->antenna_radome();
    const std::string content =
        // ── Live current-position status (top) — polled from /status ────────────
        "<div class='sec'><h2>Current Position</h2>"
        "<div class='grid'>"
        "<div class='tile'><p class='tlabel'>Coordinate</p>"
        "<div><span id='p-coord' class='tval' "
        "style='font-size:clamp(15px,2.6vw,20px)'>--</span></div>"
        "<p class='tsub' id='p-height'>height --</p></div>"
        "<div class='tile'><p class='tlabel'>Base Mode</p>"
        "<div><span id='p-mode' class='tval idle' "
        "style='font-size:clamp(16px,3vw,22px)'>--</span></div>"
        "<p class='tsub' id='p-mode-sub'>survey / base TX</p></div>"
        "<div class='tile'><p class='tlabel'>Survey-In</p>"
        "<div><span id='p-survey' class='tval' "
        "style='font-size:clamp(16px,3vw,22px)'>--</span></div>"
        "<div class='track' style='margin-top:8px'><div id='p-survey-bar' "
        "style='width:0%;background:var(--accent)'></div></div>"
        "<p class='tsub' id='p-survey-sub'>--</p></div>"
        "<div class='tile'><p class='tlabel'>Antenna In Use</p>"
        "<div><span class='tval' style='font-size:clamp(15px,2.6vw,20px)'>" +
        html_escape(antenna_in_use) + "</span></div>"
        "<p class='tsub'>radome " +
        html_escape(antenna_radome.empty() ? std::string("NONE") : antenna_radome) +
        "</p></div>"
        "</div></div>"
        "<div class='sec'><h2>Manual Position</h2>"
        "<p class='dim'>Set a known fixed base coordinate (ITRF/WGS84). Leaves "
        "survey mode and starts Base TX once stored.</p>"
        "<form method='post' action='/config/position'>"
        "<p><input name='lat' placeholder='Latitude' value='" +
        std::string(lat) + "'></p>"
        "<p><input name='lon' placeholder='Longitude' value='" +
        std::string(lon) + "'></p>"
        "<p><input name='hgt' placeholder='Ellipsoidal height (m)' value='" +
        std::string(height) + "'></p>"
        "<button>Save Position</button></form></div>"
        "<div class='sec'><h2>Survey</h2>"
        "<p class='dim'>Survey-in averages the receiver position over time, then "
        "switches to Base TX. This restarts the survey from scratch.</p>"
        "<form method='post' action='/survey'>"
        "<button>Start New Survey</button></form></div>"
        "<div class='sec'><h2>RINEX Collection</h2>"
        "<p class='dim'>Logs raw observations to hourly RINEX files on the SD "
        "card for post-processing (OPUS / CSRS-PPP).</p>"
        "<p><span id='rx-state' class='pill " +
        std::string(rinex_active ? "good" : "idle") + "'><span class='dot'></span>"
        "<span id='rx-txt'>" +
        std::string(rinex_active ? "RINEX LIVE" : "RINEX OFF") + "</span></span></p>"
        "<p><button type='button' id='rx-start' onclick='rxToggle(true)'>Start "
        "Logging</button> "
        "<button type='button' id='rx-stop' onclick='rxToggle(false)'>Stop "
        "Logging</button></p>"
        "<p class='dim' id='rx-msg' style='min-height:1.2em'></p></div>"
        "<div class='sec'><h2>Antenna (RINEX)</h2>"
        "<p class='dim'>Written into the RINEX header so OPUS / CSRS-PPP apply "
        "phase-centre corrections automatically. Model must match the IGS/NGS "
        "antenna name. Height is the ARP delta-H (0 = solve for the ARP).</p>"
        "<form method='post' action='/config/antenna'>"
        "<p><input name='ant_model' placeholder='Antenna model (e.g. HXCGPS500)' value='" +
        html_escape(server->storage_->antenna_model()) + "'></p>"
        "<p><input name='ant_radome' placeholder='Radome (e.g. NONE)' value='" +
        html_escape(server->storage_->antenna_radome()) + "'></p>"
        "<p><input name='ant_h' placeholder='Antenna height / ARP delta-H (m)' value='" +
        [&]() { char b[32]; snprintf(b, sizeof(b), "%.4f", server->storage_->antenna_height()); return std::string(b); }() +
        "'></p>"
        "<button>Save Antenna</button></form></div>"
        "<div class='sec'><h2>RINEX Export</h2>"
        "<p class='dim'>Download a merged RINEX file for a chosen UTC time "
        "range.</p>"
        "<p><a class='pill good' href='/rinex/export'><span class='dot'></span>"
        "Open RINEX Export</a></p></div>"
        "<script>"
        "function rxSet(on){"
        "var p=document.getElementById('rx-state');"
        "p.className='pill '+(on?'good':'idle');"
        "document.getElementById('rx-txt').textContent=on?'RINEX LIVE':'RINEX OFF';}"
        "async function rxToggle(on){"
        "var m=document.getElementById('rx-msg');m.textContent='Saving…';"
        "try{var r=await fetch('/rinex/toggle',{method:'POST',"
        "headers:{'Content-Type':'application/x-www-form-urlencoded'},"
        "body:'start='+(on?'1':'0')});"
        "var j=await r.json();"
        "if(!j.ok)throw 0;rxSet(on);"
        "m.textContent=on?'RINEX logging started.':'RINEX logging stopped.';}"
        "catch(e){m.textContent='Toggle failed.';}}"
        // ── Live current-position status poll (header tiles) ─────────────────────
        "function pSet(id,v){var e=document.getElementById(id);if(e)e.textContent=v;}"
        "function pSev(id,s){var e=document.getElementById(id);"
        "if(e)e.className='tval '+s;}"
        "var pBusy=false;"
        "async function pPoll(){if(pBusy)return;pBusy=true;"
        "try{var r=await fetch('/status',{cache:'no-store'});"
        "if(r.ok){var d=await r.json();"
        // Coordinate + height: the live solution (stored fixed base while in Base
        // TX, or the converging survey solution while surveying).
        "if(d.mode==='base_tx'&&d.position_valid){"
        "pSet('p-coord',d.position_lat.toFixed(7)+', '+d.position_lon.toFixed(7));"
        "pSet('p-height','height '+d.position_height.toFixed(3)+' m');}"
        "else if(d.survey_lat){pSet('p-coord',d.survey_lat.toFixed(7)+', '+d.survey_lon.toFixed(7));"
        "pSet('p-height','height '+d.survey_height.toFixed(3)+' m');}"
        "else{pSet('p-coord','not set');pSet('p-height','height --');}"
        // Base mode.
        "var tx=d.mode==='base_tx';"
        "pSet('p-mode',tx?'Base TX':'Survey');pSev('p-mode',tx?'good':'warn');"
        "pSet('p-mode-sub',tx?'transmitting corrections':'surveying-in');"
        // Survey-in progress/status.
        "var st=d.survey_state;"
        "var lbl=st==='collecting'?'In progress':st==='done'?'Complete':st==='error'?'Error':'Idle';"
        "pSet('p-survey',lbl);"
        "pSev('p-survey',st==='collecting'?'warn':st==='done'?'good':st==='error'?'crit':'idle');"
        // Progress bar from stability (lower = closer to converged); fall back to elapsed.
        "var pct=0;"
        "if(st==='done')pct=100;"
        "else if(st==='collecting'){var s=d.survey_stability;"
        "pct=s>0?Math.max(5,Math.min(95,Math.round(100*(1-Math.min(s,1))))):5;}"
        "var bar=document.getElementById('p-survey-bar');if(bar)bar.style.width=pct+'%';"
        "var mins=Math.floor((d.survey_elapsed||0)/60);"
        "pSet('p-survey-sub',(d.survey_samples||0)+' samples · '+mins+' min');"
        "}}catch(e){}finally{pBusy=false;setTimeout(pPoll,15000);}}"
        "pPoll();"
        "</script>";
    return server->send_page(
        request, "Position Setup", content, "/position");
}

// GET /system — System hub: WiFi, AP hotspot password, plus links to firmware,
// console, CA certificate, and admin. WiFi/AP forms moved here from /config.
esp_err_t AdminWebServer::system_page_handler(httpd_req_t *request) {
    AdminWebServer *server = self(request);
    if (!server->authorize(request)) return server->send_unauthorized(request);
    const WifiCredentials wifi = server->storage_->load_wifi();
    const std::string content =
        "<div class='sec'><h2>WiFi</h2>"
        "<form method='post' action='/config/wifi'>"
        "<p><input id='ssid' name='ssid' placeholder='SSID' required value='" +
        html_escape(wifi.ssid) + "'></p>"
        "<p><input name='password' type='password' "
        "placeholder='WiFi password'></p>"
        "<button>Save WiFi &amp; Restart</button> "
        "<button type='button' id='scan'>Scan Networks</button></form>"
        "<div id='networks'></div><script>"
        "document.getElementById('scan').onclick=async()=>{"
        "const o=document.getElementById('networks');o.textContent='Scanning...';"
        "const r=await fetch('/wifi/scan');"
        "if(!r.ok){o.textContent=await r.text();return}const a=await r.json();"
        "o.textContent='';a.forEach(n=>{const p=document.createElement('p');"
        "const b=document.createElement('button');b.type='button';"
        "b.textContent=n.ssid;b.onclick=()=>document.getElementById('ssid').value=n.ssid;"
        "p.append(b,document.createTextNode(` ${n.rssi} dBm ${n.secured?'secured':'open'}`));"
        "o.appendChild(p)});};"
        "</script></div>"
        "<div class='sec'><h2>Access Point (Hotspot)</h2>"
        "<form method='post' action='/config/ap'>"
        "<p class='dim'>SSID: <b>" + html_escape(config::kAccessPointSsid) +
        "</b> (WPA2). Set the password used to join the device's own hotspot.</p>"
        "<p><input name='ap_pw' type='password' minlength='8' maxlength='63' "
        "placeholder='New hotspot password (8-63 chars)' required></p>"
        "<button>Save Hotspot Password</button></form></div>"
        "<div class='sec'><h2>Firmware &amp; Tools</h2>"
        "<p><a href='/update'>Firmware Update (OTA)</a></p>"
        "<p><a href='/logs'>Console Logs</a></p>"
        "<p><a href='/ca.crt'>Download CA Certificate</a></p>"
        "<p class='dim'>Admin password is set on first-time setup. To change it, "
        "clear the device's stored credentials.</p></div>";
    return server->send_page(
        request, "System", content, "/system");
}

esp_err_t AdminWebServer::wifi_scan_handler(httpd_req_t *request) {
    AdminWebServer *server = self(request);
    if (!server->authorize(request)) return server->send_unauthorized(request);
    if (!server->wifi_->access_point_active()) {
        httpd_resp_set_status(request, "409 Conflict");
        return httpd_resp_sendstr(
            request, "WiFi scanning is available in AP fallback mode");
    }
    const auto networks = server->wifi_->scan_networks();
    std::string body = "[";
    for (size_t i = 0; i < networks.size(); ++i) {
        if (i) body += ",";
        body += "{\"ssid\":\"" + json_escape(networks[i].ssid) +
                "\",\"rssi\":" + std::to_string(networks[i].rssi) +
                ",\"secured\":" +
                (networks[i].secured ? "true" : "false") + "}";
    }
    body += "]";
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_hdr(request, "Connection", "close");
    return httpd_resp_send(request, body.c_str(), body.size());
}

esp_err_t AdminWebServer::skyplot_handler(httpd_req_t *request) {
    AdminWebServer *server = self(request);
    if (!server->authorize(request)) return server->send_unauthorized(request);
    const std::string content = R"HTML(
<h2>Satellite Sky Plot</h2>
<canvas id='sky' width='640' height='640'
 style='width:100%;max-width:640px;background:var(--surface);border:1px solid var(--border);border-radius:12px'></canvas>
<p id='count' class='tsub'></p>
<h2 style='margin-top:22px'>Satellites in View</h2>
<table id='sattab'>
 <thead><tr>
  <td style='font-weight:600;color:var(--text-dim)'>Constellation</td>
  <td style='font-weight:600;color:var(--text-dim)'>PRN</td>
  <td style='font-weight:600;color:var(--text-dim)'>Elev</td>
  <td style='font-weight:600;color:var(--text-dim)'>Azim</td>
  <td style='font-weight:600;color:var(--text-dim)'>Signal (dB-Hz)</td>
 </tr></thead>
 <tbody id='satrows'></tbody>
</table>
<p id='satempty' class='dim' style='display:none'>No satellites reported.</p>
<script>
const c=document.getElementById('sky'),x=c.getContext('2d'),C=320,R=280;
const cs=getComputedStyle(document.documentElement);
const ring=cs.getPropertyValue('--border').trim()||'#496';
const lbl=cs.getPropertyValue('--text-dim').trim()||'#9b9';
// sys index: 1=GPS 2=GLONASS 3=Galileo 4=BeiDou 5=QZSS (matches /skyplot/data).
const colors=['','#3af','#f86','#5d5','#fd5','#c9f'];
const sysName=['','GPS','GLONASS','Galileo','BeiDou','QZSS'];
// Theme tokens for the SNR bar (strength → good/warn/crit).
function snrColor(snr){return snr>=40?'var(--good)':snr>=30?'var(--warn)':snr>0?'var(--crit)':'var(--text-faint)';}
function draw(a){
 x.clearRect(0,0,640,640);x.strokeStyle=ring;x.fillStyle=lbl;
 x.textAlign='center';x.font='14px sans-serif';
 [0,30,60].forEach(e=>{let r=R*(90-e)/90;x.beginPath();x.arc(C,C,r,0,7);x.stroke()});
 x.fillText('N',C,24);x.fillText('S',C,630);x.fillText('E',620,C);x.fillText('W',20,C);
 a.forEach(s=>{let r=R*(90-s.el)/90,q=s.az*Math.PI/180;
  let px=C+r*Math.sin(q),py=C-r*Math.cos(q);
  x.fillStyle=colors[s.sys]||'#fff';x.beginPath();x.arc(px,py,8,0,7);x.fill();
  x.fillText(s.prn,px,py-12);
 });document.getElementById('count').textContent=a.length+' satellites in view';
}
// Per-satellite signal table, built client-side from the same /skyplot/data poll.
// Sorted by constellation then PRN; SNR shown as a value plus a colored bar.
function table(a){
 const tb=document.getElementById('satrows');tb.textContent='';
 document.getElementById('satempty').style.display=a.length?'none':'block';
 a.slice().sort((p,q)=>p.sys-q.sys||p.prn-q.prn).forEach(s=>{
  const tr=document.createElement('tr');
  function cell(txt){const td=document.createElement('td');td.textContent=txt;return td;}
  const cn=cell(sysName[s.sys]||('sys '+s.sys));
  const sw=document.createElement('span');
  sw.style.cssText='display:inline-block;width:10px;height:10px;border-radius:50%;margin-right:7px;vertical-align:middle;background:'+(colors[s.sys]||'#fff');
  cn.prepend(sw);
  tr.appendChild(cn);
  tr.appendChild(cell(s.prn));
  tr.appendChild(cell(s.el+'°'));
  tr.appendChild(cell(s.az+'°'));
  const sigTd=document.createElement('td');
  const wrap=document.createElement('div');
  wrap.style.cssText='display:flex;align-items:center;gap:8px';
  const num=document.createElement('span');
  num.style.cssText='min-width:34px;font-variant-numeric:tabular-nums';
  num.textContent=s.snr>0?s.snr:'--';
  const track=document.createElement('div');
  track.className='track';track.style.cssText='flex:1;min-width:60px;background:var(--surface-hi)';
  const fill=document.createElement('div');
  const pct=Math.max(0,Math.min(100,Math.round((s.snr/55)*100)));
  fill.style.cssText='width:'+pct+'%;background:'+snrColor(s.snr);
  track.appendChild(fill);
  wrap.appendChild(num);wrap.appendChild(track);
  sigTd.appendChild(wrap);
  tr.appendChild(sigTd);
  tb.appendChild(tr);
 });
}
async function update(){try{const a=await (await fetch('/skyplot/data')).json();draw(a);table(a);}catch(e){}}
update();setInterval(update,10000);
</script>)HTML";
    return server->send_page(
        request, "Satellites", content, "/skyplot");
}

esp_err_t AdminWebServer::skyplot_data_handler(httpd_req_t *request) {
    AdminWebServer *server = self(request);
    if (!server->authorize(request)) return server->send_unauthorized(request);
    SatelliteInfo satellites[64];
    const size_t count =
        server->station_->satellites(satellites, std::size(satellites));
    std::string body = "[";
    for (size_t i = 0; i < count; ++i) {
        if (i) body += ",";
        body += "{\"prn\":" + std::to_string(satellites[i].prn) +
                ",\"el\":" + std::to_string(satellites[i].elevation) +
                ",\"az\":" + std::to_string(satellites[i].azimuth) +
                ",\"snr\":" + std::to_string(satellites[i].snr) +
                ",\"sys\":" + std::to_string(satellites[i].system) + "}";
    }
    body += "]";
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, body.c_str(), body.size());
}

esp_err_t AdminWebServer::config_post_handler(httpd_req_t *request) {
    AdminWebServer *server = self(request);
    if (!server->authorize(request)) return server->send_unauthorized(request);
    const std::string body = read_body(request, 2048);
    if (body.empty()) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Invalid form");
    }
    struct ServiceForm {
        const char *name;
        const char *enable;
        const char *mountpoint;
        const char *password;
    };
    const ServiceForm services[] = {
        {"rtk2go", "r2g_en", "r2g_mp", "r2g_pw"},
        {"onocoy", "onc_en", "onc_mp", "onc_pw"},
        {"rtkdata", "rtk_en", "rtk_mp", "rtk_pw"},
    };
    for (const auto &form : services) {
        ServiceCredentials credentials = server->storage_->load_service(form.name);
        credentials.mountpoint = form_value(body, form.mountpoint);
        const std::string password = form_value(body, form.password);
        if (!password.empty()) credentials.password = password;
        ESP_RETURN_ON_ERROR(
            server->storage_->save_service(form.name, credentials),
            kTag, "Service save failed");
        ESP_RETURN_ON_ERROR(
            server->storage_->set_service_enabled(
                form.name, !form_value(body, form.enable).empty()),
            kTag, "Service state save failed");
    }
    server->station_->reload_services();
    httpd_resp_set_status(request, "303 See Other");
    httpd_resp_set_hdr(request, "Location", "/config");
    return httpd_resp_send(request, nullptr, 0);
}

esp_err_t AdminWebServer::status_handler(httpd_req_t *request) {
    AdminWebServer *server = self(request);
    if (!server->authorize(request)) return server->send_unauthorized(request);

    const BasePosition position = server->storage_->load_position();
    const BaseStationStatus station = server->station_->status();
    auto provider_json = [](const NtripStatus &status) {
        return std::string("{\"enabled\":") +
            (status.enabled ? "true" : "false") +
            ",\"connected\":" + (status.connected ? "true" : "false") +
            ",\"message\":\"" + json_escape(status.message) +
            "\",\"bytes\":" + std::to_string(status.bytes_sent) +
            ",\"dropped\":" + std::to_string(status.dropped_batches) +
            ",\"reconnects\":" + std::to_string(status.reconnects) +
            ",\"last_error\":\"" + json_escape(status.last_error) +
            "\",\"connected_sec\":" + std::to_string(status.connected_sec) +
            ",\"ever_sent\":" + (status.ever_sent ? "true" : "false") +
            ",\"last_send_age\":" + std::to_string(status.last_send_age_sec) + "}";
    };
    const size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    const SurveySnapshot &sv = station.survey;
    const char *survey_state =
        sv.state == SurveyState::kCollecting ? "collecting" :
        sv.state == SurveyState::kDone       ? "done" :
        sv.state == SurveyState::kError      ? "error" : "idle";
    char survey_json[256];
    snprintf(survey_json, sizeof(survey_json),
        ",\"survey_state\":\"%s\",\"survey_elapsed\":%u,\"survey_samples\":%d,"
        "\"survey_blocks\":%d,\"survey_stability\":%.3f,\"survey_valid\":%s,"
        "\"survey_lat\":%.8f,\"survey_lon\":%.8f,\"survey_height\":%.3f",
        survey_state, static_cast<unsigned>(sv.elapsed_sec), sv.samples,
        sv.blocks, sv.stability, sv.valid ? "true" : "false",
        sv.lat, sv.lon, sv.height);
    std::string body =
        "{\"framework\":\"ESP-IDF " + json_escape(esp_get_idf_version()) +
        "\",\"version\":\"" + json_escape(esp_app_get_description()->version) +
        "\",\"healthy\":" + (server->station_->healthy() ? "true" : "false") +
        ",\"uptime_sec\":" + std::to_string(esp_timer_get_time() / 1000000ULL) +
        ",\"reset_reason\":\"" + json_escape(reset_reason_str(esp_reset_reason())) + "\"" +
        ",\"wifi_connected\":" + (server->wifi_->connected() ? "true" : "false") +
        ",\"ap_active\":" + (server->wifi_->access_point_active() ? "true" : "false") +
        ",\"ssid\":\"" + json_escape(server->wifi_->ssid()) +
        "\",\"ip\":\"" + json_escape(server->wifi_->ip_address()) +
        "\",\"rssi\":" + std::to_string(server->wifi_->rssi()) +
        ",\"free_heap\":" + std::to_string(free_heap) +
        ",\"heap_total\":" +
        std::to_string(heap_caps_get_total_size(MALLOC_CAP_8BIT)) +
        ",\"min_free_heap\":" +
        std::to_string(heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT)) +
        ",\"mode\":\"" +
        std::string(station.mode == BaseMode::kTransmit ? "base_tx" : "survey") +
        "\",\"rtcm_bps\":" +
        std::to_string(station.rtcm_bytes_per_second) +
        ",\"rtcm_total\":" + std::to_string(station.rtcm_bytes_total) +
        ",\"gps\":" + std::to_string(station.survey.gps) +
        ",\"glonass\":" + std::to_string(station.survey.glonass) +
        ",\"galileo\":" + std::to_string(station.survey.galileo) +
        ",\"beidou\":" + std::to_string(station.survey.beidou) +
        std::string(survey_json) +
        ",\"local_clients\":" + std::to_string(station.local_clients) +
        ",\"local_client_ips\":" +
        local_client_ips_json(station.local_client_ips) +
        ",\"ntrip_enabled\":" +
        (server->station_->streams_enabled() ? "true" : "false") +
        ",\"rtk2go\":" + provider_json(station.rtk2go) +
        ",\"onocoy\":" + provider_json(station.onocoy) +
        ",\"rtkdata\":" + provider_json(station.rtkdata) +
        ",\"position_valid\":" + (position.valid ? "true" : "false") +
        ",\"position_lat\":" + std::to_string(position.lat) +
        ",\"position_lon\":" + std::to_string(position.lon) +
        ",\"position_height\":" + std::to_string(position.height) +
        [&]() {
            const bool m = server->sd_->is_mounted();
            const auto ds = server->sd_->disk_stats();
            return std::string(",\"sd_mounted\":") + (m ? "true" : "false") +
                ",\"sd_total\":" + std::to_string(ds.total_bytes) +
                ",\"sd_used\":"  + std::to_string(ds.used_bytes);
        }() +
        [&]() {
            const auto rs = server->station_->rinex_status();
            return std::string(",\"rinex_active\":") +
                (rs.active ? "true" : "false") +
                ",\"rinex_epochs\":" + std::to_string(rs.epochs) +
                ",\"rinex_files\":"  + std::to_string(rs.files) +
                ",\"rinex_file\":\"" + json_escape(rs.current_file) + "\"";
        }() + "}";
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_hdr(request, "Connection", "close");
    return httpd_resp_send(request, body.c_str(), body.size());
}

esp_err_t AdminWebServer::update_page_handler(httpd_req_t *request) {
    AdminWebServer *server = self(request);
    if (!server->authorize(request)) return server->send_unauthorized(request);

    const std::string content = R"HTML(
<h2>Firmware Update</h2>
<p>Select the ESP-IDF application image (.bin). The upload is sent as raw binary
and validated before the boot partition changes. The device restarts automatically
after a successful flash.</p>
<form id='otaForm'>
  <input type='file' id='otaFile' accept='.bin' style='display:none'
    onchange='document.getElementById("otaName").textContent=this.files[0].name'>
  <div style='display:flex;gap:8px;align-items:center;flex-wrap:wrap'>
    <button type='button' onclick='document.getElementById("otaFile").click()'>Choose .bin</button>
    <span id='otaName' style='opacity:.6;font-size:0.85em'>No file chosen</span>
  </div>
  <div id='otaProgress' style='display:none;margin-top:12px'>
    <div class='track'>
      <div id='otaBar' style='background:var(--good);width:0%;transition:width 0.3s'></div>
    </div>
    <p id='otaStatus' style='margin:6px 0 0;font-size:0.85em;color:var(--text-dim)'>Uploading...</p>
  </div>
  <button type='submit' style='margin-top:12px' onclick='startOTA(event)'>Upload &amp; Flash</button>
</form>
<script>
function startOTA(e){
  e.preventDefault();
  var file=document.getElementById('otaFile').files[0];
  if(!file){alert('Choose a .bin file first.');return;}
  if(!confirm('Flash '+file.name+' ('+(file.size/1024).toFixed(1)+' KB)?\nThe device will restart after flashing.'))return;
  var progress=document.getElementById('otaProgress');
  var bar=document.getElementById('otaBar');
  var status=document.getElementById('otaStatus');
  progress.style.display='block';
  var xhr=new XMLHttpRequest();
  xhr.open('POST','/update');
  xhr.setRequestHeader('Content-Type','application/octet-stream');
  xhr.upload.onprogress=function(e){
    if(e.lengthComputable){
      var pct=Math.round(e.loaded/e.total*100);
      bar.style.width=pct+'%';
      status.textContent='Uploading... '+pct+'%';
    }
  };
  xhr.onload=function(){
    bar.style.width='100%';
    if(xhr.status===200){
      bar.style.background='var(--good)';
      status.textContent=xhr.responseText;
      setTimeout(function(){window.location.href='/';},9000);
    }else{
      bar.style.background='var(--crit)';
      status.textContent='Failed: '+xhr.responseText;
    }
  };
  xhr.onerror=function(){
    bar.style.background='var(--crit)';
    status.textContent='Upload error — check connection.';
  };
  xhr.send(file);
}
</script>)HTML";
    return server->send_page(
        request, "Firmware Update", content, "/system");
}

esp_err_t AdminWebServer::update_upload_handler(httpd_req_t *request) {
    AdminWebServer *server = self(request);
    if (!server->authorize(request)) return server->send_unauthorized(request);
    server->station_->set_streams_suspended(true);
    // Let the streaming tasks release TCP buffers before the TLS upload and
    // flash writer compete for the ESP32's remaining heap.
    vTaskDelay(pdMS_TO_TICKS(1100));
    if (request->content_len <= 0) {
        server->station_->set_streams_suspended(false);
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Empty image");
    }

    const esp_partition_t *partition = esp_ota_get_next_update_partition(nullptr);
    if (!partition || static_cast<size_t>(request->content_len) > partition->size) {
        server->station_->set_streams_suspended(false);
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Image too large");
    }

    esp_ota_handle_t handle = 0;
    // OTA_WITH_SEQUENTIAL_WRITES erases sectors lazily as each sector boundary
    // is crossed, avoiding a large synchronous upfront erase that would stall
    // the connection during the request body receive phase.
    esp_err_t result = esp_ota_begin(partition, OTA_WITH_SEQUENTIAL_WRITES, &handle);
    if (result != ESP_OK) {
        server->station_->set_streams_suspended(false);
        return httpd_resp_send_err(
            request, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(result));
    }

    std::vector<uint8_t> buffer(4096);
    size_t remaining = request->content_len;
    int consecutive_timeouts = 0;
    while (remaining > 0) {
        int received = httpd_req_recv(
            request, reinterpret_cast<char *>(buffer.data()),
            std::min(buffer.size(), remaining));
        if (received == HTTPD_SOCK_ERR_TIMEOUT && consecutive_timeouts++ < 20) {
            continue;
        }
        if (received <= 0) {
            esp_ota_abort(handle);
            server->station_->set_streams_suspended(false);
            return httpd_resp_send_err(
                request, HTTPD_500_INTERNAL_SERVER_ERROR, "Upload interrupted");
        }
        consecutive_timeouts = 0;
        result = esp_ota_write(handle, buffer.data(), received);
        if (result != ESP_OK) {
            esp_ota_abort(handle);
            server->station_->set_streams_suspended(false);
            return httpd_resp_send_err(
                request, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(result));
        }
        remaining -= received;
    }

    result = esp_ota_end(handle);
    if (result == ESP_OK) result = esp_ota_set_boot_partition(partition);
    if (result != ESP_OK) {
        server->station_->set_streams_suspended(false);
        return httpd_resp_send_err(
            request, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(result));
    }

    httpd_resp_set_hdr(request, "Connection", "close");
    const esp_err_t response_result =
        httpd_resp_sendstr(request, "Update accepted. Restarting.");
    if (response_result != ESP_OK) {
        ESP_LOGW(
            kTag, "OTA response could not be flushed: %s",
            esp_err_to_name(response_result));
    }
    xTaskCreate(restart_task, "ota_restart", 2048, nullptr, 3, nullptr);
    return ESP_OK;
}

esp_err_t AdminWebServer::position_handler(httpd_req_t *request) {
    AdminWebServer *server = self(request);
    if (!server->authorize(request)) return server->send_unauthorized(request);
    const std::string body = read_body(request, 512);
    if (body.empty()) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Invalid form");
    }

    double lat = 0, lon = 0, height = 0;
    if (!parse_double(form_value(body, "lat"), lat) ||
        !parse_double(form_value(body, "lon"), lon) ||
        !parse_double(form_value(body, "hgt"), height) ||
        lat < -90 || lat > 90 || lon < -180 || lon > 180 ||
        height < -1000 || height > 20000) {
        return httpd_resp_send_err(
            request, HTTPD_400_BAD_REQUEST, "Invalid coordinates");
    }
    ESP_RETURN_ON_ERROR(
        server->station_->request_position(lat, lon, height),
        kTag, "Position request failed");
    httpd_resp_set_status(request, "303 See Other");
    httpd_resp_set_hdr(request, "Location", "/position");
    return httpd_resp_send(request, nullptr, 0);
}

esp_err_t AdminWebServer::survey_handler(httpd_req_t *request) {
    AdminWebServer *server = self(request);
    if (!server->authorize(request)) return server->send_unauthorized(request);
    ESP_RETURN_ON_ERROR(
        server->station_->request_survey(), kTag, "Survey request failed");
    httpd_resp_set_status(request, "303 See Other");
    httpd_resp_set_hdr(request, "Location", "/position");
    return httpd_resp_send(request, nullptr, 0);
}

esp_err_t AdminWebServer::wifi_handler(httpd_req_t *request) {
    AdminWebServer *server = self(request);
    if (!server->authorize(request)) return server->send_unauthorized(request);
    const std::string body = read_body(request, 512);
    WifiCredentials credentials{
        form_value(body, "ssid"),
        form_value(body, "password"),
        true,
    };
    if (credentials.password.empty()) {
        const WifiCredentials saved = server->storage_->load_wifi();
        if (saved.ssid == credentials.ssid) credentials.password = saved.password;
    }
    if (credentials.ssid.empty() || credentials.ssid.size() > 32 ||
        credentials.password.size() > 64) {
        return httpd_resp_send_err(
            request, HTTPD_400_BAD_REQUEST, "Invalid WiFi credentials");
    }
    ESP_RETURN_ON_ERROR(
        server->storage_->save_wifi(credentials), kTag, "WiFi save failed");
    httpd_resp_sendstr(request, "WiFi credentials saved. Restarting.");
    xTaskCreate(restart_task, "wifi_restart", 2048, nullptr, 3, nullptr);
    return ESP_OK;
}

esp_err_t AdminWebServer::ap_password_handler(httpd_req_t *request) {
    AdminWebServer *server = self(request);
    if (!server->authorize(request)) return server->send_unauthorized(request);
    const std::string body = read_body(request, 256);
    const std::string password = form_value(body, "ap_pw");
    if (password.size() < 8 || password.size() > 63) {
        return httpd_resp_send_err(
            request, HTTPD_400_BAD_REQUEST,
            "Hotspot password must be 8-63 characters");
    }
    ESP_RETURN_ON_ERROR(
        server->storage_->save_ap_password(password), kTag, "AP password save failed");
    // Apply live so the change takes effect without a reboot. Currently joined
    // hotspot clients will be dropped and must reconnect with the new password.
    server->wifi_->apply_ap_settings();
    httpd_resp_set_status(request, "303 See Other");
    httpd_resp_set_hdr(request, "Location", "/system");
    return httpd_resp_send(request, nullptr, 0);
}

esp_err_t AdminWebServer::antenna_handler(httpd_req_t *request) {
    AdminWebServer *server = self(request);
    if (!server->authorize(request)) return server->send_unauthorized(request);
    const std::string body = read_body(request, 512);
    std::string model  = form_value(body, "ant_model");
    std::string radome = form_value(body, "ant_radome");
    const std::string height_str = form_value(body, "ant_h");
    if (model.empty())  model  = "HXCGPS500";
    if (radome.empty()) radome = "NONE";
    if (model.size() > 16 || radome.size() > 4) {
        return httpd_resp_send_err(
            request, HTTPD_400_BAD_REQUEST,
            "Antenna model max 16 chars, radome max 4");
    }
    const double height = height_str.empty() ? 0.0 : atof(height_str.c_str());
    ESP_RETURN_ON_ERROR(
        server->storage_->save_antenna(model, radome, height),
        kTag, "Antenna save failed");
    // Takes effect on the next RINEX file (open_file writes the header).
    httpd_resp_set_status(request, "303 See Other");
    httpd_resp_set_hdr(request, "Location", "/position");
    return httpd_resp_send(request, nullptr, 0);
}

esp_err_t AdminWebServer::logs_page_handler(httpd_req_t *request) {
    AdminWebServer *server = self(request);
    if (!server->authorize(request)) return server->send_unauthorized(request);
    const std::string content = R"HTML(
<h2>Console Logs</h2>
<p class='dim'>Live ESP-IDF console output (most recent ~16 KB). Useful for
diagnosing NTRIP, WiFi, and SD errors without a serial cable.</p>
<p>
 <label><input type='checkbox' id='auto' checked style='width:auto'> Auto-refresh (5s)</label>
 &nbsp; <button type='button' id='refresh'>Refresh now</button>
 &nbsp; <button type='button' id='bottom'>Jump to end</button>
</p>
<pre id='log' style='background:var(--surface);color:var(--text);padding:10px;
 border:1px solid var(--border);border-radius:8px;
 max-height:70vh;overflow:auto;white-space:pre-wrap;word-break:break-word;
 font:12px/1.4 monospace'>loading…</pre>
<script>
const pre=document.getElementById('log'),auto=document.getElementById('auto');
let busy=false,cursor=0;
async function load(full=false){
 if(busy)return;busy=true;
 try{
  const r=await fetch(full||!cursor?'/logs/data':'/logs/data?since='+cursor,{cache:'no-store'});
  if(r.ok){
   const atEnd=pre.scrollTop+pre.clientHeight>=pre.scrollHeight-20;
   const text=await r.text(),next=Number(r.headers.get('X-Log-Cursor')||0);
   const truncated=r.headers.get('X-Log-Truncated')==='1';
   if(full||!cursor||truncated)pre.textContent=text;
   else if(text.length){
    pre.textContent+=text;
    if(pre.textContent.length>16384)pre.textContent=pre.textContent.slice(-16384);
   }
   if(next)cursor=next;
   if(atEnd)pre.scrollTop=pre.scrollHeight;
  }
 }catch(e){}finally{busy=false;}
}
document.getElementById('refresh').onclick=()=>load(true);
document.getElementById('bottom').onclick=()=>{pre.scrollTop=pre.scrollHeight;};
load(true).then(()=>{pre.scrollTop=pre.scrollHeight;});
setInterval(()=>{if(auto.checked)load();},5000);
</script>)HTML";
    return server->send_page(
        request, "Console Logs", content, "/system");
}

esp_err_t AdminWebServer::logs_data_handler(httpd_req_t *request) {
    AdminWebServer *server = self(request);
    if (!server->authorize(request)) return server->send_unauthorized(request);
    uint64_t since = 0;
    const std::string since_param = query_param(request, "since");
    if (!since_param.empty()) {
        since = strtoull(since_param.c_str(), nullptr, 10);
    }
    const log_buffer::Snapshot logs = log_buffer::snapshot_since(since);
    char cursor[24];
    snprintf(cursor, sizeof(cursor), "%llu",
             static_cast<unsigned long long>(logs.next));
    httpd_resp_set_type(request, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_hdr(request, "X-Log-Cursor", cursor);
    httpd_resp_set_hdr(request, "X-Log-Truncated", logs.truncated ? "1" : "0");
    return httpd_resp_send(request, logs.text.c_str(), logs.text.size());
}

bool AdminWebServer::authorize(httpd_req_t *request) const {
    if (!storage_->admin_password_set()) return false;
    const std::string plain =
        std::string(kAdminUser) + ":" + storage_->admin_password();
    size_t encoded_size = 0;
    mbedtls_base64_encode(
        nullptr, 0, &encoded_size,
        reinterpret_cast<const unsigned char *>(plain.data()), plain.size());
    std::vector<unsigned char> encoded(encoded_size + 1);
    if (mbedtls_base64_encode(
            encoded.data(), encoded.size(), &encoded_size,
            reinterpret_cast<const unsigned char *>(plain.data()),
            plain.size()) != 0) {
        return false;
    }
    const std::string expected =
        "Basic " + std::string(reinterpret_cast<char *>(encoded.data()), encoded_size);

    size_t length = httpd_req_get_hdr_value_len(request, "Authorization");
    if (length == 0) return false;
    std::vector<char> header(length + 1);
    if (httpd_req_get_hdr_value_str(
            request, "Authorization", header.data(), header.size()) != ESP_OK) {
        return false;
    }
    return expected == header.data();
}

esp_err_t AdminWebServer::send_unauthorized(httpd_req_t *request) const {
    httpd_resp_set_status(request, "401 Unauthorized");
    httpd_resp_set_hdr(
        request, "WWW-Authenticate", "Basic realm=\"GPS Base Station\"");
    httpd_resp_set_hdr(request, "Connection", "close");
    return httpd_resp_sendstr(request, "Authentication required");
}

// POST /theme?night=0|1 — persist the day/night choice to NVS so both surfaces agree.
esp_err_t AdminWebServer::theme_post_handler(httpd_req_t *request) {
    AdminWebServer *server = self(request);
    if (!server->authorize(request)) return server->send_unauthorized(request);
    const std::string v = query_param(request, "night");
    const bool night = (v != "0");  // default to night unless explicitly "0"
    if (server->storage_) server->storage_->set_night_mode(night);
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, "ok", 2);
}

esp_err_t AdminWebServer::send_page(
    httpd_req_t *request, const char *title,
    const std::string &content, const char *active_route) const {
    static constexpr std::string_view kPrefix =
        "<!doctype html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<link rel='icon' href='data:,'><title>";
    // Marine-instrument theme. The :root (night) and [data-theme=day] tokens mirror
    // the touchscreen palette in theme.hpp — keep the two in sync (Phase-4 audit
    // checks parity). Class names .ok/.warn/.err/.dim are preserved so every existing
    // page renders unchanged, just retinted.
    static constexpr std::string_view kAfterTitle =
        "</title><style>"
        ":root{--bg:#05080B;--surface:#0D141B;--surface-hi:#142029;"
        "--border:#203040;--text:#C9D6E0;--text-dim:#6E8294;--text-faint:#3F5364;"
        "--accent:#1C7FCC;--good:#1FAE66;--warn:#D98A1F;--crit:#E0473F;"
        "--font:'Segoe UI',system-ui,-apple-system,Roboto,'Helvetica Neue',Arial,sans-serif}"
        "[data-theme=day]{--bg:#0B1016;--surface:#16202B;--surface-hi:#1E2C3A;"
        "--border:#2C4053;--text:#F2F6FA;--text-dim:#9DB0C2;--text-faint:#5E7488;"
        "--accent:#2FA4FF;--good:#27D17C;--warn:#FFB02E;--crit:#FF5A52}"
        "*{box-sizing:border-box}"
        "body{font-family:var(--font);background:var(--bg);color:var(--text);"
        "padding:1em;max-width:720px;margin:auto;"
        "font-variant-numeric:tabular-nums}"
        "h1,h2{color:var(--text);font-weight:600}h1{font-size:20px}h2{font-size:17px}"
        "table{border-collapse:collapse;width:100%}"
        "td{border:1px solid var(--border);padding:8px}"
        "input,select{width:100%;max-width:420px;padding:8px;"
        "background:var(--surface);color:var(--text);border:1px solid var(--border);"
        "border-radius:8px;box-sizing:border-box}"
        "button{padding:9px 16px;background:var(--surface-hi);color:var(--text);"
        "border:1px solid var(--border);border-radius:8px;cursor:pointer}"
        "button:hover{border-color:var(--accent)}"
        "a{color:var(--accent)}"
        ".ok{color:var(--good)}.warn{color:var(--warn)}.err{color:var(--crit)}"
        ".dim{color:var(--text-faint)}"
        // ── Shared marine chrome: header row, segmented nav, status pill ──────────
        ".bar{display:flex;align-items:center;gap:10px;flex-wrap:wrap;margin:0 0 14px}"
        "h1{margin:0;flex:0 0 auto}"
        ".pill{display:inline-flex;align-items:center;gap:6px;font-size:13px;"
        "font-weight:600;padding:3px 11px;border-radius:999px;line-height:1.4;"
        "border:1px solid transparent;white-space:nowrap}"
        ".pill .dot{width:8px;height:8px;border-radius:50%;background:currentColor}"
        ".pill.good{color:var(--good);background:color-mix(in srgb,var(--good) 14%,transparent)}"
        ".pill.warn{color:var(--warn);background:color-mix(in srgb,var(--warn) 14%,transparent)}"
        ".pill.crit{color:var(--crit);background:color-mix(in srgb,var(--crit) 14%,transparent)}"
        ".pill.idle{color:var(--text-faint);border-color:var(--border)}"
        ".pill.idle .dot{background:transparent;border:1px solid currentColor;width:7px;height:7px}"
        ".spacer{flex:1 1 auto}"
        ".tbtn{background:transparent;border:1px solid var(--border);color:var(--text-dim);"
        "border-radius:8px;padding:6px 9px;cursor:pointer;line-height:0}"
        ".tbtn:hover{border-color:var(--accent);color:var(--text)}"
        "nav{display:flex;gap:4px;overflow-x:auto;border:1px solid var(--border);"
        "border-radius:10px;padding:4px;margin:0 0 18px;background:var(--surface)}"
        "nav a{flex:1 1 0;min-width:84px;display:flex;flex-direction:column;"
        "align-items:center;gap:3px;padding:7px 4px;border-radius:7px;"
        "color:var(--text-dim);text-decoration:none;font-size:12px;font-weight:600}"
        "nav a svg{width:20px;height:20px}"
        "nav a:hover{color:var(--text);background:var(--surface-hi)}"
        "nav a.on{color:var(--accent);background:var(--surface-hi)}"
        // ── Readout tiles + dashboard grid ───────────────────────────────────────
        ".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));"
        "gap:14px}"
        ".tile{display:block;background:var(--surface);border:1px solid var(--border);"
        "border-radius:12px;padding:16px;color:var(--text);text-decoration:none;"
        "position:relative;min-height:110px}"
        "a.tile:hover{border-color:var(--accent)}"
        "a.tile::after{content:'\\203A';position:absolute;top:12px;right:14px;"
        "color:var(--text-faint);font-size:18px;line-height:1}"
        ".tile.hero{grid-column:1 / -1}"
        "@media(min-width:720px){.tile.hero{grid-column:span 2}}"
        ".tile.crit{border-left:3px solid var(--crit)}"
        ".tlabel{font-size:13px;font-weight:600;text-transform:uppercase;"
        "letter-spacing:.06em;color:var(--text-dim);margin:0 0 8px}"
        ".tval{font-size:clamp(24px,5vw,36px);font-weight:600;line-height:1.05}"
        ".tile.hero .tval{font-size:clamp(40px,9vw,64px);font-weight:700}"
        ".tunit{font-size:14px;font-weight:500;color:var(--text-dim);margin-left:4px}"
        ".tsub{font-size:12px;color:var(--text-dim);margin-top:8px}"
        ".tval.good{color:var(--good)}.tval.warn{color:var(--warn)}.tval.crit{color:var(--crit)}"
        ".tval.idle{color:var(--text-faint)}"
        // ── Per-service status lights (NTRIP push tile) ──────────────────────────
        ".svclist{display:flex;flex-direction:column;gap:7px;margin:2px 0}"
        ".svc{display:flex;align-items:center;gap:8px;font-size:15px;font-weight:600;"
        "color:var(--text)}"
        ".svc .dot{width:10px;height:10px;border-radius:50%;flex:0 0 auto}"
        ".dot.good{background:var(--good)}.dot.warn{background:var(--warn)}"
        ".dot.crit{background:var(--crit)}"
        ".dot.idle{background:transparent;border:1px solid var(--text-faint)}"
        ".svc-rc{color:var(--warn);font-size:12px;font-weight:600}"
        // ── Storage progress bar (re-themed, shared) ─────────────────────────────
        ".track{background:var(--surface-hi);border-radius:999px;height:8px;overflow:hidden}"
        ".track>div{height:8px;border-radius:999px}"
        ".sec{margin:22px 0}.sec h2{margin:0 0 10px}"
        "footer{margin-top:26px;padding-top:12px;border-top:1px solid var(--border);"
        "font-size:12px;color:var(--text-faint)}footer a{color:var(--text-dim)}"
        "</style></head><body>";
    static constexpr std::string_view kSuffix = "</body></html>";

    httpd_resp_set_type(request, "text/html");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_hdr(request, "Connection", "close");
    httpd_resp_set_hdr(request, "Strict-Transport-Security", "max-age=3600");
    esp_err_t result = send_chunks(request, kPrefix);
    if (result == ESP_OK) result = send_chunks(request, title);
    if (result == ESP_OK) result = send_chunks(request, kAfterTitle);
    // Initial theme from NVS (shared with the touchscreen) — set window.__theme
    // before nav_html's IIFE applies it, so the page loads in the persisted palette.
    if (result == ESP_OK) {
        const bool night = storage_ ? storage_->night_mode() : true;
        result = send_chunks(request, night
            ? std::string_view("<script>window.__theme='night'</script>")
            : std::string_view("<script>window.__theme='day'</script>"));
    }
    if (result == ESP_OK) result = send_chunks(request, nav_html(active_route));
    if (result == ESP_OK) result = send_chunks(request, content);
    if (result == ESP_OK) result = send_chunks(request, kSuffix);
    if (result == ESP_OK) result = httpd_resp_send_chunk(request, nullptr, 0);
    return result;
}

// Top header bar (title + global worst-of-all status pill + day/night toggle)
// followed by the SIX-tab web segmented nav. The web nav is intentionally one tab
// wider than ui_sections::Section (5, shared & frozen with the touch UI): Satellite
// gets its own tab here. So the nav is driven by a *web-local* table of
// {label, route, icon} and the active tab is chosen by string-matching active_route
// against each entry's route — ui_sections is left untouched. Inline SVG icons
// inherit currentColor. The status pill starts neutral; the poller below keeps it
// live (worst-of-all) on EVERY page, not just the dashboard.
std::string AdminWebServer::nav_html(const char *active_route) {
    struct WebTab { const char *label; const char *route; const char *icon; };
    // Six web tabs. Order: Dashboard · Position · Satellite · Links · Storage · System.
    static const WebTab kTabs[] = {
        // Dashboard — gauge cluster
        {"Dashboard", "/",
         "<svg viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='2' "
         "stroke-linecap='round' stroke-linejoin='round'><path d='M3 12a9 9 0 0 1 18 0'/>"
         "<path d='M12 12l4-3'/><circle cx='12' cy='12' r='1.5'/></svg>"},
        // Position — crosshair
        {"Position", "/position",
         "<svg viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='2' "
         "stroke-linecap='round' stroke-linejoin='round'><circle cx='12' cy='12' r='7'/>"
         "<path d='M12 2v3M12 19v3M2 12h3M19 12h3'/></svg>"},
        // Satellite — dish on a stand
        {"Satellite", "/skyplot",
         "<svg viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='2' "
         "stroke-linecap='round' stroke-linejoin='round'><path d='M4 20a8 8 0 0 1 8-8'/>"
         "<path d='M4 16a4 4 0 0 1 4 4'/><circle cx='5' cy='19' r='1'/>"
         "<path d='M14 4l6 6'/><path d='M14.5 9.5l-3 3'/>"
         "<path d='M17 3a4 4 0 0 1 4 4'/></svg>"},
        // Links — broadcast / cloud-up
        {"Links", "/config",
         "<svg viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='2' "
         "stroke-linecap='round' stroke-linejoin='round'><path d='M5 13a7 7 0 0 1 14 0'/>"
         "<path d='M8.5 13a3.5 3.5 0 0 1 7 0'/><path d='M12 13v7'/></svg>"},
        // Storage — SD card
        {"Storage", "/files",
         "<svg viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='2' "
         "stroke-linecap='round' stroke-linejoin='round'><path d='M6 3h9l3 3v15H6z'/>"
         "<path d='M10 3v4M13 3v4'/></svg>"},
        // System — gear
        {"System", "/system",
         "<svg viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='2' "
         "stroke-linecap='round' stroke-linejoin='round'><circle cx='12' cy='12' r='3'/>"
         "<path d='M12 2v3M12 19v3M2 12h3M19 12h3M5 5l2 2M17 17l2 2M19 5l-2 2M7 17l-2 2'/></svg>"},
    };
    const std::string active = active_route ? active_route : "/";
    std::string out =
        "<div class='bar'><h1>GPS Base Station</h1>"
        "<span class='spacer'></span>"
        "<span id='navpill' class='pill idle'><span class='dot'></span>"
        "<span id='navpilltxt'>STATUS</span></span>"
        "<button class='tbtn' id='themebtn' title='Day / night' "
        "onclick='toggleTheme()'>"
        "<svg viewBox='0 0 24 24' width='18' height='18' fill='none' stroke='currentColor' "
        "stroke-width='2' stroke-linecap='round' stroke-linejoin='round'>"
        "<circle cx='12' cy='12' r='4'/><path d='M12 2v2M12 20v2M2 12h2M20 12h2"
        "M5 5l1.5 1.5M17.5 17.5L19 19M19 5l-1.5 1.5M6.5 17.5L5 19'/></svg></button>"
        "</div><nav>";
    for (const auto &tab : kTabs) {
        const bool on = (active == tab.route);
        out += "<a class='";
        out += on ? "on" : "";
        out += "' href='";
        out += tab.route;
        out += "'>";
        out += tab.icon;
        out += "<span>";
        out += tab.label;
        out += "</span></a>";
    }
    out +=
        "</nav>"
        "<script>"
        "(function(){var t=window.__theme||'night';"
        "document.documentElement.setAttribute('data-theme',t);})();"
        "function toggleTheme(){var h=document.documentElement,"
        "d=h.getAttribute('data-theme')==='day'?'night':'day';"
        "h.setAttribute('data-theme',d);window.__theme=d;"
        "fetch('/theme?night='+(d==='night'?1:0),{method:'POST'}).catch(function(){});}"
        // Update the global worst-of-all pill. Severity rank: crit>warn>good>idle.
        "function setNavPill(sev,txt){var p=document.getElementById('navpill');"
        "if(!p)return;p.className='pill '+sev;"
        "document.getElementById('navpilltxt').textContent=txt;}"
        // ── Shared global status poller (runs on EVERY page) ─────────────────────
        // Computes the same worst-of-all severity the dashboard used and drives the
        // header pill. The dashboard keeps its own tile updates separately; this is
        // the single source of truth for the pill so it stays live everywhere.
        "var NAVRANK={idle:0,good:1,warn:2,crit:3};"
        "function navWorse(a,b){return NAVRANK[b]>NAVRANK[a]?b:a;}"
        "function navApplyStatus(d){"
        "var worst='good';"
        "var tx=d.mode==='base_tx';"
        // crit: unhealthy, any enabled-but-down NTRIP service, or SD error.
        "if(!d.healthy)worst=navWorse(worst,'crit');"
        "[d.rtk2go,d.onocoy,d.rtkdata].forEach(function(s){"
        "if(s&&s.enabled&&!s.connected)worst=navWorse(worst,'crit');});"
        "if(d.sd_mounted===false)worst=navWorse(worst,'crit');"
        // warn: surveying, disk >80%, or weak/missing link.
        "var surveying=!tx;"
        "if(surveying)worst=navWorse(worst,'warn');"
        "if(d.sd_total>0){var sp=Math.round(d.sd_used*100/d.sd_total);"
        "if(sp>=95)worst=navWorse(worst,'crit');else if(sp>=80)worst=navWorse(worst,'warn');}"
        "if(d.wifi_connected){if(d.rssi<=-75)worst=navWorse(worst,'warn');}"
        "else if(d.ap_active)worst=navWorse(worst,'warn');"
        "else worst=navWorse(worst,'crit');"
        "if(worst==='crit')setNavPill('crit','ALARM');"
        "else if(worst==='warn')setNavPill('warn',surveying?'SURVEYING':'WARN');"
        "else setNavPill('good','LIVE');}"
        "var navBusy=false;"
        "async function navPoll(){if(navBusy)return;navBusy=true;"
        "try{var r=await fetch('/status',{cache:'no-store'});"
        "if(r.ok)navApplyStatus(await r.json());}catch(e){}"
        "finally{navBusy=false;setTimeout(navPoll,15000);}}"
        "navPoll();"
        "</script>";
    return out;
}

AdminWebServer *AdminWebServer::self(httpd_req_t *request) {
    return static_cast<AdminWebServer *>(request->user_ctx);
}

std::string AdminWebServer::read_body(
    httpd_req_t *request, size_t max_length) {
    if (request->content_len <= 0 ||
        static_cast<size_t>(request->content_len) > max_length) {
        return {};
    }
    std::string body(request->content_len, '\0');
    size_t offset = 0;
    int consecutive_timeouts = 0;
    while (offset < body.size()) {
        int result = httpd_req_recv(
            request, body.data() + offset, body.size() - offset);
        if (result == HTTPD_SOCK_ERR_TIMEOUT && consecutive_timeouts++ < 20) {
            continue;
        }
        if (result <= 0) return {};
        consecutive_timeouts = 0;
        offset += result;
    }
    return body;
}

std::string AdminWebServer::form_value(
    const std::string &body, const char *name) {
    const std::string prefix = std::string(name) + "=";
    size_t start = 0;
    while (start <= body.size()) {
        size_t end = body.find('&', start);
        if (end == std::string::npos) end = body.size();
        if (body.compare(start, prefix.size(), prefix) == 0) {
            return url_decode(body.substr(start + prefix.size(), end - start - prefix.size()));
        }
        start = end + 1;
    }
    return {};
}

std::string AdminWebServer::url_decode(const std::string &value) {
    std::string out;
    out.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '+') {
            out.push_back(' ');
        } else if (value[i] == '%' && i + 2 < value.size()) {
            char hex[3] = {value[i + 1], value[i + 2], '\0'};
            char *end = nullptr;
            long decoded = strtol(hex, &end, 16);
            if (end && *end == '\0') {
                out.push_back(static_cast<char>(decoded));
                i += 2;
            }
        } else {
            out.push_back(value[i]);
        }
    }
    return out;
}

std::string AdminWebServer::html_escape(const std::string &value) {
    std::string out;
    for (char c : value) {
        if (c == '&') out += "&amp;";
        else if (c == '<') out += "&lt;";
        else if (c == '>') out += "&gt;";
        else if (c == '"') out += "&quot;";
        else if (c == '\'') out += "&#39;";
        else out += c;
    }
    return out;
}

std::string AdminWebServer::json_escape(const std::string &value) {
    std::string out;
    for (unsigned char c : value) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c >= 0x20) out.push_back(static_cast<char>(c));
        }
    }
    return out;
}

std::string AdminWebServer::query_param(httpd_req_t *request, const char *key) {
    size_t qlen = httpd_req_get_url_query_len(request);
    if (qlen == 0) return {};
    std::string qbuf(qlen + 1, '\0');
    if (httpd_req_get_url_query_str(request, qbuf.data(), qbuf.size()) != ESP_OK) return {};
    std::string keyed = std::string(key) + "=";
    size_t pos = qbuf.find(keyed);
    if (pos == std::string::npos) return {};
    size_t val_start = pos + keyed.size();
    size_t val_end = qbuf.find('&', val_start);
    if (val_end == std::string::npos) val_end = qlen;
    return url_decode(qbuf.substr(val_start, val_end - val_start));
}

// Extracts a simple unescaped string field from a JSON body.
// Only handles the pattern: "key":"value" — sufficient for file paths.
std::string AdminWebServer::json_field(const std::string &body, const char *key) {
    std::string needle = std::string("\"") + key + "\":\"";
    size_t pos = body.find(needle);
    if (pos == std::string::npos) return {};
    pos += needle.size();
    size_t end = body.find('"', pos);
    if (end == std::string::npos) return {};
    return body.substr(pos, end - pos);
}

// Extract a JSON string array (e.g. {"paths":["/sdcard/a","/sdcard/b"]}) into a
// vector. Small + defensive, not a full JSON parser: it scans between the first
// '[' after the key and the matching ']', pulling out each double-quoted element
// and JSON-unescaping it. Hard caps protect the heap and the path buffers:
//   - at most kMaxElems elements; over the cap => reject (empty + *rejected=true)
//   - each element at most kMaxLen chars; over the cap => reject
//   - control chars / malformed escapes => reject
// Caller is expected to additionally validate each element with safe_path /
// check_deletable before acting on it.
std::vector<std::string> AdminWebServer::json_string_array(
    const std::string &body, const char *key, bool *rejected) {
    constexpr size_t kMaxElems = 256;
    constexpr size_t kMaxLen   = 256;
    if (rejected) *rejected = false;
    std::vector<std::string> out;
    // Single point of failure: clear results, flag rejection, hand back the empty
    // vector. Keeps every error path one statement long (no misleading-indent).
    auto reject = [&]() -> std::vector<std::string> {
        if (rejected) *rejected = true;
        out.clear();
        return out;
    };

    const std::string needle = std::string("\"") + key + "\"";
    size_t pos = body.find(needle);
    if (pos == std::string::npos) return out;       // key absent: empty, not an error
    pos = body.find('[', pos + needle.size());
    if (pos == std::string::npos) return reject();
    ++pos;  // first char inside the array

    while (pos < body.size()) {
        // Skip whitespace / commas between elements.
        while (pos < body.size() &&
               (body[pos] == ' ' || body[pos] == '\t' || body[pos] == '\n' ||
                body[pos] == '\r' || body[pos] == ',')) {
            ++pos;
        }
        if (pos >= body.size()) return reject();
        if (body[pos] == ']') break;                // end of array
        if (body[pos] != '"') return reject();      // non-string element
        ++pos;  // step over opening quote

        std::string elem;
        bool closed = false;
        while (pos < body.size()) {
            const char c = body[pos++];
            if (c == '"') { closed = true; break; }
            if (c == '\\') {
                if (pos >= body.size()) break;
                const char e = body[pos++];
                switch (e) {
                    case '"':  elem.push_back('"');  break;
                    case '\\': elem.push_back('\\'); break;
                    case '/':  elem.push_back('/');  break;
                    case 'b':  elem.push_back('\b'); break;
                    case 'f':  elem.push_back('\f'); break;
                    case 'n':  elem.push_back('\n'); break;
                    case 'r':  elem.push_back('\r'); break;
                    case 't':  elem.push_back('\t'); break;
                    case 'u': {  // \uXXXX — decode only ASCII (XX==00..7F); else reject
                        if (pos + 4 > body.size()) return reject();
                        char hx[5] = {body[pos], body[pos + 1], body[pos + 2], body[pos + 3], '\0'};
                        char *he = nullptr;
                        long cp = strtol(hx, &he, 16);
                        if (he != hx + 4 || cp < 0 || cp > 0x7F) return reject();
                        pos += 4;
                        elem.push_back(static_cast<char>(cp));
                        break;
                    }
                    default: return reject();
                }
            } else if (static_cast<unsigned char>(c) < 0x20) {
                return reject();  // raw control char
            } else {
                elem.push_back(c);
            }
            if (elem.size() > kMaxLen) return reject();
        }
        if (!closed) return reject();
        out.push_back(std::move(elem));
        if (out.size() > kMaxElems) return reject();
    }
    return out;
}

// ── SD card file browser ──────────────────────────────────────────────────────

esp_err_t AdminWebServer::files_page_handler(httpd_req_t *request) {
    AdminWebServer *server = self(request);
    if (!server->authorize(request)) return server->send_unauthorized(request);

    const bool mounted = server->sd_->is_mounted();
    const std::string mount = SdManager::kMountPoint;

    // Inject the mount-point constant in a plain string tag so it is evaluated
    // before the raw-string script block (raw strings cannot be interpolated).
    const SdManager::DiskStats ds = server->sd_->disk_stats();
    const unsigned sd_pct = (ds.valid && ds.total_bytes)
        ? static_cast<unsigned>(ds.used_bytes * 100 / ds.total_bytes) : 0;
    const char *bar_class = sd_pct < 80 ? "ok" : (sd_pct < 95 ? "warn" : "err");

    std::string storage_block;
    if (mounted && ds.valid) {
        char bar_html[512];
        snprintf(bar_html, sizeof(bar_html),
            "<div style='margin-bottom:14px'>"
            "<div class='tsub' style='margin-bottom:4px'>"
            "Storage: <span class='%s'>%s used of %s (%u%%)</span></div>"
            "<div class='track'>"
            "<div style='background:var(--%s);width:%u%%'></div>"
            "</div></div>",
            bar_class,
            human_bytes(ds.used_bytes).c_str(),
            human_bytes(ds.total_bytes).c_str(),
            sd_pct,
            sd_pct < 80 ? "good" : (sd_pct < 95 ? "warn" : "crit"),
            sd_pct);
        storage_block = bar_html;
    } else {
        storage_block = "<p class='tsub'>Storage: " +
            std::string(mounted ? "calculating&hellip;" : "<span class='err'>not mounted</span>") +
            "</p>";
    }

    std::string content =
        // Scoped marine styles for the selection toolbar, checkboxes, modal and
        // progress. Uses only var(--token)s already defined by send_page.
        "<style>"
        ".fb-cbx{appearance:none;-webkit-appearance:none;width:20px;height:20px;"
        "margin:0;border:2px solid var(--border);border-radius:5px;background:var(--surface);"
        "cursor:pointer;position:relative;flex:0 0 auto;vertical-align:middle}"
        ".fb-cbx:checked{background:var(--accent);border-color:var(--accent)}"
        ".fb-cbx:checked::after{content:'';position:absolute;left:5px;top:1px;width:5px;"
        "height:10px;border:solid #fff;border-width:0 2px 2px 0;transform:rotate(45deg)}"
        ".fb-cbx:indeterminate{background:var(--accent);border-color:var(--accent)}"
        ".fb-cbx:indeterminate::after{content:'';position:absolute;left:3px;top:8px;"
        "width:10px;height:2px;background:#fff}"
        // 44px hit target around the 20px checkbox.
        ".fb-cbwrap{display:inline-flex;align-items:center;justify-content:center;"
        "width:44px;height:44px;cursor:pointer}"
        ".fb-selcol{display:none}body.fb-select .fb-selcol{display:table-cell}"
        ".fb-actcol{display:table-cell}body.fb-select .fb-actcol{display:none}"
        "#fb-selbar{display:none;align-items:center;gap:10px;flex-wrap:wrap;"
        "margin:0 0 10px;padding:8px 10px;background:var(--surface-hi);"
        "border:1px solid var(--border);border-radius:10px}"
        "body.fb-select #fb-selbar{display:flex}"
        "body.fb-select #fb-normbar{display:none}"
        "#fb-count{color:var(--text-dim);font-size:13px;flex:1;min-width:120px}"
        ".btn-crit{background:var(--crit);border-color:var(--crit);color:#fff}"
        ".btn-crit:hover{filter:brightness(1.1);border-color:var(--crit)}"
        ".btn-crit:disabled{opacity:.45;cursor:not-allowed;filter:none}"
        // Modal scrim + card (component 4.8).
        "#fb-modal{display:none;position:fixed;inset:0;z-index:50;"
        "background:rgba(0,0,0,.6);align-items:center;justify-content:center;padding:16px}"
        "#fb-modal.show{display:flex}"
        ".fb-card{background:var(--surface);border:1px solid var(--border);"
        "border-radius:14px;padding:20px;max-width:440px;width:100%;"
        "box-shadow:0 10px 40px rgba(0,0,0,.5)}"
        ".fb-card h3{margin:0 0 10px;font-size:18px;color:var(--text)}"
        ".fb-card .fb-sub{color:var(--text-dim);font-size:14px;margin:0 0 6px}"
        ".fb-card .fb-warn{color:var(--crit);font-weight:600}"
        ".fb-foot{display:flex;gap:10px;justify-content:flex-end;margin-top:18px;flex-wrap:wrap}"
        "#fb-typewrap{margin:14px 0 0;display:none}"
        "#fb-typewrap label{display:block;color:var(--text-dim);font-size:13px;margin:0 0 4px}"
        "#fb-prog{display:none;margin-top:16px}"
        "</style>"
        "<div class='sec'><h2>SD Card Files</h2>"
        "<div id='fb-storage' style='margin-bottom:14px'>" +
        storage_block +
        "</div>"
        // Prominent RINEX export entry point (the picker page lives at /rinex/export).
        "<p style='margin:0 0 14px'><a class='pill good' href='/rinex/export'>"
        "<span class='dot'></span>RINEX Export</a></p>"
        // Normal toolbar (breadcrumb + New Folder + Select).
        "<div id='fb-normbar' style='display:flex;align-items:center;gap:8px;margin-bottom:8px;flex-wrap:wrap'>"
        "<div id='breadcrumb' style='color:var(--text-dim);word-break:break-all;flex:1'>/</div>"
        "<button id='fb-select-btn' onclick='enterSelect()' style='white-space:nowrap'>"
        + std::string(ui_sections::del::kSelect) + "</button>"
        "<button id='mkdir-btn' onclick='mkdirPrompt()' style='white-space:nowrap'>+ New Folder</button>"
        "</div>"
        // Selection toolbar (count + Delete (N) + Cancel).
        "<div id='fb-selbar'>"
        "<span id='fb-count'>0 selected</span>"
        "<button id='fb-del-btn' class='btn-crit' disabled onclick='confirmSelection()'>"
        + std::string(ui_sections::del::kDelete) + " (0)</button>"
        "<button onclick='exitSelect()'>" + std::string(ui_sections::del::kCancel) + "</button>"
        "</div>"
        "<div style='overflow-x:auto'>"
        "<table><thead><tr>"
        "<th class='fb-selcol' style='width:44px;padding:4px 0;text-align:center'>"
        "<input type='checkbox' class='fb-cbx' id='fb-all' onclick='toggleAll(this)' "
        "title='" + std::string(ui_sections::del::kSelectAll) + "'></th>"
        "<th style='text-align:left;padding:4px 8px;color:var(--text-dim)'>Name</th>"
        "<th style='text-align:right;padding:4px 8px;color:var(--text-dim)'>Size</th>"
        "<th class='fb-actcol' style='text-align:right;padding:4px 8px;color:var(--text-dim)'>Actions</th>"
        "</tr></thead>"
        "<tbody id='file-list'>"
        "<tr><td colspan='4' style='padding:10px 8px;color:var(--text-dim)'>Loading&hellip;</td></tr>"
        "</tbody></table></div></div>"
        // Confirm / progress modal.
        "<div id='fb-modal'><div class='fb-card'>"
        "<h3 id='fb-title'>Delete</h3>"
        "<p class='fb-sub' id='fb-detail'></p>"
        "<p class='fb-sub' id='fb-note'></p>"
        "<p class='fb-sub'><span class='fb-warn' id='fb-undo'></span></p>"
        "<div id='fb-typewrap'>"
        "<label id='fb-typelbl'>Type DELETE to confirm:</label>"
        "<input type='text' id='fb-typein' autocomplete='off' oninput='checkType()'></div>"
        "<div id='fb-prog'>"
        "<div class='track'><div id='fb-prog-bar' style='width:0%;background:var(--crit)'></div></div>"
        "<p class='fb-sub' id='fb-prog-txt' style='margin-top:6px'></p></div>"
        "<div class='fb-foot' id='fb-foot'>"
        "<button id='fb-cancel' onclick='closeModal()'>" + std::string(ui_sections::del::kCancel) + "</button>"
        "<button id='fb-confirm' class='btn-crit' onclick='doDelete()'>" + std::string(ui_sections::del::kDelete) + "</button>"
        "</div></div></div>"
        "<script>var SD_ROOT='" + mount + "';"
        "var MANAGED=['" + mount + "/logs','" + mount + "/rawdata'];"
        "var T_UNDO=" + json_string_literal(ui_sections::del::kCannotUndo) + ";"
        "var T_KEPT=" + json_string_literal(ui_sections::del::kFolderKept) + ";"
        "var T_SELALL=" + json_string_literal(ui_sections::del::kSelectAll) + ";"
        "var T_CONFIRM=" + json_string_literal(ui_sections::del::kConfirmType) + ";"
        "var T_DELETE=" + json_string_literal(ui_sections::del::kDelete) + ";"
        "var T_DELFOLDER=" + json_string_literal(ui_sections::del::kDeleteFolder) + ";"
        "var T_EMPTYFOLDER=" + json_string_literal(ui_sections::del::kEmptyFolder) + ";</script>"
        R"JSEOF(
<script>
var filePath=SD_ROOT;
var fileEntries=[];
var selectMode=false;
var selected={};          // path -> true
var previewTimer=null;
var pendingPaths=null;    // paths armed for the open confirm dialog
function esc(s){return String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/"/g,'&quot;');}
function fmtSize(b){if(b===0)return'<span style="color:var(--text-faint)">Folder</span>';if(b>=1048576)return(b/1048576).toFixed(1)+' MB';if(b>=1024)return(b/1024).toFixed(1)+' KB';return b+' B';}
function fmtBytes(b){if(b>=1073741824)return(b/1073741824).toFixed(1)+' GB';if(b>=1048576)return(b/1048576).toFixed(1)+' MB';if(b>=1024)return(b/1024).toFixed(1)+' KB';return b+' B';}
function plural(n,w){return n+' '+w+(n===1?'':'s');}
function isManaged(p){return MANAGED.indexOf(p)>=0;}
function baseName(p){var s=p.replace(/\/+$/,'');var i=s.lastIndexOf('/');return i>=0?s.substring(i+1):s;}
function breadcrumb(path){
  var parts=path.split('/').filter(function(p){return p.length>0;});
  var html='<span style="color:var(--text-faint)">/ </span>';var built='';
  parts.forEach(function(p,i){built+='/'+p;var b=built;
    if(i<parts.length-1)html+='<a href="#" onclick="loadDir(\''+esc(b)+'\');return false">'+esc(p)+'</a><span style="color:var(--text-faint)"> / </span>';
    else html+='<span>'+esc(p)+'</span>';
  });
  document.getElementById('breadcrumb').innerHTML=html;
}
function loadDir(path){
  filePath=path;breadcrumb(path);
  var tb=document.getElementById('file-list');
  tb.innerHTML='<tr><td colspan="4" style="padding:10px 8px;color:var(--text-dim)">Loading&hellip;</td></tr>';
  fetch('/files/list?path='+encodeURIComponent(path))
    .then(function(r){return r.json();}).then(function(entries){
      fileEntries=entries.sort(function(a,b){
        if(a.is_dir!==b.is_dir)return a.is_dir?-1:1;
        return a.name<b.name?-1:a.name>b.name?1:0;
      });
      // A re-listing invalidates prior selections (paths may be gone).
      selected={};
      var rows='';
      if(path!==SD_ROOT){
        var parent=path.substring(0,path.lastIndexOf('/'))||SD_ROOT;
        rows+='<tr><td class="fb-selcol"></td><td colspan="3" style="padding:5px 8px"><a href="#" onclick="loadDir(\''+esc(parent)+'\');return false">&#8679; ..</a></td></tr>';
      }
      if(!fileEntries.length){rows+='<tr><td colspan="4" style="padding:10px 8px;color:var(--text-dim)">Empty directory</td></tr>';}
      fileEntries.forEach(function(e,i){
        var nameCell=e.is_dir
          ?'<a href="#" onclick="rowClick('+i+');return false">[dir] '+esc(e.name)+'</a>'
          :'[file] '+esc(e.name);
        var sel='<td class="fb-selcol" style="text-align:center;padding:0">'
          +'<span class="fb-cbwrap"><input type="checkbox" class="fb-cbx" '
          +'onclick="toggleRow('+i+',this)" data-i="'+i+'"></span></td>';
        var acts='';
        if(!e.is_dir)acts+='<button onclick="dlFile('+i+')" title="Download" style="margin:1px;padding:2px 8px">dl</button>';
        acts+='<button onclick="renameEntry('+i+')" title="Rename" style="margin:1px;padding:2px 8px">rn</button>';
        acts+='<button onclick="delEntry('+i+')" title="Delete" style="margin:1px;padding:2px 8px;color:var(--crit);border-color:var(--crit)">del</button>';
        // Whole-directory recursive delete (or empty, for managed dirs) — always available.
        if(e.is_dir){
          var dlbl=isManaged(e.path)?T_EMPTYFOLDER:T_DELFOLDER;
          acts+='<button onclick="confirmFolder('+i+')" title="'+esc(dlbl)+'" '
            +'style="margin:1px;padding:2px 8px;color:var(--crit);border-color:var(--crit)">'+esc(dlbl)+'</button>';
        }
        rows+='<tr style="border-bottom:1px solid var(--border)">'
          +sel
          +'<td style="padding:6px 8px">'+nameCell+'</td>'
          +'<td style="padding:6px 8px;text-align:right;color:var(--text-dim)">'+fmtSize(e.size)+'</td>'
          +'<td class="fb-actcol" style="padding:6px 4px;text-align:right;white-space:nowrap">'+acts+'</td></tr>';
      });
      tb.innerHTML=rows;
      updateCounter();
    }).catch(function(){
      document.getElementById('file-list').innerHTML='<tr><td colspan="4" style="padding:10px 8px;color:var(--crit)">Failed to load directory</td></tr>';
    });
}
function rowClick(i){if(selectMode){toggleIndex(i);}else{loadDir(fileEntries[i].path);}}
function dlFile(i){window.location.href='/files/download?path='+encodeURIComponent(fileEntries[i].path);}
function renameEntry(i){
  var e=fileEntries[i];
  var n=prompt('Rename "'+e.name+'" to:',e.name);
  if(!n||n===e.name)return;
  var dir=e.path.substring(0,e.path.lastIndexOf('/'));
  fetch('/files/rename',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({from:e.path,to:dir+'/'+n})})
    .then(function(r){return r.json();}).then(function(res){if(res.ok)loadDir(filePath);else alert('Rename failed');})
    .catch(function(){alert('Request failed');});
}
function delEntry(i){
  var e=fileEntries[i];
  if(!confirm('Delete '+(e.is_dir?'folder':'file')+' "'+e.name+'"?'))return;
  fetch('/files/delete',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({path:e.path})})
    .then(function(r){return r.json();}).then(function(res){if(res.ok)loadDir(filePath);else alert('Delete failed');})
    .catch(function(){alert('Request failed');});
}
function mkdirPrompt(){
  var n=prompt('New folder name:');
  if(!n||!n.trim())return;
  n=n.trim();
  fetch('/files/mkdir',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({path:filePath+'/'+n})})
    .then(function(r){return r.json();}).then(function(res){if(res.ok)loadDir(filePath);else alert('Failed: '+res.error);})
    .catch(function(){alert('Request failed');});
}

// ── Selection mode ───────────────────────────────────────────────────────────
function enterSelect(){selectMode=true;document.body.classList.add('fb-select');updateCounter();}
function exitSelect(){selectMode=false;selected={};document.body.classList.remove('fb-select');syncRowBoxes();updateCounter();}
function toggleIndex(i){
  var p=fileEntries[i].path;
  if(selected[p])delete selected[p];else selected[p]=true;
  syncRowBoxes();updateCounter();
}
function toggleRow(i,box){
  // Clicking a checkbox enters selection mode if not already in it.
  if(!selectMode)enterSelect();
  var p=fileEntries[i].path;
  if(box.checked)selected[p]=true;else delete selected[p];
  updateCounter();
}
function toggleAll(box){
  if(!selectMode)enterSelect();
  if(box.checked){fileEntries.forEach(function(e){selected[e.path]=true;});}
  else{selected={};}
  syncRowBoxes();updateCounter();
}
function selectedPaths(){return Object.keys(selected);}
function syncRowBoxes(){
  var boxes=document.querySelectorAll('#file-list .fb-cbx');
  boxes.forEach(function(b){
    var i=parseInt(b.getAttribute('data-i'),10);
    if(isNaN(i)||!fileEntries[i])return;
    b.checked=!!selected[fileEntries[i].path];
  });
}
function updateCounter(){
  var paths=selectedPaths();
  var n=paths.length;
  var all=document.getElementById('fb-all');
  if(all){
    if(n===0){all.checked=false;all.indeterminate=false;}
    else if(n>=fileEntries.length&&fileEntries.length>0){all.checked=true;all.indeterminate=false;}
    else{all.checked=false;all.indeterminate=true;}
  }
  var del=document.getElementById('fb-del-btn');
  del.disabled=(n===0);del.textContent=T_DELETE+' ('+n+')';
  var c=document.getElementById('fb-count');
  if(n===0){c.textContent='0 selected';return;}
  // Optimistic immediate readout from known file sizes; refined by debounced preview.
  var quick=0,allFiles=true;
  paths.forEach(function(p){
    var e=fileEntries.find(function(x){return x.path===p;});
    if(e&&!e.is_dir)quick+=e.size;else allFiles=false;
  });
  c.textContent=n+' selected'+(allFiles?' · '+fmtBytes(quick):' · …');
  if(previewTimer)clearTimeout(previewTimer);
  previewTimer=setTimeout(function(){previewSize(paths);},350);
}
function previewSize(paths){
  fetch('/files/preview',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({paths:paths})})
    .then(function(r){return r.json();}).then(function(d){
      // Only apply if selection is unchanged.
      var cur=selectedPaths();
      if(cur.length!==paths.length)return;
      if(!d.ok)return;
      var c=document.getElementById('fb-count');
      var sz=fmtBytes(d.bytes)+(d.truncated?'+':'');
      c.textContent=cur.length+' selected · '+sz;
    }).catch(function(){});
}

// ── Confirm dialog ───────────────────────────────────────────────────────────
// mode: 'selection' (N items) | 'folder' (recursive dir) ; folderManaged=>"empty".
function openConfirm(paths,opts){
  pendingPaths=paths;
  document.getElementById('fb-prog').style.display='none';
  document.getElementById('fb-foot').style.display='flex';
  var title=document.getElementById('fb-title');
  var detail=document.getElementById('fb-detail');
  var note=document.getElementById('fb-note');
  var undo=document.getElementById('fb-undo');
  var confirmBtn=document.getElementById('fb-confirm');
  var typewrap=document.getElementById('fb-typewrap');
  var typein=document.getElementById('fb-typein');
  note.textContent='';typewrap.style.display='none';typein.value='';
  undo.textContent=T_UNDO;
  title.textContent=opts.title;
  detail.textContent='…';                       // until preview lands
  confirmBtn.textContent=opts.confirmLabel||T_DELETE;
  // The destructive button defaults disabled when a typed gate applies.
  var needType=!!opts.requireType;
  confirmBtn.disabled=needType;
  if(opts.managed)note.textContent=T_KEPT;
  if(needType){
    typewrap.style.display='block';
    document.getElementById('fb-typelbl').textContent='Type '+T_CONFIRM+' to confirm:';
  }
  document.getElementById('fb-modal').classList.add('show');
  // Never default-focus the destructive button; focus Cancel.
  document.getElementById('fb-cancel').focus();
  // Fetch authoritative count + size for the detail line.
  fetch('/files/preview',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({paths:paths})})
    .then(function(r){return r.json();}).then(function(d){
      if(!d.ok){detail.textContent='Could not read selection.';return;}
      var sz=fmtBytes(d.bytes)+(d.truncated?'+':'');
      if(opts.folder){
        var parts=[plural(d.files,'file')];
        if(d.dirs>0)parts.push(plural(d.dirs,'subfolder'));
        detail.textContent=parts.join(' · ')+' · '+sz;
      }else{
        detail.textContent=plural(d.files+d.dirs,'item')+' · '+sz;
      }
      // Typed-confirm gate applies whenever a non-empty directory is in scope:
      //  - a folder op whose target has any contents (files or subfolders), or
      //  - a selection that pulled in a directory containing anything.
      var nonEmptyDir=opts.folder
        ? (d.files>0||d.dirs>0)
        : (opts.dirInScope&&(d.dirs>0));
      if(!needType&&nonEmptyDir){
        document.getElementById('fb-typewrap').style.display='block';
        document.getElementById('fb-typelbl').textContent='Type '+T_CONFIRM+' to confirm:';
        confirmBtn.disabled=true;needType=true;opts.requireType=true;
      }
    }).catch(function(){detail.textContent='Could not read selection.';});
  confirmState=opts;
}
var confirmState=null;
function checkType(){
  var v=document.getElementById('fb-typein').value;
  document.getElementById('fb-confirm').disabled=(v!==T_CONFIRM);
}
function closeModal(){
  document.getElementById('fb-modal').classList.remove('show');
  pendingPaths=null;confirmState=null;
}
function confirmSelection(){
  var paths=selectedPaths();
  if(!paths.length)return;
  // Does the selection contain any directory? (typed-confirm gate)
  var hasDir=paths.some(function(p){
    var e=fileEntries.find(function(x){return x.path===p;});
    return e&&e.is_dir;
  });
  openConfirm(paths,{
    title:'Delete '+plural(paths.length,'item')+'?',
    confirmLabel:T_DELETE,
    folder:false,
    dirInScope:hasDir
  });
}
function confirmFolder(i){
  var e=fileEntries[i];
  var managed=isManaged(e.path);
  openConfirm([e.path],{
    title:(managed?'Empty folder "':'Delete folder "')+esc(e.name).replace(/&amp;/g,'&')+'"?',
    confirmLabel:managed?T_EMPTYFOLDER:T_DELFOLDER,
    folder:true,
    managed:managed,
    requireType:!managed,   // user dir delete always gated; managed empty also gated below if non-empty
    dirInScope:true
  });
}
// ── Execute + poll ───────────────────────────────────────────────────────────
function doDelete(){
  if(!pendingPaths)return;
  var paths=pendingPaths;
  var confirmBtn=document.getElementById('fb-confirm');
  var cancelBtn=document.getElementById('fb-cancel');
  confirmBtn.disabled=true;cancelBtn.disabled=true;
  fetch('/files/delete-batch',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({paths:paths})})
    .then(function(r){return r.json().then(function(j){return{status:r.status,j:j};});})
    .then(function(res){
      var d=res.j;
      if(!d.ok){
        // Active-file block or other refusal: show as a blocking notice in-dialog.
        document.getElementById('fb-foot').style.display='flex';
        document.getElementById('fb-typewrap').style.display='none';
        document.getElementById('fb-detail').textContent=d.error||'Delete failed.';
        document.getElementById('fb-undo').textContent='';
        document.getElementById('fb-note').textContent='';
        document.getElementById('fb-title').textContent='Cannot delete';
        confirmBtn.style.display='none';
        cancelBtn.disabled=false;cancelBtn.textContent='Close';
        return;
      }
      // Async job started — switch to progress view and poll.
      document.getElementById('fb-foot').style.display='none';
      document.getElementById('fb-typewrap').style.display='none';
      var prog=document.getElementById('fb-prog');prog.style.display='block';
      document.getElementById('fb-prog-bar').style.width='0%';
      document.getElementById('fb-prog-txt').textContent='Deleting 0 / '+(d.total||paths.length)+'…';
      pollStatus();
    }).catch(function(){
      document.getElementById('fb-detail').textContent='Request failed.';
      cancelBtn.disabled=false;cancelBtn.textContent='Close';
    });
}
function pollStatus(){
  fetch('/files/delete-status',{cache:'no-store'})
    .then(function(r){return r.json();}).then(function(s){
      var bar=document.getElementById('fb-prog-bar');
      var txt=document.getElementById('fb-prog-txt');
      var pct=s.total>0?Math.round(s.progress*100/s.total):0;
      bar.style.width=pct+'%';
      if(s.state==='running'){
        txt.textContent='Deleting '+s.progress+' / '+s.total+'…';
        setTimeout(pollStatus,1000);
        return;
      }
      if(s.state==='done'||s.state==='failed'){
        bar.style.width='100%';
        if(s.failed>0){bar.style.background='var(--warn)';txt.textContent='Deleted '+s.deleted+', '+s.failed+' failed.';}
        else{bar.style.background='var(--good)';txt.textContent='Deleted '+plural(s.deleted,'item')+'.';}
        setTimeout(function(){
          closeModal();exitSelect();
          loadDir(filePath);refreshStorage();
        },1200);
        return;
      }
      // idle (shouldn't happen mid-job) — just refresh.
      setTimeout(function(){closeModal();exitSelect();loadDir(filePath);refreshStorage();},800);
    }).catch(function(){setTimeout(pollStatus,1500);});
}
// Re-fetch the storage-usage bar after a delete.
function refreshStorage(){
  fetch('/status',{cache:'no-store'}).then(function(r){return r.json();}).then(function(d){
    var holder=document.getElementById('fb-storage');
    if(!holder)return;
    if(!d.sd_mounted){holder.innerHTML='<p class="tsub">Storage: <span class="err">not mounted</span></p>';return;}
    if(!d.sd_total){holder.innerHTML='<p class="tsub">Storage: calculating&hellip;</p>';return;}
    var pct=Math.round(d.sd_used*100/d.sd_total);
    var cls=pct<80?'ok':pct<95?'warn':'err';
    var tok=pct<80?'good':pct<95?'warn':'crit';
    holder.innerHTML='<div class="tsub" style="margin-bottom:4px">Storage: <span class="'+cls+'">'
      +fmtBytes(d.sd_used)+' used of '+fmtBytes(d.sd_total)+' ('+pct+'%)</span></div>'
      +'<div class="track"><div style="background:var(--'+tok+');width:'+pct+'%"></div></div>';
  }).catch(function(){});
}
loadDir(SD_ROOT);
</script>
)JSEOF";

    return server->send_page(
        request, "SD Card Files", content, "/files");
}

esp_err_t AdminWebServer::files_list_handler(httpd_req_t *request) {
    AdminWebServer *server = self(request);
    if (!server->authorize(request)) return server->send_unauthorized(request);

    if (!server->sd_->is_mounted()) {
        httpd_resp_set_type(request, "application/json");
        return httpd_resp_sendstr(request, "[]");
    }
    std::string path = server->query_param(request, "path");
    if (path.empty()) path = SdManager::kMountPoint;

    if (!SdManager::safe_path(path.c_str())) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Invalid path");
    }
    char *json = server->sd_->list_dir(path.c_str());
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_sendstr(request, json ? json : "[]");
    free(json);
    return ESP_OK;
}

esp_err_t AdminWebServer::files_download_handler(httpd_req_t *request) {
    AdminWebServer *server = self(request);
    if (!server->authorize(request)) return server->send_unauthorized(request);

    if (!server->sd_->is_mounted()) {
        httpd_resp_set_status(request, "503 Service Unavailable");
        return httpd_resp_sendstr(request, "SD card not mounted");
    }
    const std::string path = server->query_param(request, "path");
    if (path.empty() || !SdManager::safe_path(path.c_str())) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Invalid path");
    }
    struct stat st;
    if (stat(path.c_str(), &st) != 0 || S_ISDIR(st.st_mode)) {
        return httpd_resp_send_err(request, HTTPD_404_NOT_FOUND, "Not found");
    }
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) {
        return httpd_resp_send_err(
            request, HTTPD_500_INTERNAL_SERVER_ERROR, "Open failed");
    }
    const char *filename = strrchr(path.c_str(), '/');
    filename = filename ? filename + 1 : path.c_str();
    char disp[160];
    snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"", filename);
    httpd_resp_set_type(request, "application/octet-stream");
    httpd_resp_set_hdr(request, "Content-Disposition", disp);
    // 4 KB chunks reduce TLS-record overhead per byte; one tick (10 ms @ 100 Hz)
    // between chunks paces output to ~400 KB/s so we don't flood the esp_hosted
    // SDIO queue and cause silent TCP packet drops / download stalls.
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (httpd_resp_send_chunk(request, buf, static_cast<ssize_t>(n)) != ESP_OK) break;
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    fclose(f);
    httpd_resp_send_chunk(request, nullptr, 0);
    return ESP_OK;
}

esp_err_t AdminWebServer::files_delete_handler(httpd_req_t *request) {
    AdminWebServer *server = self(request);
    if (!server->authorize(request)) return server->send_unauthorized(request);

    const std::string body = read_body(request, 512);
    const std::string path = json_field(body, "path");
    const bool ok = !path.empty() &&
                    SdManager::safe_path(path.c_str()) &&
                    server->sd_->delete_entry(path.c_str());
    httpd_resp_set_type(request, "application/json");
    return httpd_resp_sendstr(request, ok ? "{\"ok\":true}" : "{\"ok\":false}");
}

// POST /files/preview — body {"paths":[...]} or {"path":"..."}.
// Returns {"ok":bool,"files":n,"dirs":n,"bytes":n,"truncated":bool}.
esp_err_t AdminWebServer::files_preview_handler(httpd_req_t *request) {
    AdminWebServer *server = self(request);
    if (!server->authorize(request)) return server->send_unauthorized(request);
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");

    if (!server->sd_->is_mounted()) {
        return httpd_resp_sendstr(
            request, "{\"ok\":false,\"error\":\"SD not mounted\"}");
    }
    const std::string body = read_body(request, 8192);
    bool rejected = false;
    std::vector<std::string> paths = json_string_array(body, "paths", &rejected);
    if (rejected) {
        return httpd_resp_sendstr(
            request, "{\"ok\":false,\"error\":\"invalid paths\"}");
    }
    if (paths.empty()) {
        const std::string single = json_field(body, "path");
        if (!single.empty()) paths.push_back(single);
    }
    if (paths.empty()) {
        return httpd_resp_sendstr(
            request, "{\"ok\":false,\"error\":\"no paths\"}");
    }
    std::vector<const char *> cptrs;
    cptrs.reserve(paths.size());
    for (const auto &p : paths) cptrs.push_back(p.c_str());

    const SdManager::DeletePreview pv =
        sd_delete::preview(cptrs.data(), cptrs.size());
    char out[160];
    snprintf(out, sizeof(out),
        "{\"ok\":%s,\"files\":%u,\"dirs\":%u,\"bytes\":%llu,\"truncated\":%s}",
        pv.ok ? "true" : "false",
        static_cast<unsigned>(pv.files),
        static_cast<unsigned>(pv.dirs),
        static_cast<unsigned long long>(pv.bytes),
        pv.truncated ? "true" : "false");
    return httpd_resp_sendstr(request, out);
}

// POST /files/delete-batch — body {"paths":[...]}.
// Pre-checks the active-RINEX-file guard, refuses if a job is already running
// (HTTP 409), else starts the off-task delete and returns {"ok":true,"async":true,"total":n}.
esp_err_t AdminWebServer::files_delete_batch_handler(httpd_req_t *request) {
    AdminWebServer *server = self(request);
    if (!server->authorize(request)) return server->send_unauthorized(request);
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");

    if (!server->sd_->is_mounted()) {
        return httpd_resp_sendstr(
            request, "{\"ok\":false,\"error\":\"SD not mounted\"}");
    }
    if (sd_delete::busy()) {
        httpd_resp_set_status(request, "409 Conflict");
        return httpd_resp_sendstr(
            request, "{\"ok\":false,\"error\":\"delete in progress\"}");
    }
    const std::string body = read_body(request, 8192);
    bool rejected = false;
    std::vector<std::string> paths = json_string_array(body, "paths", &rejected);
    if (rejected) {
        return httpd_resp_sendstr(
            request, "{\"ok\":false,\"error\":\"invalid paths\"}");
    }
    if (paths.empty()) {
        const std::string single = json_field(body, "path");
        if (!single.empty()) paths.push_back(single);
    }
    if (paths.empty()) {
        return httpd_resp_sendstr(
            request, "{\"ok\":false,\"error\":\"no paths\"}");
    }
    std::vector<const char *> cptrs;
    cptrs.reserve(paths.size());
    for (const auto &p : paths) cptrs.push_back(p.c_str());

    // Active-RINEX-file guard (caller layer; needs BaseStation visibility).
    char block_msg[160] = {0};
    if (sd_delete::blocks_active_file(
            cptrs.data(), cptrs.size(), block_msg, sizeof(block_msg))) {
        std::string out = "{\"ok\":false,\"error\":\"" +
            json_escape(block_msg) + "\"}";
        return httpd_resp_sendstr(request, out.c_str());
    }

    if (!sd_delete::start(cptrs.data(), cptrs.size())) {
        // Lost the race / unmounted / empty / still in scope: surface generically.
        if (sd_delete::busy()) {
            httpd_resp_set_status(request, "409 Conflict");
            return httpd_resp_sendstr(
                request, "{\"ok\":false,\"error\":\"delete in progress\"}");
        }
        return httpd_resp_sendstr(
            request, "{\"ok\":false,\"error\":\"could not start delete\"}");
    }
    char out[64];
    snprintf(out, sizeof(out), "{\"ok\":true,\"async\":true,\"total\":%u}",
             static_cast<unsigned>(paths.size()));
    return httpd_resp_sendstr(request, out);
}

// GET /files/delete-status — snapshot of the running/last delete job.
esp_err_t AdminWebServer::files_delete_status_handler(httpd_req_t *request) {
    AdminWebServer *server = self(request);
    if (!server->authorize(request)) return server->send_unauthorized(request);
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");

    const sd_delete::Progress pr = sd_delete::poll();
    const char *state =
        pr.state == sd_delete::State::kRunning  ? "running" :
        pr.state == sd_delete::State::kDoneOk   ? "done" :
        pr.state == sd_delete::State::kDoneFail ? "failed" : "idle";
    std::string out =
        std::string("{\"state\":\"") + state +
        "\",\"progress\":" + std::to_string(pr.done) +
        ",\"total\":" + std::to_string(pr.total) +
        ",\"deleted\":" + std::to_string(pr.result.deleted) +
        ",\"failed\":" + std::to_string(pr.result.failed) +
        ",\"skipped\":" + std::to_string(pr.result.skipped) +
        ",\"error\":\"" + json_escape(pr.result.first_error) + "\"}";
    return httpd_resp_sendstr(request, out.c_str());
}

esp_err_t AdminWebServer::files_rename_handler(httpd_req_t *request) {
    AdminWebServer *server = self(request);
    if (!server->authorize(request)) return server->send_unauthorized(request);

    const std::string body = read_body(request, 512);
    const std::string from = json_field(body, "from");
    const std::string to   = json_field(body, "to");
    const bool ok = !from.empty() && !to.empty() &&
                    SdManager::safe_path(from.c_str()) &&
                    SdManager::safe_path(to.c_str()) &&
                    server->sd_->rename_entry(from.c_str(), to.c_str());
    httpd_resp_set_type(request, "application/json");
    return httpd_resp_sendstr(request, ok ? "{\"ok\":true}" : "{\"ok\":false}");
}

esp_err_t AdminWebServer::files_mkdir_handler(httpd_req_t *request) {
    AdminWebServer *server = self(request);
    if (!server->authorize(request)) return server->send_unauthorized(request);

    if (!server->sd_->is_mounted()) {
        httpd_resp_set_type(request, "application/json");
        return httpd_resp_sendstr(request, "{\"ok\":false,\"error\":\"not mounted\"}");
    }
    const std::string body = read_body(request, 512);
    const std::string path = json_field(body, "path");
    if (path.empty() || !SdManager::safe_path(path.c_str())) {
        httpd_resp_set_type(request, "application/json");
        return httpd_resp_sendstr(request, "{\"ok\":false,\"error\":\"invalid path\"}");
    }
    const bool ok = mkdir(path.c_str(), 0755) == 0;
    httpd_resp_set_type(request, "application/json");
    return httpd_resp_sendstr(request, ok ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"mkdir failed\"}");
}

// POST /rinex/toggle — body: start=1 or start=0
esp_err_t AdminWebServer::rinex_toggle_handler(httpd_req_t *request) {
    AdminWebServer *server = self(request);
    if (!server->authorize(request)) return server->send_unauthorized(request);

    const std::string body  = read_body(request, 64);
    const std::string start = form_value(body, "start");
    const bool enable = (start == "1" || start == "true");
    const esp_err_t err = server->station_->request_raw_collection(enable);
    httpd_resp_set_type(request, "application/json");
    if (err != ESP_OK) {
        return httpd_resp_sendstr(request, "{\"ok\":false}");
    }
    return httpd_resp_sendstr(request, "{\"ok\":true}");
}

esp_err_t AdminWebServer::ntrip_toggle_handler(httpd_req_t *request) {
    AdminWebServer *server = self(request);
    if (!server->authorize(request)) return server->send_unauthorized(request);

    const std::string body = read_body(request, 64);
    const std::string on   = form_value(body, "on");
    const bool enable = (on == "1" || on == "true");
    server->station_->set_streams_enabled(enable);  // persists across power cycles
    httpd_resp_set_type(request, "application/json");
    return httpd_resp_sendstr(request, "{\"ok\":true}");
}

// GET /rinex/export  — date/time picker page
esp_err_t AdminWebServer::rinex_export_page_handler(httpd_req_t *request) {
    AdminWebServer *server = self(request);
    if (!server->authorize(request)) return server->send_unauthorized(request);

    // Scan rawdata to find the earliest and latest available RINEX files.
    time_t t_min = 0, t_max = 0;
    {
        DIR *dir = opendir(kRawDataDir);
        if (dir) {
            struct dirent *ent;
            while ((ent = readdir(dir)) != nullptr) {
                if (ent->d_type != DT_REG) continue;
                time_t ft = parse_rinex_filename_utc(ent->d_name);
                if (ft <= 0) continue;
                if (t_min == 0 || ft < t_min) t_min = ft;
                if (ft > t_max) t_max = ft;
            }
            closedir(dir);
        }
    }
    if (t_max > 0) t_max += 3600;  // end of last file's hour

    // Format Unix time as "YYYY-MM-DDTHH:MM" for datetime-local inputs.
    auto fmt_dt = [](time_t t, char *buf, size_t len) {
        struct tm tm {};
        gmtime_r(&t, &tm);
        snprintf(buf, len, "%04d-%02d-%02dT%02d:%02d",
                 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                 tm.tm_hour, tm.tm_min);
    };

    char start_val[20] = "", end_val[20] = "";
    if (t_min > 0) fmt_dt(t_min, start_val, sizeof(start_val));
    if (t_max > 0) fmt_dt(t_max, end_val, sizeof(end_val));

    const std::string no_data_note = (t_min == 0)
        ? "<p class='warn'>No RINEX files found in /rawdata.</p>"
        : "";

    const std::string content =
        "<div class='sec'><h2>RINEX Export</h2>" +
        no_data_note +
        "<p class='dim'>Select a UTC time range. All 1-hour RINEX files within "
        "that range will be merged into a single download.</p>"
        "<label for='start' style='display:block;margin:10px 0 4px;color:var(--text-dim)'>Start (UTC)</label>"
        "<input type='datetime-local' id='start' value='" + std::string(start_val) + "'>"
        "<label for='end' style='display:block;margin:10px 0 4px;color:var(--text-dim)'>End (UTC)</label>"
        "<input type='datetime-local' id='end' value='" + std::string(end_val) + "'>"
        "<br><button id='btn' onclick='doExport()' style='margin-top:16px'>"
        "&#8659; Export &amp; Download</button>"
        "<div id='status' style='margin-top:12px;font-size:0.85em;min-height:1.5em'></div></div>"
        "<script>"
        "function fmtSize(b){return b>=1048576?(b/1048576).toFixed(1)+' MB':(b/1024).toFixed(0)+' KB';}"
        "async function doExport(){"
        "var s=document.getElementById('start').value;"
        "var e=document.getElementById('end').value;"
        "if(!s||!e){alert('Please set both start and end times.');return;}"
        "if(s>=e){alert('Start must be before end.');return;}"
        "var btn=document.getElementById('btn');"
        "var st=document.getElementById('status');"
        "btn.disabled=true;btn.textContent='Exporting…';"
        "st.style.color='var(--text-dim)';st.textContent='Connecting…';"
        "var t0=Date.now(),rx=0;"
        "var dots='';var dotTimer=setInterval(function(){dots=dots.length<3?dots+'.':'';btn.textContent='Exporting'+dots;},500);"
        "try{"
        "var resp=await fetch('/rinex/export',{method:'POST',"
        "headers:{'Content-Type':'application/json'},"
        "credentials:'same-origin',"
        "body:JSON.stringify({start:s,end:e})});"
        "if(!resp.ok){throw new Error('HTTP '+resp.status+(resp.status===404?' — no files in range':''));}"
        "var reader=resp.body.getReader();"
        "var chunks=[];"
        "while(true){"
        "var res=await reader.read();"
        "if(res.done)break;"
        "chunks.push(res.value);"
        "rx+=res.value.length;"
        "var sec=Math.round((Date.now()-t0)/1000);"
        "st.textContent=fmtSize(rx)+' received — '+sec+'s elapsed — do not navigate away.';"
        "}"
        "clearInterval(dotTimer);"
        "var blob=new Blob(chunks,{type:'application/octet-stream'});"
        "var fn='rinex_'+s.replace(/[T:]/g,'').replace(/-/g,'')+'.rnx';"
        "var a=document.createElement('a');a.href=URL.createObjectURL(blob);a.download=fn;a.click();URL.revokeObjectURL(a.href);"
        "st.style.color='var(--good)';"
        "st.textContent='✓ Done — '+fmtSize(blob.size)+' downloaded.';"
        "}catch(err){"
        "clearInterval(dotTimer);"
        "st.style.color='var(--crit)';"
        "st.textContent='✗ '+err.message;"
        "}"
        "btn.disabled=false;btn.textContent='⇓ Export & Download';"
        "}"
        "</script>";

    return server->send_page(
        request, "RINEX Export", content, "/files");
}

// POST /rinex/export  body: {"start":"YYYY-MM-DDTHH:MM","end":"YYYY-MM-DDTHH:MM"}
// Selects matching rawdata .rnx files and streams them merged.
esp_err_t AdminWebServer::rinex_export_handler(httpd_req_t *request) {
    AdminWebServer *server = self(request);
    if (!server->authorize(request)) return server->send_unauthorized(request);

    const std::string body = read_body(request, 256);
    if (body.empty()) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "No body");
    }

    // Extract "start" and "end" string values from the JSON body.
    auto extract_field = [&](const char *key) -> std::string {
        const std::string needle = std::string("\"") + key + "\":\"";
        size_t p = body.find(needle);
        if (p == std::string::npos) return {};
        p += needle.size();
        size_t q = body.find('"', p);
        if (q == std::string::npos) return {};
        return body.substr(p, q - p);
    };

    const time_t start = parse_datetime_input(extract_field("start").c_str());
    const time_t end   = parse_datetime_input(extract_field("end").c_str());

    if (start < 0 || end < 0 || end <= start) {
        return httpd_resp_send_err(
            request, HTTPD_400_BAD_REQUEST, "Invalid time range");
    }

    const std::vector<std::string> paths = select_rinex_files(start, end);
    if (paths.empty()) {
        return httpd_resp_send_err(
            request, HTTPD_404_NOT_FOUND, "No RINEX files in range");
    }

    // Read the TIME OF LAST OBS value field (columns 1–60) from the last file's
    // header so we can patch it into the first file's header on output.
    // RINEX 3 header lines: cols 1-60 = value, cols 61-80 = label.
    char last_obs_value[61] = {};  // 60 chars + NUL
    {
        FILE *lf = fopen(paths.back().c_str(), "r");
        if (lf) {
            char line[128];
            while (fgets(line, sizeof(line), lf)) {
                size_t len = strlen(line);
                if (len >= 76 && strncmp(line + 60, "TIME OF LAST OBS", 16) == 0) {
                    memcpy(last_obs_value, line, 60);
                    break;
                }
                if (len >= 60 && strstr(line + 60, "END OF HEADER")) break;
            }
            fclose(lf);
        }
    }

    httpd_resp_set_type(request, "application/octet-stream");
    httpd_resp_set_hdr(request, "Content-Disposition",
                       "attachment; filename=\"export.rnx\"");

    // 4 KB chunks + one tick (10 ms @ 100 Hz) paces output to ~400 KB/s,
    // staying within esp_hosted SDIO bandwidth.
    char buf[4096];
    bool header_sent = false;

    for (const std::string &path : paths) {
        FILE *f = fopen(path.c_str(), "r");
        if (!f) continue;

        if (!header_sent) {
            // Stream first file's header line by line so we can patch
            // TIME OF LAST OBS, then bulk-read the observations.
            bool past_header = false;
            while (!past_header) {
                if (!fgets(buf, sizeof(buf), f)) break;
                size_t len = strlen(buf);
                // Detect line ending from the original line (\r\n or \n).
                const char *eol = (len >= 2 && buf[len - 2] == '\r') ? "\r\n" : "\n";

                if (last_obs_value[0] != '\0' && len >= 76 &&
                    strncmp(buf + 60, "TIME OF LAST OBS", 16) == 0) {
                    // Replace the 60-char value field, preserve label and line ending.
                    char patched[90];
                    int pl = snprintf(patched, sizeof(patched),
                                      "%sTIME OF LAST OBS    %s",
                                      last_obs_value, eol);
                    if (httpd_resp_send_chunk(request, patched, pl) != ESP_OK) {
                        fclose(f);
                        goto done;
                    }
                } else {
                    if (httpd_resp_send_chunk(
                            request, buf, static_cast<ssize_t>(len)) != ESP_OK) {
                        fclose(f);
                        goto done;
                    }
                }
                if (len >= 60 && strstr(buf + 60, "END OF HEADER")) past_header = true;
            }
            // Bulk-stream the observations from the first file.
            size_t n;
            while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
                if (httpd_resp_send_chunk(
                        request, buf, static_cast<ssize_t>(n)) != ESP_OK) {
                    fclose(f);
                    goto done;
                }
                vTaskDelay(pdMS_TO_TICKS(1));
            }
            header_sent = true;
        } else {
            bool past_header = false;
            while (!past_header && fgets(buf, sizeof(buf), f)) {
                if (strstr(buf, "END OF HEADER")) past_header = true;
            }
            if (past_header) {
                size_t n;
                while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
                    if (httpd_resp_send_chunk(
                            request, buf, static_cast<ssize_t>(n)) != ESP_OK) {
                        fclose(f);
                        goto done;
                    }
                    vTaskDelay(pdMS_TO_TICKS(1));
                }
            }
        }
        fclose(f);
    }

done:
    httpd_resp_send_chunk(request, nullptr, 0);
    return ESP_OK;
}
