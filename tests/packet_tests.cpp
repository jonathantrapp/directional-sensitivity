#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <ntddmou.h>
#include "directional/packet_filter.h"
#include "directional/filter_state.h"
#include <array>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>

using namespace directional;
namespace {
void check(bool ok,const char* text) {
    if (!ok) { std::cerr << "FAIL: " << text << '\n'; std::exit(1); }
}
MOUSE_INPUT_DATA packet(LONG x,LONG y) {
    MOUSE_INPUT_DATA p{};
    p.UnitId=17; p.ButtonFlags=MOUSE_LEFT_BUTTON_DOWN|MOUSE_WHEEL;
    p.ButtonData=120; p.RawButtons=0x13579; p.ExtraInformation=0xabcd1234;
    p.LastX=x; p.LastY=y;
    return p;
}
Profile profile() {
    std::array<GainPair,4> gains{};
    for (auto& g:gains) g={81920,81920};
    Profile p; check(Profile::create(gains,p),"profile validation"); return p;
}
}
int main() {
    FilterState control;
    auto snapshot=control.snapshot();
    check(snapshot.enabled==0 && snapshot.connected==0,"driver starts disabled/unavailable");
    driver::SetProfile command{};
    command.abi_version=driver::version; command.size=sizeof(command); command.enabled=1;
    command.expected_generation=snapshot.generation;
    for (auto& g:command.gains) g={81920,81920};
    check(control.apply(command)==ApplyStatus::not_ready,"cannot enable before connection/power");
    control.set_connected(true); control.set_powered(true);
    command.expected_generation=control.snapshot().generation;
    check(control.apply(command)==ApplyStatus::ok,"valid command commits");
    check(control.apply(command)==ApplyStatus::stale,"stale writer rejected");
    check(control.processor.process({1,-1}).delta.x==1,"control applies real transform");
    auto invalid=command; invalid.expected_generation=control.snapshot().generation; invalid.gains[0].x=0;
    check(control.apply(invalid)==ApplyStatus::invalid,"malformed gains rejected");
    check(control.processor.process({1,-1}).delta.x==2,"rejected config does not reset fractions");
    control.set_powered(false);
    check(control.processor.process({100,-100}).delta.x==100 && control.snapshot().enabled==0,"suspend bypass");
    control.set_powered(true);
    check(control.snapshot().enabled==0,"resume requires explicit apply");
    command.expected_generation=control.snapshot().generation;
    check(control.apply(command)==ApplyStatus::ok,"reapply after resume");
    check(control.processor.process({1,-1}).delta.x==1,"resume clears residuals");
    control.set_connected(false);
    check(control.snapshot().enabled==0 && control.snapshot().connected==0,"disconnect bypass");
    std::array<MOUSE_INPUT_DATA,5> input{{packet(1,-1),packet(1,-1),packet(9,0),packet(9,-9),packet(1,-1)}};
    input[3].Flags=MOUSE_MOVE_ABSOLUTE|MOUSE_VIRTUAL_DESKTOP;
    const auto original=input;
    Processor all; all.apply(profile());
    std::vector<MOUSE_INPUT_DATA> expected;
    const auto full=forward_packets(input.data(),input.size(),all,[&](const auto& p) noexcept {
        expected.push_back(p); return true;
    });
    check(full.consumed==5,"full batch consumed");
    check(std::memcmp(input.data(),original.data(),sizeof(input))==0,"original packet storage untouched");
    check(expected[0].LastX==1 && expected[1].LastX==2,"fractional diagonal counts");
    check(expected[2].LastX==9 && expected[2].LastY==0,"cardinal exactly preserved");
    check(std::memcmp(&expected[3],&input[3],sizeof(MOUSE_INPUT_DATA))==0,"absolute metadata and counts preserved");
    check(expected[4].LastX==1,"absolute report resets relative fractions");
    for (std::size_t i=0;i<input.size();++i) {
        auto without_movement=expected[i];
        without_movement.LastX=input[i].LastX; without_movement.LastY=input[i].LastY;
        check(std::memcmp(&without_movement,&input[i],sizeof(MOUSE_INPUT_DATA))==0,"all nonmovement fields preserved");
    }
    Processor partial; partial.apply(profile());
    std::vector<MOUSE_INPUT_DATA> actual;
    auto first=forward_packets(input.data(),input.size(),partial,[&](const auto& p) noexcept {
        if (actual.size()==1) return false;
        actual.push_back(p); return true;
    });
    check(first.consumed==1,"backpressure returns exact consumed prefix");
    auto remaining=forward_packets(input.data()+first.consumed,input.size()-first.consumed,partial,
        [&](const auto& p) noexcept { actual.push_back(p); return true; });
    check(remaining.consumed==4 && actual.size()==expected.size(),"suffix retry consumed");
    check(std::memcmp(actual.data(),expected.data(),expected.size()*sizeof(MOUSE_INPUT_DATA))==0,
          "retry does not double-transform or double-advance residuals");
    Processor rejected; rejected.apply(profile());
    auto none=forward_packets(input.data(),input.size(),rejected,[](const auto&) noexcept { return false; });
    check(none.consumed==0,"zero consumed");
    check(rejected.process({1,-1}).delta.x==1,"zero consumption leaves state untouched");
    std::cout << "PASS: Windows packet metadata, absolute bypass and backpressure\n";
}
