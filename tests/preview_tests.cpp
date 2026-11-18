#include "../windows/preview_input.h"
#include <array>
#include <cstdlib>
#include <iostream>

namespace {
int checks=0;
void check(bool ok,const char* message) {
    ++checks;
    if (!ok) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
RAWINPUT mouse(HANDLE device,LONG x,LONG y,USHORT flags=0) {
    RAWINPUT input{};
    input.header.dwType=RIM_TYPEMOUSE; input.header.dwSize=sizeof(input); input.header.hDevice=device;
    input.data.mouse.lLastX=x; input.data.mouse.lLastY=y; input.data.mouse.usFlags=flags;
    return input;
}
directional::Profile profile() {
    directional::GainPair anchors[4];
    for (auto& g:anchors) g={81920,81920};
    directional::Profile result;
    check(directional::Profile::create(anchors,result),"profile accepted"); return result;
}
void lifecycle() {
    // Fake tokens only. No registration, hardware enumeration or windows opened.
    const auto selected=reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(1));
    const auto other=reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(2));
    preview_tool::Model model;
    check(!model.start(nullptr,profile()),"explicit nonzero device required");
    check(model.start(selected,profile()),"start preview");
    model.process(mouse(other,100,-100),true);
    check(model.reports==0,"other mouse ignored");
    model.process(mouse(selected,1,-1),true);
    check(model.out.x==1 && model.out.y==-1 && model.processor.residual().x==directional::residual_one/4,"real transform used");
    model.process(mouse(selected,10,0),true);
    check(model.out.x==10 && model.out.y==0 && model.processor.residual().x==directional::residual_one/4,"cardinal bypass/carry freeze");
    model.process(mouse(selected,1,-1),true);
    check(model.out.x==2 && model.out.y==-2 && model.raw_x==12 && model.out_x==13,"totals and fractional carry");
    model.process(mouse(selected,9,-9),false);
    check(!model.running && model.reports==3 && model.processor.residual().x==0,"focus loss stops without processing");
    model.start(selected,profile()); model.process(mouse(selected,1,-1),true);
    model.removed(other);
    check(model.running,"other removal ignored");
    model.removed(selected);
    check(!model.running && model.processor.residual().x==0,"selected removal resets/stops");
    model.start(selected,profile()); model.process(mouse(selected,10000,10000,MOUSE_MOVE_ABSOLUTE|MOUSE_VIRTUAL_DESKTOP),true);
    check(!model.running && model.reports==0,"absolute device stopped, never scaled");
    model.start(selected,profile()); model.process(mouse(selected,1,-1,MOUSE_ATTRIBUTES_CHANGED),true);
    check(!model.running && model.reports==0,"changed attributes stop for setup reconfirmation");
    check(!model.start(selected,profile()),"Start cannot reuse setup after changed attributes");
    model=preview_tool::Model{}; // a fresh app must reload/confirm profile metadata
    model.start(selected,profile()); model.process(mouse(selected,INT32_MAX,INT32_MAX),true);
    check(model.overflows==1 && model.out.x==INT32_MAX,"overflow diagnostic preserved");
    model.start(selected,profile()); model.raw_x=std::numeric_limits<std::int64_t>::max();
    model.process(mouse(selected,1,1),true);
    check(!model.running && model.reports==0,"session totals cannot wrap");
}
void buffers() {
    const auto selected=reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(1));
    const RAWINPUT first=mouse(selected,7,-9);
    // Deliberately unaligned input tests byte-safe parsing; the live storage is aligned.
    std::array<unsigned char,2*sizeof(RAWINPUT)+16> bytes{};
    std::memcpy(bytes.data()+1,&first,sizeof(first));
    std::memcpy(bytes.data()+1+sizeof(first),&first,sizeof(first));
    unsigned seen=0;
    const auto consume=[&](const RAWINPUT& raw) noexcept { ++seen; check(raw.data.mouse.lLastY==-9,"parsed report"); };
    check(preview_tool::consume_buffer(bytes.data()+1,2*sizeof(first),2,consume) && seen==2,"two native records traversed");
    for (std::size_t size=0;size<sizeof(first);++size) {
        unsigned called=0;
        check(!preview_tool::consume_buffer(bytes.data()+1,size,1,[&](const auto&) noexcept { ++called; }) && called==0,
              "truncation rejected before mouse access");
    }
    auto header=first.header;
    header.dwSize=0; std::memcpy(bytes.data()+1,&header,sizeof(header));
    check(!preview_tool::consume_buffer(bytes.data()+1,bytes.size()-1,1,consume),"zero-sized record rejected");
    header.dwSize=UINT32_MAX; std::memcpy(bytes.data()+1,&header,sizeof(header));
    check(!preview_tool::consume_buffer(bytes.data()+1,bytes.size()-1,1,consume),"oversized record rejected");
    header.dwSize=sizeof(RAWINPUTHEADER); std::memcpy(bytes.data()+1,&header,sizeof(header));
    check(!preview_tool::consume_buffer(bytes.data()+1,bytes.size()-1,1,consume),"mouse header without payload rejected");
    header.dwType=RIM_TYPEKEYBOARD; std::memcpy(bytes.data()+1,&header,sizeof(header));
    seen=0;
    check(preview_tool::consume_buffer(bytes.data()+1,sizeof(header),1,consume) && seen==0,"nonmouse record skipped");
    check(preview_tool::consume_buffer(nullptr,0,0,consume),"empty batch");
    check(!preview_tool::consume_buffer(nullptr,0,1,consume),"null nonempty batch rejected");
    // Variable-size records must honor native padding before the next header.
    header=first.header; header.dwSize=sizeof(first)+1;
    bytes.fill(0);
    std::memcpy(bytes.data(),&first,sizeof(first)); std::memcpy(bytes.data(),&header,sizeof(header));
    constexpr std::size_t align=sizeof(void*)==8?8:4;
    constexpr auto next=(sizeof(first)+1+align-1)&~(align-1);
    std::memcpy(bytes.data()+next,&first,sizeof(first));
    seen=0;
    check(preview_tool::consume_buffer(bytes.data(),next+sizeof(first),2,consume) && seen==2,"variable record padding");
}
}
int main() { lifecycle(); buffers(); std::cout << "PASS: " << checks << " preview model/parser checks (no live input)\n"; }
