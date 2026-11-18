#include "directional/capture_session.h"
#include "directional/calibration.h"
#include "directional/movement_timing.h"
#include <array>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>

using namespace directional;
using namespace directional::capture;
namespace {
int checks=0;
void check(bool ok,const char* text) { ++checks; if (!ok) { std::cerr<<"FAIL: "<<text<<'\n'; std::exit(1); } }
Settings settings() { Settings s; s.device_path="test-only\\mouse,\"quoted\""; s.synthetic=true; return s; }
void schedule() {
    Session a,b;
    std::string error;
    check(a.prepare(settings(),error) && b.prepare(settings(),error),"prepare deterministic schedules");
    std::array<std::array<unsigned,2>,8> total{},validation{};
    for (std::size_t i=0;i<a.trials().size();++i) {
        const auto& x=a.trials()[i].target;
        const auto& y=b.trials()[i].target;
        check(x.direction==y.direction && x.distance==y.distance && x.repetition==y.repetition,"same seed order");
        ++total[x.direction][x.distance]; if (x.validation) ++validation[x.direction][x.distance];
        const double radius=std::hypot(x.counts.x*.5,x.counts.y*.5);
        check(std::abs(radius-(x.distance?224:128))<.5,"target counts map back to intended pixels");
    }
    for (unsigned d=0;d<8;++d) for (unsigned r=0;r<2;++r)
        check(total[d][r]==6 && validation[d][r]==2,"balanced distance/direction/split cells");
    check(!a.prepare(settings(),error),"never overwrite existing session");
    Session invalid; auto s=settings(); s.pixels_per_count=0;
    check(!invalid.prepare(s,error) && !invalid.prepared(),"bad mapping transactional rejection");
    s=settings(); s.canvas_width=INT32_MIN;
    check(!invalid.prepare(s,error),"geometry bounds reject signed extremes before subtraction");
}
void outcomes() {
    std::string error;
    Session s; check(s.prepare(settings(),error),"prepare outcome test");
    check(s.arm(1000),"arm"); const auto target=s.target()->counts;
    s.sample(2000,target,1,0); // same report's movement must precede click
    check(s.pending() && !s.armed() && !s.arm(3000),"pending trial cannot be replaced");
    s.flush();
    check(s.trials()[0].outcome==Outcome::hit && s.trials()[0].endpoint_x==target.x,"hit endpoint reconstruction");
    check(s.trials()[0].packets[0].time_us==1000,"timestamp relative to arm");
    check(!s.arm(1500),"later trials cannot precede previous trial end");
    s.arm(4000); s.sample(4500,{0,0},1,0); s.flush();
    check(s.trials()[1].outcome==Outcome::miss && s.trials()[1].packets.size()==1,"miss preserved");
    s.arm(10000); s.sample(12000,{4,5},0,0); s.abort(Outcome::focus_lost,14000); s.flush();
    check(s.trials()[2].outcome==Outcome::focus_lost && s.trials()[2].packets.size()==1,"interruption preserves trace");
    s.arm(20000); s.sample(21000,{1,1},0,0); s.sample(20500,{2,2},0,0); s.flush();
    check(s.trials()[3].outcome==Outcome::clock_error && s.trials()[3].packets.size()==1,"clock regression stops");
    s.arm(30000); s.sample(31000,{10000,10000},0,1); s.flush();
    check(s.trials()[4].outcome==Outcome::absolute_input && s.trials()[4].endpoint_x==0 &&
        s.trials()[4].packets.size()==1,"absolute values archived but not treated as relative movement");
    s.arm(40000); s.sample(41000,{0,0},0,4); s.flush();
    check(s.trials()[5].outcome==Outcome::attributes_changed,"changed attributes invalidates trial");
    s.arm(50000); s.tick(5050000); s.flush();
    check(s.trials()[6].outcome==Outcome::timeout,"idle timeout captured");
    Session limited; auto small=settings(); small.packet_capacity=2;
    limited.prepare(small,error); limited.arm(0);
    limited.sample(1,{1,1},0,0); limited.sample(2,{1,1},0,0); limited.sample(3,{1,1},0,0); limited.flush();
    check(limited.trials()[0].outcome==Outcome::capacity && limited.trials()[0].packets.size()==2,"bounded capture no silent truncation");
}
void segmentation() {
    RecordedTrial t; t.target.counts={100,-100}; t.outcome=Outcome::miss; t.target_painted=true;
    t.packets={{100,{70,-70},0,0},{200,{50,-50},0,0},{300,{-20,20},0,0},{400,{0,0},1,0}};
    auto first=first_flick(t);
    check(first.eligible && first.end==2 && first.x==120 && first.y==-120,"overshoot retained before correction");
    check(t.packets.size()==4 && t.packets[2].delta.x==-20,"full correction trace unchanged");
    t.packets={{100,{80,-80},0,0},{70100,{20,-20},0,0},{70200,{0,0},1,0}};
    first=first_flick(t);
    check(first.eligible && first.end==1 && first.x==80,"first pause boundary");
    t.packets={{1,{100,0},0,0},{2,{0,-100},1,0}};
    check(!first_flick(t).eligible,"alternating-cardinal exposure cannot identify diagonal gain");
    t.target.counts={100,0}; check(!first_flick(t).eligible,"cardinal targets remain diagnostics");
    t.packets={{1,{100,1},0,0},{2,{0,-1},1,0}};
    const auto all=exposure(t);
    check(all.movement_packets==2 && all.mixed_packets==1 && all.weighted_path_counts>0,
          "cardinal targets can contain mixed packets; full exposure is not guessed from target direction");
    t.target.counts={100,-100}; t.target_painted=false;
    check(!first_flick(t).eligible,"unpainted target cannot label an initial flick");
    t.target_painted=true; t.first_paint_us=100;
    check(!first_flick(t).eligible,"pre-paint motion remains recorded but ineligible");
    t.first_paint_us=0;
    t.packets={{10,{0,1},0,0},{100000,{50,-51},0,0},{100100,{50,-50},0,0},{100200,{0,0},1,0}};
    first=first_flick(t);
    check(first.eligible && first.begin==0 && first.end==3 && first.x==100 && first.y==-100,
          "one-count starting jitter and idle gap stay in an intact recovered prefix");
    t.packets={{1,{30,-30},0,0},{2,{-1,1},0,0},{3,{71,-71},0,0},{4,{0,0},1,0}};
    check(first_flick(t).eligible && first_flick(t).x==100,"interior quantization reversal tolerated");
    t.packets={{1,{3,-1},0,0},{2,{-1,1},0,0},{3,{-4,4},0,0},{4,{100,-100},1,0}};
    check(!first_flick(t).eligible,"tiny fragment before sustained reversal is not a full flick");
    t.packets={{1,{100,-100},0,0},{2,{-1,1},0,0},{3,{-3,3},0,0},{4,{4,-4},1,0}};
    first=first_flick(t);
    check(first.eligible && first.end==1 && first.x==100,"cumulative correction rolls back all reversal packets");
    t.packets={{1,{-8,0},0,0},{2,{108,-100},1,0}};
    check(!first_flick(t).eligible,"significant wrong start cannot be relabeled as clean flick");
    t.packets={{1,{20,-20},0,0},{70000,{80,-80},1,0}};
    check(!first_flick(t).eligible,"pause cannot turn a short fragment into a candidate");
    t.packets={{1,{100,-100},0,1}};
    check(!first_flick(t).eligible,"unsupported raw flags cannot enter a candidate");
    for (const int sx:{-1,1}) for (const int sy:{-1,1}) {
        t.target.counts={sx*100,sy*100};
        t.packets={{1,{-sx,0},0,0},{2,{sx*51,sy*50},0,0},{3,{sx*50,sy*50},1,0}};
        const auto recovered=first_flick(t);
        check(recovered.eligible && recovered.x==sx*100 && recovered.y==sy*100,
              "starting jitter recovery is symmetric in all four quadrants");
    }
}
void exports(const std::filesystem::path& root) {
    std::string error;
    Session s; check(s.prepare(settings(),error),"prepare exported session");
    std::uint64_t time=0;
    while (!s.complete()) {
        check(s.arm(time),"arm scheduled trial");
        const auto target=s.target()->counts;
        s.mark_presented(time+100);
        // Whole relative movement plus a click gives a known identity trace.
        s.sample(time+1000,target,0,0); s.sample(time+2000,{0,0},1,0); s.flush(); time+=10000;
    }
    check(export_session(s,root,error),"export complete synthetic session");
    check(!export_session(s,root,error),"existing export directory cannot be overwritten");
    std::ifstream file(root/"candidate-trials.csv"); std::vector<Trial> trials;
    check(read_trials(file,trials,error) && trials.size()==48,"derived candidates load through existing estimator parser");
    CalibrationReport report;
    check(calibrate(trials,1600,1,report,error) && !report.improved,"identity recordings do not manufacture corrective gains");
    std::ifstream manifest(root/"session.json"); std::stringstream text; text<<manifest.rdbuf();
    check(text.str().find("\"synthetic\": true")!=std::string::npos &&
          text.str().find("\"session_complete\": true")!=std::string::npos,"provenance/completion metadata");
    check(text.str().find("\\\"quoted\\\"")!=std::string::npos,"device path JSON escaping");
    std::ifstream plot(root/"plots"/"trial_1.svg"); std::stringstream svg; svg<<plot.rdbuf();
    check(svg.str().find("<polyline")!=std::string::npos && svg.str().find("endpoint")!=std::string::npos,"inspectable full-trace plot exported");
}
void phases() {
    RecordedTrial t; t.target.counts={100,-100}; t.target_painted=true; t.outcome=Outcome::hit;
    t.packets={{100,{-20,20},0,0},{200,{60,-60},0,0},{300,{70,-70},0,0},{400,{-10,10},1,0}};
    const auto p=movement_phases(t);
    check(p.identified && p.begin==1 && p.end==3,"later forward stroke annotated despite preparation");
    check(p.start_x==-20 && p.start_y==20 && p.end_x==110 && p.end_y==-110,
          "phase annotation preserves absolute start and end coordinates");
    check(!first_flick(t).eligible,"annotated later stroke never silently promoted to fitting");
    check(t.packets.size()==4 && t.packets[0].delta.x==-20,"phase analysis never mutates full trace");
    t.target.counts={100,0};
    t.packets={{100,{80,0},0,0},{70100,{20,0},1,0}};
    const auto cardinal=movement_phases(t);
    check(cardinal.identified && cardinal.end==1 && cardinal.end_x==80,"cardinal targets receive phase diagnostics");
    t.target_painted=false;
    check(!movement_phases(t).identified,"unpainted trial not annotated as known movement");
    t.target_painted=true; t.outcome=Outcome::focus_lost;
    check(!movement_phases(t).identified,"interrupted trial remains unclassified");
    t.outcome=Outcome::hit; t.packets={{100,{100,0},0,1}};
    check(!movement_phases(t).identified,"unsupported phase input rejected");
}
void comparison(const std::filesystem::path& root) {
    auto config=settings(); config.comparison=true;
    std::array<GainPair,4> gains{}; gains[0]={81920,81920};
    check(Profile::create(gains,config.candidate_profile),"valid comparison profile");
    Session s; std::string error; check(s.prepare(config,error),"prepare comparison schedule");
    std::array<std::array<unsigned,2>,8> candidate_counts{},baseline_counts{};
    for (const auto& t:s.trials()) {
        auto& counts=t.target.candidate_condition?candidate_counts:baseline_counts;
        ++counts[t.target.direction][t.target.distance];
    }
    for (unsigned d=0;d<8;++d) for (unsigned distance=0;distance<2;++distance)
        check(candidate_counts[d][distance]==3 && baseline_counts[d][distance]==3,"conditions balanced in every target cell");
    std::uint64_t time=0; bool tested_candidate=false;
    while (!s.complete()) {
        const auto target=*s.target(); s.arm(time); s.mark_presented(time+1);
        if (target.candidate_condition && target.direction==7) {
            const Delta input{target.counts.x*4/5,target.counts.y*4/5};
            s.sample(time+100,input,1,0); s.flush();
            const auto& t=s.trials()[s.next_index()-1];
            check(t.endpoint_x==input.x && t.packets[0].delta.x==input.x,"raw comparison data retained");
            check(t.cursor_endpoint_x!=t.endpoint_x && t.outcome==Outcome::hit,"hit uses transformed displayed endpoint");
            tested_candidate=true;
        } else { s.sample(time+100,target.counts,1,0); s.flush(); }
        time+=1000;
    }
    check(tested_candidate,"candidate condition exercised");
    check(export_session(s,root,error),"comparison export succeeds");
    std::ifstream candidate_file(root/"candidate-trials.csv"); std::string line;
    std::getline(candidate_file,line);
    check(!std::getline(candidate_file,line),"comparison packets never exported as identity fitting candidates");
    std::ifstream meta(root/"session.json"); std::stringstream json; json<<meta.rdbuf();
    check(json.str().find("randomized_profile_comparison")!=std::string::npos &&
        json.str().find("81920")!=std::string::npos,"comparison provenance contains mode and fixed gains");
    auto odd=config; odd.repetitions=7; Session invalid;
    check(!invalid.prepare(odd,error),"unbalanced comparison schedule rejected");
}
void timing() {
    RecordedTrial t; t.target.counts={200,-200}; t.target_painted=true; t.outcome=Outcome::hit;
    for (std::uint64_t i=0;i<10;++i) t.packets.push_back({100000+i*10000,{10,-10},0,0});
    for (std::uint64_t i=0;i<10;++i) t.packets.push_back({400000+i*10000,{10,-10},0,0});
    t.packets.push_back({600000,{0,0},1,0});
    const auto a=movement_timing(t);
    check(a.identified && a.epochs==2 && a.begin==0 && a.end==10,"speed epochs separate movement from later correction");
    check(a.end_x==100 && a.end_y==-100 && a.start_x==0,"timing preserves original spatial coordinates");
    check(t.packets.size()==21 && a.onset_us>=t.first_paint_us,"timing never mutates or predates painted trace");
    t.packets={{100000,{200,-200},1,0}};
    check(!movement_timing(t).identified,"isolated spike cannot fake sustained movement through smoothing");
    t.packets={{UINT64_MAX,{1,1},1,0}};
    check(!movement_timing(t).identified,"unbounded timestamp rejected before allocating bins");
    t.packets={{100000,{20,-20},0,0},{90000,{20,-20},1,0}};
    check(!movement_timing(t).identified,"nonmonotonic timing rejected");
    t.packets={{100000,{20,-20},0,1}};
    check(!movement_timing(t).identified,"absolute input excluded from timing");
}
void wide_angles(const std::filesystem::path& root) {
    auto config=settings(); config.comparison=true; config.direction_count=24; config.repetitions=2;
    Session s; std::string error; check(s.prepare(config,error),"prepare wider-angle comparison");
    check(s.trials().size()==96,"24 angles do not increase total trial burden");
    std::array<std::array<std::array<unsigned,2>,2>,24> counts{};
    unsigned cardinals=0;
    for (const auto& t:s.trials()) {
        check(t.target.angle_index<24,"exact angular cell retained");
        ++counts[t.target.angle_index][t.target.distance][t.target.candidate_condition?1:0];
        const double angle=std::atan2(static_cast<double>(t.target.counts.y),static_cast<double>(t.target.counts.x));
        constexpr double tau=6.2831853071795864769;
        const double expected=tau*t.target.angle_index/24;
        check(std::abs(std::sin(angle-expected))<.005,"quantized target follows requested 15-degree angle");
        const double radius=std::hypot(t.target.counts.x*.5,t.target.counts.y*.5);
        check(std::abs(radius-(t.target.distance?224:128))<.5,"wide-angle target remains within ring geometry");
        if (!t.target.counts.x || !t.target.counts.y) ++cardinals;
    }
    for (const auto& angle:counts) for (const auto& distance:angle)
        check(distance[0]==1 && distance[1]==1,"conditions balanced per exact angle and distance");
    check(cardinals==16,"wide mode retains exactly four cardinal direction controls");
    std::uint64_t time=0;
    while (!s.complete()) {
        const auto target=s.target()->counts; s.arm(time); s.mark_presented(time+1);
        s.sample(time+100,target,1,0); s.flush(); time+=1000;
    }
    check(export_session(s,root,error),"wide-angle session exports exact angular cells");
    config.comparison=false; Session invalid;
    check(!invalid.prepare(config,error),"wide comparison cannot silently change calibration schedule");
}
}
int main(int argc,char** argv) {
    if (argc!=2) { std::cerr<<"Test requires a NEW export directory argument\n"; return 2; }
    const std::filesystem::path root(argv[1]);
    schedule(); outcomes(); segmentation(); phases(); timing(); wide_angles(root.string()+"-wide"); comparison(root.string()+"-comparison"); exports(root);
    std::cout<<"PASS: "<<checks<<" capture/session checks, synthetic only\n";
}
