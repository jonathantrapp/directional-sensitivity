#include "directional/capture_session.h"
#include <charconv>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Row=std::map<std::string,std::string>;
// Recorder CSVs have numeric fields and fixed labels; quoted/free-text CSV is
// deliberately unsupported. Device strings live in the untouched manifest.
std::vector<std::string> split(std::string line) {
    if (!line.empty() && line.back()=='\r') line.pop_back();
    if (line.size()>4096 || line.find('"')!=std::string::npos) throw std::runtime_error("Unsupported CSV row");
    std::vector<std::string> values;
    std::size_t start=0;
    for (;;) {
        const auto end=line.find(',',start); values.push_back(line.substr(start,end-start));
        if (end==std::string::npos) return values;
        start=end+1;
    }
}
template<class T> T number(const Row& row,const char* key) {
    const auto& value=row.at(key); T out{};
    const auto parsed=std::from_chars(value.data(),value.data()+value.size(),out);
    if (parsed.ec!=std::errc{} || parsed.ptr!=value.data()+value.size()) throw std::runtime_error("Invalid numeric CSV value");
    return out;
}
template<class F> void read(const std::filesystem::path& path,F consume,std::size_t limit) {
    std::ifstream input(path); std::string line;
    if (!std::getline(input,line)) throw std::runtime_error("Cannot read CSV header");
    const auto header=split(line); std::size_t count=0;
    while (std::getline(input,line)) {
        if (++count>limit) throw std::runtime_error("CSV limit exceeded");
        const auto values=split(line);
        if (values.size()!=header.size()) throw std::runtime_error("CSV column mismatch");
        Row row;
        for (std::size_t i=0;i<header.size();++i)
            if (!row.emplace(header[i],values[i]).second) throw std::runtime_error("Duplicate CSV header");
        consume(row);
    }
    if (input.bad()) throw std::runtime_error("CSV read failure");
}
}
int main(int argc,char** argv) {
    using namespace directional::capture;
    if (argc!=3) { std::cerr<<"Usage: review_capture SAVED_SESSION NEW_REVIEW_DIRECTORY\n"; return 2; }
    try {
        const std::filesystem::path source(argv[1]),output(argv[2]);
        if (!std::filesystem::is_regular_file(source/"session.json")) throw std::runtime_error("Missing source manifest");
        std::ifstream manifest_file(source/"session.json");
        const std::string manifest((std::istreambuf_iterator<char>(manifest_file)),std::istreambuf_iterator<char>());
        if (manifest.find("randomized_profile_comparison")!=std::string::npos)
            throw std::runtime_error("Comparison sessions require compare_summary.py; they are not identity calibration data");
        std::vector<RecordedTrial> trials;
        std::vector<std::size_t> counts;
        read(source/"trials.csv",[&](const Row& row) {
            if (row.at("trial")!="trial_"+std::to_string(trials.size()+1)) throw std::runtime_error("Unexpected trial order/ID");
            RecordedTrial t;
            t.target.counts={number<std::int32_t>(row,"target_x"),number<std::int32_t>(row,"target_y")};
            bool direction_found=false;
            for (unsigned d=0;d<8;++d) if (row.at("direction")==direction_name(d)) {
                t.target.direction=d; direction_found=true;
            }
            if (!direction_found) throw std::runtime_error("Invalid direction");
            const auto& split_name=row.at("split");
            if (split_name!="train" && split_name!="validation") throw std::runtime_error("Invalid split");
            t.target.validation=split_name=="validation";
            bool found=false;
            for (int i=0;i<=static_cast<int>(Outcome::geometry_changed);++i)
                if (row.at("outcome")==outcome_name(static_cast<Outcome>(i))) { t.outcome=static_cast<Outcome>(i); found=true; }
            if (!found) throw std::runtime_error("Invalid outcome");
            const auto painted=number<unsigned>(row,"target_painted");
            if (painted>1) throw std::runtime_error("Invalid paint marker");
            t.target_painted=painted==1; t.first_paint_us=number<std::uint64_t>(row,"first_paint_us");
            t.endpoint_x=number<std::int64_t>(row,"endpoint_x"); t.endpoint_y=number<std::int64_t>(row,"endpoint_y");
            counts.push_back(number<std::size_t>(row,"packet_count")); trials.push_back(std::move(t));
        },4096);
        if (trials.empty()) throw std::runtime_error("No recorded trials");
        read(source/"packets.csv",[&](const Row& row) {
            const auto& id=row.at("trial");
            if (id.substr(0,6)!="trial_") throw std::runtime_error("Invalid packet trial ID");
            Row index{{"index",id.substr(6)}}; const auto n=number<std::size_t>(index,"index");
            if (!n || n>trials.size()) throw std::runtime_error("Unknown packet trial");
            auto& t=trials[n-1];
            if (number<std::size_t>(row,"sample")!=t.packets.size()) throw std::runtime_error("Packet order mismatch");
            Sample s{number<std::uint64_t>(row,"time_us"),
                {number<std::int32_t>(row,"dx"),number<std::int32_t>(row,"dy")},
                number<std::uint16_t>(row,"buttons"),number<std::uint16_t>(row,"flags")};
            if (!t.packets.empty() && s.time_us<t.packets.back().time_us) throw std::runtime_error("Nonmonotonic timestamps");
            t.packets.push_back(s);
        },1000000);
        for (std::size_t i=0;i<trials.size();++i) {
            const auto& t=trials[i]; std::int64_t x=0,y=0;
            for (const auto& p:t.packets) if (!(p.flags&0x5u)) { x+=p.delta.x; y+=p.delta.y; }
            if (t.packets.size()!=counts[i] || x!=t.endpoint_x || y!=t.endpoint_y)
                throw std::runtime_error("Recorded counts/endpoint mismatch");
        }
        std::string error;
        if (!export_review(trials,output,error)) throw std::runtime_error(error);
        // Preserve provenance without rewriting the original session manifest.
        std::filesystem::copy_file(source/"session.json",output/"source-session.json");
        std::cout<<"Reviewed "<<trials.size()<<" trials. Original files unchanged; inspect review.csv and plots.\n";
        return 0;
    } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 2; }
}
