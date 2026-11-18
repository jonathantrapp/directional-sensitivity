#pragma once
#include "directional/driver_protocol.h"
#include "directional/transform.h"

namespace directional {
enum class ApplyStatus { ok, invalid, stale, not_ready };

// Value-only control state shared by the kernel adapter and user-mode tests.
// The adapter must hold its per-device spin lock for every operation, including
// processor access. Publication and residual reset therefore share one boundary.
class FilterState {
public:
    FilterState() noexcept { processor.set_enabled(false); }
    Processor processor;
    ApplyStatus apply(const driver::SetProfile& command) noexcept {
        GainPair gains[4];
        for (unsigned q=0;q<4;++q) gains[q]={command.gains[q].x,command.gains[q].y};
        Profile next;
        if (command.abi_version!=driver::version || command.size!=sizeof(command) || command.enabled>1 ||
            !Profile::create(gains,next)) return ApplyStatus::invalid;
        if (command.expected_generation!=generation_) return ApplyStatus::stale;
        if (command.enabled && (!connected_ || !powered_)) return ApplyStatus::not_ready;
        processor.apply(next); processor.set_enabled(command.enabled!=0);
        profile_=next; enabled_=command.enabled!=0; advance();
        return ApplyStatus::ok;
    }
    driver::State snapshot() const noexcept {
        driver::State result{};
        result.abi_version=driver::version; result.size=sizeof(result);
        result.enabled=enabled_; result.generation=generation_;
        result.connected=connected_ && powered_;
        result.packets=packets_; result.overflow_bypasses=overflows_;
        for (unsigned q=0;q<4;++q) {
            const auto g=profile_.gain(static_cast<Quadrant>(q)); result.gains[q]={g.x,g.y};
        }
        return result;
    }
    void set_powered(bool value) noexcept { powered_=value; disable(); }
    void set_connected(bool value) noexcept { connected_=value; disable(); }
    void record(size_type count,size_type overflows) noexcept { packets_+=count; overflows_+=overflows; }
private:
    void advance() noexcept { if (++generation_==0) generation_=1; }
    void disable() noexcept {
        processor.set_enabled(false); processor.reset(); enabled_=false; advance();
    }
    Profile profile_;
    uint32 generation_=1;
    bool enabled_=false,connected_=false,powered_=false;
    uint64 packets_=0,overflows_=0;
};
} // namespace directional
