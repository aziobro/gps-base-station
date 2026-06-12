#pragma once

#include "esp_err.h"
#include "esp_http_server.h"
#include "storage.hpp"
#include "wifi_manager.hpp"

class BootstrapWebServer {
public:
    esp_err_t start(Storage &storage, WifiManager &wifi);
    void stop();

private:
    Storage *storage_ = nullptr;
    httpd_handle_t https_server_ = nullptr;

    static esp_err_t root_handler(httpd_req_t *request);
    static esp_err_t status_handler(httpd_req_t *request);
    static esp_err_t update_page_handler(httpd_req_t *request);
    static esp_err_t update_upload_handler(httpd_req_t *request);

    bool authorize(httpd_req_t *request) const;
    esp_err_t send_unauthorized(httpd_req_t *request) const;

    static BootstrapWebServer *self(httpd_req_t *request);
};
