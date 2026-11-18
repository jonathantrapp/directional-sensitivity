# Personal directional model — 2026-10-23

## What we are building

Corsair describes PureVector as calibration of a player's directional sensitivity in Aimlabs, followed by a fixed profile stored on the mouse. Its guide describes five one-minute SpiderShot rounds and eight-direction results; collecting subsequent play is required before its after-calibration graph is populated. These are vendor descriptions, not independently verified benefit estimates. The public guides do not disclose the fitting formula or establish that the displayed eight directions are eight runtime parameters.

Sources: [Corsair calibration guide](https://www.corsair.com/ca/en/explorer/gamer/mice/how-to-use-purevector/), [Corsair technology description](https://www.corsair.com/ca/en/explorer/gamer/mice/purevector-the-future-of-aiming/).

Our software keeps your four cardinal directions fixed at 1.0, as you requested. That restriction is our design choice. We fit four linked diagonal gains; independent X/Y anchors remain supported by the core but are not estimated from this small data set. Raw Accel's [official guide](https://github.com/RawAccelOfficial/rawaccel/blob/master/doc/Guide.md?plain=1) also documents direction-dependent input scaling, but its acceleration/anisotropy formula is not the same as our speed-independent smoothstep curve. No proprietary formula was recovered, and nothing accesses mouse firmware.

## New run and source selection

Baseline 004 saved 48 completed trials (44 hits, 4 misses), not the full 96 scheduled trials. All 24 recorded diagonal trials passed the existing v2 candidate extraction. Partial sessions are valid data; this one can be evaluated as a later session because its counts per quadrant are adequate when earlier runs supply training data.

The fit uses all current v2-admissible diagonal prefixes from baseline 002 and baseline 003: 40 training trials. Baseline 004 supplies 24 later-session validation trials. These sessions match declared DPI (1600), reference sensitivity (1), pixel mapping (0.5), and selected device. Trial IDs are prefixed with session IDs to prevent accidental merging. Original within-session train/validation labels are deliberately superseded by this documented temporal split. Baseline 001 is excluded from fitting because it predates the pointer confinement fix; its window issue is an experimental confound. Cardinal trials and flagged traces remain available as diagnostics, not hidden or deleted.

The older data were already inspected during development. The later session is used to gate this model, so it is now validation data, not an untouched future confirmation test. The sessions were collected on the same day, with the same original target order. Transfer across days and games remains untested.

## Explainable model

For each packet, the existing processor computes `w = r²(3−2r)`, with `r = min(abs(x),abs(y))/max(abs(x),abs(y))`. A linked quadrant gain `c` gives ideal packet gain `1+w(c−1)`. Cardinal packets bypass unchanged, and integer residuals follow the tested fixed-point implementation.

For each training trace, we project target-minus-endpoint error onto its gain-sensitive movement. The per-quadrant median of those implied gains is bounded to [0.75, 1.25]. This is a robust four-parameter model, not a neural network. The data volume and segmentation uncertainty do not justify a much more flexible model. Existing coverage and replay gates are preserved; no threshold was reduced to force a candidate.

Validation replays the actual packets through the compiled fixed-point processor. The final combination is rechecked after any quadrant veto. A seeded 10,000-resample paired bootstrap gives a descriptive uncertainty interval. With only 5–8 validation traces per quadrant and post-validation selection, these are not confirmatory significance tests.

## Result

| Quadrant | Training | Later validation | Final linked gain | Relative endpoint RMSE before → after |
| --- | ---: | ---: | ---: | ---: |
| NE | 7 | 5 | 1.000 | 9.10% → 9.10% |
| SE | 13 | 5 | 1.000 | 6.49% → 6.49% |
| SW | 12 | 8 | 1.000 | 6.66% → 6.66% |
| NW | 8 | 6 | 1.0919 | 9.87% → 9.61% |

NW passes the existing practical replay gate, but its paired squared-error improvement interval is approximately [-0.00946, +0.00779] in normalized squared-distance units. It includes worse results. The observed reduction is only 0.26 percentage points of relative RMSE, about 2.7% relative—not a measured 2.7% improvement in human aim. The other proposed gains were vetoed.

The profile at `models/personal-v1/candidate.dsp` is therefore **experimental**, for controlled app-local testing. There is insufficient evidence to recommend changing your everyday sensitivity. The complete report, source hashes, scores, and fitting log are in the same directory. `score_profile` also replays every completed later-session trace, including cardinal targets and misses. Those counterfactual final endpoints are diagnostic: a person would change their corrections when the gain is actually active.

## Test the candidate against baseline

```powershell
.\build\Release\recorder.exe --dpi 1600 --sensitivity 1 --compare .\models\personal-v1\candidate.dsp --output .\sessions\comparison-001
```

The task randomly interleaves baseline and candidate trials with a fresh recorded seed. Every direction/distance cell has three trials per condition. The active condition is not displayed during the trial, although feel may reveal it. This is not a guaranteed blind experiment. Hold still briefly, press Space, flick, and click as before. Save when finished; partial sessions are retained but may be unbalanced.

Only the blue crosshair inside this task is transformed. The desktop pointer and system-wide input are not replaced. Raw packets remain unchanged in the recording; separate displayed endpoints determine hit/miss. The profile is fixed, never target-aware or adaptive during the trial. The driver remains uninstalled.

All outcomes count. Misses must not appear better merely because of fast clicks. Summarize with:

```powershell
python .\tools\compare_summary.py .\sessions\comparison-001 --output .\sessions\comparison-001-summary.json
```

The report includes hit rate, hit-only arm-to-click duration, and an explicit deadline-penalized score (hit duration, otherwise the five-second deadline). It keeps misses/timeouts in that score, lists technical interruptions separately, and reports each direction as well as matched target pairs. This score is not measured time to an eventual successful hit. NW is the only changed quadrant, so the aggregate result alone can hide its effect. Repeat on another day before adopting anything; the task cannot establish FPS transfer. [Aimlabs' setup guide](https://aimlabs.com/articles/aimlabs/how-to-configure-and-convert-your-sensitivity-in-aimlabs/) explains why game profile and sensitivity mapping also matter.

## Reproduce the offline fit

Use a new output directory each time:

```powershell
python .\tools\fit_personal.py --train .\sessions\baseline-002 .\sessions\baseline-003 --test .\sessions\baseline-004 --output .\models\personal-reproduction
```

The tool rejects mismatched setups, reused train/test sources, synthetic data, and comparison recordings. It hashes original files and verifies they remain unchanged. No profile is exported if the existing validation gate fails. The new scorer and comparison path pass automated checks for exact fixed-point output, cardinal identity, setup mismatch, per-trial state reset, condition balance, raw-data retention, displayed hit detection, and exclusion of comparison recordings from calibration input. Actual live comparison behavior still needs the human run above.
