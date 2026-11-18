#pragma once
#include "directional/transform.h"
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace directional::capture {
struct Settings {
    std::uint32_t dpi=1600,seed=0x51a7u;
    double reference_sensitivity=1,pixels_per_count=.5;
    unsigned repetitions=6; // per direction/distance: four train, two validation
    unsigned direction_count=8;
    std::uint64_t timeout_us=5000000;
    std::size_t packet_capacity=65536;
    std::string device_path;
    int canvas_width=1030,canvas_height=780,origin_px_x=515,origin_px_y=435;
    bool synthetic=false;
    bool comparison=false;
    Profile candidate_profile{};
};
struct Target {
    Delta counts{};
    unsigned direction=0,distance=0,repetition=0;
    bool validation=false;
    bool candidate_condition=false;
    unsigned angle_index=0; // exact target-angle cell; direction is nearest compass sector
};
struct Sample {
    std::uint64_t time_us=0; // relative to arm, QPC message-dispatch time
    Delta delta{};
    std::uint16_t buttons=0,flags=0;
};
enum class Outcome { not_attempted,hit,miss,timeout,user_cancel,focus_lost,device_removed,
                     attributes_changed,absolute_input,read_error,capacity,clock_error,geometry_changed };
const char* outcome_name(Outcome outcome) noexcept;
const char* direction_name(unsigned direction) noexcept;
struct RecordedTrial {
    Target target;
    Outcome outcome=Outcome::not_attempted;
    std::uint64_t armed_us=0,ended_us=0; // start from session clock; end relative to arm
    std::uint64_t first_paint_us=0; // QPC after first target paint, NOT display presentation
    bool target_painted=false;
    std::int64_t endpoint_x=0,endpoint_y=0;
    std::int64_t cursor_endpoint_x=0,cursor_endpoint_y=0;
    std::vector<Sample> packets;
};
struct Segment {
    std::size_t begin=0,end=0,mixed_packets=0;
    std::int64_t x=0,y=0;
    bool eligible=false;
    const char* reason="not_attempted";
    const char* boundary="none";
};
// Versioned heuristic, never ground-truth labels: contiguous original prefix,
// four-count reversal hysteresis along the target direction, >=60ms pause,
// >=25% target progress.
// Small starting movements remain in the prefix; full traces stay unchanged.
Segment first_flick(const RecordedTrial& trial) noexcept;
struct MovementPhases {
    std::size_t begin=0,end=0;
    std::int64_t start_x=0,start_y=0,end_x=0,end_y=0;
    bool identified=false;
    const char* reason="not_identified";
    const char* boundary="none";
};
// Descriptive annotation for ALL target directions. Finds the first forward
// departure from the preceding low-water position, with the original offset.
// It never promotes a later stroke into the calibration candidate set.
MovementPhases movement_phases(const RecordedTrial& trial) noexcept;
struct Exposure {
    std::size_t movement_packets=0,mixed_packets=0;
    double path_counts=0,weighted_path_counts=0;
};
// Describes the entire relative trace, even for a cardinal-target or rejected
// trial. Mixed exposure is potential sensitivity, not proof of applied gain.
Exposure exposure(const RecordedTrial& trial) noexcept;

class Session {
public:
    bool prepare(const Settings& settings,std::string& error);
    bool arm(std::uint64_t session_us) noexcept;
    void mark_presented(std::uint64_t session_us) noexcept;
    void sample(std::uint64_t session_us,Delta delta,std::uint16_t buttons,std::uint16_t flags) noexcept;
    void tick(std::uint64_t session_us) noexcept;
    void abort(Outcome why,std::uint64_t session_us) noexcept;
    // Call on UI timer/command, not from a report callback: copies the used
    // preallocated buffer to its permanent trial without allocating per report.
    void flush();
    bool armed() const noexcept { return armed_; }
    bool pending() const noexcept { return pending_; }
    bool prepared() const noexcept { return prepared_; }
    bool complete() const noexcept { return prepared_ && next_==trials_.size(); }
    std::size_t next_index() const noexcept { return next_; }
    const Target* target() const noexcept { return next_<trials_.size()?&trials_[next_].target:nullptr; }
    const std::vector<RecordedTrial>& trials() const noexcept { return trials_; }
    const Settings& settings() const noexcept { return settings_; }
    std::int64_t x() const noexcept { return x_; }
    std::int64_t y() const noexcept { return y_; }
    std::int64_t cursor_x() const noexcept { return cursor_x_; }
    std::int64_t cursor_y() const noexcept { return cursor_y_; }
    std::size_t active_packets() const noexcept { return buffer_.size(); }
private:
    void finish(Outcome outcome,std::uint64_t session_us) noexcept;
    Settings settings_;
    std::vector<RecordedTrial> trials_;
    std::vector<Sample> buffer_;
    std::size_t next_=0;
    bool prepared_=false,armed_=false,pending_=false;
    std::uint64_t arm_us_=0,last_us_=0;
    std::int64_t x_=0,y_=0;
    std::int64_t cursor_x_=0,cursor_y_=0;
    Processor processor_;
};
// New-directory export only. Full packets, all trial outcomes, metadata and
// heuristic candidates are separate artifacts. Never overwrites an old session.
bool export_session(const Session& session,const std::filesystem::path& directory,std::string& error);
// Offline derived artifacts only; does not replace source packets/metadata.
bool export_review(const std::vector<RecordedTrial>& trials,const std::filesystem::path& directory,std::string& error);
} // namespace directional::capture
