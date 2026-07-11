---
name: project_um980_config
description: UM980 receiver configuration for a fixed base station -- current commands, Unicore manual findings, and gotchas
metadata:
  node_type: memory
  type: project
---

UM980 GNSS receiver configuration for the fixed base station role, and what an in-depth review of Unicore's own reference manual turned up. See [[project_ntrip]] for the RTCM pipeline these messages feed, [[project_structure]] for where `um980.hpp/.cpp` sits.

## Reference manual
`Manuals/Unicore Reference Commands Manual For N4 High Precision Products_V2_EN_R1.6.pdf` (tracked in repo). **Page numbering gotcha:** the PDF's raw page index is offset +18 from the document's own printed page numbers (front matter/TOC eats the first 18 pages). E.g. "page 40" as cited in the document's own footer = raw PDF page 58. This machine has no `pdftoppm`/poppler, so extract text with `pypdf` instead of the Read tool's PDF renderer: `uv run --with pypdf python3 -c "from pypdf import PdfReader; r = PdfReader('...'); print(r.pages[N].extract_text())"`.

## Current configure_base() sequence (main/um980.cpp), in order
1. `UNLOGALL COM2` / `UNLOGALL COM3` (`stop_output()`)
2. `CONFIG UNDULATION AUTO` — manual states this should be configured first when setting up base station mode
3. `CONFIG PVTALG MULTI` — dual-frequency solution; the manual repeats this recommendation verbatim 3x for fixed/self-optimizing base stations in open sky. UM980's factory default is `AUTO` (single-frequency + ionospheric error estimation), not `MULTI`
4. `CONFIG BASEANTENNAMODEL "<model>" a0001 0 USER` — embeds `storage_.antenna_model()` into RTCM 1005/1006/1033's antenna descriptor field (default without this is the placeholder `ADVNULLANTENNA`). SN/setupID aren't tracked in storage — placeholders are fine, purely informational RTCM fields, not used in any correction calculation
5. `MODE BASE <lat> <lon> <height>` — sets the fixed position. **Not** `CONFIG BASE GEODETIC` (see gotcha below)
6. `LOG COM3 RTCM1006 ONTIME 5` — station position + antenna height (was bare RTCM1005 until ota140; 1006 is a strict superset)
7. `LOG COM3 RTCM1033 ONTIME 10` — receiver + antenna descriptor (new in ota140)
8. `LOG COM3 RTCM1077/1087/1097/1117/1127/1137 ONTIME 1` — MSM7 (GPS/GLONASS/Galileo/QZSS/BeiDou/NavIC)
9. `LOG COM3 RTCM1230 ONTIME 30` — GLONASS code-phase biases (static calibration data, doesn't need 1Hz)
10. `configure_satellite_output()` — COM2 GSA/GSV, for our own UI/RINEX only, not RTCM
11. `SAVECONFIG`

## SIGNALGROUP 2 (ota139 — deliberately NOT part of configure_base())
Factory default SIGNALGROUP 1 doesn't track NavIC at all (despite us requesting RTCM1137 the whole time), and has narrower GLONASS/Galileo band coverage than SIGNALGROUP 2 (adds GLONASS G3, Galileo E6, NavIC L5 — manual section 4.23, Table 4-32; UM980 master-antenna supports 1 (default)/2/8). **The receiver resets itself automatically whenever this value changes** (duration undocumented — `Um980::configure_signal_group()` uses a conservative 5s settle, confirmed sufficient on-device). Because of that reset, this is a standalone, user-triggered action, not folded into the automatic path: System page → "Reset UM980 Signal Group" button → `POST /um980/reset` → `BaseStation::reset_um980()`, which suspends outbound/local streams first, sends the command, waits, then re-runs the full `configure_base()` sequence. Re-sending the same value later is a no-op (no reset).

## Gotcha: CONFIG BASE GEODETIC was never a documented command
Confirmed via full-text search of the entire 365-page manual — `CONFIG BASE GEODETIC` does not appear anywhere in it. It empirically worked for months regardless (RTCM always broadcast the exact requested position), almost certainly as an undocumented/legacy alias, but ota141 switched to the officially-documented `MODE BASE [lat] [lon] [height]` for long-term reliability (geodetic vs. ECEF is disambiguated automatically by value range, no keyword needed). If a future firmware update ever silently drops the old syntax, this is why we moved off it pre-emptively.

## Gotcha: MODE BASE's height parameter is orthometric (MSL), not ellipsoidal (2026-07-09)
`MODE BASE <lat> <lon> <height>` does **not** take ellipsoidal height despite that being the standard GNSS/RTCM convention and what the field name suggests. The receiver treats `height` as orthometric (MSL) and internally applies **its own built-in geoid model** (per `CONFIG UNDULATION AUTO`, sent earlier in `configure_base()`) to derive the ellipsoidal height it actually uses for its WGS84 position and RTCM1006 broadcast.

**Discovered via rinex-recorder** (see [[project_ntrip]]): a position update through `/config/position` was computed from OPUS + CSRS-PPP as an ellipsoidal height (-22.8440m) and entered directly -- expecting the receiver to broadcast that same ellipsoidal height. Grabbing a fresh 15s sample straight off the local caster (`GET /BASE0`) and decoding the RTCM1006 position (via `convbin` on the snippet, then ECEF→geodetic) showed it was actually broadcasting **-56.3610m**, a **-33.517m** offset. The *previous* configured height (12.2596, whatever that was originally derived from) showed the identical -33.517m offset (broadcast -21.2574m) -- proving this is a consistent, reproducible conversion applied by the receiver, not a one-off fluke. The offset closely tracks the real local geoid undulation (CSRS-PPP computed -33.2694m for this location; OPUS's `ORTHO HGT` was 10.714m, matching the corrected input almost exactly) -- the small residual (~0.25m) is normal geoid-model-to-model disagreement (UM980's internal model vs. GEOID18/CGG2013a).

**Practical rule:** to make the receiver broadcast a *target ellipsoidal height* H_e, enter `H_e + 33.517` (at this specific location -- re-measure if the station ever physically relocates, since undulation varies geographically). Cheapest way to re-derive it: enter any height, capture a short snippet off the local caster, decode the RTCM1006 position back to geodetic, and diff against what was entered.
**Verification method** (reusable for any future position change): connect directly to the local caster for ~15s (`GET /BASE0 HTTP/1.1` → `ICY 200 OK` → raw RTCM3, see `local_caster.cpp`), run `convbin -r rtcm3 -o out.obs snippet.bin`, read `APPROX POSITION XYZ` from the header, convert ECEF→geodetic (standard WGS84 iterative or Bowring's formula). Confirmed this round-trips to within 0.1mm once the corrected height was entered.

## No response-checking on any UM980 command
`Um980::command()` writes the ASCII command and waits a fixed 200ms — it does **not** read back or parse the receiver's response. Every command's "did it work" signal is empirical (does RTCM/BASEPOS reflect what was asked for), not a real ack. Keep this in mind before trusting a new command blindly — every UM980 change this session was verified by checking `/status` position and caster acceptance after deploy, not just "the command was sent without an ESP_RETURN_ON_ERROR failure" (that only catches UART write failures, not receiver-side rejection).

## Firmware: running Build13504, newer Build17548 available (deferred, 2026-07-11)
Current firmware confirmed via the RTCM1033-reported receiver version string ("13504-28051" in `REC # / TYPE / VERS`, visible in any rinex-recorder output -- there's no VERSIONA query wired up in our own code, `Um980::command()` doesn't read responses at all, see below). Checked Unicore's firmware distribution (mirrored at `github.com/sparkfun/SparkFun_RTK_Torch/UM980_Firmware`, the only accessible source found -- Unicore's own download-center page doesn't expose direct file listings): the newest build there is **Build17548** (release note dated the entry says Dec. 24 2024, filename says Oct 24 2024 -- minor inconsistency in Unicore's own doc, not ours to resolve), no newer build found as of this check.

Read the actual Build17548 release notes (11 items) and filtered for relevance to a 24/7 fixed marine base station on SIGNALGROUP 2. Most don't apply (QZSS L6/MADOCA-PPP, SIGNALGROUP 8/10, UAV HIGHDYN, Korean SBAS, serial parity) -- but four stood out:
1. **"Fixed the problem of no response with a certain probability when RTCM and NMEA commands were input together."** We send both every `configure_base()` call (RTCM on COM3, NMEA GSA/GSV on COM2) and never check the receiver's response (see "No response-checking" below) -- if this bug ever silently dropped one of our commands, we'd have no way of knowing. The single most relevant fix for us.
2. Improved STANDALONE exit mechanism and long-term computation logic -- general reliability for exactly the kind of continuous, weeks-long unattended operation this is.
3. Improved start-up time -- relevant given this session's whole RTK2go-ban saga was about slow-to-stabilize RTCM output after a mode transition (see [[project_ntrip]], `kMinOutboundRtcmBps` gate).
4. New `ENVINFOA`/`ENVINFOB` (environmental info output) -- could feed a future `/status` addition (e.g. receiver temperature, useful on a boat).

**Decision: deferred, not applied.** Updating means running Unicore's UPrecise tool (Windows GUI, `uprecise-v2-0.exe`) against the receiver directly -- not reachable through our web API or SSH, needs physical/USB access to the UM980's own config port. Revisit this if we ever hit a symptom that matches #1 (a config silently not taking effect) or #3 (startup instability recurs) -- those are the two most likely to actually explain a real future bug.

## No response-checking on any UM980 command
- **Elevation mask** (`MASK` command, default 5°) — standard, sensible default. Changing it trades sky-visibility (more low-elevation satellites tracked) against RMS/cycle-slip noise (low-elevation signals are noisier, more multipath-prone) — a real tradeoff, not a clear win in either direction, and no evidence it needs tuning.
- **IONMODE** — default `GPSK8` is correct; BDS-3/Galileo ionospheric models aren't supported on this hardware yet.
- **ALLEPHRTCM** — controls batching of RTCM ephemeris messages (1019/1020/1042 etc.), which we don't send at all. Moot unless we start broadcasting ephemeris RTCM types separately (a bigger, separate decision).
- **PSRPOSBIAS**, **MASK RTCMCN0** — available levers (position bias compensation; C/N0-based RTCM output filtering), not explicitly recommended for our scenario by the manual, no evidence of need.
