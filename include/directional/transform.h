#pragma once
#include "directional/types.h"

namespace directional {
// Gain values are unsigned Q16.16; movement and output are signed device counts.
constexpr uint32 unity_gain = 1u << 16;
enum class Quadrant : unsigned { NE, SE, SW, NW };
struct GainPair {
    uint32 x = unity_gain;
    uint32 y = unity_gain;
};
struct Delta { int32 x, y; };
constexpr int64 residual_one = int64{1} << 24;  // internal Q24 fixed point

class Profile {
public:
    // Fails without modifying output. Validation includes a sufficient analytic
    // condition for monotonic output angle, not a sampled angular plot.
    static bool create(const GainPair (&gains)[4], Profile& output) noexcept;
    template<class Container>
    static bool create(const Container& gains, Profile& output) noexcept {
        if (gains.size()!=4) return false;
        GainPair copy[4];
        for (unsigned q=0;q<4;++q) copy[q]=gains[q];
        return create(copy,output);
    }
    const GainPair& gain(Quadrant q) const noexcept { return gains_[static_cast<unsigned>(q)]; }
private:
    GainPair gains_[4]{};
};

struct Result {
    Delta delta;
    bool overflow_bypass = false;
};

// One processor per device. Caller serializes process/apply/set_enabled/reset;
// this class is not a driver configuration publication or concurrency primitive.
class Processor {
public:
    void apply(const Profile& profile) noexcept;
    void set_enabled(bool enabled) noexcept;
    void reset() noexcept;
    Result process(Delta input) noexcept;
    // Read-only diagnostic: fraction carried per axis, in Q24 counts (residual_one == 1 output
    // count). After any non-overflow packet |value| <= residual_one/2; zero after reset.
    struct Residual { int64 x, y; };
    Residual residual() const noexcept { return {residual_x_, residual_y_}; }
private:
    Profile profile_{};
    bool enabled_ = true;
    int64 residual_x_ = 0, residual_y_ = 0;
    int sign_x_ = 0, sign_y_ = 0;
};
} // namespace directional
