#pragma once

#include "esp_err.h"
#include "lvgl.h"

#include "base_station.hpp"
#include "sd_manager.hpp"
#include "storage.hpp"
#include "wifi_manager.hpp"

class Display;

// LVGL-based touchscreen UI for the GPS RTK base station.
// Four tabs navigated by touch.  The Status tab is a combined dashboard
// showing operation, position, and NTRIP state in grouped boxes.
// Call init() once after Display::init() completes.
class Ui {
public:
    esp_err_t init(Display &display, BaseStation &station,
                   SdManager &sd, WifiManager &wifi, Storage &storage);

private:
    BaseStation *station_ = nullptr;
    SdManager   *sd_      = nullptr;
    WifiManager *wifi_    = nullptr;
    Storage     *storage_ = nullptr;

    // Tabview page containers
    lv_obj_t *tab_status_ = nullptr;
    lv_obj_t *tab_ntrip_  = nullptr;
    lv_obj_t *tab_pos_    = nullptr;
    lv_obj_t *tab_sys_    = nullptr;

    // Status tab — Base Operation group
    lv_obj_t *lbl_mode_  = nullptr;
    lv_obj_t *lbl_rtcm_  = nullptr;
    lv_obj_t *lbl_sats_  = nullptr;

    // Status tab — Position group
    lv_obj_t *lbl_lat_      = nullptr;
    lv_obj_t *lbl_lon_      = nullptr;
    lv_obj_t *lbl_alt_      = nullptr;
    lv_obj_t *lbl_survey_   = nullptr;
    lv_obj_t *bar_survey_   = nullptr;

    // Status tab — NTRIP group
    lv_obj_t *lbl_rtk2go_      = nullptr;
    lv_obj_t *lbl_onocoy_      = nullptr;
    lv_obj_t *lbl_rtkdata_     = nullptr;
    lv_obj_t *lbl_local_ntrip_ = nullptr;

    // NTRIP detail tab (mirrors status tab NTRIP group)
    lv_obj_t *lbl_d_rtk2go_      = nullptr;
    lv_obj_t *lbl_d_onocoy_      = nullptr;
    lv_obj_t *lbl_d_rtkdata_     = nullptr;
    lv_obj_t *lbl_d_local_ntrip_ = nullptr;

    // Position detail tab
    lv_obj_t *lbl_d_lat_    = nullptr;
    lv_obj_t *lbl_d_lon_    = nullptr;
    lv_obj_t *lbl_d_alt_    = nullptr;
    lv_obj_t *lbl_d_survey_ = nullptr;
    lv_obj_t *bar_d_survey_ = nullptr;

    // System tab
    lv_obj_t *lbl_ip_    = nullptr;
    lv_obj_t *lbl_wifi_  = nullptr;
    lv_obj_t *lbl_sd_    = nullptr;
    lv_obj_t *lbl_rinex_ = nullptr;
    lv_obj_t *lbl_fw_    = nullptr;

    void build_screens(lv_display_t *disp);
    void build_status_tab(lv_obj_t *parent);
    void build_ntrip_tab(lv_obj_t *parent);
    void build_position_tab(lv_obj_t *parent);
    void build_system_tab(lv_obj_t *parent);

    static void refresh_timer_cb(lv_timer_t *timer);
    void refresh();

    // Helpers
    static lv_obj_t *make_group(lv_obj_t *parent, const char *title);
    static lv_obj_t *make_row(lv_obj_t *parent, lv_obj_t **val_out,
                               const char *key);
    static void fmt_ntrip_label(lv_obj_t *lbl, const NtripStatus &ns,
                                 char *buf, size_t buf_len);
};
