#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "esp_err.h"
#include "esp_http_server.h"

#include "sd_manager.hpp"
#include "storage.hpp"
#include "ui_sections.hpp"
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
    std::string session_token_;
    int64_t session_last_seen_us_ = 0;
    uint32_t failed_login_count_ = 0;
    int64_t login_block_until_us_ = 0;

    esp_err_t register_secure_handlers();
    static esp_err_t login_get_handler(httpd_req_t *request);
    static esp_err_t login_post_handler(httpd_req_t *request);
    static esp_err_t logout_handler(httpd_req_t *request);
    static esp_err_t root_handler(httpd_req_t *request);
    static esp_err_t setup_get_handler(httpd_req_t *request);
    static esp_err_t setup_post_handler(httpd_req_t *request);
    static esp_err_t config_get_handler(httpd_req_t *request);
    static esp_err_t config_post_handler(httpd_req_t *request);
    static esp_err_t position_page_handler(httpd_req_t *request);
    static esp_err_t system_page_handler(httpd_req_t *request);
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
    static esp_err_t antenna_handler(httpd_req_t *request);
    static esp_err_t logs_page_handler(httpd_req_t *request);
    static esp_err_t logs_data_handler(httpd_req_t *request);
    static esp_err_t http_gateway_handler(httpd_req_t *request);
    static esp_err_t ca_certificate_handler(httpd_req_t *request);
    static esp_err_t files_page_handler(httpd_req_t *request);
    static esp_err_t files_list_handler(httpd_req_t *request);
    static esp_err_t files_download_handler(httpd_req_t *request);
    static esp_err_t files_delete_handler(httpd_req_t *request);
    static esp_err_t files_preview_handler(httpd_req_t *request);
    static esp_err_t files_delete_batch_handler(httpd_req_t *request);
    static esp_err_t files_delete_status_handler(httpd_req_t *request);
    static esp_err_t files_rename_handler(httpd_req_t *request);
    static esp_err_t files_mkdir_handler(httpd_req_t *request);
    static esp_err_t rinex_toggle_handler(httpd_req_t *request);
    static esp_err_t ntrip_toggle_handler(httpd_req_t *request);
    static esp_err_t theme_post_handler(httpd_req_t *request);
    static esp_err_t rinex_export_page_handler(httpd_req_t *request);
    static esp_err_t rinex_export_handler(httpd_req_t *request);

    bool authorize(httpd_req_t *request);
    bool password_matches(const std::string &user, const std::string &password) const;
    std::string start_session_cookie(bool secure);
    std::string clear_session_cookie(bool secure);
    esp_err_t send_unauthorized(httpd_req_t *request) const;
    esp_err_t send_login_page(
        httpd_req_t *request, const std::string &message = "",
        const std::string &next = "/") const;
    // The web nav has SIX tabs (Dashboard/Position/Satellite/Links/Storage/
    // System) — one more than ui_sections::Section, which is shared with the touch
    // UI and frozen at 5. The active tab is therefore selected by matching the
    // page's *route* string (e.g. "/skyplot") against a web-local list, not by the
    // shared Section enum. Pass the route that owns the current page.
    esp_err_t send_page(
        httpd_req_t *request, const char *title,
        const std::string &content,
        const char *active_route = "/") const;
    // Shared top segmented nav (6 web tabs + status pill + day/night toggle),
    // rendered once per page right after the <h1> by send_page. Highlights the tab
    // whose route equals active_route, and carries the global /status poller that
    // keeps the worst-of-all pill live on every page.
    static std::string nav_html(const char *active_route);

    static AdminWebServer *self(httpd_req_t *request);
    static std::string read_body(httpd_req_t *request, size_t max_length);
    static std::string form_value(const std::string &body, const char *name);
    static std::string url_decode(const std::string &value);
    static std::string html_escape(const std::string &value);
    static std::string json_escape(const std::string &value);
    static std::string query_param(httpd_req_t *request, const char *key);
    static std::string json_field(const std::string &body, const char *key);
    // Extracts the elements of a JSON string array (e.g. "paths":[...]) into a
    // vector. Defensive: caps element count/length, JSON/url-unescapes each, and
    // rejects (returns empty + sets *rejected) on oversize or malformed input.
    static std::vector<std::string> json_string_array(
        const std::string &body, const char *key, bool *rejected);
};
