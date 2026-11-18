#pragma once
#include "directional/transform.h"

namespace directional {
struct ForwardResult {
    size_type consumed=0;
    size_type overflow_bypasses=0;
};

// Packet is the Windows MOUSE_INPUT_DATA layout (or a compatible test type).
// Sink accepts exactly one temporary packet synchronously and returns whether
// the downstream class queue consumed it. Original input storage is never edited.
// Caller serializes state/profile changes and supplies nonpaged storage in kernel.
template<class Packet,class Sink>
ForwardResult forward_packets(const Packet* packets,size_type count,Processor& state,Sink&& sink) noexcept {
    ForwardResult result;
    for (size_type i=0;i<count;++i) {
        Processor next=state;
        Packet packet=packets[i];
        bool overflow=false;
        // Absolute movement and attribute changes cannot share relative residuals.
        // Reset only if this packet is actually consumed by the downstream queue.
        if (packet.Flags & 0x0005u) next.reset();
        else {
            const auto transformed=next.process({packet.LastX,packet.LastY});
            packet.LastX=transformed.delta.x; packet.LastY=transformed.delta.y;
            overflow=transformed.overflow_bypass;
        }
        if (!sink(packet)) break;
        state=next;
        ++result.consumed;
        if (overflow) ++result.overflow_bypasses;
    }
    return result;
}
} // namespace directional
