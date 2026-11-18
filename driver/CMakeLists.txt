if(NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "The driver currently requires MSVC x64")
endif()
set(DIRECTIONAL_WDK_ROOT "${CMAKE_BINARY_DIR}/wdk/package/c" CACHE PATH "Extracted Microsoft WDK NuGet c directory")
set(wdk "${DIRECTIONAL_WDK_ROOT}")
if(NOT EXISTS "${wdk}/Include/10.0.26100.0/km/ntddk.h")
  message(FATAL_ERROR "WDK missing; see scripts/build-driver.ps1 and docs/driver.md")
endif()
add_executable(directional_filter filter.cpp ../core/transform.cpp)
set_target_properties(directional_filter PROPERTIES SUFFIX ".sys" MSVC_RUNTIME_LIBRARY MultiThreaded)
target_include_directories(directional_filter PRIVATE ../include
  "${wdk}/Include/10.0.26100.0/km" "${wdk}/Include/10.0.26100.0/km/crt"
  "${wdk}/Include/wdf/kmdf/1.33")
target_compile_definitions(directional_filter PRIVATE _AMD64_ AMD64 _KERNEL_MODE=1
  _HAS_EXCEPTIONS=0 NTDDI_VERSION=0x0A00000B _WIN32_WINNT=0x0A00
  WDF_MAJOR_VERSION=1 WDF_MINOR_VERSION=33 WDF_MINIMUM_VERSION_REQUIRED=33)
target_compile_options(directional_filter PRIVATE /kernel /Zl /W4 /WX /GR- /EHs-c- /guard:cf)
target_link_options(directional_filter PRIVATE /DRIVER /SUBSYSTEM:NATIVE /ENTRY:FxDriverEntry
  /NODEFAULTLIB /MANIFEST:NO /INTEGRITYCHECK /GUARD:CF /OPT:REF /OPT:ICF)
target_link_libraries(directional_filter PRIVATE
  "${wdk}/Lib/10.0.26100.0/km/x64/ntoskrnl.lib"
  "${wdk}/Lib/10.0.26100.0/km/x64/hal.lib"
  "${wdk}/Lib/10.0.26100.0/km/x64/BufferOverflowFastFailK.lib"
  "${wdk}/Lib/wdf/kmdf/x64/1.33/wdfdriverentry.lib"
  "${wdk}/Lib/wdf/kmdf/x64/1.33/wdfldr.lib")
