#include "directional/capture_session.h"
#include "directional/flick_rules.h"
#include "directional/movement_timing.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <locale>
#include <sstream>
#include <system_error>

namespace directional::capture {
const char* direction_name(unsigned d) noexcept {
    const char* names[]={"E","SE","S","SW","W","NW","N","NE"};
    return d<8?names[d]:"invalid";
}
const char* outcome_name(Outcome outcome) noexcept {
    switch(outcome) {
        case Outcome::not_attempted:return "not_attempted";
        case Outcome::hit:return "hit";
        case Outcome::miss:return "miss";
        case Outcome::timeout:return "timeout";
        case Outcome::user_cancel:return "user_cancel";
        case Outcome::focus_lost:return "focus_lost";
        case Outcome::device_removed:return "device_removed";
        case Outcome::attributes_changed:return "attributes_changed";
        case Outcome::absolute_input:return "absolute_input";
        case Outcome::read_error:return "read_error";
        case Outcome::capacity:return "capacity";
        case Outcome::clock_error:return "clock_error";
        case Outcome::geometry_changed:return "geometry_changed";
    }
    return "invalid";
}
bool Session::prepare(const Settings& settings,std::string& error) {
    if (prepared_) { error="A session cannot be replaced. Export it and start a new application."; return false; }
    if (settings.dpi<1 || settings.dpi>1000000 || !std::isfinite(settings.reference_sensitivity) ||
        settings.reference_sensitivity<=0 || !std::isfinite(settings.pixels_per_count) ||
        settings.pixels_per_count<.01 || settings.pixels_per_count>8 ||
        (settings.direction_count!=8 && settings.direction_count!=24) ||
        (settings.direction_count==24 && !settings.comparison) ||
        settings.repetitions<(settings.comparison?2u:6u) ||
        settings.repetitions>12 || (settings.comparison && settings.repetitions%2) ||
        settings.timeout_us<1000000 || settings.timeout_us>10000000 ||
        settings.packet_capacity<2 || settings.packet_capacity>200000 || settings.device_path.empty() ||
        settings.canvas_width<480 || settings.canvas_width>16384 || settings.canvas_height<480 || settings.canvas_height>16384 ||
        settings.origin_px_x<239 || settings.origin_px_y<239 ||
        settings.origin_px_x>settings.canvas_width-239 || settings.origin_px_y>settings.canvas_height-239) {
        error="Invalid capture settings"; return false;
    }
    std::vector<RecordedTrial> planned;
    planned.reserve(settings.direction_count*2*settings.repetitions);
    constexpr int sx[]={1,1,0,-1,-1,-1,0,1},sy[]={0,1,1,1,0,-1,-1,-1};
    for (unsigned direction=0;direction<settings.direction_count;++direction) for (unsigned distance=0;distance<2;++distance)
        for (unsigned rep=0;rep<settings.repetitions;++rep) {
            const double pixels=distance?224.:128.;
            Delta counts{};
            const unsigned sector=((direction*8+settings.direction_count/2)/settings.direction_count)%8;
            if (settings.direction_count==8) {
                const double divisor=(sx[direction] && sy[direction])?std::sqrt(2.):1.;
                const auto component=static_cast<int32>(std::llround(pixels/settings.pixels_per_count/divisor));
                counts={sx[direction]*component,sy[direction]*component};
            } else {
                constexpr double tau=6.2831853071795864769;
                const double theta=tau*direction/settings.direction_count;
                counts={static_cast<int32>(std::llround(std::cos(theta)*pixels/settings.pixels_per_count)),
                        static_cast<int32>(std::llround(std::sin(theta)*pixels/settings.pixels_per_count))};
            }
            Target target{counts,sector,distance,rep,
                          rep>=settings.repetitions-settings.repetitions/3};
            // Three repetitions per condition in every direction/distance cell.
            // Labels are hidden in the task; both conditions share one shuffle.
            target.candidate_condition=settings.comparison && rep%2==1;
            target.angle_index=direction;
            planned.push_back({target});
        }
    // Explicit shuffle makes a seed replayable across standard-library versions.
    std::uint32_t random=settings.seed;
    for (std::size_t i=planned.size()-1;i>0;--i) {
        random=random*1664525u+1013904223u;
        std::swap(planned[i],planned[random%(i+1)]);
    }
    buffer_.reserve(settings.packet_capacity);
    settings_=settings; trials_=std::move(planned); prepared_=true;
    error.clear(); return true;
}
bool Session::arm(std::uint64_t session_us) noexcept {
    if (!prepared_ || armed_ || pending_ || next_>=trials_.size() || session_us<last_us_) return false;
    buffer_.clear(); x_=y_=0; arm_us_=last_us_=session_us;
    cursor_x_=cursor_y_=0;
    processor_.apply(trials_[next_].target.candidate_condition?settings_.candidate_profile:Profile{});
    armed_=true; return true;
}
void Session::finish(Outcome outcome,std::uint64_t session_us) noexcept {
    if (!armed_) return;
    auto& trial=trials_[next_];
    trial.outcome=outcome; trial.armed_us=arm_us_;
    last_us_=std::max(last_us_,session_us);
    trial.ended_us=last_us_-arm_us_;
    trial.endpoint_x=x_; trial.endpoint_y=y_;
    trial.cursor_endpoint_x=cursor_x_; trial.cursor_endpoint_y=cursor_y_;
    armed_=false; pending_=true;
}
void Session::mark_presented(std::uint64_t time) noexcept {
    if (!armed_ || trials_[next_].target_painted) return;
    if (time<last_us_) { finish(Outcome::clock_error,last_us_); return; }
    trials_[next_].target_painted=true; trials_[next_].first_paint_us=time-arm_us_; last_us_=time;
}
void Session::sample(std::uint64_t time,Delta delta,std::uint16_t buttons,std::uint16_t flags) noexcept {
    if (!armed_) return;
    if (time<last_us_) { finish(Outcome::clock_error,last_us_); return; }
    last_us_=time;
    if (buffer_.size()==settings_.packet_capacity) { finish(Outcome::capacity,time); return; }
    buffer_.push_back({time-arm_us_,delta,buttons,flags}); // reserved before capture
    if (flags&0x4u) { finish(Outcome::attributes_changed,time); return; }
    if (flags&0x1u) { finish(Outcome::absolute_input,time); return; }
    // <=200k reports of int32 counts => |sum| < 2^49; no int64 wrap.
    x_+=delta.x; y_+=delta.y;
    const auto transformed=processor_.process(delta);
    if (transformed.overflow_bypass) { finish(Outcome::read_error,time); return; }
    cursor_x_+=transformed.delta.x; cursor_y_+=transformed.delta.y;
    if (time-arm_us_>=settings_.timeout_us) { finish(Outcome::timeout,time); return; }
    if (buttons&0x1u) { // RI_MOUSE_LEFT_BUTTON_DOWN; same-report movement precedes click
        const auto target=trials_[next_].target.counts;
        const double dx=(static_cast<double>(cursor_x_)-target.x)*settings_.pixels_per_count;
        const double dy=(static_cast<double>(cursor_y_)-target.y)*settings_.pixels_per_count;
        finish(dx*dx+dy*dy<=14*14?Outcome::hit:Outcome::miss,time);
    }
}
void Session::tick(std::uint64_t time) noexcept {
    if (!armed_) return;
    if (time<last_us_) finish(Outcome::clock_error,last_us_);
    else if (time-arm_us_>=settings_.timeout_us) finish(Outcome::timeout,time);
}
void Session::abort(Outcome why,std::uint64_t time) noexcept { finish(why,time); }
void Session::flush() {
    if (!pending_) return;
    trials_[next_].packets.assign(buffer_.begin(),buffer_.end());
    buffer_.clear(); pending_=false; ++next_;
}

Segment first_flick(const RecordedTrial& trial) noexcept {
    Segment result;
    if (trial.outcome!=Outcome::hit && trial.outcome!=Outcome::miss) {
        result.reason=outcome_name(trial.outcome); return result;
    }
    const auto target=trial.target.counts;
    if (target.x==0 || target.y==0) { result.reason="cardinal_target_diagnostic_only"; return result; }
    if (!trial.target_painted) { result.reason="no_target_paint_marker"; return result; }
    bool began=false;
    std::int64_t x=0,y=0;
    double peak_progress=0;
    std::size_t peak_end=0;
    std::int64_t endpoint_x=0,endpoint_y=0;
    const double length2=static_cast<double>(target.x)*target.x+static_cast<double>(target.y)*target.y;
    const double hysteresis=flick::motion_counts/std::sqrt(length2);
    std::uint64_t last_motion=0;
    std::uint64_t previous=0;
    for (std::size_t i=0;i<trial.packets.size();++i) {
        const auto& sample=trial.packets[i];
        if (sample.time_us<previous) { result.reason="nonmonotonic_trace"; return result; }
        previous=sample.time_us;
        if (sample.flags&0x5u) { result.reason="unsupported_packet"; return result; }
        const auto d=sample.delta;
        if (!d.x && !d.y) continue;
        if (sample.time_us<trial.first_paint_us) { result.reason="movement_before_first_paint"; return result; }
        if (began && last_motion && sample.time_us-last_motion>=60000) {
            result.boundary="pause_60ms"; break;
        }
        x+=d.x; y+=d.y;
        const double progress=(static_cast<double>(x)*target.x+static_cast<double>(y)*target.y)/length2;
        if (!began && progress<=-hysteresis) {
            result.reason="wrong_direction_start"; return result;
        }
        if (progress>=hysteresis) began=true;
        if (began && peak_progress-progress>=hysteresis) {
            // Roll back the complete correction, including its small leading
            // packets. Never delete/reorder individual jitter packets.
            result.boundary="cumulative_reversal_4counts"; break;
        }
        if (progress>peak_progress) {
            peak_progress=progress; peak_end=i+1; endpoint_x=x; endpoint_y=y;
        }
        result.boundary="click_end"; last_motion=sample.time_us;
    }
    if (!began) { result.reason="no_outbound_segment"; return result; }
    result.end=peak_end; result.x=endpoint_x; result.y=endpoint_y;
    if (peak_progress<flick::minimum_progress) { result.reason="fragment_below_25pct_progress"; return result; }
    for (std::size_t i=0;i<result.end;++i)
        if (trial.packets[i].delta.x && trial.packets[i].delta.y) ++result.mixed_packets;
    if (result.mixed_packets==0) { result.reason="cardinal_packets_only"; return result; }
    double wx=0,wy=0;
    for (std::size_t i=result.begin;i<result.end;++i) {
        const auto d=trial.packets[i].delta;
        if (!d.x || !d.y) continue;
        const double ax=std::abs(static_cast<double>(d.x)),ay=std::abs(static_cast<double>(d.y));
        const double r=std::min(ax,ay)/std::max(ax,ay),w=r*r*(3-2*r);
        wx+=d.x*w; wy+=d.y*w;
    }
    if (wx*wx+wy*wy<1) { result.reason="insufficient_diagonal_exposure"; return result; }
    result.eligible=true; result.reason="heuristic_candidate_not_human_reviewed"; return result;
}
MovementPhases movement_phases(const RecordedTrial& trial) noexcept {
    MovementPhases out;
    if (trial.outcome!=Outcome::hit && trial.outcome!=Outcome::miss) {
        out.reason="incomplete_trial"; return out;
    }
    const auto target=trial.target.counts;
    const double length=std::hypot(static_cast<double>(target.x),static_cast<double>(target.y));
    if (length==0 || !trial.target_painted) { out.reason="missing_target_or_paint"; return out; }
    std::int64_t x=0,y=0,low_x=0,low_y=0;
    double low=0,peak=0;
    std::size_t low_end=0;
    std::uint64_t previous=0,last_motion=0;
    for (std::size_t i=0;i<trial.packets.size();++i) {
        const auto& s=trial.packets[i];
        if (s.time_us<previous || (s.flags&0x5u)) { out.identified=false; out.reason="invalid_trace"; return out; }
        previous=s.time_us;
        if (!s.delta.x && !s.delta.y) continue;
        if (s.time_us<trial.first_paint_us) { out.identified=false; out.reason="movement_before_paint"; return out; }
        if (out.identified && s.time_us-last_motion>=60000) { out.boundary="pause_60ms"; break; }
        x+=s.delta.x; y+=s.delta.y;
        const double progress=(static_cast<double>(x)*target.x+static_cast<double>(y)*target.y)/length;
        if (!out.identified) {
            if (progress<=low) { low=progress; low_end=i+1; low_x=x; low_y=y; }
            if (progress-low<flick::motion_counts) continue;
            out.identified=true; out.begin=low_end; out.start_x=low_x; out.start_y=low_y;
            out.reason="heuristic_phase_requires_review"; peak=progress;
        } else if (peak-progress>=flick::motion_counts) { out.boundary="reversal_4counts"; break; }
        // Keep the maximum-progress endpoint, excluding the whole retreat.
        if (progress>=peak) { peak=progress; out.end=i+1; out.end_x=x; out.end_y=y; }
        out.boundary="click_end"; last_motion=s.time_us;
    }
    return out;
}
Exposure exposure(const RecordedTrial& trial) noexcept {
    Exposure result;
    for (const auto& sample:trial.packets) {
        if ((sample.flags&0x5u) || (!sample.delta.x && !sample.delta.y)) continue;
        ++result.movement_packets;
        const double x=sample.delta.x,y=sample.delta.y;
        const double length=std::hypot(x,y); result.path_counts+=length;
        if (x && y) {
            ++result.mixed_packets;
            const double ax=std::abs(x),ay=std::abs(y),r=std::min(ax,ay)/std::max(ax,ay);
            result.weighted_path_counts+=length*r*r*(3-2*r);
        }
    }
    return result;
}
namespace {
bool write_plot(const std::filesystem::path& path,const RecordedTrial& trial,const Segment& segment,
                const MovementPhases* phases=nullptr,const Profile* displayed_profile=nullptr) {
    std::int64_t x=0,y=0;
    Processor plot_processor; if (displayed_profile) plot_processor.apply(*displayed_profile);
    double extent=std::max(std::abs(static_cast<double>(trial.target.counts.x)),std::abs(static_cast<double>(trial.target.counts.y)));
    for (const auto& sample:trial.packets) if (!(sample.flags&0x5u)) {
        const auto delta=plot_processor.process(sample.delta).delta;
        x+=delta.x; y+=delta.y;
        extent=std::max({extent,std::abs(static_cast<double>(x)),std::abs(static_cast<double>(y))});
    }
    const double scale=150/std::max(1.,extent);
    std::ofstream out(path,std::ios::binary); out.imbue(std::locale::classic()); out<<std::setprecision(9);
    out<<"<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"440\" height=\"470\" viewBox=\"0 0 440 470\">\n"
       <<"<rect width=\"440\" height=\"470\" fill=\"white\"/><g font-family=\"sans-serif\" font-size=\"12\" fill=\"#24354a\">"
       <<"<text x=\"15\" y=\"22\">"<<direction_name(trial.target.direction)<<" / "<<outcome_name(trial.outcome)<<" / "<<segment.reason<<"</text>"
       <<"<path d=\"M 40 220 H 400 M 220 40 V 400\" stroke=\"#d6dee8\"/>"
       <<"<circle cx=\""<<220+trial.target.counts.x*scale<<"\" cy=\""<<220+trial.target.counts.y*scale
       <<"\" r=\"6\" fill=\"#ed953e\"/>";
    auto trace=[&](std::size_t begin,std::size_t end,const char* color) {
        std::int64_t px=0,py=0;
        Processor processor; if (displayed_profile) processor.apply(*displayed_profile);
        // Prefix coordinates matter if a future segmentation version begins later.
        for (std::size_t i=0;i<begin;++i) if (!(trial.packets[i].flags&0x5u)) {
            const auto d=processor.process(trial.packets[i].delta).delta; px+=d.x; py+=d.y;
        }
        out<<"<polyline fill=\"none\" stroke=\""<<color<<"\" stroke-width=\"2\" points=\""<<220+px*scale<<','<<220+py*scale;
        for (std::size_t i=begin;i<end;++i) if (!(trial.packets[i].flags&0x5u)) {
            const auto d=processor.process(trial.packets[i].delta).delta; px+=d.x; py+=d.y;
            out<<' '<<220+px*scale<<','<<220+py*scale;
        }
        out<<"\"/>";
    };
    trace(0,trial.packets.size(),"#8c98a5");
    if (phases && phases->identified) {
        trace(0,phases->begin,"#9a641b");
        trace(phases->begin,phases->end,"#16835d");
        trace(phases->end,trial.packets.size(),"#9955bb");
    } else if (segment.eligible) trace(segment.begin,segment.end,"#196edc");
    out<<"<circle cx=\""<<220+x*scale<<"\" cy=\""<<220+y*scale<<"\" r=\"4\" fill=\"#202c3d\"/>"
       <<"<text x=\"15\" y=\"425\">"<<(displayed_profile?"Gray: displayed crosshair, including the active profile.":phases?"Brown: pre-onset. Green: main. Purple: post-main.":
            "Gray: complete relative trace. Blue: candidate segment.")<<"</text>"
       <<"<text x=\"15\" y=\"445\">Orange: target. Black: endpoint. Plot fitted to trace.</text>"
       <<"<text x=\"15\" y=\"463\">Candidate boundary: "<<segment.boundary<<"; requires human review.</text></g></svg>\n";
    out.close(); return static_cast<bool>(out);
}
std::string json_string(const std::string& input) {
    std::ostringstream out; out<<'"';
    for (const unsigned char c:input) {
        switch(c) {
            case '"':out<<"\\\"";break;
            case '\\':out<<"\\\\";break;
            case '\n':out<<"\\n";break;
            case '\r':out<<"\\r";break;
            case '\t':out<<"\\t";break;
            default:
                if (c<32) out<<"\\u00"<<std::hex<<std::setw(2)<<std::setfill('0')<<static_cast<unsigned>(c)<<std::dec;
                else out<<static_cast<char>(c);
        }
    }
    out<<'"'; return out.str();
}
}
bool export_review(const std::vector<RecordedTrial>& trials,const std::filesystem::path& directory,std::string& error) {
    std::error_code ec;
    if (!std::filesystem::create_directories(directory,ec) || ec) { error="Review directory must be new"; return false; }
    if (!std::filesystem::create_directory(directory/"plots",ec) || ec) { error="Cannot create plots directory"; return false; }
    if (!export_timing(trials,directory/"timing.csv")) { error="Timing export failed"; return false; }
    std::ofstream rows(directory/"review.csv"),candidates(directory/"candidate-trials.csv"),
        phases_file(directory/"phases.csv"),labels(directory/"packet-phases.csv"),html(directory/"index.html");
    phases_file<<"trial,identified,reason,boundary,begin,end,start_x,start_y,end_x,end_y,remaining_target_x,remaining_target_y\n";
    labels<<"trial,sample,phase\n";
    html<<"<!doctype html><meta charset=utf-8><title>Movement review</title>"
        <<"<style>body{font:17px system-ui;max-width:1100px;margin:30px auto;padding:15px}"
        <<"section{display:inline-block;vertical-align:top;width:48%;margin:1%}[hidden]{display:none!important}img{width:100%}"
        <<"p{line-height:1.5}select{font:inherit}</style><h1>All recorded trials</h1>"
        <<"<p>Every trial is retained. Brown: movement before detected onset; green: main movement candidate; "
        <<"purple: later movement, possibly correction. These are heuristic annotations, not known human intent. "
        <<"A main movement annotation does not make a trial eligible for fitting. Coordinates retain the original start offset.</p>"
        <<"<label>Show <select onchange=\"document.querySelectorAll('section').forEach(s=>s.hidden=this.value!='all'&amp;&amp;s.dataset.kind!=this.value)\">"
        <<"<option value=all>All trials</option><option value=candidate>Fitting candidates</option>"
        <<"<option value=flagged>Flagged diagonal starts</option><option value=cardinal>Cardinal diagnostics</option></select></label>";
    rows<<"trial,split,eligible,reason,boundary,begin,end,x,y\n";
    candidates<<"trial,split,target_x,target_y,dx,dy\n";
    std::size_t eligible=0;
    for (std::size_t i=0;i<trials.size();++i) {
        const auto& t=trials[i]; const auto s=first_flick(t);
        const auto p=movement_phases(t);
        const auto id="trial_"+std::to_string(i+1);
        const auto split=t.target.validation?"validation":"train";
        rows<<id<<','<<split<<','<<s.eligible<<','<<s.reason<<','<<s.boundary<<','<<s.begin<<','<<s.end<<','<<s.x<<','<<s.y<<'\n';
        phases_file<<id<<','<<p.identified<<','<<p.reason<<','<<p.boundary<<','<<p.begin<<','<<p.end<<','
            <<p.start_x<<','<<p.start_y<<','<<p.end_x<<','<<p.end_y<<','
            <<static_cast<std::int64_t>(t.target.counts.x)-p.start_x<<','
            <<static_cast<std::int64_t>(t.target.counts.y)-p.start_y<<'\n';
        for (std::size_t j=0;j<t.packets.size();++j)
            labels<<id<<','<<j<<','<<(!p.identified?"unclassified":j<p.begin?"pre_onset_candidate":j<p.end?"main_candidate":"post_main_possible_correction")<<'\n';
        const char* kind=s.eligible?"candidate":(t.target.counts.x==0 || t.target.counts.y==0)?"cardinal":"flagged";
        html<<"<section data-kind="<<kind<<"><h2>"<<id<<" / "<<direction_name(t.target.direction)<<" / "<<outcome_name(t.outcome)<<"</h2><p>"<<s.reason
            <<"<br>Main start offset: ("<<p.start_x<<", "<<p.start_y<<") counts</p>";
        if (t.outcome!=Outcome::not_attempted) html<<"<a href=plots/"<<id<<".svg><img alt=\"Full movement trace\" loading=lazy src=plots/"<<id<<".svg></a>";
        html<<"</section>";
        if (t.outcome!=Outcome::not_attempted && !write_plot(directory/"plots"/(id+".svg"),t,s,&p)) {
            error="Review plot write failed"; return false;
        }
        if (!s.eligible) continue;
        ++eligible;
        for (std::size_t j=s.begin;j<s.end;++j) {
            const auto d=t.packets[j].delta;
            candidates<<id<<','<<split<<','<<t.target.counts.x<<','<<t.target.counts.y<<','<<d.x<<','<<d.y<<'\n';
        }
    }
    rows.close(); candidates.close(); phases_file.close(); labels.close(); html.close();
    if (!rows || !candidates || !phases_file || !labels || !html) { error="Review write failed"; return false; }
    std::ofstream manifest(directory/"review.json");
    manifest<<"{\"segmentation\":"<<json_string(flick::version)
        <<",\"phase_annotation\":\"projected_low_water_4counts_v1\",\"timing_annotation\":\"raw_speed_10ms_smooth3_peak20pct_run3_v1\",\"human_reviewed\":false,\"trials\":"<<trials.size()<<",\"eligible\":"<<eligible<<"}\n";
    manifest.close();
    if (!manifest) { error="Review manifest write failed"; return false; }
    error.clear(); return true;
}
bool export_session(const Session& session,const std::filesystem::path& directory,std::string& error) {
    if (!session.prepared() || session.armed() || session.pending()) { error="Stop/flush the capture before exporting"; return false; }
    std::error_code ec;
    if (!directory.parent_path().empty()) std::filesystem::create_directories(directory.parent_path(),ec);
    if (ec || !std::filesystem::create_directory(directory,ec)) { error="Export directory must be new: "+ec.message(); return false; }
    if (!std::filesystem::create_directory(directory/"plots",ec)) { error="Cannot create plot directory"; return false; }
    if (!export_timing(session.trials(),directory/"timing.csv")) { error="Timing export failed"; return false; }
    auto open=[&](const char* name) {
        std::ofstream output(directory/name,std::ios::binary);
        output.imbue(std::locale::classic()); output<<std::setprecision(17); return output;
    };
    auto packets=open("packets.csv"),trials=open("trials.csv"),candidates=open("candidate-trials.csv");
    packets<<"trial,sample,time_us,dx,dy,buttons,flags\n";
    trials<<"trial,split,direction,distance,rep,target_x,target_y,target_px_x,target_px_y,outcome,armed_us,duration_us,target_painted,first_paint_us,endpoint_x,endpoint_y,packet_count,movement_packets,mixed_packets,path_counts,weighted_path_counts,segment_mixed_packets,segment_begin,segment_end,segment_x,segment_y,eligible,reason,boundary,condition,cursor_endpoint_x,cursor_endpoint_y,angle_index,angle_degrees\n";
    candidates<<"trial,split,target_x,target_y,dx,dy\n";
    std::size_t attempted=0,eligible=0;
    for (std::size_t i=0;i<session.trials().size();++i) {
        const auto& t=session.trials()[i]; auto segment=first_flick(t);
        if (session.settings().comparison) { segment.eligible=false; segment.reason="comparison_not_calibration"; }
        const auto observed=exposure(t);
        const auto id="trial_"+std::to_string(i+1);
        const char* split=t.target.validation?"validation":"train";
        if (t.outcome!=Outcome::not_attempted) ++attempted;
        if (segment.eligible) ++eligible;
        const auto active_profile=t.target.candidate_condition?session.settings().candidate_profile:Profile{};
        if (t.outcome!=Outcome::not_attempted && !write_plot(directory/"plots"/(id+".svg"),t,segment,nullptr,
                session.settings().comparison?&active_profile:nullptr)) {
            error="Plot export failed; directory is incomplete"; return false;
        }
        trials<<id<<','<<split<<','<<direction_name(t.target.direction)<<','<<t.target.distance<<','<<t.target.repetition<<','
            <<t.target.counts.x<<','<<t.target.counts.y<<','<<t.target.counts.x*session.settings().pixels_per_count<<','
            <<t.target.counts.y*session.settings().pixels_per_count<<','<<outcome_name(t.outcome)<<','<<t.armed_us<<','<<t.ended_us<<','
            <<t.target_painted<<','<<t.first_paint_us<<','<<t.endpoint_x<<','<<t.endpoint_y<<','<<t.packets.size()<<','
            <<observed.movement_packets<<','<<observed.mixed_packets<<','<<observed.path_counts<<','<<observed.weighted_path_counts<<','
            <<segment.mixed_packets<<','<<segment.begin<<','<<segment.end<<','
            <<segment.x<<','<<segment.y<<','<<segment.eligible<<','<<segment.reason<<','<<segment.boundary<<','
            <<(t.target.candidate_condition?"candidate":"baseline")<<','<<t.cursor_endpoint_x<<','<<t.cursor_endpoint_y<<','
            <<t.target.angle_index<<','<<360.*t.target.angle_index/session.settings().direction_count<<'\n';
        for (std::size_t j=0;j<t.packets.size();++j) {
            const auto& s=t.packets[j];
            packets<<id<<','<<j<<','<<s.time_us<<','<<s.delta.x<<','<<s.delta.y<<','<<s.buttons<<','<<s.flags<<'\n';
            if (segment.eligible && j>=segment.begin && j<segment.end)
                candidates<<id<<','<<split<<','<<t.target.counts.x<<','<<t.target.counts.y<<','<<s.delta.x<<','<<s.delta.y<<'\n';
        }
    }
    packets.close(); trials.close(); candidates.close();
    if (!packets || !trials || !candidates) { error="Export write failed; directory contains incomplete data"; return false; }
    // Written last: absence of session.json means the other files are incomplete.
    auto manifest=open("session.json");
    const auto& s=session.settings();
    manifest<<"{\n  \"schema_version\": 2,\n  \"mode\": "<<json_string(s.comparison?"randomized_profile_comparison":"identity_only")
        <<",\n  \"synthetic\": "<<(s.synthetic?"true":"false")
        <<",\n  \"dpi\": "<<s.dpi<<",\n  \"reference_sensitivity\": "<<s.reference_sensitivity
        <<",\n  \"pixels_per_count\": "<<s.pixels_per_count<<",\n  \"seed\": "<<s.seed
        <<",\n  \"repetitions_per_cell\": "<<s.repetitions<<",\n  \"timeout_us\": "<<s.timeout_us
        <<",\n  \"direction_count\": "<<s.direction_count
        <<",\n  \"packet_capacity_per_trial\": "<<s.packet_capacity<<",\n  \"target_radius_px\": 14,\n"
        <<"  \"canvas_px\": ["<<s.canvas_width<<','<<s.canvas_height<<"],\n  \"origin_px\": ["<<s.origin_px_x<<','<<s.origin_px_y<<"],\n"
        <<"  \"timestamp_source\": "<<json_string(s.synthetic?"synthetic_monotonic_microseconds":
            "QPC_at_WM_INPUT_dispatch_relative_to_arm_microseconds")<<",\n"
        <<"  \"baseline_mapping\": \"virtual_xy_counts_times_pixels_per_count_no_OS_pointer_mapping\",\n"
        <<"  \"segmentation\": "<<json_string(flick::version)<<",\n"
        <<"  \"segments_human_reviewed\": false,\n  \"device_path\": "<<json_string(s.device_path)
        <<",\n  \"scheduled_trials\": "<<session.trials().size()<<",\n  \"attempted_trials\": "<<attempted
        <<",\n  \"heuristic_eligible_trials\": "<<eligible<<",\n  \"session_complete\": "<<(session.complete()?"true":"false")
        <<",\n  \"candidate_gains_q16\": [";
    for (unsigned q=0;q<4;++q) {
        const auto gain=s.candidate_profile.gain(static_cast<Quadrant>(q));
        if (q) manifest<<',';
        manifest<<'['<<gain.x<<','<<gain.y<<']';
    }
    manifest<<"]\n}\n";
    manifest.close();
    if (!manifest) { error="Manifest write failed; export is incomplete"; return false; }
    error.clear(); return true;
}
} // namespace directional::capture
