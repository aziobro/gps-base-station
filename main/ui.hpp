#pragma once

#include "esp_err.h"
#include "lvgl.h"

#include "base_station.hpp"
#include "sd_manager.hpp"
#include "storage.hpp"
#include "wifi_manager.hpp"

class Display;

// LVGL-based touchscreen UI for the GPS RTK base station.
// Four tabs (Status / NTRIP / Position / System) navigated by touch.
// Call init() once after Display::init() completes.
// The BSP's LVGL port task handles rendering and touch — thread safety is
// managed via bsp_display_lock() / bsp_display_unlock().
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

    // Status tab
    lv_obj_t *lbl_mode_   = nullptr;
    lv_obj_t *lbl_rtcm_   = nullptr;
    lv_obj_t *lbl_sats_   = nullptr;
    lv_obj_t *lbl_local_  = nullptr;

    // NTRIP tab
    lv_obj_t *lbl_rtk2go_      = nullptr;
    lv_obj_t *lbl_onocoy_      = nullptr;
    lv_obj_t *lbl_rtkdata_     = nullptr;
    lv_obj_t *lbl_local_ntrip_ = nullptr;

    // Position tab
    lv_obj_t *lbl_pos_     = nullptr;
    lv_obj_t *lbl_survey_  = nullptr;
    lv_obj_t *bar_survey_  = nullptr;

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
};
