#pragma once

#include <string>

#include "esp_err.h"
#include "esp_http_server.h"

#include "sd_manager.hpp"
#include "storage.hpp"
#include "wifi_manager.hpp"

class BaseStation;

class AdminWebServer {
public:
    esp_err_t start(
        Storage &storage, WifiManager &wifi,
        BaseStation &station, SdManager &sd);
    void stop();

private:
    Storage *storage_ = nullptr;
    WifiManager *wifi_ = nullptr;
    BaseStation *station_ = nullptr;
    SdManager *sd_ = nullptr;
    httpd_handle_t https_server_ = nullptr;
    httpd_handle_t http_server_ = nullptr;

    esp_err_t register_secure_handlers();
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
    static esp_err_t ap_password_handler(httpd_req_t *request);
    static esp_err_t logs_page_handler(httpd_req_t *request);
    static esp_err_t logs_data_handler(httpd_req_t *request);
    static esp_err_t http_gateway_handler(httpd_req_t *request);
    static esp_err_t ca_certificate_handler(httpd_req_t *request);
    static esp_err_t files_page_handler(httpd_req_t *request);
    static esp_err_t files_list_handler(httpd_req_t *request);
    static esp_err_t files_download_handler(httpd_req_t *request);
    static esp_err_t files_delete_handler(httpd_req_t *request);
    static esp_err_t files_rename_handler(httpd_req_t *request);
    static esp_err_t files_mkdir_handler(httpd_req_t *request);
    static esp_err_t rinex_toggle_handler(httpd_req_t *request);
    static esp_err_t ntrip_toggle_handler(httpd_req_t *request);
    static esp_err_t files_rinex_merge_handler(httpd_req_t *request);

    bool authorize(httpd_req_t *request) const;
    esp_err_t send_unauthorized(httpd_req_t *request) const;
    esp_err_t send_page(
        httpd_req_t *request, const char *title,
        const std::string &content) const;

    static AdminWebServer *self(httpd_req_t *request);
    static std::string read_body(httpd_req_t *request, size_t max_length);
    static std::string form_value(const std::string &body, const char *name);
    static std::string url_decode(const std::string &value);
    static std::string html_escape(const std::string &value);
    static std::string json_escape(const std::string &value);
    static std::string query_param(httpd_req_t *request, const char *key);
    static std::string json_field(const std::string &body, const char *key);
};
