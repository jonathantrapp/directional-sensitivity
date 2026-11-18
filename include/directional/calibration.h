#pragma once
#include "directional/profile_io.h"
#include <array>
#include <iosfwd>
#include <string>
#include <vector>

namespace directional {
struct Trial {
    std::string id;
    bool validation = false;
    Delta target{}; // Desired endpoint, in identity-profile raw-count units.
    std::vector<Delta> packets;
};
struct CalibrationReport {
    SavedProfile candidate;
    std::array<std::size_t,4> training_count{}, validation_count{};
    std::array<double,4> baseline_rmse{}, candidate_rmse{};
    bool improved = false;
};
// Offline calibration only: fit on train trials and evaluate a separate held-out
// split. No live learning or target-aware correction occurs on the input path.
bool read_trials(std::istream& input, std::vector<Trial>& output, std::string& error);
bool calibrate(const std::vector<Trial>& trials, std::uint32_t dpi, double sensitivity,
               CalibrationReport& report, std::string& error);
} // namespace directional
