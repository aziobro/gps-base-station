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

std::string uptime_str(uint64_t seconds) {
    char text[40];
    unsigned d = seconds / 86400;
    unsigned h = (seconds % 86400) / 3600;
    unsigned m = (seconds % 3600) / 60;
    unsigned s = seconds % 60;
    if (d) snprintf(text, sizeof(text), "%ud %02u:%02u:%02u", d, h, m, s);
    else   snprintf(text, sizeof(text), "%02u:%02u:%02u", h, m, s);
    return text;
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

std::string local_client_ips_html(
    const LocalCaster::ClientSnapshot &clients) {
    if (clients.count == 0) return "<span class='dim'>none</span>";
    std::string out;
    for (int i = 0; i < clients.count; ++i) {
        if (i) out += ", ";
        out += ipv4_to_string(clients.ipv4[i]);
    }
    return out;
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


std::string escape_html(const std::string &value) {
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

std::string service_html(const NtripStatus &status) {
    if (!status.enabled) return "<span class='warn'>disabled</span>";
    const char *css = status.connected ? "ok" : "err";
    const std::string label =
        status.connected
            ? "connected <span class='dim'>(" + uptime_str(status.connected_sec) + ")</span>"
            : escape_html(status.message);
    std::string out = "<span class='" + std::string(css) + "'>" + label + "</span>"
           " <span class='dim'>| " + human_bytes(status.bytes_sent) +
           " sent | " + std::to_string(status.dropped_batches) + " dropped";
    if (status.reconnects) {
        out += " | " + std::to_string(status.reconnects) + " reconnects";
    }
    if (status.ever_sent) {
        out += " | last data " + std::to_string(status.last_send_age_sec) + "s ago";
    }
    out += "</span>";
    if (!status.connected && !status.last_error.empty()) {
        out += " <span class='err' style='font-size:0.85em'>" +
               escape_html(status.last_error) + "</span>";
    }
    return out;
}

std::string rssi_html(int rssi) {
    const char *css = "err";
    const char *health = "weak";
    if (rssi >= -60) {
        css = "ok";
        health = "excellent";
    } else if (rssi >= -70) {
        css = "ok";
        health = "good";
    } else if (rssi >= -80) {
        css = "warn";
        health = "fair";
    }
    return "<span class='" + std::string(css) + "'>" +
           std::to_string(rssi) + " dBm (" + health + ")</span>";
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

}  // namespace

esp_err_t AdminWebServer::start(
    Storage &storage, WifiManager &wifi,
    BaseStation &station, SdManager &sd) {
    storage_ = &storage;
    wifi_ = &wifi;
    station_ = &station;
    sd_ = &sd;

    httpd_ssl_config_t tls_config = HTTPD_SSL_CONFIG_DEFAULT();
    tls_config.httpd.max_uri_handlers = 32;
    tls_config.httpd.stack_size = 12288;
    tls_config.httpd.recv_wait_timeout = 30;
    tls_config.httpd.send_wait_timeout = 30;
    tls_config.httpd.lru_purge_enable = true;
    // Allow up to seven dashboard tabs plus one administrative request (for
    // example, configuration or OTA) without evicting an active TLS session.
    tls_config.httpd.max_open_sockets = 8;
    tls_config.httpd.keep_alive_enable = true;
    tls_config.httpd.keep_alive_idle = 15;
    tls_config.httpd.keep_alive_interval = 5;
    tls_config.httpd.keep_alive_count = 2;
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
        {"/files/rename", HTTP_POST, files_rename_handler, this},
        {"/files/mkdir", HTTP_POST, files_mkdir_handler, this},
        {"/rinex/toggle", HTTP_POST, rinex_toggle_handler, this},
        {"/rinex/export", HTTP_GET,  rinex_export_page_handler, this},
        {"/rinex/export", HTTP_POST, rinex_export_handler, this},
        {"/ntrip/toggle", HTTP_POST, ntrip_toggle_handler, this},
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

    const BasePosition position = server->storage_->load_position();
    const BaseStationStatus station = server->station_->status();
    const std::string ip = server->wifi_->ip_address();
    const char *wifi_state = server->wifi_->connected()
        ? "connected" : (server->wifi_->access_point_active() ? "AP fallback" : "disconnected");

    char position_row[256];
    if (position.valid) {
        snprintf(position_row, sizeof(position_row),
                 "%.8f, %.8f, %.4f m", position.lat, position.lon, position.height);
    } else {
        strlcpy(position_row, "not set", sizeof(position_row));
    }

    const int rssi = server->wifi_->rssi();
    const std::string ssid = server->wifi_->ssid();
    const size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    const size_t total_heap = heap_caps_get_total_size(MALLOC_CAP_8BIT);
    const unsigned free_percent =
        total_heap ? static_cast<unsigned>(free_heap * 100 / total_heap) : 0;
    const char *heap_class =
        free_percent >= 40 ? "ok" : (free_percent >= 20 ? "warn" : "err");
    const unsigned satellite_total =
        station.survey.gps + station.survey.glonass +
        station.survey.galileo + station.survey.beidou;

    const SurveySnapshot &sv = station.survey;
    char survey_buf[256];
    if (sv.state == SurveyState::kCollecting) {
        int n = snprintf(survey_buf, sizeof(survey_buf),
            "<span class='warn'>collecting</span> <span class='dim'>%us "
            "&middot; %d samples &middot; %d blocks &middot; </span>",
            static_cast<unsigned>(sv.elapsed_sec), sv.samples, sv.blocks);
        if (n < 0) n = 0;
        if (sv.valid && sv.stability < 9000.0F) {
            snprintf(survey_buf + n, sizeof(survey_buf) - n,
                "stability %.3f m <span class='dim'>| %.8f, %.8f, %.3f m</span>",
                sv.stability, sv.lat, sv.lon, sv.height);
        } else {
            snprintf(survey_buf + n, sizeof(survey_buf) - n, "stabilizing&hellip;");
        }
    } else if (sv.state == SurveyState::kDone) {
        strlcpy(survey_buf, "<span class='ok'>complete</span>", sizeof(survey_buf));
    } else {
        strlcpy(survey_buf, "<span class='dim'>idle</span>", sizeof(survey_buf));
    }

    std::string content =
        "<h2>Status</h2><table>"
        "<tr><td>Framework</td><td>ESP-IDF " + std::string(esp_get_idf_version()) + "</td></tr>"
        "<tr><td>Application</td><td>" + std::string(esp_app_get_description()->version) + "</td></tr>"
        "<tr><td>Uptime</td><td id='st-uptime'>" +
        uptime_str(esp_timer_get_time() / 1000000ULL) + "</td></tr>"
        "<tr><td>Last reset</td><td id='st-reset'>" +
        std::string(reset_reason_str(esp_reset_reason())) + "</td></tr>"
        "<tr><td>System health</td><td id='st-health'><span class='" +
        std::string(server->station_->healthy() ? "ok'>healthy" : "err'>unhealthy") +
        "</span></td></tr>"
        "<tr><td>WiFi</td><td id='st-wifi'>" + wifi_state +
        (ssid.empty() ? "" : " <span class='dim'>| " + html_escape(ssid) + "</span>") +
        "</td></tr>"
        "<tr><td>IP</td><td id='st-ip'>" +
        html_escape(ip.empty() ? "192.168.4.1" : ip) + "</td></tr>"
        "<tr><td>WiFi signal</td><td id='st-rssi'>" + rssi_html(rssi) + "</td></tr>"
        "<tr><td>Mode</td><td id='st-mode'><span class='" +
        std::string(station.mode == BaseMode::kTransmit ? "ok'>Base TX" : "warn'>Survey") +
        "</span>"
        "</td></tr>"
        "<tr><td>Position</td><td>" + std::string(position_row) + "</td></tr>"
        "<tr><td>Survey</td><td id='st-survey'>" + std::string(survey_buf) +
        "</td></tr>"
        "<tr><td>Satellites</td><td id='st-sats'>GPS " +
        std::to_string(station.survey.gps) + " / GLO " +
        std::to_string(station.survey.glonass) + " / GAL " +
        std::to_string(station.survey.galileo) + " / BDS " +
        std::to_string(station.survey.beidou) +
        " <span class='dim'>| total " + std::to_string(satellite_total) +
        "</span></td></tr>"
        "<tr><td>RTCM</td><td id='st-rtcm'>" +
        std::to_string(station.rtcm_bytes_per_second) + " B/s"
        " <span class='dim'>| " + human_bytes(station.rtcm_bytes_total) +
        " total</span></td></tr>"
        "<tr><td>Local NTRIP clients</td><td id='st-clients'>" +
        std::to_string(station.local_clients) + "</td></tr>"
        "<tr><td>Local NTRIP client IPs</td><td id='st-client-ips'>" +
        local_client_ips_html(station.local_client_ips) + "</td></tr>"
        "<tr><td>NTRIP push</td><td id='st-ntrip'>" +
        [&]() {
            const bool en = server->station_->streams_enabled();
            return std::string("<span class='") + (en ? "ok'>enabled" : "warn'>disabled") +
                "</span> &nbsp; <button onclick=\"toggleNtrip(" +
                (en ? "false" : "true") + ")\">" + (en ? "Disable" : "Enable") +
                "</button>";
        }() +
        "</td></tr>"
        "<tr><td>RTK2go</td><td id='st-r2g'>" +
        service_html(station.rtk2go) + "</td></tr>"
        "<tr><td>Onocoy</td><td id='st-onc'>" +
        service_html(station.onocoy) + "</td></tr>"
        "<tr><td>RTKdata</td><td id='st-rtk'>" +
        service_html(station.rtkdata) + "</td></tr>"
        "<tr><td>Free heap</td><td id='st-heap'><span class='" +
        std::string(heap_class) + "'>" + human_bytes(free_heap) + " (" +
        std::to_string(free_percent) + "% free)</span>"
        " <span class='dim'>| low watermark " +
        human_bytes(heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT)) +
        "</span></td></tr>"
        "<tr><td>SD card</td><td id='st-sd'>" +
        [&]() {
            if (!server->sd_->is_mounted()) return std::string("<span class='err'>not mounted</span>");
            const auto ds = server->sd_->disk_stats();
            if (!ds.valid) return std::string("<span class='ok'>mounted</span>");
            const unsigned pct = ds.total_bytes
                ? static_cast<unsigned>(ds.used_bytes * 100 / ds.total_bytes) : 0;
            const char *cls = pct < 80 ? "ok" : (pct < 95 ? "warn" : "err");
            return std::string("<span class='") + cls + "'>" +
                human_bytes(ds.used_bytes) + " used of " +
                human_bytes(ds.total_bytes) + " (" +
                std::to_string(pct) + "%)</span>";
        }() +
        "</td></tr>"
        "<tr><td>RINEX collection</td><td id='st-rinex'>" +
        [&]() {
            const auto rs = server->station_->rinex_status();
            if (!rs.active) {
                return std::string("<span class='dim'>inactive</span>"
                    " &nbsp; <button onclick=\"toggleRinex(true)\">Start</button>");
            }
            const std::string fname = rs.current_file.empty() ? "" :
                rs.current_file.substr(rs.current_file.rfind('/') + 1);
            return std::string("<span class='ok'>active</span>"
                " <span class='dim'>| epochs: ") + std::to_string(rs.epochs) +
                (fname.empty() ? "" : " | " + html_escape(fname)) +
                "</span>"
                " &nbsp; <button onclick=\"toggleRinex(false)\">Stop</button>";
        }() +
        "</td></tr>"
        "</table><p><a href='/config'>Configuration</a> &nbsp; "
        "<a href='/skyplot'>Sky plot</a> &nbsp; "
        "<a href='/files'>SD card files</a> &nbsp; "
        "<a href='/rinex/export'>RINEX export</a> &nbsp; "
        "<a href='/logs'>Console logs</a> &nbsp; "
        "<a href='/status'>JSON status</a> &nbsp; "
        "<a href='/update'>Firmware update</a></p>"
        R"HTML(<script>
let statusRequest=false;
function esc(v){return String(v).replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));}
function bytes(v){return v>=1048576?(v/1048576).toFixed(1)+' MB':v>=1024?(v/1024).toFixed(1)+' KB':v+' B';}
function set(id,v){const e=document.getElementById(id);if(e)e.innerHTML=v;}
function upt(s){s=s|0;var d=(s/86400)|0,h=((s%86400)/3600)|0,m=((s%3600)/60)|0,x=s%60,p=n=>String(n).padStart(2,'0');return(d?d+'d ':'')+p(h)+':'+p(m)+':'+p(x);}
function svc(p){if(!p.enabled)return "<span class='warn'>disabled</span>";const c=p.connected?'ok':'err',m=p.connected?("connected <span class='dim'>("+upt(p.connected_sec)+")</span>"):esc(p.message);var d="<span class='"+c+"'>"+m+"</span> <span class='dim'>| "+bytes(p.bytes)+" sent | "+p.dropped+" dropped";if(p.reconnects)d+=" | "+p.reconnects+" reconnects";if(p.ever_sent)d+=" | last data "+p.last_send_age+"s ago";d+="</span>";if(!p.connected&&p.last_error)d+=" <span class='err' style='font-size:0.85em'>"+esc(p.last_error)+"</span>";return d;}
function ips(a){return a&&a.length?esc(a.join(', ')):"<span class='dim'>none</span>";}
async function refresh(){
 if(statusRequest)return;statusRequest=true;
 try{
  const r=await fetch('/status',{cache:'no-store'});if(!r.ok)throw Error(r.status);
  const d=await r.json(),total=d.gps+d.glonass+d.galileo+d.beidou;
  set('st-health',"<span class='"+(d.healthy?'ok':'err')+"'>"+(d.healthy?'healthy':'unhealthy')+"</span>");
  set('st-uptime',upt(d.uptime_sec));set('st-reset',esc(d.reset_reason));
  set('st-wifi',(d.wifi_connected?'connected':d.ap_active?'AP fallback':'disconnected')+(d.ssid?" <span class='dim'>| "+esc(d.ssid)+"</span>":""));
  set('st-ip',esc(d.ip||'192.168.4.1'));
  const rc=d.rssi>=-70?'ok':d.rssi>=-80?'warn':'err',rh=d.rssi>=-60?'excellent':d.rssi>=-70?'good':d.rssi>=-80?'fair':'weak';
  set('st-rssi',"<span class='"+rc+"'>"+d.rssi+" dBm ("+rh+")</span>");
  set('st-mode',"<span class='"+(d.mode==='base_tx'?'ok':'warn')+"'>"+(d.mode==='base_tx'?'Base TX':'Survey')+"</span>");
  let sv;
  if(d.survey_state==='collecting'){sv="<span class='warn'>collecting</span> <span class='dim'>"+d.survey_elapsed+"s &middot; "+d.survey_samples+" samples &middot; "+d.survey_blocks+" blocks &middot; </span>";sv+=(d.survey_valid&&d.survey_stability<9000)?("stability "+d.survey_stability.toFixed(3)+" m <span class='dim'>| "+d.survey_lat.toFixed(8)+", "+d.survey_lon.toFixed(8)+", "+d.survey_height.toFixed(3)+" m</span>"):"stabilizing&hellip;";}
  else if(d.survey_state==='done')sv="<span class='ok'>complete</span>";
  else sv="<span class='dim'>idle</span>";
  set('st-survey',sv);
  set('st-sats','GPS '+d.gps+' / GLO '+d.glonass+' / GAL '+d.galileo+' / BDS '+d.beidou+" <span class='dim'>| total "+total+"</span>");
  set('st-rtcm',d.rtcm_bps+" B/s <span class='dim'>| "+bytes(d.rtcm_total)+" total</span>");
  set('st-clients',d.local_clients);set('st-client-ips',ips(d.local_client_ips));set('st-r2g',svc(d.rtk2go));set('st-onc',svc(d.onocoy));set('st-rtk',svc(d.rtkdata));
  set('st-ntrip',(d.ntrip_enabled?"<span class='ok'>enabled</span>":"<span class='warn'>disabled</span>")+" &nbsp; <button onclick=\"toggleNtrip("+(d.ntrip_enabled?'false':'true')+")\">"+(d.ntrip_enabled?'Disable':'Enable')+"</button>");
  const hp=Math.round(d.free_heap*100/d.heap_total),hc=hp>=40?'ok':hp>=20?'warn':'err';
  set('st-heap',"<span class='"+hc+"'>"+bytes(d.free_heap)+" ("+hp+"% free)</span> <span class='dim'>| low watermark "+bytes(d.min_free_heap)+"</span>");
  if(!d.sd_mounted){set('st-sd',"<span class='err'>not mounted</span>");}
  else if(!d.sd_total){set('st-sd',"<span class='ok'>mounted</span>");}
  else{const sp=Math.round(d.sd_used*100/d.sd_total),sc=sp<80?'ok':sp<95?'warn':'err';set('st-sd',"<span class='"+sc+"'>"+bytes(d.sd_used)+" used of "+bytes(d.sd_total)+" ("+sp+"%)</span>");}
  if(!d.rinex_active){set('st-rinex',"<span class='dim'>inactive</span> &nbsp; <button onclick=\"toggleRinex(true)\">Start</button>");}
  else{const fn=d.rinex_file?d.rinex_file.split('/').pop():'';set('st-rinex',"<span class='ok'>active</span> <span class='dim'>| epochs: "+d.rinex_epochs+(fn?" | "+fn:"")+"</span> &nbsp; <button onclick=\"toggleRinex(false)\">Stop</button>");}
 }catch(e){}finally{statusRequest=false;setTimeout(refresh,15000);}
}
async function toggleRinex(start){
 try{await fetch('/rinex/toggle',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'start='+(start?'1':'0')});}catch(e){}
 setTimeout(refresh,500);
}
async function toggleNtrip(on){
 try{await fetch('/ntrip/toggle',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'on='+(on?'1':'0')});}catch(e){}
 setTimeout(refresh,500);
}
setTimeout(refresh,15000);
</script>)HTML";
    return server->send_page(request, "GPS Base Station", content);
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
    const BasePosition position = server->storage_->load_position();
    const WifiCredentials wifi = server->storage_->load_wifi();
    const ServiceCredentials r2g = server->storage_->load_service("rtk2go");
    const ServiceCredentials onc = server->storage_->load_service("onocoy");
    const ServiceCredentials rtk = server->storage_->load_service("rtkdata");
    auto checked = [server](const char *name) {
        return server->storage_->service_enabled(name) ? " checked" : "";
    };
    char lat[32]{}, lon[32]{}, height[32]{};
    if (position.valid) {
        snprintf(lat, sizeof(lat), "%.8f", position.lat);
        snprintf(lon, sizeof(lon), "%.8f", position.lon);
        snprintf(height, sizeof(height), "%.4f", position.height);
    }
    const std::string content =
        "<h2>NTRIP Services</h2><form method='post' action='/config'>"
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
        "<button>Save Services</button></form>"
        "<h2>Manual Position</h2>"
        "<form method='post' action='/config/position'>"
        "<p><input name='lat' placeholder='Latitude' value='" +
        std::string(lat) + "'></p>"
        "<p><input name='lon' placeholder='Longitude' value='" +
        std::string(lon) + "'></p>"
        "<p><input name='hgt' placeholder='Ellipsoidal height (m)' value='" +
        std::string(height) + "'></p>"
        "<button>Save Position</button></form>"
        "<h2>Antenna (RINEX)</h2>"
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
        "<button>Save Antenna</button></form>"
        "<h2>Survey</h2><form method='post' action='/survey'>"
        "<button>Start New Survey</button></form>"
        "<h2>WiFi</h2><form method='post' action='/config/wifi'>"
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
        "</script>"
        "<h2>Access Point (Hotspot)</h2>"
        "<form method='post' action='/config/ap'>"
        "<p class='dim'>SSID: <b>" + html_escape(config::kAccessPointSsid) +
        "</b> (WPA2). Set the password used to join the device's own hotspot.</p>"
        "<p><input name='ap_pw' type='password' minlength='8' maxlength='63' "
        "placeholder='New hotspot password (8-63 chars)' required></p>"
        "<button>Save Hotspot Password</button></form>"
        "<p><a href='/logs'>Console logs</a> &nbsp; <a href='/'>Back</a></p>";
    return server->send_page(request, "Configuration", content);
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
    return httpd_resp_send(request, body.c_str(), body.size());
}

esp_err_t AdminWebServer::skyplot_handler(httpd_req_t *request) {
    AdminWebServer *server = self(request);
    if (!server->authorize(request)) return server->send_unauthorized(request);
    const std::string content = R"HTML(
<h2>Satellite Sky Plot</h2>
<canvas id='sky' width='640' height='640'
 style='width:100%;max-width:640px;background:#080b08'></canvas>
<p id='count'></p><p><a href='/'>Back</a></p>
<script>
const c=document.getElementById('sky'),x=c.getContext('2d'),C=320,R=280;
const colors=['','#3af','#f86','#5d5','#fd5'];
function draw(a){
 x.clearRect(0,0,640,640);x.strokeStyle='#496';x.fillStyle='#9b9';
 x.textAlign='center';x.font='14px monospace';
 [0,30,60].forEach(e=>{let r=R*(90-e)/90;x.beginPath();x.arc(C,C,r,0,7);x.stroke()});
 x.fillText('N',C,24);x.fillText('S',C,630);x.fillText('E',620,C);x.fillText('W',20,C);
 a.forEach(s=>{let r=R*(90-s.el)/90,q=s.az*Math.PI/180;
  let px=C+r*Math.sin(q),py=C-r*Math.cos(q);
  x.fillStyle=colors[s.sys]||'#fff';x.beginPath();x.arc(px,py,8,0,7);x.fill();
  x.fillText(s.prn,px,py-12);
 });document.getElementById('count').textContent=a.length+' satellites in view';
}
async function update(){try{draw(await (await fetch('/skyplot/data')).json())}catch(e){}}
update();setInterval(update,10000);
</script>)HTML";
    return server->send_page(request, "Satellite Sky Plot", content);
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
    <div style='background:#222;border-radius:3px;height:8px;overflow:hidden'>
      <div id='otaBar' style='background:#0f0;height:100%;width:0%;transition:width 0.3s'></div>
    </div>
    <p id='otaStatus' style='margin:4px 0 0;font-size:0.85em;opacity:.8'>Uploading...</p>
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
      bar.style.background='#4f4';
      status.textContent=xhr.responseText;
      setTimeout(function(){window.location.href='/';},9000);
    }else{
      bar.style.background='#f44';
      status.textContent='Failed: '+xhr.responseText;
    }
  };
  xhr.onerror=function(){
    bar.style.background='#f44';
    status.textContent='Upload error — check connection.';
  };
  xhr.send(file);
}
</script>
<p><a href='/'>Back</a></p>)HTML";
    return server->send_page(request, "Firmware Update", content);
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
    httpd_resp_set_hdr(request, "Location", "/");
    return httpd_resp_send(request, nullptr, 0);
}

esp_err_t AdminWebServer::survey_handler(httpd_req_t *request) {
    AdminWebServer *server = self(request);
    if (!server->authorize(request)) return server->send_unauthorized(request);
    ESP_RETURN_ON_ERROR(
        server->station_->request_survey(), kTag, "Survey request failed");
    httpd_resp_set_status(request, "303 See Other");
    httpd_resp_set_hdr(request, "Location", "/");
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
    httpd_resp_set_hdr(request, "Location", "/config");
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
    httpd_resp_set_hdr(request, "Location", "/config");
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
<pre id='log' style='background:#080b08;color:#cde;padding:10px;border-radius:6px;
 max-height:70vh;overflow:auto;white-space:pre-wrap;word-break:break-word;
 font:12px/1.4 monospace'>loading…</pre>
<p><a href='/config'>Configuration</a> &nbsp; <a href='/'>Back</a></p>
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
    return server->send_page(request, "Console Logs", content);
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

esp_err_t AdminWebServer::send_page(
    httpd_req_t *request, const char *title,
    const std::string &content) const {
    static constexpr std::string_view kPrefix =
        "<!doctype html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<link rel='icon' href='data:,'><title>";
    static constexpr std::string_view kAfterTitle =
        "</title><style>"
        "body{font-family:monospace;background:#111;color:#cfc;padding:1em;"
        "max-width:720px;margin:auto}h1,h2{color:#0f0}"
        "table{border-collapse:collapse;width:100%}td{border:1px solid #333;"
        "padding:6px}input{width:100%;max-width:420px;padding:7px;"
        "background:#1a1a1a;color:#cfc;border:1px solid #444;"
        "box-sizing:border-box}"
        "button{padding:8px 16px;background:#1a1a1a;color:#0f0;"
        "border:1px solid #0f0}.ok{color:#0f0}.warn{color:#fa0}"
        ".err{color:#f44}.dim{opacity:.6}a{color:#0d0}"
        "</style></head><body><h1>GPS Base Station</h1>";
    static constexpr std::string_view kSuffix = "</body></html>";

    httpd_resp_set_type(request, "text/html");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_hdr(request, "Strict-Transport-Security", "max-age=3600");
    esp_err_t result = send_chunks(request, kPrefix);
    if (result == ESP_OK) result = send_chunks(request, title);
    if (result == ESP_OK) result = send_chunks(request, kAfterTitle);
    if (result == ESP_OK) result = send_chunks(request, content);
    if (result == ESP_OK) result = send_chunks(request, kSuffix);
    if (result == ESP_OK) result = httpd_resp_send_chunk(request, nullptr, 0);
    return result;
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
            "<div style='margin-bottom:12px'>"
            "<div style='color:#888;font-size:0.85em;margin-bottom:4px'>"
            "Storage: <span class='%s'>%s used of %s (%u%%)</span></div>"
            "<div style='background:#222;border-radius:3px;height:8px;width:100%%'>"
            "<div style='background:%s;border-radius:3px;height:8px;width:%u%%'></div>"
            "</div></div>",
            bar_class,
            human_bytes(ds.used_bytes).c_str(),
            human_bytes(ds.total_bytes).c_str(),
            sd_pct,
            sd_pct < 80 ? "#0f0" : (sd_pct < 95 ? "#fa0" : "#f44"),
            sd_pct);
        storage_block = bar_html;
    } else {
        storage_block = "<p style='color:#888;font-size:0.85em'>Storage: " +
            std::string(mounted ? "calculating…" : "<span class='err'>not mounted</span>") +
            "</p>";
    }

    std::string content =
        "<p><a href='/' style='color:#0d0'>&larr; Status</a> &nbsp; "
        "<a href='/rinex/export' style='color:#08f'>RINEX export &rarr;</a></p>"
        "<h2>SD Card Files</h2>" +
        storage_block +
        "<div style='display:flex;align-items:center;gap:8px;margin-bottom:8px;flex-wrap:wrap'>"
        "<div id='breadcrumb' style='color:#888;word-break:break-all;flex:1'>/</div>"
        "<button id='mkdir-btn' onclick='mkdirPrompt()' style='padding:3px 10px;background:#1a1a1a;color:#0f0;border:1px solid #0f0;cursor:pointer;white-space:nowrap'>+ New Folder</button>"
        "</div>"
        "<div style='overflow-x:auto'>"
        "<table><thead><tr>"
        "<th style='text-align:left;padding:4px 8px'>Name</th>"
        "<th style='text-align:right;padding:4px 8px'>Size</th>"
        "<th style='text-align:right;padding:4px 8px'>Actions</th>"
        "</tr></thead>"
        "<tbody id='file-list'>"
        "<tr><td colspan='3' style='padding:10px 8px;color:#888'>Loading&hellip;</td></tr>"
        "</tbody></table></div>"
        "<script>var SD_ROOT='" + mount + "';</script>"
        R"JSEOF(
<script>
var filePath=SD_ROOT;
var fileEntries=[];
function esc(s){return String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/"/g,'&quot;');}
function fmtSize(b){if(b===0)return'<span style="color:#888">Folder</span>';if(b>=1048576)return(b/1048576).toFixed(1)+' MB';if(b>=1024)return(b/1024).toFixed(1)+' KB';return b+' B';}
function breadcrumb(path){
  var parts=path.split('/').filter(function(p){return p.length>0;});
  var html='<span style="color:#555">/ </span>';var built='';
  parts.forEach(function(p,i){built+='/'+p;var b=built;
    if(i<parts.length-1)html+='<a href="#" onclick="loadDir(\''+esc(b)+'\');return false" style="color:#0d0">'+esc(p)+'</a><span style="color:#555"> / </span>';
    else html+='<span>'+esc(p)+'</span>';
  });
  document.getElementById('breadcrumb').innerHTML=html;
}
function loadDir(path){
  filePath=path;breadcrumb(path);
  var tb=document.getElementById('file-list');
  tb.innerHTML='<tr><td colspan="3" style="padding:10px 8px;color:#888">Loading&hellip;</td></tr>';
  fetch('/files/list?path='+encodeURIComponent(path))
    .then(function(r){return r.json();}).then(function(entries){
      fileEntries=entries.sort(function(a,b){
        if(a.is_dir!==b.is_dir)return a.is_dir?-1:1;
        return a.name<b.name?-1:a.name>b.name?1:0;
      });
      var rows='';
      if(path!==SD_ROOT){
        var parent=path.substring(0,path.lastIndexOf('/'))||SD_ROOT;
        rows+='<tr><td colspan="3" style="padding:5px 8px"><a href="#" onclick="loadDir(\''+esc(parent)+'\');return false" style="color:#0d0">&#8679; ..</a></td></tr>';
      }
      if(!fileEntries.length){rows+='<tr><td colspan="3" style="padding:10px 8px;color:#888">Empty directory</td></tr>';}
      fileEntries.forEach(function(e,i){
        var nameCell=e.is_dir
          ?'<a href="#" onclick="loadDir(fileEntries['+i+'].path);return false" style="color:#0d0">[dir] '+esc(e.name)+'</a>'
          :'[file] '+esc(e.name);
        var acts='';
        if(!e.is_dir)acts+='<button onclick="dlFile('+i+')" title="Download" style="margin:1px;padding:2px 6px;background:#1a1a1a;color:#0f0;border:1px solid #0f0;cursor:pointer">dl</button>';
        acts+='<button onclick="renameEntry('+i+')" title="Rename" style="margin:1px;padding:2px 6px;background:#1a1a1a;color:#fa0;border:1px solid #fa0;cursor:pointer">rn</button>';
        acts+='<button onclick="delEntry('+i+')" title="Delete" style="margin:1px;padding:2px 6px;background:#1a1a1a;color:#f44;border:1px solid #f44;cursor:pointer">del</button>';
        rows+='<tr style="border-bottom:1px solid #222">'
          +'<td style="padding:6px 8px">'+nameCell+'</td>'
          +'<td style="padding:6px 8px;text-align:right;color:#888">'+fmtSize(e.size)+'</td>'
          +'<td style="padding:6px 4px;text-align:right;white-space:nowrap">'+acts+'</td></tr>';
      });
      tb.innerHTML=rows;
    }).catch(function(){
      document.getElementById('file-list').innerHTML='<tr><td colspan="3" style="padding:10px 8px;color:#f44">Failed to load directory</td></tr>';
    });
}
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
loadDir(SD_ROOT);
</script>
)JSEOF";

    return server->send_page(request, "SD Card Files", content);
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
        ? "<p style='color:#f84'>No RINEX files found in /rawdata.</p>"
        : "";

    httpd_resp_set_type(request, "text/html");
    const std::string page =
        "<!DOCTYPE html><html><head>"
        "<meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>RINEX Export</title>"
        "<style>body{background:#0d0d0d;color:#e0e0e0;font-family:monospace;max-width:560px;margin:0 auto;padding:16px}"
        "h2{color:#0f0}input{background:#1a1a1a;color:#e0e0e0;border:1px solid #444;padding:5px 8px;width:100%;box-sizing:border-box;font-family:monospace}"
        "label{display:block;margin:10px 0 4px;color:#888}"
        "button{margin-top:16px;padding:8px 20px;background:#1a1a1a;color:#08f;border:1px solid #08f;cursor:pointer;font-family:monospace;font-size:1em}"
        "button:disabled{color:#444;border-color:#444;cursor:default}"
        "#status{margin-top:12px;font-size:0.85em;min-height:1.5em}"
        "</style></head><body>"
        "<p><a href='/files' style='color:#0d0'>&larr; SD Files</a> &nbsp; "
        "<a href='/' style='color:#0d0'>&larr; Status</a></p>"
        "<h2>RINEX Export</h2>" +
        no_data_note +
        "<p style='color:#888;font-size:0.85em'>Select a UTC time range. All 1-hour RINEX files within that range will be merged into a single download.</p>"
        "<label for='start'>Start (UTC)</label>"
        "<input type='datetime-local' id='start' value='" + std::string(start_val) + "'>"
        "<label for='end'>End (UTC)</label>"
        "<input type='datetime-local' id='end' value='" + std::string(end_val) + "'>"
        "<br><button id='btn' onclick='doExport()'>&#8659; Export &amp; Download</button>"
        "<div id='status'></div>"
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
        "st.style.color='#888';st.textContent='Connecting…';"
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
        "st.style.color='#0f0';"
        "st.textContent='✓ Done — '+fmtSize(blob.size)+' downloaded.';"
        "}catch(err){"
        "clearInterval(dotTimer);"
        "st.style.color='#f44';"
        "st.textContent='✗ '+err.message;"
        "}"
        "btn.disabled=false;btn.textContent='⇓ Export & Download';"
        "}"
        "</script></body></html>";

    return send_chunks(request, page);
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
