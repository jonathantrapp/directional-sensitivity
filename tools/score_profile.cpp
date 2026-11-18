#include "directional/calibration.h"
#include <fstream>
#include <iomanip>
#include <iostream>
#include <locale>
#include <sstream>

// Counterfactual packet replay only. Human corrections would change when a
// profile is active; these scores are not measured live aiming improvements.
int main(int argc,char** argv) {
    if (argc!=5) { std::cerr<<"Usage: score_profile PROFILE TRIAL_CSV DPI SENSITIVITY\n"; return 2; }
    directional::SavedProfile saved;
    std::string error;
    std::ifstream profile(argv[1]),input(argv[2]);
    std::vector<directional::Trial> trials;
    std::uint32_t dpi=0; double sensitivity=0;
    std::istringstream d(argv[3]),s(argv[4]);
    d.imbue(std::locale::classic()); s.imbue(std::locale::classic());
    if (!(d>>dpi) || !(d>>std::ws).eof() || !(s>>sensitivity) || !(s>>std::ws).eof() ||
        !directional::read_profile(profile,saved,error) || !directional::matches_setup(saved,dpi,sensitivity) ||
        !directional::read_trials(input,trials,error)) {
        std::cerr<<"Invalid profile/setup or trial data: "<<error<<'\n'; return 2;
    }
    std::cout<<std::setprecision(17)<<"trial,target_x,target_y,baseline_x,baseline_y,candidate_x,candidate_y,overflow_packets\n";
    for (const auto& t:trials) {
        directional::Processor processor; processor.apply(saved.transform);
        std::int64_t bx=0,by=0,cx=0,cy=0; std::size_t overflows=0;
        for (const auto delta:t.packets) {
            const auto result=processor.process(delta);
            bx+=delta.x; by+=delta.y; cx+=result.delta.x; cy+=result.delta.y;
            if (result.overflow_bypass) ++overflows;
        }
        std::cout<<t.id<<','<<t.target.x<<','<<t.target.y<<','<<bx<<','<<by<<','<<cx<<','<<cy<<','<<overflows<<'\n';
    }
    return std::cout?0:2;
}
