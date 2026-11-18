#include "directional/movement_timing.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <locale>

namespace directional::capture {
MovementTiming movement_timing(const RecordedTrial& t) {
    MovementTiming out;
    if (t.outcome!=Outcome::hit && t.outcome!=Outcome::miss) { out.reason="incomplete_trial"; return out; }
    if (!t.target_painted || t.packets.empty()) { out.reason="no_painted_trace"; return out; }
    constexpr std::uint64_t bin_us=10000,max_us=10000000;
    const auto last=t.packets.back().time_us;
    if (last>max_us || t.first_paint_us>last) { out.reason="unsupported_time_range"; return out; }
    const auto bins=static_cast<std::size_t>((last-t.first_paint_us)/bin_us+1);
    std::vector<double> speed(bins,0),smooth(bins,0);
    std::uint64_t previous=0;
    for (const auto& s:t.packets) {
        if (s.time_us<previous || (s.flags&5u)) { out.reason="invalid_trace"; return out; }
        previous=s.time_us;
        if (!s.delta.x && !s.delta.y) continue;
        if (s.time_us<t.first_paint_us) { out.reason="movement_before_paint"; return out; }
        speed[static_cast<std::size_t>((s.time_us-t.first_paint_us)/bin_us)]+=std::hypot(static_cast<double>(s.delta.x),static_cast<double>(s.delta.y))*100;
    }
    for (std::size_t i=0;i<bins;++i) {
        const auto lo=i?i-1:0,hi=std::min(bins-1,i+1);
        for (auto j=lo;j<=hi;++j) smooth[i]+=speed[j]/static_cast<double>(hi-lo+1);
        out.peak_counts_per_second=std::max(out.peak_counts_per_second,smooth[i]);
    }
    if (!out.peak_counts_per_second) { out.reason="no_motion"; return out; }
    const double threshold=.2*out.peak_counts_per_second;
    std::size_t first_bin=0,final_bin=0;
    for (std::size_t i=0;i<bins;) {
        if (smooth[i]<threshold) { ++i; continue; }
        const auto start=i;
        while (i<bins && smooth[i]>=threshold) ++i;
        std::size_t occupied=0;
        for (auto j=start;j<i;++j) if (speed[j]>0) ++occupied;
        // Smoothing can spread one isolated spike across three bins. Require
        // three actual occupied bins too, so one report cannot fake a stroke.
        if (i-start<3 || occupied<3) continue;
        if (!out.epochs) { first_bin=start; final_bin=i; }
        ++out.epochs;
    }
    if (!out.epochs) { out.reason="no_sustained_epoch"; return out; }
    out.onset_us=t.first_paint_us+first_bin*bin_us;
    out.end_us=std::min(last+1,t.first_paint_us+final_bin*bin_us); // exclusive endpoint
    std::int64_t x=0,y=0; out.begin=t.packets.size(); out.end=t.packets.size();
    for (std::size_t i=0;i<t.packets.size();++i) {
        const auto& s=t.packets[i];
        if (out.begin==t.packets.size() && s.time_us>=out.onset_us) { out.begin=i; out.start_x=x; out.start_y=y; }
        if (s.time_us>=out.end_us && out.end==t.packets.size()) { out.end=i; out.end_x=x; out.end_y=y; }
        x+=s.delta.x; y+=s.delta.y;
    }
    if (out.end==t.packets.size()) { out.end_x=x; out.end_y=y; }
    out.identified=out.begin<out.end;
    out.reason=out.identified?(out.epochs>1?"multiple_epochs_review_required":"single_epoch_candidate"):"empty_epoch";
    return out;
}
bool export_timing(const std::vector<RecordedTrial>& trials,const std::filesystem::path& path) {
    std::ofstream file(path); file.imbue(std::locale::classic()); file<<std::setprecision(17);
    file<<"trial,identified,reason,epochs,begin,end,onset_us,end_us,start_x,start_y,end_x,end_y,peak_raw_counts_per_second\n";
    for (std::size_t i=0;i<trials.size();++i) {
        const auto t=movement_timing(trials[i]);
        file<<"trial_"<<i+1<<','<<t.identified<<','<<t.reason<<','<<t.epochs<<','<<t.begin<<','<<t.end<<','
            <<t.onset_us<<','<<t.end_us<<','<<t.start_x<<','<<t.start_y<<','<<t.end_x<<','<<t.end_y<<','<<t.peak_counts_per_second<<'\n';
    }
    file.close(); return static_cast<bool>(file);
}
}
