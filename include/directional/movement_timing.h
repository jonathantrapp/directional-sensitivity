#pragma once
#include "directional/capture_session.h"

namespace directional::capture {
struct MovementTiming {
    bool identified=false;
    std::size_t begin=0,end=0,epochs=0;
    std::uint64_t onset_us=0,end_us=0;
    std::int64_t start_x=0,start_y=0,end_x=0,end_y=0;
    double peak_counts_per_second=0;
    const char* reason="not_identified";
};
// Independent offline timing heuristic, NOT recovered Aimlabs math. Ten-ms
// bins, three-bin smoothing, 20% of peak threshold, >=30ms sustained motion.
// Dispatch timestamps do not measure human reaction or sensor latency.
MovementTiming movement_timing(const RecordedTrial& trial);
bool export_timing(const std::vector<RecordedTrial>& trials,const std::filesystem::path& path);
}
