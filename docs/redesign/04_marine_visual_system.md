I have enough concrete research. I've confirmed the marine-instrument visual language: dark transflective backgrounds, white day / red-green-blue night illumination, extra-large high-contrast digits (36mm scalable), large data tiles in a grid with a shared alarm strip, configurable numeric fields with color-coded status (green=on-target, blue/amber/red shifts), and Fitts-law finger targets. Combined with the current codebase palette and constraints, I'll now distill this into a concrete, implementable design system.

Here is the deliverable.

---

# Marine-Instrument Design System â€” GPS RTK Base Station

A single token set + component spec that renders consistently on the 720Ã—720 LVGL touch screen and the responsive web UI. Built around the B&G H5000 / Garmin GMI / SailSteer visual language: dark, sunlight-readable, big high-contrast numerics, color-coded status, minimal chrome.

## 1. Design principles (distilled from the references)

| Principle | Source observation | How we apply it |
|---|---|---|
| Sunlight-first contrast | B&G/Garmin use transflective LCDs + "high-contrast digits"; case study: "readable in bright sun, heavy overcast and night" | Near-black bg + near-white primary text (â‰ˆ15:1). Status colors all â‰¥4.5:1 on bg. |
| The data tile is the atomic unit | Outboard cluster: "the engine tile carries key telemetryâ€¦ repeats in a grid" | Everything is built from one "big readout tile" component. |
| Big number, small label | Garmin GMI: "extra large digitsâ€¦ scalable up to 36 mm" | Value dominates; unit + label are secondary, dimmed. |
| Color = status, sparingly | SailSteer: green=on-target, shifts to blue/amber/red off-target; "yellow dot outside the red sector" | Only 4 status colors. Color never the sole signal â€” pair with text/icon. |
| Day + night palettes | Both: white day mode; red/green/blue night | Two palettes sharing one token contract; one toggle swaps them. |
| Shared alarm strip | Cluster: "consistent area where the most critical fault is always summarised" | Persistent header status pill = worst current condition. |
| Finger targets (Fitts) | "Gloved hands land less precisely; generous touch targets" | Min 64px touch target on 720px screen; 44px web. |

## 2. Color tokens

Two themes (`day`, `night`) behind identical token names. Night is the default for a base station (typically indoors/shed, glanceable). All hex are 24-bit; LVGL uses `lv_color_hex(0xRRGGBB)`, web uses CSS custom properties.

### Day palette (sunlight / bright ambient)

| Token | Hex | Role | Contrast vs bg |
|---|---|---|---|
| `--bg` | `#0B1016` | Screen background (near-black, slight blue) | â€” |
| `--surface` | `#16202B` | Tile / card surface | â€” |
| `--surface-hi` | `#1E2C3A` | Raised / selected surface | â€” |
| `--border` | `#2C4053` | Hairline borders, dividers | 1.6:1 (non-text) |
| `--text` | `#F2F6FA` | Primary numeric / text | 15.6:1 âœ“AAA |
| `--text-dim` | `#9DB0C2` | Labels, units, secondary | 6.2:1 âœ“AA |
| `--text-faint`| `#5E7488` | Disabled, captions | 3.3:1 (large only) |
| `--accent` | `#2FA4FF` | Interactive / brand / links / focus | 6.4:1 âœ“AA |
| `--good` | `#27D17C` | OK / connected / fixed | 7.9:1 âœ“AAA |
| `--warn` | `#FFB02E` | Warning / surveying / degraded | 10.2:1 âœ“AAA |
| `--crit` | `#FF5A52` | Critical / alarm / disconnected | 5.4:1 âœ“AA |

### Night palette (dark-adapted; lower luminance, no harsh blue-white)

| Token | Hex | Notes |
|---|---|---|
| `--bg` | `#05080B` | Deeper black |
| `--surface` | `#0D141B` | |
| `--surface-hi` | `#142029` | |
| `--border` | `#203040` | |
| `--text` | `#C9D6E0` | Dimmed white â€” avoids night-vision bloom (cluster note: "night colours interfered with night vision") |
| `--text-dim` | `#6E8294` | |
| `--text-faint`| `#3F5364` | |
| `--accent` | `#1C7FCC` | Desaturated blue |
| `--good` | `#1FAE66` | |
| `--warn` | `#D98A1F` | Amber, toned down |
| `--crit` | `#E0473F` | |

Status-color semantics (identical on both surfaces):
- **good (green)** â€” base transmitting, NTRIP pushed/connected, SD mounted, RINEX logging healthy, RTK fix.
- **warn (amber)** â€” survey-in in progress, reconnecting, disk >80%, RSSI weak, degraded sat count.
- **crit (red)** â€” disconnected/dropped, SD unmounted/error, disk >95%, survey failed, no fix.
- **dim** â€” feature off/idle/not applicable (e.g. a disabled NTRIP service) â€” never red. "Off" â‰  "broken."

Rule: every status color is **redundantly encoded** â€” a pill carries both a color and a word ("LIVE", "SURVEYING", "OFFLINE"), and an icon glyph, so it survives sunlight washout and color-blindness.

## 3. Typography

**Approach:** one sans-serif identity, sourced natively on each platform â€” no font downloads anywhere.

### (a) LVGL embedded
Use the built-in **Montserrat** family (already linked in this build) as the marine sans. Currently only `montserrat_14` is compiled; enable a numeric-readout size set in `sdkconfig`/`lv_conf.h`:

```
LV_FONT_MONTSERRAT_14   (labels, list rows)        â€” already on
LV_FONT_MONTSERRAT_18   (sub-values, pills)        â€” enable
LV_FONT_MONTSERRAT_28   (tile values)              â€” enable
LV_FONT_MONTSERRAT_48   (hero readouts)            â€” enable
```

Flash cost is ~3â€“10 KB/size; only enable the four above. For the big lat/lon/RTCM numbers, Montserrat's even stroke weight reads cleanly at distance. (If a tabular-figure look is wanted for live-changing numbers, that's the only reason to consider a custom font â€” defer it; Montserrat is acceptable and free here.)

### (b) Web
System font stack â€” zero download, native crispness, matches the embedded sans feel:

```css
--font: "Segoe UI", system-ui, -apple-system, "Roboto", "Helvetica Neue", Arial, sans-serif;
--font-num: "Segoe UI", system-ui, sans-serif; /* same; add font-variant-numeric: tabular-nums */
```

Replaces the current `monospace` terminal stack everywhere **except** the console/log view, where monospace is correct and should stay.

### Type scale (shared semantic ramp)

| Token | Touch (LVGL) | Web | Weight | Use |
|---|---|---|---|---|
| `hero` | 48 px | `clamp(40px, 9vw, 64px)` | 700 | The one dominant value on a screen/tile (RTCM rate, survey Ïƒ) |
| `value` | 28 px | `clamp(24px, 5vw, 36px)` | 600 | Standard tile value |
| `value-sm` | 18 px | 18â€“20 px | 600 | Dense tiles, sat-by-constellation |
| `label` | 14 px | 13 px | 600, **uppercase, +0.06em tracking** | Tile label above/below value |
| `unit` | 14 px | 14 px | 500, dim | Unit suffix beside value |
| `body` | 14 px | 14â€“15 px | 400 | List rows, descriptions |
| `caption`| 14 px* | 12 px | 500, dim | Timestamps, hints (*touch min size) |

Rhythm: labels are **uppercase + dimmed + tracked**; values are **large + bright + tight**. That single contrast is what makes it read as a marine instrument rather than a form.

## 4. Core components

All measurements: **touch px @720Ã—720 / web**.

### 4.1 Big readout tile (the atom)
```
â”Œâ”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”   â€¢ surface bg, 12px radius, 1px border
â”‚ LABEL            [pill]  â”‚   â€¢ optional status pill, top-right
â”‚                          â”‚
â”‚   1247  B/s              â”‚   â€¢ value (hero/value) + unit, baseline-aligned
â”‚                          â”‚   â€¢ status color tints the VALUE when meaningful
â”‚ caption / sub-detail     â”‚   â€¢ optional dim sub-line
â””â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”˜
```
- Touch: min height 150px, padding 16px. Web: min-height 110px, padding 16px.
- The value, not the tile, carries status color (B&G pattern). Border stays neutral except in `crit` state, where a 2px `--crit` left-edge bar is added (redundant cue).
- Tappable tiles (open a detail/config) show a faint `>` chevron + `--accent` ripple/hover.

### 4.2 Status pill
- Pill = color-filled-at-12%-opacity background + solid-color text + 8px dot + word.
  e.g. `â— LIVE` (good), `â— SURVEYING` (warn), `â— OFFLINE` (crit), `â—‹ OFF` (dim/outline only).
- Touch height 32px / web 22px; radius = full (capsule). Used in tile corners, list rows, and the header.

### 4.3 Header (shared, persistent)
A 64px (touch) / 56px (web) bar: section title left Â· **global status pill** (worst-of-all-systems, the "shared alarm strip") center/right Â· day/night toggle + (web) hamburger right. The global pill is the at-a-glance "is my base healthy?" â€” green LIVE / amber SURVEYING / red ALARM.

### 4.4 Section navigation
One IA, two renderings of the **same sections in the same order**:
- **Touch:** bottom tab bar, 5 icon+label tabs, 80px tall, each tab â‰¥120px wide (finger). Active tab = `--accent` icon + top 3px accent rule.
- **Web:** same five as a top segmented nav (desktop) that collapses to a bottom tab bar / drawer on narrow screens. Identical icons, labels, order.

### 4.5 List row (SD files, WiFi scan, NTRIP services)
- 64px touch / 48px web. Leading icon Â· primary text + dim sub-text Â· trailing pill or value or chevron.
- **Multi-select mode** (bulk SD): leading 36px checkbox appears; long-press (touch) or a "Select" button (both) enters the mode; selected rows get `--surface-hi` + `--accent` check. A bottom action bar slides up: `Delete (N)` in `--crit`. Directory rows get a "Delete folderâ€¦" affordance. Destructive actions always route through the confirm dialog (4.8).

### 4.6 Gauge / arc (survey progress, disk %, RSSI)
- 270Â° arc, 12px stroke, `--border` track + status-colored fill; centered value uses `value` token. Use for bounded 0â€“100 quantities (survey stability %, disk usage, signal). LVGL `lv_arc`; web `<svg>` stroke-dasharray (no canvas needed, themeable via `currentColor`).

### 4.7 Progress bar (survey-in, OTA, RINEX rate)
- 10px (touch) / 8px (web) track, full-radius, status-colored fill, transition 0.3s. Label + % above. (Reuses existing OTA bar markup â€” just retheme: track `--surface-hi`, fill `--good`â†’`--crit` by state, not `#0f0`/`#f44`.)

### 4.8 Confirm dialog (destructive)
- Centered modal over a 60%-dim scrim. Title in `--text`, body naming exactly what's affected ("Delete 7 files from /rawdata? This cannot be undone."), and for whole-directory deletes the count + total size. Two buttons: **Cancel** (neutral, `--surface-hi`) and **Delete** (`--crit` filled). Delete is **never** the default focus. Touch buttons â‰¥64px tall.

## 5. Layout grammar

- **Grid:** 8px base unit. Gaps 16px (touch) / 12â€“16px (web). Tile corners 12px, pills full, buttons 8px.
- **Touch dashboard (Status):** 2Ã—2 or 2Ã—3 tile grid filling 720px under the 64px header and above the 80px tab bar (usable ~576px tall). One hero tile may span 2 columns. Touch-scroll for overflow.
- **Web dashboard:** CSS grid `repeat(auto-fit, minmax(220px, 1fr))` â€” same tiles reflow from 1 col (phone) to 4 col (desktop). The 720 touch layout is essentially the 2-col breakpoint, which is what keeps them looking like the same product.
- **Hierarchy:** per screen, exactly one `hero` value; everything else steps down. Don't make ten things big â€” nothing reads at a glance if all do.
- **Iconography:** single-weight line icons, ~2px stroke, rounded caps, 24px (web) / 28px touch. Marine-glanceable set: satellite, broadcast/antenna, crosshair (position), gear (system), waveform/list (logs/debug), wifi, SD card, cloud-up (NTRIP push), folder, trash. LVGL: use built-in symbol font glyphs where they exist (`LV_SYMBOL_*`) to avoid shipping an icon font; web: a small inline-SVG set colored by `currentColor` so status color flows through.
- **Day/night toggle:** a single control in the header. Touch: swaps a theme struct of `lv_color_t` tokens and restyles via a shared `apply_theme()`; web: toggles `data-theme="night"` on `<html>`, all colors being `var(--token)`. Persist the choice in `Storage` so both surfaces agree.

## 6. Unified IA (same sections, both surfaces)

Maps all current functionality into one shared structure (preserves everything; web-only extras noted):

1. **Dashboard** (was Status) â€” global health pill, mode (survey-in/base-TX), RTCM rate hero, sat count, NTRIP push summary, key position. Survey start/confirm action.
2. **Position** â€” lat/lon/alt big readouts, survey-in detail (elapsed, samples, Ïƒ, stability arc), sat-by-constellation, antenna model/radome/height, **sky plot**.
3. **Links** (was NTRIP) â€” per-service tiles (RTK2go/Onocoy/RTKdata) with status pill + bytes/bitrate/drops/reconnects, local caster + client IPs, master enable, per-service config.
4. **Storage** â€” SD mount + disk arc, RINEX logging toggle + file/epochs, **file browser with bulk multi-select delete + directory delete** (both surfaces). Web-only: file **download**, bulk **RINEX export**.
5. **System** â€” uptime, reset reason, WiFi station (SSID/RSSI/IP) + AP, firmware version + compile, C6 version + OTA, day/night, free heap. Web-only: **OTA firmware upload**, full **console log** viewer (keep monospace there).

This is a proposal for the shared section spine; the same five names, order, icons, and status semantics drive both the LVGL tab bar and the web nav.

## 7. One system, two renderers â€” the contract

- **Single token contract:** the table in Â§2/Â§3 is the source of truth. LVGL gets it as a `Theme` struct (`lv_color_t bg, surface, text, â€¦` + font pointers) selected by day/night; web gets it as one `:root` / `[data-theme]` CSS variable block emitted **once** as a shared string literal and reused by every page (replaces today's per-page inline `<style>` blocks).
- **Same component vocabulary:** "tile / pill / list-row / arc / bar / confirm" exist on both sides with matching proportions (scaled by platform target sizes), so a feature looks like itself in both places.
- **Same IA + same words + same icons + same status colors.** Divergence is allowed *only* for the web-only capabilities listed (download, OTA upload, RINEX export, full log). 
- **Responsive trick:** design the web at the 2-column â‰ˆ720px breakpoint to mirror the touch screen; let it expand to 4-col on desktop and collapse to 1-col on phones. The touch screen is then "the product at one fixed breakpoint," which is exactly why they'll feel unified.

## Sources
- [B&G TritonÂ² Digital Display (day/night white-red-green-blue illumination, transflective, sunlight)](https://www.bandg.com/bg/type/instruments/triton2-digital-display/)
- [Garmin GMI 20 (extra-large high-contrast digits to 36mm, red/black & green/black night modes)](https://www.hodgesmarine.com/gar010-01140-00-garmin-gmitrade-20-marine-instrument-display.html)
- [B&G SailSteer (configurable data fields, color-coded status: green on-target â†’ blue/amber/red, yellow/red sectors)](https://www.bandg.com/sailing-features/)
- [Marine/outboard cluster GUI case study (data-tile architecture, shared alarm strip, sunlight + night-vision palette tuning, Fitts-law touch targets)](https://interface-design.co.uk/case-studies/gui-design-cluster-display/)
- [Simrad â€” Understanding Multifunction Displays (daylight-readable, customizable split/tile layouts)](https://www.simrad-yachting.com/learning-and-news-hub/technology/understanding-multifunction-displays/)
- [WCAG 2.2 contrast thresholds used to validate the palette (4.5:1 text / 3:1 large & UI)](https://www.allaccessible.org/blog/color-contrast-accessibility-wcag-guide-2025)

Codebase anchors for the implementer: current touch palette is in `C:\Users\aziob\Documents\Development\GPSBaseStation\gps-base-station\main\ui.cpp` (lines 51â€“57, `kBgScreen 0x0d1b2a` etc.) using only `lv_font_montserrat_14`; current web theme is the inline `monospace`/`#0f0`-on-`#111` block in `C:\Users\aziob\Documents\Development\GPSBaseStation\gps-base-station\main\web_server.cpp` (â‰ˆlines 1317â€“1326, repeated per page) â€” both are what this token system replaces.
