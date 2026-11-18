# Guided baseline recorder

By default the recorder collects identity-only aiming traces for review. It does not install a driver or change mouse firmware. The optional `--compare PROFILE` mode applies a fixed profile to the task's crosshair on balanced randomized trials; see the [personal model and comparison guide](personal-model.md). The target task is our own design; it is not Corsair's calibration protocol or a reproduction of its undisclosed algorithm.

## Run a session

```powershell
.\scripts\build.ps1
.\build\Release\recorder.exe --dpi 1600 --sensitivity 1 --output .\sessions\baseline-001
```

Replace DPI and sensitivity with your confirmed setup. Reference sensitivity is metadata; it does not reproduce a game's camera mapping. `--pixels-per-count` defaults to `0.5` and controls this task's count-to-client-pixel mapping. Raw Input may already reflect an upstream filter: identity mode cannot certify that the device stream is untouched. Record any existing filter in your experiment notes.

The output directory must not exist. Save creates it and any missing parents, then ends the session. Use a new directory for each session.

1. Select the intended mouse explicitly and click Start.
2. Put the regular pointer over the center circle, hold it still for at least 300 ms, then press Space. If you press too early, wait for the Ready message and press Space again. The target appears and a virtual crosshair starts at the center. The stillness check is only before arm; every accepted packet after arm is retained.
3. Flick toward the orange target and left-click. Both hits and misses are retained.
4. Return the regular pointer to the center and press Space for the next trial.
5. Use Save for a full or partial session. Closing an unsaved session offers Save, Discard, or Cancel.

Escape/Pause cancels the current trial while keeping its trace. Losing focus pauses capture; Resume is explicit. During an armed trial the regular cursor is hidden and temporarily confined to the aiming area, so clicking a bottom target cannot click another application. Raw counts and the virtual crosshair remain unbounded. Confinement ends on completion, timeout, pause, focus loss, or close; Escape is the immediate release action. Moving the window interrupts a trial and releases the cursor. The application does not warp it. Device removal, attribute changes, unsupported absolute input, and display geometry changes require restarting with confirmed settings. Generic mouse APIs cannot reliably detect every DPI change. Cursor confinement and release use [Win32 ClipCursor](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-clipcursor).

## Targets and coordinates

For optional wider coverage in comparison mode, add `--directions 24`. This uses 15-degree target spacing and two repetitions per angle/distance, one per condition, for the same 96 total trials. The original eight-direction identity calibration schedule stays unchanged. The [comparison review](comparison-results.md) explains why the core already handles every angle and how exact-angle pairing is recorded.

The default schedule has 96 trials: eight directions, two distances, six repetitions. Each direction/distance cell has four training and two validation trials. A deterministic shuffle uses seed `0x51a7`. Interrupted trials remain in the record and are not replaced.

Near/far targets are approximately 128/224 client pixels from the center; rounding to integer counts makes the exported count target authoritative. The hit radius is 14 client pixels. Positive Y points down. The virtual position is the accumulated relative counts multiplied by pixels-per-count, plus the center origin. Display clamping does not alter recorded counts.

## Export and review

| Artifact | Contents |
| --- | --- |
| `session.json` | Setup, device identity, mapping, seed, limits, timestamp source, segmentation version, and completion counts. Written last; require valid JSON before treating an export as complete. |
| `packets.csv` | All accepted samples: trial ID, sample index, time, signed counts, button transition bits, and raw movement flags. Wheel magnitude and other HID payloads are not recorded. |
| `trials.csv` | Every scheduled trial, including misses, interruptions, and unattempted targets; endpoints, exposure, and segmentation decisions. Target pixel coordinates are offsets from the center. |
| `plots/trial_N.svg` | Attempted full trace in gray, candidate prefix in blue, target in orange, endpoint in black. Plot scale fits each trace. |
| `candidate-trials.csv` | Eligible prefixes in the existing offline estimator's input format. These are unreviewed heuristic candidates. |

Trial IDs are local to one session. Do not concatenate session files without assigning distinct IDs and preserving metadata.

Mixed-packet exposure is measured over the full relative trace, including movement toward cardinal targets and rejected trials. Weighted path length uses the directional smoothstep weight. It measures potential exposure to a future gain, not applied correction: this baseline uses unity throughout. Absolute/attribute-change samples are archived but not accumulated as relative movement.

The v2 first-flick heuristic uses cumulative progress along the target direction, not individual packet signs. It preserves the original contiguous prefix, including tiny starting movements. Four counts of forward progress establish onset. Four counts of backward travel from the greatest forward progress end the flick, rolling back the correction to that peak. A movement arrival gap of at least 60 ms after onset also ends it. Opposite-axis packets alone do not establish a correction: ordinary curved motion can change either coordinate without moving away from the target.

A prefix must reach at least 25% of the target distance along its direction before it can qualify, so a tiny fragment cannot masquerade as an initial flick. These are explicit heuristic thresholds, not measured human-motion constants or Corsair behavior. They are shared with the offline estimator's reversal tolerance; the estimator fits target-quadrant exposure and checks the final combined profile by replaying actual packets. Full traces retain other quadrants and starting jitter.

Significant backward starts, insufficient mixed movement, movement before the first target paint, and interrupted trials remain ineligible. A miss can have an eligible prefix. Full traces are never replaced by segmented traces. Review the SVGs and exclusion reasons, including whether exclusions differ by direction or error severity. Later aiming comparisons must retain all outcomes, not just eligible calibration prefixes.

### Reprocess a saved session

```powershell
.\build\Release\review_capture.exe .\sessions\baseline-002 .\sessions\baseline-002-new-review
```

Use a new output directory. The command validates packet order, counts, timestamps, and reconstructed endpoints. It writes `review.csv` (one decision for every trial), `candidate-trials.csv`, per-trial SVGs, `review.json` with the segmentation version, and an unchanged copy of the original manifest as `source-session.json`. It never rewrites the original session. Candidates remain unreviewed; higher retention is not proof that segmentation or a gain is correct.

The completed baseline 002 was reprocessed into `sessions/baseline-002-review-final`: 36 of its 48 diagonal trials qualify, compared with 15 under v1. The remaining 48 trials are cardinal diagnostics. Twelve diagonal trials have significant backward starts. NE has 5 training/1 validation candidates and NW has 4/3; SE has 8/4 and SW has 7/4. The unchanged minimum of 6 training and 3 validation candidates per quadrant still prevents profile export. No threshold was relaxed simply to force a profile through that gate.

### Review every movement phase

New exports also contain `timing.csv`: an independent speed-based diagnostic with raw-count units, explicit offsets, and multiple-epoch flags. The Aimlabs inspection report explains its source evidence and limits. It does not change fitting eligibility. Comparison summaries include these timing diagnostics when available; pre/post intervals are not known human reaction/correction latencies.

`sessions/baseline-002-phases/index.html` provides a local review page with all 96 trials and filters for fitting candidates, flagged diagonal starts, and cardinal diagnostics. Open it in your usual browser. SVGs show pre-onset movement in brown, a possible main movement in green, and later movement in purple. Later movement may include correction; the colors do not establish human intent.

`phases.csv` retains the detected starting position, ending position, original packet boundaries, and target remaining from that starting position. `packet-phases.csv` labels every original sample, including those outside any fitting candidate. Main movement is detected as the first four-count forward departure from the preceding lowest progress along the original target direction; a 60 ms pause or four-count retreat ends it. The selected endpoint is the maximum forward progress before that boundary. This is versioned descriptive analysis and also covers cardinal targets.

Crucially, the later stroke in a flagged trial is not moved to the origin or silently added to fitting input. Its nonzero start offset remains explicit. All 96 trials receive a main-movement annotation in this particular recording, while fitting eligibility remains 36. Existing profile validation and the minimum-coverage gate are unchanged.

The phase annotation tests cover offset preservation, correction boundaries, cardinal diagnostics, interrupted/unsupported inputs, and avoiding automatic promotion to fitting. The export contract checks that every synthetic trial and packet is retained. The local page was checked structurally; an automated browser visual check was unavailable because local-file navigation was blocked. Live ready-cue behavior still needs a human check.

For the next manual check, use a new session directory, try 5–10 trials, and Save early. Confirm that moving into the center and immediately pressing Space does not arm, that waiting briefly does arm, and that Escape releases the pointer. Confirm bottom targets remain clickable if one appears. A fresh full 96-trial run is not required for this check.

## Timing and input handling

Live timestamps use QPC at `WM_INPUT` dispatch. Packet, end, and first-paint times are relative to trial arm; the arm time is relative to the application clock. The first-paint marker is taken after painting, not at display presentation. These are not sensor timestamps or end-to-end latency measurements. Message backlog and painting can affect apparent pauses.

Registration is foreground-only. The report handler writes to a preallocated buffer without per-report allocation or file I/O; the UI later copies completed traces and exports them. Defaults are 65,536 samples per trial and a five-second deadline. These are project choices. Buffer exhaustion and read/clock errors end a trial explicitly.

## Verification and remaining manual work

All nine automated checks pass, including schedule balance, lifecycle errors, segmentation, exposure, overflow-related geometry checks, and estimator compatibility. A synthetic session exported 96 trial rows, 192 sample rows, 48 candidate trials, and 96 SVGs. Its JSON and an SVG parsed successfully. The offscreen synthetic UI render was visually inspected.

No live recorder, preview, raw capture, or driver-control session was launched during implementation. A human still needs to verify device selection, actual report delivery, cursor/target behavior, focus/resume, reconnects, and timestamp behavior. Follow the preview's manual checklist as well. The protected driver sources are unchanged and the driver remains uninstalled.

Automatic recommendations, randomized baseline-versus-candidate comparisons, later-day validation, FPS camera mapping, and kernel integration remain later work.
