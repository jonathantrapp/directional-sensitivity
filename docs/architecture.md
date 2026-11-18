# Directional sensitivity: design proposal

Status: core/replay milestone implemented and tested, 2026-10-21. Calibration and live Windows interception remain planned. Proposed driver/calibration sections below are not implemented behavior.

## Reference behavior and limits

Corsair PureVector measures aiming performance by direction, calculates a personal sensitivity profile, and saves it to mouse firmware. Its guide describes five one-minute SpiderShot calibration rounds and results displayed in eight directions. The public material reviewed does not disclose its runtime interpolation, gain calculation, or calibration estimator. Eight displayed directions do not prove eight runtime control points.

Sources:
- https://www.corsair.com/us/en/explorer/gamer/mice/purevector-the-future-of-aiming/
- https://www.corsair.com/eu/cs/explorer/gamer/mice/how-to-use/

This project's goal is a software implementation of personalized directional sensitivity. No firmware access is planned. Exact mathematical parity with PureVector remains unverified. Fixed cardinal sensitivity is a user requirement, not a documented PureVector property.

## Behavioral contract

- Inputs are relative integer movement counts, not desktop cursor positions.
- Eight compass anchors: E, SE, S, SW, W, NW, N, NE. Windows positive Y points down.
- Four cardinal anchors are fixed at unity; four diagonal anchors each store a positive X/Y gain pair.
- Link X/Y gains by default for heading-preserving scaling; allow independent gains for deliberate heading changes.
- Zero packets and cardinal packets pass through exactly, without consuming residual fractions from earlier diagonal packets.
- The first version classifies each packet independently and introduces no direction-estimation window.
- A physical diagonal can arrive as alternating cardinal packets. Such packets remain unchanged. This limitation follows from the strict packet-level unity contract.
- Profiles remain fixed during play. Calibration updates them only through explicit application of a new profile.

## Proposed interpolation

For a non-cardinal input (x,y), select its quadrant's diagonal anchor (a,b) using the original signs. Compute:

    r = min(|x|, |y|) / max(|x|, |y|)
    w = r*r*(3 - 2*r)
    gx = 1 + w*(a - 1)
    gy = 1 + w*(b - 1)
    ideal_x = x*gx
    ideal_y = y*gy

This is our ratio-based smoothstep interpolation, not recovered Corsair math. It uses no trigonometry and is independent of speed for a fixed input direction. At the diagonal r=1, so the anchor gains apply in full. At a cardinal direction the packet bypasses the calculation entirely.

Example: NE anchor (1.2,1.2) maps (10,-10) to ideal (12,-12), preserving heading; (1.2,0.9) maps it to (12,-9), changing heading. Horizontal (10,0) always remains (10,0).

Store the resulting gain pairs rather than redundant global X/Y factors multiplied by four quadrant coefficients. A UI may provide convenience controls that compile into these pairs. Unconditional global axis scaling conflicts with exact cardinal unity.

## Quantization and numeric rules

Use an allocation-free bounded fixed-point implementation with wide intermediates, compared against a high-precision reference. Choose precision and gain bounds only after establishing count ranges and worst-case products. Widen before absolute value so the minimum signed input is handled safely.

Keep fractional output residuals per device and per axis. Cardinal packets bypass quantization and leave residuals untouched; zero inputs never emit stored fractions. On a transformed axis sign reversal, clear that axis's old residual to avoid releasing fractions from the preceding motion. Clear both residuals on profile application, enable/disable, device removal, and reconnect. These resets intentionally discard sub-count fractions; document and measure that tradeoff.

Implemented overflow policy: if the rounded transformed movement cannot fit signed int32 destination fields, pass the original movement through and reset residual state for that packet. Do not wrap or leave clipped counts in residuals. The result reports the bypass to callers.

The implementation stores anchors as Q16.16 and computes ratio, weights, effective gains and residuals as Q24. Intermediate products remain below 2^58. Supported anchor values are [0.25,4]. The sufficient angular-order test is `9*min(1,a)*min(1,b) > 8*abs(a-b)`; it bounds the derivative on both sides of a diagonal using `r*w'(r) <= 8/9`. This conservative condition may reject usable settings. It guarantees the continuous ideal curve's order, not strictly increasing quantized integer headings. Identity quadrants also bypass and clear residuals, so unity profile regions cannot emit old diagonal fractions.

Reject invalid coefficients and configurations with unintended angular folds. Positive gains alone do not prove that increasing input angle produces increasing output angle. Gain bounds and angular validation need a justified implementation, not only a coarse visual plot.

## Module boundaries

| Module | Responsibility |
| --- | --- |
| core/transform | Packet classification, interpolation, ideal scaled counts |
| core/quantize | Integer output and residual lifecycle |
| core/config | Validated immutable runtime profile and versioned data types |
| tools/replay | Synthetic and recorded packet replay, reference comparisons |
| tests | Behavior, numeric edge cases, transitions and profile application |
| bench | Core processing cost, separate from end-to-end input latency |
| calibration | Collect labeled target/movement observations and propose bounded gains |
| utility | Device selection, profiles, explicit apply/reset, configuration CLI |
| driver | Windows mouse-stack adapter, packet forwarding, control interface |

Use C++ for a small platform-independent math core without runtime allocation, exceptions, or RTTI dependencies in the input path. The Windows adapter can expose a C-compatible boundary. Avoid a GUI until the behavior and driver interface are stable.

## Windows integration

    selected mouse -> filter -> transform/quantize -> normal Windows delivery
    utility -> validated versioned profile -> driver-safe publication

Raw Input supports observation and calibration; it does not replace other applications' physical input stream. A mouse-stack filter is the intended system-wide adapter. Microsoft Moufiltr is an architectural reference, not a production-ready USB deployment package.

References:
- https://learn.microsoft.com/en-us/windows/win32/inputdev/about-raw-input
- https://github.com/microsoft/Windows-driver-samples/blob/main/input/moufiltr/moufiltr.c

Transform supported relative movement only. Preserve buttons, wheels, metadata, ordering, and absolute packets. Maintain state by actual device lifetime; define reset behavior for suspend/resume as well as reconnect. Handle callback concurrency and downstream consumption explicitly; retrying a partially consumed batch must not transform or advance residuals twice. Use nonpaged state, no blocking work or per-packet logging, and a complete validated configuration snapshot with safe lifetime management. Reset state at the same boundary as the configuration change. Validate where the filter output reaches raw-input consumers and how downstream Windows pointer settings affect desktop motion; a unity filter does not establish unity for the entire desktop pointer pipeline.

Signing, installation, recovery, hardware compatibility, and application acceptance are separate deployment validation. Fast core arithmetic does not establish end-to-end latency or compatibility.

## Calibration plan

Manual profiles establish the transform first; they are not the finished personalized calibration feature. Add calibration as a separate module using measured target direction, initial flick displacement, signed distance error, correction behavior, and timing. Preserve observations and DPI/game sensitivity metadata. Derive bounded candidate gains under explicit assumptions, then compare them with baseline on held-out trials. Do not infer gains from hit rate alone or assume Corsair's five-round workflow validates our estimator.

The first calibration version should recommend linked gains. Keep independent-axis recommendations manual until there is enough evidence to distinguish heading error from distance error. Cardinal observations remain useful for measurement, even though this project's cardinal gains stay fixed. After a known DPI or reference-sensitivity change, mark calibration stale and use identity until the user explicitly selects a matching profile or recalibrates. Generic mice may not expose DPI changes; document that automatic detection is not guaranteed.

## Implementation sequence and acceptance

1. Implement the documented transform and replay harness. Verify default identity, cardinal identity after diagonal residuals, diagonal anchors, boundary continuity, reversals, zero packets, profile resets, extreme counts, output overflow, and unequal gains. Compare mixed-axis and alternating-cardinal representations explicitly; their difference is a documented limitation, not a promised invariant.
2. Benchmark at representative report rates through 8 kHz and record processing distributions without claiming end-to-end latency. Validate angular ordering over the allowed configuration domain.
3. Implement the calibration capture/estimation module and compare baseline against recommended profiles on held-out trials.
4. Integrate the Windows filter and configuration utility. Validate packet metadata, device isolation, reconnect, batch consumption, concurrent updates, and bypass behavior.
5. Validate deployment and end-to-end operation on actual hardware.

## Documentation conventions

Keep a short README with build/run examples, this architecture note for formulas and decisions, and comments explaining units, coordinate signs, interpolation, residual resets, and overflow behavior beside their implementation. Document ownership and concurrency at the driver boundary. Avoid comments that simply restate the next line of code.
