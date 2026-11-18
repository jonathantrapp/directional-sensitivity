#include "directional/calibration.h"
#include "directional/flick_rules.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <istream>
#include <locale>
#include <map>
#include <sstream>

namespace directional {
namespace {
unsigned quadrant(Delta d) { return d.y<0 ? (d.x>0 ? 0u:3u) : (d.x>0 ? 1u:2u); }
struct Features { double x=0,y=0,wx=0,wy=0; };
Features features(const Trial& trial) {
    Features f;
    for (auto d:trial.packets) {
        const double x=d.x,y=d.y;
        f.x+=x; f.y+=y;
        if (x==0 || y==0 || quadrant(d)!=quadrant(trial.target)) continue;
        const double ax=std::abs(x),ay=std::abs(y);
        const double r=std::min(ax,ay)/std::max(ax,ay);
        const double w=r*r*(3-2*r);
        f.wx+=x*w; f.wy+=y*w;
    }
    return f;
}
double endpoint_error(const Trial& trial, const Profile& profile) {
    Processor p; p.apply(profile);
    std::int64_t x=0,y=0;
    for (auto d:trial.packets) { auto r=p.process(d); x+=r.delta.x; y+=r.delta.y; }
    const double dx=static_cast<double>(x)-trial.target.x;
    const double dy=static_cast<double>(y)-trial.target.y;
    const double tx=trial.target.x,ty=trial.target.y;
    return (dx*dx+dy*dy)/(tx*tx+ty*ty);
}
bool parse_int(const std::string& text, std::int32_t& result) {
    std::istringstream row(text); row.imbue(std::locale::classic());
    std::int64_t value;
    if (!(row>>value) || !(row>>std::ws).eof() || value<INT32_MIN || value>INT32_MAX) return false;
    result=static_cast<std::int32_t>(value); return true;
}
}
bool read_trials(std::istream& input, std::vector<Trial>& output, std::string& error) {
    std::vector<Trial> trials;
    std::map<std::string,std::size_t> indexes;
    std::string line;
    std::size_t rows=0,packets=0;
    if (!std::getline(input,line)) { error="Missing trial CSV header"; return false; }
    if (!line.empty() && line.back()=='\r') line.pop_back();
    if (line!="trial,split,target_x,target_y,dx,dy") { error="Invalid trial CSV header"; return false; }
    while (std::getline(input,line)) {
        ++rows;
        if (line.size()>256 || ++packets>1000000) { error="Trial data exceeds limits"; return false; }
        if (!line.empty() && line.back()=='\r') line.pop_back();
        if (line.empty()) continue;
        std::istringstream row(line); std::array<std::string,6> fields;
        bool ok=true;
        for (auto& field:fields) if (!std::getline(row,field,',')) ok=false;
        std::string extra;
        if (std::getline(row,extra,',') || (!line.empty() && line.back()==',')) ok=false;
        Delta target{},d{};
        if (!ok || fields[0].empty() || fields[0].size()>64 ||
            (fields[1]!="train" && fields[1]!="validation") ||
            !parse_int(fields[2],target.x) || !parse_int(fields[3],target.y) ||
            !parse_int(fields[4],d.x) || !parse_int(fields[5],d.y)) {
            error="Invalid trial CSV row "+std::to_string(rows+1); return false;
        }
        auto found=indexes.find(fields[0]);
        if (found==indexes.end()) {
            if (trials.size()>=4096) { error="Too many trials"; return false; }
            indexes.emplace(fields[0],trials.size());
            trials.push_back({fields[0],fields[1]=="validation",target,{}});
            found=indexes.find(fields[0]);
        }
        auto& trial=trials[found->second];
        if (trial.validation!=(fields[1]=="validation") || trial.target.x!=target.x || trial.target.y!=target.y) {
            error="Inconsistent metadata for trial "+trial.id; return false;
        }
        trial.packets.push_back(d);
    }
    if (input.bad()) { error="Trial CSV read failed"; return false; }
    output=std::move(trials); error.clear(); return true;
}
bool calibrate(const std::vector<Trial>& trials, std::uint32_t dpi, double sensitivity,
               CalibrationReport& report, std::string& error) {
    if (dpi<1 || dpi>1000000 || !std::isfinite(sensitivity) || sensitivity<=0) {
        error="DPI and reference sensitivity must be positive"; return false;
    }
    CalibrationReport result;
    result.candidate.dpi=dpi; result.candidate.reference_sensitivity=sensitivity;
    std::array<std::vector<double>,4> estimates;
    for (const auto& t:trials) {
        if (t.target.x==0 || t.target.y==0 || t.packets.empty()) {
            error="Trials must have nonzero diagonal targets and packet data: "+t.id; return false;
        }
        const unsigned q=quadrant(t.target);
        double progress=0,peak=0;
        const double length=std::hypot(static_cast<double>(t.target.x),static_cast<double>(t.target.y));
        for (auto d:t.packets) {
            progress+=(static_cast<double>(d.x)*t.target.x+static_cast<double>(d.y)*t.target.y)/length;
            if (peak-progress>=flick::motion_counts) {
                error="Trial includes reversal/correction; supply the initial flick only: "+t.id;
                return false;
            }
            peak=std::max(peak,progress);
        }
        const auto f=features(t);
        const double denominator=f.wx*f.wx+f.wy*f.wy;
        if (denominator<1) {
            error="Trial has no usable diagonal sensitivity (cardinal-only packets): "+t.id; return false;
        }
        if (t.validation) ++result.validation_count[q];
        else {
            ++result.training_count[q];
            // Under linked gains and a single quadrant: endpoint(c)=base+(c-1)*weighted.
            // Project target error onto weighted motion. Median resists outliers.
            estimates[q].push_back(1+(f.wx*(t.target.x-f.x)+f.wy*(t.target.y-f.y))/denominator);
        }
    }
    std::array<GainPair,4> gains{};
    for (unsigned q=0;q<4;++q) {
        if (result.training_count[q]<6 || result.validation_count[q]<3) {
            error="Need at least 6 training and 3 held-out validation flicks per quadrant"; return false;
        }
        auto& e=estimates[q]; std::sort(e.begin(),e.end());
        const double median=e.size()%2 ? e[e.size()/2] : (e[e.size()/2-1]+e[e.size()/2])/2;
        // Conservative initial calibration authority; manual profiles support wider gains.
        const double bounded=std::clamp(median,.75,1.25);
        const auto g=static_cast<std::uint32_t>(std::llround(bounded*unity_gain));
        gains[q]={g,g};
    }
    if (!Profile::create(gains,result.candidate.transform)) { error="Candidate validation failed"; return false; }
    for (const auto& t:trials) if (t.validation) {
        const auto q=quadrant(t.target);
        result.baseline_rmse[q]+=endpoint_error(t,Profile{});
        result.candidate_rmse[q]+=endpoint_error(t,result.candidate.transform);
    }
    result.improved=false;
    for (unsigned q=0;q<4;++q) {
        result.baseline_rmse[q]=std::sqrt(result.baseline_rmse[q]/result.validation_count[q]);
        result.candidate_rmse[q]=std::sqrt(result.candidate_rmse[q]/result.validation_count[q]);
        // Keep an already accurate direction unchanged. Validation gates each
        // proposal; it is not proof of improvement on future human aim trials.
        if (result.candidate_rmse[q] <= result.baseline_rmse[q]*.98 &&
            result.baseline_rmse[q]-result.candidate_rmse[q] >= .001) result.improved=true;
        else { gains[q]={}; result.candidate_rmse[q]=result.baseline_rmse[q]; }
    }
    // Jitter may touch another quadrant. Recheck the actual final combination
    // after any veto, rather than reporting scores for a discarded combination.
    for (unsigned pass=0;pass<5;++pass) {
        if (!Profile::create(gains,result.candidate.transform)) { error="Final validation failed"; return false; }
        result.candidate_rmse.fill(0);
        for (const auto& t:trials) if (t.validation)
            result.candidate_rmse[quadrant(t.target)]+=endpoint_error(t,result.candidate.transform);
        bool changed=false; result.improved=false;
        for (unsigned q=0;q<4;++q) {
            result.candidate_rmse[q]=std::sqrt(result.candidate_rmse[q]/result.validation_count[q]);
            if (gains[q].x==unity_gain) continue;
            if (result.candidate_rmse[q]<=result.baseline_rmse[q]*.98 &&
                result.baseline_rmse[q]-result.candidate_rmse[q]>=.001) result.improved=true;
            else { gains[q]={}; changed=true; }
        }
        if (!changed) break;
    }
    report=result; error.clear(); return true;
}
} // namespace directional
