#include "directional/calibration.h"
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <sstream>

using namespace directional;
namespace {
int checks=0;
void check(bool good,const char* message) {
    ++checks;
    if (!good) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
std::vector<Trial> samples() {
    std::vector<Trial> result;
    for (unsigned q=0;q<4;++q) for (int i=0;i<9;++i) {
        const int sx=(q==0 || q==1) ? 1:-1;
        const int sy=(q==0 || q==3) ? -1:1;
        const int n=80+i*4;
        // Correctable packet data include cardinal motion, so the required
        // diagonal gain differs from target-distance / total-distance.
        result.push_back({std::to_string(q)+"_"+std::to_string(i),i>=6,
            {sx*(n*5/4+20),sy*(n*5/4+20)},{{sx*n,sy*n},{sx*20,0},{0,sy*20}}});
    }
    return result;
}
void profiles() {
    SavedProfile source; source.dpi=1600; source.reference_sensitivity=.723456789;
    std::array<GainPair,4> gains{};
    gains[0]={81921,70001};
    check(Profile::create(gains,source.transform),"test profile accepted");
    std::stringstream text;
    std::string error;
    check(write_profile(text,source,error),"write profile");
    SavedProfile loaded;
    check(read_profile(text,loaded,error),"round trip read");
    check(loaded.dpi==source.dpi && loaded.reference_sensitivity==source.reference_sensitivity,
          "metadata exactly round trips");
    check(loaded.transform.gain(Quadrant::NE).x==81921 && loaded.transform.gain(Quadrant::NE).y==70001,
          "fixed point anchors exactly round trip");
    check(matches_setup(loaded,1600,.723456789),"setup matches");
    check(!matches_setup(loaded,800,.723456789),"DPI invalidates calibration");
    check(!matches_setup(loaded,1600,1),"sensitivity invalidates calibration");
    const std::string good="directional-profile=1\ndpi=1600\nreference_sensitivity=1\nne=1,1\nse=1,1\nsw=1,1\nnw=1,1\n";
    for (const auto& invalid : {good+"dpi=800\n",good+"unknown=1\n",std::string("directional-profile=2\n"),
                               good+std::string(65537,'#')}) {
        std::istringstream bad(invalid);
        loaded.dpi=3200;
        check(!read_profile(bad,loaded,error) && loaded.dpi==3200,"invalid profile is transactional");
    }
    for (const char* invalid : {"nan","inf","0","-1","4.01","1junk","1,1"}) {
        std::uint32_t g;
        check(!parse_gain(invalid,g),"bad gain rejected");
    }
}
void estimation() {
    auto trials=samples();
    CalibrationReport report;
    std::string error;
    check(calibrate(trials,1600,1,report,error),"fit trace-aware calibration");
    check(report.improved,"held-out improvement");
    for (unsigned q=0;q<4;++q) {
        check(report.candidate.transform.gain(static_cast<Quadrant>(q)).x==81920,
              "fit excludes immutable cardinal counts");
        check(report.baseline_rmse[q]>.1 && report.candidate_rmse[q]<.0001,"exact replay validation");
    }
    // Held-out data does not participate in fitting: contradictory validation
    // vetoes a training candidate rather than learning its gains.
    for (auto& t:trials) if (t.validation) {
        t.target.x=static_cast<int>(t.packets[0].x*.75)+(t.target.x>0?20:-20);
        t.target.y=static_cast<int>(t.packets[0].y*.75)+(t.target.y>0?20:-20);
    }
    check(calibrate(trials,1600,1,report,error) && !report.improved,"held-out regression veto");
    check(report.candidate.transform.gain(Quadrant::NE).x==unity_gain,"failed proposals revert to identity");
    trials=samples();
    for (auto& t:trials) if (t.target.x>0 && t.target.y<0) {
        t.target.x=t.packets[0].x+20; t.target.y=t.packets[0].y-20;
    }
    check(calibrate(trials,1600,1,report,error) && report.improved,"keep accurate direction while improving others");
    check(report.candidate.transform.gain(Quadrant::NE).x==unity_gain,"accurate direction unchanged");
    trials=samples();
    trials[0].packets={{100,0},{0,-100}};
    check(!calibrate(trials,1600,1,report,error),"cardinal-only evidence is unidentifiable");
    trials=samples(); trials[0].packets.insert(trials[0].packets.begin(),{0,1});
    check(calibrate(trials,1600,1,report,error),"one-count pre-onset jitter remains usable");
    trials=samples(); trials[0].packets.push_back({-6,0});
    check(!calibrate(trials,1600,1,report,error),"corrections excluded from initial flick evidence");
    trials=samples(); trials.pop_back();
    check(!calibrate(trials,1600,1,report,error),"insufficient held-out coverage rejected");
    trials=samples();
    // One wildly wrong training target cannot dominate the robust median.
    trials[0].target={10000,-10000};
    check(calibrate(trials,1600,1,report,error) && report.candidate.transform.gain(Quadrant::NE).x==81920,
          "training outlier resistance");
}
void csv() {
    std::string error;
    std::vector<Trial> result;
    std::istringstream good("trial,split,target_x,target_y,dx,dy\na,train,100,-100,40,-40\na,train,100,-100,40,-40\n");
    check(read_trials(good,result,error) && result.size()==1 && result[0].packets.size()==2,"group packets by trial");
    for (const char* row : {"a,train,100,-100,4,-4,extra", "a,train,100,-100,4,-4,", "a,bogus,100,-100,4,-4",
                            "a,train,100,-100,2147483648,1"}) {
        std::istringstream bad(std::string("trial,split,target_x,target_y,dx,dy\n")+row+"\n");
        check(!read_trials(bad,result,error) && result.size()==1,"bad row preserves output");
    }
    std::istringstream mismatch("trial,split,target_x,target_y,dx,dy\na,train,100,-100,40,-40\na,validation,100,-100,40,-40\n");
    check(!read_trials(mismatch,result,error),"split metadata cannot change within a trial");
}
}
int main() { profiles(); estimation(); csv(); std::cout << "PASS: " << checks << " utility checks\n"; }
