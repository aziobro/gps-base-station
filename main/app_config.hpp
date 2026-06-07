#pragma once

#include <cstdint>

namespace config {

inline constexpr char kFirmwareVersion[] = "2026.06.07-idf2";

inline constexpr uint32_t kSurveyMinTimeSec = 300;
inline constexpr uint32_t kSurveyBlockTimeSec = 60;
inline constexpr int kSurveyMinBlocks = 5;
inline constexpr float kSurveyMaxStabilityM = 0.50F;

inline constexpr int kRtcmBasePositionRateSec = 5;
inline constexpr int kRtcmGpsRateSec = 1;
inline constexpr int kRtcmGlonassRateSec = 1;
inline constexpr int kRtcmGalileoRateSec = 1;
inline constexpr int kRtcmBeidouRateSec = 1;

inline constexpr uint16_t kLocalNtripPort = 2101;
inline constexpr char kLocalMountpoint[] = "BASE0";

inline constexpr char kRtk2goHost[] = "rtk2go.com";
inline constexpr uint16_t kRtk2goPort = 2101;
inline constexpr char kOnocoyHost[] = "servers.onocoy.com";
inline constexpr uint16_t kOnocoyPort = 2101;
inline constexpr char kRtkdataHost[] = "rtkdata.online";
inline constexpr uint16_t kRtkdataPort = 2101;

}  // namespace config
