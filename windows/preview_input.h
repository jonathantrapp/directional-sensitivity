#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "directional/transform.h"
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace preview_tool {
// Parse native-architecture GetRawInputBuffer records. No pointers into the
// source buffer escape. The validated mouse copy is aligned, even in tests
// using unaligned/truncated bytes. Earlier records can be consumed before a
// later malformed record is rejected; the caller must then stop/reset preview.
template<class Consumer>
bool consume_buffer(const void* bytes,std::size_t capacity,UINT count,Consumer&& consume) noexcept {
    const auto* data=static_cast<const unsigned char*>(bytes);
    if (!data) return count==0;
    std::size_t offset=0;
    for (UINT i=0;i<count;++i) {
        if (offset>capacity || capacity-offset<sizeof(RAWINPUTHEADER)) return false;
        RAWINPUTHEADER header{};
        std::memcpy(&header,data+offset,sizeof(header));
        const auto size=static_cast<std::size_t>(header.dwSize);
        if (size<sizeof(header) || size>capacity-offset) return false;
        if (header.dwType==RIM_TYPEMOUSE) {
            if (size<offsetof(RAWINPUT,data)+sizeof(RAWMOUSE)) return false;
            RAWINPUT raw{}; raw.header=header;
            std::memcpy(&raw.data.mouse,data+offset+offsetof(RAWINPUT,data),sizeof(RAWMOUSE));
            consume(raw);
        }
        // Same native record alignment as NEXTRAWINPUTBLOCK. WOW64 buffered
        // mode is disabled in the UI; native x86/x64 have different alignments.
        constexpr std::size_t alignment=sizeof(void*)==8?8:4;
        if (i+1<count) {
            const auto padded=(size+alignment-1)&~(alignment-1);
            if (padded>capacity-offset) return false;
            offset+=padded;
        }
    }
    return true;
}

class Model {
public:
    directional::Processor processor;
    HANDLE selected=nullptr;
    bool running=false;
    bool setup_stale=false;
    const wchar_t* status=L"Select a mouse, then Start. Foreground only.";
    directional::Delta raw{},out{};
    std::int64_t raw_x=0,raw_y=0,out_x=0,out_y=0;
    std::uint64_t reports=0,overflows=0;

    bool start(HANDLE device,const directional::Profile& profile) noexcept {
        if (!device || setup_stale) return false;
        processor.apply(profile); processor.set_enabled(true);
        selected=device; raw={}; out={}; raw_x=raw_y=out_x=out_y=0;
        reports=overflows=0; running=true; status=L"Running in this window only. System input is unchanged.";
        return true;
    }
    void stop(const wchar_t* reason) noexcept { running=false; processor.reset(); status=reason; }
    void removed(HANDLE device) noexcept {
        if (device==selected) stop(L"Selected mouse removed. Refresh and select it again.");
    }
    void process(const RAWINPUT& input,bool foreground) noexcept {
        if (!running) return;
        if (!foreground) { stop(L"Paused on focus loss. Click Start to begin a fresh preview."); return; }
        if (input.header.dwType!=RIM_TYPEMOUSE || input.header.hDevice!=selected) return;
        const auto& mouse=input.data.mouse;
        if (mouse.usFlags&MOUSE_ATTRIBUTES_CHANGED) {
            setup_stale=true;
            stop(L"Mouse attributes changed. Confirm DPI/settings and restart the application."); return;
        }
        if (mouse.usFlags&MOUSE_MOVE_ABSOLUTE) {
            stop(L"Absolute input is unsupported. Select a relative mouse."); return;
        }
        const directional::Delta delta{mouse.lLastX,mouse.lLastY};
        const auto result=processor.process(delta);
        if (!fits_sum(raw_x,delta.x) || !fits_sum(raw_y,delta.y) ||
            !fits_sum(out_x,result.delta.x) || !fits_sum(out_y,result.delta.y) ||
            reports==std::numeric_limits<std::uint64_t>::max()) {
            stop(L"Session totals reached capacity. Start a fresh preview."); return;
        }
        raw=delta; out=result.delta;
        raw_x+=raw.x; raw_y+=raw.y; out_x+=out.x; out_y+=out.y;
        ++reports; overflows+=result.overflow_bypass;
    }
private:
    static bool fits_sum(std::int64_t sum,directional::int32 delta) noexcept {
        return delta>=0 ? sum<=std::numeric_limits<std::int64_t>::max()-delta :
                          sum>=std::numeric_limits<std::int64_t>::min()-delta;
    }
};
} // namespace preview_tool
