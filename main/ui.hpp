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

    lv_display_t *disp_   = nullptr;   // saved for runtime theme rebuilds
    bool          night_  = true;      // current palette: true = night, false = day

    // ── Shell ───────────────────────────────────────────────────────────────────
    lv_obj_t *tabview_       = nullptr;   // bottom tabview (5 sections)
    lv_obj_t *header_title_  = nullptr;   // persistent section title (left)
    lv_obj_t *header_pill_   = nullptr;   // global worst-of-all status pill container
    lv_obj_t *header_pill_dot_ = nullptr; // pill colored dot
    lv_obj_t *header_pill_lbl_ = nullptr; // pill word (LIVE / SURVEYING / ALARM)
    lv_obj_t *header_theme_btn_ = nullptr; // day/night toggle (placeholder this phase)

    // ── Tab containers ──────────────────────────────────────────────────────────
    lv_obj_t *tab_dash_    = nullptr;
    lv_obj_t *tab_pos_     = nullptr;
    lv_obj_t *tab_links_   = nullptr;
    lv_obj_t *tab_storage_ = nullptr;
    lv_obj_t *tab_sys_     = nullptr;

    // ── Dashboard tab ───────────────────────────────────────────────────────────
    lv_obj_t *lbl_hero_val_   = nullptr;   // RTCM B/s (48px hero) / survey ± when surveying
    lv_obj_t *lbl_hero_unit_  = nullptr;   // hero unit suffix
    lv_obj_t *lbl_hero_sub_   = nullptr;   // hero sub-line (total / gate)
    lv_obj_t *lbl_hero_cap_   = nullptr;   // hero caption / title
    lv_obj_t *lbl_t_mode_     = nullptr;   // Mode tile value
    lv_obj_t *lbl_t_sats_     = nullptr;   // Satellites tile value
    lv_obj_t *lbl_t_sats_sub_ = nullptr;   // Satellites tile constellation sub
    // NTRIP Push tile — three per-service lights (RTK2go / Onocoy / RTKdata).
    // Each row: colored dot + service name + reconnect count after the name.
    lv_obj_t *t_ntrip_dot_[3]   = {nullptr, nullptr, nullptr};  // service status dots
    lv_obj_t *t_ntrip_recon_[3] = {nullptr, nullptr, nullptr};  // per-service reconnect labels
    lv_obj_t *t_ntrip_clients_  = nullptr;                      // direct client IPs
    lv_obj_t *lbl_t_pos_      = nullptr;   // Position tile value (compact lat/lon)
    lv_obj_t *lbl_t_pos_sub_  = nullptr;   // Position tile sub
    lv_obj_t *lbl_t_sd_       = nullptr;   // SD/RINEX tile value
    lv_obj_t *lbl_t_sd_sub_   = nullptr;   // SD/RINEX tile sub (RINEX state)
    lv_obj_t *lbl_t_link_     = nullptr;   // Link tile value (SSID / AP)
    lv_obj_t *lbl_t_link_sub_ = nullptr;   // Link tile sub (RSSI / IP)
    lv_obj_t *btn_survey_start_ = nullptr; // pinned Start/Restart Survey
    lv_obj_t *lbl_survey_btn_   = nullptr;

    // ── Position tab ────────────────────────────────────────────────────────────
    lv_obj_t *lbl_p_lat_       = nullptr;
    lv_obj_t *lbl_p_lon_       = nullptr;
    lv_obj_t *lbl_p_alt_       = nullptr;
    lv_obj_t *survey_group_    = nullptr;   // SURVEY-IN group (dimmed when Base TX)
    lv_obj_t *arc_sv_time_     = nullptr;    // gate: elapsed >= 300s
    lv_obj_t *lbl_sv_time_     = nullptr;
    lv_obj_t *arc_sv_blocks_   = nullptr;    // gate: blocks >= 5
    lv_obj_t *lbl_sv_blocks_   = nullptr;
    lv_obj_t *arc_sv_stab_     = nullptr;    // gate: stability <= 0.50m
    lv_obj_t *lbl_sv_stab_     = nullptr;
    lv_obj_t *lbl_sv_samples_  = nullptr;
    lv_obj_t *lbl_sv_sigma_    = nullptr;
    lv_obj_t *lbl_sv_used_     = nullptr;
    lv_obj_t *lbl_sv_gps_      = nullptr;
    lv_obj_t *lbl_sv_glo_      = nullptr;
    lv_obj_t *lbl_sv_gal_      = nullptr;
    lv_obj_t *lbl_sv_bds_      = nullptr;
    lv_obj_t *lbl_sv_detail_   = nullptr;    // per-sat SNR list
    lv_obj_t *lbl_p_ant_       = nullptr;    // antenna read-only echo
    lv_obj_t *sky_plot_        = nullptr;    // square polar sky-plot area (rings + NSEW)
    lv_obj_t *sky_dots_        = nullptr;    // overlay rebuilt with sat dots each tick
    lv_obj_t *lbl_sky_count_   = nullptr;    // "N satellites in view"

    // ── Links tab ───────────────────────────────────────────────────────────────
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

    // ── Storage tab ─────────────────────────────────────────────────────────────
    lv_obj_t *arc_disk_        = nullptr;
    lv_obj_t *lbl_disk_pct_    = nullptr;    // % centered in arc
    lv_obj_t *lbl_sd_          = nullptr;    // used / total
    lv_obj_t *sw_rinex_        = nullptr;
    lv_obj_t *lbl_rinex_file_  = nullptr;
    lv_obj_t *lbl_s_ant_       = nullptr;    // antenna echo (RINEX header)

    // ── System tab ──────────────────────────────────────────────────────────────
    lv_obj_t *lbl_uptime_      = nullptr;
    lv_obj_t *lbl_reset_       = nullptr;
    lv_obj_t *arc_heap_        = nullptr;
    lv_obj_t *lbl_heap_pct_    = nullptr;
    lv_obj_t *lbl_heap_        = nullptr;    // free heap value
    lv_obj_t *lbl_wifi_state_  = nullptr;    // Station: SSID + RSSI / status
    lv_obj_t *lbl_ip_          = nullptr;    // Station IP
    lv_obj_t *lbl_ap_name_     = nullptr;    // Hotspot SSID
    lv_obj_t *lbl_ap_ip_       = nullptr;    // Hotspot IP
    lv_obj_t *lbl_fw_          = nullptr;
    lv_obj_t *lbl_compile_     = nullptr;
    lv_obj_t *lbl_c6_running_  = nullptr;
    lv_obj_t *lbl_c6_fw_       = nullptr;    // "C6 available" / OTA status line
    lv_obj_t *btn_c6_ota_      = nullptr;
    lv_obj_t *lbl_admin_       = nullptr;    // admin password set / not set
    lv_obj_t *lbl_console_     = nullptr;    // console log tail (folded-in Debug)
    std::atomic<int> c6_ota_progress_{-1};   // -1=idle, 0-100=%, -2=failed
    char c6_running_ver_[48]   = "";         // filled by c6_version_task (RPC)
    char c6_avail_ver_[64]     = "";         // parsed from embedded C6 image
    std::atomic<bool> c6_running_ready_{false};

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

    // ── Admin password modal ──────────────────────────────────────────────────
    lv_obj_t *modal_admin_      = nullptr;
    lv_obj_t *ta_admin_pass_    = nullptr;
    lv_obj_t *lbl_admin_msg_    = nullptr;
    lv_obj_t *kb_admin_         = nullptr;

    // ── Antenna edit modal ────────────────────────────────────────────────────
    lv_obj_t *modal_ant_        = nullptr;
    lv_obj_t *ta_ant_model_     = nullptr;
    lv_obj_t *ta_ant_radome_    = nullptr;
    lv_obj_t *ta_ant_height_    = nullptr;
    lv_obj_t *lbl_ant_msg_      = nullptr;
    lv_obj_t *kb_ant_           = nullptr;

    // ── Console modal (separate scrollable log view, opened from System) ───────
    lv_obj_t *modal_console_    = nullptr;
    lv_obj_t *console_scroll_   = nullptr;

    // ── Position Setup modal (manual position / survey / RINEX / antenna) ──────
    lv_obj_t *modal_pos_setup_  = nullptr;
    lv_obj_t *kb_pos_setup_     = nullptr;
    // Manual position
    lv_obj_t *ta_ps_lat_        = nullptr;
    lv_obj_t *ta_ps_lon_        = nullptr;
    lv_obj_t *ta_ps_height_     = nullptr;
    lv_obj_t *lbl_ps_pos_msg_   = nullptr;
    // RINEX collection
    lv_obj_t *sw_ps_rinex_      = nullptr;
    lv_obj_t *lbl_ps_rinex_     = nullptr;   // current state echo
    // Antenna (canonical entry; moved here from System)
    lv_obj_t *ta_ps_ant_model_  = nullptr;
    lv_obj_t *ta_ps_ant_radome_ = nullptr;
    lv_obj_t *ta_ps_ant_height_ = nullptr;
    lv_obj_t *lbl_ps_ant_msg_   = nullptr;

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
    lv_obj_t *btn_fb_select_    = nullptr;   // "Select" button in the path bar
    std::string fb_path_;
    std::vector<std::string> fb_entries_;
    std::vector<bool>        fb_is_dir_;     // parallel to fb_entries_ (dir flag)

    // ── Bulk-delete selection state (mutated only on the LVGL task) ──────────────
    bool fb_select_mode_ = false;            // selection mode active
    std::vector<bool> fb_selected_;          // parallel to fb_entries_
    lv_obj_t *fb_action_bar_   = nullptr;    // selection action bar (slides up)
    lv_obj_t *lbl_fb_count_    = nullptr;    // "N selected · {size}"
    lv_obj_t *btn_fb_delete_   = nullptr;    // "Delete (N)" (--crit)
    lv_obj_t *lbl_fb_delete_   = nullptr;

    // ── Bulk-delete confirm / progress dialog (lv_msgbox on lv_layer_top) ────────
    lv_obj_t *fb_confirm_mbox_   = nullptr;  // active confirm/progress msgbox
    lv_obj_t *fb_confirm_del_btn_ = nullptr; // the Delete footer button (gated)
    lv_obj_t *fb_confirm_switch_  = nullptr; // "I understand…" toggle (non-empty dirs)
    lv_obj_t *fb_prog_bar_       = nullptr;  // progress bar (shown during delete)
    lv_obj_t *fb_prog_lbl_       = nullptr;  // "Deleting N / M…"
    std::vector<std::string> fb_pending_;    // paths queued for the active confirm
    bool fb_delete_in_progress_  = false;    // a job is running, drive from refresh()

    // ── Build helpers ─────────────────────────────────────────────────────────
    void build_screens(lv_display_t *disp);
    void apply_theme(bool night);            // runtime day/night swap (rebuilds screen)
    void build_header(lv_obj_t *parent);
    void build_dashboard_tab(lv_obj_t *parent);
    void build_ntrip_lights_tile(lv_obj_t *grid);   // custom 3-service-light tile
    void build_position_tab(lv_obj_t *parent);
    void build_links_tab(lv_obj_t *parent);
    void build_storage_tab(lv_obj_t *parent);
    void build_system_tab(lv_obj_t *parent);
    void build_wifi_modal();
    void build_ntrip_modal();
    void build_file_browser();
    void build_admin_modal();
    void build_antenna_modal();
    void build_console_modal();
    void build_position_setup();

    // ── Modal show/populate ───────────────────────────────────────────────────
    void open_wifi_modal();
    void open_ntrip_modal(int idx);
    void open_file_browser(const std::string &path);
    void open_admin_modal();
    void open_antenna_modal();
    void open_console_modal();
    void open_position_setup();
    void refresh_file_browser();
    void populate_wifi_scan_list(const std::vector<WifiNetwork> &nets);
    void save_ntrip_config();

    // ── Periodic refresh ──────────────────────────────────────────────────────
    static void refresh_timer_cb(lv_timer_t *timer);
    void refresh();
    void refresh_console_log();
    void update_header(const BaseStationStatus &st);

    // ── Static helpers ────────────────────────────────────────────────────────
    static lv_obj_t *make_group(lv_obj_t *parent, const char *title);
    static lv_obj_t *make_row(lv_obj_t *parent, lv_obj_t **val_out,
                               const char *key);
    static lv_obj_t *make_switch_row(lv_obj_t *parent, lv_obj_t **sw_out,
                                      const char *key);
    static lv_obj_t *make_modal_base(lv_obj_t *parent, const char *title);
    // 2-column dashboard tile. span2 makes it span both columns (hero).
    // tappable wires a deep-link to switch tab on click (via on_tile_click).
    lv_obj_t *make_tile(lv_obj_t *parent, const char *label,
                        lv_obj_t **val_out, lv_obj_t **sub_out,
                        const lv_font_t *val_font, bool span2,
                        int target_tab);
    // Status pill (dot + word). Returns container; dot/word via outs.
    static lv_obj_t *make_pill(lv_obj_t *parent, lv_obj_t **dot_out,
                                lv_obj_t **word_out);
    // 270° gauge arc with a centered value label.
    static lv_obj_t *make_gauge(lv_obj_t *parent, const char *label,
                                 lv_obj_t **arc_out, lv_obj_t **val_out,
                                 int32_t range_max);
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
    static void on_tab_changed(lv_event_t *e);
    static void on_tile_click(lv_event_t *e);
    static void on_theme_btn(lv_event_t *e);

    // ── WiFi modal callbacks ──────────────────────────────────────────────────
    static void on_wifi_btn(lv_event_t *e);
    static void on_wifi_close(lv_event_t *e);
    static void on_wifi_scan(lv_event_t *e);
    static void on_wifi_connect(lv_event_t *e);
    static void on_wifi_list_click(lv_event_t *e);
    static void on_ap_save(lv_event_t *e);
    static void ap_apply_task(void *arg);
    static void wifi_scan_task(void *arg);

    // ── Admin password modal callbacks ────────────────────────────────────────
    static void on_admin_btn(lv_event_t *e);
    static void on_admin_close(lv_event_t *e);
    static void on_admin_save(lv_event_t *e);

    // ── Antenna edit modal callbacks ──────────────────────────────────────────
    static void on_antenna_btn(lv_event_t *e);
    static void on_antenna_close(lv_event_t *e);
    static void on_antenna_save(lv_event_t *e);
    static void on_console_btn(lv_event_t *e);
    static void on_console_close(lv_event_t *e);

    // ── Position Setup modal callbacks ────────────────────────────────────────
    static void on_pos_setup_tile(lv_event_t *e);   // Dashboard Position tile → open
    static void on_pos_setup_close(lv_event_t *e);
    static void on_ps_pos_save(lv_event_t *e);       // Save manual position
    static void on_ps_ant_save(lv_event_t *e);       // Save antenna metadata

    // ── NTRIP modal callbacks ─────────────────────────────────────────────────
    static void on_ntrip_close(lv_event_t *e);
    static void on_ntrip_save(lv_event_t *e);

    // ── File browser callbacks ────────────────────────────────────────────────
    static void on_files_btn(lv_event_t *e);
    static void on_fb_close(lv_event_t *e);
    static void on_fb_item(lv_event_t *e);
    static void on_fb_item_long(lv_event_t *e);   // long-press → selection mode
    static void on_fb_up(lv_event_t *e);

    // ── Bulk-delete UI (touch) ────────────────────────────────────────────────
    void enter_select_mode(int seed_idx);   // seed_idx<0 → none preselected
    void exit_select_mode();
    void update_fb_selection();              // refresh checks, count, Delete (N)
    void open_delete_confirm(const std::vector<std::string> &paths,
                             bool folder, const std::string &folder_name,
                             bool managed);
    void poll_delete_progress();             // called from refresh() each 1 s tick
    static void on_fb_select_btn(lv_event_t *e);
    static void on_fb_selall_btn(lv_event_t *e);
    static void on_fb_delete_btn(lv_event_t *e);     // "Delete (N)" in action bar
    static void on_fb_cancel_btn(lv_event_t *e);     // action-bar Cancel
    static void on_fb_folder_btn(lv_event_t *e);     // per-row trash glyph
    static void on_fb_confirm_switch(lv_event_t *e); // "I understand" gate
    static void on_fb_confirm_del(lv_event_t *e);    // confirm Delete → start job
    static void on_fb_confirm_cancel(lv_event_t *e); // confirm Cancel/close/OK

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
