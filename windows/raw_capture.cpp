#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "directional/transform.h"
#include <atomic>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {
struct Device { HANDLE handle; std::wstring path; };
std::atomic_bool stop_requested{false};
BOOL WINAPI console_signal(DWORD signal) {
    if (signal==CTRL_C_EVENT || signal==CTRL_BREAK_EVENT) { stop_requested=true; return TRUE; }
    return FALSE;
}
bool devices(std::vector<Device>& output) {
    UINT count=0;
    if (GetRawInputDeviceList(nullptr,&count,sizeof(RAWINPUTDEVICELIST))!=0) return false;
    std::vector<RAWINPUTDEVICELIST> list(count);
    if (count && GetRawInputDeviceList(list.data(),&count,sizeof(RAWINPUTDEVICELIST))==UINT(-1)) return false;
    for (UINT i=0;i<count;++i) if (list[i].dwType==RIM_TYPEMOUSE) {
        UINT characters=0;
        if (GetRawInputDeviceInfoW(list[i].hDevice,RIDI_DEVICENAME,nullptr,&characters)==UINT(-1)) return false;
        std::wstring path(characters+1,L'\0');
        if (GetRawInputDeviceInfoW(list[i].hDevice,RIDI_DEVICENAME,path.data(),&characters)==UINT(-1)) return false;
        path.resize(wcslen(path.c_str()));
        output.push_back({list[i].hDevice,std::move(path)});
    }
    return true;
}
struct Capture {
    HANDLE selected=nullptr;
    std::vector<directional::Delta> packets;
    std::size_t limit=0;
    DWORD duration_ms=0;
    ULONGLONG start=0;
    bool failed=false;
    const char* failure="";
    void fail(const char* message) { failed=true; failure=message; PostQuitMessage(3); }
};
LRESULT CALLBACK procedure(HWND window,UINT message,WPARAM wp,LPARAM lp) {
    auto* state=reinterpret_cast<Capture*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    if (message==WM_NCCREATE) {
        state=static_cast<Capture*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(state));
    }
    if (state) {
        if (message==WM_TIMER && (stop_requested || GetTickCount64()-state->start>=state->duration_ms)) {
            PostQuitMessage(0); return 0;
        }
        if (message==WM_INPUT_DEVICE_CHANGE && wp==GIDC_REMOVAL && reinterpret_cast<HANDLE>(lp)==state->selected) {
            state->fail("Selected mouse disconnected; capture discarded"); return 0;
        }
        if (message==WM_INPUT) {
            // Fixed storage: keyboard/HID data is not registered. A mouse report
            // fits RAWINPUT, and unexpected sizes are rejected without allocation.
            RAWINPUT raw{};
            UINT bytes=sizeof(raw);
            const UINT result=GetRawInputData(reinterpret_cast<HRAWINPUT>(lp),RID_INPUT,&raw,&bytes,sizeof(RAWINPUTHEADER));
            if (result==UINT(-1) || result<sizeof(RAWINPUTHEADER)) state->fail("Raw input read failed");
            else if (raw.header.dwType==RIM_TYPEMOUSE && raw.header.hDevice==state->selected) {
                if (result<sizeof(RAWINPUT)) state->fail("Truncated mouse report");
                else if (raw.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) state->fail("Absolute device cannot supply relative replay data");
                else if (raw.data.mouse.usFlags & MOUSE_ATTRIBUTES_CHANGED) state->fail("Mouse attributes changed; restart capture with confirmed setup");
                else if (state->packets.size()==state->limit) state->fail("Capture capacity exceeded; capture discarded");
                else state->packets.push_back({raw.data.mouse.lLastX,raw.data.mouse.lLastY});
            }
            // Windows requires foreground WM_INPUT cleanup through DefWindowProc.
            return DefWindowProcW(window,message,wp,lp);
        }
    }
    return DefWindowProcW(window,message,wp,lp);
}
bool unsigned_number(const wchar_t* text,unsigned& result) {
    if (!*text) return false;
    unsigned value=0;
    for (const wchar_t* p=text;*p;++p) {
        if (*p<L'0' || *p>L'9' || value>100000) return false;
        value=value*10+static_cast<unsigned>(*p-L'0');
    }
    result=value; return true;
}
}
int wmain(int argc,wchar_t** argv) {
    std::vector<Device> mice;
    if (!devices(mice)) { std::cerr << "Mouse enumeration failed: " << GetLastError() << '\n'; return 2; }
    if (argc==2 && std::wstring(argv[1])==L"--list") {
        for (std::size_t i=0;i<mice.size();++i) std::wcout << i << L": " << mice[i].path << L'\n';
        return 0;
    }
    unsigned index=0,seconds=0;
    if (argc!=5 || std::wstring(argv[1])!=L"--device-index" || !unsigned_number(argv[2],index) ||
        std::wstring(argv[3])!=L"--seconds" || !unsigned_number(argv[4],seconds) ||
        seconds<1 || seconds>120 || index>=mice.size()) {
        std::cerr << "Usage: raw_capture --list\n       raw_capture --device-index N --seconds 1..120 > motion.csv\n"
                     "Indexes are session-local: list devices immediately before capture.\n";
        return 2;
    }
    Capture state;
    state.selected=mice[index].handle;
    state.duration_ms=seconds*1000;
    // Preallocate for 32k reports/s plus a burst margin; no disk writes/heap
    // allocation in the report handler. Output is flushed only after capture.
    state.limit=static_cast<std::size_t>(seconds)*32000+1024;
    state.packets.reserve(state.limit);
    WNDCLASSW wc{}; wc.lpfnWndProc=procedure; wc.hInstance=GetModuleHandleW(nullptr);
    wc.lpszClassName=L"DirectionalRawCapture";
    if (!RegisterClassW(&wc)) { std::cerr << "Window registration failed\n"; return 2; }
    HWND window=CreateWindowExW(0,wc.lpszClassName,L"",0,0,0,0,0,HWND_MESSAGE,nullptr,wc.hInstance,&state);
    if (!window) { std::cerr << "Capture window creation failed\n"; return 2; }
    RAWINPUTDEVICE rid{1,2,RIDEV_INPUTSINK|RIDEV_DEVNOTIFY,window};
    if (!RegisterRawInputDevices(&rid,1,sizeof(rid))) {
        std::cerr << "Raw input registration failed\n"; DestroyWindow(window); return 2;
    }
    state.start=GetTickCount64();
    if (!SetTimer(window,1,50,nullptr)) { DestroyWindow(window); std::cerr << "Timer creation failed\n"; return 2; }
    SetConsoleCtrlHandler(console_signal,TRUE);
    std::wcerr << L"Recording only " << mice[index].path << L" for " << seconds << L"s. Ctrl+C ends early.\n";
    MSG message{};
    int status;
    while ((status=static_cast<int>(GetMessageW(&message,nullptr,0,0)))>0) {
        TranslateMessage(&message); DispatchMessageW(&message);
    }
    KillTimer(window,1);
    rid.dwFlags=RIDEV_REMOVE; rid.hwndTarget=nullptr;
    RegisterRawInputDevices(&rid,1,sizeof(rid));
    DestroyWindow(window);
    SetConsoleCtrlHandler(console_signal,FALSE);
    if (state.failed || status<0) {
        std::cerr << (state.failed ? state.failure:"Message loop failed") << '\n'; return 3;
    }
    std::cout << "# raw_capture relative counts; no system input was replaced\n";
    for (auto d:state.packets) std::cout << d.x << ',' << d.y << '\n';
    std::cerr << "Captured " << state.packets.size() << " reports.\n";
    if (!std::cout) { std::cerr << "Capture output failed\n"; return 2; }
}
