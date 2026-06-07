#include "web_server.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "base_station.hpp"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"

namespace {

constexpr char kTag[] = "web";
constexpr char kAdminUser[] = "admin";

std::string page(const char *title, const std::string &content) {
    return "<!doctype html><html><head><meta charset='utf-8'>"
           "<meta name='viewport' content='width=device-width,initial-scale=1'>"
           "<title>" + std::string(title) + "</title><style>"
           "body{font-family:monospace;background:#111;color:#cfc;padding:1em;"
           "max-width:720px;margin:auto}h1,h2{color:#0f0}"
           "table{border-collapse:collapse;width:100%}td{border:1px solid #333;"
           "padding:6px}input{width:100%;max-width:420px;padding:7px;"
           "background:#1a1a1a;color:#cfc;border:1px solid #444;box-sizing:border-box}"
           "button{padding:8px 16px;background:#1a1a1a;color:#0f0;"
           "border:1px solid #0f0}.warn{color:#fa0}.err{color:#f44}"
           "a{color:#0d0}</style></head><body><h1>GPS Base Station</h1>" +
           content + "</body></html>";
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
    vTaskDelay(pdMS_TO_TICKS(750));
    esp_restart();
}

}  // namespace

esp_err_t AdminWebServer::start(
    Storage &storage, WifiManager &wifi, BaseStation &station) {
    storage_ = &storage;
    wifi_ = &wifi;
    station_ = &station;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 20;
    config.stack_size = 8192;
    config.recv_wait_timeout = 10;
    config.send_wait_timeout = 10;
    ESP_RETURN_ON_ERROR(httpd_start(&server_, &config), kTag, "HTTP start failed");

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
    };
    for (const auto &handler : handlers) {
        ESP_RETURN_ON_ERROR(
            httpd_register_uri_handler(server_, &handler),
            kTag, "URI registration failed");
    }

    ESP_LOGI(kTag, "Native administration server listening on port 80");
    return ESP_OK;
}

void AdminWebServer::stop() {
    if (!server_) return;
    httpd_stop(server_);
    server_ = nullptr;
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

    std::string content =
        "<h2>ESP-IDF Migration Status</h2><table>"
        "<tr><td>Framework</td><td>ESP-IDF " + std::string(esp_get_idf_version()) + "</td></tr>"
        "<tr><td>Application</td><td>" + std::string(esp_app_get_description()->version) + "</td></tr>"
        "<tr><td>WiFi</td><td>" + wifi_state + "</td></tr>"
        "<tr><td>IP</td><td>" + html_escape(ip.empty() ? "192.168.4.1" : ip) + "</td></tr>"
        "<tr><td>RSSI</td><td>" + std::to_string(server->wifi_->rssi()) + " dBm</td></tr>"
        "<tr><td>Mode</td><td>" +
        std::string(station.mode == BaseMode::kTransmit ? "Base TX" : "Survey") +
        "</td></tr>"
        "<tr><td>Position</td><td>" + std::string(position_row) + "</td></tr>"
        "<tr><td>Satellites</td><td>GPS " +
        std::to_string(station.survey.gps) + " / GLO " +
        std::to_string(station.survey.glonass) + " / GAL " +
        std::to_string(station.survey.galileo) + " / BDS " +
        std::to_string(station.survey.beidou) + "</td></tr>"
        "<tr><td>RTCM</td><td>" +
        std::to_string(station.rtcm_bytes_per_second) + " B/s</td></tr>"
        "<tr><td>RTK2go</td><td>" +
        html_escape(station.rtk2go.message) + " / " +
        std::to_string(station.rtk2go.bytes_sent) + " bytes</td></tr>"
        "<tr><td>Onocoy</td><td>" +
        html_escape(station.onocoy.message) + " / " +
        std::to_string(station.onocoy.bytes_sent) + " bytes</td></tr>"
        "<tr><td>RTKdata</td><td>" +
        html_escape(station.rtkdata.message) + " / " +
        std::to_string(station.rtkdata.bytes_sent) + " bytes</td></tr>"
        "<tr><td>Free heap</td><td>" +
        std::to_string(heap_caps_get_free_size(MALLOC_CAP_8BIT)) + " bytes</td></tr>"
        "</table><p><a href='/config'>Configuration</a> &nbsp; "
        "<a href='/skyplot'>Sky plot</a> &nbsp; "
        "<a href='/status'>JSON status</a> &nbsp; "
        "<a href='/update'>Firmware update</a></p>";
    return server->send_html(request, page("GPS Base Station", content));
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
    return server->send_html(request, page("Setup", content));
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
    return server->send_html(request, page("Configuration", content));
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
    return server->send_html(request, page("Satellite Sky Plot", content));
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
    std::string body =
        "{\"framework\":\"ESP-IDF " + json_escape(esp_get_idf_version()) +
        "\",\"version\":\"" + json_escape(esp_app_get_description()->version) +
        "\",\"wifi_connected\":" + (server->wifi_->connected() ? "true" : "false") +
        ",\"ap_active\":" + (server->wifi_->access_point_active() ? "true" : "false") +
        ",\"ip\":\"" + json_escape(server->wifi_->ip_address()) +
        "\",\"rssi\":" + std::to_string(server->wifi_->rssi()) +
        ",\"free_heap\":" +
        std::to_string(heap_caps_get_free_size(MALLOC_CAP_8BIT)) +
        ",\"mode\":\"" +
        std::string(station.mode == BaseMode::kTransmit ? "base_tx" : "survey") +
        "\",\"rtcm_bps\":" +
        std::to_string(station.rtcm_bytes_per_second) +
        ",\"rtcm_total\":" + std::to_string(station.rtcm_bytes_total) +
        ",\"gps\":" + std::to_string(station.survey.gps) +
        ",\"glonass\":" + std::to_string(station.survey.glonass) +
        ",\"galileo\":" + std::to_string(station.survey.galileo) +
        ",\"beidou\":" + std::to_string(station.survey.beidou) +
        ",\"rtk2go_bytes\":" + std::to_string(station.rtk2go.bytes_sent) +
        ",\"onocoy_bytes\":" + std::to_string(station.onocoy.bytes_sent) +
        ",\"rtkdata_bytes\":" + std::to_string(station.rtkdata.bytes_sent) +
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
    return server->send_html(request, page("Firmware Update", content));
}

esp_err_t AdminWebServer::update_upload_handler(httpd_req_t *request) {
    AdminWebServer *server = self(request);
    if (!server->authorize(request)) return server->send_unauthorized(request);
    server->station_->set_streams_suspended(true);
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

    httpd_resp_sendstr(request, "Update accepted. Restarting.");
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
    return httpd_resp_sendstr(request, "Authentication required");
}

esp_err_t AdminWebServer::send_html(
    httpd_req_t *request, const std::string &body) const {
    httpd_resp_set_type(request, "text/html");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, body.c_str(), body.size());
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
    while (offset < body.size()) {
        int result = httpd_req_recv(
            request, body.data() + offset, body.size() - offset);
        if (result <= 0) return {};
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
