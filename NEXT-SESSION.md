# Next session: start here (written 2026-11-08 ~01:50)

Branch `ui-tiles` (from main; local, not pushed) became this tree when the repository moved to
`mgpu-scaling` on 2026-11-08. Read [plans/UI-TILES.md](plans/UI-TILES.md) part 2
and the TODO at the top of [SESSION-NOTES.md](SESSION-NOTES.md).

## State

- `main` and `test/mv-freeze-hud-bars` are merged (incl. single-GPU AutoHDR) and pushed (bcb1b92).
- `ui-tiles` has UI tiles part 2 **step 1, measurement only** (831717a): `UITiles.h/.cpp`,
  `UITilesCS.hlsl`, called at the top of `DX12SwapChain::Present`. On frames upscaled with FSR 4.1
  on the 9060, it compares the finished frame with `Upscaling::sceneBlitTarget` (the stretch the UI
  is drawn over) and logs `[UI-TILES]` every 10 s: 32x32 / 16x16 tiles with UI (avg, p50, p95,
  max), frames without UI, MB per frame that tiles-only would send vs the whole frame, pass time on
  the 3090. Build: `D:\fgh\MGPU-Scaling.zip` (0 warnings, shader checked with fxc). The user has it
  installed (game DLL dated 01:43).

## First in-game run (01:45-01:47): nothing measured

- The build loads and runs fine: 58.8-63.6 game fps, direct copy on, no errors.
- **No `[UI-TILES]` line at all**, because the user's `Data\MCM\Settings\MGPUScaling.ini` has
  `iUpscaleMethodPreference =2` (FSR 3 on the 3090, log: "not upscaling here"). The measurement
  only runs with `= 3` (FSR 4.1 on the display GPU); no frame is `frameUpscaled` otherwise.
  Not a bug in the code, but there is also no log line saying the measurement is idle. Consider
  adding a one-time INFO when second-GPU mode runs without upscale lanes.

## Second run (01:51-01:52, `iUpscaleMethodPreference = 3`): measurement works, the HUD is tiny

Only ~20 s of play after loading, two full 10 s windows; `tm` was not tried. Raw lines:

```
01:52:03 [UI-TILES] 387 upscaled frames | 32x32 tiles with UI: avg 74 p50 66 p95 78 max 8160 of 8160 | 16x16: avg 260 p95 254
         | UI pixels avg 49040 (65% of the tiles' area) | frames without UI 0 | would send avg 0.30 MB, max 31.89, whole frame 31.64 MB
         | pass on render GPU avg 0.131 ms max 0.776 ms
01:52:13 [UI-TILES] 492 upscaled frames | 32x32: avg 589 p50 68 p95 3772 max 3809 of 8160 (46.7% max) | 16x16: avg 2182 p95 14238
         | UI pixels avg 493740 (82%) | frames without UI 0 | would send avg 2.32 MB, max 14.89 | pass avg 0.127 ms max 0.140 ms
```

- Normal HUD: **~66-68 tiles of 8160 (0.8%) = ~0.3 MB instead of 31.6 MB per frame.** Big win if exact.
- Some frames had ~3800 tiles (47%): a large UI element for part of the second window (ask the
  user what was on screen; dialogue, a message box or the Pip-Boy fade would fit). One frame had
  all 8160 (first window: fade-in after loading, or a full-screen change). The plan's 15% budget
  + fallback to full frames covers these.
- Frames without UI: 0 everywhere; the HUD is never empty (crosshair, compass). **Exactness is
  still unproven**: run `tm` and check the count drops to ~0.
- 16x16 tiles barely help (0.32 vs 0.30 MB at the HUD; the HUD's tiles are 65-82% filled):
  stay with 32x32.
- Pass cost on the 3090: 0.13 ms avg. Fine.
- **The user saw lower fps with `= 3`**: 38.8 and 49.2 game fps in these two windows vs 58.8-63.6
  with `= 2` in the 01:46 run. Both windows are right after loading (the first had 344 native frames
  before upscaling started), so not a fair comparison yet; FSR 4.1 on the 9060 avg 4.5 ms, UI
  composite 0.4 ms, copy on 3090 7.7-7.9 ms. Earlier FSR 4.1 runs (00:02) reached 62.7-70.9. Needs a
  settled 60 s A/B at one spot, `= 2` vs `= 3`, before blaming the measurement pass (0.13 ms
  cannot explain 10+ fps).

## Third run (01:56-01:57, same settings, "for fun"): fps fine again

The user changed nothing, and fps was good: 55.8 -> 67.4 -> 70.9 -> 69.3 game fps (cap 71), copy
on the 3090 7.6 ms. So the low fps in the second run was not the setting (or not only).
**Possibly the mod sometimes starts up in a bad state; investigate later** (diff the 01:51 log
against this one: lane setup, swap-chain format, which GPU/queue things landed on, first
FSR4/bridge lines). The user's logs are overwritten every start; use `tools/save-runs.ps1`.

UI tiles in this run: a constant **5234 tiles (64%)** for whole windows, dropping in the last
window after a settings reload at 01:57:07 (MCM closed?). Probably a menu was open over the
game the whole time. Ask the user; and check that a static menu really differs from the stretch
everywhere it says (a full-screen dim behind menus would do it, and would correctly need the
fallback to full frames).

## Do next

1. Have the user set `iUpscaleMethodPreference = 3`, then run again: ~1-2 min normal play (walking,
   combat, dialogue, subtitles), then `tm` (HUD hidden) and stand still ~20 s.
2. Read the `[UI-TILES]` lines (first one at startup: "measuring which tiles ..."). Look for:
   - with `tm`: tiles ~0 and "frames without UI" ~all. If not ~0, something other than the UI
     writes the frame buffer after PostDisplay (ReShade effects, Community Shaders); then the
     compare method saves nothing and the user's alternative (redirect UI draws into their own
     transparent layer via the game's UI renderer) becomes the route. See the trade-offs below.
   - normal play: p95 / max tiles vs the 8160 total (3840x2160) and the 15% budget in the plan;
     32x32 vs 16x16 MB; pass time (should be well under 0.2 ms).
3. If exact and small: step 2 of the plan (bridge `SetLaneActive`, no behaviour change), then step 3.

## The two ways to get the UI (discussed with the user 2026-11-08)

- **Compare (the plan, built now):** exact copy of the finished frame's pixels; no extra game
  hooks; works with every blend mode and mod UI. Depends on nothing else touching the frame after
  PostDisplay.
- **UI into its own layer (user's idea, F4SE):** redirect the UI draws after PostDisplay into a
  cleared transparent target, send its non-empty tiles. Normal and additive blending give a
  correct premultiplied layer; multiply/subtract blends, UI shaders that read the scene behind
  them (menu blur), Pip-Boy / 3D menus do not. Needs hooks on the game's UI renderer (more
  fragile). Bonus: a real UI layer with alpha could also stop HUD smear in generated frames.
- Agreed order: measure first (step 1); its numbers matter for either route.

## Parked

- Why a deleted `Data\MCM\Settings\FrameGeneration.ini` lowered fps (RAM was already fixed; restoring
  the file fixed it). `MGPUScaling.dll` only reads it in `mgpu::MigrateSettings`. Suspects: an old
  FG plugin / the leftover `Data\F4SE\Plugins\FrameGeneration` folder, or MCM. Details in
  SESSION-NOTES TODO item 2.
