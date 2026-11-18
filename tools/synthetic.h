#pragma once
#include "directional/transform.h"
#include <cmath>
#include <string>
#include <vector>

namespace replay_tool {
inline bool known_generator(const std::string& name) {
    return name=="circle" || name=="flick" || name=="micro" || name=="cardinal" ||
           name=="reversal" || name=="mixed";
}

// Offline only. Trigonometry and vector allocation finish BEFORE any timing.
// These are synthetic relative counts, not a model of a particular mouse sensor.
inline std::vector<directional::Delta> generate(const std::string& name,std::size_t count) {
    std::vector<directional::Delta> packets;
    packets.reserve(count);
    constexpr double tau=6.2831853071795864769;
    constexpr directional::Delta micro[]={{1,-1},{1,-1},{0,0},{1,0},{0,-1},{-1,1},{-1,-1},{1,1}};
    constexpr directional::Delta cardinal[]={{1,0},{0,1},{-1,0},{0,-1},{0,0}};
    for (std::size_t i=0;i<count;++i) {
        const unsigned mode=name=="mixed" ? static_cast<unsigned>((i/256)%5) :
            name=="circle"?0u:name=="flick"?1u:name=="micro"?2u:name=="cardinal"?3u:4u;
        directional::Delta delta{};
        if (mode==0) {
            const double angle=static_cast<double>(i%1024)*tau/1024;
            delta={static_cast<directional::int32>(std::llround(32*std::cos(angle))),
                   static_cast<directional::int32>(std::llround(32*std::sin(angle)))};
        } else if (mode==1) {
            const int phase=static_cast<int>(i%64);
            const int pulse=(phase<32?phase:63-phase)*1057; // maximum 32767 counts
            const int sx=(i/64)%2? -1:1,sy=(i/128)%2? -1:1;
            delta={sx*pulse,sy*(pulse/2)};
        } else if (mode==2) delta=micro[i%8];
        else if (mode==3) delta=cardinal[i%5];
        else {
            const int sx=(i/8)%2? -1:1,sy=(i/16)%2? -1:1;
            delta={sx*(1+static_cast<int>(i%37)),sy*(1+static_cast<int>(i%23))};
        }
        packets.push_back(delta);
    }
    return packets;
}
} // namespace replay_tool
