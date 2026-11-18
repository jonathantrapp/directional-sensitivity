#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include "directional/driver_protocol.h"
#include "directional/profile_io.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>

namespace {
using namespace directional;
struct Entry { std::wstring path,parent_id; };
struct Handle {
    HANDLE value=INVALID_HANDLE_VALUE;
    ~Handle() { if (value!=INVALID_HANDLE_VALUE) CloseHandle(value); }
};
bool enumerate(std::vector<Entry>& devices) {
    const auto set=SetupDiGetClassDevsW(&driver::interface_id,nullptr,nullptr,DIGCF_PRESENT|DIGCF_DEVICEINTERFACE);
    if (set==INVALID_HANDLE_VALUE) return false;
    bool ok=true;
    for (DWORD i=0;;++i) {
        SP_DEVICE_INTERFACE_DATA interface_data{}; interface_data.cbSize=sizeof(interface_data);
        if (!SetupDiEnumDeviceInterfaces(set,nullptr,&driver::interface_id,i,&interface_data)) {
            ok=GetLastError()==ERROR_NO_MORE_ITEMS; break;
        }
        DWORD bytes=0;
        SetupDiGetDeviceInterfaceDetailW(set,&interface_data,nullptr,0,&bytes,nullptr);
        if (!bytes || bytes>65536) { ok=false; break; }
        std::vector<unsigned char> buffer(bytes);
        auto* detail=reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buffer.data());
        detail->cbSize=sizeof(*detail);
        SP_DEVINFO_DATA devinfo{}; devinfo.cbSize=sizeof(devinfo);
        if (!SetupDiGetDeviceInterfaceDetailW(set,&interface_data,detail,bytes,nullptr,&devinfo)) { ok=false; break; }
        DEVINST parent;
        wchar_t parent_id[MAX_DEVICE_ID_LEN]{};
        if (CM_Get_Parent(&parent,devinfo.DevInst,0)!=CR_SUCCESS ||
            CM_Get_Device_IDW(parent,parent_id,MAX_DEVICE_ID_LEN,0)!=CR_SUCCESS) {
            ok=false; break;
        }
        devices.push_back({detail->DevicePath,parent_id});
    }
    SetupDiDestroyDeviceInfoList(set);
    return ok;
}
bool get_state(HANDLE handle,driver::State& state) {
    DWORD bytes=0;
    return DeviceIoControl(handle,driver::ioctl_get_state,nullptr,0,&state,sizeof(state),&bytes,nullptr) &&
        bytes==sizeof(state) && state.abi_version==driver::version && state.size==sizeof(state) && state.reserved==0;
}
template<class T> bool number(const wchar_t* text,T& value) {
    std::wistringstream input(text); input.imbue(std::locale::classic());
    return static_cast<bool>(input>>value) && (input>>std::ws).eof();
}
}
int wmain(int argc,wchar_t** argv) {
    using namespace directional;
    if (argc<2) {
        std::cerr << "Usage: driver_ctl list\n"
                     "       driver_ctl status DEVICE_INTERFACE_PATH\n"
                     "       driver_ctl disable DEVICE_INTERFACE_PATH\n"
                     "       driver_ctl apply DEVICE_INTERFACE_PATH profile.dsp DPI SENSITIVITY\n";
        return 2;
    }
    const std::wstring command=argv[1];
    if (command==L"list" && argc==2) {
        std::vector<Entry> devices;
        if (!enumerate(devices)) { std::cerr << "Driver enumeration failed\n"; return 2; }
        for (const auto& entry:devices) std::wcout << L"Mouse: " << entry.parent_id << L"\nInterface: " << entry.path << L"\n\n";
        if (devices.empty()) std::cout << "No directional filter devices installed.\n";
        return 0;
    }
    if (!((command==L"status" || command==L"disable") && argc==3) && !(command==L"apply" && argc==6)) {
        std::cerr << "Invalid command/arguments\n"; return 2;
    }
    // Use the exact enumerated interface rather than a transient index: a device
    // reconnect cannot silently retarget an apply command to another mouse.
    std::vector<Entry> devices;
    if (!enumerate(devices)) { std::cerr << "Driver enumeration failed\n"; return 2; }
    bool found=false;
    for (const auto& entry:devices) if (_wcsicmp(entry.path.c_str(),argv[2])==0) found=true;
    if (!found) { std::cerr << "Device interface is not present; run driver_ctl list\n"; return 2; }
    Handle handle;
    handle.value=CreateFileW(argv[2],GENERIC_READ|(command==L"status"?0:GENERIC_WRITE),
        FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if (handle.value==INVALID_HANDLE_VALUE) {
        std::cerr << "Cannot open device (administrator access required), Windows error " << GetLastError() << '\n'; return 2;
    }
    driver::State state{};
    if (!get_state(handle.value,state)) { std::cerr << "Cannot read compatible driver state\n"; return 2; }
    if (command!=L"status") {
        driver::SetProfile update{};
        update.abi_version=driver::version; update.size=sizeof(update);
        update.expected_generation=state.generation;
        for (unsigned q=0;q<4;++q) update.gains[q]={unity_gain,unity_gain};
        if (command==L"apply") {
            std::ifstream file{std::filesystem::path(argv[3])};
            SavedProfile profile;
            std::string error;
            std::uint32_t dpi=0; double sensitivity=0;
            if (!file || !read_profile(file,profile,error) || !number(argv[4],dpi) || !number(argv[5],sensitivity)) {
                std::cerr << "Invalid profile/setup: " << error << '\n'; return 2;
            }
            if (!matches_setup(profile,dpi,sensitivity)) { std::cerr << "Setup mismatch; recalibrate or select a matching profile\n"; return 3; }
            for (unsigned q=0;q<4;++q) {
                const auto gain=profile.transform.gain(static_cast<Quadrant>(q)); update.gains[q]={gain.x,gain.y};
            }
            update.enabled=1;
        }
        DWORD bytes=0;
        if (!DeviceIoControl(handle.value,driver::ioctl_set_profile,&update,sizeof(update),nullptr,0,&bytes,nullptr)) {
            std::cerr << "Apply failed (device state may have changed), Windows error " << GetLastError() << '\n'; return 2;
        }
        if (!get_state(handle.value,state)) { std::cerr << "Update sent, but state verification failed\n"; return 2; }
        // Detect racing writers rather than reporting someone else's profile as ours.
        const auto expected=update.expected_generation==UINT32_MAX ? 1:update.expected_generation+1;
        bool verified=state.generation==expected && state.enabled==update.enabled;
        for (unsigned q=0;q<4;++q) verified=verified && state.gains[q].x==update.gains[q].x && state.gains[q].y==update.gains[q].y;
        if (!verified) { std::cerr << "Update raced with another device change; inspect status\n"; return 3; }
    }
    std::cout << "enabled=" << state.enabled << " connected=" << state.connected << " generation=" << state.generation
              << " packets=" << state.packets << " overflow_bypasses=" << state.overflow_bypasses << '\n';
    const char* names[]={"NE","SE","SW","NW"};
    for (unsigned q=0;q<4;++q) std::cout << names[q] << '=' << static_cast<double>(state.gains[q].x)/unity_gain << ','
                                       << static_cast<double>(state.gains[q].y)/unity_gain << '\n';
}
