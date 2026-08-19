#pragma once

#include <cstdint>
#include <chrono>
#include <string>

namespace ifs {

using Timestamp = std::chrono::system_clock::time_point;
using SteadyTimestamp = std::chrono::steady_clock::time_point;
using Duration = std::chrono::milliseconds;

inline constexpr const char* SERVICE_NAME    = "c6g3-infotainment-services";
inline constexpr const char* SERVICE_VERSION = "0.1.0";

} // namespace ifs
