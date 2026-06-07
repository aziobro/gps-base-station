#include "web_server.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <vector>

#include "base_station.hpp"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_https_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"

namespace {

constexpr char kTag[] = "web";
constexpr char kAdminUser[] = "admin";

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
        status.connected ? "connected" : escape_html(status.message);
    return "<span class='" + std::string(css) + "'>" + label + "</span>"
           " <span class='dim'>| " + human_bytes(status.bytes_sent) +
           " sent | " + std::to_string(status.dropped_batches) +
           " dropped</span>";
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
    Storage &storage, WifiManager &wifi, BaseStation &station) {
    storage_ = &storage;
    wifi_ = &wifi;
    station_ = &station;

    httpd_ssl_config_t tls_config = HTTPD_SSL_CONFIG_DEFAULT();
    tls_config.httpd.max_uri_handlers = 20;
    tls_config.httpd.stack_size = 12288;
    tls_config.httpd.recv_wait_timeout = 10;
    tls_config.httpd.send_wait_timeout = 10;
    tls_config.httpd.lru_purge_enable = true;
    tls_config.httpd.max_open_sockets = 2;
    tls_config.httpd.keep_alive_enable = true;
    tls_config.httpd.keep_alive_idle = 30;
    tls_config.httpd.keep_alive_interval = 5;
    tls_config.httpd.keep_alive_count = 3;
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
        {"/ca.crt", HTTP_GET, ca_certificate_handler, this},
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

    std::string content =
        "<h2>Status</h2><table>"
        "<tr><td>Framework</td><td>ESP-IDF " + std::string(esp_get_idf_version()) + "</td></tr>"
        "<tr><td>Application</td><td>" + std::string(esp_app_get_description()->version) + "</td></tr>"
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
        "</table><p><a href='/config'>Configuration</a> &nbsp; "
        "<a href='/skyplot'>Sky plot</a> &nbsp; "
        "<a href='/status'>JSON status</a> &nbsp; "
        "<a href='/update'>Firmware update</a></p>"
        R"HTML(<script>
let statusRequest=false;
function esc(v){return String(v).replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));}
function bytes(v){return v>=1048576?(v/1048576).toFixed(1)+' MB':v>=1024?(v/1024).toFixed(1)+' KB':v+' B';}
function set(id,v){const e=document.getElementById(id);if(e)e.innerHTML=v;}
function svc(p){if(!p.enabled)return "<span class='warn'>disabled</span>";const c=p.connected?'ok':'err',m=p.connected?'connected':esc(p.message);return "<span class='"+c+"'>"+m+"</span> <span class='dim'>| "+bytes(p.bytes)+" sent | "+p.dropped+" dropped</span>";}
async function refresh(){
 if(statusRequest)return;statusRequest=true;
 try{
  const r=await fetch('/status',{cache:'no-store'});if(!r.ok)throw Error(r.status);
  const d=await r.json(),total=d.gps+d.glonass+d.galileo+d.beidou;
  set('st-health',"<span class='"+(d.healthy?'ok':'err')+"'>"+(d.healthy?'healthy':'unhealthy')+"</span>");
  set('st-wifi',(d.wifi_connected?'connected':d.ap_active?'AP fallback':'disconnected')+(d.ssid?" <span class='dim'>| "+esc(d.ssid)+"</span>":""));
  set('st-ip',esc(d.ip||'192.168.4.1'));
  const rc=d.rssi>=-70?'ok':d.rssi>=-80?'warn':'err',rh=d.rssi>=-60?'excellent':d.rssi>=-70?'good':d.rssi>=-80?'fair':'weak';
  set('st-rssi',"<span class='"+rc+"'>"+d.rssi+" dBm ("+rh+")</span>");
  set('st-mode',"<span class='"+(d.mode==='base_tx'?'ok':'warn')+"'>"+(d.mode==='base_tx'?'Base TX':'Survey')+"</span>");
  set('st-sats','GPS '+d.gps+' / GLO '+d.glonass+' / GAL '+d.galileo+' / BDS '+d.beidou+" <span class='dim'>| total "+total+"</span>");
  set('st-rtcm',d.rtcm_bps+" B/s <span class='dim'>| "+bytes(d.rtcm_total)+" total</span>");
  set('st-clients',d.local_clients);set('st-r2g',svc(d.rtk2go));set('st-onc',svc(d.onocoy));set('st-rtk',svc(d.rtkdata));
  const hp=Math.round(d.free_heap*100/d.heap_total),hc=hp>=40?'ok':hp>=20?'warn':'err';
  set('st-heap',"<span class='"+hc+"'>"+bytes(d.free_heap)+" ("+hp+"% free)</span> <span class='dim'>| low watermark "+bytes(d.min_free_heap)+"</span>");
 }catch(e){}finally{statusRequest=false;setTimeout(refresh,15000);}
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
        "<p><a href='/'>Back</a></p>";
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
            ",\"dropped\":" + std::to_string(status.dropped_batches) + "}";
    };
    const size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    std::string body =
        "{\"framework\":\"ESP-IDF " + json_escape(esp_get_idf_version()) +
        "\",\"version\":\"" + json_escape(esp_app_get_description()->version) +
        "\",\"healthy\":" + (server->station_->healthy() ? "true" : "false") +
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
        ",\"local_clients\":" + std::to_string(station.local_clients) +
        ",\"rtk2go\":" + provider_json(station.rtk2go) +
        ",\"onocoy\":" + provider_json(station.onocoy) +
        ",\"rtkdata\":" + provider_json(station.rtkdata) +
        ",\"position_valid\":" + (position.valid ? "true" : "false") + "}";
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, body.c_str(), body.size());
}

esp_err_t AdminWebServer::update_page_handler(httpd_req_t *request) {
    AdminWebServer *server = self(request);
    if (!server->authorize(request)) return server->send_unauthorized(request);

    const std::string content = R"HTML(
<h2>Firmware Update</h2>
<p>Select the ESP-IDF application image. The upload is sent as raw binary and
validated before the boot partition changes.</p>
<input id='firmware' type='file' accept='.bin'>
<button id='upload'>Upload &amp; Restart</button>
<pre id='result'></pre>
<script>
document.getElementById('upload').onclick=async function(){
  const f=document.getElementById('firmware').files[0];
  if(!f)return;
  const out=document.getElementById('result');
  out.textContent='Uploading...';
  try{
    const r=await fetch('/update',{method:'POST',headers:{'Content-Type':'application/octet-stream'},body:f});
    out.textContent=await r.text();
  }catch(e){out.textContent='Upload failed: '+e;}
};
</script><p><a href='/'>Back</a></p>)HTML";
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
    esp_err_t result = esp_ota_begin(
        partition, static_cast<size_t>(request->content_len), &handle);
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
