// RINEX 3 satellite-ID formatting for the UM980 base station.
//
// Pure, dependency-free helpers shared by the RINEX logger (firmware) and the
// host unit test (tools/test_rinex_satid.cpp). No ESP-IDF / FreeRTOS deps so it
// compiles with a plain host C++ compiler.
//
// The UM980 follows the Novatel channel-tracking-status convention: the
// satellite-system id lives in bits 16-18 of the status word:
//   0 = GPS, 1 = GLONASS, 2 = SBAS, 3 = Galileo, 4 = BeiDou, 5 = QZSS.
//
// Each constellation must be written with its own RINEX prefix and a PRN within
// the legal range — writing every satellite as "G##" (the old bug) is invalid
// RINEX and is rejected by OPUS.
#pragma once

#include <cstdint>
#include <cstdio>

namespace rinex {

enum SatSystem : uint8_t {
    kSysGps  = 0,
    kSysGlo  = 1,
    kSysSbas = 2,
    kSysGal  = 3,
    kSysBds  = 4,
    kSysQzss = 5,
};

struct SystemDef {
    uint8_t     sys;        // channel-tracking-status system id
    char        code;       // RINEX constellation letter
    int         max_prn;    // inclusive upper bound for a legal PRN
    int         offset;     // subtract from the raw PRN when raw >= threshold
    int         threshold;  // raw-PRN value at/above which the offset applies
    const char *obs;        // space-separated RINEX 3 observation codes (4 per band)
    int         nobs;       // number of observation codes (= 4 * frequency bands)
};

// Header emission order. SBAS is intentionally omitted: OPUS ignores it.
// Each constellation lists 4 observation codes (C/L/D/S) per frequency band.
// GPS carries three bands (L1 C/A, L2 P(Y), L5) so OPUS gets a clean dual-
// frequency pair plus L5; the others carry their two primary bands.
inline const SystemDef *systems(int *count) {
    static const SystemDef kSystems[] = {
        {kSysGps,  'G', 32,   0, 100000, "C1C L1C D1C S1C C2W L2W D2W S2W C5Q L5Q D5Q S5Q", 12},
        {kSysGlo,  'R', 24,  37,     38, "C1C L1C D1C S1C C2C L2C D2C S2C",  8},
        {kSysGal,  'E', 36,   0, 100000, "C1C L1C D1C S1C C5Q L5Q D5Q S5Q",  8},
        {kSysBds,  'C', 63, 140,    141, "C2I L2I D2I S2I C6I L6I D6I S6I",  8},
        {kSysQzss, 'J', 10, 192,    193, "C1C L1C D1C S1C C2L L2L D2L S2L",  8},
    };
    if (count) *count = (int)(sizeof(kSystems) / sizeof(kSystems[0]));
    return kSystems;
}

// Number of frequency bands a system emits (4 observation codes per band).
inline int system_bands(const SystemDef *d) { return d ? d->nobs / 4 : 0; }

inline const SystemDef *find_system(int sys) {
    int n = 0;
    const SystemDef *t = systems(&n);
    for (int i = 0; i < n; ++i) {
        if (t[i].sys == (uint8_t)sys) return &t[i];
    }
    return nullptr;
}

// Fill out[>=4] with a valid RINEX 3 satellite id ("G12", "R05", "E24", "C38",
// "J03"). Returns false when the system is unsupported or the PRN falls outside
// the constellation's legal range (caller should skip the record and log it).
// raw_prn is the UM980 PRN field, which may carry a per-constellation offset
// (e.g. GLONASS slot+37) that this normalises away.
inline bool format_sat_id(int sys, int raw_prn, char out[4]) {
    const SystemDef *d = find_system(sys);
    if (!d) return false;
    int prn = raw_prn;
    if (prn >= d->threshold) prn -= d->offset;
    if (prn < 1 || prn > d->max_prn) return false;
    // prn is now in [1, max_prn] and max_prn <= 99, so it is always two digits;
    // the % 100 makes that provable to -Wformat-truncation.
    snprintf(out, 4, "%c%02d", d->code, prn % 100);
    return true;
}

}  // namespace rinex
