#include "ui.hpp"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>

#include "bsp/esp-bsp.h"
#include "display.hpp"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_hosted.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace {

constexpr char kTag[] = "ui";

// Colour palette
constexpr uint32_t kBgScreen  = 0x0d1b2a;
constexpr uint32_t kBgGroup   = 0x152638;
constexpr uint32_t kBorderCol = 0x1f3d5c;
constexpr uint32_t kKeyCol    = 0x5a8098;
constexpr uint32_t kTitleCol  = 0x3d6480;
constexpr uint32_t kDimCol    = 0x3a5570;

// ── Debug log ring buffer ─────────────────────────────────────────────────────

constexpr int kLogLines = 60;
constexpr int kLogWidth = 100;
static char     log_buf[kLogLines][kLogWidth];
static int      log_head  = 0;
static int      log_count = 0;
static SemaphoreHandle_t log_sem = nullptr;

static int ui_log_vprintf(const char *fmt, va_list args) {
    va_list copy;
    va_copy(copy, args);
    char tmp[kLogWidth];
    vsnprintf(tmp, sizeof(tmp), fmt, copy);
    va_end(copy);
    // strip trailing newline/CR
    int len = static_cast<int>(strlen(tmp));
    while (len > 0 && (tmp[len - 1] == '\n' || tmp[len - 1] == '\r')) {
        tmp[--len] = '\0';
    }
    if (log_sem && xSemaphoreTake(log_sem, 0) == pdTRUE) {
        strlcpy(log_buf[log_head], tmp, kLogWidth);
        log_head = (log_head + 1) % kLogLines;
        if (log_count < kLogLines) log_count++;
        xSemaphoreGive(log_sem);
    }
    return vprintf(fmt, args);
}

} // namespace

// ── Ui::init ──────────────────────────────────────────────────────────────────

esp_err_t Ui::init(
    Display &display, BaseStation &station,
    SdManager &sd, WifiManager &wifi, Storage &storage) {
    station_ = &station;
    sd_      = &sd;
    wifi_    = &wifi;
    storage_ = &storage;

    log_sem = xSemaphoreCreateMutex();

    bsp_display_lock(0);
    build_screens(display.handle());
    lv_timer_create(refresh_timer_cb, 1000, this);
    bsp_display_unlock();

    // The embedded C6 image version is local to parse (no SDIO RPC).
    load_c6_available_version();
    // Query the *running* C6 version off the LVGL task — it's an RPC over SDIO.
    xTaskCreate(c6_version_task, "c6_ver", 4096, this, 3, nullptr);

    // Install log hook after display init so the build_screens path
    // doesn't go through our hook (LVGL creates many objects, each may log).
    esp_log_set_vprintf(ui_log_vprintf);
    ESP_LOGI(kTag, "UI ready");
    return ESP_OK;
}

// ── Static helpers ─────────────────────────────────────────────────────────────

void Ui::fmt_bytes_str(char *buf, size_t n, uint64_t bytes) {
    if (bytes >= 1024ULL * 1024 * 1024)
        snprintf(buf, n, "%.1f GB", (double)bytes / (1024.0 * 1024 * 1024));
    else if (bytes >= 1024ULL * 1024)
        snprintf(buf, n, "%.1f MB", (double)bytes / (1024.0 * 1024));
    else if (bytes >= 1024)
        snprintf(buf, n, "%.0f KB", (double)bytes / 1024.0);
    else
        snprintf(buf, n, "%llu B", (unsigned long long)bytes);
}

// Titled group box – returns the card container (add rows directly to it).
lv_obj_t *Ui::make_group(lv_obj_t *parent, const char *title) {
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

    lv_obj_t *hdr = lv_label_create(card);
    lv_label_set_text(hdr, title);
    lv_obj_set_style_text_color(hdr, lv_color_hex(kTitleCol), 0);
    lv_obj_set_style_text_font(hdr, &lv_font_montserrat_14, 0);
    lv_obj_set_style_pad_bottom(hdr, 6, 0);

    lv_obj_t *div = lv_obj_create(card);
    lv_obj_remove_style_all(div);
    lv_obj_set_size(div, LV_PCT(100), 1);
    lv_obj_set_style_bg_color(div, lv_color_hex(kBorderCol), 0);
    lv_obj_set_style_bg_opa(div, LV_OPA_COVER, 0);
    lv_obj_set_style_margin_bottom(div, 6, 0);

    return card;
}

// Row with fixed 148 px key label + flex value label.
lv_obj_t *Ui::make_row(lv_obj_t *parent, lv_obj_t **val_out,
                        const char *key) {
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_ver(row, 4, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                           LV_FLEX_ALIGN_CENTER);

    lv_obj_t *k = lv_label_create(row);
    lv_label_set_text(k, key);
    lv_obj_set_style_text_color(k, lv_color_hex(kKeyCol), 0);
    lv_obj_set_width(k, 148);

    *val_out = lv_label_create(row);
    lv_label_set_text(*val_out, "\xe2\x80\x94");
    lv_obj_set_flex_grow(*val_out, 1);
    return row;
}

// Row with label on left and switch on right.
lv_obj_t *Ui::make_switch_row(lv_obj_t *parent, lv_obj_t **sw_out,
                                const char *key) {
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_ver(row, 6, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                           LV_FLEX_ALIGN_CENTER);

    lv_obj_t *k = lv_label_create(row);
    lv_label_set_text(k, key);
    lv_obj_set_style_text_color(k, lv_color_hex(kKeyCol), 0);
    lv_obj_set_flex_grow(k, 1);

    *sw_out = lv_switch_create(row);
    return row;
}

// Full-screen modal base on lv_layer_top() – title bar with close button.
// Returns the content container (add children to it).
lv_obj_t *Ui::make_modal_base(lv_obj_t *parent, const char *title) {
    lv_obj_t *modal = lv_obj_create(parent);
    lv_obj_set_size(modal, Display::kWidth, Display::kHeight);
    lv_obj_set_pos(modal, 0, 0);
    lv_obj_set_style_bg_color(modal, lv_color_hex(kBgScreen), 0);
    lv_obj_set_style_bg_opa(modal, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(modal, 0, 0);
    lv_obj_set_style_border_width(modal, 0, 0);
    lv_obj_set_flex_flow(modal, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(modal, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                           LV_FLEX_ALIGN_START);
    lv_obj_add_flag(modal, LV_OBJ_FLAG_HIDDEN);

    // Title bar
    lv_obj_t *bar = lv_obj_create(modal);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, LV_PCT(100), 52);
    lv_obj_set_style_bg_color(bar, lv_color_hex(kBgGroup), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(bar, 16, 0);
    lv_obj_set_style_pad_ver(bar, 0, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                           LV_FLEX_ALIGN_CENTER);

    lv_obj_t *lbl = lv_label_create(bar);
    lv_label_set_text(lbl, title);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_flex_grow(lbl, 1);

    lv_obj_t *close_btn = lv_button_create(bar);
    lv_obj_set_size(close_btn, 40, 40);
    lv_obj_t *close_lbl = lv_label_create(close_btn);
    lv_label_set_text(close_lbl, LV_SYMBOL_CLOSE);
    lv_obj_center(close_lbl);

    // Divider
    lv_obj_t *div = lv_obj_create(modal);
    lv_obj_remove_style_all(div);
    lv_obj_set_size(div, LV_PCT(100), 1);
    lv_obj_set_style_bg_color(div, lv_color_hex(kBorderCol), 0);
    lv_obj_set_style_bg_opa(div, LV_OPA_COVER, 0);

    return modal;
}

void Ui::fmt_ntrip_label(lv_obj_t *lbl, const NtripStatus &ns,
                          char *buf, size_t buf_len) {
    if (!ns.enabled) {
        lv_label_set_text(lbl, "Disabled");
        lv_obj_set_style_text_color(lbl, lv_color_hex(kDimCol), 0);
    } else if (ns.connected) {
        char kb[20] = "";
        fmt_bytes_str(kb, sizeof(kb), ns.bytes_sent);
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

void Ui::on_ta_focused(lv_event_t *e) {
    lv_obj_t *kb = static_cast<lv_obj_t *>(lv_event_get_user_data(e));
    lv_obj_t *ta = lv_event_get_target_obj(e);
    lv_keyboard_set_textarea(kb, ta);
    lv_obj_clear_flag(kb, LV_OBJ_FLAG_HIDDEN);
}

void Ui::on_ta_defocused(lv_event_t *e) {
    lv_obj_t *kb = static_cast<lv_obj_t *>(lv_event_get_user_data(e));
    lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
}

void Ui::on_kb_ready(lv_event_t *e) {
    lv_obj_t *kb = lv_event_get_target_obj(e);
    lv_obj_t *ta = lv_keyboard_get_textarea(kb);
    if (ta) lv_obj_send_event(ta, LV_EVENT_DEFOCUSED, nullptr);
    lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
}

// ── Screen construction ────────────────────────────────────────────────────────

static void style_tab(lv_obj_t *tab) {
    lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(tab, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                           LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(tab, 14, 0);
    lv_obj_set_style_pad_row(tab, 10, 0);
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
    tab_debug_  = lv_tabview_add_tab(tv, LV_SYMBOL_LIST " Debug");

    style_tab(tab_status_);
    style_tab(tab_ntrip_);
    style_tab(tab_pos_);
    style_tab(tab_sys_);
    style_tab(tab_debug_);

    build_status_tab(tab_status_);
    build_ntrip_tab(tab_ntrip_);
    build_position_tab(tab_pos_);
    build_system_tab(tab_sys_);
    build_debug_tab(tab_debug_);
    // Modals are built lazily on first open to keep init fast.
}

// ── Status tab ────────────────────────────────────────────────────────────────

void Ui::build_status_tab(lv_obj_t *parent) {
    lv_obj_t *g_op = make_group(parent, "BASE OPERATION");
    make_row(g_op, &lbl_mode_, "Mode");
    make_row(g_op, &lbl_rtcm_, "RTCM output");
    make_row(g_op, &lbl_sats_, "Satellites");

    // Start Survey / Resurvey button
    btn_survey_start_ = lv_button_create(g_op);
    lv_obj_set_size(btn_survey_start_, LV_PCT(100), 40);
    lv_obj_set_style_margin_top(btn_survey_start_, 8, 0);
    lv_obj_set_style_bg_color(btn_survey_start_,
        lv_palette_darken(LV_PALETTE_ORANGE, 2), 0);
    lbl_survey_btn_ = lv_label_create(btn_survey_start_);
    lv_label_set_text(lbl_survey_btn_, LV_SYMBOL_REFRESH "  Start Survey");
    lv_obj_center(lbl_survey_btn_);
    lv_obj_add_event_cb(btn_survey_start_, on_survey_start, LV_EVENT_CLICKED, this);

    lv_obj_t *g_pos = make_group(parent, "POSITION");
    make_row(g_pos, &lbl_lat_, "Latitude");
    make_row(g_pos, &lbl_lon_, "Longitude");
    make_row(g_pos, &lbl_alt_, "Height");

    lbl_survey_ = lv_label_create(g_pos);
    lv_label_set_text(lbl_survey_, "");
    lv_obj_set_style_text_color(lbl_survey_, lv_palette_main(LV_PALETTE_ORANGE), 0);
    lv_obj_set_style_pad_top(lbl_survey_, 4, 0);

    bar_survey_ = lv_bar_create(g_pos);
    lv_obj_set_size(bar_survey_, LV_PCT(100), 10);
    lv_obj_set_style_margin_top(bar_survey_, 4, 0);
    lv_bar_set_range(bar_survey_, 0, 300);
    lv_bar_set_value(bar_survey_, 0, LV_ANIM_OFF);
    lv_obj_add_flag(bar_survey_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lbl_survey_, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *g_ntrip = make_group(parent, "NTRIP CASTERS");
    make_row(g_ntrip, &lbl_rtk2go_,      "RTK2go");
    make_row(g_ntrip, &lbl_onocoy_,      "Onocoy");
    make_row(g_ntrip, &lbl_rtkdata_,     "RTKdata");
    make_row(g_ntrip, &lbl_local_ntrip_, "Local :2101");
}

// ── NTRIP tab ─────────────────────────────────────────────────────────────────

static lv_obj_t *ntrip_detail_group(lv_obj_t *parent, const char *title,
    lv_obj_t **lbl_status, lv_obj_t **lbl_bytes, lv_obj_t **lbl_drop,
    Ui *ui, int svc_idx) {
    lv_obj_t *g = lv_obj_create(parent);
    lv_obj_remove_style_all(g);
    lv_obj_set_width(g, LV_PCT(100));
    lv_obj_set_height(g, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(g, lv_color_hex(0x152638), 0);
    lv_obj_set_style_bg_opa(g, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(g, lv_color_hex(0x1f3d5c), 0);
    lv_obj_set_style_border_width(g, 1, 0);
    lv_obj_set_style_border_opa(g, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(g, 8, 0);
    lv_obj_set_style_pad_all(g, 12, 0);
    lv_obj_set_flex_flow(g, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(g, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                           LV_FLEX_ALIGN_START);

    // Header row: title + configure button
    lv_obj_t *hdr_row = lv_obj_create(g);
    lv_obj_remove_style_all(hdr_row);
    lv_obj_set_size(hdr_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(hdr_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                           LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_bottom(hdr_row, 6, 0);

    lv_obj_t *hdr_lbl = lv_label_create(hdr_row);
    lv_label_set_text(hdr_lbl, title);
    lv_obj_set_style_text_color(hdr_lbl, lv_color_hex(0x3d6480), 0);
    lv_obj_set_style_text_font(hdr_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_flex_grow(hdr_lbl, 1);

    lv_obj_t *cfg_btn = lv_button_create(hdr_row);
    lv_obj_set_size(cfg_btn, 80, 32);
    lv_obj_t *cfg_lbl = lv_label_create(cfg_btn);
    lv_label_set_text(cfg_lbl, LV_SYMBOL_SETTINGS " Config");
    lv_obj_center(cfg_lbl);
    lv_obj_set_style_text_font(cfg_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_user_data(cfg_btn, (void *)(uintptr_t)svc_idx);
    lv_obj_add_event_cb(cfg_btn, Ui::on_ntrip_cfg_btn, LV_EVENT_CLICKED, ui);

    // Divider
    lv_obj_t *div = lv_obj_create(g);
    lv_obj_remove_style_all(div);
    lv_obj_set_size(div, LV_PCT(100), 1);
    lv_obj_set_style_bg_color(div, lv_color_hex(0x1f3d5c), 0);
    lv_obj_set_style_bg_opa(div, LV_OPA_COVER, 0);
    lv_obj_set_style_margin_bottom(div, 6, 0);

    // Status rows
    auto make_r = [&](lv_obj_t **out, const char *key) {
        lv_obj_t *row = lv_obj_create(g);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_pad_ver(row, 4, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                               LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_t *k = lv_label_create(row);
        lv_label_set_text(k, key);
        lv_obj_set_style_text_color(k, lv_color_hex(0x5a8098), 0);
        lv_obj_set_width(k, 148);
        *out = lv_label_create(row);
        lv_label_set_text(*out, "\xe2\x80\x94");
        lv_obj_set_flex_grow(*out, 1);
    };
    make_r(lbl_status, "Status");
    make_r(lbl_bytes,  "Bytes sent");
    make_r(lbl_drop,   "Dropped");
    return g;
}

void Ui::build_ntrip_tab(lv_obj_t *parent) {
    // Global enable/disable switch
    lv_obj_t *g_global = make_group(parent, "ALL NTRIP SERVICES");
    lv_obj_t *row = lv_obj_create(g_global);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                           LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *sw_lbl = lv_label_create(row);
    lv_label_set_text(sw_lbl, "Enable all services");
    lv_obj_set_style_text_color(sw_lbl, lv_color_hex(kKeyCol), 0);
    lv_obj_set_flex_grow(sw_lbl, 1);
    sw_ntrip_all_ = lv_switch_create(row);
    lv_obj_add_state(sw_ntrip_all_, LV_STATE_CHECKED);  // default: on
    lv_obj_add_event_cb(sw_ntrip_all_, on_ntrip_all_toggle, LV_EVENT_VALUE_CHANGED, this);

    ntrip_detail_group(parent, "RTK2go",
        &lbl_d_rtk2go_, &lbl_d_rtk2go_bytes_, &lbl_d_rtk2go_drop_,
        this, 0);
    ntrip_detail_group(parent, "Onocoy",
        &lbl_d_onocoy_, &lbl_d_onocoy_bytes_, &lbl_d_onocoy_drop_,
        this, 1);
    ntrip_detail_group(parent, "RTKdata",
        &lbl_d_rtkdata_, &lbl_d_rtkdata_bytes_, &lbl_d_rtkdata_drop_,
        this, 2);

    lv_obj_t *g_local = make_group(parent, "LOCAL CASTER :2101");
    make_row(g_local, &lbl_d_local_ntrip_, "Clients");
}

// ── Position tab ──────────────────────────────────────────────────────────────

void Ui::build_position_tab(lv_obj_t *parent) {
    lv_obj_t *g_pos = make_group(parent, "FIXED BASE POSITION");
    make_row(g_pos, &lbl_d_lat_, "Latitude");
    make_row(g_pos, &lbl_d_lon_, "Longitude");
    make_row(g_pos, &lbl_d_alt_, "Height");

    lbl_d_survey_ = lv_label_create(g_pos);
    lv_label_set_text(lbl_d_survey_, "");
    lv_obj_set_style_text_color(lbl_d_survey_, lv_palette_main(LV_PALETTE_ORANGE), 0);
    lv_obj_set_style_pad_top(lbl_d_survey_, 4, 0);
    lv_obj_add_flag(lbl_d_survey_, LV_OBJ_FLAG_HIDDEN);

    bar_d_survey_ = lv_bar_create(g_pos);
    lv_obj_set_size(bar_d_survey_, LV_PCT(100), 10);
    lv_obj_set_style_margin_top(bar_d_survey_, 4, 0);
    lv_bar_set_range(bar_d_survey_, 0, 300);
    lv_bar_set_value(bar_d_survey_, 0, LV_ANIM_OFF);
    lv_obj_add_flag(bar_d_survey_, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *g_sv = make_group(parent, "SURVEY QUALITY");
    make_row(g_sv, &lbl_sv_elapsed_,   "Elapsed");
    make_row(g_sv, &lbl_sv_blocks_,    "Blocks");
    make_row(g_sv, &lbl_sv_samples_,   "Samples");
    make_row(g_sv, &lbl_sv_stability_, "Stability \xc2\xb1");
    make_row(g_sv, &lbl_sv_sigma_,     "Inst. sigma");

    lv_obj_t *g_sats = make_group(parent, "SATELLITES");
    make_row(g_sats, &lbl_sv_used_, "Used / tracked");
    make_row(g_sats, &lbl_sv_gps_,  "GPS");
    make_row(g_sats, &lbl_sv_glo_,  "GLONASS");
    make_row(g_sats, &lbl_sv_gal_,  "Galileo");
    make_row(g_sats, &lbl_sv_bds_,  "BeiDou");

    // Per-satellite SNR summary label (multi-line, wraps)
    lbl_sv_detail_ = lv_label_create(g_sats);
    lv_label_set_long_mode(lbl_sv_detail_, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl_sv_detail_, LV_PCT(100));
    lv_obj_set_style_text_color(lbl_sv_detail_, lv_color_hex(kDimCol), 0);
    lv_obj_set_style_text_font(lbl_sv_detail_, &lv_font_montserrat_14, 0);
    lv_obj_set_style_pad_top(lbl_sv_detail_, 6, 0);
    lv_label_set_text(lbl_sv_detail_, "");
}

// ── System tab ────────────────────────────────────────────────────────────────

void Ui::build_system_tab(lv_obj_t *parent) {
    lv_obj_t *g_net = make_group(parent, "NETWORK");
    make_row(g_net, &lbl_wifi_state_, "Station");
    make_row(g_net, &lbl_ip_,         "Station IP");
    make_row(g_net, &lbl_ap_name_,    "Hotspot");
    make_row(g_net, &lbl_ap_ip_,      "Hotspot IP");

    lv_obj_t *wifi_btn = lv_button_create(g_net);
    lv_obj_set_size(wifi_btn, LV_PCT(100), 40);
    lv_obj_set_style_margin_top(wifi_btn, 8, 0);
    lv_obj_t *wifi_lbl = lv_label_create(wifi_btn);
    lv_label_set_text(wifi_lbl, LV_SYMBOL_WIFI "  Configure WiFi");
    lv_obj_center(wifi_lbl);
    lv_obj_add_event_cb(wifi_btn, on_wifi_btn, LV_EVENT_CLICKED, this);

    lv_obj_t *g_stor = make_group(parent, "STORAGE");
    make_row(g_stor, &lbl_sd_, "SD card");
    make_switch_row(g_stor, &sw_rinex_, "RINEX collection");
    make_row(g_stor, &lbl_rinex_file_, "Current file");
    lv_obj_add_event_cb(sw_rinex_, on_rinex_toggle, LV_EVENT_VALUE_CHANGED, this);

    lv_obj_t *files_btn = lv_button_create(g_stor);
    lv_obj_set_size(files_btn, LV_PCT(100), 40);
    lv_obj_set_style_margin_top(files_btn, 8, 0);
    lv_obj_t *files_lbl = lv_label_create(files_btn);
    lv_label_set_text(files_lbl, LV_SYMBOL_DIRECTORY "  Browse SD Card");
    lv_obj_center(files_lbl);
    lv_obj_add_event_cb(files_btn, on_files_btn, LV_EVENT_CLICKED, this);

    lv_obj_t *g_fw = make_group(parent, "FIRMWARE");
    make_row(g_fw, &lbl_fw_,         "Firmware");
    make_row(g_fw, &lbl_compile_,    "Built");
    make_row(g_fw, &lbl_c6_running_, "C6 running");
    make_row(g_fw, &lbl_c6_fw_,      "C6 available");

    btn_c6_ota_ = lv_button_create(g_fw);
    lv_obj_set_size(btn_c6_ota_, LV_PCT(100), 40);
    lv_obj_set_style_margin_top(btn_c6_ota_, 8, 0);
    lv_obj_t *c6_lbl = lv_label_create(btn_c6_ota_);
    lv_label_set_text(c6_lbl, LV_SYMBOL_UPLOAD "  Update C6 Firmware");
    lv_obj_center(c6_lbl);
    lv_obj_add_event_cb(btn_c6_ota_, on_c6_ota_btn, LV_EVENT_CLICKED, this);
}

// ── Debug tab ─────────────────────────────────────────────────────────────────

void Ui::build_debug_tab(lv_obj_t *parent) {
    lv_obj_t *cont = lv_obj_create(parent);
    lv_obj_set_size(cont, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(cont, lv_color_hex(kBgGroup), 0);
    lv_obj_set_style_bg_opa(cont, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(cont, 0, 0);
    lv_obj_set_style_pad_all(cont, 8, 0);

    lbl_debug_ = lv_label_create(cont);
    lv_label_set_long_mode(lbl_debug_, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl_debug_, LV_PCT(100));
    lv_obj_set_style_text_color(lbl_debug_, lv_color_hex(0x7aaa7a), 0);
    lv_obj_set_style_text_font(lbl_debug_, &lv_font_montserrat_14, 0);
    lv_label_set_text(lbl_debug_, "Log output will appear here...");
}

// ── WiFi modal ────────────────────────────────────────────────────────────────

void Ui::build_wifi_modal() {
    modal_wifi_ = make_modal_base(lv_layer_top(), "WiFi Setup");

    // Close button is the 3rd child of the title bar (which is child [0] of modal)
    lv_obj_t *bar = lv_obj_get_child(modal_wifi_, 0);
    lv_obj_t *close_btn = lv_obj_get_child(bar, lv_obj_get_child_count(bar) - 1);
    lv_obj_add_event_cb(close_btn, on_wifi_close, LV_EVENT_CLICKED, this);

    // Keyboard (hidden until textarea focused)
    kb_wifi_ = lv_keyboard_create(modal_wifi_);
    lv_obj_set_size(kb_wifi_, Display::kWidth, Display::kHeight * 40 / 100);
    lv_obj_set_style_margin_top(kb_wifi_, 0, 0);
    lv_obj_add_flag(kb_wifi_, LV_OBJ_FLAG_HIDDEN);
    // Put keyboard at end so it overlaps; we'll re-order by using a separate layout
    // Actually keyboard should be at the bottom; use absolute pos workaround:
    // → keyboard will be last flex child, so it stays at bottom
    lv_obj_add_event_cb(kb_wifi_, on_kb_ready, LV_EVENT_READY, nullptr);
    lv_obj_add_event_cb(kb_wifi_, on_kb_ready, LV_EVENT_CANCEL, nullptr);

    // Content container (above keyboard)
    lv_obj_t *cont = lv_obj_create(modal_wifi_);
    lv_obj_remove_style_all(cont);
    lv_obj_set_size(cont, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(cont, 16, 0);
    lv_obj_set_style_pad_ver(cont, 10, 0);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                           LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(cont, 8, 0);

    // SSID row
    lv_obj_t *lbl_ssid = lv_label_create(cont);
    lv_label_set_text(lbl_ssid, "Network SSID:");
    lv_obj_set_style_text_color(lbl_ssid, lv_color_hex(kKeyCol), 0);

    ta_wifi_ssid_ = lv_textarea_create(cont);
    lv_textarea_set_one_line(ta_wifi_ssid_, true);
    lv_obj_set_width(ta_wifi_ssid_, LV_PCT(100));
    lv_textarea_set_placeholder_text(ta_wifi_ssid_, "Network name");
    lv_obj_add_event_cb(ta_wifi_ssid_, on_ta_focused,   LV_EVENT_FOCUSED,   kb_wifi_);
    lv_obj_add_event_cb(ta_wifi_ssid_, on_ta_defocused, LV_EVENT_DEFOCUSED, kb_wifi_);

    // Password row
    lv_obj_t *lbl_pass = lv_label_create(cont);
    lv_label_set_text(lbl_pass, "Password:");
    lv_obj_set_style_text_color(lbl_pass, lv_color_hex(kKeyCol), 0);

    ta_wifi_pass_ = lv_textarea_create(cont);
    lv_textarea_set_one_line(ta_wifi_pass_, true);
    lv_textarea_set_password_mode(ta_wifi_pass_, true);
    lv_obj_set_width(ta_wifi_pass_, LV_PCT(100));
    lv_textarea_set_placeholder_text(ta_wifi_pass_, "Password");
    lv_obj_add_event_cb(ta_wifi_pass_, on_ta_focused,   LV_EVENT_FOCUSED,   kb_wifi_);
    lv_obj_add_event_cb(ta_wifi_pass_, on_ta_defocused, LV_EVENT_DEFOCUSED, kb_wifi_);

    // Buttons row
    lv_obj_t *btn_row = lv_obj_create(cont);
    lv_obj_remove_style_all(btn_row);
    lv_obj_set_size(btn_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                           LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    btn_wifi_scan_ = lv_button_create(btn_row);
    lv_obj_set_size(btn_wifi_scan_, 160, 40);
    lv_obj_t *scan_lbl = lv_label_create(btn_wifi_scan_);
    lv_label_set_text(scan_lbl, LV_SYMBOL_REFRESH "  Scan");
    lv_obj_center(scan_lbl);
    lv_obj_add_event_cb(btn_wifi_scan_, on_wifi_scan, LV_EVENT_CLICKED, this);

    lv_obj_t *conn_btn = lv_button_create(btn_row);
    lv_obj_set_size(conn_btn, 160, 40);
    lv_obj_t *conn_lbl = lv_label_create(conn_btn);
    lv_label_set_text(conn_lbl, LV_SYMBOL_WIFI "  Connect");
    lv_obj_center(conn_lbl);
    lv_obj_add_event_cb(conn_btn, on_wifi_connect, LV_EVENT_CLICKED, this);

    // Status message
    lbl_wifi_msg_ = lv_label_create(cont);
    lv_label_set_text(lbl_wifi_msg_, "");
    lv_obj_set_style_text_color(lbl_wifi_msg_, lv_color_hex(kDimCol), 0);

    // Scan results list
    lv_obj_t *scan_hdr = lv_label_create(cont);
    lv_label_set_text(scan_hdr, "Available networks:");
    lv_obj_set_style_text_color(scan_hdr, lv_color_hex(kKeyCol), 0);

    list_wifi_scan_ = lv_list_create(cont);
    lv_obj_set_size(list_wifi_scan_, LV_PCT(100), 160);
    lv_obj_set_style_bg_color(list_wifi_scan_, lv_color_hex(kBgGroup), 0);
    lv_obj_set_style_border_color(list_wifi_scan_, lv_color_hex(kBorderCol), 0);
    lv_list_add_text(list_wifi_scan_, "Tap Scan to find networks");
}

void Ui::open_wifi_modal() {
    if (!modal_wifi_) build_wifi_modal();
    const WifiCredentials saved = storage_->load_wifi();
    lv_textarea_set_text(ta_wifi_ssid_, saved.ssid.c_str());
    lv_textarea_set_text(ta_wifi_pass_, saved.password.c_str());
    lv_label_set_text(lbl_wifi_msg_, "");
    lv_obj_clear_flag(modal_wifi_, LV_OBJ_FLAG_HIDDEN);
}

void Ui::populate_wifi_scan_list(const std::vector<WifiNetwork> &nets) {
    lv_obj_clean(list_wifi_scan_);
    if (nets.empty()) {
        lv_list_add_text(list_wifi_scan_, "No networks found");
        return;
    }
    for (const auto &n : nets) {
        char label[64];
        const char *lock = n.secured ? LV_SYMBOL_CLOSE "  " : "  ";
        snprintf(label, sizeof(label), "%s%s  (%d dBm)", lock,
                 n.ssid.c_str(), n.rssi);
        lv_obj_t *btn = lv_list_add_button(list_wifi_scan_, nullptr, label);
        lv_obj_add_event_cb(btn, on_wifi_list_click, LV_EVENT_CLICKED, this);
    }
}

// ── NTRIP config modal ─────────────────────────────────────────────────────────

void Ui::build_ntrip_modal() {
    modal_ntrip_ = make_modal_base(lv_layer_top(), "NTRIP Config");

    lv_obj_t *bar = lv_obj_get_child(modal_ntrip_, 0);
    lbl_ntrip_title_ = lv_obj_get_child(bar, 0);
    lv_obj_t *close_btn = lv_obj_get_child(bar, lv_obj_get_child_count(bar) - 1);
    lv_obj_add_event_cb(close_btn, on_ntrip_close, LV_EVENT_CLICKED, this);

    lv_obj_t *cont = lv_obj_create(modal_ntrip_);
    lv_obj_remove_style_all(cont);
    lv_obj_set_size(cont, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(cont, 16, 0);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                           LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(cont, 10, 0);

    // Enable switch
    lv_obj_t *en_row = lv_obj_create(cont);
    lv_obj_remove_style_all(en_row);
    lv_obj_set_size(en_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(en_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(en_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                           LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *en_lbl = lv_label_create(en_row);
    lv_label_set_text(en_lbl, "Service enabled");
    lv_obj_set_style_text_color(en_lbl, lv_color_hex(kKeyCol), 0);
    lv_obj_set_flex_grow(en_lbl, 1);
    sw_ntrip_en_ = lv_switch_create(en_row);

    // Mountpoint
    lv_obj_t *mp_lbl = lv_label_create(cont);
    lv_label_set_text(mp_lbl, "Mountpoint:");
    lv_obj_set_style_text_color(mp_lbl, lv_color_hex(kKeyCol), 0);

    ta_ntrip_mp_ = lv_textarea_create(cont);
    lv_textarea_set_one_line(ta_ntrip_mp_, true);
    lv_obj_set_width(ta_ntrip_mp_, LV_PCT(100));
    lv_textarea_set_placeholder_text(ta_ntrip_mp_, "MOUNTPOINT");

    // Password
    lv_obj_t *pw_lbl = lv_label_create(cont);
    lv_label_set_text(pw_lbl, "Password:");
    lv_obj_set_style_text_color(pw_lbl, lv_color_hex(kKeyCol), 0);

    ta_ntrip_pw_ = lv_textarea_create(cont);
    lv_textarea_set_one_line(ta_ntrip_pw_, true);
    lv_textarea_set_password_mode(ta_ntrip_pw_, true);
    lv_obj_set_width(ta_ntrip_pw_, LV_PCT(100));
    lv_textarea_set_placeholder_text(ta_ntrip_pw_, "Password");

    // Keyboard
    kb_ntrip_ = lv_keyboard_create(modal_ntrip_);
    lv_obj_set_size(kb_ntrip_, Display::kWidth, Display::kHeight * 40 / 100);
    lv_obj_add_flag(kb_ntrip_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(kb_ntrip_, on_kb_ready, LV_EVENT_READY,  nullptr);
    lv_obj_add_event_cb(kb_ntrip_, on_kb_ready, LV_EVENT_CANCEL, nullptr);

    lv_obj_add_event_cb(ta_ntrip_mp_, on_ta_focused,   LV_EVENT_FOCUSED,   kb_ntrip_);
    lv_obj_add_event_cb(ta_ntrip_mp_, on_ta_defocused, LV_EVENT_DEFOCUSED, kb_ntrip_);
    lv_obj_add_event_cb(ta_ntrip_pw_, on_ta_focused,   LV_EVENT_FOCUSED,   kb_ntrip_);
    lv_obj_add_event_cb(ta_ntrip_pw_, on_ta_defocused, LV_EVENT_DEFOCUSED, kb_ntrip_);

    // Save/Cancel buttons
    lv_obj_t *btn_row = lv_obj_create(cont);
    lv_obj_remove_style_all(btn_row);
    lv_obj_set_size(btn_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                           LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *cancel_btn = lv_button_create(btn_row);
    lv_obj_set_size(cancel_btn, 160, 44);
    lv_obj_t *c_lbl = lv_label_create(cancel_btn);
    lv_label_set_text(c_lbl, "Cancel");
    lv_obj_center(c_lbl);
    lv_obj_add_event_cb(cancel_btn, on_ntrip_close, LV_EVENT_CLICKED, this);

    lv_obj_t *save_btn = lv_button_create(btn_row);
    lv_obj_set_size(save_btn, 160, 44);
    lv_obj_t *s_lbl = lv_label_create(save_btn);
    lv_label_set_text(s_lbl, LV_SYMBOL_SAVE "  Save");
    lv_obj_center(s_lbl);
    lv_obj_add_event_cb(save_btn, on_ntrip_save, LV_EVENT_CLICKED, this);
}

void Ui::open_ntrip_modal(int idx) {
    if (!modal_ntrip_) build_ntrip_modal();
    static const char *titles[] = {
        "RTK2go Config", "Onocoy Config", "RTKdata Config"};
    static const char *keys[] = {"rtk2go", "onocoy", "rtkdata"};

    ntrip_cfg_idx_ = idx;
    lv_label_set_text(lbl_ntrip_title_, titles[idx]);

    const ServiceCredentials creds = storage_->load_service(keys[idx]);
    const bool enabled = storage_->service_enabled(keys[idx]);

    if (enabled)
        lv_obj_add_state(sw_ntrip_en_, LV_STATE_CHECKED);
    else
        lv_obj_clear_state(sw_ntrip_en_, LV_STATE_CHECKED);

    lv_textarea_set_text(ta_ntrip_mp_, creds.mountpoint.c_str());
    lv_textarea_set_text(ta_ntrip_pw_, creds.password.c_str());
    lv_obj_add_flag(kb_ntrip_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(modal_ntrip_, LV_OBJ_FLAG_HIDDEN);
}

void Ui::save_ntrip_config() {
    static const char *keys[] = {"rtk2go", "onocoy", "rtkdata"};
    const char *key = keys[ntrip_cfg_idx_];

    const bool enabled = lv_obj_has_state(sw_ntrip_en_, LV_STATE_CHECKED);
    ServiceCredentials creds{
        lv_textarea_get_text(ta_ntrip_mp_),
        lv_textarea_get_text(ta_ntrip_pw_)
    };

    storage_->set_service_enabled(key, enabled);
    storage_->save_service(key, creds);
    station_->reload_services();

    lv_obj_add_flag(modal_ntrip_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(kb_ntrip_, LV_OBJ_FLAG_HIDDEN);
}

// ── File browser modal ─────────────────────────────────────────────────────────

void Ui::build_file_browser() {
    modal_files_ = make_modal_base(lv_layer_top(), "SD Card");

    lv_obj_t *bar = lv_obj_get_child(modal_files_, 0);
    lv_obj_t *close_btn = lv_obj_get_child(bar, lv_obj_get_child_count(bar) - 1);
    lv_obj_add_event_cb(close_btn, on_fb_close, LV_EVENT_CLICKED, this);

    // Path bar
    lv_obj_t *path_bar = lv_obj_create(modal_files_);
    lv_obj_remove_style_all(path_bar);
    lv_obj_set_size(path_bar, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(path_bar, 16, 0);
    lv_obj_set_style_pad_ver(path_bar, 6, 0);
    lv_obj_set_flex_flow(path_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(path_bar, LV_FLEX_ALIGN_SPACE_BETWEEN,
                           LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lbl_fb_path_ = lv_label_create(path_bar);
    lv_label_set_text(lbl_fb_path_, "/sdcard");
    lv_obj_set_style_text_color(lbl_fb_path_, lv_color_hex(kKeyCol), 0);
    lv_obj_set_flex_grow(lbl_fb_path_, 1);

    lv_obj_t *up_btn = lv_button_create(path_bar);
    lv_obj_set_size(up_btn, 80, 36);
    lv_obj_t *up_lbl = lv_label_create(up_btn);
    lv_label_set_text(up_lbl, LV_SYMBOL_UP "  Up");
    lv_obj_center(up_lbl);
    lv_obj_add_event_cb(up_btn, on_fb_up, LV_EVENT_CLICKED, this);

    // File list
    list_fb_ = lv_list_create(modal_files_);
    lv_obj_set_size(list_fb_, LV_PCT(100), Display::kHeight - 52 - 48 - 2);
    lv_obj_set_style_bg_color(list_fb_, lv_color_hex(kBgGroup), 0);
    lv_obj_set_style_border_color(list_fb_, lv_color_hex(kBorderCol), 0);
    lv_obj_set_style_radius(list_fb_, 0, 0);
}

void Ui::open_file_browser(const std::string &path) {
    if (!modal_files_) build_file_browser();
    fb_path_ = path;
    refresh_file_browser();
    lv_obj_clear_flag(modal_files_, LV_OBJ_FLAG_HIDDEN);
}

void Ui::refresh_file_browser() {
    fb_entries_.clear();
    lv_obj_clean(list_fb_);
    lv_label_set_text(lbl_fb_path_, fb_path_.c_str());

    DIR *dir = opendir(fb_path_.c_str());
    if (!dir) {
        lv_list_add_text(list_fb_, "Cannot open directory");
        return;
    }

    struct dirent *entry;
    // Collect entries, directories first
    std::vector<std::pair<bool, std::string>> items; // (is_dir, name)
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.') continue;
        items.push_back({entry->d_type == DT_DIR, entry->d_name});
    }
    closedir(dir);

    std::sort(items.begin(), items.end(), [](const auto &a, const auto &b) {
        if (a.first != b.first) return a.first > b.first; // dirs first
        return a.second < b.second;
    });

    for (const auto &[is_dir, name] : items) {
        std::string full = fb_path_ + "/" + name;
        fb_entries_.push_back(full);
        size_t idx = fb_entries_.size() - 1;

        char label[96];
        if (is_dir) {
            snprintf(label, sizeof(label), "%s %s/",
                     LV_SYMBOL_DIRECTORY, name.c_str());
        } else {
            struct stat st{};
            stat(full.c_str(), &st);
            char sz[16];
            fmt_bytes_str(sz, sizeof(sz), (uint64_t)st.st_size);
            snprintf(label, sizeof(label), "%s %-40s  %s",
                     LV_SYMBOL_FILE, name.c_str(), sz);
        }

        lv_obj_t *btn = lv_list_add_button(list_fb_, nullptr, label);
        lv_obj_set_user_data(btn, (void *)(uintptr_t)idx);
        lv_obj_add_event_cb(btn, on_fb_item, LV_EVENT_CLICKED, this);
    }

    if (items.empty()) {
        lv_list_add_text(list_fb_, "(empty)");
    }
}

// ── Event callbacks ────────────────────────────────────────────────────────────

void Ui::on_wifi_btn(lv_event_t *e) {
    static_cast<Ui *>(lv_event_get_user_data(e))->open_wifi_modal();
}

void Ui::on_wifi_close(lv_event_t *e) {
    auto *ui = static_cast<Ui *>(lv_event_get_user_data(e));
    if (ui->modal_wifi_) lv_obj_add_flag(ui->modal_wifi_, LV_OBJ_FLAG_HIDDEN);
    if (ui->kb_wifi_)    lv_obj_add_flag(ui->kb_wifi_,    LV_OBJ_FLAG_HIDDEN);
}

void Ui::on_wifi_scan(lv_event_t *e) {
    auto *ui = static_cast<Ui *>(lv_event_get_user_data(e));
    if (ui->scan_running_.load()) return;
    ui->scan_running_.store(true);

    lv_obj_clean(ui->list_wifi_scan_);
    lv_list_add_text(ui->list_wifi_scan_, LV_SYMBOL_REFRESH "  Scanning...");
    lv_obj_clear_state(ui->btn_wifi_scan_, LV_STATE_DISABLED);

    xTaskCreate(wifi_scan_task, "wifi_scan", 4096, ui, 5, nullptr);
}

void Ui::wifi_scan_task(void *arg) {
    auto *ui = static_cast<Ui *>(arg);
    auto nets = ui->wifi_->scan_networks();
    bsp_display_lock(0);
    ui->populate_wifi_scan_list(nets);
    ui->scan_running_.store(false);
    bsp_display_unlock();
    vTaskDelete(nullptr);
}

struct WifiConnectArg {
    WifiManager *wifi;
    WifiCredentials creds;
};

static void wifi_connect_task(void *arg) {
    auto *a = static_cast<WifiConnectArg *>(arg);
    a->wifi->update_credentials(a->creds);
    delete a;
    vTaskDelete(nullptr);
}

void Ui::on_wifi_connect(lv_event_t *e) {
    auto *ui = static_cast<Ui *>(lv_event_get_user_data(e));

    const char *ssid = lv_textarea_get_text(ui->ta_wifi_ssid_);
    const char *pass = lv_textarea_get_text(ui->ta_wifi_pass_);

    if (!ssid || ssid[0] == '\0') {
        lv_label_set_text(ui->lbl_wifi_msg_, "SSID cannot be empty");
        lv_obj_set_style_text_color(ui->lbl_wifi_msg_,
            lv_palette_main(LV_PALETTE_RED), 0);
        return;
    }

    WifiCredentials creds{ssid, pass, true};
    ui->storage_->save_wifi(creds);

    lv_label_set_text(ui->lbl_wifi_msg_, "Saved. Connecting...");
    lv_obj_set_style_text_color(ui->lbl_wifi_msg_,
        lv_palette_main(LV_PALETTE_YELLOW), 0);
    lv_obj_add_flag(ui->modal_wifi_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui->kb_wifi_, LV_OBJ_FLAG_HIDDEN);

    // update_credentials calls esp_wifi_stop/start/connect which go over SDIO
    // to the C6 coprocessor — must not run inside the LVGL task (display lock held)
    auto *arg = new WifiConnectArg{ui->wifi_, creds};
    xTaskCreate(wifi_connect_task, "wifi_conn", 4096, arg, 5, nullptr);
}

void Ui::on_wifi_list_click(lv_event_t *e) {
    auto *ui = static_cast<Ui *>(lv_event_get_user_data(e));
    lv_obj_t *btn = lv_event_get_target_obj(e);
    // The list button label contains the SSID – extract it
    const char *text = lv_list_get_button_text(ui->list_wifi_scan_, btn);
    if (!text) return;
    // Strip leading icon/lock chars up to the actual SSID
    // Label format: "[lock] ssid  (rssi dBm)"
    // Find the last ')' and work backward to find "  (" before rssi
    const char *rssi_start = strstr(text, "  (");
    if (!rssi_start) return;
    // SSID starts after any leading non-alnum prefix characters
    const char *p = text;
    while (*p && (*p < 0x20 || *p > 0x7E)) p++; // skip UTF-8 symbols
    // Skip lock symbol garbage bytes (may be multi-byte UTF-8)
    while (*p == ' ') p++;
    size_t ssid_len = (size_t)(rssi_start - p);
    if (ssid_len == 0 || ssid_len >= 33) return;
    char ssid[33];
    memcpy(ssid, p, ssid_len);
    ssid[ssid_len] = '\0';
    // Trim trailing spaces
    int l = (int)ssid_len - 1;
    while (l >= 0 && ssid[l] == ' ') ssid[l--] = '\0';
    lv_textarea_set_text(ui->ta_wifi_ssid_, ssid);
}

void Ui::on_ntrip_cfg_btn(lv_event_t *e) {
    auto *ui = static_cast<Ui *>(lv_event_get_user_data(e));
    int idx = (int)(uintptr_t)lv_obj_get_user_data(lv_event_get_target_obj(e));
    ui->open_ntrip_modal(idx);
}

void Ui::on_ntrip_close(lv_event_t *e) {
    auto *ui = static_cast<Ui *>(lv_event_get_user_data(e));
    if (ui->modal_ntrip_) lv_obj_add_flag(ui->modal_ntrip_, LV_OBJ_FLAG_HIDDEN);
    if (ui->kb_ntrip_)    lv_obj_add_flag(ui->kb_ntrip_,    LV_OBJ_FLAG_HIDDEN);
}

void Ui::on_ntrip_save(lv_event_t *e) {
    static_cast<Ui *>(lv_event_get_user_data(e))->save_ntrip_config();
}

void Ui::on_files_btn(lv_event_t *e) {
    auto *ui = static_cast<Ui *>(lv_event_get_user_data(e));
    ui->open_file_browser(SdManager::kMountPoint);
}

void Ui::on_fb_close(lv_event_t *e) {
    auto *ui = static_cast<Ui *>(lv_event_get_user_data(e));
    lv_obj_add_flag(ui->modal_files_, LV_OBJ_FLAG_HIDDEN);
}

void Ui::on_fb_item(lv_event_t *e) {
    auto *ui = static_cast<Ui *>(lv_event_get_user_data(e));
    size_t idx = (size_t)(uintptr_t)lv_obj_get_user_data(lv_event_get_target_obj(e));
    if (idx >= ui->fb_entries_.size()) return;
    const std::string &path = ui->fb_entries_[idx];

    struct stat st{};
    if (stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
        ui->open_file_browser(path);
    }
    // Files: long-press could delete but for now tap does nothing for files
}

void Ui::on_fb_up(lv_event_t *e) {
    auto *ui = static_cast<Ui *>(lv_event_get_user_data(e));
    if (ui->fb_path_ == SdManager::kMountPoint) return;
    size_t slash = ui->fb_path_.rfind('/');
    if (slash == std::string::npos) return;
    std::string parent = ui->fb_path_.substr(0, slash);
    if (parent.empty()) parent = SdManager::kMountPoint;
    ui->open_file_browser(parent);
}

void Ui::on_rinex_toggle(lv_event_t *e) {
    auto *ui = static_cast<Ui *>(lv_event_get_user_data(e));
    lv_obj_t *sw = lv_event_get_target_obj(e);
    bool enabled = lv_obj_has_state(sw, LV_STATE_CHECKED);
    ui->station_->request_raw_collection(enabled);
}

void Ui::on_ntrip_all_toggle(lv_event_t *e) {
    auto *ui = static_cast<Ui *>(lv_event_get_user_data(e));
    lv_obj_t *sw = lv_event_get_target_obj(e);
    bool enabled = lv_obj_has_state(sw, LV_STATE_CHECKED);
    ui->station_->set_streams_enabled(enabled);  // persists across power cycles
}

void Ui::on_survey_start(lv_event_t *e) {
    auto *ui = static_cast<Ui *>(lv_event_get_user_data(e));

    // If already surveying, no confirm needed — just re-queue the action.
    // If in transmit (base) mode, ask for confirmation since it resets position.
    const BaseStationStatus st = ui->station_->status();
    if (st.mode == BaseMode::kTransmit) {
        // Build a compact confirm dialog on lv_layer_top()
        static const char *btns[] = {"Yes", "No", ""};
        lv_obj_t *mbox = lv_msgbox_create(lv_layer_top());
        lv_msgbox_add_title(mbox, "Start New Survey?");
        lv_msgbox_add_text(mbox, "This will reset the current fixed position and begin a new accuracy survey.");
        lv_msgbox_add_footer_button(mbox, "Yes");
        lv_msgbox_add_footer_button(mbox, "Cancel");
        lv_obj_set_width(mbox, 480);
        lv_obj_center(mbox);
        (void)btns;  // suppress unused warning

        // Tag each button with the Ui pointer so the callback can act
        lv_obj_t *footer = lv_msgbox_get_footer(mbox);
        if (footer) {
            uint32_t n = lv_obj_get_child_count(footer);
            for (uint32_t i = 0; i < n; i++) {
                lv_obj_t *btn = lv_obj_get_child(footer, (int32_t)i);
                lv_obj_set_user_data(btn, ui);
                lv_obj_add_event_cb(btn, on_survey_confirm, LV_EVENT_CLICKED, mbox);
            }
        }
    } else {
        ui->station_->request_survey();
    }
}

void Ui::on_survey_confirm(lv_event_t *e) {
    lv_obj_t *mbox = static_cast<lv_obj_t *>(lv_event_get_user_data(e));
    lv_obj_t *btn  = lv_event_get_target_obj(e);
    auto *ui = static_cast<Ui *>(lv_obj_get_user_data(btn));

    const char *label = lv_label_get_text(lv_obj_get_child(btn, 0));
    if (label && strcmp(label, "Yes") == 0 && ui) {
        ui->station_->request_survey();
    }
    lv_msgbox_close(mbox);
}

// ── C6 coprocessor OTA ─────────────────────────────────────────────────────────

void Ui::on_c6_ota_btn(lv_event_t *e) {
    auto *ui = static_cast<Ui *>(lv_event_get_user_data(e));
    if (ui->c6_ota_progress_.load() >= 0) return;

    lv_obj_t *mbox = lv_msgbox_create(lv_layer_top());
    lv_msgbox_add_title(mbox, "Update C6 Firmware?");
    lv_msgbox_add_text(mbox,
        "This flashes new firmware to the WiFi coprocessor. "
        "The device will restart when complete.");
    lv_msgbox_add_footer_button(mbox, "Update");
    lv_msgbox_add_footer_button(mbox, "Cancel");
    lv_obj_set_width(mbox, 480);
    lv_obj_center(mbox);

    lv_obj_t *footer = lv_msgbox_get_footer(mbox);
    if (footer) {
        uint32_t n = lv_obj_get_child_count(footer);
        for (uint32_t i = 0; i < n; i++) {
            lv_obj_t *btn = lv_obj_get_child(footer, (int32_t)i);
            lv_obj_set_user_data(btn, ui);
            lv_obj_add_event_cb(btn, on_c6_ota_confirm, LV_EVENT_CLICKED, mbox);
        }
    }
}

void Ui::on_c6_ota_confirm(lv_event_t *e) {
    lv_obj_t *mbox = static_cast<lv_obj_t *>(lv_event_get_user_data(e));
    lv_obj_t *btn  = lv_event_get_target_obj(e);
    auto *ui = static_cast<Ui *>(lv_obj_get_user_data(btn));

    const char *label = lv_label_get_text(lv_obj_get_child(btn, 0));
    if (label && strcmp(label, "Update") == 0 && ui) {
        ui->c6_ota_progress_.store(0);
        lv_obj_add_state(ui->btn_c6_ota_, LV_STATE_DISABLED);
        xTaskCreate(c6_ota_task, "c6_ota", 8192, ui, 5, nullptr);
    }
    lv_msgbox_close(mbox);
}

void Ui::c6_ota_task(void *arg) {
    auto *ui = static_cast<Ui *>(arg);
    constexpr char kTag[] = "c6_ota";

    extern const uint8_t c6_fw_start[] asm("_binary_c6_slave_fw_bin_start");
    extern const uint8_t c6_fw_end[]   asm("_binary_c6_slave_fw_bin_end");
    const size_t fw_size = (size_t)(c6_fw_end - c6_fw_start);

    ESP_LOGI(kTag, "Starting C6 OTA (%u bytes)", (unsigned)fw_size);

    esp_err_t err = esp_hosted_slave_ota_begin();
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "ota_begin: %s", esp_err_to_name(err));
        ui->c6_ota_progress_.store(-2);
        vTaskDelete(nullptr);
        return;
    }

    constexpr size_t kChunk = 1500;
    size_t offset = 0;
    while (offset < fw_size) {
        size_t chunk = std::min(kChunk, fw_size - offset);
        err = esp_hosted_slave_ota_write(
            const_cast<uint8_t *>(c6_fw_start + offset), (uint32_t)chunk);
        if (err != ESP_OK) {
            ESP_LOGE(kTag, "ota_write at %u: %s", (unsigned)offset, esp_err_to_name(err));
            esp_hosted_slave_ota_end();
            ui->c6_ota_progress_.store(-2);
            vTaskDelete(nullptr);
            return;
        }
        offset += chunk;
        ui->c6_ota_progress_.store((int)(offset * 100 / fw_size));
    }

    err = esp_hosted_slave_ota_end();
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "ota_end: %s", esp_err_to_name(err));
        ui->c6_ota_progress_.store(-2);
        vTaskDelete(nullptr);
        return;
    }

    ESP_LOGI(kTag, "OTA complete, activating C6...");
    esp_hosted_slave_ota_activate();
    // C6 reboots; give it time then restart P4 to resync
    vTaskDelay(pdMS_TO_TICKS(3000));
    esp_restart();
}

// Parse the version + build date out of the embedded C6 image's app descriptor.
// The descriptor sits immediately after the image + first segment headers.
void Ui::load_c6_available_version() {
    extern const uint8_t c6_fw_start[] asm("_binary_c6_slave_fw_bin_start");
    const auto *desc = reinterpret_cast<const esp_app_desc_t *>(
        c6_fw_start + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t));
    if (desc->magic_word == ESP_APP_DESC_MAGIC_WORD) {
        snprintf(c6_avail_ver_, sizeof(c6_avail_ver_), "%s  (%s)",
                 desc->version, desc->date);
    } else {
        strlcpy(c6_avail_ver_, "embedded image", sizeof(c6_avail_ver_));
    }
}

void Ui::c6_version_task(void *arg) {
    auto *ui = static_cast<Ui *>(arg);
    esp_hosted_coprocessor_fwver_t v = {};
    if (esp_hosted_get_coprocessor_fwversion(&v) == ESP_OK) {
        snprintf(ui->c6_running_ver_, sizeof(ui->c6_running_ver_),
                 "%u.%u.%u", (unsigned)v.major1, (unsigned)v.minor1,
                 (unsigned)v.patch1);
    } else {
        strlcpy(ui->c6_running_ver_, "unknown", sizeof(ui->c6_running_ver_));
    }
    ui->c6_running_ready_.store(true);
    vTaskDelete(nullptr);
}

// ── Periodic refresh ──────────────────────────────────────────────────────────

void Ui::refresh_timer_cb(lv_timer_t *timer) {
    static_cast<Ui *>(lv_timer_get_user_data(timer))->refresh();
}

void Ui::refresh() {
    char buf[192];
    const BaseStationStatus st = station_->status();
    const RinexLogger::Status rx = station_->rinex_status();
    const SurveySnapshot &sv = st.survey;

    // ── Status: Base Operation ────────────────────────────────────────────────
    if (st.mode == BaseMode::kSurvey) {
        lv_label_set_text(lbl_mode_, "SURVEY");
        lv_obj_set_style_text_color(lbl_mode_,
            lv_palette_main(LV_PALETTE_ORANGE), 0);
        lv_label_set_text(lbl_survey_btn_, LV_SYMBOL_REFRESH "  Restart Survey");
    } else {
        lv_label_set_text(lbl_mode_, "BASE TX");
        lv_obj_set_style_text_color(lbl_mode_,
            lv_palette_main(LV_PALETTE_GREEN), 0);
        lv_label_set_text(lbl_survey_btn_, LV_SYMBOL_REFRESH "  Start New Survey");
    }

    // Sync global NTRIP toggle without retriggering the callback
    if (sw_ntrip_all_) {
        bool enabled = !station_->streams_suspended();
        bool sw_on   = lv_obj_has_state(sw_ntrip_all_, LV_STATE_CHECKED);
        if (enabled != sw_on) {
            if (enabled) lv_obj_add_state(sw_ntrip_all_, LV_STATE_CHECKED);
            else         lv_obj_clear_state(sw_ntrip_all_, LV_STATE_CHECKED);
        }
    }

    if (st.mode == BaseMode::kTransmit && st.rtcm_bytes_per_second > 0) {
        char tot[20];
        fmt_bytes_str(tot, sizeof(tot), st.rtcm_bytes_total);
        snprintf(buf, sizeof(buf), "%lu B/s  (total: %s)",
                 (unsigned long)st.rtcm_bytes_per_second, tot);
    } else {
        snprintf(buf, sizeof(buf), "\xe2\x80\x94");
    }
    lv_label_set_text(lbl_rtcm_, buf);

    snprintf(buf, sizeof(buf), "G:%d  R:%d  E:%d  C:%d  (%d tracked)",
             sv.gps, sv.glonass, sv.galileo, sv.beidou, sv.satellites_tracked);
    lv_label_set_text(lbl_sats_, buf);

    // ── Status: Position ──────────────────────────────────────────────────────
    const BasePosition bpos = storage_->load_position();
    if (bpos.valid) {
        snprintf(buf, sizeof(buf), "%.7f\xc2\xb0", bpos.lat);
        lv_label_set_text(lbl_lat_, buf);
        lv_label_set_text(lbl_d_lat_, buf);
        snprintf(buf, sizeof(buf), "%.7f\xc2\xb0", bpos.lon);
        lv_label_set_text(lbl_lon_, buf);
        lv_label_set_text(lbl_d_lon_, buf);
        snprintf(buf, sizeof(buf), "%.4f m", bpos.height);
        lv_label_set_text(lbl_alt_, buf);
        lv_label_set_text(lbl_d_alt_, buf);
    } else {
        lv_label_set_text(lbl_lat_,   "—");
        lv_label_set_text(lbl_lon_,   "—");
        lv_label_set_text(lbl_alt_,   "No position stored");
        lv_label_set_text(lbl_d_lat_, "—");
        lv_label_set_text(lbl_d_lon_, "—");
        lv_label_set_text(lbl_d_alt_, "No position stored");
    }

    // Survey progress bars (both Status and Position tabs)
    if (st.mode == BaseMode::kSurvey) {
        snprintf(buf, sizeof(buf),
                 "Survey  %lus  blocks:%d  \xc2\xb1%.2fm",
                 (unsigned long)sv.elapsed_sec, sv.blocks,
                 (double)sv.stability);
        lv_label_set_text(lbl_survey_,   buf);
        lv_label_set_text(lbl_d_survey_, buf);
        lv_obj_clear_flag(lbl_survey_,   LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(bar_survey_,   LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(lbl_d_survey_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(bar_d_survey_, LV_OBJ_FLAG_HIDDEN);
        int32_t p = (int32_t)std::min<uint32_t>(sv.elapsed_sec, 300);
        lv_bar_set_value(bar_survey_,   p, LV_ANIM_OFF);
        lv_bar_set_value(bar_d_survey_, p, LV_ANIM_OFF);
    } else {
        lv_obj_add_flag(lbl_survey_,   LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(bar_survey_,   LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lbl_d_survey_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(bar_d_survey_, LV_OBJ_FLAG_HIDDEN);
    }

    // ── Status: NTRIP compact ─────────────────────────────────────────────────
    fmt_ntrip_label(lbl_rtk2go_,     st.rtk2go,  buf, sizeof(buf));
    fmt_ntrip_label(lbl_onocoy_,     st.onocoy,  buf, sizeof(buf));
    fmt_ntrip_label(lbl_rtkdata_,    st.rtkdata, buf, sizeof(buf));
    snprintf(buf, sizeof(buf), "%d client%s",
             st.local_clients, st.local_clients == 1 ? "" : "s");
    lv_label_set_text(lbl_local_ntrip_, buf);
    lv_obj_set_style_text_color(lbl_local_ntrip_,
        st.local_clients > 0
            ? lv_palette_main(LV_PALETTE_GREEN)
            : lv_color_hex(kDimCol), 0);

    // ── NTRIP tab: detailed ───────────────────────────────────────────────────
    auto update_ntrip_detail = [&](const NtripStatus &ns,
        lv_obj_t *lbl_st, lv_obj_t *lbl_bytes, lv_obj_t *lbl_drop) {
        fmt_ntrip_label(lbl_st, ns, buf, sizeof(buf));
        if (ns.enabled) {
            char kb[20];
            fmt_bytes_str(kb, sizeof(kb), ns.bytes_sent);
            lv_label_set_text(lbl_bytes, kb);
            snprintf(buf, sizeof(buf), "%lu", (unsigned long)ns.dropped_batches);
            lv_label_set_text(lbl_drop, buf);
        } else {
            lv_label_set_text(lbl_bytes, "\xe2\x80\x94");
            lv_label_set_text(lbl_drop,  "\xe2\x80\x94");
        }
    };
    update_ntrip_detail(st.rtk2go,  lbl_d_rtk2go_,  lbl_d_rtk2go_bytes_,  lbl_d_rtk2go_drop_);
    update_ntrip_detail(st.onocoy,  lbl_d_onocoy_,  lbl_d_onocoy_bytes_,  lbl_d_onocoy_drop_);
    update_ntrip_detail(st.rtkdata, lbl_d_rtkdata_,  lbl_d_rtkdata_bytes_, lbl_d_rtkdata_drop_);

    snprintf(buf, sizeof(buf), "%d client%s",
             st.local_clients, st.local_clients == 1 ? "" : "s");
    lv_label_set_text(lbl_d_local_ntrip_, buf);
    lv_obj_set_style_text_color(lbl_d_local_ntrip_,
        st.local_clients > 0
            ? lv_palette_main(LV_PALETTE_GREEN)
            : lv_color_hex(kDimCol), 0);

    // ── Position tab: survey quality ──────────────────────────────────────────
    snprintf(buf, sizeof(buf), "%lus", (unsigned long)sv.elapsed_sec);
    lv_label_set_text(lbl_sv_elapsed_, buf);
    snprintf(buf, sizeof(buf), "%d", sv.blocks);
    lv_label_set_text(lbl_sv_blocks_, buf);
    snprintf(buf, sizeof(buf), "%d", sv.samples);
    lv_label_set_text(lbl_sv_samples_, buf);
    snprintf(buf, sizeof(buf), "%.3f m", (double)sv.stability);
    lv_label_set_text(lbl_sv_stability_, buf);
    snprintf(buf, sizeof(buf), "%.3f m", (double)sv.instantaneous_sigma);
    lv_label_set_text(lbl_sv_sigma_, buf);

    snprintf(buf, sizeof(buf), "%d / %d",
             sv.satellites_used, sv.satellites_tracked);
    lv_label_set_text(lbl_sv_used_, buf);
    snprintf(buf, sizeof(buf), "%d", sv.gps);
    lv_label_set_text(lbl_sv_gps_, buf);
    snprintf(buf, sizeof(buf), "%d", sv.glonass);
    lv_label_set_text(lbl_sv_glo_, buf);
    snprintf(buf, sizeof(buf), "%d", sv.galileo);
    lv_label_set_text(lbl_sv_gal_, buf);
    snprintf(buf, sizeof(buf), "%d", sv.beidou);
    lv_label_set_text(lbl_sv_bds_, buf);

    // Per-satellite SNR string (top 16 by SNR)
    SatelliteInfo sats[32];
    size_t nsat = station_->satellites(sats, 32);
    if (nsat > 0) {
        std::sort(sats, sats + nsat,
                  [](const SatelliteInfo &a, const SatelliteInfo &b) {
                      return a.snr > b.snr;
                  });
        static const char *sys_names[] = {"G","R","?","E","C","J"};
        char sat_buf[256] = {};
        int pos = 0;
        for (size_t i = 0; i < std::min(nsat, (size_t)16) && pos < 240; i++) {
            const char *sys = (sats[i].system < 6) ? sys_names[sats[i].system] : "?";
            pos += snprintf(sat_buf + pos, sizeof(sat_buf) - pos,
                            "%s%02d:%02ddB  ", sys,
                            (int)(sats[i].prn % 100), (int)sats[i].snr);
        }
        lv_label_set_text(lbl_sv_detail_, sat_buf);
    } else {
        lv_label_set_text(lbl_sv_detail_, "No satellite data");
    }

    // ── System tab: NETWORK ───────────────────────────────────────────────────
    // Station row: connected network + signal, or the link state while down.
    if (wifi_->connected()) {
        snprintf(buf, sizeof(buf), "%s  %d dBm",
                 wifi_->ssid().c_str(), wifi_->rssi());
        lv_obj_set_style_text_color(lbl_wifi_state_,
            lv_palette_main(LV_PALETTE_GREEN), 0);
    } else {
        const WifiCredentials wc = storage_->load_wifi();
        if (wc.valid) {
            snprintf(buf, sizeof(buf), "Connecting to %s\xe2\x80\xa6",
                     wc.ssid.c_str());
            lv_obj_set_style_text_color(lbl_wifi_state_,
                lv_palette_main(LV_PALETTE_YELLOW), 0);
        } else {
            snprintf(buf, sizeof(buf), "Not configured");
            lv_obj_set_style_text_color(lbl_wifi_state_, lv_color_hex(kDimCol), 0);
        }
    }
    lv_label_set_text(lbl_wifi_state_, buf);

    const std::string sta_ip = wifi_->ip_address();
    lv_label_set_text(lbl_ip_, sta_ip.empty() ? "\xe2\x80\x94" : sta_ip.c_str());

    // Hotspot row: SoftAP name + IP (always up in AP+STA).
    const std::string ap_name = wifi_->access_point_ssid();
    const std::string ap_ip   = wifi_->access_point_ip();
    lv_label_set_text(lbl_ap_name_, ap_name.empty() ? "off" : ap_name.c_str());
    lv_obj_set_style_text_color(lbl_ap_name_,
        ap_name.empty() ? lv_color_hex(kDimCol) : lv_palette_main(LV_PALETTE_GREEN), 0);
    lv_label_set_text(lbl_ap_ip_, ap_ip.empty() ? "\xe2\x80\x94" : ap_ip.c_str());

    if (sd_->is_mounted()) {
        const SdManager::DiskStats stats = sd_->disk_stats();
        if (stats.valid) {
            char used[20], total[20];
            fmt_bytes_str(used,  sizeof(used),  stats.used_bytes);
            fmt_bytes_str(total, sizeof(total), stats.total_bytes);
            snprintf(buf, sizeof(buf), "%s / %s", used, total);
        } else {
            snprintf(buf, sizeof(buf), "Mounted");
        }
    } else {
        snprintf(buf, sizeof(buf), "Not mounted");
    }
    lv_label_set_text(lbl_sd_, buf);

    // RINEX switch – keep in sync with actual state (don't fight user)
    bool rinex_active = rx.active;
    if (rinex_active != lv_obj_has_state(sw_rinex_, LV_STATE_CHECKED)) {
        if (rinex_active)
            lv_obj_add_state(sw_rinex_, LV_STATE_CHECKED);
        else
            lv_obj_clear_state(sw_rinex_, LV_STATE_CHECKED);
    }

    if (rx.active && !rx.current_file.empty()) {
        // Show just the filename, not the full path
        const std::string &fn = rx.current_file;
        size_t slash = fn.rfind('/');
        const char *fname = (slash != std::string::npos)
                            ? fn.c_str() + slash + 1 : fn.c_str();
        snprintf(buf, sizeof(buf), "%s  ep:%d", fname, rx.epochs);
    } else {
        snprintf(buf, sizeof(buf), rx.active ? "Active" : "—");
    }
    lv_label_set_text(lbl_rinex_file_, buf);

    const esp_app_desc_t *desc = esp_app_get_description();
    if (desc) {
        lv_label_set_text(lbl_fw_, desc->version);
        snprintf(buf, sizeof(buf), "%s  %s", desc->date, desc->time);
        lv_label_set_text(lbl_compile_, buf);
    }

    // C6 running version (cached once the RPC query completes).
    lv_label_set_text(lbl_c6_running_,
                      c6_running_ready_.load() ? c6_running_ver_ : "querying\xe2\x80\xa6");

    // C6 available line doubles as the OTA progress/status indicator.
    int ota_p = c6_ota_progress_.load();
    if (ota_p == -1) {
        lv_label_set_text(lbl_c6_fw_, c6_avail_ver_);
        lv_obj_clear_state(btn_c6_ota_, LV_STATE_DISABLED);
    } else if (ota_p == -2) {
        lv_label_set_text(lbl_c6_fw_, "Update failed");
        lv_obj_set_style_text_color(lbl_c6_fw_, lv_palette_main(LV_PALETTE_RED), 0);
        lv_obj_clear_state(btn_c6_ota_, LV_STATE_DISABLED);
    } else {
        snprintf(buf, sizeof(buf), "Flashing\xe2\x80\xa6 %d%%", ota_p);
        lv_label_set_text(lbl_c6_fw_, buf);
        lv_obj_add_state(btn_c6_ota_, LV_STATE_DISABLED);
    }

    // ── Debug tab (every other tick to reduce overhead) ───────────────────────
    static int debug_tick = 0;
    if (++debug_tick >= 2) {
        debug_tick = 0;
        refresh_debug_log();
    }
}

void Ui::refresh_debug_log() {
    if (!lbl_debug_ || !log_sem) return;

    static char log_text[kLogLines * kLogWidth + kLogLines]; // ~6 KB
    log_text[0] = '\0';

    if (xSemaphoreTake(log_sem, pdMS_TO_TICKS(10)) == pdTRUE) {
        int start = (log_count == kLogLines) ? log_head : 0;
        int write_pos = 0;
        for (int i = 0; i < log_count; i++) {
            int idx = (start + i) % kLogLines;
            int n = snprintf(log_text + write_pos,
                             sizeof(log_text) - write_pos - 1,
                             "%s\n", log_buf[idx]);
            if (n > 0) write_pos += n;
        }
        xSemaphoreGive(log_sem);
    }

    lv_label_set_text(lbl_debug_, log_text);
    // Scroll the tab page (not the inner container) to show the latest log lines.
    // Use get_scroll_y + get_scroll_bottom for a bounded, safe target value.
    if (tab_debug_) {
        int32_t target = lv_obj_get_scroll_y(tab_debug_) +
                         lv_obj_get_scroll_bottom(tab_debug_);
        lv_obj_scroll_to_y(tab_debug_, target, LV_ANIM_OFF);
    }
}
