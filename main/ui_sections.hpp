#pragma once

// ── Shared UI contract for BOTH surfaces (LVGL touchscreen + web) ────────────────
// Plain values only — NO LVGL or ESP-IDF types — so that web_server.cpp and ui.cpp
// can both #include this and render the SAME five sections (names, order), the SAME
// status-color semantics, and the SAME bulk-delete wording. This file is the spine
// that keeps the two codebases from drifting; change a label here, both surfaces
// follow.
namespace ui_sections {

// The five top-level sections, in display order. Identical on web and touch.
enum class Section { Dashboard, Position, Links, Storage, System };
constexpr int kSectionCount = 5;

// Web (full) names, indexed by Section.
constexpr const char *kWebName[kSectionCount] = {
    "Dashboard", "Position", "Links", "Storage", "System",
};

// Touch (terse) labels that fit a 5-up bottom tab bar at 720 px.
constexpr const char *kTouchLabel[kSectionCount] = {
    "Dash", "Position", "Links", "Storage", "System",
};

// One-line purpose (web header subtitle / touch tooltip).
constexpr const char *kPurpose[kSectionCount] = {
    "At-a-glance instrument face",
    "Coordinate, survey-in, satellites, sky plot, antenna",
    "NTRIP push services and local caster",
    "SD usage, RINEX logging, files",
    "Health, network, firmware, console, settings",
};

// ── Status severity — drives the shared color semantics ─────────────────────────
// Maps to theme good / warn / crit and the faint "idle" token. Same thresholds and
// meaning on both surfaces. "Idle" is dim/faint — never red ("off" != "broken").
enum class Status { Good, Warn, Crit, Idle };

// Disk-usage thresholds (percent of capacity used).
constexpr int kDiskWarnPct = 80;  // amber at/above this
constexpr int kDiskCritPct = 95;  // red at/above this

// WiFi RSSI (dBm) at/under which the link is considered "weak" (warn).
constexpr int kRssiWeakDbm = -75;

// ── Bulk SD-card delete — user-facing strings (identical on both surfaces) ───────
// UTF-8; \xE2\x80\xA6 = "…", \xE2\x80\x94 = "—". Both renderers reference these by
// name so neither hand-types "This cannot be undone."
namespace del {
constexpr const char *kSelect       = "Select";
constexpr const char *kDelete       = "Delete";                    // shown "Delete (N)"
constexpr const char *kDeleteFolder = "Delete folder\xE2\x80\xA6"; // "Delete folder…"
constexpr const char *kEmptyFolder  = "Empty folder\xE2\x80\xA6";  // "Empty folder…"
constexpr const char *kSelectAll    = "Select all";
constexpr const char *kCancel       = "Cancel";
constexpr const char *kCannotUndo   = "This cannot be undone.";
constexpr const char *kFolderKept   = "The folder itself is kept.";
constexpr const char *kActiveFile   =
    "Cannot delete \xE2\x80\x94 RINEX logging is writing this file. "
    "Stop logging in System first.";
constexpr const char *kConfirmType  = "DELETE";  // typed-confirmation word (web)
}  // namespace del

}  // namespace ui_sections
