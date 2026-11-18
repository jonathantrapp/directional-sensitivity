#pragma once
#include "directional/transform.h"
#include <cstdint>
#include <iosfwd>
#include <string>

namespace directional {
// User-mode metadata only. Never put strings or streams on the input callback.
struct SavedProfile {
    Profile transform;
    std::uint32_t dpi = 0;
    double reference_sensitivity = 1;
};
bool read_profile(std::istream& input, SavedProfile& output, std::string& error);
bool write_profile(std::ostream& output, const SavedProfile& profile, std::string& error);
bool parse_gain(const std::string& text, std::uint32_t& output);
// Known setup changes invalidate calibration. DPI changes are not always exposed
// by mice, so the user must supply/confirm the current setup.
bool matches_setup(const SavedProfile& profile, std::uint32_t dpi, double sensitivity) noexcept;
} // namespace directional
