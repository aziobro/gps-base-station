#pragma once

#include <cstdint>

namespace config {

// Firmware version is sourced from version.txt at build time and read at
// runtime via esp_app_get_description()->version — do not duplicate it here.

inline constexpr uint32_t kSurveyMinTimeSec = 300;
inline constexpr uint32_t kSurveyBlockTimeSec = 60;
inline constexpr int kSurveyMinBlocks = 5;
inline constexpr float kSurveyMaxStabilityM = 0.50F;

inline constexpr int kRtcmBasePositionRateSec = 5;
inline constexpr int kRtcmGpsRateSec = 1;
inline constexpr int kRtcmGlonassRateSec = 1;
inline constexpr int kRtcmGalileoRateSec = 1;
inline constexpr int kRtcmQzssRateSec = 1;
inline constexpr int kRtcmBeidouRateSec = 1;
inline constexpr int kRtcmNavicRateSec = 1;
inline constexpr int kRtcmGlonassBiasRateSec = 30;

// Minimum sustained RTCM output (bytes/sec, one full measurement window --
// see BaseStation::run()'s rtcm_bps_ calc) required before the outbound
// NTRIP push destinations (RTK2go/Onocoy/RTKdata) are allowed to connect.
// Added after a real RTK2go ban (2026-07-06): a fresh mode transition
// (post-relocation survey -> transmit) unsuspended streams on the very
// first published RTCM frame, but the receiver hadn't reached steady
// output yet -- RTK2go accepted the handshake, got ~0kb for 25s, and
// banned the IP for "not enough data soon enough after connecting."
// Local caster is unaffected (LAN clients, no external abuse-prevention).
inline constexpr uint32_t kMinOutboundRtcmBps = 900;

inline constexpr uint16_t kLocalNtripPort = 2101;
inline constexpr char kLocalMountpoint[] = "BASE0";

// SoftAP (hotspot) defaults. The AP is secured with WPA2; this password is
// used until one is set on the /config page (stored in NVS). It must be 8-63
// characters. Documented in the README so a fresh device can be reached before
// any custom password is configured.
inline constexpr char kAccessPointSsid[] = "GPS-BaseStation";
inline constexpr char kDefaultApPassword[] = "gpsbase-rtk";

inline constexpr char kRtk2goHost[] = "rtk2go.com";
inline constexpr uint16_t kRtk2goPort = 2101;
inline constexpr char kOnocoyHost[] = "servers.onocoy.com";
inline constexpr uint16_t kOnocoyPort = 2101;
inline constexpr char kRtkdataHost[] = "rtkdata.online";
inline constexpr uint16_t kRtkdataPort = 2101;

}  // namespace config
