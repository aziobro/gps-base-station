// Host unit test for the RINEX satellite-ID formatter.
//
// Build & run:
//   c++ -std=c++17 -I main tools/test_rinex_satid.cpp -o /tmp/test_rinex_satid \
//       && /tmp/test_rinex_satid
//
// Regression target: the logger used to write every satellite as "G##" because
// the channel-status system field was read at the wrong bit offset, so BeiDou
// (system 4) decoded as GPS (0) and PRNs like 34/38/40/42 became G34/G38/...
// These tests pin the correct constellation prefixes and PRN ranges.
#include <cstdio>
#include <cstring>
#include <string>

#include "rinex_satid.h"

static int g_failures = 0;

static void expect_id(int sys, int prn, const char *want) {
    char got[4] = {0};
    bool ok = rinex::format_sat_id(sys, prn, got);
    if (!ok || strcmp(got, want) != 0) {
        printf("  FAIL: format(sys=%d, prn=%d) -> %s (wanted %s)\n",
               sys, prn, ok ? got : "<invalid>", want);
        ++g_failures;
    }
}

static void expect_invalid(int sys, int prn) {
    char got[4] = {0};
    if (rinex::format_sat_id(sys, prn, got)) {
        printf("  FAIL: format(sys=%d, prn=%d) -> %s (wanted <invalid>)\n",
               sys, prn, got);
        ++g_failures;
    }
}

int main() {
    using namespace rinex;

    // --- The bug: BeiDou must never be written as a GPS id ----------------
    expect_id(kSysBds, 34, "C34");   // was G34
    expect_id(kSysBds, 38, "C38");   // was G38
    expect_id(kSysBds, 40, "C40");   // was G40
    expect_id(kSysBds, 42, "C42");   // was G42
    expect_id(kSysBds, 63, "C63");

    // --- GPS: only G01..G32 -----------------------------------------------
    expect_id(kSysGps, 1, "G01");
    expect_id(kSysGps, 32, "G32");
    expect_invalid(kSysGps, 0);
    expect_invalid(kSysGps, 33);   // no fake G33+
    expect_invalid(kSysGps, 40);

    // --- GLONASS: R01..R24, slot+37 normalised ----------------------------
    expect_id(kSysGlo, 1, "R01");
    expect_id(kSysGlo, 24, "R24");
    expect_id(kSysGlo, 38, "R01");  // slot 1 reported as 38
    expect_id(kSysGlo, 61, "R24");  // slot 24 reported as 61
    expect_invalid(kSysGlo, 25);
    expect_invalid(kSysGlo, 62);

    // --- Galileo: E01..E36 ------------------------------------------------
    expect_id(kSysGal, 1, "E01");
    expect_id(kSysGal, 24, "E24");
    expect_id(kSysGal, 36, "E36");
    expect_invalid(kSysGal, 37);

    // --- QZSS: J01..J10, native 193.. normalised --------------------------
    expect_id(kSysQzss, 1, "J01");
    expect_id(kSysQzss, 193, "J01");
    expect_id(kSysQzss, 202, "J10");
    expect_invalid(kSysQzss, 11);

    // --- Unknown / unsupported system -------------------------------------
    expect_invalid(kSysSbas, 120);  // SBAS deliberately not emitted
    expect_invalid(6, 1);           // reserved
    expect_invalid(7, 1);           // "other"

    if (g_failures == 0) {
        printf("test_rinex_satid: PASS\n");
        return 0;
    }
    printf("test_rinex_satid: %d FAILURE(S)\n", g_failures);
    return 1;
}
