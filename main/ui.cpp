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

// Colour palette
constexpr uint32_t kBgScreen  = 0x0d1b2a;
constexpr uint32_t kBgGroup   = 0x152638;
constexpr uint32_t kBorderCol = 0x1f3d5c;
constexpr uint32_t kKeyCol    = 0x5a8098;
constexpr uint32_t kTitleCol  = 0x3d6480;
constexpr uint32_t kDimCol    = 0x3a5570;

void fmt_bytes(char *buf, size_t n, uint64_t bytes) {
    if (bytes >= 1024ULL * 1024 * 1024)
        snprintf(buf, n, "%.1f GB", (double)bytes / (1024.0 * 1024 * 1024));
    else if (bytes >= 1024ULL * 1024)
        snprintf(buf, n, "%.1f MB", (double)bytes / (1024.0 * 1024));
    else
        snprintf(buf, n, "%.0f KB", (double)bytes / 1024.0);
}
} // namespace

// ── Ui::init ─────────────────────────────────────────────────────────────────

esp_err_t Ui::init(
    Display &display, BaseStation &station,
    SdManager &sd, WifiManager &wifi, Storage &storage) {
    station_ = &station;
    sd_      = &sd;
    wifi_    = &wifi;
    storage_ = &storage;

    bsp_display_lock(0);
    build_screens(display.handle());
    lv_timer_create(refresh_timer_cb, 1000, this);
    bsp_display_unlock();

    ESP_LOGI(kTag, "UI ready");
    return ESP_OK;
}

// ── Helpers ───────────────────────────────────────────────────────────────────

// Titled group box — returns the inner content container.
lv_obj_t *Ui::make_group(lv_obj_t *parent, const char *title) {
    // Outer card
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, lv_color_hex(kBgGroup), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(kBorderCol), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 8, 0);
    lv_obj_set_style_pad_all(card, 12, 0);
    lv_obj_set_style_pad_row(card, 0, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                           LV_FLEX_ALIGN_START);

    // Title label
    lv_obj_t *hdr = lv_label_create(card);
    lv_label_set_text(hdr, title);
    lv_obj_set_style_text_color(hdr, lv_color_hex(kTitleCol), 0);
    lv_obj_set_style_text_font(hdr, &lv_font_montserrat_14, 0);
    lv_obj_set_style_pad_bottom(hdr, 8, 0);

    // Divider
    lv_obj_t *div = lv_obj_create(card);
    lv_obj_remove_style_all(div);
    lv_obj_set_size(div, LV_PCT(100), 1);
    lv_obj_set_style_bg_color(div, lv_color_hex(kBorderCol), 0);
    lv_obj_set_style_bg_opa(div, LV_OPA_COVER, 0);
    lv_obj_set_style_margin_bottom(div, 6, 0);

    return card;
}

// Row with a fixed-width key and an adjacent value — key and value sit close
// together rather than being pushed to opposite edges of the container.
lv_obj_t *Ui::make_row(lv_obj_t *parent, lv_obj_t **val_out, const char *key) {
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_ver(row, 5, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                           LV_FLEX_ALIGN_CENTER);

    lv_obj_t *k = lv_label_create(row);
    lv_label_set_text(k, key);
    lv_obj_set_style_text_color(k, lv_color_hex(kKeyCol), 0);
    lv_obj_set_width(k, 148);   // fixed key column — value starts at 148 px

    *val_out = lv_label_create(row);
    lv_label_set_text(*val_out, "\xe2\x80\x94");  // em dash placeholder
    lv_obj_set_flex_grow(*val_out, 1);

    return row;
}

// Shared NTRIP label formatter used by both Status and NTRIP detail tabs.
void Ui::fmt_ntrip_label(lv_obj_t *lbl, const NtripStatus &ns,
                          char *buf, size_t buf_len) {
    if (!ns.enabled) {
        lv_label_set_text(lbl, "Disabled");
        lv_obj_set_style_text_color(lbl, lv_color_hex(kDimCol), 0);
    } else if (ns.connected) {
        char kb[16] = "";
        fmt_bytes(kb, sizeof(kb), ns.bytes_sent);
        snprintf(buf, buf_len, "Connected  %s", kb);
        lv_label_set_text(lbl, buf);
        lv_obj_set_style_text_color(lbl, lv_palette_main(LV_PALETTE_GREEN), 0);
    } else {
        const char *msg = ns.message.empty() ? "Connecting\xe2\x80\xa6"
                                              : ns.message.c_str();
        lv_label_set_text(lbl, msg);
        lv_obj_set_style_text_color(lbl, lv_palette_main(LV_PALETTE_RED), 0);
    }
}

// ── Screen construction ───────────────────────────────────────────────────────

static void style_tab(lv_obj_t *tab) {
    lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(tab, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                           LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(tab, 14, 0);
    lv_obj_set_style_pad_row(tab, 10, 0);  // gap between group boxes
    lv_obj_set_scroll_dir(tab, LV_DIR_VER);
    lv_obj_set_style_bg_color(tab, lv_color_hex(kBgScreen), 0);
}

void Ui::build_screens(lv_display_t *disp) {
    lv_theme_t *theme = lv_theme_default_init(
        disp,
        lv_palette_main(LV_PALETTE_BLUE),
        lv_palette_main(LV_PALETTE_BLUE_GREY),
        true, LV_FONT_DEFAULT);
    lv_display_set_theme(disp, theme);

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(kBgScreen), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    lv_obj_t *tv = lv_tabview_create(scr);
    lv_obj_set_size(tv, Display::kWidth, Display::kHeight);
    lv_obj_set_pos(tv, 0, 0);
    lv_tabview_set_tab_bar_position(tv, LV_DIR_BOTTOM);
    lv_tabview_set_tab_bar_size(tv, 52);
    lv_obj_set_style_bg_color(tv, lv_color_hex(kBgScreen), 0);

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
    // ── Base Operation group ─────────────────────────────────────────────────
    lv_obj_t *g_op = make_group(parent, "BASE OPERATION");
    make_row(g_op, &lbl_mode_, "Mode");
    make_row(g_op, &lbl_rtcm_, "RTCM output");
    make_row(g_op, &lbl_sats_, "Satellites");

    // ── Position group ───────────────────────────────────────────────────────
    lv_obj_t *g_pos = make_group(parent, "POSITION");
    make_row(g_pos, &lbl_lat_, "Latitude");
    make_row(g_pos, &lbl_lon_, "Longitude");
    make_row(g_pos, &lbl_alt_, "Height");

    // Survey-in-progress row (shown only while surveying)
    lbl_survey_ = lv_label_create(g_pos);
    lv_label_set_text(lbl_survey_, "");
    lv_obj_set_style_text_color(lbl_survey_, lv_palette_main(LV_PALETTE_ORANGE), 0);
    lv_obj_set_style_text_font(lbl_survey_, &lv_font_montserrat_14, 0);
    lv_obj_set_style_pad_top(lbl_survey_, 4, 0);

    bar_survey_ = lv_bar_create(g_pos);
    lv_obj_set_size(bar_survey_, LV_PCT(100), 10);
    lv_obj_set_style_margin_top(bar_survey_, 6, 0);
    lv_bar_set_range(bar_survey_, 0, 300);
    lv_bar_set_value(bar_survey_, 0, LV_ANIM_OFF);
    lv_obj_add_flag(bar_survey_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lbl_survey_, LV_OBJ_FLAG_HIDDEN);

    // ── NTRIP Casters group ──────────────────────────────────────────────────
    lv_obj_t *g_ntrip = make_group(parent, "NTRIP CASTERS");
    make_row(g_ntrip, &lbl_rtk2go_,      "RTK2go");
    make_row(g_ntrip, &lbl_onocoy_,      "Onocoy");
    make_row(g_ntrip, &lbl_rtkdata_,     "RTKdata");
    make_row(g_ntrip, &lbl_local_ntrip_, "Local :2101");
}

void Ui::build_ntrip_tab(lv_obj_t *parent) {
    lv_obj_t *g = make_group(parent, "CASTER STATUS");
    make_row(g, &lbl_d_rtk2go_,      "RTK2go");
    make_row(g, &lbl_d_onocoy_,      "Onocoy");
    make_row(g, &lbl_d_rtkdata_,     "RTKdata");
    make_row(g, &lbl_d_local_ntrip_, "Local :2101");
}

void Ui::build_position_tab(lv_obj_t *parent) {
    lv_obj_t *g = make_group(parent, "FIXED BASE POSITION");
    make_row(g, &lbl_d_lat_, "Latitude");
    make_row(g, &lbl_d_lon_, "Longitude");
    make_row(g, &lbl_d_alt_, "Height");

    lbl_d_survey_ = lv_label_create(g);
    lv_label_set_text(lbl_d_survey_, "");
    lv_obj_set_style_text_color(lbl_d_survey_,
                                 lv_palette_main(LV_PALETTE_ORANGE), 0);
    lv_obj_set_style_text_font(lbl_d_survey_, &lv_font_montserrat_14, 0);
    lv_obj_set_style_pad_top(lbl_d_survey_, 4, 0);
    lv_obj_add_flag(lbl_d_survey_, LV_OBJ_FLAG_HIDDEN);

    bar_d_survey_ = lv_bar_create(g);
    lv_obj_set_size(bar_d_survey_, LV_PCT(100), 10);
    lv_obj_set_style_margin_top(bar_d_survey_, 6, 0);
    lv_bar_set_range(bar_d_survey_, 0, 300);
    lv_bar_set_value(bar_d_survey_, 0, LV_ANIM_OFF);
    lv_obj_add_flag(bar_d_survey_, LV_OBJ_FLAG_HIDDEN);
}

void Ui::build_system_tab(lv_obj_t *parent) {
    lv_obj_t *g_net = make_group(parent, "NETWORK");
    make_row(g_net, &lbl_ip_,   "IP address");
    make_row(g_net, &lbl_wifi_, "WiFi");

    lv_obj_t *g_stor = make_group(parent, "STORAGE");
    make_row(g_stor, &lbl_sd_,    "SD card");
    make_row(g_stor, &lbl_rinex_, "RINEX");

    lv_obj_t *g_fw = make_group(parent, "FIRMWARE");
    make_row(g_fw, &lbl_fw_, "Version");
}

// ── Periodic refresh ─────────────────────────────────────────────────────────

void Ui::refresh_timer_cb(lv_timer_t *timer) {
    static_cast<Ui *>(lv_timer_get_user_data(timer))->refresh();
}

void Ui::refresh() {
    char buf[160];
    const BaseStationStatus st = station_->status();
    const RinexLogger::Status rx = station_->rinex_status();
    const SurveySnapshot &sv = st.survey;

    // ── Status tab — Base Operation ─────────────────────────────────────────
    if (st.mode == BaseMode::kSurvey) {
        lv_label_set_text(lbl_mode_, "SURVEY");
        lv_obj_set_style_text_color(lbl_mode_,
            lv_palette_main(LV_PALETTE_ORANGE), 0);
    } else {
        lv_label_set_text(lbl_mode_, "BASE TX");
        lv_obj_set_style_text_color(lbl_mode_,
            lv_palette_main(LV_PALETTE_GREEN), 0);
    }

    if (st.mode == BaseMode::kTransmit && st.rtcm_bytes_per_second > 0) {
        snprintf(buf, sizeof(buf), "%lu B/s",
                 static_cast<unsigned long>(st.rtcm_bytes_per_second));
    } else {
        snprintf(buf, sizeof(buf), "\xe2\x80\x94");
    }
    lv_label_set_text(lbl_rtcm_, buf);

    snprintf(buf, sizeof(buf), "G:%d  R:%d  E:%d  C:%d  (%d)",
             sv.gps, sv.glonass, sv.galileo, sv.beidou, sv.satellites_tracked);
    lv_label_set_text(lbl_sats_, buf);

    // ── Status tab — Position ────────────────────────────────────────────────
    const BasePosition bpos = storage_->load_position();
    if (bpos.valid) {
        snprintf(buf, sizeof(buf), "%.7f\xc2\xb0", bpos.lat);
        lv_label_set_text(lbl_lat_, buf);
        snprintf(buf, sizeof(buf), "%.7f\xc2\xb0", bpos.lon);
        lv_label_set_text(lbl_lon_, buf);
        snprintf(buf, sizeof(buf), "%.3f m", bpos.height);
        lv_label_set_text(lbl_alt_, buf);

        // Also update detail tab
        snprintf(buf, sizeof(buf), "%.7f\xc2\xb0", bpos.lat);
        lv_label_set_text(lbl_d_lat_, buf);
        snprintf(buf, sizeof(buf), "%.7f\xc2\xb0", bpos.lon);
        lv_label_set_text(lbl_d_lon_, buf);
        snprintf(buf, sizeof(buf), "%.3f m", bpos.height);
        lv_label_set_text(lbl_d_alt_, buf);
    } else {
        lv_label_set_text(lbl_lat_, "—");
        lv_label_set_text(lbl_lon_, "—");
        lv_label_set_text(lbl_alt_, "No position stored");
        lv_label_set_text(lbl_d_lat_, "—");
        lv_label_set_text(lbl_d_lon_, "—");
        lv_label_set_text(lbl_d_alt_, "No position stored");
    }

    if (st.mode == BaseMode::kSurvey) {
        snprintf(buf, sizeof(buf),
                 "Survey  %lus  blocks:%d  \xc2\xb1%.2fm",
                 static_cast<unsigned long>(sv.elapsed_sec),
                 sv.blocks,
                 static_cast<double>(sv.stability));
        lv_label_set_text(lbl_survey_, buf);
        lv_label_set_text(lbl_d_survey_, buf);
        lv_obj_clear_flag(lbl_survey_,   LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(bar_survey_,   LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(lbl_d_survey_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(bar_d_survey_, LV_OBJ_FLAG_HIDDEN);
        int32_t prog = static_cast<int32_t>(
            std::min<uint32_t>(sv.elapsed_sec, 300));
        lv_bar_set_value(bar_survey_,   prog, LV_ANIM_OFF);
        lv_bar_set_value(bar_d_survey_, prog, LV_ANIM_OFF);
    } else {
        lv_label_set_text(lbl_survey_,   "");
        lv_label_set_text(lbl_d_survey_, "");
        lv_obj_add_flag(lbl_survey_,   LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(bar_survey_,   LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lbl_d_survey_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(bar_d_survey_, LV_OBJ_FLAG_HIDDEN);
    }

    // ── Status tab — NTRIP ──────────────────────────────────────────────────
    fmt_ntrip_label(lbl_rtk2go_,  st.rtk2go,  buf, sizeof(buf));
    fmt_ntrip_label(lbl_onocoy_,  st.onocoy,  buf, sizeof(buf));
    fmt_ntrip_label(lbl_rtkdata_, st.rtkdata, buf, sizeof(buf));

    snprintf(buf, sizeof(buf), "%d client%s",
             st.local_clients, st.local_clients == 1 ? "" : "s");
    lv_label_set_text(lbl_local_ntrip_, buf);
    lv_obj_set_style_text_color(lbl_local_ntrip_,
        st.local_clients > 0
            ? lv_palette_main(LV_PALETTE_GREEN)
            : lv_color_hex(kDimCol), 0);

    // ── NTRIP detail tab ─────────────────────────────────────────────────────
    fmt_ntrip_label(lbl_d_rtk2go_,  st.rtk2go,  buf, sizeof(buf));
    fmt_ntrip_label(lbl_d_onocoy_,  st.onocoy,  buf, sizeof(buf));
    fmt_ntrip_label(lbl_d_rtkdata_, st.rtkdata, buf, sizeof(buf));
    snprintf(buf, sizeof(buf), "%d client%s",
             st.local_clients, st.local_clients == 1 ? "" : "s");
    lv_label_set_text(lbl_d_local_ntrip_, buf);
    lv_obj_set_style_text_color(lbl_d_local_ntrip_,
        st.local_clients > 0
            ? lv_palette_main(LV_PALETTE_GREEN)
            : lv_color_hex(kDimCol), 0);

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
        lv_obj_set_style_text_color(lbl_rinex_,
            lv_palette_main(LV_PALETTE_GREEN), 0);
    } else {
        snprintf(buf, sizeof(buf), "Off");
        lv_obj_set_style_text_color(lbl_rinex_, lv_color_hex(kDimCol), 0);
    }
    lv_label_set_text(lbl_rinex_, buf);

    const esp_app_desc_t *app_desc = esp_app_get_description();
    lv_label_set_text(lbl_fw_, app_desc ? app_desc->version : "unknown");
}
