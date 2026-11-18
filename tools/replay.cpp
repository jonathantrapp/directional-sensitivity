#include "directional/transform.h"
#include "directional/profile_io.h"
#include "synthetic.h"
#include <array>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

using namespace directional;
namespace {
bool parse_gain(const char* text, std::uint32_t& gain) {
    char* end = nullptr;
    const double value = std::strtod(text, &end);
    if (end == text || *end || !std::isfinite(value) || value < .25 || value > 4) return false;
    gain = static_cast<std::uint32_t>(std::llround(value * unity_gain));
    return true;
}
using Clock=std::chrono::steady_clock;
double nanoseconds(Clock::duration value) { return std::chrono::duration<double,std::nano>(value).count(); }
double percentile(const std::vector<double>& sorted,double p) {
    const auto rank=static_cast<std::size_t>(std::ceil(p*static_cast<double>(sorted.size())));
    return sorted[rank?rank-1:0]; // nearest rank, p in (0,1]
}
int benchmark(Processor& processor,const std::vector<Delta>& packets,bool individual) {
    constexpr std::size_t batch_size=1000;
    const auto batch_count=(packets.size()+batch_size-1)/batch_size;
    std::vector<double> samples(individual?packets.size():batch_count);
    std::vector<double> clock_samples(10000);
    for (auto& sample:clock_samples) {
        const auto start=Clock::now();
        const auto end=Clock::now();
        sample=nanoseconds(end-start);
    }
    std::sort(clock_samples.begin(),clock_samples.end());
    for (std::size_t i=0;i<10000;++i) processor.process(packets[i%packets.size()]);
    processor.reset();
    std::int64_t checksum=0;
    std::size_t overflows=0;
    double measured_ns=0;
    // All sample storage and generated input were allocated before these loops.
    if (individual) {
        for (std::size_t i=0;i<packets.size();++i) {
            const auto start=Clock::now();
            const auto result=processor.process(packets[i]);
            const auto end=Clock::now();
            samples[i]=nanoseconds(end-start);
            measured_ns+=samples[i];
            checksum+=static_cast<std::int64_t>(result.delta.x)+result.delta.y;
            overflows+=result.overflow_bypass;
        }
    } else {
        for (std::size_t begin=0,batch=0;begin<packets.size();begin+=batch_size,++batch) {
            const auto end_index=std::min(begin+batch_size,packets.size());
            const auto start=Clock::now();
            for (auto i=begin;i<end_index;++i) {
                const auto result=processor.process(packets[i]);
                checksum+=static_cast<std::int64_t>(result.delta.x)+result.delta.y;
                overflows+=result.overflow_bypass;
            }
            const auto end=Clock::now();
            const auto elapsed=nanoseconds(end-start);
            measured_ns+=elapsed;
            samples[batch]=elapsed/static_cast<double>(end_index-begin);
        }
    }
    std::sort(samples.begin(),samples.end());
    std::cout << "packets=" << packets.size() << " samples=" << samples.size()
              << " mean_ns_per_packet=" << measured_ns/static_cast<double>(packets.size()) << '\n'
              << (individual?"instrumented_packet_ns":"batch_mean_ns_per_packet")
              << "_p50=" << percentile(samples,.5) << " p99=" << percentile(samples,.99)
              << " p99.9=" << percentile(samples,.999) << '\n'
              << "empty_clock_pair_ns_p50=" << percentile(clock_samples,.5)
              << " p99.9=" << percentile(clock_samples,.999)
              << " checksum=" << checksum << " overflow_bypasses=" << overflows << '\n'
              << "CPU replay only; not hardware polling or end-to-end latency. "
              << (individual?"Per-packet timings include clock/instrumentation overhead; none subtracted.\n":
                  "Percentiles describe batch averages, not individual packets; loop/checksum overhead included.\n");
    return std::cout?0:2;
}
} // namespace

int main(int argc, char** argv) {
    std::array<GainPair, 4> gains{};
    bool bench = false;
    bool individual=false,csv_diag=false,count_set=false;
    std::string generator;
    std::size_t count=1000000;
    bool manual_gains=false;
    std::string profile_path;
    std::uint32_t dpi=0;
    double sensitivity=0;
    for (int i = 1; i < argc; ++i) {
        const std::string option = argv[i];
        if (option == "--help") {
            std::cout << "replay [--ne X Y] [--se X Y] [--sw X Y] [--nw X Y] [--bench]\n"
                         "   or: replay --profile file.dsp --dpi N --sensitivity S [--bench]\n"
                         "--gen circle|flick|micro|cardinal|reversal|mixed [--count 1..10000000]\n"
                         "--csv-diag: raw_x,raw_y,out_x,out_y,res_x,res_y (residuals in Q24 units)\n"
                         "--bench or --bench-packet: >=1000000 generated packets; default mixed\n"
                         "stdin: signed integer x,y per line, one device. Output: x,y,out_x,out_y,overflow_bypass\n";
            return 0;
        }
        if (option=="--bench" || option=="--bench-packet") {
            if (bench) { std::cerr << "Choose one benchmark mode\n"; return 2; }
            bench=true; individual=option=="--bench-packet"; continue;
        }
        if (option=="--csv-diag") { csv_diag=true; continue; }
        if ((option=="--gen" || option=="--count") && i+1<argc) {
            const std::string value=argv[++i];
            if (option=="--gen") {
                if (!generator.empty() || !replay_tool::known_generator(value)) {
                    std::cerr << "Invalid/duplicate generator\n"; return 2;
                }
                generator=value;
            } else {
                std::istringstream parsed(value);
                std::int64_t n;
                if (count_set || !(parsed>>n) || !(parsed>>std::ws).eof() || n<1 || n>10000000) {
                    std::cerr << "Count must be in [1,10000000], specified once\n"; return 2;
                }
                count=static_cast<std::size_t>(n); count_set=true;
            }
            continue;
        }
        if ((option=="--profile" || option=="--dpi" || option=="--sensitivity") && i+1<argc) {
            const std::string value=argv[++i];
            if (option=="--profile") {
                if (!profile_path.empty()) { std::cerr << "Duplicate profile\n"; return 2; }
                profile_path=value;
            } else {
                std::istringstream parsed(value); parsed.imbue(std::locale::classic());
                if (option=="--dpi") {
                    std::int64_t n;
                    if (!(parsed>>n) || !(parsed>>std::ws).eof() || n<1 || n>1000000 || dpi) {
                        std::cerr << "Invalid/duplicate DPI\n"; return 2;
                    }
                    dpi=static_cast<std::uint32_t>(n);
                } else if (sensitivity || !(parsed>>sensitivity) || !(parsed>>std::ws).eof() ||
                           !std::isfinite(sensitivity) || sensitivity<=0) {
                    std::cerr << "Invalid/duplicate sensitivity\n"; return 2;
                }
            }
            continue;
        }
        unsigned q = 4;
        if (option == "--ne") q = 0;
        if (option == "--se") q = 1;
        if (option == "--sw") q = 2;
        if (option == "--nw") q = 3;
        if (q == 4 || i + 2 >= argc || !parse_gain(argv[i + 1], gains[q].x) ||
            !parse_gain(argv[i + 2], gains[q].y)) {
            std::cerr << "Invalid option/gain. Use --help; gains must be finite and in [0.25,4].\n";
            return 2;
        }
        i += 2;
        manual_gains=true;
    }
    Profile profile;
    if (!Profile::create(gains, profile)) {
        std::cerr << "Rejected profile: independent gains fail the angular monotonicity bound.\n";
        return 2;
    }
    if (!profile_path.empty()) {
        directional::SavedProfile saved;
        std::string error;
        std::ifstream file(profile_path);
        if (manual_gains || !file || !directional::read_profile(file,saved,error)) {
            std::cerr << "Cannot load profile (do not combine with manual gains): " << error << '\n'; return 2;
        }
        if (!directional::matches_setup(saved,dpi,sensitivity)) {
            std::cerr << "Profile setup mismatch: confirm --dpi and --sensitivity or recalibrate\n"; return 3;
        }
        profile=saved.transform;
    } else if (dpi || sensitivity) { std::cerr << "Setup flags require --profile\n"; return 2; }
    Processor processor;
    processor.apply(profile);
    if (bench && (csv_diag || count<1000000)) {
        std::cerr << "Benchmark requires >=1000000 packets and cannot emit CSV\n"; return 2;
    }
    if (generator.empty() && count_set && !bench) { std::cerr << "--count requires --gen or a benchmark\n"; return 2; }
    if (bench && generator.empty()) generator="mixed";
    std::vector<Delta> generated;
    if (!generator.empty()) generated=replay_tool::generate(generator,count);
    if (bench) return benchmark(processor,generated,individual);
    std::cout << (csv_diag?"raw_x,raw_y,out_x,out_y,res_x,res_y\n":"x,y,out_x,out_y,overflow_bypass\n");
    const auto emit=[&](Delta input) {
        const auto result=processor.process(input);
        std::cout << input.x << ',' << input.y << ',' << result.delta.x << ',' << result.delta.y << ',';
        if (csv_diag) {
            const auto residual=processor.residual();
            std::cout << residual.x << ',' << residual.y;
        } else std::cout << result.overflow_bypass;
        std::cout << '\n';
    };
    if (!generator.empty()) {
        for (const auto input:generated) emit(input);
        return std::cout?0:2;
    }
    std::string line;
    std::size_t line_number = 0;
    while (std::getline(std::cin, line)) {
        ++line_number;
        if (line.empty() || line[0] == '#') continue;
        std::istringstream row(line);
        std::int64_t x, y;
        char comma;
        if (!(row >> x >> comma >> y) || comma != ',' ||
            !(row >> std::ws).eof() || x < INT32_MIN || x > INT32_MAX || y < INT32_MIN || y > INT32_MAX) {
            std::cerr << "Invalid signed int32 x,y row at line " << line_number << '\n';
            return 2;
        }
        emit({static_cast<std::int32_t>(x),static_cast<std::int32_t>(y)});
    }
    if (std::cin.bad()) { std::cerr << "Input read failure\n"; return 2; }
    if (!std::cout) { std::cerr << "Output write failure\n"; return 2; }
}
