#pragma once
#include "preview_input.h"
#include <string>
#include <vector>
namespace capture_window {
struct Device { HANDLE handle; std::wstring path; };
inline bool enumerate(std::vector<Device>& output) {
    UINT count=0;
    if (GetRawInputDeviceList(nullptr,&count,sizeof(RAWINPUTDEVICELIST))!=0) return false;
    std::vector<RAWINPUTDEVICELIST> list(count);
    if (count && GetRawInputDeviceList(list.data(),&count,sizeof(RAWINPUTDEVICELIST))==UINT(-1)) return false;
    std::vector<Device> result;
    for (UINT i=0;i<count;++i) if (list[i].dwType==RIM_TYPEMOUSE) {
        UINT characters=0;
        if (GetRawInputDeviceInfoW(list[i].hDevice,RIDI_DEVICENAME,nullptr,&characters)==UINT(-1) || !characters || characters>32768) return false;
        std::wstring path(characters+1,L'\0');
        if (GetRawInputDeviceInfoW(list[i].hDevice,RIDI_DEVICENAME,path.data(),&characters)==UINT(-1)) return false;
        path.resize(wcslen(path.c_str())); result.push_back({list[i].hDevice,std::move(path)});
    }
    output=std::move(result); return true;
}
inline std::string utf8(const std::wstring& text) {
    const int size=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text.c_str(),-1,nullptr,0,nullptr,nullptr);
    if (size<=0) return {};
    std::string result(static_cast<std::size_t>(size),'\0');
    if (!WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text.c_str(),-1,result.data(),size,nullptr,nullptr)) return {};
    result.pop_back(); return result;
}
} // namespace capture_window
