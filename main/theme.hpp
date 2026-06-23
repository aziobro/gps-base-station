#pragma once

#include <cstdint>

// ── Marine-instrument color tokens ──────────────────────────────────────────────
// Single source of truth for the touchscreen (LVGL) UI palette. The web UI mirrors
// these EXACT hex values in its CSS :root / [data-theme=day] block (web_server.cpp,
// kAfterTitle). Keep the two in sync — the Phase-4 consistency audit verifies parity.
//
// Values are 0xRRGGBB. Touch wraps them with lv_color_hex(); web emits them as
// CSS custom properties. Night is the default palette; Day is the sunlight variant.
namespace theme {

struct Theme {
    uint32_t bg;          // screen background
    uint32_t surface;     // tile / card surface
    uint32_t surface_hi;  // raised / selected surface
    uint32_t border;      // hairlines, dividers
    uint32_t text;        // primary numeric / text
    uint32_t text_dim;    // labels, units
    uint32_t text_faint;  // disabled, captions, idle ("off" — never red)
    uint32_t accent;      // interactive / links / focus
    uint32_t good;        // ok / connected / transmitting / fix
    uint32_t warn;        // surveying / reconnecting / disk>80% / weak RSSI
    uint32_t crit;        // disconnected / SD error / disk>95% / failed
};

// Field order: bg, surface, surface_hi, border, text, text_dim, text_faint,
//              accent, good, warn, crit.  (C++17 — no designated initializers.)

// Night palette (default — dark-adapted).
constexpr Theme kNight = {
    0x05080B, 0x0D141B, 0x142029, 0x203040,
    0xC9D6E0, 0x6E8294, 0x3F5364, 0x1C7FCC,
    0x1FAE66, 0xD98A1F, 0xE0473F,
};

// Day palette (sunlight / bright ambient — higher contrast).
constexpr Theme kDay = {
    0x0B1016, 0x16202B, 0x1E2C3A, 0x2C4053,
    0xF2F6FA, 0x9DB0C2, 0x5E7488, 0x2FA4FF,
    0x27D17C, 0xFFB02E, 0xFF5A52,
};

}  // namespace theme
