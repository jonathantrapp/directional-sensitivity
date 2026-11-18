#pragma once
#include "directional/types.h"
#include <guiddef.h>

namespace directional::driver {
// Stable, pointer-free METHOD_BUFFERED ABI. Do not send C++ Profile objects.
inline constexpr GUID interface_id={0x69fa5831,0x949b,0x47b8,{0x88,0xcf,0xd8,0xcd,0x3a,0x09,0xa5,0x4e}};
inline constexpr GUID raw_class_id={0xb2847247,0x9721,0x4dae,{0xa2,0xa1,0x98,0x85,0x48,0xd5,0x41,0xca}};
constexpr uint32 version=1;
constexpr uint32 ioctl_get_state=(0x22u<<16)|(1u<<14)|(0x800u<<2);
constexpr uint32 ioctl_set_profile=(0x22u<<16)|(3u<<14)|(0x801u<<2);
struct Gains { uint32 x,y; };
struct SetProfile {
    uint32 abi_version, size, enabled, expected_generation;
    Gains gains[4]; // NE, SE, SW, NW, unsigned Q16.16
};
struct State {
    uint32 abi_version,size,enabled,generation,connected,reserved;
    uint64 packets,overflow_bypasses;
    Gains gains[4];
};
static_assert(sizeof(SetProfile)==48 && sizeof(State)==72,"Unexpected driver protocol packing");
} // namespace directional::driver
