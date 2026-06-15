---
name: project_rinex
description: RINEX logging — signal→band classification, Unicore UM980 signal codes, antenna metadata, OPUS/CSRS-PPP
metadata:
  node_type: memory
  type: project
---

How `rinex_logger.cpp` turns UM980 RANGEA into RINEX 3.03, and the hard-won fix that made OPUS/CSRS-PPP accept it. See [[project_structure]].

## The classification bug (fixed ota74/ota76)
- OPUS rejected merged files with 9011 "noisy/kinematic". Root cause: L1/L2/L5 were assigned to RINEX columns by a "primary" bit `(tstat >> 23) & 1` — but **bit 23 is inside the 5-bit signal-type field (bits 21-25)**, so it scrambled frequencies (Doppler ratio of the two "dual" columns was 1.0435 = L2/L5, and L1 C/A was dropped). OPUS can't form the iono-free combo from scrambled L1/L2 → noisy.
- Fix: classify by **signal type → band** in `signal_band(sys, sig, &pref)`. Never use a "primary" bit. Doppler is the unambiguous frequency probe (scales exactly with freq; carrier ADR has an arbitrary ambiguity so its magnitude is NOT a frequency probe).

## Unicore UM980 signal-type codes (decoded on-device via Doppler ratios)
Channel tracking status: system = bits 16-18, signal type = bits 21-25.
- **GPS (0):** 0=L1 C/A, 9=L2C, 14=L5. (No L2P/sig5 emitted in practice.)
- **GLONASS (1):** 0=L1 C/A, 5=L2 C/A.
- **Galileo (3):** 2=E1, 12=E5a, 17=E5b.
- **BeiDou (4):** 0=B1I, 8=B1C, 21=B3I, 12=B2a, 13=B2b.

## RINEX band layout (per `rinex_satid.h` SystemDef.obs / nobs)
- GPS = 3 bands (12 obs): L1 `C1C L1C`, L2 `C2W L2W` (note: carries L2C data under the L2W label — OPUS handled it, 90% amb fix), L5 `C5Q L5Q`.
- GLONASS/Galileo/BeiDou = 2 bands (8 obs). GAL E1`1C`+E5a`5Q`; BDS B1I`2I`+B3I`6I`.
- `signal_band` returns -1 for signals not carried (GPS L2P/L1C, GAL E5b, BDS B1C/B2a/B2b) → dropped cleanly, never misfiled.
- Verify any change by downloading a file and checking Doppler ratios: GPS b0/b1=1.283, b0/b2=1.339; GLO 1.286; GAL 1.339; BDS 1.231.

## Antenna metadata (ota75+)
- RINEX header needs a real `ANT # / TYPE` (16-char model + 4-char radome) or CSRS-PPP refuses and OPUS needs a manual antenna pick.
- Configurable on `/config` (Antenna section) → NVS keys `ant_model`/`ant_radome`/`ant_h`. Defaults: `HXCGPS500` / `NONE` / 0.0. Passed via `RinexLogger::start(...)`; takes effect on the next file.
- ARP height 0.0 → OPUS/CSRS solve for the ARP (the `1008` note is informational).

## Result
- Full-day file on the fixed firmware: OPUS 90% ambiguity fix, 0.017 m RMS, ~2 mm H / 2 mm V network accuracy. Base ARP (ITRF2020) ≈ 1324149.681, -4677290.873, 4115305.213 (Keyport NJ).
- See [[project_build_deploy]] for the build/OTA flow. RINEX collection is toggled on the status page / `/rinex/toggle`; needs Base TX mode.
