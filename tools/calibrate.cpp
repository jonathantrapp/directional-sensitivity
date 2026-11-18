#include "directional/calibration.h"
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <locale>
#include <sstream>

int main(int argc, char** argv) {
    if (argc!=4) {
        std::cerr << "Usage: calibrate trials.csv DPI REFERENCE_SENSITIVITY > profile.dsp\n";
        return 2;
    }
    std::int64_t dpi=0;
    double sensitivity=0;
    std::istringstream dpi_text(argv[2]),sens_text(argv[3]);
    dpi_text.imbue(std::locale::classic()); sens_text.imbue(std::locale::classic());
    if (!(dpi_text>>dpi) || !(dpi_text>>std::ws).eof() || dpi<1 || dpi>1000000 ||
        !(sens_text>>sensitivity) || !(sens_text>>std::ws).eof() ||
        !std::isfinite(sensitivity) || sensitivity<=0) {
        std::cerr << "Invalid DPI/reference sensitivity\n"; return 2;
    }
    std::ifstream input(argv[1]);
    if (!input) { std::cerr << "Cannot open trial CSV\n"; return 2; }
    std::vector<directional::Trial> trials;
    directional::CalibrationReport report;
    std::string error;
    if (!directional::read_trials(input,trials,error) ||
        !directional::calibrate(trials,static_cast<std::uint32_t>(dpi),sensitivity,report,error)) {
        std::cerr << error << '\n'; return 2;
    }
    const char* names[]={"NE","SE","SW","NW"};
    std::cerr << std::fixed << std::setprecision(3);
    for (unsigned q=0;q<4;++q) {
        const auto g=report.candidate.transform.gain(static_cast<directional::Quadrant>(q));
        std::cerr << names[q] << ": train=" << report.training_count[q] << " validation=" << report.validation_count[q]
                  << " relative endpoint RMSE " << report.baseline_rmse[q]*100 << "% -> "
                  << report.candidate_rmse[q]*100 << "% gain="
                  << static_cast<double>(g.x)/directional::unity_gain << '\n';
    }
    if (!report.improved) {
        std::cerr << "No candidate passed validation. No profile exported.\n"; return 3;
    }
    if (!directional::write_profile(std::cout,report.candidate,error)) {
        std::cerr << error << '\n'; return 2;
    }
    std::cerr << "Candidate exported; validate with new aim trials before applying system-wide.\n";
}
