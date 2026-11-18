#include "directional/transform.h"
#include <array>
#include <cstdint>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>

using namespace directional;
namespace {
int checks = 0;
void require(bool ok, const char* message) {
    ++checks;
    if (!ok) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
std::uint32_t gain(double x) { return static_cast<std::uint32_t>(std::llround(x * unity_gain)); }
Profile uniform(double x, double y) {
    std::array<GainPair, 4> pairs{};
    for (auto& p : pairs) p = {gain(x), gain(y)};
    Profile result;
    require(Profile::create(pairs, result), "valid profile");
    return result;
}
void equal(Result r, Delta expected, const char* message) {
    require(r.delta.x == expected.x && r.delta.y == expected.y, message);
}
void identity_and_limits() {
    Processor p;
    for (auto d : {Delta{0,0}, Delta{1,-1}, Delta{INT32_MIN,INT32_MAX}, Delta{INT32_MIN,0}})
        equal(p.process(d), d, "default exact identity");
    p.apply(uniform(4,4));
    const auto overflow = p.process({INT32_MIN, INT32_MIN});
    equal(overflow, {INT32_MIN, INT32_MIN}, "overflow pass-through");
    require(overflow.overflow_bypass, "overflow reported");
    equal(p.process({1,1}), {4,4}, "overflow clears residuals");
    p.apply(uniform(.25,.25));
    equal(p.process({INT32_MIN,INT32_MIN}), {INT32_MIN/4,INT32_MIN/4}, "negative limit scaling");
}
void state_contract() {
    Processor p;
    const auto profile = uniform(1.25,1.25);
    p.apply(profile);
    equal(p.process({1,1}), {1,1}, "fraction retained");
    for (auto d : {Delta{0,0}, Delta{100,0}, Delta{0,-100}, Delta{INT32_MAX,0}})
        equal(p.process(d), d, "cardinal bypass after fraction");
    equal(p.process({1,1}), {2,2}, "fraction survives cardinal bypass");
    equal(p.process({-1,-1}), {-1,-1}, "reversal clears fraction");
    p.apply(profile);
    equal(p.process({1,1}), {1,1}, "profile apply resets");
    p.set_enabled(false);
    equal(p.process({100,100}), {100,100}, "disabled pass-through");
    p.set_enabled(true);
    equal(p.process({1,1}), {1,1}, "enable resets");
    Processor other;
    other.apply(profile);
    equal(other.process({1,1}), {1,1}, "device state independent");
    p.reset();
    equal(p.process({1,1}), {1,1}, "device lifecycle reset");
}
void anchors_and_packetization() {
    Profile profile;
    std::array<GainPair,4> pairs{{{gain(1.5),gain(1.5)}, {gain(2),gain(2)},
                               {gain(.5),gain(.5)}, {gain(.75),gain(.75)}}};
    require(Profile::create(pairs, profile), "asymmetric quadrant profile");
    Processor p;
    p.apply(profile);
    equal(p.process({100,-100}), {150,-150}, "NE anchor");
    equal(p.process({100,100}), {200,200}, "SE anchor");
    equal(p.process({-100,100}), {-50,50}, "SW anchor");
    equal(p.process({-100,-100}), {-75,-75}, "NW anchor");
    p.apply(uniform(1.25,.875));
    equal(p.process({8,-8}), {10,-7}, "independent axis heading change");
    p.apply(uniform(2,2));
    equal(p.process({1,1}), {2,2}, "mixed packet scales");
    equal(p.process({1,0}), {1,0}, "alternating cardinal limitation X");
    equal(p.process({0,1}), {0,1}, "alternating cardinal limitation Y");
}
void reference_and_accumulation() {
    const double a = static_cast<double>(gain(1.25))/unity_gain;
    const double b = static_cast<double>(gain(.875))/unity_gain;
    Processor p;
    const auto profile = uniform(a,b);
    // Independent double-precision oracle checks the complete integer pipeline,
    // allowing half a count rounding plus bounded Q24 approximation error.
    for (int sx : {-1,1}) for (int sy : {-1,1})
        for (int x = 1; x <= 300; x += 3) for (int y = 1; y <= 300; y += 7) {
            p.apply(profile);
            const double r = static_cast<double>(x < y ? x : y)/(x > y ? x : y);
            const double w = r*r*(3-2*r);
            const auto result = p.process({sx*x,sy*y});
            require(std::abs(result.delta.x-sx*x*(1+w*(a-1))) < .501, "reference X");
            require(std::abs(result.delta.y-sy*y*(1+w*(b-1))) < .501, "reference Y");
        }
    p.apply(uniform(1.25,1.25));
    std::int64_t sum = 0;
    for (int i = 0; i < 100000; ++i) sum += p.process({1,1}).delta.x;
    require(sum == 125000, "long stream no rounding drift");
    // Near-axis ideal gain approaches unity without a snapping threshold.
    p.apply(uniform(2,2));
    equal(p.process({10000,1}), {10000,1}, "near-axis continuity");
}
void validation() {
    Profile output = uniform(1.25,1.25);
    std::array<GainPair,4> bad{};
    bad[0].x = 0;
    require(!Profile::create(bad,output), "zero gain rejected");
    require(output.gain(Quadrant::NE).x == gain(1.25), "failure leaves profile intact");
    bad[0] = {gain(4), gain(.25)};
    require(!Profile::create(bad,output), "fold-prone gains rejected");
    bad[0] = {UINT32_MAX, unity_gain};
    require(!Profile::create(bad,output), "out of range rejected");
    // Conditional angular sweep corroborates the analytic validator for accepted
    // gain pairs. Asymmetric quadrants are not expected to have sign symmetry.
    for (double a : {.25,.5,.875,1.,1.25,2.,4.})
        for (double b : {.25,.5,.875,1.,1.25,2.,4.}) {
            std::array<GainPair,4> pairs{};
            for (auto& pair : pairs) pair = {gain(a),gain(b)};
            Profile profile;
            if (!Profile::create(pairs,profile)) continue;
            double previous = -1;
            for (int i = 0; i <= 2000; ++i) {
                const double angle = i * 1.5707963267948966 / 2000;
                const double x = std::cos(angle), y = std::sin(angle);
                const double r = (x < y ? x : y)/(x > y ? x : y);
                const double w = r*r*(3-2*r);
                const double out = std::atan2(y*(1+w*(b-1)),x*(1+w*(a-1)));
                require(out > previous, "accepted curve angular ordering");
                previous = out;
            }
        }
}
// ---- Float reference (tests only) -------------------------------------------------
struct Ref { double x, y; };
Quadrant quad_of(Delta d) {
    if (d.y < 0) return d.x > 0 ? Quadrant::NE : Quadrant::NW;
    return d.x > 0 ? Quadrant::SE : Quadrant::SW;
}
// r = min/max, w = r*r*(3-2r), g = 1 + w*(anchor-1), ideal = input*g. Anchors in real units.
Ref reference(Delta d, double ax, double ay) {
    const double X = d.x, Y = d.y;
    const double mx = std::fabs(X), my = std::fabs(Y);
    const double r = (mx < my ? mx : my) / (mx > my ? mx : my);
    const double w = r * r * (3 - 2 * r);
    return {X * (1 + w * (ax - 1)), Y * (1 + w * (ay - 1))};
}
struct TestProfile { Profile profile; double ax[4], ay[4]; };
TestProfile make(const double (&ax)[4], const double (&ay)[4]) {
    TestProfile t{};
    std::array<GainPair, 4> pairs{};
    for (unsigned q = 0; q < 4; ++q) {
        t.ax[q] = ax[q]; t.ay[q] = ay[q];
        pairs[q] = {gain(ax[q]), gain(ay[q])};
    }
    require(Profile::create(pairs, t.profile), "test profile valid");
    return t;
}
std::int64_t magnitude64(std::int64_t v) { return v < 0 ? -v : v; }

// Independent real-valued reference; the step-by-step audit in transform.cpp
// bounds gain error by 17.5/U. Use 18/U plus final half-count rounding here.
void float_reference_grid() {
    const TestProfile profiles[] = {
        make({1.25,1.25,1.25,1.25}, {1.25,1.25,1.25,1.25}),
        make({2,2,2,2}, {2,2,2,2}),
        make({.5,.5,.5,.5}, {.5,.5,.5,.5}),
        make({1.5,1.5,1.5,1.5}, {1,1,1,1}),                       // unequal axis gains
        make({1.5,2,.5,1.25}, {1.5,2,.5,.875}),                     // independent quadrants
    };
    const std::int32_t values[] = {1, 2, 3, 7, 100, 999, 32767, 100000, 1000000};
    for (const auto& tp : profiles)
        for (auto a : values) for (auto b : values)
            for (int sa : {-1, 1}) for (int sb : {-1, 1}) {
                const Delta in{sa * a, sb * b};
                const auto q = static_cast<unsigned>(quad_of(in));
                const Ref ref = reference(in, tp.ax[q], tp.ay[q]);
                Processor p;
                p.apply(tp.profile);
                const Result r = p.process(in);
                require(!r.overflow_bypass, "grid packet fits");
                const double tx = .5 + 18*std::fabs(double(in.x)) / residual_one;
                const double ty = .5 + 18*std::fabs(double(in.y)) / residual_one;
                require(std::fabs(r.delta.x - ref.x) <= tx && std::fabs(r.delta.y - ref.y) <= ty,
                        "fixed point within derived bound of float reference");
                const auto res = p.residual();
                require(magnitude64(res.x) <= residual_one / 2 && magnitude64(res.y) <= residual_one / 2,
                        "residual within half a count");
            }
}
void extremes_and_overflow() {
    // |input| = 2^31 at gain 0.25 is a valid, non-overflowing extreme.
    const TestProfile quarter = make({.25,.25,.25,.25}, {.25,.25,.25,.25});
    for (Delta in : {Delta{INT32_MAX, INT32_MAX}, Delta{INT32_MIN, INT32_MIN},
                     Delta{INT32_MIN, INT32_MAX}, Delta{INT32_MAX, INT32_MIN}}) {
        Processor p; p.apply(quarter.profile);
        const Result r = p.process(in);
        const Ref ref = reference(in, .25, .25);
        require(!r.overflow_bypass, "quarter gain extreme does not overflow");
        require(std::fabs(r.delta.x - ref.x) <= .5 + 18*std::fabs(double(in.x))/residual_one &&
                std::fabs(r.delta.y - ref.y) <= .5 + 18*std::fabs(double(in.y))/residual_one,
                "int32 extreme within bound");
    }
    // Overflow at gain 4 returns the complete original packet and clears residuals.
    const TestProfile four = make({4,4,4,4}, {4,4,4,4});
    for (Delta in : {Delta{INT32_MAX, INT32_MAX}, Delta{INT32_MIN, INT32_MIN},
                     Delta{INT32_MAX, INT32_MAX/2}, Delta{INT32_MIN/2, INT32_MIN}}) {
        Processor p; p.apply(four.profile);
        p.process({1, 1});
        p.process({3, 2});                       // off-diagonal: weight < 1, builds a residual
        require(p.residual().x != 0 || p.residual().y != 0, "residual built before overflow");
        const Result r = p.process(in);
        require(r.overflow_bypass && r.delta.x == in.x && r.delta.y == in.y, "overflow returns original packet");
        require(p.residual().x == 0 && p.residual().y == 0, "overflow clears residuals");
    }
    // +/-32767 on the diagonal at the largest gain.
    Processor p; p.apply(four.profile);
    equal(p.process({32767, 32767}), {131068, 131068}, "32767 diagonal at gain 4");
    equal(p.process({-32767, -32767}), {-131068, -131068}, "-32767 diagonal at gain 4");
}
// 1.25x anchor on a diagonal is exact in Q24, so every four packets of (1,1) must release
// exactly five counts. We do not assert WHICH packet carries the extra count.
void repeated_diagonal_accumulation() {
    const TestProfile t = make({1.25,1.25,1.25,1.25}, {1.25,1.25,1.25,1.25});
    for (Delta step : {Delta{1, 1}, Delta{-1, -1}, Delta{1, -1}, Delta{-1, 1}}) {
        Processor p; p.apply(t.profile);
        std::int64_t sx = 0, sy = 0;
        for (int n = 1; n <= 4000; ++n) {
            const Result r = p.process(step);
            sx += r.delta.x; sy += r.delta.y;
            const double ideal = 1.25 * n;
            require(std::fabs(double(magnitude64(sx)) - ideal) <= .5 + 1e-9 &&
                    std::fabs(double(magnitude64(sy)) - ideal) <= .5 + 1e-9, "accumulated error <= 0.5 count");
            if (n % 4 == 0)
                require(magnitude64(sx) == 5 * n / 4 && magnitude64(sy) == 5 * n / 4, "five counts per four packets");
        }
    }
}
void unequal_gain_accumulation() {
    const TestProfile t = make({1.25,1.25,1.25,1.25}, {.875,.875,.875,.875});
    Processor p; p.apply(t.profile);
    std::int64_t sx = 0, sy = 0;
    for (int n = 1; n <= 8000; ++n) {
        const Result r = p.process({1, 1});
        sx += r.delta.x; sy += r.delta.y;
        require(std::fabs(double(sx) - 1.25 * n) <= .5 + 1e-9 && std::fabs(double(sy) - .875 * n) <= .5 + 1e-9,
                "per-axis accumulated error <= 0.5 count");
    }
}
void residual_lifecycle() {
    const TestProfile t = make({1.25,1.25,1.25,1.25}, {1.25,1.25,1.25,1.25});
    const TestProfile u = make({2,2,2,2}, {2,2,2,2});
    Processor p; p.apply(t.profile);
    require(p.residual().x == 0 && p.residual().y == 0, "residual zero after apply");
    equal(p.process({1, 1}), {1, 1}, "first diagonal packet");
    const auto held = p.residual();
    require(held.x == residual_one / 4 && held.y == residual_one / 4, "quarter count carried");

    // Cardinal and zero packets bypass BOTH accumulators, even at extremes.
    for (Delta c : {Delta{5, 0}, Delta{0, -7}, Delta{0, 0}, Delta{INT32_MIN, 0}, Delta{0, INT32_MAX}}) {
        equal(p.process(c), c, "cardinal exact identity after fractional diagonal");
        require(p.residual().x == held.x && p.residual().y == held.y, "cardinal leaves residuals untouched");
    }
    // The held fraction is still released by the next diagonal packet: 1.25 + .25 = 1.5 -> 2.
    equal(p.process({1, 1}), {2, 2}, "stored fraction released by diagonal");
    require(p.residual().x == -residual_one / 2, "tie rounds away from zero, residual -0.5");

    // Reversal on one axis clears only that axis. Without clearing x would be -1.25-.5 -> -2.
    equal(p.process({-1, 1}), {-1, 1}, "x reversal drops x residual");
    require(p.residual().x == -residual_one / 4 && p.residual().y == -residual_one / 4,
            "x restarted from zero; y carried (.75 -> 1, -0.25)");

    // apply(), reset() and a real enable change clear; a no-op set_enabled does not.
    p.set_enabled(true);
    require(p.residual().x == -residual_one / 4, "set_enabled(same) keeps residuals");
    p.set_enabled(false);
    require(p.residual().x == 0 && p.residual().y == 0, "disable clears residuals");
    equal(p.process({1, 1}), {1, 1}, "disabled passes through");
    require(p.residual().x == 0, "disabled does not accumulate");
    p.set_enabled(true);
    p.process({1, 1});
    require(p.residual().x != 0, "residual rebuilt after enable");
    p.apply(u.profile);
    require(p.residual().x == 0 && p.residual().y == 0, "profile change clears residuals");
    p.apply(t.profile); p.process({1, 1});
    p.reset();
    require(p.residual().x == 0 && p.residual().y == 0, "reset clears residuals");
}
void random_residual_bounds() {
    const TestProfile t = make({1.5,2,.5,1.25}, {1.5,2,.5,.875});
    Processor p; p.apply(t.profile);
    std::uint32_t s = 12345;
    auto next = [&s]() { s = s * 1664525u + 1013904223u; return s >> 8; };
    for (int i = 0; i < 200000; ++i) {
        const Delta in{static_cast<std::int32_t>(next() % 4001) - 2000, static_cast<std::int32_t>(next() % 4001) - 2000};
        const Result r = p.process(in);
        require(!r.overflow_bypass, "random packet fits");
        require(magnitude64(p.residual().x) <= residual_one / 2 && magnitude64(p.residual().y) <= residual_one / 2,
                "random residual bound");
    }
}
void quadrants_gains_and_symmetry() {
    // On exact diagonals r = 1, w = 1, so each quadrant's anchor applies in full.
    const TestProfile mixed = make({1.5,2,.5,1.25}, {1.5,2,.5,1.25});   // NE, SE, SW, NW
    Processor p; p.apply(mixed.profile);
    equal(p.process({100, -100}), {150, -150}, "NE anchor");
    equal(p.process({100, 100}), {200, 200}, "SE anchor");
    equal(p.process({-100, 100}), {-50, 50}, "SW anchor");
    equal(p.process({-100, -100}), {-125, -125}, "NW anchor");
    // Unequal anchors change heading on purpose; equal anchors keep it.
    const TestProfile unequal = make({1.5,1.5,1.5,1.5}, {1,1,1,1});
    p.apply(unequal.profile);
    equal(p.process({100, 100}), {150, 100}, "unequal gains change heading");
    p.apply(uniform(2, 2));
    const Result big = p.process({30000, 10000});
    require(std::fabs(double(big.delta.y) / big.delta.x - 10000.0 / 30000.0) < 1e-4, "equal gains keep heading");

    // Sign symmetry holds only for symmetric profiles, from fresh state.
    for (Delta d : {Delta{3, 2}, Delta{100, 37}, Delta{1, 999}, Delta{7, 7}}) {
        Processor a, b, c; a.apply(uniform(1.5, 1.5)); b.apply(uniform(1.5, 1.5)); c.apply(uniform(1.5, 1.5));
        const Result pos = a.process(d), neg = b.process({-d.x, -d.y}), mirror = c.process({-d.x, d.y});
        require(neg.delta.x == -pos.delta.x && neg.delta.y == -pos.delta.y, "point symmetry, symmetric profile");
        require(mirror.delta.x == -pos.delta.x && mirror.delta.y == pos.delta.y, "mirror symmetry, symmetric profile");
    }
    Processor a, b; a.apply(mixed.profile); b.apply(mixed.profile);
    const Result ne = a.process({100, -100}), sw = b.process({-100, 100});
    require(sw.delta.x != -ne.delta.x, "asymmetric profile is not sign symmetric");

    // Near-axis and diagonal boundaries are continuous within a couple of counts.
    p.apply(uniform(2, 2));
    equal(p.process({32767, 1}), {32767, 1}, "near-axis x");
    const Result d99 = p.process({100, 99}), d100 = p.process({100, 100});
    require(std::abs(d100.delta.x - d99.delta.x) <= 2 && std::abs(d100.delta.y - d99.delta.y) <= 3, "diagonal boundary continuity");
}
void boundary_sweeps() {
    constexpr int n=1000;
    for (const auto anchors : {GainPair{gain(.25),gain(.25)},GainPair{gain(4),gain(4)},
                               GainPair{gain(1.25),gain(.875)}}) {
        const double a=double(anchors.x)/unity_gain,b=double(anchors.y)/unity_gain;
        const auto profile=uniform(a,b);
        // For x=N, |d out_x /dy| <=1.5|a-1| on both sides of y=N.
        // |d out_y /dy| <=max(1,b)+1.5|b-1|. Add two independent
        // rounding errors and fixed-point error to bound adjacent integer steps.
        const double error=18.0*(2*n)/residual_one;
        const double max_dx=std::ceil(1.5*std::abs(a-1)+1+2*error);
        const double max_dy=std::ceil(std::max(1.,b)+1.5*std::abs(b-1)+1+2*error);
        for (int sx:{-1,1}) for (int sy:{-1,1}) {
            Delta previous{};
            for (int y=0;y<=2*n;++y) {
                Processor p; p.apply(profile);
                const Delta in{sx*n,sy*y};
                const auto result=p.process(in);
                const auto ideal=reference(in,a,b);
                require(!result.overflow_bypass,"boundary sweep fits");
                require(std::fabs(result.delta.x-ideal.x)<=.5+error &&
                        std::fabs(result.delta.y-ideal.y)<=.5+error,"boundary reference bound");
                require(result.delta.x*sx>=0 && result.delta.y*sy>=0,"no unintended sign flips");
                if (y) require(std::abs(result.delta.x-previous.x)<=max_dx &&
                               std::abs(result.delta.y-previous.y)<=max_dy,"derived adjacent-step bound");
                previous=result.delta;
            }
        }
    }
}
} // namespace
int main() {
    identity_and_limits(); state_contract(); anchors_and_packetization();
    reference_and_accumulation(); validation();
    float_reference_grid(); extremes_and_overflow(); repeated_diagonal_accumulation();
    unequal_gain_accumulation(); residual_lifecycle(); random_residual_bounds(); quadrants_gains_and_symmetry();
    boundary_sweeps();
    std::cout << "PASS: " << checks << " checks\n";
}
