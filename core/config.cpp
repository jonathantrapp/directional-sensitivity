#include "directional/config.h"

namespace directional {
namespace {
constexpr int64 half = 1 << 15;  // 0.5 in Q16.16

// (a*b)/65536 rounded half away from zero. |a*b| <= 2^62, so int64 cannot overflow.
int64 mul_q16(int32 a, int32 b) noexcept {
    const int64 p = static_cast<int64>(a) * static_cast<int64>(b);
    return p >= 0 ? (p + half) >> 16 : -((-p + half) >> 16);
}

bool expand(const FilterConfig& c, DirectionalAnchors& out, Profile* profile) noexcept {
    if ((c.enabled != 0 && c.enabled != 1) || c.sx<=0 || c.sy<=0) return false;
    const int32 cq[4] = {c.c_ne, c.c_se, c.c_sw, c.c_nw};
    GainPair gains[4];
    DirectionalAnchors tmp;
    for (unsigned q = 0; q < 4; ++q) {
        // Reject invalid factors themselves, not only the resulting anchors:
        // two negative factors could otherwise cancel into an accepted gain.
        if (cq[q]<=0) return false;
        const int64 ax = mul_q16(c.sx, cq[q]), ay = mul_q16(c.sy, cq[q]);
        if (ax < 0 || ay < 0 || ax > 0xFFFFFFFFLL || ay > 0xFFFFFFFFLL) return false;
        tmp.x[q] = static_cast<uint32>(ax);
        tmp.y[q] = static_cast<uint32>(ay);
        gains[q].x = tmp.x[q];
        gains[q].y = tmp.y[q];
    }
    Profile p;
    if (!Profile::create(gains, p)) return false;  // existing range + angular-order rules
    out = tmp;
    if (profile) *profile = p;
    return true;
}
} // namespace

void make_identity_config(FilterConfig* config) noexcept {
    if (!config) return;
    config->enabled = 1;
    config->sx = config->sy = static_cast<int32_t>(unity_gain);
    config->c_ne = config->c_se = config->c_sw = config->c_nw = static_cast<int32_t>(unity_gain);
}

bool validate_config(const FilterConfig* config) noexcept {
    DirectionalAnchors scratch;
    return config && expand(*config, scratch, nullptr);
}

bool config_to_profile(const FilterConfig* config, Profile& output) noexcept {
    DirectionalAnchors scratch;
    return config && expand(*config, scratch, &output);
}
} // namespace directional

extern "C" {
void directional_config_identity(FilterConfig* config) { directional::make_identity_config(config); }

int directional_config_to_anchors(const FilterConfig* config, DirectionalAnchors* out) {
    if (!config || !out) return 0;
    DirectionalAnchors tmp;
    if (!directional::expand(*config, tmp, nullptr)) return 0;
    *out = tmp;
    return 1;
}

int directional_config_validate(const FilterConfig* config) {
    return directional::validate_config(config) ? 1 : 0;
}
}
