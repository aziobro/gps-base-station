#pragma once

#include <atomic>
#include <string>
#include <vector>

#include "esp_err.h"
#include "lvgl.h"

#include "base_station.hpp"
#include "sd_manager.hpp"
#include "storage.hpp"
#include "wifi_manager.hpp"

class Display;

class Ui {
public:
    esp_err_t init(Display &display, BaseStation &station,
                   SdManager &sd, WifiManager &wifi, Storage &storage);

private:
    BaseStation *station_ = nullptr;
    SdManager   *sd_      = nullptr;
    WifiManager *wifi_    = nullptr;
    Storage     *storage_ = nullptr;

    // ── Tab containers ────────────────────────────────────────────────────────
    lv_obj_t *tab_status_  = nullptr;
    lv_obj_t *tab_ntrip_   = nullptr;
    lv_obj_t *tab_pos_     = nullptr;
    lv_obj_t *tab_sys_     = nullptr;
    lv_obj_t *tab_debug_   = nullptr;

    // ── Status tab ────────────────────────────────────────────────────────────
    lv_obj_t *lbl_mode_         = nullptr;
    lv_obj_t *lbl_rtcm_         = nullptr;
    lv_obj_t *lbl_sats_         = nullptr;
    lv_obj_t *btn_survey_start_ = nullptr;
    lv_obj_t *lbl_survey_btn_   = nullptr;
    lv_obj_t *lbl_lat_          = nullptr;
    lv_obj_t *lbl_lon_          = nullptr;
    lv_obj_t *lbl_alt_          = nullptr;
    lv_obj_t *lbl_survey_       = nullptr;
    lv_obj_t *bar_survey_       = nullptr;
    lv_obj_t *lbl_rtk2go_       = nullptr;
    lv_obj_t *lbl_onocoy_       = nullptr;
    lv_obj_t *lbl_rtkdata_      = nullptr;
    lv_obj_t *lbl_local_ntrip_  = nullptr;
    lv_obj_t *lbl_local_ntrip_ips_ = nullptr;

    // ── NTRIP tab ─────────────────────────────────────────────────────────────
    lv_obj_t *sw_ntrip_all_        = nullptr;
    lv_obj_t *lbl_d_rtk2go_        = nullptr;
    lv_obj_t *lbl_d_rtk2go_bytes_  = nullptr;
    lv_obj_t *lbl_d_rtk2go_drop_   = nullptr;
    lv_obj_t *lbl_d_rtk2go_recon_  = nullptr;
    lv_obj_t *lbl_d_onocoy_        = nullptr;
    lv_obj_t *lbl_d_onocoy_bytes_  = nullptr;
    lv_obj_t *lbl_d_onocoy_drop_   = nullptr;
    lv_obj_t *lbl_d_onocoy_recon_  = nullptr;
    lv_obj_t *lbl_d_rtkdata_       = nullptr;
    lv_obj_t *lbl_d_rtkdata_bytes_ = nullptr;
    lv_obj_t *lbl_d_rtkdata_drop_  = nullptr;
    lv_obj_t *lbl_d_rtkdata_recon_ = nullptr;
    lv_obj_t *lbl_d_local_ntrip_   = nullptr;
    lv_obj_t *lbl_d_local_ntrip_ips_ = nullptr;

    // ── Position tab ──────────────────────────────────────────────────────────
    lv_obj_t *lbl_d_lat_        = nullptr;
    lv_obj_t *lbl_d_lon_        = nullptr;
    lv_obj_t *lbl_d_alt_        = nullptr;
    lv_obj_t *lbl_d_survey_     = nullptr;
    lv_obj_t *bar_d_survey_     = nullptr;
    lv_obj_t *lbl_sv_elapsed_   = nullptr;
    lv_obj_t *lbl_sv_blocks_    = nullptr;
    lv_obj_t *lbl_sv_samples_   = nullptr;
    lv_obj_t *lbl_sv_stability_ = nullptr;
    lv_obj_t *lbl_sv_sigma_     = nullptr;
    lv_obj_t *lbl_sv_used_      = nullptr;
    lv_obj_t *lbl_sv_gps_       = nullptr;
    lv_obj_t *lbl_sv_glo_       = nullptr;
    lv_obj_t *lbl_sv_gal_       = nullptr;
    lv_obj_t *lbl_sv_bds_       = nullptr;
    lv_obj_t *lbl_sv_detail_    = nullptr;

    // ── System tab ────────────────────────────────────────────────────────────
    lv_obj_t *lbl_uptime_       = nullptr;
    lv_obj_t *lbl_reset_        = nullptr;
    lv_obj_t *lbl_wifi_state_   = nullptr;  // Station: SSID + RSSI / status
    lv_obj_t *lbl_ip_           = nullptr;  // Station IP
    lv_obj_t *lbl_ap_name_      = nullptr;  // Hotspot SSID
    lv_obj_t *lbl_ap_ip_        = nullptr;  // Hotspot IP
    lv_obj_t *lbl_sd_           = nullptr;
    lv_obj_t *sw_rinex_         = nullptr;
    lv_obj_t *lbl_rinex_file_   = nullptr;
    lv_obj_t *lbl_fw_           = nullptr;
    lv_obj_t *lbl_compile_      = nullptr;
    lv_obj_t *lbl_c6_running_   = nullptr;
    lv_obj_t *lbl_c6_fw_        = nullptr;  // "C6 available" / OTA status line
    lv_obj_t *btn_c6_ota_       = nullptr;
    std::atomic<int> c6_ota_progress_{-1}; // -1=idle, 0-100=%, -2=failed
    char c6_running_ver_[48]    = "";       // filled by c6_version_task (RPC)
    char c6_avail_ver_[64]      = "";       // parsed from embedded C6 image
    std::atomic<bool> c6_running_ready_{false};

    // ── Debug tab ─────────────────────────────────────────────────────────────
    lv_obj_t *lbl_debug_        = nullptr;

    // ── WiFi config modal ─────────────────────────────────────────────────────
    lv_obj_t *modal_wifi_       = nullptr;
    lv_obj_t *ta_wifi_ssid_     = nullptr;
    lv_obj_t *ta_wifi_pass_     = nullptr;
    lv_obj_t *kb_wifi_          = nullptr;
    lv_obj_t *list_wifi_scan_   = nullptr;
    lv_obj_t *lbl_wifi_msg_     = nullptr;
    lv_obj_t *btn_wifi_scan_    = nullptr;
    lv_obj_t *ta_ap_pass_       = nullptr;  // SoftAP (hotspot) password field
    lv_obj_t *lbl_ap_msg_       = nullptr;
    std::atomic<bool> scan_running_{false};

    // ── NTRIP config modal (shared, repopulated per service) ──────────────────
    lv_obj_t *modal_ntrip_      = nullptr;
    lv_obj_t *lbl_ntrip_title_  = nullptr;
    lv_obj_t *sw_ntrip_en_      = nullptr;
    lv_obj_t *ta_ntrip_mp_      = nullptr;
    lv_obj_t *ta_ntrip_pw_      = nullptr;
    lv_obj_t *kb_ntrip_         = nullptr;
    int       ntrip_cfg_idx_    = 0;

    // ── File browser modal ────────────────────────────────────────────────────
    lv_obj_t *modal_files_      = nullptr;
    lv_obj_t *lbl_fb_path_      = nullptr;
    lv_obj_t *list_fb_          = nullptr;
    std::string fb_path_;
    std::vector<std::string> fb_entries_;

    // ── Build helpers ─────────────────────────────────────────────────────────
    void build_screens(lv_display_t *disp);
    void build_status_tab(lv_obj_t *parent);
    void build_ntrip_tab(lv_obj_t *parent);
    void build_position_tab(lv_obj_t *parent);
    void build_system_tab(lv_obj_t *parent);
    void build_debug_tab(lv_obj_t *parent);
    void build_wifi_modal();
    void build_ntrip_modal();
    void build_file_browser();

    // ── Modal show/populate ───────────────────────────────────────────────────
    void open_wifi_modal();
    void open_ntrip_modal(int idx);
    void open_file_browser(const std::string &path);
    void refresh_file_browser();
    void populate_wifi_scan_list(const std::vector<WifiNetwork> &nets);
    void save_ntrip_config();

    // ── Periodic refresh ──────────────────────────────────────────────────────
    static void refresh_timer_cb(lv_timer_t *timer);
    void refresh();
    void refresh_debug_log();

    // ── Static helpers ────────────────────────────────────────────────────────
    static lv_obj_t *make_group(lv_obj_t *parent, const char *title);
    static lv_obj_t *make_row(lv_obj_t *parent, lv_obj_t **val_out,
                               const char *key);
    static lv_obj_t *make_switch_row(lv_obj_t *parent, lv_obj_t **sw_out,
                                      const char *key);
    static lv_obj_t *make_modal_base(lv_obj_t *parent, const char *title);
    static void fmt_ntrip_label(lv_obj_t *lbl, const NtripStatus &ns,
                                 char *buf, size_t buf_len);
    static void fmt_bytes_str(char *buf, size_t n, uint64_t bytes);
    static void on_ta_focused(lv_event_t *e);
    static void on_ta_defocused(lv_event_t *e);
    static void on_kb_ready(lv_event_t *e);

    // ── Static event callbacks (public so free helper functions can use them) ──
public:
    static void on_ntrip_cfg_btn(lv_event_t *e);
private:
    // ── WiFi modal callbacks ──────────────────────────────────────────────────
    static void on_wifi_btn(lv_event_t *e);
    static void on_wifi_close(lv_event_t *e);
    static void on_wifi_scan(lv_event_t *e);
    static void on_wifi_connect(lv_event_t *e);
    static void on_wifi_list_click(lv_event_t *e);
    static void on_ap_save(lv_event_t *e);
    static void ap_apply_task(void *arg);
    static void wifi_scan_task(void *arg);

    // ── NTRIP modal callbacks ─────────────────────────────────────────────────
    static void on_ntrip_close(lv_event_t *e);
    static void on_ntrip_save(lv_event_t *e);

    // ── File browser callbacks ────────────────────────────────────────────────
    static void on_files_btn(lv_event_t *e);
    static void on_fb_close(lv_event_t *e);
    static void on_fb_item(lv_event_t *e);
    static void on_fb_up(lv_event_t *e);

    // ── RINEX toggle callback ─────────────────────────────────────────────────
    static void on_rinex_toggle(lv_event_t *e);

    // ── NTRIP global toggle ───────────────────────────────────────────────────
    static void on_ntrip_all_toggle(lv_event_t *e);

    // ── Survey start button ───────────────────────────────────────────────────
    static void on_survey_start(lv_event_t *e);
    static void on_survey_confirm(lv_event_t *e);

    // ── C6 coprocessor OTA ────────────────────────────────────────────────────
    static void on_c6_ota_btn(lv_event_t *e);
    static void on_c6_ota_confirm(lv_event_t *e);
    static void c6_ota_task(void *arg);
    static void c6_version_task(void *arg);  // queries running C6 fw version (RPC)
    void load_c6_available_version();        // parses embedded C6 image app-desc
};
