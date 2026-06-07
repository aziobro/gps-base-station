#pragma once

#include <string>

#include "esp_err.h"
#include "esp_http_server.h"

#include "storage.hpp"
#include "wifi_manager.hpp"

class BaseStation;

class AdminWebServer {
public:
    esp_err_t start(Storage &storage, WifiManager &wifi, BaseStation &station);
    void stop();

private:
    Storage *storage_ = nullptr;
    WifiManager *wifi_ = nullptr;
    BaseStation *station_ = nullptr;
    httpd_handle_t server_ = nullptr;

    static esp_err_t root_handler(httpd_req_t *request);
    static esp_err_t setup_get_handler(httpd_req_t *request);
    static esp_err_t setup_post_handler(httpd_req_t *request);
    static esp_err_t config_get_handler(httpd_req_t *request);
    static esp_err_t config_post_handler(httpd_req_t *request);
    static esp_err_t wifi_scan_handler(httpd_req_t *request);
    static esp_err_t skyplot_handler(httpd_req_t *request);
    static esp_err_t skyplot_data_handler(httpd_req_t *request);
    static esp_err_t status_handler(httpd_req_t *request);
    static esp_err_t update_page_handler(httpd_req_t *request);
    static esp_err_t update_upload_handler(httpd_req_t *request);
    static esp_err_t position_handler(httpd_req_t *request);
    static esp_err_t survey_handler(httpd_req_t *request);
    static esp_err_t wifi_handler(httpd_req_t *request);

    bool authorize(httpd_req_t *request) const;
    esp_err_t send_unauthorized(httpd_req_t *request) const;
    esp_err_t send_html(httpd_req_t *request, const std::string &body) const;

    static AdminWebServer *self(httpd_req_t *request);
    static std::string read_body(httpd_req_t *request, size_t max_length);
    static std::string form_value(const std::string &body, const char *name);
    static std::string url_decode(const std::string &value);
    static std::string html_escape(const std::string &value);
    static std::string json_escape(const std::string &value);
};
