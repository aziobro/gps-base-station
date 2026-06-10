#include "ui.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "bsp/esp-bsp.h"
#include "display.hpp"
#include "esp_app_desc.h"
#include "esp_log.h"

namespace {
constexpr char kTag[] = "ui";

void fmt_bytes(char *buf, size_t n, uint64_t bytes) {
    if (bytes >= 1024ULL * 1024 * 1024) {
        snprintf(buf, n, "%.1f GB", (double)bytes / (1024.0 * 1024 * 1024));
    } else if (bytes >= 1024ULL * 1024) {
        snprintf(buf, n, "%.1f MB", (double)bytes / (1024.0 * 1024));
    } else {
        snprintf(buf, n, "%.0f KB", (double)bytes / 1024.0);
    }
}
}  // namespace

// ── Ui::init ─────────────────────────────────────────────────────────────────

esp_err_t Ui::init(
    Display &display, BaseStation &station,
    SdManager &sd, WifiManager &wifi, Storage &storage) {
    station_ = &station;
    sd_      = &sd;
    wifi_    = &wifi;
    storage_ = &storage;

    // BSP has already initialised LVGL, registered the GT911 touch device,
    // allocated framebuffers, and started the LVGL port task.
    // Lock before touching any LVGL objects.
    bsp_display_lock(0);

    build_screens(display.handle());

    // 1-second refresh timer — fired by the BSP's LVGL port task.
    lv_timer_create(refresh_timer_cb, 1000, this);

    bsp_display_unlock();

    ESP_LOGI(kTag, "UI ready");
    return ESP_OK;
}

// ── Screen construction ───────────────────────────────────────────────────────

static lv_obj_t *make_row(lv_obj_t *parent, lv_obj_t **val_out, const char *key) {
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_ver(row, 6, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(
        row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
        LV_FLEX_ALIGN_CENTER);

    lv_obj_t *k = lv_label_create(row);
    lv_label_set_text(k, key);
    lv_obj_set_style_text_color(k, lv_color_hex(0x6a8fa8), 0);

    *val_out = lv_label_create(row);
    lv_label_set_text(*val_out, "\xe2\x80\x94");  // em dash placeholder
    lv_obj_set_style_text_align(*val_out, LV_TEXT_ALIGN_RIGHT, 0);

    return row;
}

static void style_tab(lv_obj_t *tab) {
    lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(
        tab, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(tab, 18, 0);
    lv_obj_set_style_pad_row(tab, 0, 0);
    lv_obj_set_scroll_dir(tab, LV_DIR_NONE);
    lv_obj_set_style_bg_color(tab, lv_color_hex(0x0d1b2a), 0);
}

void Ui::build_screens(lv_display_t *disp) {
    lv_theme_t *theme = lv_theme_default_init(
        disp,
        lv_palette_main(LV_PALETTE_BLUE),
        lv_palette_main(LV_PALETTE_BLUE_GREY),
        true,
        LV_FONT_DEFAULT);
    lv_display_set_theme(disp, theme);

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0d1b2a), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    lv_obj_t *tv = lv_tabview_create(scr);
    lv_obj_set_size(tv, Display::kWidth, Display::kHeight);
    lv_obj_set_pos(tv, 0, 0);
    lv_tabview_set_tab_bar_position(tv, LV_DIR_BOTTOM);
    lv_tabview_set_tab_bar_size(tv, 52);
    lv_obj_set_style_bg_color(tv, lv_color_hex(0x0d1b2a), 0);

    tab_status_ = lv_tabview_add_tab(tv, LV_SYMBOL_HOME " Status");
    tab_ntrip_  = lv_tabview_add_tab(tv, LV_SYMBOL_WIFI " NTRIP");
    tab_pos_    = lv_tabview_add_tab(tv, LV_SYMBOL_GPS " Position");
    tab_sys_    = lv_tabview_add_tab(tv, LV_SYMBOL_SETTINGS " System");

    style_tab(tab_status_);
    style_tab(tab_ntrip_);
    style_tab(tab_pos_);
    style_tab(tab_sys_);

    build_status_tab(tab_status_);
    build_ntrip_tab(tab_ntrip_);
    build_position_tab(tab_pos_);
    build_system_tab(tab_sys_);
}

void Ui::build_status_tab(lv_obj_t *parent) {
    make_row(parent, &lbl_mode_,  "Mode");
    make_row(parent, &lbl_rtcm_,  "RTCM rate");
    make_row(parent, &lbl_sats_,  "Satellites");
    make_row(parent, &lbl_local_, "Local clients");
}

void Ui::build_ntrip_tab(lv_obj_t *parent) {
    make_row(parent, &lbl_rtk2go_,      "RTK2go");
    make_row(parent, &lbl_onocoy_,      "Onocoy");
    make_row(parent, &lbl_rtkdata_,     "RTKdata");
    make_row(parent, &lbl_local_ntrip_, "Local (2101)");
}

void Ui::build_position_tab(lv_obj_t *parent) {
    lbl_pos_ = lv_label_create(parent);
    lv_label_set_text(lbl_pos_, "Lat:    —\nLon:    —\nHeight: —");
    lv_obj_set_style_text_line_space(lbl_pos_, 6, 0);

    lbl_survey_ = lv_label_create(parent);
    lv_label_set_text(lbl_survey_, "");
    lv_obj_set_style_pad_top(lbl_survey_, 18, 0);

    bar_survey_ = lv_bar_create(parent);
    lv_obj_set_size(bar_survey_, LV_PCT(100), 18);
    lv_obj_set_style_pad_top(bar_survey_, 8, 0);
    lv_bar_set_range(bar_survey_, 0, 300);
    lv_bar_set_value(bar_survey_, 0, LV_ANIM_OFF);
    lv_obj_add_flag(bar_survey_, LV_OBJ_FLAG_HIDDEN);
}

void Ui::build_system_tab(lv_obj_t *parent) {
    make_row(parent, &lbl_ip_,    "IP address");
    make_row(parent, &lbl_wifi_,  "WiFi");
    make_row(parent, &lbl_sd_,    "SD card");
    make_row(parent, &lbl_rinex_, "RINEX");
    make_row(parent, &lbl_fw_,    "Firmware");
}

// ── Periodic refresh ─────────────────────────────────────────────────────────

void Ui::refresh_timer_cb(lv_timer_t *timer) {
    static_cast<Ui *>(lv_timer_get_user_data(timer))->refresh();
}

void Ui::refresh() {
    char buf[160];
    const BaseStationStatus st = station_->status();
    const RinexLogger::Status rx = station_->rinex_status();

    // ── Status tab ──────────────────────────────────────────────────────────
    if (st.mode == BaseMode::kSurvey) {
        lv_label_set_text(lbl_mode_, "SURVEY");
        lv_obj_set_style_text_color(
            lbl_mode_, lv_palette_main(LV_PALETTE_ORANGE), 0);
    } else {
        lv_label_set_text(lbl_mode_, "BASE TX");
        lv_obj_set_style_text_color(
            lbl_mode_, lv_palette_main(LV_PALETTE_GREEN), 0);
    }

    if (st.mode == BaseMode::kTransmit && st.rtcm_bytes_per_second > 0) {
        snprintf(buf, sizeof(buf), "%lu B/s",
                 static_cast<unsigned long>(st.rtcm_bytes_per_second));
    } else {
        snprintf(buf, sizeof(buf), "—");
    }
    lv_label_set_text(lbl_rtcm_, buf);

    const SurveySnapshot &sv = st.survey;
    snprintf(buf, sizeof(buf), "G:%d R:%d E:%d C:%d  (%d total)",
             sv.gps, sv.glonass, sv.galileo, sv.beidou, sv.satellites_tracked);
    lv_label_set_text(lbl_sats_, buf);

    snprintf(buf, sizeof(buf), "%d", st.local_clients);
    lv_label_set_text(lbl_local_, buf);

    // ── NTRIP tab ───────────────────────────────────────────────────────────
    auto fmt_ntrip = [&](lv_obj_t *lbl, const NtripStatus &ns) {
        if (!ns.enabled) {
            lv_label_set_text(lbl, "Disabled");
            lv_obj_set_style_text_color(lbl, lv_color_hex(0x5a7080), 0);
        } else if (ns.connected) {
            char kb[16] = "";
            fmt_bytes(kb, sizeof(kb), ns.bytes_sent);
            snprintf(buf, sizeof(buf), "Connected  %s", kb);
            lv_label_set_text(lbl, buf);
            lv_obj_set_style_text_color(
                lbl, lv_palette_main(LV_PALETTE_GREEN), 0);
        } else {
            const char *msg = ns.message.empty()
                              ? "Connecting…"
                              : ns.message.c_str();
            lv_label_set_text(lbl, msg);
            lv_obj_set_style_text_color(
                lbl, lv_palette_main(LV_PALETTE_RED), 0);
        }
    };
    fmt_ntrip(lbl_rtk2go_,  st.rtk2go);
    fmt_ntrip(lbl_onocoy_,  st.onocoy);
    fmt_ntrip(lbl_rtkdata_, st.rtkdata);

    snprintf(buf, sizeof(buf), "%d client%s",
             st.local_clients, st.local_clients == 1 ? "" : "s");
    lv_label_set_text(lbl_local_ntrip_, buf);

    // ── Position tab ────────────────────────────────────────────────────────
    const BasePosition bpos = storage_->load_position();
    if (bpos.valid) {
        snprintf(buf, sizeof(buf),
                 "Lat:    %.7f\xc2\xb0\n"
                 "Lon:    %.7f\xc2\xb0\n"
                 "Height: %.3f m",
                 bpos.lat, bpos.lon, bpos.height);
    } else {
        snprintf(buf, sizeof(buf), "No position stored");
    }
    lv_label_set_text(lbl_pos_, buf);

    if (st.mode == BaseMode::kSurvey) {
        snprintf(buf, sizeof(buf),
                 "Surveying — %lus  blocks:%d  stab:%.2fm",
                 static_cast<unsigned long>(sv.elapsed_sec), sv.blocks,
                 static_cast<double>(sv.stability));
        lv_label_set_text(lbl_survey_, buf);
        lv_obj_clear_flag(bar_survey_, LV_OBJ_FLAG_HIDDEN);
        lv_bar_set_value(
            bar_survey_,
            static_cast<int>(std::min<uint32_t>(sv.elapsed_sec, 300)),
            LV_ANIM_OFF);
    } else {
        lv_label_set_text(lbl_survey_, "");
        lv_obj_add_flag(bar_survey_, LV_OBJ_FLAG_HIDDEN);
    }

    // ── System tab ──────────────────────────────────────────────────────────
    const std::string ip = wifi_->ip_address();
    lv_label_set_text(lbl_ip_, ip.empty() ? "AP mode" : ip.c_str());

    if (wifi_->connected()) {
        snprintf(buf, sizeof(buf), "%s  %d dBm",
                 wifi_->ssid().c_str(), wifi_->rssi());
    } else {
        snprintf(buf, sizeof(buf), "Not connected");
    }
    lv_label_set_text(lbl_wifi_, buf);

    if (sd_->is_mounted()) {
        const SdManager::DiskStats stats = sd_->disk_stats();
        if (stats.valid) {
            char used[16], total[16];
            fmt_bytes(used,  sizeof(used),  stats.used_bytes);
            fmt_bytes(total, sizeof(total), stats.total_bytes);
            snprintf(buf, sizeof(buf), "%s / %s", used, total);
        } else {
            snprintf(buf, sizeof(buf), "Mounted");
        }
    } else {
        snprintf(buf, sizeof(buf), "Not mounted");
    }
    lv_label_set_text(lbl_sd_, buf);

    if (rx.active) {
        snprintf(buf, sizeof(buf), "Recording  %d epochs", rx.epochs);
        lv_obj_set_style_text_color(
            lbl_rinex_, lv_palette_main(LV_PALETTE_GREEN), 0);
    } else {
        snprintf(buf, sizeof(buf), "Off");
        lv_obj_set_style_text_color(lbl_rinex_, lv_color_hex(0x5a7080), 0);
    }
    lv_label_set_text(lbl_rinex_, buf);

    const esp_app_desc_t *app_desc = esp_app_get_description();
    lv_label_set_text(lbl_fw_, app_desc ? app_desc->version : "unknown");
}
