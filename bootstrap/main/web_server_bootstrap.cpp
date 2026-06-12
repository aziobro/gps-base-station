#include "web_server_bootstrap.hpp"

#include <algorithm>
#include <string>
#include <vector>

#include "esp_check.h"
#include "esp_https_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"

namespace {

constexpr char kTag[] = "bootstrap_web";
constexpr char kAdminUser[] = "admin";

extern const uint8_t server_cert_start[] asm("_binary_server_cert_pem_start");
extern const uint8_t server_cert_end[]   asm("_binary_server_cert_pem_end");
extern const uint8_t server_key_start[]  asm("_binary_server_key_pem_start");
extern const uint8_t server_key_end[]    asm("_binary_server_key_pem_end");

// Last OTA result stored here so /status can report it even if the response
// could not be sent over the (now-closed) upload connection.
static esp_err_t s_last_ota_result = ESP_OK;
static char s_last_ota_stage[32] = "none";

void restart_task(void *) {
    vTaskDelay(pdMS_TO_TICKS(3000));
    esp_restart();
    vTaskDelete(nullptr);
}

}  // namespace

esp_err_t BootstrapWebServer::start(Storage &storage, WifiManager &wifi) {
    storage_ = &storage;

    httpd_ssl_config_t tls_config = HTTPD_SSL_CONFIG_DEFAULT();
    tls_config.httpd.max_uri_handlers = 5;
    tls_config.httpd.stack_size = 12288;
    tls_config.httpd.recv_wait_timeout = 30;
    tls_config.httpd.send_wait_timeout = 30;
    tls_config.httpd.lru_purge_enable = true;
    tls_config.httpd.max_open_sockets = 4;
    tls_config.servercert = server_cert_start;
    tls_config.servercert_len = server_cert_end - server_cert_start;
    tls_config.prvtkey_pem = server_key_start;
    tls_config.prvtkey_len = server_key_end - server_key_start;

    ESP_RETURN_ON_ERROR(
        httpd_ssl_start(&https_server_, &tls_config),
        kTag, "HTTPS server start failed");

    const httpd_uri_t routes[] = {
        {"/",       HTTP_GET,  root_handler,          this},
        {"/status", HTTP_GET,  status_handler,        this},
        {"/update", HTTP_GET,  update_page_handler,   this},
        {"/update", HTTP_POST, update_upload_handler, this},
    };
    for (const auto &r : routes) {
        ESP_RETURN_ON_ERROR(
            httpd_register_uri_handler(https_server_, &r),
            kTag, "URI registration failed: %s", r.uri);
    }
    ESP_LOGI(kTag, "Bootstrap HTTPS server ready");
    return ESP_OK;
}

void BootstrapWebServer::stop() {
    if (https_server_) {
        httpd_ssl_stop(https_server_);
        https_server_ = nullptr;
    }
}

esp_err_t BootstrapWebServer::root_handler(httpd_req_t *request) {
    BootstrapWebServer *server = self(request);
    if (!server->authorize(request)) return server->send_unauthorized(request);

    static constexpr char kPage[] =
        "<!doctype html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>GPS Bootstrap</title>"
        "<style>body{font-family:monospace;background:#111;color:#cfc;padding:1em;"
        "max-width:600px;margin:auto}h1{color:#0f0}a{color:#0d0}</style>"
        "</head><body>"
        "<h1>GPS Bootstrap</h1>"
        "<p>Bootstrap firmware — use this to upload the full firmware.</p>"
        "<p><a href='/update'>Upload &amp; Flash Firmware</a></p>"
        "</body></html>";

    httpd_resp_set_type(request, "text/html");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_sendstr(request, kPage);
}

esp_err_t BootstrapWebServer::status_handler(httpd_req_t *request) {
    BootstrapWebServer *server = self(request);
    if (!server->authorize(request)) return server->send_unauthorized(request);

    auto state_str = [](esp_ota_img_states_t s) -> const char * {
        switch (s) {
            case ESP_OTA_IMG_NEW:             return "NEW";
            case ESP_OTA_IMG_PENDING_VERIFY:  return "PENDING_VERIFY";
            case ESP_OTA_IMG_VALID:           return "VALID";
            case ESP_OTA_IMG_INVALID:         return "INVALID";
            case ESP_OTA_IMG_ABORTED:         return "ABORTED";
            default:                          return "UNDEFINED";
        }
    };

    std::string body = "{\"partitions\":[";
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *boot    = esp_ota_get_boot_partition();
    const esp_partition_t *next    = esp_ota_get_next_update_partition(nullptr);

    esp_partition_iterator_t it = esp_partition_find(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, nullptr);
    bool first = true;
    while (it) {
        const esp_partition_t *p = esp_partition_get(it);
        esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
        esp_ota_get_state_partition(p, &state);
        if (!first) body += ",";
        first = false;
        body += "{\"label\":\"";
        body += p->label;
        body += "\",\"state\":\"";
        body += state_str(state);
        body += "\",\"running\":";
        body += (p == running ? "true" : "false");
        body += ",\"boot\":";
        body += (p == boot ? "true" : "false");
        body += ",\"next\":";
        body += (p == next ? "true" : "false");
        body += "}";
        it = esp_partition_next(it);
    }
    esp_partition_iterator_release(it);
    body += "],\"last_ota_stage\":\"";
    body += s_last_ota_stage;
    body += "\",\"last_ota_result\":\"";
    body += esp_err_to_name(s_last_ota_result);
    body += "\"}";

    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_sendstr(request, body.c_str());
}

esp_err_t BootstrapWebServer::update_page_handler(httpd_req_t *request) {
    BootstrapWebServer *server = self(request);
    if (!server->authorize(request)) return server->send_unauthorized(request);

    static constexpr char kPage[] =
        "<!doctype html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>Firmware Update</title>"
        "<style>body{font-family:monospace;background:#111;color:#cfc;padding:1em;"
        "max-width:600px;margin:auto}h1,h2{color:#0f0}a{color:#0d0}"
        "button{padding:8px 16px;background:#1a1a1a;color:#0f0;border:1px solid #0f0}"
        "</style></head><body>"
        "<h1>GPS Bootstrap</h1>"
        "<h2>Firmware Update</h2>"
        "<p>Select the ESP-IDF application image (.bin). The device restarts after flashing.</p>"
        "<form id='otaForm'>"
        "  <input type='file' id='otaFile' accept='.bin' style='display:none'"
        "    onchange='document.getElementById(\"otaName\").textContent=this.files[0].name'>"
        "  <div style='display:flex;gap:8px;align-items:center;flex-wrap:wrap'>"
        "    <button type='button' onclick='document.getElementById(\"otaFile\").click()'>Choose .bin</button>"
        "    <span id='otaName' style='opacity:.6;font-size:0.85em'>No file chosen</span>"
        "  </div>"
        "  <div id='otaProgress' style='display:none;margin-top:12px'>"
        "    <div style='background:#222;border-radius:3px;height:8px;overflow:hidden'>"
        "      <div id='otaBar' style='background:#0f0;height:100%;width:0%;transition:width 0.3s'></div>"
        "    </div>"
        "    <p id='otaStatus' style='margin:4px 0 0;font-size:0.85em;opacity:.8'>Uploading...</p>"
        "  </div>"
        "  <button type='submit' style='margin-top:12px' onclick='startOTA(event)'>Upload &amp; Flash</button>"
        "</form>"
        "<script>"
        "function startOTA(e){"
        "  e.preventDefault();"
        "  var file=document.getElementById('otaFile').files[0];"
        "  if(!file){alert('Choose a .bin file first.');return;}"
        "  if(!confirm('Flash '+file.name+' ('+(file.size/1024).toFixed(1)+' KB)?\\nThe device will restart after flashing.'))return;"
        "  var progress=document.getElementById('otaProgress');"
        "  var bar=document.getElementById('otaBar');"
        "  var status=document.getElementById('otaStatus');"
        "  progress.style.display='block';"
        "  var xhr=new XMLHttpRequest();"
        "  xhr.open('POST','/update');"
        "  xhr.setRequestHeader('Content-Type','application/octet-stream');"
        "  xhr.upload.onprogress=function(e){"
        "    if(e.lengthComputable){"
        "      var pct=Math.round(e.loaded/e.total*100);"
        "      bar.style.width=pct+'%';"
        "      status.textContent='Uploading... '+pct+'%';"
        "    }"
        "  };"
        "  xhr.onload=function(){"
        "    bar.style.width='100%';"
        "    if(xhr.status===200){"
        "      bar.style.background='#4f4';"
        "      status.textContent=xhr.responseText;"
        "      setTimeout(function(){window.location.href='/';},9000);"
        "    }else{"
        "      bar.style.background='#f44';"
        "      status.textContent='Failed: '+xhr.responseText;"
        "    }"
        "  };"
        "  xhr.onerror=function(){"
        "    bar.style.background='#f44';"
        "    status.textContent='Upload error — check connection.';"
        "  };"
        "  xhr.send(file);"
        "}"
        "</script>"
        "<p><a href='/'>Back</a></p>"
        "</body></html>";

    httpd_resp_set_type(request, "text/html");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_sendstr(request, kPage);
}

esp_err_t BootstrapWebServer::update_upload_handler(httpd_req_t *request) {
    BootstrapWebServer *server = self(request);
    if (!server->authorize(request)) return server->send_unauthorized(request);

    if (request->content_len <= 0) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Empty image");
    }

    const esp_partition_t *partition = esp_ota_get_next_update_partition(nullptr);
    if (!partition || static_cast<size_t>(request->content_len) > partition->size) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Image too large");
    }

    esp_ota_handle_t handle = 0;
    // OTA_WITH_SEQUENTIAL_WRITES erases one sector at a time as each boundary is
    // crossed rather than erasing the whole partition upfront, preventing the TLS
    // session from timing out during a large synchronous erase.
    snprintf(s_last_ota_stage, sizeof(s_last_ota_stage), "begin");
    esp_err_t result = esp_ota_begin(partition, OTA_WITH_SEQUENTIAL_WRITES, &handle);
    if (result != ESP_OK) {
        s_last_ota_result = result;
        snprintf(s_last_ota_stage, sizeof(s_last_ota_stage), "begin_fail");
        return httpd_resp_send_err(
            request, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(result));
    }

    std::vector<uint8_t> buffer(4096);
    size_t remaining = request->content_len;
    int consecutive_timeouts = 0;
    size_t written = 0;
    snprintf(s_last_ota_stage, sizeof(s_last_ota_stage), "writing");
    while (remaining > 0) {
        int received = httpd_req_recv(
            request, reinterpret_cast<char *>(buffer.data()),
            std::min(buffer.size(), remaining));
        if (received == HTTPD_SOCK_ERR_TIMEOUT && consecutive_timeouts++ < 20) {
            continue;
        }
        if (received <= 0) {
            s_last_ota_result = ESP_ERR_TIMEOUT;
            snprintf(s_last_ota_stage, sizeof(s_last_ota_stage), "recv_fail@%zuB", written);
            esp_ota_abort(handle);
            return httpd_resp_send_err(
                request, HTTPD_500_INTERNAL_SERVER_ERROR, "Upload interrupted");
        }
        consecutive_timeouts = 0;
        result = esp_ota_write(handle, buffer.data(), received);
        if (result != ESP_OK) {
            s_last_ota_result = result;
            snprintf(s_last_ota_stage, sizeof(s_last_ota_stage), "write_fail@%zuB", written);
            esp_ota_abort(handle);
            return httpd_resp_send_err(
                request, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(result));
        }
        written += received;
        remaining -= received;
    }

    snprintf(s_last_ota_stage, sizeof(s_last_ota_stage), "end(%zuB)", written);
    result = esp_ota_end(handle);
    snprintf(s_last_ota_stage, sizeof(s_last_ota_stage), "end_done(%zuB)", written);
    s_last_ota_result = result;
    if (result == ESP_OK) {
        snprintf(s_last_ota_stage, sizeof(s_last_ota_stage), "set_boot");
        result = esp_ota_set_boot_partition(partition);
        s_last_ota_result = result;
    }
    if (result != ESP_OK) {
        snprintf(s_last_ota_stage, sizeof(s_last_ota_stage), "failed(%s)", esp_err_to_name(result));
        return httpd_resp_send_err(
            request, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(result));
    }

    snprintf(s_last_ota_stage, sizeof(s_last_ota_stage), "success");
    httpd_resp_set_hdr(request, "Connection", "close");
    const esp_err_t resp = httpd_resp_sendstr(request, "Update accepted. Restarting.");
    if (resp != ESP_OK) {
        ESP_LOGW(kTag, "OTA response flush: %s", esp_err_to_name(resp));
    }
    xTaskCreate(restart_task, "ota_restart", 2048, nullptr, 3, nullptr);
    return ESP_OK;
}

bool BootstrapWebServer::authorize(httpd_req_t *request) const {
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

esp_err_t BootstrapWebServer::send_unauthorized(httpd_req_t *request) const {
    httpd_resp_set_status(request, "401 Unauthorized");
    httpd_resp_set_hdr(
        request, "WWW-Authenticate", "Basic realm=\"GPS Bootstrap\"");
    httpd_resp_set_hdr(request, "Connection", "close");
    return httpd_resp_sendstr(request, "Authentication required");
}

BootstrapWebServer *BootstrapWebServer::self(httpd_req_t *request) {
    return static_cast<BootstrapWebServer *>(request->user_ctx);
}
