#include "raw_devices.h"
#include "directional/capture_session.h"
#include "directional/profile_io.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <locale>
#include <sstream>

namespace {
using namespace directional::capture;
constexpr int client_width=1030,client_height=780;
enum : int { combo_id=201,resume_id=202,pause_id=203,save_id=204,refresh_id=205 };
POINT origin() noexcept { return {client_width/2,435}; }
void text(HDC dc,int x,int y,const wchar_t* value,COLORREF color=RGB(35,45,60)) {
    SetTextColor(dc,color); SetBkMode(dc,TRANSPARENT);
    TextOutW(dc,x,y,value,static_cast<int>(wcslen(value)));
}
struct App {
    Session session;
    Settings settings;
    std::filesystem::path output;
    std::vector<capture_window::Device> devices;
    HWND combo=nullptr,resume=nullptr,refresh=nullptr;
    HANDLE selected=nullptr;
    std::wstring selected_path;
    LARGE_INTEGER frequency{},epoch{};
    std::uint64_t last_time=0;
    std::uint64_t last_motion_us=0;
    bool waiting_for_stillness=false;
    bool clock_ok=true,registered=false,active=false,stale=false,saved=false,cursor_confined=false;
    HFONT font=CreateFontW(-16,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,DEFAULT_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    ~App() { release_cursor(); if (font) DeleteObject(font); }
    const wchar_t* notice=L"Select one mouse, then Start. Identity input only; no gain is applied.";
    wchar_t feedback[256]{};

    std::uint64_t now() noexcept {
        LARGE_INTEGER value{};
        if (!QueryPerformanceCounter(&value) || value.QuadPart<epoch.QuadPart || frequency.QuadPart<=0) {
            clock_ok=false; return last_time;
        }
        const auto ticks=static_cast<std::uint64_t>(value.QuadPart-epoch.QuadPart);
        const auto hz=static_cast<std::uint64_t>(frequency.QuadPart);
        const auto value_us=ticks/hz*1000000+(ticks%hz)*1000000/hz;
        if (value_us<last_time) clock_ok=false;
        else last_time=value_us;
        return last_time;
    }
    void release_cursor() noexcept {
        if (cursor_confined) {
            ClipCursor(nullptr);
            cursor_confined=false;
        }
    }
    bool confine_cursor(HWND window) noexcept {
        if (GetForegroundWindow()!=window) return false;
        // Only the legacy desktop pointer is confined. The virtual crosshair
        // continues to accumulate the selected device's unbounded raw counts.
        POINT corners[2]{{21,166},{client_width-21,699}};
        if (!ClientToScreen(window,&corners[0]) || !ClientToScreen(window,&corners[1])) return false;
        RECT bounds{corners[0].x,corners[0].y,corners[1].x,corners[1].y};
        if (!ClipCursor(&bounds)) return false;
        cursor_confined=true; return true;
    }
    void unregister_input() noexcept {
        if (!registered) return;
        RAWINPUTDEVICE device{1,2,RIDEV_REMOVE,nullptr};
        if (RegisterRawInputDevices(&device,1,sizeof(device))) registered=false;
        else { stale=true; notice=L"Raw Input removal failed. Save and close the application."; }
    }
    void pause(Outcome why,const wchar_t* message) noexcept {
        release_cursor();
        session.abort(why,now()); active=false; notice=message; unregister_input();
        SetCursor(LoadCursorW(nullptr,IDC_ARROW));
    }
    void refresh_devices() {
        if (session.prepared()) return;
        if (!capture_window::enumerate(devices)) { notice=L"Mouse enumeration failed. Retry Refresh."; return; }
        SendMessageW(combo,CB_RESETCONTENT,0,0);
        for (const auto& device:devices) SendMessageW(combo,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(device.path.c_str()));
        SendMessageW(combo,CB_SETCURSEL,static_cast<WPARAM>(-1),0);
    }
    void start(HWND window) {
        if (stale || saved || !clock_ok || session.complete()) return;
        if (session.armed()) return;
        session.flush();
        if (!session.prepared()) {
            RECT client{};
            if (!GetClientRect(window,&client) || client.right!=settings.canvas_width || client.bottom!=settings.canvas_height) {
                notice=L"Unexpected canvas size; session was not started."; return;
            }
            const auto index=SendMessageW(combo,CB_GETCURSEL,0,0);
            if (index==CB_ERR || static_cast<std::size_t>(index)>=devices.size()) { notice=L"Select a mouse first."; return; }
            const auto& chosen=devices[static_cast<std::size_t>(index)];
            selected=chosen.handle; selected_path=chosen.path;
        }
        std::vector<capture_window::Device> current;
        if (!capture_window::enumerate(current)) { notice=L"Cannot verify mouse selection."; return; }
        bool found=false;
        for (const auto& device:current) if (device.handle==selected && device.path==selected_path) found=true;
        if (!found) { stale=true; notice=L"Selection changed/disconnected. Save existing trials and restart."; return; }
        if (!session.prepared()) {
            settings.device_path=capture_window::utf8(selected_path);
            std::string error;
            if (!session.prepare(settings,error)) { notice=L"Invalid session settings; restart with valid arguments."; return; }
            EnableWindow(combo,FALSE); EnableWindow(refresh,FALSE);
        }
        if (registered) unregister_input();
        if (registered) return;
        RAWINPUTDEVICE device{1,2,RIDEV_DEVNOTIFY,window}; // foreground, legacy delivery untouched
        if (!RegisterRawInputDevices(&device,1,sizeof(device))) { notice=L"Raw Input registration failed."; return; }
        registered=true; active=true;
        last_motion_us=now(); waiting_for_stillness=false;
        notice=L"Center the regular pointer, hold still briefly, then press Space. Flick once and click.";
        SetWindowTextW(resume,L"Resume");
    }
    void arm(HWND window) noexcept {
        if (!active || saved || stale || !clock_ok || session.armed() || session.pending() || session.complete()) return;
        POINT cursor{}; const auto center=origin();
        if (!GetCursorPos(&cursor) || !ScreenToClient(window,&cursor) ||
            std::hypot(static_cast<double>(cursor.x-center.x),static_cast<double>(cursor.y-center.y))>30) {
            notice=L"Place the regular pointer in the center circle, then press Space."; return;
        }
        const auto stamp=now();
        if (!clock_ok) { pause(Outcome::clock_error,L"Clock failure. Save and restart."); return; }
        // A brief ready period avoids arming while the user is still returning
        // to center. It never drops packets after a trial has actually armed.
        if (stamp<last_motion_us || stamp-last_motion_us<300000) {
            waiting_for_stillness=true;
            notice=L"Hold the mouse still for a moment. Wait for Ready, then press Space again."; return;
        }
        waiting_for_stillness=false;
        if (!confine_cursor(window)) {
            notice=L"Cannot keep the pointer inside the aiming area. Trial was not started."; return;
        }
        if (session.arm(stamp)) {
            notice=L"Flick and click with the blue crosshair. Pointer stays inside; Escape releases it.";
            InvalidateRect(window,nullptr,FALSE);
        } else release_cursor();
    }
    void read_input(HWND window,LPARAM lp) noexcept {
        RAWINPUT raw{}; UINT bytes=sizeof(raw);
        const auto size=GetRawInputData(reinterpret_cast<HRAWINPUT>(lp),RID_INPUT,&raw,&bytes,sizeof(RAWINPUTHEADER));
        if (size==UINT(-1) || size>sizeof(raw) || size<sizeof(RAWINPUTHEADER) || raw.header.dwSize!=size ||
            (raw.header.dwType==RIM_TYPEMOUSE && size<offsetof(RAWINPUT,data)+sizeof(RAWMOUSE))) {
            pause(Outcome::read_error,L"Input read error. Save existing data and restart."); stale=true; return;
        }
        if (!active || raw.header.dwType!=RIM_TYPEMOUSE || raw.header.hDevice!=selected) return;
        if (GetForegroundWindow()!=window) { pause(Outcome::focus_lost,L"Paused on focus loss. Click Resume, then Space at the center."); return; }
        const auto stamp=now();
        if (!clock_ok) { pause(Outcome::clock_error,L"Clock failure. Save and restart."); stale=true; return; }
        const auto& mouse=raw.data.mouse;
        if (mouse.lLastX || mouse.lLastY) last_motion_us=stamp;
        session.sample(stamp,{mouse.lLastX,mouse.lLastY},mouse.usButtonFlags,mouse.usFlags);
        if (mouse.usFlags&(MOUSE_MOVE_ABSOLUTE|MOUSE_ATTRIBUTES_CHANGED)) {
            pause(mouse.usFlags&MOUSE_MOVE_ABSOLUTE?Outcome::absolute_input:Outcome::attributes_changed,
                  L"Unsupported/changed mouse attributes. Save and restart with confirmed settings."); stale=true;
        }
        if (session.pending()) { release_cursor(); SetCursor(LoadCursorW(nullptr,IDC_ARROW)); }
    }
    bool save() {
        if (saved || !session.prepared()) return saved;
        pause(Outcome::user_cancel,L"Saving ends this session."); session.flush();
        std::string error;
        if (!export_session(session,output,error)) {
            notice=L"Save failed: directory must be new and writable. Existing files were not replaced.";
            MessageBoxA(nullptr,error.c_str(),"Session export failed",MB_OK|MB_ICONERROR); return false;
        }
        saved=true; notice=L"Saved. Review outcomes and traces. No system-wide profile was applied.";
        return true;
    }
    void paint(HDC dc,bool demo=false) {
        RECT all{0,0,client_width,client_height}; FillRect(dc,&all,GetSysColorBrush(COLOR_WINDOW));
        const auto old_font=SelectObject(dc,font?font:GetStockObject(DEFAULT_GUI_FONT));
        text(dc,20,18,settings.comparison?L"Directional profile comparison  |  Condition hidden  |  Application-local":
            L"Guided baseline recorder  |  Identity only  |  Application-local");
        text(dc,20,43,demo?L"OFFSCREEN SCENE PREVIEW — synthetic input; no device registration":
            L"Start / Resume registers one foreground mouse. Space begins a trial. Escape pauses. Save ends the session.");
        if (demo) text(dc,20,83,L"[ Mouse selection ]     [ Start / Resume ]     [ Pause ]     [ Save session ]");
        text(dc,20,125,notice,RGB(40,70,120));
        RECT board{20,165,client_width-20,700};
        const auto brush=CreateSolidBrush(RGB(247,249,252)); FillRect(dc,&board,brush); DeleteObject(brush);
        const auto pen=CreatePen(PS_SOLID,1,RGB(205,215,225)); const auto old_pen=SelectObject(dc,pen);
        const auto old_brush=SelectObject(dc,GetStockObject(HOLLOW_BRUSH));
        const auto center=origin();
        for (int radius:{128,224}) Ellipse(dc,center.x-radius,center.y-radius,center.x+radius,center.y+radius);
        MoveToEx(dc,center.x-15,center.y,nullptr); LineTo(dc,center.x+15,center.y);
        MoveToEx(dc,center.x,center.y-15,nullptr); LineTo(dc,center.x,center.y+15);
        Ellipse(dc,center.x-30,center.y-30,center.x+30,center.y+30);
        if (session.armed() && session.target()) {
            const auto target=session.target()->counts;
            const int tx=center.x+static_cast<int>(std::llround(target.x*settings.pixels_per_count));
            const int ty=center.y+static_cast<int>(std::llround(target.y*settings.pixels_per_count));
            const auto target_brush=CreateSolidBrush(RGB(242,150,55)); SelectObject(dc,target_brush);
            Ellipse(dc,tx-14,ty-14,tx+14,ty+14); SelectObject(dc,old_brush); DeleteObject(target_brush);
            // Clamp rendering only; full unbounded relative counts stay in the trace.
            const int x=static_cast<int>(std::clamp(center.x+session.cursor_x()*settings.pixels_per_count,30.,1000.));
            const int y=static_cast<int>(std::clamp(center.y+session.cursor_y()*settings.pixels_per_count,175.,690.));
            const auto cross=CreatePen(PS_SOLID,3,RGB(25,110,220)); SelectObject(dc,cross);
            MoveToEx(dc,x-8,y,nullptr); LineTo(dc,x+9,y); MoveToEx(dc,x,y-8,nullptr); LineTo(dc,x,y+9);
            SelectObject(dc,pen); DeleteObject(cross);
            text(dc,40,185,L"Aim the blue crosshair at the orange target, then click.");
        } else text(dc,center.x-135,center.y+42,session.complete()?L"Complete. Save the session.":L"Regular pointer here, then Space.");
        SelectObject(dc,old_brush); SelectObject(dc,old_pen); DeleteObject(pen);
        wchar_t details[700]{};
        swprintf_s(details,L"Recorded: %zu / %zu trials    Active reports: %zu    %.6g pixels/count    DPI: %u    Reference sensitivity: %.6g",
            session.next_index(),session.trials().size(),session.active_packets(),settings.pixels_per_count,settings.dpi,settings.reference_sensitivity);
        text(dc,20,714,details);
        text(dc,20,740,L"Complete traces include misses and corrections. Candidate segments are heuristics requiring review.");
        text(dc,20,760,L"Pointer stays inside during trials; Escape releases it. This task does not reproduce your FPS camera mapping.");
        SelectObject(dc,old_font);
    }
};

HWND control(HWND window,const wchar_t* type,const wchar_t* label,int x,int width,int id,DWORD style=0) {
    const auto child=CreateWindowExW(0,type,label,WS_CHILD|WS_VISIBLE|WS_TABSTOP|style,x,75,width,
        id==combo_id?220:30,window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);
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
    switch(message) {
        case WM_CREATE: {
            app->combo=control(window,L"COMBOBOX",L"",20,500,combo_id,CBS_DROPDOWNLIST|WS_VSCROLL);
            app->resume=control(window,L"BUTTON",L"Start",530,90,resume_id);
            const auto pause=control(window,L"BUTTON",L"Pause",630,75,pause_id);
            const auto save=control(window,L"BUTTON",L"Save session",715,130,save_id);
            app->refresh=control(window,L"BUTTON",L"Refresh",855,100,refresh_id);
            if (!app->combo || !app->resume || !pause || !save || !app->refresh) return -1;
            app->refresh_devices(); if (!SetTimer(window,1,33,nullptr)) return -1; return 0;
        }
        case WM_COMMAND:
            if (LOWORD(wp)==resume_id) app->start(window);
            if (LOWORD(wp)==pause_id) app->pause(Outcome::user_cancel,L"Paused. Resume, move the pointer to the center, then Space.");
            if (LOWORD(wp)==save_id) app->save();
            if (LOWORD(wp)==refresh_id) app->refresh_devices();
            InvalidateRect(window,nullptr,FALSE); return 0;
        case WM_KEYDOWN:
            if (wp==VK_SPACE && !(lp&(1LL<<30))) app->arm(window);
            if (wp==VK_ESCAPE) app->pause(Outcome::user_cancel,L"Paused by Escape. Resume or Save.");
            return 0;
        case WM_SETCURSOR:
            if (app->session.armed() && LOWORD(lp)==HTCLIENT) { SetCursor(nullptr); return TRUE; }
            break;
        case WM_ACTIVATEAPP:
            if (!wp && app->active) app->pause(Outcome::focus_lost,L"Focus lost. Resume explicitly; interrupted trial is preserved.");
            return 0;
        case WM_ACTIVATE:
            if (LOWORD(wp)==WA_INACTIVE && app->active)
                app->pause(Outcome::focus_lost,L"Focus lost. Resume explicitly; interrupted trial is preserved.");
            break;
        case WM_MOVE:
            if (app->session.armed())
                app->pause(Outcome::geometry_changed,L"Window moved during trial. Resume and return to the center.");
            break;
        case WM_INPUT_DEVICE_CHANGE:
            if (wp==GIDC_REMOVAL && reinterpret_cast<HANDLE>(lp)==app->selected) {
                app->pause(Outcome::device_removed,L"Selected mouse disconnected. Save and start a new session."); app->stale=true;
            }
            return 0;
        case WM_INPUT:
            app->read_input(window,lp); return DefWindowProcW(window,message,wp,lp);
        case WM_TIMER: {
            const auto stamp=app->now();
            if (!app->clock_ok && app->active) { app->pause(Outcome::clock_error,L"Clock failed. Save and restart."); app->stale=true; }
            app->session.tick(stamp);
            if (app->active && app->waiting_for_stillness && stamp>=app->last_motion_us && stamp-app->last_motion_us>=300000) {
                app->waiting_for_stillness=false;
                app->notice=L"Ready. Keep the mouse still and press Space to show the target.";
            }
            const bool finished=app->session.pending();
            app->session.flush();
            if (finished) {
                app->release_cursor();
                SetCursor(LoadCursorW(nullptr,IDC_ARROW));
                if (app->active && !app->session.complete()) {
                    const auto& last=app->session.trials()[app->session.next_index()-1];
                    swprintf_s(app->feedback,L"Recorded trial %zu (%hs). Return the regular pointer to the center and press Space.",
                        app->session.next_index(),outcome_name(last.outcome));
                    app->notice=app->feedback;
                }
            }
            if (app->session.complete() && app->active) app->pause(Outcome::user_cancel,L"Session complete. Save before closing.");
            InvalidateRect(window,nullptr,FALSE); return 0;
        }
        case WM_DPICHANGED: case WM_DISPLAYCHANGE:
            app->pause(Outcome::geometry_changed,L"Display geometry changed. Save and start a new session."); app->stale=true; return 0;
        case WM_PAINT: {
            PAINTSTRUCT paint; const auto dc=BeginPaint(window,&paint); app->paint(dc); EndPaint(window,&paint);
            app->session.mark_presented(app->now()); return 0;
        }
        case WM_CLOSE:
            app->pause(Outcome::user_cancel,L"Closing."); app->session.flush();
            if (app->session.next_index() && !app->saved) {
                const int answer=MessageBoxW(window,L"Save the recorded session before closing?",L"Keep session data",MB_YESNOCANCEL|MB_ICONQUESTION);
                if (answer==IDCANCEL || (answer==IDYES && !app->save())) return 0;
            }
            DestroyWindow(window); return 0;
        case WM_DESTROY:
            app->unregister_input(); SetCursor(LoadCursorW(nullptr,IDC_ARROW)); KillTimer(window,1); PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(window,message,wp,lp);
}

bool render_demo(const std::filesystem::path& path) {
    if (std::filesystem::exists(path)) return false;
    App app; app.settings.synthetic=true; app.settings.device_path="synthetic-offscreen";
    std::string error; if (!app.session.prepare(app.settings,error)) return false;
    app.session.arm(0); app.session.mark_presented(100);
    const auto target=app.session.target()->counts;
    app.session.sample(5000,{target.x*2/3,target.y*2/3},0,0);
    app.notice=L"Synthetic scene: partial outbound movement. No mouse input was captured.";
    BITMAPINFO info{}; info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth=client_width; info.bmiHeader.biHeight=-client_height;
    info.bmiHeader.biPlanes=1; info.bmiHeader.biBitCount=32; info.bmiHeader.biCompression=BI_RGB;
    void* pixels=nullptr;
    const auto bitmap=CreateDIBSection(nullptr,&info,DIB_RGB_COLORS,&pixels,nullptr,0);
    const auto dc=CreateCompatibleDC(nullptr);
    if (!bitmap || !dc) { if (bitmap) DeleteObject(bitmap); if (dc) DeleteDC(dc); return false; }
    const auto old=SelectObject(dc,bitmap); app.paint(dc,true); GdiFlush();
    BITMAPFILEHEADER header{}; header.bfType=0x4d42;
    header.bfOffBits=sizeof(header)+sizeof(BITMAPINFOHEADER);
    const DWORD image_bytes=client_width*client_height*4;
    header.bfSize=header.bfOffBits+image_bytes;
    std::ofstream file(path,std::ios::binary);
    file.write(reinterpret_cast<const char*>(&header),sizeof(header));
    file.write(reinterpret_cast<const char*>(&info.bmiHeader),sizeof(BITMAPINFOHEADER));
    file.write(static_cast<const char*>(pixels),image_bytes); file.close();
    SelectObject(dc,old); DeleteObject(bitmap); DeleteDC(dc);
    return static_cast<bool>(file);
}
template<class T> bool number(const wchar_t* value,T& out) {
    std::wistringstream input(value); input.imbue(std::locale::classic());
    return static_cast<bool>(input>>out) && (input>>std::ws).eof();
}
}
int wmain(int argc,wchar_t** argv) {
    // Offline layout verification never enumerates/registers hardware or opens a window.
    if (argc==3 && std::wstring(argv[1])==L"--render-demo") return render_demo(std::filesystem::path(argv[2]))?0:2;
    App app;
    std::filesystem::path comparison_profile;
    bool dpi=false,sensitivity=false,scale=false,directions=false;
    for (int i=1;i<argc;++i) {
        const std::wstring option=argv[i];
        if (++i>=argc) { std::cerr<<"Missing argument\n"; return 2; }
        if (option==L"--output" && app.output.empty()) { app.output=argv[i]; continue; }
        if (option==L"--compare" && comparison_profile.empty()) { comparison_profile=argv[i]; continue; }
        if (option==L"--directions" && !directions) {
            unsigned value=0;
            if (number(argv[i],value) && (value==8 || value==24)) {
                app.settings.direction_count=value; directions=true; continue;
            }
        }
        if (option==L"--dpi" && !dpi) {
            std::int64_t value; if (number(argv[i],value) && value>0 && value<=1000000) { app.settings.dpi=static_cast<std::uint32_t>(value); dpi=true; continue; }
        }
        if (option==L"--sensitivity" && !sensitivity && number(argv[i],app.settings.reference_sensitivity) &&
            std::isfinite(app.settings.reference_sensitivity) && app.settings.reference_sensitivity>0) { sensitivity=true; continue; }
        if (option==L"--pixels-per-count" && !scale && number(argv[i],app.settings.pixels_per_count) &&
            std::isfinite(app.settings.pixels_per_count) && app.settings.pixels_per_count>=.01 && app.settings.pixels_per_count<=8) { scale=true; continue; }
        std::cerr<<"Invalid/duplicate argument\n"; return 2;
    }
    if (!dpi || !sensitivity || app.output.empty()) {
        std::cerr<<"Usage: recorder --dpi N --sensitivity S --output NEW_DIRECTORY [--pixels-per-count .5] [--compare PROFILE] [--directions 8|24]\n"; return 2;
    }
    if (!comparison_profile.empty()) {
        directional::SavedProfile profile; std::string error; std::ifstream file(comparison_profile);
        if (!directional::read_profile(file,profile,error) ||
            !directional::matches_setup(profile,app.settings.dpi,app.settings.reference_sensitivity)) {
            std::cerr<<"Comparison profile/setup invalid: "<<error<<'\n'; return 2;
        }
        app.settings.comparison=true; app.settings.candidate_profile=profile.transform;
        // Fresh ordering for each comparison; the exported seed reproduces it.
        app.settings.seed=static_cast<std::uint32_t>(GetTickCount64());
    }
    if (app.settings.direction_count==24) {
        if (!app.settings.comparison) { std::cerr<<"24-angle mode requires --compare\n"; return 2; }
        app.settings.repetitions=2; // 24 angles x two distances x two conditions = 96
    }
    if (std::filesystem::exists(app.output)) { std::cerr<<"Output directory already exists; choose a new session path\n"; return 2; }
    if (!QueryPerformanceFrequency(&app.frequency) || app.frequency.QuadPart<=0 || app.frequency.QuadPart>1000000000000LL ||
        !QueryPerformanceCounter(&app.epoch)) { std::cerr<<"Cannot initialize session clock\n"; return 2; }
    SetProcessDPIAware();
    WNDCLASSW wc{}; wc.lpfnWndProc=procedure; wc.hInstance=GetModuleHandleW(nullptr);
    wc.lpszClassName=L"DirectionalBaselineRecorder"; wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);
    wc.hbrBackground=GetSysColorBrush(COLOR_WINDOW);
    if (!RegisterClassW(&wc)) { std::cerr<<"Window registration failed\n"; return 2; }
    constexpr DWORD style=WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX|WS_CLIPCHILDREN;
    RECT rectangle{0,0,client_width,client_height}; AdjustWindowRectEx(&rectangle,style,FALSE,0);
    const auto window=CreateWindowExW(0,wc.lpszClassName,L"Guided baseline recorder — no system-wide filtering",style,
        CW_USEDEFAULT,CW_USEDEFAULT,rectangle.right-rectangle.left,rectangle.bottom-rectangle.top,nullptr,nullptr,wc.hInstance,&app);
    if (!window) { std::cerr<<"Cannot create recorder window\n"; return 2; }
    ShowWindow(window,SW_SHOW); UpdateWindow(window);
    MSG message{}; int result;
    while ((result=static_cast<int>(GetMessageW(&message,nullptr,0,0)))>0) {
        // Space is the explicit arm action even when a child button has focus.
        if (message.message==WM_KEYDOWN && (message.wParam==VK_SPACE || message.wParam==VK_ESCAPE))
            SendMessageW(window,WM_KEYDOWN,message.wParam,message.lParam);
        else if (!IsDialogMessageW(window,&message)) { TranslateMessage(&message); DispatchMessageW(&message); }
    }
    if (result<0) { app.pause(Outcome::read_error,L"Message loop error."); app.session.flush(); DestroyWindow(window); return 2; }
    return 0;
}
