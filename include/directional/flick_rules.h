#pragma once
#include <cstdint>

namespace directional::flick {
// Offline segmentation only; these constants never affect live mouse counts.
// Hysteresis tolerates quantization noise, not sustained corrective movement.
constexpr std::int64_t motion_counts=4;
constexpr double minimum_progress=.25;
constexpr const char* version="projected_hysteresis_4counts_pause_60000us_minprogress_25pct_v2";
}
