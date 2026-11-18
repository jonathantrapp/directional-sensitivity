#include "preview_input.h"
#include "directional/profile_io.h"
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <locale>
#include <sstream>
#include <string>
#include <vector>

namespace {
struct Device { HANDLE handle; std::wstring path; };
bool enumerate(std::vector<Device>& output) {
    UINT count=0;
    if (GetRawInputDeviceList(nullptr,&count,sizeof(RAWINPUTDEVICELIST))!=0) return false;
    std::vector<RAWINPUTDEVICELIST> list(count);
    if (count && GetRawInputDeviceList(list.data(),&count,sizeof(RAWINPUTDEVICELIST))==UINT(-1)) return false;
    std::vector<Device> result;
    for (UINT i=0;i<count;++i) if (list[i].dwType==RIM_TYPEMOUSE) {
        UINT characters=0;
        if (GetRawInputDeviceInfoW(list[i].hDevice,RIDI_DEVICENAME,nullptr,&characters)==UINT(-1) ||
            characters==0 || characters>32768) return false;
        std::wstring path(characters+1,L'\0');
        if (GetRawInputDeviceInfoW(list[i].hDevice,RIDI_DEVICENAME,path.data(),&characters)==UINT(-1)) return false;
        path.resize(wcslen(path.c_str()));
        result.push_back({list[i].hDevice,std::move(path)});
    }
    output=std::move(result); return true;
}
enum : int { device_combo=101,start_button=102,stop_button=103,refresh_button=104 };
struct App {
    preview_tool::Model model;
    directional::SavedProfile profile;
    std::vector<Device> devices;
    HWND combo=nullptr,status=nullptr,metrics=nullptr;
    bool registered=false,buffered=false;
    // uint64 storage guarantees native RAWINPUT alignment without per-read allocation.
    std::array<std::uint64_t,8192> buffer{};

    void unregister_input() noexcept {
        if (!registered) return;
        RAWINPUTDEVICE rid{1,2,RIDEV_REMOVE,nullptr};
        if (RegisterRawInputDevices(&rid,1,sizeof(rid))) registered=false;
        else model.stop(L"Raw Input deregistration failed; close the application. Preview is stopped.");
    }
    void stop(const wchar_t* reason) noexcept { model.stop(reason); unregister_input(); }
    void refresh() {
        stop(L"Select a mouse, then Start. Foreground only.");
        if (!enumerate(devices)) { model.stop(L"Mouse enumeration failed. Try Refresh again."); return; }
        SendMessageW(combo,CB_RESETCONTENT,0,0);
        for (const auto& device:devices) SendMessageW(combo,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(device.path.c_str()));
        SendMessageW(combo,CB_SETCURSEL,static_cast<WPARAM>(-1),0);
    }
    void start(HWND window) {
        stop(L"Select a mouse before starting.");
        if (model.setup_stale) {
            model.stop(L"Setup is stale. Confirm DPI/settings and restart the application."); return;
        }
        if (registered) return; // failed deregistration requires process restart
        const auto selection=SendMessageW(combo,CB_GETCURSEL,0,0);
        if (selection==CB_ERR || static_cast<std::size_t>(selection)>=devices.size()) return;
        const auto& chosen=devices[static_cast<std::size_t>(selection)];
        std::vector<Device> current;
        if (!enumerate(current)) { model.stop(L"Cannot verify selected mouse. Refresh and retry."); return; }
        bool present=false;
        for (const auto& device:current) if (device.handle==chosen.handle && device.path==chosen.path) present=true;
        if (!present) { model.stop(L"Device selection is stale. Refresh and select it again."); return; }
        // No INPUTSINK, NOLEGACY, hooks or injection. Other applications and
        // Windows continue receiving normal input; only this app draws results.
        RAWINPUTDEVICE rid{1,2,RIDEV_DEVNOTIFY,window};
        if (!RegisterRawInputDevices(&rid,1,sizeof(rid))) { model.stop(L"Raw Input registration failed."); return; }
        registered=true; model.start(chosen.handle,profile.transform);
    }
    void process(const RAWINPUT& raw,HWND window) noexcept {
        model.process(raw,GetForegroundWindow()==window);
    }
    void drain(HWND window) noexcept {
        // Bound queue work so a sustained report stream cannot create an
        // unbounded loop. Oversized/unexpected records stop instead of reallocating.
        for (unsigned batch=0;batch<4 && model.running;++batch) {
            UINT bytes=sizeof(buffer);
            const UINT count=GetRawInputBuffer(reinterpret_cast<PRAWINPUT>(buffer.data()),&bytes,sizeof(RAWINPUTHEADER));
            if (count==UINT(-1)) { stop(L"Buffered input read failed or exceeded capacity. Restart without --buffered."); return; }
            if (count==0) return;
            if (!preview_tool::consume_buffer(buffer.data(),sizeof(buffer),count,
                [&](const RAWINPUT& raw) noexcept { process(raw,window); })) {
                stop(L"Malformed buffered input; preview stopped."); return;
            }
        }
    }
    void read_current(HWND window,LPARAM lp) noexcept {
        RAWINPUT raw{};
        UINT bytes=sizeof(raw);
        const UINT count=GetRawInputData(reinterpret_cast<HRAWINPUT>(lp),RID_INPUT,&raw,&bytes,sizeof(RAWINPUTHEADER));
        if (count==UINT(-1) || count<sizeof(RAWINPUTHEADER) || count>sizeof(raw) || raw.header.dwSize!=count) {
            stop(L"Raw input read failed or exceeded capacity."); return;
        }
        if (raw.header.dwType==RIM_TYPEMOUSE && count<offsetof(RAWINPUT,data)+sizeof(RAWMOUSE)) {
            stop(L"Truncated mouse report."); return;
        }
        process(raw,window);
        if (buffered && model.running) drain(window);
        if (!model.running) unregister_input();
    }
    void draw() noexcept {
        SetWindowTextW(status,model.status);
        const auto residual=model.processor.residual();
        wchar_t text[1400]{};
        swprintf_s(text,L"                         ORIGINAL             TRANSFORMED\r\n\r\n"
            L"Last packet X,Y     %11d, %11d      %11d, %11d\r\n\r\n"
            L"Session total X,Y   %11lld, %11lld      %11lld, %11lld\r\n\r\n"
            L"Reports: %llu       Overflow bypasses: %llu\r\n\r\n"
            L"Residual X,Y (Q24 counts): %lld, %lld     [16777216 = one count]\r\n\r\n"
            L"Confirmed setup: %u DPI, reference sensitivity %.9g\r\n"
            L"Reading: %s. Display refresh: about 30 Hz.",
            model.raw.x,model.raw.y,model.out.x,model.out.y,model.raw_x,model.raw_y,model.out_x,model.out_y,
            model.reports,model.overflows,residual.x,residual.y,profile.dpi,profile.reference_sensitivity,
            buffered?L"current event + bounded buffered drain":L"standard current event");
        SetWindowTextW(metrics,text);
    }
};

HWND control(HWND parent,const wchar_t* type,const wchar_t* text,DWORD style,int x,int y,int width,int height,int id) {
    const auto child=CreateWindowExW(0,type,text,WS_CHILD|WS_VISIBLE|style,x,y,width,height,parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);
    if (child) SendMessageW(child,WM_SETFONT,reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)),TRUE);
    return child;
}
LRESULT CALLBACK procedure(HWND window,UINT message,WPARAM wp,LPARAM lp) {
    auto* app=reinterpret_cast<App*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    if (message==WM_NCCREATE) {
        app=static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(app));
    }
    if (!app) return DefWindowProcW(window,message,wp,lp);
    switch (message) {
        case WM_CREATE: {
            const auto title=control(window,L"STATIC",L"Directional sensitivity: application-local preview",0,20,15,910,25,0);
            const auto note=control(window,L"STATIC",L"Select one mouse and Start. No system-wide interception; desktop input stays unchanged.",0,20,42,910,25,0);
            app->combo=control(window,L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_VSCROLL|WS_TABSTOP,20,75,650,220,device_combo);
            const auto start=control(window,L"BUTTON",L"Start",WS_TABSTOP,685,75,75,28,start_button);
            const auto stop=control(window,L"BUTTON",L"Stop",WS_TABSTOP,770,75,75,28,stop_button);
            const auto refresh=control(window,L"BUTTON",L"Refresh",WS_TABSTOP,855,75,75,28,refresh_button);
            app->status=control(window,L"STATIC",L"",0,20,120,910,40,0);
            app->metrics=control(window,L"STATIC",L"",0,20,175,910,285,0);
            const auto help=control(window,L"STATIC",L"Focus loss pauses and clears carry. Resume with Start. Reconfirm setup after DPI changes.\r\n"
                L"Cardinal packets pass through exactly; alternating cardinal reports cannot reveal a physical diagonal.",0,20,465,910,45,0);
            if (!title || !note || !app->combo || !start || !stop || !refresh || !app->status || !app->metrics || !help) return -1;
            SendMessageW(app->metrics,WM_SETFONT,reinterpret_cast<WPARAM>(GetStockObject(ANSI_FIXED_FONT)),TRUE);
            app->refresh(); app->draw();
            if (!SetTimer(window,1,33,nullptr)) return -1;
            return 0;
        }
        case WM_COMMAND:
            if (LOWORD(wp)==start_button) app->start(window);
            else if (LOWORD(wp)==stop_button) app->stop(L"Stopped. Start begins a fresh preview.");
            else if (LOWORD(wp)==refresh_button) app->refresh();
            else if (LOWORD(wp)==device_combo && HIWORD(wp)==CBN_SELCHANGE) app->stop(L"Selection changed. Click Start.");
            return 0;
        case WM_ACTIVATEAPP:
            if (!wp) app->stop(L"Paused on focus loss. Click Start to begin a fresh preview.");
            return 0;
        case WM_INPUT_DEVICE_CHANGE:
            if (wp==GIDC_REMOVAL && reinterpret_cast<HANDLE>(lp)==app->model.selected) {
                app->model.removed(reinterpret_cast<HANDLE>(lp)); app->unregister_input();
                SendMessageW(app->combo,CB_SETCURSEL,static_cast<WPARAM>(-1),0);
            }
            return 0;
        case WM_INPUT:
            app->read_current(window,lp);
            // Always clean up the current foreground event, even after stop/error.
            return DefWindowProcW(window,message,wp,lp);
        case WM_TIMER:
            if (wp==1) app->draw();
            return 0;
        case WM_DESTROY:
            app->stop(L"Closed."); KillTimer(window,1); PostQuitMessage(0); return 0;
        default: return DefWindowProcW(window,message,wp,lp);
    }
}
template<class T> bool number(const std::wstring& text,T& output) {
    std::wistringstream input(text); input.imbue(std::locale::classic());
    return static_cast<bool>(input>>output) && (input>>std::ws).eof();
}
}

int wmain(int argc,wchar_t** argv) {
    App app;
    std::wstring path;
    std::uint32_t dpi=0; double sensitivity=0;
    for (int i=1;i<argc;++i) {
        const std::wstring option=argv[i];
        if (option==L"--buffered" && !app.buffered) { app.buffered=true; continue; }
        if (i+1<argc && (option==L"--profile" || option==L"--dpi" || option==L"--sensitivity")) {
            const std::wstring value=argv[++i];
            if (option==L"--profile" && path.empty()) { path=value; continue; }
            if (option==L"--dpi" && !dpi) {
                std::int64_t wide;
                if (number(value,wide) && wide>=1 && wide<=1000000) { dpi=static_cast<std::uint32_t>(wide); continue; }
            }
            if (option==L"--sensitivity" && !sensitivity && number(value,sensitivity) && std::isfinite(sensitivity) && sensitivity>0) continue;
        }
        std::cerr << "Invalid/duplicate arguments\n"; return 2;
    }
    if (path.empty() || !dpi || sensitivity<=0) {
        std::cerr << "Usage: preview --profile file.dsp --dpi N --sensitivity S [--buffered]\n"; return 2;
    }
    BOOL wow64=FALSE;
    if (app.buffered && (!IsWow64Process(GetCurrentProcess(),&wow64) || wow64)) {
        std::cerr << "Buffered preview requires a native process, not WOW64. Omit --buffered.\n"; return 2;
    }
    std::ifstream file{std::filesystem::path(path)};
    std::string error;
    if (!file || !directional::read_profile(file,app.profile,error)) { std::cerr << "Cannot load profile: " << error << '\n'; return 2; }
    if (!directional::matches_setup(app.profile,dpi,sensitivity)) { std::cerr << "Profile setup mismatch; select a matching profile or recalibrate\n"; return 3; }
    SetProcessDPIAware();
    WNDCLASSW wc{}; wc.lpfnWndProc=procedure; wc.hInstance=GetModuleHandleW(nullptr);
    wc.lpszClassName=L"DirectionalSensitivityPreview"; wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);
    wc.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_WINDOW+1);
    if (!RegisterClassW(&wc)) { std::cerr << "Window registration failed\n"; return 2; }
    const auto window=CreateWindowExW(0,wc.lpszClassName,L"Directional sensitivity preview (application only)",
        WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,CW_USEDEFAULT,CW_USEDEFAULT,975,565,nullptr,nullptr,wc.hInstance,&app);
    if (!window) { std::cerr << "Preview window creation failed\n"; return 2; }
    ShowWindow(window,SW_SHOW); UpdateWindow(window);
    MSG message{};
    int result;
    while ((result=static_cast<int>(GetMessageW(&message,nullptr,0,0)))>0) {
        if (!IsDialogMessageW(window,&message)) { TranslateMessage(&message); DispatchMessageW(&message); }
    }
    if (result<0) { app.stop(L"Message loop failed."); DestroyWindow(window); return 2; }
    return 0;
}
