# Usage guide

## Build and test

With the Visual Studio C++ Build Tools, the Windows SDK and CMake installed, run this from PowerShell in the repo root:

```powershell
.\scripts\build.ps1
```

For other toolchains with CMake and a C++17 compiler:

```text
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Executables are under `build/Release` for Visual Studio. Single-configuration generators generally place them directly under `build`.

The build script uses the existing build cache; if `DIRECTIONAL_BUILD_DRIVER` was previously enabled, it also compiles the uninstalled driver. To configure an explicitly user-space-only build first, run:

```powershell
cmake -S . -B build -DDIRECTIONAL_BUILD_DRIVER=OFF
.\scripts\build.ps1
```

Compilation does not install or activate anything. Existing strict compiler warnings are retained; new C++ targets also use `/W4 /WX /EHsc` under MSVC.

## Replay input

```powershell
Get-Content .\examples\motions.csv | .\build\Release\replay.exe --ne 1.5 1.5
```

Input is one device's signed integer relative counts, `x,y` per line; blank lines and lines beginning with `#` are ignored. No header is expected. Positive Y points down. Output includes the input, transformed counts and an overflow-bypass indicator. Errors return exit code 2; output before an invalid row is partial and should be discarded if the process fails.

Options `--ne X Y`, `--se X Y`, `--sw X Y`, `--nw X Y` set diagonal gain pairs. Unspecified quadrants remain unity. Use equal X/Y values to preserve ideal movement heading; unequal values deliberately change heading. Gains must be within `[0.25,4]`, and independent gain pairs must also pass a conservative analytic angular-order condition. A rejected pair can be mathematically usable but outside this sufficient safety bound.

Examples with NE gains `1.5,1.5`:

| Input | Output | Reason |
| --- | --- | --- |
| `10,0` | `10,0` | Horizontal identity |
| `0,-10` | `0,-10` | Vertical identity |
| `10,-10` | `15,-15` | Full NE anchor gain |

Integer rounding carries fractions across diagonal packets. Cardinal and zero packets pass through exactly without releasing stored fractions. Consequently `(1,1)` can scale while consecutive `(1,0)` and `(0,1)` packets cannot: physical direction cannot always be inferred from individual packets.

## Saved profiles and configuration adapter

```powershell
Get-Content .\examples\motions.csv | .\build\Release\replay.exe --profile .\examples\manual.dsp --dpi 1600 --sensitivity 1
```

Profiles are versioned `key=value` files, not JSON. Required keys are `directional-profile=1`, `dpi`, `reference_sensitivity`, and `ne`, `se`, `sw`, `nw` gain pairs. See [manual.dsp](../examples/manual.dsp). All fields are validated; malformed files, duplicate/unknown keys, and angular-order failures are rejected without replacing the existing profile. The user-confirmed DPI/reference sensitivity must match; mismatches return exit code 3. Do not combine saved profiles with manual gain arguments.

`include/directional/config.h` exposes a C POD `FilterConfig` and `directional_config_*` functions, plus C++ wrappers. This convenience adapter expands `sx*c_q` and `sy*c_q` with int64 intermediates and Q16.16 rounding, then calls the same anchor validator. It does not replace `.dsp` files, change the driver ABI, or send itself to a driver. Its `enabled` member is validated metadata; callers still control `Processor::set_enabled`. A C translation unit verifies header compatibility and linkage.

## Synthetic replay and residual CSV

```powershell
.\build\Release\replay.exe --gen mixed --count 512 --csv-diag --ne 1.25 1.25 > .\build\diagnostics.csv
```

Choose `circle`, `flick`, `micro`, `cardinal`, `reversal`, or `mixed`. Generated input is deterministic; count defaults to 1,000,000 and is bounded to 1..10,000,000. `--count` requires generation or benchmarking. Circles sweep directions, flicks reach 32767 counts, micro packets exercise rounding, cardinal packets alternate axes, and reversals exercise carry resets. These streams are test inputs, not a model of a specific mouse.

`--csv-diag` works on generated or stdin input and emits exactly:

```text
raw_x,raw_y,out_x,out_y,res_x,res_y
```

Residuals are signed integer Q24 fractions: divide by 16777216 to get counts. They describe state **after** the row. Cardinal and zero rows freeze both values; transformed sign reversals clear the reversed axis first. An identity quadrant clears carry, and overflowing transformed packets return original counts and clear carry. Use normal replay output when you need the explicit `overflow_bypass` column.

## Benchmark

```powershell
.\build\Release\replay.exe --gen mixed --bench --ne 1.25 .875 --se 1.25 .875 --sw 1.25 .875 --nw 1.25 .875
.\build\Release\replay.exe --gen mixed --bench-packet --ne 1.25 .875 --se 1.25 .875 --sw 1.25 .875 --nw 1.25 .875
```

Both modes require at least 1,000,000 packets, default to `mixed`, and reject CSV output. Input generation, allocation, clock-overhead sampling, and warmup finish before the processing measurements. No input is injected or paced at a hardware polling rate.

- `--bench`: 1,000-packet batches; mean cost includes loop/checksum overhead. p50/p99/p99.9 are **batch-average** ns per packet, not individual-packet percentiles.
- `--bench-packet`: each packet is bracketed by `steady_clock` reads. Percentiles describe instrumented samples, including clock overhead. Empty clock-pair percentiles are reported separately and are not subtracted.

These measure CPU replay, not driver, USB, display, polling accuracy, or end-to-end latency. On the recorded local run, individual samples were largely 0/100 ns, and empty clock pairs had the same granularity. A zero sample means the clock could not resolve the interval, not zero processing time.

## Foreground live preview (manual test)

```powershell
.\build\Release\preview.exe --profile .\examples\manual.dsp --dpi 1600 --sensitivity 1
# Optional native-process batching:
.\build\Release\preview.exe --profile .\examples\manual.dsp --dpi 1600 --sensitivity 1 --buffered
```

The preview was compiled and its model/parser tested without opening its live window. A human must verify the following workflow:

1. Select the intended mouse from the dropdown; no mouse is selected automatically. Click Start.
2. Move that mouse with the window foreground. Compare original/transformed last packets, totals, report count, overflow count and residuals. The display refreshes around 30 Hz, separately from input processing.
3. Verify another mouse does not change the numbers. Stop and Start must clear carry and begin fresh totals.
4. Switch to another application: preview must pause. Return and click Start to resume. Desktop input should behave normally throughout.
5. Unplug the selected mouse: preview must stop. Use Refresh and select it explicitly after reconnecting.
6. Confirm absolute input is not scaled. Attribute changes stop the preview; restart the app after confirming setup. DPI changes are not reliably exposed by generic mice, so confirm them yourself.
7. Repeat with `--buffered` to check ordering under high report rates. WOW64 buffered mode is rejected; use native x64 or standard reads.

The window registers foreground mouse input only while running. It does not suppress legacy messages, inject movement, capture/warp the cursor, or contact the driver. It reads the current `WM_INPUT` before optional bounded buffered draining and always calls `DefWindowProcW` for the current event. Capacity/read errors stop/reset instead of allocating in the report handler. See [preview notes](preview.md) for architecture and API references.

## Guided baseline recorder

```powershell
.\build\Release\recorder.exe --dpi 1600 --sensitivity 1 --output .\sessions\baseline-001
```

The identity-only recorder guides 96 randomized target trials and exports full packet traces, misses and interruptions, per-trial SVG plots, and explicitly unreviewed first-flick candidates. Choose the mouse, click Start, then center the pointer and press Space before each trial. Save uses a new directory and ends the session. It does not recommend or apply gains. Automated checks and an offscreen render pass. See the [recorder guide](recorder.md) for controls, coordinate mapping, timing limits, export review, and remaining manual checks.

## Offline calibration

`calibrate trials.csv DPI REFERENCE_SENSITIVITY > candidate.dsp` fits linked diagonal gains from labeled initial-flick packet traces. The CSV header is `trial,split,target_x,target_y,dx,dy`; repeated trial IDs group ordered packets and must retain the same metadata. Split is `train` or `validation`; targets use identity-profile count units, not arbitrary screen pixels. At least six training and three validation trials per quadrant are required. The tool rejects reversals/corrections and cardinal-only evidence, bounds recommendations to [0.75,1.25], evaluates candidates through the actual processor, and leaves unhelped quadrants at unity. Exit code 3 means no candidate passed the validation gate; no profile is exported.

The estimator operates offline; the separate recorder above supplies a guided baseline task. Neither establishes improved human aiming. Review segmented candidates and validate any exported profile with fresh trials. Do not run `raw_capture` or `driver_ctl` without reading their source first.

## Experimental personal model and comparison

The [personal model report](personal-model.md) records refreshed PureVector research, training on earlier sessions, and validation on baseline 004. Only a northwest gain passed the existing replay gate, and its uncertainty remains too large to recommend everyday use. The candidate is for a randomized app-local comparison:

```powershell
.\build\Release\recorder.exe --dpi 1600 --sensitivity 1 --compare .\models\personal-v1\candidate.dsp --output .\sessions\comparison-001
```

The profile affects only the task's crosshair. Conditions are balanced and hidden during trials; raw packets and displayed endpoints are recorded separately. It does not install the driver or alter system-wide input. See the report for analysis, limits, and comparison scoring.

## Code map

Saved sessions can be reprocessed without overwriting their packets using `review_capture SAVED_SESSION NEW_REVIEW_DIRECTORY`. See the [recorder guide](recorder.md) for v2 jitter tolerance, fragment rejection, and review outputs.

- `include/directional/transform.h`: profile and per-device processor interface.
- `include/directional/config.h`, `core/config.cpp`: C-compatible factor-to-anchor adapter.
- `core/transform.cpp`: gain validation, smoothstep interpolation and integer quantization.
- `tools/replay.cpp`: user-mode CSV utility and benchmark.
- `tools/review_capture.cpp`: validated offline resegmentation of saved recorder CSVs.
- `tools/synthetic.h`: deterministic offline packet generation.
- `windows/preview.cpp`: foreground Win32 preview; `preview_input.h` contains its testable model and native-buffer parser.
- `windows/recorder.cpp`: guided foreground baseline recorder and offscreen render mode.
- `include/directional/capture_session.h`, `calibration/capture_session.cpp`: session lifecycle, target schedule, trace segmentation, and inspectable exports.
- `utility/profile_io.cpp`, `calibration/calibration.cpp`: persisted profiles and offline calibration.
- `tests/`: core behavior, independent floating-point reference, numeric limits and CLI checks.
- [Architecture](architecture.md): math, state rules, Windows integration and calibration plan.

`Processor` owns one device's residuals. It must be called serially; use `apply` for a validated profile, `set_enabled` for bypass, and `reset` on reconnect or other device lifecycle changes. It is not a thread-safe driver publication mechanism. See the header comments before integrating it.

The core requires no heap allocation, floating point, exceptions or RTTI on its packet path. Profile parsing and benchmarks run outside that path. The ideal angular-order guarantee applies to the continuous curve; integer packet output is necessarily quantized.
