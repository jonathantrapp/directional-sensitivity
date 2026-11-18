#include "directional/transform.h"

namespace directional {
namespace {
template<class T> constexpr T minimum(T a,T b) noexcept { return a<b?a:b; }
template<class T> constexpr T maximum(T a,T b) noexcept { return a>b?a:b; }
constexpr int64 unit = residual_one;
// Arithmetic audit (validated anchors in [1/4,4], U=2^24):
// |input| <= 2^31 after widening; min(|x|,|y|)*U <= 2^55.
// ratio R and squared S are in [0,U]; R*R <= 2^48.
// S*(3U-2R) <= 3*2^48 < 2^50, with resulting weight in [0,U].
// |(anchor<<8)-U| <= 3U; its product with weight is < 2^50.
// Effective gain lies in [U/4,4U]; |input*gain| <= 2^57.
// |old residual| <= U/2, so scaled+carry and rounding offsets are < 2^58.
// Negation and signed rounding are therefore safe in int64. Output is range
// checked BEFORE int32 conversion; an overflowing packet bypasses and resets.
// After that check, output*U for the residual update has magnitude <= 2^55.
// The validator's min/difference products are <2^37 after anchor range checks.
// Float-oracle bound: ratio truncation <1/U, |h'|<=1.5, square truncation
// contributes <3/U to h and weight truncation <1/U: |weight error|<5.5/U.
// Gain truncation adds <1/U, so |gain error|<17.5/U; fresh-state output
// error versus exact-real math is <=0.5 + 18*|input|/U counts.
int sign(int32 v) noexcept { return v > 0 ? 1 : -1; }
int64 magnitude(int32 v) noexcept {
    // Widen before negation: abs(INT32_MIN) is not representable in int32_t.
    const auto wide = static_cast<int64>(v);
    return wide < 0 ? -wide : wide;
}
Quadrant quadrant(Delta d) noexcept {
    if (d.y < 0) return d.x > 0 ? Quadrant::NE : Quadrant::NW;
    return d.x > 0 ? Quadrant::SE : Quadrant::SW;
}
bool valid(GainPair g) noexcept {
    if (g.x < unity_gain / 4 || g.x > 4 * unity_gain ||
        g.y < unity_gain / 4 || g.y > 4 * unity_gain) return false;
    const auto a = static_cast<int64>(g.x);
    const auto b = static_cast<int64>(g.y);
    const auto difference = a > b ? a - b : b - a;
    // For tan(output)=r*B/A, derivative numerator is A*B+r*w'*(b-a).
    // r*w' <= 8/9; A*B >= min(1,a)*min(1,b). The reverse half-sector
    // swaps a,b, so this absolute bound covers both sides of the diagonal.
    return 9 * minimum(a, int64{unity_gain}) *
        minimum(b, int64{unity_gain}) >
        8 * difference * unity_gain;
}
int64 quantize(int64 value) noexcept {
    // Round to nearest, ties away from zero; the remaining fraction is carried.
    return value >= 0 ? (value + unit / 2) / unit : -((-value + unit / 2) / unit);
}
bool fits(int64 n) noexcept {
    return n >= -2147483647LL-1 && n <= 2147483647LL;
}
} // namespace

bool Profile::create(const GainPair (&gains)[4], Profile& output) noexcept {
    for (const auto g : gains) if (!valid(g)) return false;
    for (unsigned q=0;q<4;++q) output.gains_[q]=gains[q];
    return true;
}
void Processor::reset() noexcept {
    residual_x_ = residual_y_ = 0;
    sign_x_ = sign_y_ = 0;
}
void Processor::apply(const Profile& profile) noexcept { profile_ = profile; reset(); }
void Processor::set_enabled(bool enabled) noexcept {
    if (enabled_ != enabled) { enabled_ = enabled; reset(); }
}
Result Processor::process(Delta input) noexcept {
    // A cardinal packet must not release fractions from previous diagonal input.
    if (!enabled_ || input.x == 0 || input.y == 0) return {input, false};
    const auto anchor = profile_.gain(quadrant(input));
    if (anchor.x == unity_gain && anchor.y == unity_gain) {
        // Also prevent residuals leaking into an identity quadrant.
        reset();
        return {input, false};
    }
    const int sx = sign(input.x), sy = sign(input.y);
    if (sign_x_ != sx) residual_x_ = 0;
    if (sign_y_ != sy) residual_y_ = 0;
    sign_x_ = sx; sign_y_ = sy;

    const auto ax = magnitude(input.x), ay = magnitude(input.y);
    const auto ratio = minimum(ax, ay) * unit / maximum(ax, ay);
    const auto squared = ratio * ratio / unit;
    const auto weight = squared * (3 * unit - 2 * ratio) / unit;
    const auto gx = unit + ((static_cast<int64>(anchor.x) << 8) - unit) * weight / unit;
    const auto gy = unit + ((static_cast<int64>(anchor.y) << 8) - unit) * weight / unit;
    // Max gain=4, |input|<=2^31, Q24: intermediates stay below 2^58.
    const auto ideal_x = static_cast<int64>(input.x) * gx + residual_x_;
    const auto ideal_y = static_cast<int64>(input.y) * gy + residual_y_;
    const auto x = quantize(ideal_x), y = quantize(ideal_y);
    if (!fits(x) || !fits(y)) {
        reset();
        return {input, true};
    }
    residual_x_ = ideal_x - x * unit;
    residual_y_ = ideal_y - y * unit;
    return {{static_cast<int32>(x), static_cast<int32>(y)}, false};
}
} // namespace directional
