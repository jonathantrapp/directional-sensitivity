# Comparison 001 and 002 — review

Reviewed 2026-10-23. Both used the same experimental profile: NW linked gain 71558/65536 (1.0918884); the other three quadrants remained unity. Conditions were randomized within each session. No profile change was made from these results.

## Capture and scoring checks

Comparison 001 saved 33 completed trials, leaving 63 unattempted. Comparison 002 saved all 96. Their 7,518 and 21,035 raw packets respectively reconcile with recorded packet counts, ordered sample indices/timestamps, raw endpoints, and displayed endpoints. No pre-paint movement, click-termination discrepancy, displayed-hit mismatch, processor replay mismatch, or replay overflow was found by the audit. Original files were hashed and verified unchanged.

The audit reconstructed each recorded profile from its manifest and replayed the actual raw stream through the compiled processor. Candidate trials were checked against transformed coordinates; baseline trials against identity coordinates. A difference between raw and displayed endpoints is expected on transformed trials and is not missing data.

## Results

| Run | Condition | Hits / attempts | Hit rate | Mean arm-to-click on hits |
| --- | --- | ---: | ---: | ---: |
| 001, partial | Baseline | 19 / 22 | 86.4% | 817.4 ms |
| 001, partial | Candidate | 11 / 11 | 100% | 747.9 ms |
| 002, complete | Baseline | 46 / 48 | 95.8% | 747.3 ms |
| 002, complete | Candidate | 43 / 48 | 89.6% | 788.6 ms |

The partial first run is unbalanced; its apparently better candidate total is not a matched comparison. Do not combine these totals into a claim of improvement. The complete second run descriptively favors baseline overall, but this small experiment does not establish that the NW gain caused the additional misses in other directions.

### Northwest specifically

In run 002, NW candidate trials hit 6/6 targets; baseline hit 5/6. Mean hit-only arm-to-click times were effectively identical: 760.7 versus 760.9 ms. With only six trials per condition, one baseline miss substantially changes the penalized score. The paired NW penalized-score improvement is 706.8 ms, with a descriptive bootstrap interval of approximately **−110.3 to +2145.2 ms**. It includes worse results; it is not evidence of a reliable 707 ms speed improvement. The metric penalizes any miss with the five-second deadline and is not actual time to eventual success.

Run 001 has only one complete matched NW pair; it favored baseline by 195.8 ms. A single pair cannot estimate variability, so its interval is deliberately unavailable rather than a misleading zero-width bootstrap interval.

Cardinal TARGETS can contain diagonal PACKETS. A few N/W/E targets therefore show small raw/displayed endpoint differences under the candidate. Pure cardinal packets still pass through exactly at 1.0; this invariant does not promise that every curved trajectory toward a cardinal target is unchanged.

**Decision:** keep everyday sensitivity unchanged. The candidate remains experimental. The data do not support strengthening the gain, fitting additional parameters, or applying it system-wide. The driver remains uninstalled.

## More than eight directions

The mouse transform already works continuously at every packet angle. Eight compass anchors describe the curve; the previous task's eight target locations were a sampling limitation, not a restriction on aiming direction.

Added opt-in 24-angle comparison coverage: target angles are spaced by 15 degrees, at the same two distances. One baseline and one candidate trial per exact angle/distance yields **96 total trials**, with 48 per condition. Every trial exports `angle_index` and `angle_degrees`; the manifest records `direction_count`. Pairing now uses exact angular cells rather than coarse compass labels, preventing collisions between adjacent angles. Coarse `direction` remains the nearest compass sector; in wide mode it is not a unique angle or the transform's input quadrant. Use the per-angle results to interpret the curve.

This increases test coverage without adding 24 free gain parameters or changing the core curve. Targets at intermediate angles exercise interpolation and transitions toward the cardinal baseline. Each individual angle still has few repetitions, so this is a broad exploratory check, not high-confidence per-angle calibration.

For a wider-angle comparison, choose a new output directory:

```powershell
.\build\Release\recorder.exe --dpi 1600 --sensitivity 1 --compare .\models\personal-v1\candidate.dsp --directions 24 --output .\sessions\comparison-003
```

Hold still before Space and aim naturally. Save all hits/misses. This affects only the app's crosshair. It does not establish Fortnite or diep.io transfer and does not install the driver. The existing eight-direction mode remains available for like-for-like repeat measurements; the wide mode is a different target distribution, so compare conditions within it rather than directly comparing its total score against the old runs.

## Evidence and verification

Machine-readable audits, source hashes, exact replay endpoints, per-direction matched results, and timing summaries are in analysis/comparison-001-002-final (not published). Input sources are the original `sessions/comparison-001` and `sessions/comparison-002` exports. The repeatable audit tool is `tools/audit_comparison.py`; it never rewrites those recordings.

All ten compiled checks pass. Tests cover the wider schedule's angle/radius geometry, cardinal controls, balanced exact-angle cells, complete export, and rejection of invalid mode combinations. All three model-tool Python tests pass, including adjacent angles that share a compass sector, miss-aware scoring, and train/test source separation. A synthetic 24-angle session produced 24 distinct angle groups and 48 matched target pairs. Live 24-angle behavior remains to be checked by the user.
