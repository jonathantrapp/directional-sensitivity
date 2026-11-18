#pragma once
// C-compatible convenience configuration. It is only an adapter: it expands to the
// existing direct-anchor profile (anchor_x[q] = sx*c_q, anchor_y[q] = sy*c_q).
// It is not the .dsp format, not the driver ABI, and is never sent to the driver.
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Coefficients are strictly positive signed Q16.16 (65536 = 1.0).
// Quadrant order: NE, SE, SW, NW.
// `enabled` is 0 or 1. The caller owns the struct; no function keeps a pointer.
typedef struct FilterConfig {
    int32_t enabled;
    int32_t sx, sy;
    int32_t c_ne, c_se, c_sw, c_nw;
} FilterConfig;

// Expanded unsigned Q16.16 anchors, indexed NE, SE, SW, NW.
typedef struct DirectionalAnchors {
    uint32_t x[4];
    uint32_t y[4];
} DirectionalAnchors;

// Fills unity gains, enabled = 1.
void directional_config_identity(FilterConfig* config);

// Computes anchors with int64 intermediates and round-half-away-from-zero, then
// checks them with the existing Profile validator (range + angular order).
// Returns 1 and fills *out on success; returns 0 and leaves *out untouched on any
// failure (null pointer, bad `enabled`, anchor outside uint32, rejected profile).
int directional_config_to_anchors(const FilterConfig* config, DirectionalAnchors* out);

// 1 if directional_config_to_anchors would succeed, else 0.
int directional_config_validate(const FilterConfig* config);

#ifdef __cplusplus
} // extern "C"

#include "directional/transform.h"
namespace directional {
// C++ wrappers over the C functions above. config_to_profile leaves `output`
// untouched on failure.
void make_identity_config(FilterConfig* config) noexcept;
bool validate_config(const FilterConfig* config) noexcept;
bool config_to_profile(const FilterConfig* config, Profile& output) noexcept;
}
#endif
