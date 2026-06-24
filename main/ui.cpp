#include "ui.hpp"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <string>
#include <sys/stat.h>

#include "app_config.hpp"
#include "bsp/esp-bsp.h"
#include "theme.hpp"
#include "ui_sections.hpp"
#include "sd_delete.hpp"
#include "display.hpp"
#include "log_buffer.hpp"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_heap_caps.h"
#include "esp_hosted.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

namespace {

constexpr char kTag[] = "ui";

const char *reset_reason_str(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_POWERON:   return "power-on";
        case ESP_RST_EXT:       return "external";
        case ESP_RST_SW:        return "software";
        case ESP_RST_PANIC:     return "panic";
        case ESP_RST_INT_WDT:   return "int watchdog";
        case ESP_RST_TASK_WDT:  return "task watchdog";
        case ESP_RST_WDT:       return "watchdog";
        case ESP_RST_DEEPSLEEP: return "deep-sleep";
        case ESP_RST_BROWNOUT:  return "brownout";
        case ESP_RST_SDIO:      return "SDIO";
        default:                return "unknown";
    }
}

bool reset_is_crash(esp_reset_reason_t r) {
    return r == ESP_RST_PANIC || r == ESP_RST_INT_WDT ||
           r == ESP_RST_TASK_WDT || r == ESP_RST_WDT || r == ESP_RST_BROWNOUT;
}

// Colour palette — marine-instrument tokens sourced from the shared single-source
// table in theme.hpp (night palette). The web UI mirrors these exact hexes in its
// CSS :root block (web_server.cpp, kAfterTitle); the Phase-4 audit verifies parity.
// Marine-instrument palette — MUTABLE (was constexpr) so the day/night toggle can
// re-point these at runtime via apply_palette(); the screen is then rebuilt to re-tint
// every object (see Ui::apply_theme). Night is the default. Mirrors theme.hpp.
uint32_t kBgScreen  = theme::kNight.bg;          // screen background
uint32_t kBgGroup   = theme::kNight.surface;     // card / group surface
uint32_t kSurfaceHi = theme::kNight.surface_hi;  // raised / selected
uint32_t kBorderCol = theme::kNight.border;      // borders, dividers
uint32_t kTextCol   = theme::kNight.text;        // primary numeric / value
uint32_t kKeyCol    = theme::kNight.text_dim;    // row key labels / units
uint32_t kTitleCol  = theme::kNight.text_dim;    // group / tile labels
uint32_t kDimCol    = theme::kNight.text_faint;  // dim / placeholder / idle
uint32_t kAccentCol = theme::kNight.accent;      // interactive / links
uint32_t kGoodCol   = theme::kNight.good;        // ok / live
uint32_t kWarnCol   = theme::kNight.warn;        // surveying / weak
uint32_t kCritCol   = theme::kNight.crit;        // alarm / failed

// Re-point the palette globals at a theme (night or day). Caller rebuilds the screen.
void apply_palette(const theme::Theme &t) {
    kBgScreen = t.bg;       kBgGroup = t.surface;   kSurfaceHi = t.surface_hi;
    kBorderCol = t.border;  kTextCol = t.text;      kKeyCol = t.text_dim;
    kTitleCol = t.text_dim; kDimCol = t.text_faint; kAccentCol = t.accent;
    kGoodCol = t.good;      kWarnCol = t.warn;      kCritCol = t.crit;
}

// Survey-in completion gates (per design spec §D.2).
constexpr uint32_t kGateTimeSec = 300;     // elapsed ≥ 300 s
constexpr int      kGateBlocks  = 5;       // blocks ≥ 5
constexpr float    kGateStab    = 0.50f;   // stability ≤ 0.50 m

// Sky-plot geometry (Position tab). Square plot area; horizon-ring radius in px.
constexpr int kSkyPlotSize = 300;
constexpr int kSkyR        = 130;
// Constellation dot colors, indexed by SatelliteInfo::system. Mirrors the web
// sky-plot palette (web_server.cpp): GPS=white, GLONASS=blue, ?=coral, Galileo=green,
// BeiDou=yellow, QZSS=light blue.
constexpr uint32_t kSysColors[6] = {
    0xFFFFFF, 0x33AAFF, 0xFF8866, 0x55DD55, 0xFFDD55, 0x66CCFF,
};

// ── Console log ──────────────────────────────────────────────────────────────
// Console capture is owned by the shared log_buffer module (installed once in
// app_main, also served by the web /logs page). The System Console panel renders
// the tail of that single buffer (folds in the former Debug tab).
constexpr size_t kConsoleTailChars = 4000;

void ipv4_to_text(uint32_t ipv4, char *output, size_t output_size) {
    if (output_size == 0) return;
    if (!ipv4) {
        snprintf(output, output_size, "unknown");
        return;
    }
    const uint32_t host = ntohl(ipv4);
    snprintf(output, output_size, "%u.%u.%u.%u",
             static_cast<unsigned>((host >> 24) & 0xff),
             static_cast<unsigned>((host >> 16) & 0xff),
             static_cast<unsigned>((host >> 8) & 0xff),
             static_cast<unsigned>(host & 0xff));
}

void local_client_ips_to_text(
    const LocalCaster::ClientSnapshot &clients,
    char *output, size_t output_size) {
    if (output_size == 0) return;
    if (clients.count == 0) {
        snprintf(output, output_size, "\xe2\x80\x94");
        return;
    }

    output[0] = '\0';
    size_t used = 0;
    for (int i = 0; i < clients.count && used < output_size; ++i) {
        char ip[16];
        ipv4_to_text(clients.ipv4[i], ip, sizeof(ip));
        const int written = snprintf(
            output + used, output_size - used,
            "%s%s", i ? ", " : "", ip);
        if (written < 0) break;
        if (static_cast<size_t>(written) >= output_size - used) {
            output[output_size - 1] = '\0';
            break;
        }
        used += static_cast<size_t>(written);
    }
}

}  // namespace

// ── Ui::init ──────────────────────────────────────────────────────────────────

esp_err_t Ui::init(
    Display &display, BaseStation &station,
    SdManager &sd, WifiManager &wifi, Storage &storage) {
    station_ = &station;
    sd_      = &sd;
    wifi_    = &wifi;
    storage_ = &storage;

    // Pick the persisted palette before building the screen (shared with the web UI).
    night_ = storage_->night_mode();
    apply_palette(night_ ? theme::kNight : theme::kDay);
    disp_ = display.handle();

    bsp_display_lock(0);
    build_screens(disp_);
    lv_timer_create(refresh_timer_cb, 1000, this);
    bsp_display_unlock();

    // The embedded C6 image version is local to parse (no SDIO RPC).
    load_c6_available_version();
    // Query the *running* C6 version off the LVGL task — it's an RPC over SDIO.
    xTaskCreate(c6_version_task, "c6_ver", 4096, this, 3, nullptr);

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
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_pad_all(card, 16, 0);
    lv_obj_set_style_pad_row(card, 0, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                           LV_FLEX_ALIGN_START);

    lv_obj_t *hdr = lv_label_create(card);
    lv_label_set_text(hdr, title);
    lv_obj_set_style_text_color(hdr, lv_color_hex(kTitleCol), 0);
    lv_obj_set_style_text_font(hdr, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_letter_space(hdr, 1, 0);
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
    lv_obj_set_style_text_color(*val_out, lv_color_hex(kTextCol), 0);
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

// ── Dashboard tile (the atom) ───────────────────────────────────────────────────
// Label (uppercase/dim) top, large value center, optional dim sub. Tappable tiles
// deep-link to their owning tab. value/sub label pointers returned via outs.
lv_obj_t *Ui::make_tile(lv_obj_t *parent, const char *label,
                        lv_obj_t **val_out, lv_obj_t **sub_out,
                        const lv_font_t *val_font, bool span2,
                        int target_tab) {
    lv_obj_t *tile = lv_obj_create(parent);
    lv_obj_remove_style_all(tile);
    lv_obj_set_height(tile, LV_SIZE_CONTENT);
    // 2-up flex row-wrap: the hero spans the full row; other tiles take ~half so two
    // sit side by side and the rest wrap onto the next row.
    lv_obj_set_width(tile, span2 ? LV_PCT(100) : LV_PCT(48));
    lv_obj_set_style_bg_color(tile, lv_color_hex(kBgGroup), 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(tile, lv_color_hex(kBorderCol), 0);
    lv_obj_set_style_border_width(tile, 1, 0);
    lv_obj_set_style_border_opa(tile, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(tile, 12, 0);
    lv_obj_set_style_pad_all(tile, 16, 0);
    lv_obj_set_style_pad_row(tile, 4, 0);
    lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                           LV_FLEX_ALIGN_START);

    lv_obj_t *lbl = lv_label_create(tile);
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_color(lbl, lv_color_hex(kTitleCol), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_letter_space(lbl, 1, 0);

    *val_out = lv_label_create(tile);
    lv_label_set_text(*val_out, "\xe2\x80\x94");
    lv_obj_set_style_text_color(*val_out, lv_color_hex(kTextCol), 0);
    lv_obj_set_style_text_font(*val_out, val_font, 0);

    if (sub_out) {
        *sub_out = lv_label_create(tile);
        lv_label_set_text(*sub_out, "");
        lv_obj_set_style_text_color(*sub_out, lv_color_hex(kKeyCol), 0);
        lv_obj_set_style_text_font(*sub_out, &lv_font_montserrat_14, 0);
    }

    if (target_tab >= 0) {
        lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(tile, (void *)(uintptr_t)target_tab);
        lv_obj_add_event_cb(tile, on_tile_click, LV_EVENT_CLICKED, this);
        // Faint chevron hint that the tile is a deep-link.
        lv_obj_t *chev = lv_label_create(tile);
        lv_label_set_text(chev, LV_SYMBOL_RIGHT);
        lv_obj_set_style_text_color(chev, lv_color_hex(kDimCol), 0);
        lv_obj_add_flag(chev, LV_OBJ_FLAG_IGNORE_LAYOUT);  // float, not in flex flow
        lv_obj_align(chev, LV_ALIGN_TOP_RIGHT, 0, 0);
    }
    return tile;
}

// Status pill: colored dot + word in a capsule. Caller styles the colors.
lv_obj_t *Ui::make_pill(lv_obj_t *parent, lv_obj_t **dot_out,
                         lv_obj_t **word_out) {
    lv_obj_t *pill = lv_obj_create(parent);
    lv_obj_remove_style_all(pill);
    lv_obj_set_height(pill, 32);
    lv_obj_set_width(pill, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(pill, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(pill, lv_color_hex(kSurfaceHi), 0);
    lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(pill, 12, 0);
    lv_obj_set_style_pad_ver(pill, 0, 0);
    lv_obj_set_flex_flow(pill, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(pill, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                           LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(pill, 8, 0);

    *dot_out = lv_obj_create(pill);
    lv_obj_remove_style_all(*dot_out);
    lv_obj_set_size(*dot_out, 10, 10);
    lv_obj_set_style_radius(*dot_out, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(*dot_out, lv_color_hex(kGoodCol), 0);
    lv_obj_set_style_bg_opa(*dot_out, LV_OPA_COVER, 0);

    *word_out = lv_label_create(pill);
    lv_label_set_text(*word_out, "");
    lv_obj_set_style_text_color(*word_out, lv_color_hex(kGoodCol), 0);
    lv_obj_set_style_text_font(*word_out, &lv_font_montserrat_14, 0);
    return pill;
}

// 270° gauge arc with a centered value label. range 0..range_max.
lv_obj_t *Ui::make_gauge(lv_obj_t *parent, const char *label,
                          lv_obj_t **arc_out, lv_obj_t **val_out,
                          int32_t range_max) {
    lv_obj_t *cell = lv_obj_create(parent);
    lv_obj_remove_style_all(cell);
    lv_obj_set_size(cell, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                           LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(cell, 4, 0);

    lv_obj_t *arc = lv_arc_create(cell);
    *arc_out = arc;
    lv_obj_set_size(arc, 120, 120);
    lv_arc_set_rotation(arc, 135);
    lv_arc_set_bg_angles(arc, 0, 270);
    lv_arc_set_range(arc, 0, range_max);
    lv_arc_set_value(arc, 0);
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(arc, 12, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, 12, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, lv_color_hex(kBorderCol), LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, lv_color_hex(kAccentCol), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_pad_all(arc, 0, LV_PART_KNOB);

    *val_out = lv_label_create(arc);
    lv_label_set_text(*val_out, "\xe2\x80\x94");
    lv_obj_set_style_text_color(*val_out, lv_color_hex(kTextCol), 0);
    lv_obj_set_style_text_font(*val_out, &lv_font_montserrat_18, 0);
    lv_obj_center(*val_out);

    lv_obj_t *cap = lv_label_create(cell);
    lv_label_set_text(cap, label);
    lv_obj_set_style_text_color(cap, lv_color_hex(kTitleCol), 0);
    lv_obj_set_style_text_font(cap, &lv_font_montserrat_14, 0);
    return cell;
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
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_18, 0);
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
        char up[16];
        const unsigned s = ns.connected_sec;
        if (s >= 3600)     snprintf(up, sizeof(up), "%uh%02um", s / 3600, (s % 3600) / 60);
        else if (s >= 60)  snprintf(up, sizeof(up), "%um%02us", s / 60, s % 60);
        else               snprintf(up, sizeof(up), "%us", s);
        snprintf(buf, buf_len, "Connected  up %s", up);
        lv_label_set_text(lbl, buf);
        lv_obj_set_style_text_color(lbl, lv_color_hex(kGoodCol), 0);
    } else {
        const char *msg = !ns.message.empty() ? ns.message.c_str()
                        : !ns.last_error.empty() ? ns.last_error.c_str()
                        : "Connecting\xe2\x80\xa6";
        lv_label_set_text(lbl, msg);
        lv_obj_set_style_text_color(lbl, lv_color_hex(kCritCol), 0);
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
    lv_obj_set_style_pad_row(tab, 12, 0);
    lv_obj_set_scroll_dir(tab, LV_DIR_VER);
    lv_obj_set_style_bg_color(tab, lv_color_hex(kBgScreen), 0);
}

void Ui::build_screens(lv_display_t *disp) {
    lv_theme_t *theme = lv_theme_default_init(
        disp,
        lv_color_hex(kAccentCol),
        lv_palette_main(LV_PALETTE_BLUE_GREY),
        true, LV_FONT_DEFAULT);
    lv_display_set_theme(disp, theme);

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(kBgScreen), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(scr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                           LV_FLEX_ALIGN_START);

    // Persistent ~64px header (title + global pill + day/night toggle).
    build_header(scr);

    // Bottom tabview filling the rest of the screen.
    lv_obj_t *tv = lv_tabview_create(scr);
    tabview_ = tv;
    lv_obj_set_width(tv, Display::kWidth);
    lv_obj_set_flex_grow(tv, 1);
    lv_tabview_set_tab_bar_position(tv, LV_DIR_BOTTOM);
    lv_tabview_set_tab_bar_size(tv, 80);
    lv_obj_set_style_bg_color(tv, lv_color_hex(kBgScreen), 0);

    // Active-tab accent rule on the tab buttons.
    lv_obj_t *tab_btns = lv_tabview_get_tab_bar(tv);
    lv_obj_set_style_bg_color(tab_btns, lv_color_hex(kBgGroup), 0);
    lv_obj_set_style_text_color(tab_btns, lv_color_hex(kKeyCol), 0);
    const lv_style_selector_t checked_item =
        (lv_style_selector_t)LV_PART_ITEMS | (lv_style_selector_t)LV_STATE_CHECKED;
    lv_obj_set_style_border_color(tab_btns, lv_color_hex(kAccentCol), checked_item);
    lv_obj_set_style_text_color(tab_btns, lv_color_hex(kAccentCol), checked_item);

    using ui_sections::kTouchLabel;
    using ui_sections::Section;
    char tl[40];
    snprintf(tl, sizeof(tl), LV_SYMBOL_HOME " %s",
             kTouchLabel[(int)Section::Dashboard]);
    tab_dash_ = lv_tabview_add_tab(tv, tl);
    snprintf(tl, sizeof(tl), LV_SYMBOL_GPS " %s",
             kTouchLabel[(int)Section::Position]);
    tab_pos_ = lv_tabview_add_tab(tv, tl);
    snprintf(tl, sizeof(tl), LV_SYMBOL_WIFI " %s",
             kTouchLabel[(int)Section::Links]);
    tab_links_ = lv_tabview_add_tab(tv, tl);
    snprintf(tl, sizeof(tl), LV_SYMBOL_SD_CARD " %s",
             kTouchLabel[(int)Section::Storage]);
    tab_storage_ = lv_tabview_add_tab(tv, tl);
    snprintf(tl, sizeof(tl), LV_SYMBOL_SETTINGS " %s",
             kTouchLabel[(int)Section::System]);
    tab_sys_ = lv_tabview_add_tab(tv, tl);

    style_tab(tab_dash_);
    style_tab(tab_pos_);
    style_tab(tab_links_);
    style_tab(tab_storage_);
    style_tab(tab_sys_);

    build_dashboard_tab(tab_dash_);
    build_position_tab(tab_pos_);
    build_links_tab(tab_links_);
    build_storage_tab(tab_storage_);
    build_system_tab(tab_sys_);

    // Keep the header title in sync with the active section.
    lv_obj_add_event_cb(tv, on_tab_changed, LV_EVENT_VALUE_CHANGED, this);
    // Modals are built lazily on first open to keep init fast.
}

// ── Header ──────────────────────────────────────────────────────────────────────

void Ui::build_header(lv_obj_t *parent) {
    lv_obj_t *hdr = lv_obj_create(parent);
    lv_obj_remove_style_all(hdr);
    lv_obj_set_size(hdr, Display::kWidth, 64);
    lv_obj_set_style_bg_color(hdr, lv_color_hex(kBgGroup), 0);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(hdr, lv_color_hex(kBorderCol), 0);
    lv_obj_set_style_border_side(hdr, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(hdr, 1, 0);
    lv_obj_set_style_pad_hor(hdr, 16, 0);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                           LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(hdr, 10, 0);

    header_title_ = lv_label_create(hdr);
    lv_label_set_text(header_title_, ui_sections::kWebName[0]);
    lv_obj_set_style_text_color(header_title_, lv_color_hex(kTextCol), 0);
    lv_obj_set_style_text_font(header_title_, &lv_font_montserrat_28, 0);
    lv_obj_set_flex_grow(header_title_, 1);

    header_pill_ = make_pill(hdr, &header_pill_dot_, &header_pill_lbl_);
    lv_label_set_text(header_pill_lbl_, "LIVE");

    // Day/night toggle — visual placeholder this phase. Runtime theme-swap (a
    // full apply_theme() that re-tints every object) is Phase 4.
    header_theme_btn_ = lv_button_create(hdr);
    lv_obj_set_size(header_theme_btn_, 48, 40);
    lv_obj_set_style_bg_color(header_theme_btn_, lv_color_hex(kSurfaceHi), 0);
    lv_obj_t *tb_lbl = lv_label_create(header_theme_btn_);
    lv_label_set_text(tb_lbl, LV_SYMBOL_EYE_OPEN);
    lv_obj_center(tb_lbl);
    lv_obj_add_event_cb(header_theme_btn_, on_theme_btn, LV_EVENT_CLICKED, this);
}

// ── Dashboard tab ────────────────────────────────────────────────────────────

void Ui::build_dashboard_tab(lv_obj_t *parent) {
    using ui_sections::Section;

    // 2-up tile layout via flex row-wrap. (Was LV_LAYOUT_GRID, but tiles other than
    // the hero were never assigned grid cells, so they all collapsed onto cell (0,0)
    // and overlapped. Flex row-wrap auto-places them by width.)
    lv_obj_t *grid = lv_obj_create(parent);
    lv_obj_remove_style_all(grid);
    lv_obj_set_width(grid, LV_PCT(100));
    lv_obj_set_height(grid, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                           LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(grid, 12, 0);
    lv_obj_set_style_pad_row(grid, 12, 0);
    lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLLABLE);

    // Row 1 — hero RTCM tile (spans 2 cols). The unit ("B/s") rides as part of
    // the value text; the "RTCM OUTPUT" tile label is the caption.
    make_tile(grid, "RTCM OUTPUT", &lbl_hero_val_, &lbl_hero_sub_,
              &lv_font_montserrat_48, true, -1);
    lbl_hero_unit_ = nullptr;
    lbl_hero_cap_  = nullptr;

    // Row 2 — Mode | Satellites
    make_tile(grid, "MODE", &lbl_t_mode_, nullptr,
              &lv_font_montserrat_28, false, (int)Section::Position);
    make_tile(grid, "SATELLITES", &lbl_t_sats_, &lbl_t_sats_sub_,
              &lv_font_montserrat_28, false, (int)Section::Position);

    // Row 3 — NTRIP Push (3 service lights) | Position (opens Position Setup modal)
    build_ntrip_lights_tile(grid);
    make_tile(grid, "POSITION", &lbl_t_pos_, &lbl_t_pos_sub_,
              &lv_font_montserrat_28, false, -1);
    // The Position tile opens the Position Setup modal instead of deep-linking. The
    // tile is the last grid child added by make_tile above; wire a click handler on it.
    {
        lv_obj_t *pos_tile = lv_obj_get_parent(lbl_t_pos_);
        lv_obj_add_flag(pos_tile, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(pos_tile, on_pos_setup_tile, LV_EVENT_CLICKED, this);
        // Faint chevron hint that the tile is interactive (mirrors make_tile).
        lv_obj_t *chev = lv_label_create(pos_tile);
        lv_label_set_text(chev, LV_SYMBOL_RIGHT);
        lv_obj_set_style_text_color(chev, lv_color_hex(kDimCol), 0);
        lv_obj_add_flag(chev, LV_OBJ_FLAG_IGNORE_LAYOUT);
        lv_obj_align(chev, LV_ALIGN_TOP_RIGHT, 0, 0);
    }

    // Row 4 — SD/RINEX | Link
    make_tile(grid, "SD / RINEX", &lbl_t_sd_, &lbl_t_sd_sub_,
              &lv_font_montserrat_28, false, (int)Section::Storage);
    make_tile(grid, "LINK", &lbl_t_link_, &lbl_t_link_sub_,
              &lv_font_montserrat_28, false, (int)Section::System);

    // Pinned Start/Restart Survey action.
    btn_survey_start_ = lv_button_create(parent);
    lv_obj_set_size(btn_survey_start_, LV_PCT(100), 56);
    lv_obj_set_style_bg_color(btn_survey_start_, lv_color_hex(kWarnCol), 0);
    lbl_survey_btn_ = lv_label_create(btn_survey_start_);
    lv_label_set_text(lbl_survey_btn_, LV_SYMBOL_REFRESH "  Start Survey");
    lv_obj_set_style_text_color(lbl_survey_btn_, lv_color_hex(kBgScreen), 0);
    lv_obj_center(lbl_survey_btn_);
    lv_obj_add_event_cb(btn_survey_start_, on_survey_start, LV_EVENT_CLICKED, this);
}

// Custom NTRIP Push tile: three service lights (dot + name + reconnect count)
// instead of a single rollup value. Tappable → deep-links to the Links tab.
void Ui::build_ntrip_lights_tile(lv_obj_t *grid) {
    using ui_sections::Section;

    lv_obj_t *tile = lv_obj_create(grid);
    lv_obj_remove_style_all(tile);
    lv_obj_set_width(tile, LV_PCT(48));  // half-width like make_tile (flex row-wrap)
    lv_obj_set_height(tile, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(tile, lv_color_hex(kBgGroup), 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(tile, lv_color_hex(kBorderCol), 0);
    lv_obj_set_style_border_width(tile, 1, 0);
    lv_obj_set_style_border_opa(tile, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(tile, 12, 0);
    lv_obj_set_style_pad_all(tile, 16, 0);
    lv_obj_set_style_pad_row(tile, 6, 0);
    lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                           LV_FLEX_ALIGN_START);

    lv_obj_t *lbl = lv_label_create(tile);
    lv_label_set_text(lbl, "NTRIP PUSH");
    lv_obj_set_style_text_color(lbl, lv_color_hex(kTitleCol), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_letter_space(lbl, 1, 0);

    static const char *svc_names[3] = {"RTK2go", "Onocoy", "RTKdata"};
    for (int i = 0; i < 3; i++) {
        lv_obj_t *row = lv_obj_create(tile);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                               LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, 8, 0);

        t_ntrip_dot_[i] = lv_obj_create(row);
        lv_obj_remove_style_all(t_ntrip_dot_[i]);
        lv_obj_set_size(t_ntrip_dot_[i], 12, 12);
        lv_obj_set_style_radius(t_ntrip_dot_[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(t_ntrip_dot_[i], lv_color_hex(kDimCol), 0);
        lv_obj_set_style_bg_opa(t_ntrip_dot_[i], LV_OPA_COVER, 0);

        lv_obj_t *name = lv_label_create(row);
        lv_label_set_text(name, svc_names[i]);
        lv_obj_set_style_text_color(name, lv_color_hex(kTextCol), 0);
        lv_obj_set_style_text_font(name, &lv_font_montserrat_18, 0);

        t_ntrip_recon_[i] = lv_label_create(row);
        lv_label_set_text(t_ntrip_recon_[i], "");
        lv_obj_set_style_text_color(t_ntrip_recon_[i], lv_color_hex(kDimCol), 0);
        lv_obj_set_style_text_font(t_ntrip_recon_[i], &lv_font_montserrat_14, 0);
    }

    // Tappable → deep-link to the Links tab (mirrors make_tile behaviour).
    t_ntrip_clients_ = lv_label_create(tile);
    lv_label_set_text(t_ntrip_clients_, "Direct: none");
    lv_obj_set_width(t_ntrip_clients_, LV_PCT(100));
    lv_label_set_long_mode(t_ntrip_clients_, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(t_ntrip_clients_, lv_color_hex(kDimCol), 0);
    lv_obj_set_style_text_font(t_ntrip_clients_, &lv_font_montserrat_14, 0);

    lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(tile, (void *)(uintptr_t)(int)Section::Links);
    lv_obj_add_event_cb(tile, on_tile_click, LV_EVENT_CLICKED, this);
    lv_obj_t *chev = lv_label_create(tile);
    lv_label_set_text(chev, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(chev, lv_color_hex(kDimCol), 0);
    lv_obj_add_flag(chev, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_align(chev, LV_ALIGN_TOP_RIGHT, 0, 0);
}

// ── Position tab ──────────────────────────────────────────────────────────────

void Ui::build_position_tab(lv_obj_t *parent) {
    lv_obj_t *g_pos = make_group(parent, "FIXED BASE POSITION");
    make_row(g_pos, &lbl_p_lat_, "Latitude");
    make_row(g_pos, &lbl_p_lon_, "Longitude");
    make_row(g_pos, &lbl_p_alt_, "Height");

    // SURVEY-IN: three completion-gate arcs (time / blocks / stability).
    survey_group_ = make_group(parent, "SURVEY-IN");
    lv_obj_t *arcs = lv_obj_create(survey_group_);
    lv_obj_remove_style_all(arcs);
    lv_obj_set_width(arcs, LV_PCT(100));
    lv_obj_set_height(arcs, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(arcs, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(arcs, LV_FLEX_ALIGN_SPACE_AROUND, LV_FLEX_ALIGN_CENTER,
                           LV_FLEX_ALIGN_CENTER);
    make_gauge(arcs, "TIME",   &arc_sv_time_,   &lbl_sv_time_,   (int32_t)kGateTimeSec);
    make_gauge(arcs, "BLOCKS", &arc_sv_blocks_, &lbl_sv_blocks_, kGateBlocks);
    make_gauge(arcs, "STABLE", &arc_sv_stab_,   &lbl_sv_stab_,   100);  // % toward gate
    make_row(survey_group_, &lbl_sv_samples_, "Samples");
    make_row(survey_group_, &lbl_sv_sigma_,   "Inst. sigma");

    lv_obj_t *g_sats = make_group(parent, "SATELLITES");
    make_row(g_sats, &lbl_sv_used_, "Used / tracked");
    make_row(g_sats, &lbl_sv_gps_,  "GPS");
    make_row(g_sats, &lbl_sv_glo_,  "GLONASS");
    make_row(g_sats, &lbl_sv_gal_,  "Galileo");
    make_row(g_sats, &lbl_sv_bds_,  "BeiDou");

    lbl_sv_detail_ = lv_label_create(g_sats);
    lv_label_set_long_mode(lbl_sv_detail_, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl_sv_detail_, LV_PCT(100));
    lv_obj_set_style_text_color(lbl_sv_detail_, lv_color_hex(kDimCol), 0);
    lv_obj_set_style_text_font(lbl_sv_detail_, &lv_font_montserrat_14, 0);
    lv_obj_set_style_pad_top(lbl_sv_detail_, 6, 0);
    lv_label_set_text(lbl_sv_detail_, "");

    // SKY PLOT — polar el/az view (rings + constellation-colored dots) drawn with
    // native LVGL objects. Dots are rebuilt in refresh() only while Position is the
    // active tab, so the redraw never stacks with the survey arcs / other tabs.
    lv_obj_t *g_sky = make_group(parent, "SKY PLOT");

    sky_plot_ = lv_obj_create(g_sky);
    lv_obj_set_width(sky_plot_, LV_PCT(100));
    lv_obj_set_height(sky_plot_, kSkyPlotSize);
    lv_obj_set_style_bg_opa(sky_plot_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(sky_plot_, 0, 0);
    lv_obj_set_style_pad_all(sky_plot_, 0, 0);
    lv_obj_clear_flag(sky_plot_, LV_OBJ_FLAG_SCROLLABLE);

    // Elevation rings: horizon (el 0), el 30, el 60. r = kSkyR*(90-el)/90.
    const int ring_el[3] = {0, 30, 60};
    for (int i = 0; i < 3; i++) {
        int diam = 2 * kSkyR * (90 - ring_el[i]) / 90;
        lv_obj_t *ring = lv_obj_create(sky_plot_);
        lv_obj_set_size(ring, diam, diam);
        lv_obj_align(ring, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_color(ring, lv_color_hex(kBorderCol), 0);
        lv_obj_set_style_border_width(ring, 1, 0);
        lv_obj_clear_flag(ring, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    }

    // Cardinal labels just outside the horizon ring.
    const char *card_txt[4] = {"N", "S", "E", "W"};
    const int   card_dx[4]  = {0, 0, kSkyR + 12, -(kSkyR + 12)};
    const int   card_dy[4]  = {-(kSkyR + 12), kSkyR + 12, 0, 0};
    for (int i = 0; i < 4; i++) {
        lv_obj_t *c = lv_label_create(sky_plot_);
        lv_label_set_text(c, card_txt[i]);
        lv_obj_set_style_text_color(c, lv_color_hex(kKeyCol), 0);
        lv_obj_align(c, LV_ALIGN_CENTER, card_dx[i], card_dy[i]);
    }

    // Overlay holding the satellite dots (cleared + rebuilt each tick by refresh()).
    sky_dots_ = lv_obj_create(sky_plot_);
    lv_obj_set_size(sky_dots_, kSkyPlotSize, kSkyPlotSize);
    lv_obj_align(sky_dots_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_opa(sky_dots_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(sky_dots_, 0, 0);
    lv_obj_set_style_pad_all(sky_dots_, 0, 0);
    lv_obj_clear_flag(sky_dots_, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));

    lbl_sky_count_ = lv_label_create(g_sky);
    lv_obj_set_style_text_color(lbl_sky_count_, lv_color_hex(kDimCol), 0);
    lv_label_set_text(lbl_sky_count_, "\xe2\x80\x94");

    // ANTENNA — read-only echo; edit lives in System.
    lv_obj_t *g_ant = make_group(parent, "ANTENNA");
    make_row(g_ant, &lbl_p_ant_, "Model / height");
}

// ── Links tab ─────────────────────────────────────────────────────────────────

static lv_obj_t *ntrip_detail_group(lv_obj_t *parent, const char *title,
    lv_obj_t **lbl_status, lv_obj_t **lbl_bytes, lv_obj_t **lbl_drop,
    lv_obj_t **lbl_recon, Ui *ui, int svc_idx) {
    lv_obj_t *g = lv_obj_create(parent);
    lv_obj_remove_style_all(g);
    lv_obj_set_width(g, LV_PCT(100));
    lv_obj_set_height(g, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(g, lv_color_hex(kBgGroup), 0);
    lv_obj_set_style_bg_opa(g, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(g, lv_color_hex(kBorderCol), 0);
    lv_obj_set_style_border_width(g, 1, 0);
    lv_obj_set_style_border_opa(g, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(g, 12, 0);
    lv_obj_set_style_pad_all(g, 16, 0);
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
    lv_obj_set_style_text_color(hdr_lbl, lv_color_hex(kTitleCol), 0);
    lv_obj_set_style_text_font(hdr_lbl, &lv_font_montserrat_18, 0);
    lv_obj_set_flex_grow(hdr_lbl, 1);

    lv_obj_t *cfg_btn = lv_button_create(hdr_row);
    lv_obj_set_size(cfg_btn, 90, 36);
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
    lv_obj_set_style_bg_color(div, lv_color_hex(kBorderCol), 0);
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
        lv_obj_set_style_text_color(k, lv_color_hex(kKeyCol), 0);
        lv_obj_set_width(k, 148);
        *out = lv_label_create(row);
        lv_label_set_text(*out, "\xe2\x80\x94");
        lv_obj_set_style_text_color(*out, lv_color_hex(kTextCol), 0);
        lv_obj_set_flex_grow(*out, 1);
    };
    make_r(lbl_status, "Status");
    make_r(lbl_bytes,  "Bytes sent");
    make_r(lbl_drop,   "Dropped");
    make_r(lbl_recon,  "Reconnects");
    return g;
}

void Ui::build_links_tab(lv_obj_t *parent) {
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
        &lbl_d_rtk2go_recon_, this, 0);
    ntrip_detail_group(parent, "Onocoy",
        &lbl_d_onocoy_, &lbl_d_onocoy_bytes_, &lbl_d_onocoy_drop_,
        &lbl_d_onocoy_recon_, this, 1);
    ntrip_detail_group(parent, "RTKdata",
        &lbl_d_rtkdata_, &lbl_d_rtkdata_bytes_, &lbl_d_rtkdata_drop_,
        &lbl_d_rtkdata_recon_, this, 2);

    lv_obj_t *g_local = make_group(parent, "LOCAL CASTER :2101");
    make_row(g_local, &lbl_d_local_ntrip_, "Clients");
    make_row(g_local, &lbl_d_local_ntrip_ips_, "Client IPs");
}

// ── Storage tab ────────────────────────────────────────────────────────────────

void Ui::build_storage_tab(lv_obj_t *parent) {
    lv_obj_t *g_sd = make_group(parent, "SD CARD");
    // Disk-usage arc centered in the card.
    lv_obj_t *arc_cell = lv_obj_create(g_sd);
    lv_obj_remove_style_all(arc_cell);
    lv_obj_set_width(arc_cell, LV_PCT(100));
    lv_obj_set_height(arc_cell, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(arc_cell, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(arc_cell, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                           LV_FLEX_ALIGN_CENTER);
    make_gauge(arc_cell, "DISK USED", &arc_disk_, &lbl_disk_pct_, 100);
    make_row(g_sd, &lbl_sd_, "Used / total");

    lv_obj_t *g_rx = make_group(parent, "RINEX COLLECTION");
    make_switch_row(g_rx, &sw_rinex_, "RINEX logging");
    lv_obj_add_event_cb(sw_rinex_, on_rinex_toggle, LV_EVENT_VALUE_CHANGED, this);
    make_row(g_rx, &lbl_rinex_file_, "Current file");
    make_row(g_rx, &lbl_s_ant_, "Antenna");

    lv_obj_t *g_fb = make_group(parent, "FILE BROWSER");
    lv_obj_t *files_btn = lv_button_create(g_fb);
    lv_obj_set_size(files_btn, LV_PCT(100), 48);
    lv_obj_set_style_margin_top(files_btn, 4, 0);
    lv_obj_t *files_lbl = lv_label_create(files_btn);
    lv_label_set_text(files_lbl, LV_SYMBOL_DIRECTORY "  Browse SD Card");
    lv_obj_center(files_lbl);
    lv_obj_add_event_cb(files_btn, on_files_btn, LV_EVENT_CLICKED, this);
}

// ── System tab ────────────────────────────────────────────────────────────────

void Ui::build_system_tab(lv_obj_t *parent) {
    lv_obj_t *g_sys = make_group(parent, "SYSTEM");
    make_row(g_sys, &lbl_uptime_, "Uptime");
    make_row(g_sys, &lbl_reset_,  "Last reset");
    // Free-heap arc.
    lv_obj_t *heap_cell = lv_obj_create(g_sys);
    lv_obj_remove_style_all(heap_cell);
    lv_obj_set_width(heap_cell, LV_PCT(100));
    lv_obj_set_height(heap_cell, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(heap_cell, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(heap_cell, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                           LV_FLEX_ALIGN_CENTER);
    make_gauge(heap_cell, "HEAP FREE", &arc_heap_, &lbl_heap_pct_, 100);
    make_row(g_sys, &lbl_heap_, "Free heap");

    lv_obj_t *g_net = make_group(parent, "NETWORK");
    make_row(g_net, &lbl_wifi_state_, "Station");
    make_row(g_net, &lbl_ip_,         "Station IP");
    make_row(g_net, &lbl_ap_name_,    "Hotspot");
    make_row(g_net, &lbl_ap_ip_,      "Hotspot IP");

    lv_obj_t *wifi_btn = lv_button_create(g_net);
    lv_obj_set_size(wifi_btn, LV_PCT(100), 48);
    lv_obj_set_style_margin_top(wifi_btn, 8, 0);
    lv_obj_t *wifi_lbl = lv_label_create(wifi_btn);
    lv_label_set_text(wifi_lbl, LV_SYMBOL_WIFI "  Configure WiFi");
    lv_obj_center(wifi_lbl);
    lv_obj_add_event_cb(wifi_btn, on_wifi_btn, LV_EVENT_CLICKED, this);

    lv_obj_t *g_fw = make_group(parent, "FIRMWARE");
    make_row(g_fw, &lbl_fw_,         "Firmware");
    make_row(g_fw, &lbl_compile_,    "Built");
    make_row(g_fw, &lbl_c6_running_, "C6 running");
    make_row(g_fw, &lbl_c6_fw_,      "C6 available");

    btn_c6_ota_ = lv_button_create(g_fw);
    lv_obj_set_size(btn_c6_ota_, LV_PCT(100), 48);
    lv_obj_set_style_margin_top(btn_c6_ota_, 8, 0);
    lv_obj_t *c6_lbl = lv_label_create(btn_c6_ota_);
    lv_label_set_text(c6_lbl, LV_SYMBOL_UPLOAD "  Update C6 Firmware");
    lv_obj_center(c6_lbl);
    lv_obj_add_event_cb(btn_c6_ota_, on_c6_ota_btn, LV_EVENT_CLICKED, this);

    // CONSOLE — opens in its own modal. The live log auto-updates, so keeping it
    // inline scrolled the System tab and hid the settings above it.
    lv_obj_t *g_con = make_group(parent, "CONSOLE");
    lv_obj_t *con_btn = lv_button_create(g_con);
    lv_obj_set_size(con_btn, LV_PCT(100), 48);
    lv_obj_set_style_margin_top(con_btn, 4, 0);
    lv_obj_t *con_lbl = lv_label_create(con_btn);
    lv_label_set_text(con_lbl, LV_SYMBOL_LIST "  View Console");
    lv_obj_center(con_lbl);
    lv_obj_add_event_cb(con_btn, on_console_btn, LV_EVENT_CLICKED, this);

    // DISPLAY / ADMIN.
    lv_obj_t *g_disp = make_group(parent, "DISPLAY & ADMIN");
    make_row(g_disp, &lbl_admin_, "Admin password");
    lv_obj_t *admin_btn = lv_button_create(g_disp);
    lv_obj_set_size(admin_btn, LV_PCT(100), 48);
    lv_obj_set_style_margin_top(admin_btn, 8, 0);
    lv_obj_t *admin_lbl = lv_label_create(admin_btn);
    lv_label_set_text(admin_lbl, LV_SYMBOL_KEYBOARD "  Change Admin Password");
    lv_obj_center(admin_lbl);
    lv_obj_add_event_cb(admin_btn, on_admin_btn, LV_EVENT_CLICKED, this);
    // ANTENNA editing moved to the Position Setup modal (opened from the Dashboard
    // Position tile). It is the canonical antenna entry now.
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

    // ── Hotspot (this device's own AP) password ──
    char ap_hdr[64];
    snprintf(ap_hdr, sizeof(ap_hdr), "Hotspot \"%s\" password (WPA2):",
             config::kAccessPointSsid);
    lv_obj_t *lbl_ap = lv_label_create(cont);
    lv_label_set_text(lbl_ap, ap_hdr);
    lv_obj_set_style_text_color(lbl_ap, lv_color_hex(kKeyCol), 0);
    lv_obj_set_style_pad_top(lbl_ap, 6, 0);

    ta_ap_pass_ = lv_textarea_create(cont);
    lv_textarea_set_one_line(ta_ap_pass_, true);
    lv_textarea_set_password_mode(ta_ap_pass_, true);
    lv_obj_set_width(ta_ap_pass_, LV_PCT(100));
    lv_textarea_set_placeholder_text(ta_ap_pass_, "8-63 characters");
    lv_obj_add_event_cb(ta_ap_pass_, on_ta_focused,   LV_EVENT_FOCUSED,   kb_wifi_);
    lv_obj_add_event_cb(ta_ap_pass_, on_ta_defocused, LV_EVENT_DEFOCUSED, kb_wifi_);

    lv_obj_t *ap_btn = lv_button_create(cont);
    lv_obj_set_size(ap_btn, 220, 40);
    lv_obj_t *ap_btn_lbl = lv_label_create(ap_btn);
    lv_label_set_text(ap_btn_lbl, LV_SYMBOL_SAVE "  Save Hotspot Password");
    lv_obj_center(ap_btn_lbl);
    lv_obj_add_event_cb(ap_btn, on_ap_save, LV_EVENT_CLICKED, this);

    lbl_ap_msg_ = lv_label_create(cont);
    lv_label_set_text(lbl_ap_msg_, "");
    lv_obj_set_style_text_color(lbl_ap_msg_, lv_color_hex(kDimCol), 0);

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
    std::string ap_pw = storage_->ap_password();
    if (ap_pw.empty()) ap_pw = config::kDefaultApPassword;
    lv_textarea_set_text(ta_ap_pass_, ap_pw.c_str());
    lv_label_set_text(lbl_ap_msg_, "");
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

// ── Admin password modal ────────────────────────────────────────────────────────

void Ui::build_admin_modal() {
    modal_admin_ = make_modal_base(lv_layer_top(), "Admin Password");

    lv_obj_t *bar = lv_obj_get_child(modal_admin_, 0);
    lv_obj_t *close_btn = lv_obj_get_child(bar, lv_obj_get_child_count(bar) - 1);
    lv_obj_add_event_cb(close_btn, on_admin_close, LV_EVENT_CLICKED, this);

    kb_admin_ = lv_keyboard_create(modal_admin_);
    lv_obj_set_size(kb_admin_, Display::kWidth, Display::kHeight * 40 / 100);
    lv_obj_add_flag(kb_admin_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(kb_admin_, on_kb_ready, LV_EVENT_READY,  nullptr);
    lv_obj_add_event_cb(kb_admin_, on_kb_ready, LV_EVENT_CANCEL, nullptr);

    lv_obj_t *cont = lv_obj_create(modal_admin_);
    lv_obj_remove_style_all(cont);
    lv_obj_set_size(cont, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(cont, 16, 0);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                           LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(cont, 10, 0);

    lv_obj_t *lbl = lv_label_create(cont);
    lv_label_set_text(lbl, "New admin password (min 4 chars):");
    lv_obj_set_style_text_color(lbl, lv_color_hex(kKeyCol), 0);

    ta_admin_pass_ = lv_textarea_create(cont);
    lv_textarea_set_one_line(ta_admin_pass_, true);
    lv_textarea_set_password_mode(ta_admin_pass_, true);
    lv_obj_set_width(ta_admin_pass_, LV_PCT(100));
    lv_textarea_set_placeholder_text(ta_admin_pass_, "Password");
    lv_obj_add_event_cb(ta_admin_pass_, on_ta_focused,   LV_EVENT_FOCUSED,   kb_admin_);
    lv_obj_add_event_cb(ta_admin_pass_, on_ta_defocused, LV_EVENT_DEFOCUSED, kb_admin_);

    lv_obj_t *save_btn = lv_button_create(cont);
    lv_obj_set_size(save_btn, 200, 44);
    lv_obj_t *s_lbl = lv_label_create(save_btn);
    lv_label_set_text(s_lbl, LV_SYMBOL_SAVE "  Save Password");
    lv_obj_center(s_lbl);
    lv_obj_add_event_cb(save_btn, on_admin_save, LV_EVENT_CLICKED, this);

    lbl_admin_msg_ = lv_label_create(cont);
    lv_label_set_text(lbl_admin_msg_, "");
    lv_obj_set_style_text_color(lbl_admin_msg_, lv_color_hex(kDimCol), 0);
}

void Ui::open_admin_modal() {
    if (!modal_admin_) build_admin_modal();
    lv_textarea_set_text(ta_admin_pass_, "");
    lv_label_set_text(lbl_admin_msg_, "");
    lv_obj_add_flag(kb_admin_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(modal_admin_, LV_OBJ_FLAG_HIDDEN);
}

// ── Antenna edit modal ──────────────────────────────────────────────────────────

void Ui::build_antenna_modal() {
    modal_ant_ = make_modal_base(lv_layer_top(), "Antenna Metadata");

    lv_obj_t *bar = lv_obj_get_child(modal_ant_, 0);
    lv_obj_t *close_btn = lv_obj_get_child(bar, lv_obj_get_child_count(bar) - 1);
    lv_obj_add_event_cb(close_btn, on_antenna_close, LV_EVENT_CLICKED, this);

    kb_ant_ = lv_keyboard_create(modal_ant_);
    lv_obj_set_size(kb_ant_, Display::kWidth, Display::kHeight * 40 / 100);
    lv_obj_add_flag(kb_ant_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(kb_ant_, on_kb_ready, LV_EVENT_READY,  nullptr);
    lv_obj_add_event_cb(kb_ant_, on_kb_ready, LV_EVENT_CANCEL, nullptr);

    lv_obj_t *cont = lv_obj_create(modal_ant_);
    lv_obj_remove_style_all(cont);
    lv_obj_set_size(cont, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(cont, 16, 0);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                           LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(cont, 8, 0);

    auto field = [&](const char *cap, lv_obj_t **ta, const char *ph,
                     lv_obj_t *kb) {
        lv_obj_t *l = lv_label_create(cont);
        lv_label_set_text(l, cap);
        lv_obj_set_style_text_color(l, lv_color_hex(kKeyCol), 0);
        *ta = lv_textarea_create(cont);
        lv_textarea_set_one_line(*ta, true);
        lv_obj_set_width(*ta, LV_PCT(100));
        lv_textarea_set_placeholder_text(*ta, ph);
        lv_obj_add_event_cb(*ta, on_ta_focused,   LV_EVENT_FOCUSED,   kb);
        lv_obj_add_event_cb(*ta, on_ta_defocused, LV_EVENT_DEFOCUSED, kb);
    };
    field("Antenna model (IGS/NGS name):", &ta_ant_model_,  "HXCGPS500", kb_ant_);
    field("Radome:",                       &ta_ant_radome_, "NONE",      kb_ant_);
    field("ARP height (metres):",          &ta_ant_height_, "0.0",       kb_ant_);

    lv_obj_t *save_btn = lv_button_create(cont);
    lv_obj_set_size(save_btn, 160, 44);
    lv_obj_t *s_lbl = lv_label_create(save_btn);
    lv_label_set_text(s_lbl, LV_SYMBOL_SAVE "  Save");
    lv_obj_center(s_lbl);
    lv_obj_add_event_cb(save_btn, on_antenna_save, LV_EVENT_CLICKED, this);

    lbl_ant_msg_ = lv_label_create(cont);
    lv_label_set_text(lbl_ant_msg_, "");
    lv_obj_set_style_text_color(lbl_ant_msg_, lv_color_hex(kDimCol), 0);
}

void Ui::open_antenna_modal() {
    if (!modal_ant_) build_antenna_modal();
    lv_textarea_set_text(ta_ant_model_,  storage_->antenna_model().c_str());
    lv_textarea_set_text(ta_ant_radome_, storage_->antenna_radome().c_str());
    char h[24];
    snprintf(h, sizeof(h), "%.4f", storage_->antenna_height());
    lv_textarea_set_text(ta_ant_height_, h);
    lv_label_set_text(lbl_ant_msg_, "");
    lv_obj_add_flag(kb_ant_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(modal_ant_, LV_OBJ_FLAG_HIDDEN);
}

// ── Console modal ─────────────────────────────────────────────────────────────
// Separate, scrollable log view opened from System. Updated by refresh_console_log()
// ONLY while open, and it scrolls its own container (never the System tab).

void Ui::build_console_modal() {
    modal_console_ = make_modal_base(lv_layer_top(), "Console");

    lv_obj_t *bar = lv_obj_get_child(modal_console_, 0);
    lv_obj_t *close_btn = lv_obj_get_child(bar, lv_obj_get_child_count(bar) - 1);
    lv_obj_add_event_cb(close_btn, on_console_close, LV_EVENT_CLICKED, this);

    console_scroll_ = lv_obj_create(modal_console_);
    lv_obj_remove_style_all(console_scroll_);
    lv_obj_set_width(console_scroll_, LV_PCT(100));
    lv_obj_set_flex_grow(console_scroll_, 1);
    lv_obj_set_style_pad_all(console_scroll_, 12, 0);
    lv_obj_set_scroll_dir(console_scroll_, LV_DIR_VER);
    lv_obj_set_style_bg_color(console_scroll_, lv_color_hex(kBgScreen), 0);
    lv_obj_set_style_bg_opa(console_scroll_, LV_OPA_COVER, 0);

    lbl_console_ = lv_label_create(console_scroll_);
    lv_label_set_long_mode(lbl_console_, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl_console_, LV_PCT(100));
    lv_obj_set_style_text_color(lbl_console_, lv_color_hex(kGoodCol), 0);
    lv_obj_set_style_text_font(lbl_console_, &lv_font_montserrat_14, 0);
    lv_label_set_text(lbl_console_, "(no log output yet)");
}

void Ui::open_console_modal() {
    if (!modal_console_) build_console_modal();
    refresh_console_log();  // fill immediately on open
    lv_obj_clear_flag(modal_console_, LV_OBJ_FLAG_HIDDEN);
}

void Ui::on_console_btn(lv_event_t *e) {
    static_cast<Ui *>(lv_event_get_user_data(e))->open_console_modal();
}

void Ui::on_console_close(lv_event_t *e) {
    auto *ui = static_cast<Ui *>(lv_event_get_user_data(e));
    if (ui->modal_console_) lv_obj_add_flag(ui->modal_console_, LV_OBJ_FLAG_HIDDEN);
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

    // "Select" → enter selection mode (design §3.1).
    btn_fb_select_ = lv_button_create(path_bar);
    lv_obj_set_size(btn_fb_select_, 96, 36);
    lv_obj_t *sel_lbl = lv_label_create(btn_fb_select_);
    lv_label_set_text(sel_lbl, ui_sections::del::kSelect);
    lv_obj_center(sel_lbl);
    lv_obj_add_event_cb(btn_fb_select_, on_fb_select_btn, LV_EVENT_CLICKED, this);

    lv_obj_t *up_btn = lv_button_create(path_bar);
    lv_obj_set_size(up_btn, 80, 36);
    lv_obj_t *up_lbl = lv_label_create(up_btn);
    lv_label_set_text(up_lbl, LV_SYMBOL_UP "  Up");
    lv_obj_center(up_lbl);
    lv_obj_add_event_cb(up_btn, on_fb_up, LV_EVENT_CLICKED, this);

    // File list. Height leaves room for the (initially hidden) 64px action bar.
    list_fb_ = lv_list_create(modal_files_);
    lv_obj_set_width(list_fb_, LV_PCT(100));
    lv_obj_set_flex_grow(list_fb_, 1);
    lv_obj_set_style_bg_color(list_fb_, lv_color_hex(kBgGroup), 0);
    lv_obj_set_style_border_color(list_fb_, lv_color_hex(kBorderCol), 0);
    lv_obj_set_style_radius(list_fb_, 0, 0);

    // ── Selection action bar (design §3.2), 64px, hidden until selection mode ───
    fb_action_bar_ = lv_obj_create(modal_files_);
    lv_obj_remove_style_all(fb_action_bar_);
    lv_obj_set_size(fb_action_bar_, LV_PCT(100), 64);
    lv_obj_set_style_bg_color(fb_action_bar_, lv_color_hex(kBgGroup), 0);
    lv_obj_set_style_bg_opa(fb_action_bar_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(fb_action_bar_, lv_color_hex(kBorderCol), 0);
    lv_obj_set_style_border_width(fb_action_bar_, 1, 0);
    lv_obj_set_style_pad_hor(fb_action_bar_, 12, 0);
    lv_obj_set_style_pad_ver(fb_action_bar_, 0, 0);
    lv_obj_set_flex_flow(fb_action_bar_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(fb_action_bar_, LV_FLEX_ALIGN_START,
                           LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(fb_action_bar_, 10, 0);
    lv_obj_add_flag(fb_action_bar_, LV_OBJ_FLAG_HIDDEN);

    lbl_fb_count_ = lv_label_create(fb_action_bar_);
    lv_label_set_text(lbl_fb_count_, "0 selected");
    lv_obj_set_style_text_color(lbl_fb_count_, lv_color_hex(kTextCol), 0);
    lv_obj_set_flex_grow(lbl_fb_count_, 1);

    lv_obj_t *selall_btn = lv_button_create(fb_action_bar_);
    lv_obj_set_size(selall_btn, LV_SIZE_CONTENT, 48);
    lv_obj_set_style_pad_hor(selall_btn, 12, 0);
    lv_obj_t *selall_lbl = lv_label_create(selall_btn);
    lv_label_set_text(selall_lbl, ui_sections::del::kSelectAll);
    lv_obj_center(selall_lbl);
    lv_obj_add_event_cb(selall_btn, on_fb_selall_btn, LV_EVENT_CLICKED, this);

    btn_fb_delete_ = lv_button_create(fb_action_bar_);
    lv_obj_set_size(btn_fb_delete_, LV_SIZE_CONTENT, 64);
    lv_obj_set_style_pad_hor(btn_fb_delete_, 14, 0);
    lv_obj_set_style_bg_color(btn_fb_delete_, lv_color_hex(kCritCol), 0);
    lbl_fb_delete_ = lv_label_create(btn_fb_delete_);
    lv_label_set_text(lbl_fb_delete_, LV_SYMBOL_TRASH "  Delete (0)");
    lv_obj_center(lbl_fb_delete_);
    lv_obj_add_event_cb(btn_fb_delete_, on_fb_delete_btn, LV_EVENT_CLICKED, this);

    lv_obj_t *cancel_btn = lv_button_create(fb_action_bar_);
    lv_obj_set_size(cancel_btn, LV_SIZE_CONTENT, 48);
    lv_obj_set_style_pad_hor(cancel_btn, 12, 0);
    lv_obj_t *cancel_lbl = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_lbl, ui_sections::del::kCancel);
    lv_obj_center(cancel_lbl);
    lv_obj_add_event_cb(cancel_btn, on_fb_cancel_btn, LV_EVENT_CLICKED, this);
}

void Ui::open_file_browser(const std::string &path) {
    if (!modal_files_) build_file_browser();
    fb_path_ = path;
    refresh_file_browser();
    lv_obj_clear_flag(modal_files_, LV_OBJ_FLAG_HIDDEN);
}

void Ui::refresh_file_browser() {
    // A re-list always resets selection (design §5 "selection invalidated by
    // refresh" — selections are only preserved while NOT rebuilding the list).
    fb_select_mode_ = false;
    if (fb_action_bar_) lv_obj_add_flag(fb_action_bar_, LV_OBJ_FLAG_HIDDEN);
    fb_entries_.clear();
    fb_is_dir_.clear();
    fb_selected_.clear();
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
        fb_is_dir_.push_back(is_dir);
        fb_selected_.push_back(false);
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
        lv_obj_set_height(btn, 64);
        // Reserve a leading gutter so the selection-mode check never overlaps text.
        lv_obj_set_style_pad_left(btn, 44, 0);
        lv_obj_add_event_cb(btn, on_fb_item, LV_EVENT_CLICKED, this);
        lv_obj_add_event_cb(btn, on_fb_item_long, LV_EVENT_LONG_PRESSED, this);

        // Directory rows expose a trailing trash glyph for whole-folder delete,
        // working outside selection mode too (design §3.2). Floating so it sits
        // outside the row's flex layout.
        if (is_dir) {
            lv_obj_t *trash = lv_button_create(btn);
            lv_obj_set_size(trash, 48, 48);
            lv_obj_add_flag(trash, LV_OBJ_FLAG_FLOATING);
            lv_obj_align(trash, LV_ALIGN_RIGHT_MID, 0, 0);
            lv_obj_set_style_bg_opa(trash, LV_OPA_TRANSP, 0);
            lv_obj_set_style_shadow_width(trash, 0, 0);
            lv_obj_t *tl = lv_label_create(trash);
            lv_label_set_text(tl, LV_SYMBOL_TRASH);
            lv_obj_set_style_text_color(tl, lv_color_hex(kCritCol), 0);
            lv_obj_center(tl);
            lv_obj_set_user_data(trash, (void *)(uintptr_t)idx);
            lv_obj_add_event_cb(trash, on_fb_folder_btn, LV_EVENT_CLICKED, this);
        }
    }

    if (items.empty()) {
        lv_list_add_text(list_fb_, "(empty)");
    }
}

// ── Event callbacks ────────────────────────────────────────────────────────────

void Ui::on_tab_changed(lv_event_t *e) {
    auto *ui = static_cast<Ui *>(lv_event_get_user_data(e));
    if (!ui->tabview_ || !ui->header_title_) return;
    uint32_t idx = lv_tabview_get_tab_active(ui->tabview_);
    if (idx < (uint32_t)ui_sections::kSectionCount) {
        lv_label_set_text(ui->header_title_, ui_sections::kWebName[idx]);
    }
}

void Ui::on_tile_click(lv_event_t *e) {
    auto *ui = static_cast<Ui *>(lv_event_get_user_data(e));
    int tab = (int)(uintptr_t)lv_obj_get_user_data(lv_event_get_target_obj(e));
    if (ui->tabview_ && tab >= 0)
        lv_tabview_set_active(ui->tabview_, (uint32_t)tab, LV_ANIM_OFF);
}

void Ui::on_theme_btn(lv_event_t *e) {
    auto *ui = static_cast<Ui *>(lv_event_get_user_data(e));
    ui->apply_theme(!ui->night_);
}

// Runtime day/night swap. The palette is baked into objects at build time, so the
// reliable way to re-tint is to re-point the palette globals and rebuild the screen
// tree. Runs on the LVGL task (button event), which can't race the refresh timer
// (also LVGL task); worker tasks only touch atomics, never LVGL objects.
void Ui::apply_theme(bool night) {
    if (night == night_) return;
    night_ = night;
    if (storage_) storage_->set_night_mode(night);
    // Don't tear down the screen while a bulk delete is running (its progress dialog
    // is live); the choice is persisted and takes effect on the next rebuild/boot.
    if (sd_delete::busy()) return;

    bsp_display_lock(0);
    apply_palette(night ? theme::kNight : theme::kDay);
    // Lazily-built modals live on the top layer — clean it and null their roots so the
    // open_* guards rebuild them fresh; also reset the bulk-delete dialog + selection.
    lv_obj_clean(lv_layer_top());
    modal_wifi_  = nullptr; modal_admin_ = nullptr; modal_ant_ = nullptr;
    modal_ntrip_ = nullptr; modal_files_ = nullptr;
    modal_pos_setup_ = nullptr; kb_pos_setup_ = nullptr;
    modal_console_ = nullptr; console_scroll_ = nullptr; lbl_console_ = nullptr;
    fb_confirm_mbox_ = nullptr; fb_confirm_del_btn_ = nullptr;
    fb_confirm_switch_ = nullptr; fb_prog_bar_ = nullptr; fb_prog_lbl_ = nullptr;
    fb_select_mode_ = false; fb_delete_in_progress_ = false;
    fb_selected_.clear(); fb_pending_.clear();
    // Rebuild the main screen with the new palette (build_screens reassigns all its
    // widget pointers). The 1 s refresh repopulates live values on the next tick.
    lv_obj_clean(lv_screen_active());
    build_screens(disp_);
    bsp_display_unlock();
}

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
            lv_color_hex(kCritCol), 0);
        return;
    }

    WifiCredentials creds{ssid, pass, true};
    ui->storage_->save_wifi(creds);

    lv_label_set_text(ui->lbl_wifi_msg_, "Saved. Connecting...");
    lv_obj_set_style_text_color(ui->lbl_wifi_msg_,
        lv_color_hex(kWarnCol), 0);
    lv_obj_add_flag(ui->modal_wifi_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui->kb_wifi_, LV_OBJ_FLAG_HIDDEN);

    // update_credentials calls esp_wifi_stop/start/connect which go over SDIO
    // to the C6 coprocessor — must not run inside the LVGL task (display lock held)
    auto *arg = new WifiConnectArg{ui->wifi_, creds};
    xTaskCreate(wifi_connect_task, "wifi_conn", 4096, arg, 5, nullptr);
}

void Ui::ap_apply_task(void *arg) {
    // esp_wifi_set_config(WIFI_IF_AP) goes over SDIO to the C6 — keep it off the
    // LVGL task, which holds the display lock.
    static_cast<WifiManager *>(arg)->apply_ap_settings();
    vTaskDelete(nullptr);
}

void Ui::on_ap_save(lv_event_t *e) {
    auto *ui = static_cast<Ui *>(lv_event_get_user_data(e));
    const char *pw = lv_textarea_get_text(ui->ta_ap_pass_);
    const size_t len = pw ? strlen(pw) : 0;
    if (len < 8 || len > 63) {
        lv_label_set_text(ui->lbl_ap_msg_, "Password must be 8-63 characters");
        lv_obj_set_style_text_color(ui->lbl_ap_msg_,
            lv_color_hex(kCritCol), 0);
        return;
    }
    ui->storage_->save_ap_password(pw);
    lv_label_set_text(ui->lbl_ap_msg_,
        "Saved. Applying — reconnect AP clients with the new password.");
    lv_obj_set_style_text_color(ui->lbl_ap_msg_,
        lv_color_hex(kWarnCol), 0);
    lv_textarea_set_password_mode(ui->ta_ap_pass_, true);
    xTaskCreate(ap_apply_task, "ap_apply", 4096, ui->wifi_, 5, nullptr);
}

void Ui::on_wifi_list_click(lv_event_t *e) {
    auto *ui = static_cast<Ui *>(lv_event_get_user_data(e));
    lv_obj_t *btn = lv_event_get_target_obj(e);
    // The list button label contains the SSID – extract it
    const char *text = lv_list_get_button_text(ui->list_wifi_scan_, btn);
    if (!text) return;
    // Label format: "[lock] ssid  (rssi dBm)"
    const char *rssi_start = strstr(text, "  (");
    if (!rssi_start) return;
    const char *p = text;
    while (*p && (*p < 0x20 || *p > 0x7E)) p++; // skip UTF-8 symbols
    while (*p == ' ') p++;
    size_t ssid_len = (size_t)(rssi_start - p);
    if (ssid_len == 0 || ssid_len >= 33) return;
    char ssid[33];
    memcpy(ssid, p, ssid_len);
    ssid[ssid_len] = '\0';
    int l = (int)ssid_len - 1;
    while (l >= 0 && ssid[l] == ' ') ssid[l--] = '\0';
    lv_textarea_set_text(ui->ta_wifi_ssid_, ssid);
}

// ── Admin password modal callbacks ──────────────────────────────────────────────

void Ui::on_admin_btn(lv_event_t *e) {
    static_cast<Ui *>(lv_event_get_user_data(e))->open_admin_modal();
}

void Ui::on_admin_close(lv_event_t *e) {
    auto *ui = static_cast<Ui *>(lv_event_get_user_data(e));
    if (ui->modal_admin_) lv_obj_add_flag(ui->modal_admin_, LV_OBJ_FLAG_HIDDEN);
    if (ui->kb_admin_)    lv_obj_add_flag(ui->kb_admin_,    LV_OBJ_FLAG_HIDDEN);
}

void Ui::on_admin_save(lv_event_t *e) {
    auto *ui = static_cast<Ui *>(lv_event_get_user_data(e));
    const char *pw = lv_textarea_get_text(ui->ta_admin_pass_);
    const size_t len = pw ? strlen(pw) : 0;
    if (len < 4) {
        lv_label_set_text(ui->lbl_admin_msg_, "Password must be at least 4 characters");
        lv_obj_set_style_text_color(ui->lbl_admin_msg_, lv_color_hex(kCritCol), 0);
        return;
    }
    ui->storage_->save_admin_password(pw);
    lv_label_set_text(ui->lbl_admin_msg_, "Saved.");
    lv_obj_set_style_text_color(ui->lbl_admin_msg_, lv_color_hex(kGoodCol), 0);
    lv_textarea_set_text(ui->ta_admin_pass_, "");
    lv_obj_add_flag(ui->kb_admin_, LV_OBJ_FLAG_HIDDEN);
}

// ── Antenna edit modal callbacks ────────────────────────────────────────────────

void Ui::on_antenna_btn(lv_event_t *e) {
    static_cast<Ui *>(lv_event_get_user_data(e))->open_antenna_modal();
}

void Ui::on_antenna_close(lv_event_t *e) {
    auto *ui = static_cast<Ui *>(lv_event_get_user_data(e));
    if (ui->modal_ant_) lv_obj_add_flag(ui->modal_ant_, LV_OBJ_FLAG_HIDDEN);
    if (ui->kb_ant_)    lv_obj_add_flag(ui->kb_ant_,    LV_OBJ_FLAG_HIDDEN);
}

void Ui::on_antenna_save(lv_event_t *e) {
    auto *ui = static_cast<Ui *>(lv_event_get_user_data(e));
    const std::string model  = lv_textarea_get_text(ui->ta_ant_model_);
    const std::string radome = lv_textarea_get_text(ui->ta_ant_radome_);
    const double height = atof(lv_textarea_get_text(ui->ta_ant_height_));
    ui->storage_->save_antenna(model.empty() ? "HXCGPS500" : model,
                               radome.empty() ? "NONE" : radome, height);
    lv_label_set_text(ui->lbl_ant_msg_, "Saved.");
    lv_obj_set_style_text_color(ui->lbl_ant_msg_, lv_color_hex(kGoodCol), 0);
    lv_obj_add_flag(ui->kb_ant_, LV_OBJ_FLAG_HIDDEN);
}

// ── Position Setup modal ─────────────────────────────────────────────────────────
// Single place to set the base position: manual entry, survey start, RINEX toggle,
// and antenna metadata (canonical entry, moved here from System). Opened from the
// Dashboard Position tile.

void Ui::build_position_setup() {
    modal_pos_setup_ = make_modal_base(lv_layer_top(), "Position Setup");

    lv_obj_t *bar = lv_obj_get_child(modal_pos_setup_, 0);
    lv_obj_t *close_btn = lv_obj_get_child(bar, lv_obj_get_child_count(bar) - 1);
    lv_obj_add_event_cb(close_btn, on_pos_setup_close, LV_EVENT_CLICKED, this);

    // Shared keyboard (hidden until a textarea is focused).
    kb_pos_setup_ = lv_keyboard_create(modal_pos_setup_);
    lv_obj_set_size(kb_pos_setup_, Display::kWidth, Display::kHeight * 40 / 100);
    lv_obj_add_flag(kb_pos_setup_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(kb_pos_setup_, on_kb_ready, LV_EVENT_READY,  nullptr);
    lv_obj_add_event_cb(kb_pos_setup_, on_kb_ready, LV_EVENT_CANCEL, nullptr);

    // Scrollable content container (above the keyboard).
    lv_obj_t *cont = lv_obj_create(modal_pos_setup_);
    lv_obj_remove_style_all(cont);
    lv_obj_set_width(cont, LV_PCT(100));
    lv_obj_set_flex_grow(cont, 1);
    lv_obj_set_style_pad_hor(cont, 16, 0);
    lv_obj_set_style_pad_ver(cont, 10, 0);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                           LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(cont, 8, 0);
    lv_obj_set_scroll_dir(cont, LV_DIR_VER);

    auto section_hdr = [&](const char *text) {
        lv_obj_t *h = lv_label_create(cont);
        lv_label_set_text(h, text);
        lv_obj_set_style_text_color(h, lv_color_hex(kTitleCol), 0);
        lv_obj_set_style_text_font(h, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_letter_space(h, 1, 0);
        lv_obj_set_style_pad_top(h, 8, 0);
    };
    auto field = [&](const char *cap, lv_obj_t **ta, const char *ph) {
        lv_obj_t *l = lv_label_create(cont);
        lv_label_set_text(l, cap);
        lv_obj_set_style_text_color(l, lv_color_hex(kKeyCol), 0);
        *ta = lv_textarea_create(cont);
        lv_textarea_set_one_line(*ta, true);
        lv_obj_set_width(*ta, LV_PCT(100));
        lv_textarea_set_placeholder_text(*ta, ph);
        lv_obj_add_event_cb(*ta, on_ta_focused,   LV_EVENT_FOCUSED,   kb_pos_setup_);
        lv_obj_add_event_cb(*ta, on_ta_defocused, LV_EVENT_DEFOCUSED, kb_pos_setup_);
    };

    // ── MANUAL POSITION ──
    section_hdr("MANUAL POSITION");
    field("Latitude (deg):",  &ta_ps_lat_,    "0.0000000");
    field("Longitude (deg):", &ta_ps_lon_,    "0.0000000");
    field("Height (m):",      &ta_ps_height_, "0.0000");

    lv_obj_t *pos_save = lv_button_create(cont);
    lv_obj_set_size(pos_save, 220, 44);
    lv_obj_t *pos_save_lbl = lv_label_create(pos_save);
    lv_label_set_text(pos_save_lbl, LV_SYMBOL_SAVE "  Save Position");
    lv_obj_center(pos_save_lbl);
    lv_obj_add_event_cb(pos_save, on_ps_pos_save, LV_EVENT_CLICKED, this);

    lbl_ps_pos_msg_ = lv_label_create(cont);
    lv_label_set_text(lbl_ps_pos_msg_, "");
    lv_obj_set_style_text_color(lbl_ps_pos_msg_, lv_color_hex(kDimCol), 0);

    // ── SURVEY ──
    section_hdr("SURVEY");
    lv_obj_t *sv_btn = lv_button_create(cont);
    lv_obj_set_size(sv_btn, LV_PCT(100), 48);
    lv_obj_set_style_bg_color(sv_btn, lv_color_hex(kWarnCol), 0);
    lv_obj_t *sv_lbl = lv_label_create(sv_btn);
    lv_label_set_text(sv_lbl, LV_SYMBOL_REFRESH "  Start / Restart Survey");
    lv_obj_set_style_text_color(sv_lbl, lv_color_hex(kBgScreen), 0);
    lv_obj_center(sv_lbl);
    // Reuse the existing survey flow (same confirm when in Base TX).
    lv_obj_add_event_cb(sv_btn, on_survey_start, LV_EVENT_CLICKED, this);

    // ── RINEX COLLECTION ──
    section_hdr("RINEX COLLECTION");
    lv_obj_t *rx_row = lv_obj_create(cont);
    lv_obj_remove_style_all(rx_row);
    lv_obj_set_size(rx_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(rx_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(rx_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                           LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lbl_ps_rinex_ = lv_label_create(rx_row);
    lv_label_set_text(lbl_ps_rinex_, "RINEX logging");
    lv_obj_set_style_text_color(lbl_ps_rinex_, lv_color_hex(kKeyCol), 0);
    lv_obj_set_flex_grow(lbl_ps_rinex_, 1);
    sw_ps_rinex_ = lv_switch_create(rx_row);
    lv_obj_add_event_cb(sw_ps_rinex_, on_rinex_toggle, LV_EVENT_VALUE_CHANGED, this);

    // ── ANTENNA (canonical entry) ──
    section_hdr("ANTENNA");
    field("Antenna model (IGS/NGS name):", &ta_ps_ant_model_,  "HXCGPS500");
    field("Radome:",                       &ta_ps_ant_radome_, "NONE");
    field("ARP height (metres):",          &ta_ps_ant_height_, "0.0");

    lv_obj_t *ant_save = lv_button_create(cont);
    lv_obj_set_size(ant_save, 220, 44);
    lv_obj_t *ant_save_lbl = lv_label_create(ant_save);
    lv_label_set_text(ant_save_lbl, LV_SYMBOL_SAVE "  Save Antenna");
    lv_obj_center(ant_save_lbl);
    lv_obj_add_event_cb(ant_save, on_ps_ant_save, LV_EVENT_CLICKED, this);

    lbl_ps_ant_msg_ = lv_label_create(cont);
    lv_label_set_text(lbl_ps_ant_msg_, "");
    lv_obj_set_style_text_color(lbl_ps_ant_msg_, lv_color_hex(kDimCol), 0);
}

void Ui::open_position_setup() {
    if (!modal_pos_setup_) build_position_setup();

    // Prefill manual position from the stored fixed position.
    const BasePosition bpos = storage_->load_position();
    char b[32];
    if (bpos.valid) {
        snprintf(b, sizeof(b), "%.7f", bpos.lat);
        lv_textarea_set_text(ta_ps_lat_, b);
        snprintf(b, sizeof(b), "%.7f", bpos.lon);
        lv_textarea_set_text(ta_ps_lon_, b);
        snprintf(b, sizeof(b), "%.4f", bpos.height);
        lv_textarea_set_text(ta_ps_height_, b);
    } else {
        lv_textarea_set_text(ta_ps_lat_, "");
        lv_textarea_set_text(ta_ps_lon_, "");
        lv_textarea_set_text(ta_ps_height_, "");
    }
    lv_label_set_text(lbl_ps_pos_msg_, "");

    // RINEX switch reflects the live collection state.
    const RinexLogger::Status rx = station_->rinex_status();
    if (rx.active) lv_obj_add_state(sw_ps_rinex_, LV_STATE_CHECKED);
    else           lv_obj_clear_state(sw_ps_rinex_, LV_STATE_CHECKED);

    // Antenna fields from storage.
    lv_textarea_set_text(ta_ps_ant_model_,  storage_->antenna_model().c_str());
    lv_textarea_set_text(ta_ps_ant_radome_, storage_->antenna_radome().c_str());
    snprintf(b, sizeof(b), "%.4f", storage_->antenna_height());
    lv_textarea_set_text(ta_ps_ant_height_, b);
    lv_label_set_text(lbl_ps_ant_msg_, "");

    lv_obj_add_flag(kb_pos_setup_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(modal_pos_setup_, LV_OBJ_FLAG_HIDDEN);
}

void Ui::on_pos_setup_tile(lv_event_t *e) {
    static_cast<Ui *>(lv_event_get_user_data(e))->open_position_setup();
}

void Ui::on_pos_setup_close(lv_event_t *e) {
    auto *ui = static_cast<Ui *>(lv_event_get_user_data(e));
    if (ui->modal_pos_setup_) lv_obj_add_flag(ui->modal_pos_setup_, LV_OBJ_FLAG_HIDDEN);
    if (ui->kb_pos_setup_)    lv_obj_add_flag(ui->kb_pos_setup_,    LV_OBJ_FLAG_HIDDEN);
}

void Ui::on_ps_pos_save(lv_event_t *e) {
    auto *ui = static_cast<Ui *>(lv_event_get_user_data(e));
    const char *lat_s = lv_textarea_get_text(ui->ta_ps_lat_);
    const char *lon_s = lv_textarea_get_text(ui->ta_ps_lon_);
    const char *h_s   = lv_textarea_get_text(ui->ta_ps_height_);

    char *lat_end = nullptr, *lon_end = nullptr, *h_end = nullptr;
    double lat = strtod(lat_s ? lat_s : "", &lat_end);
    double lon = strtod(lon_s ? lon_s : "", &lon_end);
    double height = strtod(h_s ? h_s : "", &h_end);

    bool ok = lat_s && *lat_s && lat_end != lat_s &&
              lon_s && *lon_s && lon_end != lon_s &&
              h_s   && *h_s   && h_end   != h_s   &&
              lat >= -90.0 && lat <= 90.0 &&
              lon >= -180.0 && lon <= 180.0;
    if (!ok) {
        lv_label_set_text(ui->lbl_ps_pos_msg_,
                          "Enter valid lat/lon/height numbers.");
        lv_obj_set_style_text_color(ui->lbl_ps_pos_msg_, lv_color_hex(kCritCol), 0);
        return;
    }

    ui->station_->request_position(lat, lon, height);
    lv_label_set_text(ui->lbl_ps_pos_msg_, "Saved. Switching to base mode\xe2\x80\xa6");
    lv_obj_set_style_text_color(ui->lbl_ps_pos_msg_, lv_color_hex(kGoodCol), 0);
    lv_obj_add_flag(ui->kb_pos_setup_, LV_OBJ_FLAG_HIDDEN);
}

void Ui::on_ps_ant_save(lv_event_t *e) {
    auto *ui = static_cast<Ui *>(lv_event_get_user_data(e));
    const std::string model  = lv_textarea_get_text(ui->ta_ps_ant_model_);
    const std::string radome = lv_textarea_get_text(ui->ta_ps_ant_radome_);
    const double height = atof(lv_textarea_get_text(ui->ta_ps_ant_height_));
    ui->storage_->save_antenna(model.empty() ? "HXCGPS500" : model,
                               radome.empty() ? "NONE" : radome, height);
    lv_label_set_text(ui->lbl_ps_ant_msg_, "Saved.");
    lv_obj_set_style_text_color(ui->lbl_ps_ant_msg_, lv_color_hex(kGoodCol), 0);
    lv_obj_add_flag(ui->kb_pos_setup_, LV_OBJ_FLAG_HIDDEN);
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
    if (ui->fb_select_mode_) ui->exit_select_mode();
    lv_obj_add_flag(ui->modal_files_, LV_OBJ_FLAG_HIDDEN);
}

void Ui::on_fb_item(lv_event_t *e) {
    auto *ui = static_cast<Ui *>(lv_event_get_user_data(e));
    size_t idx = (size_t)(uintptr_t)lv_obj_get_user_data(lv_event_get_target_obj(e));
    if (idx >= ui->fb_entries_.size()) return;

    // In selection mode a tap toggles selection (no navigation).
    if (ui->fb_select_mode_) {
        ui->fb_selected_[idx] = !ui->fb_selected_[idx];
        ui->update_fb_selection();
        return;
    }

    const std::string &path = ui->fb_entries_[idx];
    if (idx < ui->fb_is_dir_.size() && ui->fb_is_dir_[idx]) {
        ui->open_file_browser(path);
    }
    // Files: tap outside selection mode does nothing.
}

// Long-press any row → enter selection mode with that row selected (design §3.1).
void Ui::on_fb_item_long(lv_event_t *e) {
    auto *ui = static_cast<Ui *>(lv_event_get_user_data(e));
    size_t idx = (size_t)(uintptr_t)lv_obj_get_user_data(lv_event_get_target_obj(e));
    if (idx >= ui->fb_entries_.size()) return;
    if (!ui->fb_select_mode_) {
        ui->enter_select_mode((int)idx);
    } else {
        ui->fb_selected_[idx] = !ui->fb_selected_[idx];
        ui->update_fb_selection();
    }
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

// ── Bulk-delete: selection mode ─────────────────────────────────────────────────

void Ui::enter_select_mode(int seed_idx) {
    if (fb_entries_.empty()) return;
    fb_select_mode_ = true;
    std::fill(fb_selected_.begin(), fb_selected_.end(), false);
    if (seed_idx >= 0 && (size_t)seed_idx < fb_selected_.size())
        fb_selected_[seed_idx] = true;
    if (fb_action_bar_) lv_obj_clear_flag(fb_action_bar_, LV_OBJ_FLAG_HIDDEN);
    update_fb_selection();
}

void Ui::exit_select_mode() {
    fb_select_mode_ = false;
    std::fill(fb_selected_.begin(), fb_selected_.end(), false);
    if (fb_action_bar_) lv_obj_add_flag(fb_action_bar_, LV_OBJ_FLAG_HIDDEN);
    update_fb_selection();  // clears the per-row highlight cues
}

// Redraw selection cues (row bg + check glyph), count + size, Delete(N) state.
void Ui::update_fb_selection() {
    if (!list_fb_) return;

    // Per-row visual state: --surface-hi bg + a leading --accent check (redundant
    // cue), keyed to the button's stored entry index.
    uint32_t n = lv_obj_get_child_count(list_fb_);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *btn = lv_obj_get_child(list_fb_, (int32_t)i);
        if (!lv_obj_check_type(btn, &lv_list_button_class)) continue;
        size_t idx = (size_t)(uintptr_t)lv_obj_get_user_data(btn);
        if (idx >= fb_selected_.size()) continue;
        bool sel = fb_select_mode_ && fb_selected_[idx];
        lv_obj_set_style_bg_color(btn,
            lv_color_hex(sel ? kSurfaceHi : kBgGroup), 0);

        // Leading check marker (created lazily, identified by user_data == this btn).
        lv_obj_t *chk = nullptr;
        uint32_t cc = lv_obj_get_child_count(btn);
        for (uint32_t c = 0; c < cc; c++) {
            lv_obj_t *child = lv_obj_get_child(btn, (int32_t)c);
            if (lv_obj_get_user_data(child) == (void *)btn) { chk = child; break; }
        }
        if (fb_select_mode_) {
            if (!chk) {
                chk = lv_label_create(btn);
                lv_obj_set_user_data(chk, (void *)btn);
                lv_obj_add_flag(chk, LV_OBJ_FLAG_FLOATING);  // outside flex layout
                lv_label_set_text(chk, LV_SYMBOL_OK);
                lv_obj_set_size(chk, 40, 44);
                lv_obj_align(chk, LV_ALIGN_LEFT_MID, 2, 0);
                lv_obj_set_style_text_align(chk, LV_TEXT_ALIGN_CENTER, 0);
            }
            lv_obj_set_style_text_color(chk,
                lv_color_hex(sel ? kAccentCol : kBorderCol), 0);
            lv_label_set_text(chk, sel ? LV_SYMBOL_OK : LV_SYMBOL_BULLET);
            lv_obj_clear_flag(chk, LV_OBJ_FLAG_HIDDEN);
        } else if (chk) {
            lv_obj_add_flag(chk, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (!fb_select_mode_) return;

    // Count + size readout. Size from a bounded inline preview; "…" if truncated.
    uint32_t sel_count = 0;
    std::vector<std::string> paths;
    for (size_t i = 0; i < fb_selected_.size(); i++) {
        if (fb_selected_[i]) { sel_count++; paths.push_back(fb_entries_[i]); }
    }

    char count_buf[64];
    if (sel_count == 0) {
        snprintf(count_buf, sizeof(count_buf), "0 selected");
    } else {
        std::vector<const char *> cptrs;
        cptrs.reserve(paths.size());
        for (const auto &p : paths) cptrs.push_back(p.c_str());
        SdManager::DeletePreview pv =
            sd_delete::preview(cptrs.data(), cptrs.size());
        if (pv.ok && !pv.truncated) {
            char sz[16];
            fmt_bytes_str(sz, sizeof(sz), pv.bytes);
            snprintf(count_buf, sizeof(count_buf), "%u selected \xc2\xb7 %s",
                     (unsigned)sel_count, sz);
        } else {
            snprintf(count_buf, sizeof(count_buf),
                     "%u selected \xc2\xb7 \xe2\x80\xa6", (unsigned)sel_count);
        }
    }
    if (lbl_fb_count_) lv_label_set_text(lbl_fb_count_, count_buf);

    if (lbl_fb_delete_) {
        char del_buf[32];
        snprintf(del_buf, sizeof(del_buf), LV_SYMBOL_TRASH "  %s (%u)",
                 ui_sections::del::kDelete, (unsigned)sel_count);
        lv_label_set_text(lbl_fb_delete_, del_buf);
    }
    if (btn_fb_delete_) {
        if (sel_count == 0) lv_obj_add_state(btn_fb_delete_, LV_STATE_DISABLED);
        else                lv_obj_clear_state(btn_fb_delete_, LV_STATE_DISABLED);
    }
}

void Ui::on_fb_select_btn(lv_event_t *e) {
    auto *ui = static_cast<Ui *>(lv_event_get_user_data(e));
    if (ui->fb_select_mode_) ui->exit_select_mode();
    else                     ui->enter_select_mode(-1);
}

void Ui::on_fb_selall_btn(lv_event_t *e) {
    auto *ui = static_cast<Ui *>(lv_event_get_user_data(e));
    if (!ui->fb_select_mode_) return;
    // Toggle: if everything is already selected, clear; else select all.
    bool all = !ui->fb_selected_.empty();
    for (bool b : ui->fb_selected_) if (!b) { all = false; break; }
    std::fill(ui->fb_selected_.begin(), ui->fb_selected_.end(), !all);
    ui->update_fb_selection();
}

void Ui::on_fb_cancel_btn(lv_event_t *e) {
    static_cast<Ui *>(lv_event_get_user_data(e))->exit_select_mode();
}

// ── Bulk-delete: confirm trigger (batch + per-folder) ────────────────────────────

void Ui::on_fb_delete_btn(lv_event_t *e) {
    auto *ui = static_cast<Ui *>(lv_event_get_user_data(e));
    if (!ui->fb_select_mode_) return;
    std::vector<std::string> paths;
    for (size_t i = 0; i < ui->fb_selected_.size(); i++)
        if (ui->fb_selected_[i]) paths.push_back(ui->fb_entries_[i]);
    if (paths.empty()) return;
    ui->open_delete_confirm(paths, /*folder=*/false, /*folder_name=*/"",
                            /*managed=*/false);
}

void Ui::on_fb_folder_btn(lv_event_t *e) {
    auto *ui = static_cast<Ui *>(lv_event_get_user_data(e));
    size_t idx = (size_t)(uintptr_t)lv_obj_get_user_data(lv_event_get_target_obj(e));
    if (idx >= ui->fb_entries_.size()) return;
    const std::string &path = ui->fb_entries_[idx];
    bool managed =
        (SdManager::check_deletable(path.c_str()) == SdManager::DeleteGuard::kManagedDir);
    size_t slash = path.rfind('/');
    std::string name = (slash == std::string::npos) ? path : path.substr(slash + 1);
    std::vector<std::string> paths{path};
    ui->open_delete_confirm(paths, /*folder=*/true, name, managed);
}

// ── Bulk-delete: confirm / progress dialog (lv_msgbox on lv_layer_top) ───────────

void Ui::open_delete_confirm(const std::vector<std::string> &paths, bool folder,
                             const std::string &folder_name, bool managed) {
    if (paths.empty()) return;
    if (fb_confirm_mbox_) return;       // one dialog at a time
    if (sd_delete::busy()) return;      // a job is already running

    std::vector<const char *> cptrs;
    cptrs.reserve(paths.size());
    for (const auto &p : paths) cptrs.push_back(p.c_str());

    // Active-RINEX-file guard: replace the confirm with a non-destructive info box.
    char active_msg[160];
    if (sd_delete::blocks_active_file(cptrs.data(), cptrs.size(),
                                      active_msg, sizeof(active_msg))) {
        lv_obj_t *info = lv_msgbox_create(lv_layer_top());
        lv_msgbox_add_title(info, "Cannot delete");
        lv_msgbox_add_text(info, active_msg);
        lv_obj_t *ok = lv_msgbox_add_footer_button(info, "OK");
        lv_obj_set_user_data(ok, this);
        lv_obj_add_event_cb(ok, on_fb_confirm_cancel, LV_EVENT_CLICKED, info);
        lv_obj_set_width(info, 480);
        lv_obj_center(info);
        return;
    }

    fb_pending_ = paths;

    // Bounded preview drives the count + size in the body.
    SdManager::DeletePreview pv = sd_delete::preview(cptrs.data(), cptrs.size());
    char sz[20];
    fmt_bytes_str(sz, sizeof(sz), pv.bytes);
    const char *plus = pv.truncated ? "+" : "";

    // Title (mirrors the web wording exactly).
    char title[160];
    if (folder) {
        snprintf(title, sizeof(title), "%s \"%s\"?",
                 managed ? "Empty folder" : "Delete folder", folder_name.c_str());
    } else {
        unsigned items = pv.files + pv.dirs;
        snprintf(title, sizeof(title), "Delete %u item%s?",
                 items, items == 1 ? "" : "s");
    }

    // Body: count + size — irreversibility line(s).
    char body[320];
    if (folder) {
        if (pv.dirs > 0) {
            snprintf(body, sizeof(body),
                     "%u file%s \xc2\xb7 %u subfolder%s \xc2\xb7 %s%s\n"
                     "Everything inside is permanently erased. %s%s%s",
                     (unsigned)pv.files, pv.files == 1 ? "" : "s",
                     (unsigned)pv.dirs, pv.dirs == 1 ? "" : "s", sz, plus,
                     ui_sections::del::kCannotUndo,
                     managed ? "\n" : "",
                     managed ? ui_sections::del::kFolderKept : "");
        } else {
            snprintf(body, sizeof(body),
                     "%u file%s \xc2\xb7 %s%s\n"
                     "Everything inside is permanently erased. %s%s%s",
                     (unsigned)pv.files, pv.files == 1 ? "" : "s", sz, plus,
                     ui_sections::del::kCannotUndo,
                     managed ? "\n" : "",
                     managed ? ui_sections::del::kFolderKept : "");
        }
    } else {
        unsigned items = pv.files + pv.dirs;
        snprintf(body, sizeof(body), "%u item%s \xc2\xb7 %s%s \xe2\x80\x94 %s",
                 items, items == 1 ? "" : "s", sz, plus,
                 ui_sections::del::kCannotUndo);
    }

    // Extra friction: any non-empty directory in scope requires the "I understand"
    // toggle (touch equivalent of the web typed-DELETE gate). For a folder op that
    // means it contains anything; for a batch it means a selected dir has contents.
    // Mirror the web typed-DELETE gate exactly: a folder op whose target has any
    // contents, or a batch that pulled in a directory containing subfolders.
    bool non_empty_dir;
    if (folder) {
        non_empty_dir = (pv.files > 0 || pv.dirs > 0);
    } else {
        bool dir_in_scope = false;
        for (const auto &p : paths) {
            struct stat st{};
            if (stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
                dir_in_scope = true; break;
            }
        }
        non_empty_dir = dir_in_scope && (pv.dirs > 0);
    }

    lv_obj_t *mbox = lv_msgbox_create(lv_layer_top());
    fb_confirm_mbox_ = mbox;
    lv_msgbox_add_title(mbox, title);
    lv_msgbox_add_text(mbox, body);

    // Progress widgets (hidden until the job starts) live in the content area.
    lv_obj_t *content = lv_msgbox_get_content(mbox);
    fb_prog_lbl_ = lv_label_create(content);
    lv_label_set_text(fb_prog_lbl_, "");
    lv_obj_add_flag(fb_prog_lbl_, LV_OBJ_FLAG_HIDDEN);
    fb_prog_bar_ = lv_bar_create(content);
    lv_obj_set_size(fb_prog_bar_, LV_PCT(100), 16);
    lv_bar_set_range(fb_prog_bar_, 0, 100);
    lv_bar_set_value(fb_prog_bar_, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(fb_prog_bar_, lv_color_hex(kAccentCol),
                              LV_PART_INDICATOR);
    lv_obj_add_flag(fb_prog_bar_, LV_OBJ_FLAG_HIDDEN);

    // "I understand this is permanent" toggle (gates Delete for non-empty dirs).
    fb_confirm_switch_ = nullptr;
    if (non_empty_dir) {
        lv_obj_t *row = lv_obj_create(content);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                               LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, 12, 0);
        lv_obj_set_style_pad_top(row, 8, 0);
        fb_confirm_switch_ = lv_switch_create(row);
        lv_obj_add_event_cb(fb_confirm_switch_, on_fb_confirm_switch,
                            LV_EVENT_VALUE_CHANGED, this);
        lv_obj_t *sl = lv_label_create(row);
        lv_label_set_text(sl, "I understand this is permanent");
        lv_obj_set_flex_grow(sl, 1);
        lv_label_set_long_mode(sl, LV_LABEL_LONG_WRAP);
    }

    // Footer: Cancel (neutral, first focus) · Delete (--crit, never default).
    lv_obj_t *cancel = lv_msgbox_add_footer_button(mbox, ui_sections::del::kCancel);
    lv_obj_set_user_data(cancel, this);
    lv_obj_add_event_cb(cancel, on_fb_confirm_cancel, LV_EVENT_CLICKED, mbox);

    const char *del_label = ui_sections::del::kDelete;
    if (folder) del_label = managed ? ui_sections::del::kEmptyFolder
                                    : ui_sections::del::kDeleteFolder;
    fb_confirm_del_btn_ = lv_msgbox_add_footer_button(mbox, del_label);
    lv_obj_set_style_bg_color(fb_confirm_del_btn_, lv_color_hex(kCritCol), 0);
    lv_obj_set_user_data(fb_confirm_del_btn_, this);
    lv_obj_add_event_cb(fb_confirm_del_btn_, on_fb_confirm_del, LV_EVENT_CLICKED, mbox);
    if (non_empty_dir)
        lv_obj_add_state(fb_confirm_del_btn_, LV_STATE_DISABLED);

    lv_obj_set_width(mbox, 480);
    lv_obj_center(mbox);
}

void Ui::on_fb_confirm_switch(lv_event_t *e) {
    auto *ui = static_cast<Ui *>(lv_event_get_user_data(e));
    lv_obj_t *sw = lv_event_get_target_obj(e);
    if (!ui->fb_confirm_del_btn_) return;
    if (lv_obj_has_state(sw, LV_STATE_CHECKED))
        lv_obj_clear_state(ui->fb_confirm_del_btn_, LV_STATE_DISABLED);
    else
        lv_obj_add_state(ui->fb_confirm_del_btn_, LV_STATE_DISABLED);
}

void Ui::on_fb_confirm_del(lv_event_t *e) {
    lv_obj_t *mbox = static_cast<lv_obj_t *>(lv_event_get_user_data(e));
    lv_obj_t *btn  = lv_event_get_target_obj(e);
    auto *ui = static_cast<Ui *>(lv_obj_get_user_data(btn));
    if (!ui || ui->fb_pending_.empty()) { lv_msgbox_close(mbox); return; }
    if (lv_obj_has_state(btn, LV_STATE_DISABLED)) return;

    std::vector<const char *> cptrs;
    cptrs.reserve(ui->fb_pending_.size());
    for (const auto &p : ui->fb_pending_) cptrs.push_back(p.c_str());

    if (!sd_delete::start(cptrs.data(), cptrs.size())) {
        // Refused (busy / unmounted / active file appeared). Show why and bail.
        if (ui->fb_prog_lbl_) {
            lv_label_set_text(ui->fb_prog_lbl_, "Delete could not start.");
            lv_obj_clear_flag(ui->fb_prog_lbl_, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    // Transform the confirm into a progress view: disable footer, show bar+text.
    ui->fb_delete_in_progress_ = true;
    if (ui->fb_confirm_del_btn_)
        lv_obj_add_state(ui->fb_confirm_del_btn_, LV_STATE_DISABLED);
    if (ui->fb_confirm_switch_)
        lv_obj_add_state(ui->fb_confirm_switch_, LV_STATE_DISABLED);
    if (ui->fb_prog_lbl_) {
        SdManager::DeletePreview pv =
            sd_delete::preview(cptrs.data(), cptrs.size());
        char t[48];
        snprintf(t, sizeof(t), "Deleting 0 / %u\xe2\x80\xa6",
                 (unsigned)(pv.files + pv.dirs));
        lv_label_set_text(ui->fb_prog_lbl_, t);
        lv_obj_clear_flag(ui->fb_prog_lbl_, LV_OBJ_FLAG_HIDDEN);
    }
    if (ui->fb_prog_bar_) {
        lv_bar_set_value(ui->fb_prog_bar_, 0, LV_ANIM_OFF);
        lv_obj_clear_flag(ui->fb_prog_bar_, LV_OBJ_FLAG_HIDDEN);
    }
}

void Ui::on_fb_confirm_cancel(lv_event_t *e) {
    lv_obj_t *mbox = static_cast<lv_obj_t *>(lv_event_get_user_data(e));
    lv_obj_t *btn  = lv_event_get_target_obj(e);
    auto *ui = static_cast<Ui *>(lv_obj_get_user_data(btn));
    if (ui && mbox == ui->fb_confirm_mbox_) {
        // Don't allow cancel to dismiss a running job's progress view.
        if (ui->fb_delete_in_progress_) return;
        ui->fb_confirm_mbox_ = nullptr;
        ui->fb_confirm_del_btn_ = nullptr;
        ui->fb_confirm_switch_ = nullptr;
        ui->fb_prog_bar_ = nullptr;
        ui->fb_prog_lbl_ = nullptr;
        ui->fb_pending_.clear();
    }
    lv_msgbox_close(mbox);
}

// Drive the progress view from the 1 s refresh() tick (mirrors c6_ota readout).
void Ui::poll_delete_progress() {
    if (!fb_delete_in_progress_) return;
    sd_delete::Progress p = sd_delete::poll();

    if (p.state == sd_delete::State::kRunning) {
        if (fb_prog_bar_) {
            int pct = p.total > 0 ? (int)(p.done * 100 / p.total) : 0;
            lv_bar_set_value(fb_prog_bar_, pct, LV_ANIM_OFF);
        }
        if (fb_prog_lbl_) {
            char t[48];
            snprintf(t, sizeof(t), "Deleting %u / %u\xe2\x80\xa6",
                     (unsigned)p.done, (unsigned)p.total);
            lv_label_set_text(fb_prog_lbl_, t);
        }
        return;
    }

    if (p.state == sd_delete::State::kDoneOk ||
        p.state == sd_delete::State::kDoneFail) {
        if (fb_prog_bar_) lv_bar_set_value(fb_prog_bar_, 100, LV_ANIM_OFF);
        if (fb_prog_lbl_) {
            char t[64];
            if (p.result.failed > 0) {
                snprintf(t, sizeof(t), "Deleted %u, %u failed.",
                         (unsigned)p.result.deleted, (unsigned)p.result.failed);
                lv_obj_set_style_text_color(fb_prog_lbl_, lv_color_hex(kWarnCol), 0);
                if (fb_prog_bar_)
                    lv_obj_set_style_bg_color(fb_prog_bar_, lv_color_hex(kWarnCol),
                                              LV_PART_INDICATOR);
            } else {
                snprintf(t, sizeof(t), "Deleted %u\xc2\xa0item%s.",
                         (unsigned)p.result.deleted,
                         p.result.deleted == 1 ? "" : "s");
                lv_obj_set_style_text_color(fb_prog_lbl_, lv_color_hex(kGoodCol), 0);
            }
            lv_label_set_text(fb_prog_lbl_, t);
        }
        // Tear down the dialog and re-list (selection cleared by refresh_file_browser).
        fb_delete_in_progress_ = false;
        if (fb_confirm_mbox_) {
            lv_msgbox_close(fb_confirm_mbox_);
            fb_confirm_mbox_ = nullptr;
            fb_confirm_del_btn_ = nullptr;
            fb_confirm_switch_ = nullptr;
            fb_prog_bar_ = nullptr;
            fb_prog_lbl_ = nullptr;
        }
        fb_pending_.clear();
        refresh_file_browser();  // re-list; Storage disk arc refreshes next tick
    }
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
        static const char *btns[] = {"Yes", "No", ""};
        lv_obj_t *mbox = lv_msgbox_create(lv_layer_top());
        lv_msgbox_add_title(mbox, "Start New Survey?");
        lv_msgbox_add_text(mbox, "This will reset the current fixed position and begin a new accuracy survey.");
        lv_msgbox_add_footer_button(mbox, "Yes");
        lv_msgbox_add_footer_button(mbox, "Cancel");
        lv_obj_set_width(mbox, 480);
        lv_obj_center(mbox);
        (void)btns;  // suppress unused warning

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

// Global worst-of-all status pill: red ALARM if base unhealthy, amber SURVEYING
// while in survey-in, else green LIVE.
void Ui::update_header(const BaseStationStatus &st) {
    if (!header_pill_dot_ || !header_pill_lbl_) return;
    uint32_t col;
    const char *word;
    if (!station_->healthy()) {
        col = kCritCol; word = "ALARM";
    } else if (st.mode == BaseMode::kSurvey) {
        col = kWarnCol; word = "SURVEYING";
    } else {
        col = kGoodCol; word = "LIVE";
    }
    lv_obj_set_style_bg_color(header_pill_dot_, lv_color_hex(col), 0);
    lv_label_set_text(header_pill_lbl_, word);
    lv_obj_set_style_text_color(header_pill_lbl_, lv_color_hex(col), 0);
}

void Ui::refresh() {
    char buf[192];
    const BaseStationStatus st = station_->status();
    const RinexLogger::Status rx = station_->rinex_status();
    const SurveySnapshot &sv = st.survey;

    update_header(st);

    // ── Mode + survey button label ────────────────────────────────────────────
    if (st.mode == BaseMode::kSurvey) {
        lv_label_set_text(lbl_t_mode_, "SURVEY");
        lv_obj_set_style_text_color(lbl_t_mode_, lv_color_hex(kWarnCol), 0);
        lv_label_set_text(lbl_survey_btn_, LV_SYMBOL_REFRESH "  Restart Survey");
    } else {
        lv_label_set_text(lbl_t_mode_, "BASE TX");
        lv_obj_set_style_text_color(lbl_t_mode_, lv_color_hex(kGoodCol), 0);
        lv_label_set_text(lbl_survey_btn_, LV_SYMBOL_REFRESH "  Start New Survey");
    }

    // Sync global NTRIP toggle without retriggering the callback
    if (sw_ntrip_all_) {
        bool enabled = station_->streams_enabled();
        bool sw_on   = lv_obj_has_state(sw_ntrip_all_, LV_STATE_CHECKED);
        if (enabled != sw_on) {
            if (enabled) lv_obj_add_state(sw_ntrip_all_, LV_STATE_CHECKED);
            else         lv_obj_clear_state(sw_ntrip_all_, LV_STATE_CHECKED);
        }
    }

    // ── Dashboard hero: RTCM output (or survey stability when surveying) ───────
    if (st.mode == BaseMode::kSurvey) {
        snprintf(buf, sizeof(buf), "\xc2\xb1%.2f", (double)sv.stability);
        lv_label_set_text(lbl_hero_val_, buf);
        lv_obj_set_style_text_color(lbl_hero_val_, lv_color_hex(kWarnCol), 0);
        snprintf(buf, sizeof(buf), "metres  \xc2\xb7  surveying %lus",
                 (unsigned long)sv.elapsed_sec);
        lv_label_set_text(lbl_hero_sub_, buf);
    } else if (st.rtcm_bytes_per_second > 0) {
        snprintf(buf, sizeof(buf), "%lu B/s",
                 (unsigned long)st.rtcm_bytes_per_second);
        lv_label_set_text(lbl_hero_val_, buf);
        lv_obj_set_style_text_color(lbl_hero_val_, lv_color_hex(kGoodCol), 0);
        char tot[20];
        fmt_bytes_str(tot, sizeof(tot), st.rtcm_bytes_total);
        snprintf(buf, sizeof(buf), "total %s", tot);
        lv_label_set_text(lbl_hero_sub_, buf);
    } else {
        lv_label_set_text(lbl_hero_val_, "0 B/s");
        lv_obj_set_style_text_color(lbl_hero_val_, lv_color_hex(kDimCol), 0);
        char tot[20];
        fmt_bytes_str(tot, sizeof(tot), st.rtcm_bytes_total);
        snprintf(buf, sizeof(buf), "idle  \xc2\xb7  total %s", tot);
        lv_label_set_text(lbl_hero_sub_, buf);
    }

    // ── Dashboard tiles ───────────────────────────────────────────────────────
    snprintf(buf, sizeof(buf), "%d", sv.satellites_used);
    lv_label_set_text(lbl_t_sats_, buf);
    snprintf(buf, sizeof(buf), "G%d R%d E%d C%d",
             sv.gps, sv.glonass, sv.galileo, sv.beidou);
    lv_label_set_text(lbl_t_sats_sub_, buf);

    // NTRIP Push tile: per-service lights. green=connected, red=enabled&&!connected,
    // dim=disabled; reconnect count shown after the service name.
    const NtripStatus *ntrip_svc[3] = {&st.rtk2go, &st.onocoy, &st.rtkdata};
    for (int i = 0; i < 3; i++) {
        const NtripStatus &ns = *ntrip_svc[i];
        uint32_t dot_col;
        if (ns.connected)        dot_col = kGoodCol;
        else if (ns.enabled)     dot_col = kCritCol;
        else                     dot_col = kDimCol;
        if (t_ntrip_dot_[i])
            lv_obj_set_style_bg_color(t_ntrip_dot_[i], lv_color_hex(dot_col), 0);
        if (t_ntrip_recon_[i]) {
            if (ns.reconnects > 0) {
                snprintf(buf, sizeof(buf), LV_SYMBOL_REFRESH "%u",
                         (unsigned)ns.reconnects);
                lv_obj_set_style_text_color(t_ntrip_recon_[i],
                    lv_color_hex(kWarnCol), 0);
            } else {
                buf[0] = '\0';
            }
            lv_label_set_text(t_ntrip_recon_[i], buf);
        }
    }
    if (t_ntrip_clients_) {
        if (st.local_clients > 0) {
            char ips[160];
            local_client_ips_to_text(st.local_client_ips, ips, sizeof(ips));
            snprintf(buf, sizeof(buf), "Direct: %s", ips);
            lv_obj_set_style_text_color(t_ntrip_clients_,
                lv_color_hex(kGoodCol), 0);
        } else {
            snprintf(buf, sizeof(buf), "Direct: none");
            lv_obj_set_style_text_color(t_ntrip_clients_,
                lv_color_hex(kDimCol), 0);
        }
        lv_label_set_text(t_ntrip_clients_, buf);
    }

    // ── Position (cached in RAM) ──────────────────────────────────────────────
    const BasePosition bpos = storage_->load_position();
    if (bpos.valid) {
        snprintf(buf, sizeof(buf), "%.7f\xc2\xb0", bpos.lat);
        lv_label_set_text(lbl_p_lat_, buf);
        snprintf(buf, sizeof(buf), "%.7f\xc2\xb0", bpos.lon);
        lv_label_set_text(lbl_p_lon_, buf);
        snprintf(buf, sizeof(buf), "%.4f m", bpos.height);
        lv_label_set_text(lbl_p_alt_, buf);
        // Dashboard compact position tile.
        snprintf(buf, sizeof(buf), "%.5f", bpos.lat);
        lv_label_set_text(lbl_t_pos_, buf);
        snprintf(buf, sizeof(buf), "%.5f", bpos.lon);
        lv_label_set_text(lbl_t_pos_sub_, buf);
    } else {
        lv_label_set_text(lbl_p_lat_, "\xe2\x80\x94");
        lv_label_set_text(lbl_p_lon_, "\xe2\x80\x94");
        lv_label_set_text(lbl_p_alt_, "No position stored");
        lv_label_set_text(lbl_t_pos_, "\xe2\x80\x94");
        lv_label_set_text(lbl_t_pos_sub_, "no fix stored");
    }

    // ── Position: survey-in three-gate arcs ───────────────────────────────────
    if (st.mode == BaseMode::kSurvey) {
        if (survey_group_) lv_obj_clear_flag(survey_group_, LV_OBJ_FLAG_HIDDEN);
        // Time gate.
        int32_t tval = (int32_t)std::min<uint32_t>(sv.elapsed_sec, kGateTimeSec);
        lv_arc_set_value(arc_sv_time_, tval);
        lv_obj_set_style_arc_color(arc_sv_time_,
            lv_color_hex(sv.elapsed_sec >= kGateTimeSec ? kGoodCol : kWarnCol),
            LV_PART_INDICATOR);
        snprintf(buf, sizeof(buf), "%lus", (unsigned long)sv.elapsed_sec);
        lv_label_set_text(lbl_sv_time_, buf);
        // Blocks gate.
        int32_t bval = std::min(sv.blocks, kGateBlocks);
        lv_arc_set_value(arc_sv_blocks_, bval);
        lv_obj_set_style_arc_color(arc_sv_blocks_,
            lv_color_hex(sv.blocks >= kGateBlocks ? kGoodCol : kWarnCol),
            LV_PART_INDICATOR);
        snprintf(buf, sizeof(buf), "%d", sv.blocks);
        lv_label_set_text(lbl_sv_blocks_, buf);
        // Stability gate (lower is better; fill = how close to the 0.50m gate).
        bool stab_ok = sv.stability > 0.0f && sv.stability <= kGateStab;
        int32_t spct;
        if (sv.stability <= 0.0f) spct = 0;
        else if (sv.stability <= kGateStab) spct = 100;
        else spct = (int32_t)std::max(0.0f, 100.0f * kGateStab / sv.stability);
        lv_arc_set_value(arc_sv_stab_, spct);
        lv_obj_set_style_arc_color(arc_sv_stab_,
            lv_color_hex(stab_ok ? kGoodCol : kWarnCol), LV_PART_INDICATOR);
        snprintf(buf, sizeof(buf), "%.2f", (double)sv.stability);
        lv_label_set_text(lbl_sv_stab_, buf);
    } else if (survey_group_) {
        lv_obj_add_flag(survey_group_, LV_OBJ_FLAG_HIDDEN);
    }

    snprintf(buf, sizeof(buf), "%d", sv.samples);
    lv_label_set_text(lbl_sv_samples_, buf);
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

    // SKY PLOT — rebuild dots only while Position is visible (reuses sats/nsat above).
    if (sky_dots_ &&
        lv_tabview_get_tab_active(tabview_) == (uint32_t)ui_sections::Section::Position) {
        lv_obj_clean(sky_dots_);
        const int cx = kSkyPlotSize / 2;
        for (size_t i = 0; i < nsat; i++) {
            if (sats[i].elevation > 90) continue;
            float r = (float)kSkyR * (90.0f - (float)sats[i].elevation) / 90.0f;
            float a = (float)sats[i].azimuth * 3.14159265f / 180.0f;
            int px = cx + (int)(r * sinf(a));
            int py = cx - (int)(r * cosf(a));
            int d = sats[i].snr >= 40 ? 16 : (sats[i].snr >= 30 ? 13 : 10);
            lv_obj_t *dot = lv_obj_create(sky_dots_);
            lv_obj_set_size(dot, d, d);
            lv_obj_set_pos(dot, px - d / 2, py - d / 2);
            lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
            uint32_t col = (sats[i].system < 6) ? kSysColors[sats[i].system] : 0xFFFFFFu;
            lv_obj_set_style_bg_color(dot, lv_color_hex(col), 0);
            lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(dot, 0, 0);
            lv_obj_clear_flag(dot, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
        }
        if (lbl_sky_count_) {
            snprintf(buf, sizeof(buf), "%u satellites in view", (unsigned)nsat);
            lv_label_set_text(lbl_sky_count_, buf);
        }
    }

    // Antenna read-only echo (Position + Storage).
    {
        char h[24];
        snprintf(h, sizeof(h), "%.4f", storage_->antenna_height());
        snprintf(buf, sizeof(buf), "%s / %s / %s m",
                 storage_->antenna_model().c_str(),
                 storage_->antenna_radome().c_str(), h);
        if (lbl_p_ant_) lv_label_set_text(lbl_p_ant_, buf);
        if (lbl_s_ant_) lv_label_set_text(lbl_s_ant_, buf);
    }

    // ── Links tab: detailed NTRIP ─────────────────────────────────────────────
    auto update_ntrip_detail = [&](const NtripStatus &ns,
        lv_obj_t *lbl_st, lv_obj_t *lbl_bytes, lv_obj_t *lbl_drop,
        lv_obj_t *lbl_recon) {
        fmt_ntrip_label(lbl_st, ns, buf, sizeof(buf));
        if (ns.enabled) {
            char kb[20];
            fmt_bytes_str(kb, sizeof(kb), ns.bytes_sent);
            if (ns.ever_sent) {
                snprintf(buf, sizeof(buf), "%s  (last %lus ago)", kb,
                         (unsigned long)ns.last_send_age_sec);
            } else {
                snprintf(buf, sizeof(buf), "%s", kb);
            }
            lv_label_set_text(lbl_bytes, buf);
            snprintf(buf, sizeof(buf), "%lu", (unsigned long)ns.dropped_batches);
            lv_label_set_text(lbl_drop, buf);
            snprintf(buf, sizeof(buf), "%lu", (unsigned long)ns.reconnects);
            lv_label_set_text(lbl_recon, buf);
            lv_obj_set_style_text_color(lbl_recon,
                ns.reconnects > 0 ? lv_color_hex(kWarnCol)
                                  : lv_color_hex(kDimCol), 0);
        } else {
            lv_label_set_text(lbl_bytes, "\xe2\x80\x94");
            lv_label_set_text(lbl_drop,  "\xe2\x80\x94");
            lv_label_set_text(lbl_recon, "\xe2\x80\x94");
        }
    };
    update_ntrip_detail(st.rtk2go,  lbl_d_rtk2go_,  lbl_d_rtk2go_bytes_,  lbl_d_rtk2go_drop_,  lbl_d_rtk2go_recon_);
    update_ntrip_detail(st.onocoy,  lbl_d_onocoy_,  lbl_d_onocoy_bytes_,  lbl_d_onocoy_drop_,  lbl_d_onocoy_recon_);
    update_ntrip_detail(st.rtkdata, lbl_d_rtkdata_,  lbl_d_rtkdata_bytes_, lbl_d_rtkdata_drop_, lbl_d_rtkdata_recon_);

    snprintf(buf, sizeof(buf), "%d client%s",
             st.local_clients, st.local_clients == 1 ? "" : "s");
    lv_label_set_text(lbl_d_local_ntrip_, buf);
    lv_obj_set_style_text_color(lbl_d_local_ntrip_,
        lv_color_hex(st.local_clients > 0 ? kGoodCol : kDimCol), 0);
    local_client_ips_to_text(st.local_client_ips, buf, sizeof(buf));
    lv_label_set_text(lbl_d_local_ntrip_ips_, buf);
    lv_obj_set_style_text_color(lbl_d_local_ntrip_ips_,
        lv_color_hex(st.local_clients > 0 ? kGoodCol : kDimCol), 0);

    // ── System: SYSTEM ────────────────────────────────────────────────────────
    {
        uint64_t up = (uint64_t)(esp_timer_get_time() / 1000000);
        unsigned dd = up / 86400, hh = (up % 86400) / 3600;
        unsigned mm = (up % 3600) / 60, ss = up % 60;
        if (dd) snprintf(buf, sizeof(buf), "%ud %02u:%02u:%02u", dd, hh, mm, ss);
        else    snprintf(buf, sizeof(buf), "%02u:%02u:%02u", hh, mm, ss);
        lv_label_set_text(lbl_uptime_, buf);

        const esp_reset_reason_t rr = esp_reset_reason();
        lv_label_set_text(lbl_reset_, reset_reason_str(rr));
        lv_obj_set_style_text_color(lbl_reset_,
            reset_is_crash(rr) ? lv_color_hex(kCritCol)
                               : lv_color_hex(kDimCol), 0);

        // Free-heap arc (% of total available, capped 0..100).
        size_t free_b  = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        size_t total_b = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
        int heap_pct = total_b ? (int)(free_b * 100 / total_b) : 0;
        lv_arc_set_value(arc_heap_, heap_pct);
        lv_obj_set_style_arc_color(arc_heap_,
            lv_color_hex(heap_pct < 10 ? kCritCol
                         : heap_pct < 25 ? kWarnCol : kGoodCol),
            LV_PART_INDICATOR);
        snprintf(buf, sizeof(buf), "%d%%", heap_pct);
        lv_label_set_text(lbl_heap_pct_, buf);
        char hf[20];
        fmt_bytes_str(hf, sizeof(hf), (uint64_t)free_b);
        lv_label_set_text(lbl_heap_, hf);
    }

    // ── System: NETWORK ───────────────────────────────────────────────────────
    if (wifi_->connected()) {
        snprintf(buf, sizeof(buf), "%s  %d dBm",
                 wifi_->ssid().c_str(), wifi_->rssi());
        lv_obj_set_style_text_color(lbl_wifi_state_, lv_color_hex(kGoodCol), 0);
    } else {
        const WifiCredentials wc = storage_->load_wifi();
        if (wc.valid) {
            snprintf(buf, sizeof(buf), "Connecting to %s\xe2\x80\xa6",
                     wc.ssid.c_str());
            lv_obj_set_style_text_color(lbl_wifi_state_, lv_color_hex(kWarnCol), 0);
        } else {
            snprintf(buf, sizeof(buf), "Not configured");
            lv_obj_set_style_text_color(lbl_wifi_state_, lv_color_hex(kDimCol), 0);
        }
    }
    lv_label_set_text(lbl_wifi_state_, buf);

    const std::string sta_ip = wifi_->ip_address();
    lv_label_set_text(lbl_ip_, sta_ip.empty() ? "\xe2\x80\x94" : sta_ip.c_str());

    const std::string ap_name = wifi_->access_point_ssid();
    const std::string ap_ip   = wifi_->access_point_ip();
    lv_label_set_text(lbl_ap_name_, ap_name.empty() ? "off" : ap_name.c_str());
    lv_obj_set_style_text_color(lbl_ap_name_,
        lv_color_hex(ap_name.empty() ? kDimCol : kGoodCol), 0);
    lv_label_set_text(lbl_ap_ip_, ap_ip.empty() ? "\xe2\x80\x94" : ap_ip.c_str());

    // Dashboard Link tile: station SSID/RSSI, else AP.
    if (wifi_->connected()) {
        lv_label_set_text(lbl_t_link_, wifi_->ssid().c_str());
        snprintf(buf, sizeof(buf), "%d dBm", wifi_->rssi());
        lv_label_set_text(lbl_t_link_sub_, buf);
    } else if (!ap_name.empty()) {
        lv_label_set_text(lbl_t_link_, "AP");
        lv_label_set_text(lbl_t_link_sub_, ap_name.c_str());
    } else {
        lv_label_set_text(lbl_t_link_, "OFFLINE");
        lv_label_set_text(lbl_t_link_sub_, "");
    }

    // ── Storage: SD + disk arc ────────────────────────────────────────────────
    int disk_pct = 0;
    if (sd_->is_mounted()) {
        const SdManager::DiskStats stats = sd_->disk_stats();
        if (stats.valid) {
            char used[20], total[20];
            fmt_bytes_str(used,  sizeof(used),  stats.used_bytes);
            fmt_bytes_str(total, sizeof(total), stats.total_bytes);
            snprintf(buf, sizeof(buf), "%s / %s", used, total);
            if (stats.total_bytes > 0)
                disk_pct = (int)(stats.used_bytes * 100 / stats.total_bytes);
        } else {
            snprintf(buf, sizeof(buf), "Mounted");
        }
    } else {
        snprintf(buf, sizeof(buf), "Not mounted");
    }
    lv_label_set_text(lbl_sd_, buf);
    lv_arc_set_value(arc_disk_, disk_pct);
    lv_obj_set_style_arc_color(arc_disk_,
        lv_color_hex(disk_pct >= ui_sections::kDiskCritPct ? kCritCol
                     : disk_pct >= ui_sections::kDiskWarnPct ? kWarnCol
                     : kGoodCol), LV_PART_INDICATOR);
    snprintf(buf, sizeof(buf), "%d%%", disk_pct);
    lv_label_set_text(lbl_disk_pct_, buf);

    // Dashboard SD/RINEX tile.
    snprintf(buf, sizeof(buf), "%d%%", disk_pct);
    lv_label_set_text(lbl_t_sd_, buf);
    if (rx.active) {
        lv_label_set_text(lbl_t_sd_sub_, "RINEX logging");
        lv_obj_set_style_text_color(lbl_t_sd_sub_, lv_color_hex(kGoodCol), 0);
    } else {
        lv_label_set_text(lbl_t_sd_sub_, "RINEX off");
        lv_obj_set_style_text_color(lbl_t_sd_sub_, lv_color_hex(kDimCol), 0);
    }

    // RINEX switch – keep in sync with actual state (don't fight user)
    bool rinex_active = rx.active;
    if (rinex_active != lv_obj_has_state(sw_rinex_, LV_STATE_CHECKED)) {
        if (rinex_active)
            lv_obj_add_state(sw_rinex_, LV_STATE_CHECKED);
        else
            lv_obj_clear_state(sw_rinex_, LV_STATE_CHECKED);
    }
    // Mirror the RINEX state on the Position Setup modal's switch (if built).
    if (sw_ps_rinex_ &&
        rinex_active != lv_obj_has_state(sw_ps_rinex_, LV_STATE_CHECKED)) {
        if (rinex_active)
            lv_obj_add_state(sw_ps_rinex_, LV_STATE_CHECKED);
        else
            lv_obj_clear_state(sw_ps_rinex_, LV_STATE_CHECKED);
    }

    if (rx.active && !rx.current_file.empty()) {
        const std::string &fn = rx.current_file;
        size_t slash = fn.rfind('/');
        const char *fname = (slash != std::string::npos)
                            ? fn.c_str() + slash + 1 : fn.c_str();
        snprintf(buf, sizeof(buf), "%s  ep:%d", fname, rx.epochs);
    } else {
        snprintf(buf, sizeof(buf), rx.active ? "Active" : "\xe2\x80\x94");
    }
    lv_label_set_text(lbl_rinex_file_, buf);

    // ── System: FIRMWARE ──────────────────────────────────────────────────────
    const esp_app_desc_t *desc = esp_app_get_description();
    if (desc) {
        lv_label_set_text(lbl_fw_, desc->version);
        snprintf(buf, sizeof(buf), "%s  %s", desc->date, desc->time);
        lv_label_set_text(lbl_compile_, buf);
    }

    lv_label_set_text(lbl_c6_running_,
                      c6_running_ready_.load() ? c6_running_ver_ : "querying\xe2\x80\xa6");

    int ota_p = c6_ota_progress_.load();
    if (ota_p == -1) {
        lv_label_set_text(lbl_c6_fw_, c6_avail_ver_);
        lv_obj_set_style_text_color(lbl_c6_fw_, lv_color_hex(kTextCol), 0);
        lv_obj_clear_state(btn_c6_ota_, LV_STATE_DISABLED);
    } else if (ota_p == -2) {
        lv_label_set_text(lbl_c6_fw_, "Update failed");
        lv_obj_set_style_text_color(lbl_c6_fw_, lv_color_hex(kCritCol), 0);
        lv_obj_clear_state(btn_c6_ota_, LV_STATE_DISABLED);
    } else {
        snprintf(buf, sizeof(buf), "Flashing\xe2\x80\xa6 %d%%", ota_p);
        lv_label_set_text(lbl_c6_fw_, buf);
        lv_obj_set_style_text_color(lbl_c6_fw_, lv_color_hex(kWarnCol), 0);
        lv_obj_add_state(btn_c6_ota_, LV_STATE_DISABLED);
    }

    // ── System: ADMIN password state ──────────────────────────────────────────
    if (lbl_admin_) {
        bool set = storage_->admin_password_set();
        lv_label_set_text(lbl_admin_, set ? "Set" : "Not set");
        lv_obj_set_style_text_color(lbl_admin_,
            lv_color_hex(set ? kGoodCol : kWarnCol), 0);
    }

    // ── SD bulk-delete progress (drives the file-browser confirm/progress view) ──
    poll_delete_progress();

    // ── System: console tail (every other tick to reduce overhead) ────────────
    static int console_tick = 0;
    if (++console_tick >= 2) {
        console_tick = 0;
        refresh_console_log();
    }
}

void Ui::refresh_console_log() {
    // Only update while the console modal is actually open — there is no inline
    // System-tab console anymore, so otherwise there is nothing to refresh (and, in
    // particular, we never auto-scroll the System tab and hide its settings).
    if (!lbl_console_ || !modal_console_ ||
        lv_obj_has_flag(modal_console_, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }

    // Render the tail of the shared console buffer (full history is on /logs).
    std::string logs = log_buffer::snapshot();
    if (logs.size() > kConsoleTailChars) {
        const size_t cut = logs.size() - kConsoleTailChars;
        const size_t nl = logs.find('\n', cut);
        logs.erase(0, nl == std::string::npos ? cut : nl + 1);
    }
    lv_label_set_text(lbl_console_, logs.empty() ? "(no log output yet)" : logs.c_str());
    // Auto-scroll the console view (its own container) to the newest lines.
    if (console_scroll_) {
        int32_t target = lv_obj_get_scroll_y(console_scroll_) +
                         lv_obj_get_scroll_bottom(console_scroll_);
        lv_obj_scroll_to_y(console_scroll_, target, LV_ANIM_OFF);
    }
}
