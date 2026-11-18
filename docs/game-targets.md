# Target games and Aimlabs investigation

Updated 2026-10-23 from the user's stated settings.

| Setting | Recorded value | Status |
| --- | --- | --- |
| Main game | Fortnite | User confirmed |
| Fortnite mouse X / Y sensitivity | 9.4 / 9.4 | User confirmed; preserve the game's percentage convention |
| Fortnite FOV | “max I believe” | Exact numeric minimum/maximum and game mode unconfirmed |
| Additional game | diep.io | User confirmed |
| Mouse DPI | 1600 in all recorder sessions | Session metadata; not independently read from hardware |
| Recorder reference sensitivity | 1 | Task metadata, not a conversion of Fortnite's 9.4 setting |

The existing NW 1.0919 candidate was fitted to a 2D count-to-pixel task. It is not a Fortnite sensitivity recommendation. Retain the original data/model as a separate experiment rather than relabeling it game-matched.

## What Aimlabs adds

[Aimlabs' sensitivity setup guide](https://aimlabs.com/articles/aimlabs/how-to-configure-and-convert-your-sensitivity-in-aimlabs/) documents game profiles, in-game sensitivity, DPI, FOV, and cm/360 verification. Its [Fortnite Aim Basics Routine](https://aimlabs.com/articles/aimlabs/win-more-box-fights-with-the-fortnite-aim-basics-routine/) includes third-person flicking, tracking, switching, and game-specific tasks. These are useful references for Fortnite testing. A center-out 2D click task alone does not establish tracking skill or performance in Fortnite.

Do not assume “max FOV” is a numeric value or that all Fortnite camera states use it. [Epic's current help page](https://www.epicgames.com/help/c-34254770/c-37371353/a17762816?lang=en-US) identifies separate FOV Minimum and Maximum sliders. Confirm the displayed values and resolution/aspect ratio before attempting a matched camera projection. Verify Aimlabs' Fortnite sensitivity display convention before entering 9.4; do not silently convert it to 0.094 or reuse the recorder's metadata value of 1.

For diep.io, start with a separate cursor-aiming evaluation. Measure the actual browser cursor mapping and screen geometry; do not treat an FPS cm/360 conversion as its sensitivity mapping. Tracking and moving-target tests should supplement static flicks. Neither game's candidate should be adopted just because it fits the other game's data.

## Focused reverse-engineering questions

The REA tools are advertised in this session and the reverse-engineer-anything skill was consulted. No Aimlabs binary has been opened or decompiled. Bounded installation discovery found no Aimlabs app manifest (Steam app 714010) or matching common-directory name in the four configured Steam libraries: C:, D:, E:, and F:. This does not exclude an installation elsewhere.

Once Aimlabs is installed, identify its exact build and relevant local artifacts. Begin with documented settings and normal task results. Use static REA inspection if an important question remains: sensitivity representation/conversion, local task/result/replay schemas, available directional metrics, or whether the PureVector component is shipped locally. Inspect only that feature's files and record exact evidence and unknowns. A normal Aimlabs install is not evidence that Corsair's calibration formula or a compatible PureVector workflow is accessible.

We do not need to reverse engineer the entire application. Recovering software behavior, generating a candidate profile, applying that profile in a game, and demonstrating improved human aim are separate milestones. The current app-local comparison can test the first candidate inside our task; it cannot apply that profile inside Aimlabs or either game while the driver remains uninstalled.

## Next human step

Install Aimlabs through Steam if it is not installed elsewhere, open it once, and report when it is ready (or provide the existing installation folder). Confirm the actual Fortnite FOV slider numbers and resolution when convenient. Keep current Fortnite sensitivity unchanged while the mapping is investigated. No firmware or driver installation is requested.
