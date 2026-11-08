## Update — third session (Claude, 2026-11-06 ~17:00), read this first

### The 3090 was not the limit at 65–70 base; the chain was

The second session concluded "the 3090's own render time is the ceiling". That is not supported:
nothing measured the 3090's render time, and the comparison it used (chain to list end 13.3 ms vs
the ~14.5 ms frame) is the wrong one. FFX's `Present` waits for two later events (FFX v1.1.4,
`FrameInterpolationSwapchainDX12.cpp`): at entry for the previous frame's interpolation to finish
(`interpolationEvent`, line 1208), at exit for the replacement back buffer of frame N-1, which is
released only after N-1's real frame has been composited on the present queue (signal at line
347, wait at line 1246). So the render thread waits until ~18.5 ms after Present(N-1):

- block = L - T = 18.5 - 14.3 ˜ 4.2 ms (measured 4.18–5.22), and
- period ˜ (C + L) / 2 = (10.1 + 18.5) / 2 = 14.3 ms ? 69.9 fps (measured 69.8),

with C = render-thread time outside Present (game CPU + HighFPSPhysicsFix's limiter sleep; the 71
cap is `InGameFPS=71.0`, `FramerateLimitMode=0` in `HighFPSPhysicsFix.ini`). Lowering graphics
settings cannot move L: it is PCIe volume plus FSR3 work on the 9060. 71 needs L = 2T - C ˜ 18 ms.

**Confirmed by test (17:09 run, `bSendHudlessFrame = 0`, 4K):** 71.0 base / 142.0 presented,
50% generated; FFX Present block 0.44 ms avg (3.98 max); list end 7.8 ms after Present (was 13.3);
max lag 1. One steady 10 s window. Once the block disappears, the limiter's sleep (before Present)
gives the 3090 a head start: the copies start 6.2 ms *before* Present and the finished frame is
copied ~1.7 ms after it, so the 3090 has spare time per frame.

### Fourth change: `iHalfResDepthMV` (built 17:31; mode 1 at the edge of 71, mode 2 holds 71/142)

Keeps the HUD-less frame and cuts the payload instead. Why depth is the one to halve: FSR3 FG
works at `renderSize` = the depth resolution (`ffx_frameinterpolation_reconstruct_dilated_velocity_
and_previous_depth.h`: one dilated MV per depth pixel). With half-res depth and full-res MVs FSR3
would read 1 of 4 MVs; half-res MVs with full-res depth is not a layout it accepts. Hence:

- `0` full resolution (old behaviour).
- `1` (default) depth at half res; `DepthFromHalfResCS.hlsl` rebuilds it to full res on the 9060
  (bilinear where the 2×2 neighbours agree within 5% in 1-depth ˜ 1/distance, the own block's
  sample at edges). FSR3 stays at full resolution and reads every MV. 102.8 MB/frame at 4K.
- `2` depth and MVs at half res (MV of each block's nearest-depth sample); `FidelityFX.cpp` passes
  half `renderSize`, `motionVectorScale` = that size (FO4 MVs are UV units), jitter × 0.5.
  79.1 MB/frame.

Code: `HalfResDepthMVCS.hlsl` replaces `GenerateSharedBuffersCS` / the no-reticle copy in half modes
(one thread per 2×2 block; same reads, fewer writes; nearest = smallest depth, FO4 depth is 0 = near).
`Upscaling::SetupHalfResDepthMV` latches `halfResMode` at resource creation (restart to change) and
falls back to 0 on any compile/setup failure. `CrossAdapterBridge::CreateDepthUpsampler /
SetDepthUpsampleLane / UpsampledDepth / RecordDepthUpsample`; async path: copy queue B signals
`upsampler.sourceReadyFence` after the depth lane, and `Unpack` queues the rebuild on B's direct
queue *before* its `Wait(unpackFenceB)`, so it overlaps the remaining lanes. Timestamps: display
timer marks 3/4; new log line `depth rebuild on display GPU avg … | done … ms after Present`.
`FidelityFX.cpp` is now in the overlay (it was upstream-only).

Build (17:31): MSVC `/W4 /WX`, 0 warnings, DLL 1,131,008 B; all five shader variants compile with
`fxc /Ges`; `fg-gpu1.patch` regenerated and applies cleanly to a pristine `3e237a4`.
Expected: mode 1 cuts ~1.9 ms of copy (needed ~0.5 ms), mode 2 ~3.9 ms.

**Measured (4K, HUD-less on, low test settings; mode 1 = 17:33 run, one steady window 17:34:02;
mode 2 = 17:37 run, three steady windows 17:38:26–17:38:46):**

| | original (16:49) | HUD-less off (17:09) | mode 1 (17:33) | mode 2 (17:37) |
|---|---|---|---|---|
| base / presented | 69.8 / 137.1 | 71.0 / 142.0 | 70.2 / 140.4 | **71.0 / 141.9–142.0** |
| payload / copy on 3090 | 126.6 MB / 10.23 ms | 94.9 MB / 7.63 ms | 102.8 MB / 8.45 ms | 79.1 MB / 6.34–6.36 ms |
| 9060 list end after Present | 13.3 ms | 7.8 ms | 10.0 ms | 6.6–6.9 ms |
| FFX Present block avg / max | 4.18 / 13.6 ms | 0.44 / 3.98 ms | 1.71 / 8.80 ms | 0.42–0.50 / 1.14–3.37 ms |

Mode 2: max lag 1, direct list 0.03 ms (FSR3's prepare at half res), no visual check yet (the test
settings are too low to judge). Its list end is earlier than the HUD-less-off test, so it has the
most margin of any configuration measured.

Observed in both runs: the 9060's unpack starts ~0.4–1.0 ms *after* Present even though the copies
start 3–6 ms before it, because `Unpack` (copy queue B) is only submitted from `Present`. The
early lanes have landed but are not read until then. Submitting each group's read at its own hook
(world / HUD-less) would leave only the finished frame's read after Present; estimated ~1 ms off
the chain, which may be what mode 1 needs for a clean 71.

- The depth rebuild costs 0.06 ms avg (0.66 max) and is done 1.8 ms after Present, against an
  unpack end of 9.9 ms: fully hidden, as intended. Copy time matches the estimate (8.45 vs ~8.5).
- No visible artefacts reported by the user at 4K in mode 1.
- 70.2 is not yet a clean 71: L ˜ 10.0 + ~5 ˜ 15 ms is just above one period, so a ~1–2 ms block
  remains and the limiter's slack only absorbs it when the frame's CPU time is low. One window is
  too little to tell steady-state from settling (the window before it had loading hitches).

Next: rerun modes 1 and 2 at the settings actually played (heavier 3090 work makes the lanes final
later relative to Present, which lengthens the chain) and compare moving silhouettes in generated
frames.

### Fifth change: early unpack (`bEarlyUnpack = 1`, default) — built 17:46; mode 1 now holds 71/142

**Measured (17:48 run, mode 1 + early unpack, HUD-less on, 4K, low test settings; three steady
windows 17:49:13–17:49:33):** 71.0 base / 142.0 presented, 50% generated, max lag 1, cpu waits 0.

| | mode 1 (17:33) | mode 1 + early unpack (17:48) | mode 2 (17:37) |
|---|---|---|---|
| base / presented | 70.2 / 140.4 | **71.0 / 142.0** (3 windows) | 71.0 / 141.9–142.0 |
| 3090 copy (timeline) | -3.2 – 6.2 | -7.1 – 3.3 … -4.6 – 4.7 | -5.8 – 3.0 … -4.4 – 3.6 |
| 9060 unpack (timeline) | 1.0 – 9.9 | **-6.4 – 5.9 … -3.9 – 7.3** | 0.4 – 6.8 |
| 9060 list end after Present | 10.0 ms | 6.0–7.4 ms | 6.6–6.9 ms |
| FFX Present block avg / max | 1.71 / 8.80 | 0.53–0.66 / 4.1–5.6 | 0.42–0.50 / 1.1–3.4 |
| depth rebuild done after Present | 1.8 ms | 0.4–0.8 ms | — |

The 9060 now starts reading 4–6 ms before Present. The chain shortened by ~3 ms, more than the
~1 ms estimate: once the block is gone the limiter's sleep gives the 3090 a head start, so the
copies (and now the reads) start earlier too — the same tipping effect seen in the HUD-less-off
test. Mode 1 with the UI-free frame meets the 71/142 goal at 4K; mode 2 + early unpack (not run)
should have still more margin and is the reserve for heavier settings or CPU-heavy areas.

Build: 0 warnings (`/W4 /WX`), DLL 1,132,032 B, patch regenerated and applies to `3e237a4`. The zip
ships `iHalfResDepthMV = 1`; the 17:37 mode 2 test set 2 in the installed settings.ini, which a
reinstall overwrites.

The option above. `DX12SwapChain::TransferEarlyLanes` calls `CrossAdapterBridge::UnpackGroup` right
after `TransferGroup`, so B's copy queue gets the read of the world lanes at `PostAlpha` and of the
HUD-less lane at `PostDisplay`; `Unpack` at Present reads only what is left (normally the finished
frame), then signals consumed/unpack and queues the depth rebuild and the direct-queue wait as
before.

- `BeginUnpack` (first read of a frame, at a hook or at Present): CPU-waits the slot's previous
  unpack, resets the allocator, queues `Wait(displayDoneFenceB)`, and now sets `slotUnpack[slot]`
  up front. If an earlier frame's read was started but its Present never came (slot changed
  mid-frame), it signals consumed/unpack for that frame so nothing waits for it forever; `Shutdown`
  does the same.
- `SubmitUnpackLanes`: one list per lane, `Wait(transferFenceB, laneValue)`, `laneUnpackFrame[lane]`.
- Safety of reading early: B overwrites slot s's mirrors for frame N only after Present(N-1)
  returned on the CPU, which already waited for frame N-2's interpolation (FFX entry wait), so the
  HUD-less mirror is not still in use by FFX's interpolation queue — same guarantee as before.
- `timerCopyB` now has two marks per phase (6), like `timerCopyA`; both use one `sumPhases` helper,
  so `unpack` stays busy time and the timeline's `9060 unpack` start is negative with early unpack.
- `bEarlyUnpack` is re-read every ~60 frames (live A/B possible); needs `bAsyncUnpack` + `bEarlyTransfer`.

Expected: the 9060 finishes the early lanes before Present; after Present only ~2.6 ms of reading
(the finished frame) after it lands, so mode 1's list end ~10.0 ? ~9 ms and mode 2's ~6.7 ? ~5.6 ms.

### MCM menu (built 17:55)

Every setting the plugin reads is now on the MCM page (`overlay/package/MCM/Config/FrameGeneration/
config.json`, previously upstream-only): a new "SECOND GPU (FG-GPU1)" section with
`bFrameGenOnSecondaryGPU`, `sSecondaryGPUName` (textinput / ModSettingString), `bSendHudlessFrame`,
`iHalfResDepthMV` (stepper), `bAsyncUnpack`, `bEarlyTransfer`, `bEarlyUnpack`. Labels end in
"(restart)" where the value is latched at startup: iFrameGenType, iFrameGenFrames,
bFrameGenOnSecondaryGPU, sSecondaryGPUName, bSendHudlessFrame, iHalfResDepthMV, bAsyncUnpack,
bEnableDebugLogging. Live (re-read every 60 frames): bFrameGenerationMode, bFrameLimitMode,
bDisableInMenus, bEarlyTransfer, bEarlyUnpack. The legacy `bTransportHudLess` alias is not shown.

Fix found while checking this: `PostDisplay` read `settings.sendHudlessFrame` every frame, so
switching it off mid-game would stop filling the HUD-less lane while it was still registered and
FSR3 would interpolate from a cleared (black) UI-free frame until restart. Now latched as
`Upscaling::hudlessActive` at resource creation. The ini comment for `bEarlyTransfer` said
"requires restart"; it is live and now says so.

## Update — second session (Deepseek, 2026-11-06), read this before "Why 4K is capped"

(Written by Deepseek; its diagnosis of the remaining limit is corrected in the third session above.)

### Implemented, built and measured (MSVC build 16:15, first run 16:19 — see below)

To-do #1 below, plus two things it needed:

1. **Async unpack** (`bAsyncUnpack = 1`, default): the 9060 reads the staging buffers on its
   own **copy queue** (`copyQueueB`, per-slot allocators, `unpackFenceB`). The direct (FFX game)
   queue only `Wait`s for `unpackFenceB`; `consumed` is now signalled by the copy queue right
   after the read. `bAsyncUnpack = 0` keeps the original serial path for A/B.
2. **Lane-by-lane streaming**: the 3090 now submits one copy list per lane and signals the
   transfer fence after each (`value = frame * 8 + lane`). The 9060's copy queue waits for each
   lane separately, so it reads lane k while lane k+1 is still crossing. Without this the async
   queue would still wait for the whole 126 MB before starting.
3. **Instrumentation that answers "where does the frame time go"**, logged every 10 s:
   - `display GPU: unpack … | direct list …` — 9060 copy-queue timestamps and direct-queue
     timestamps (list begin / after unpack / end).
   - `timeline ms after Present: 3090 copy a-b | 9060 unpack c-d | 9060 list e-f` — all three
     queues converted to QPC with `GetClockCalibration` (recalibrated every 64 frames).
   - `render thread blocked: FFX Present … | latency wait … | D3D11 gate pending n/m` — CPU time
     inside the FFX swap chain's Present.

Code: `CrossAdapterBridge::{Transfer, Unpack, WaitTransfer, MarkDisplayList,
SignalDisplayListDone, AddPresentBlock, CollectTimings}`, `GpuTimer`; call sites in
`DX12SwapChain::Present` / `CreateD3D12CommandQueues`; `Upscaling::Settings::asyncUnpack`.
Safety: the copy queue also waits for the previous same-slot direct list (`displayDoneFenceB`)
before overwriting its mirrors, although FFX's Present already guarantees that from the CPU side.

### First measured run (build 16:15, run 16:19) — it works

Built on this PC with `build-local.ps1` (MSVC 14.52, `/W4 /WX`): 0 warnings, 0 errors, DLL
1,113,088 B, the three F4SE exports present, `fg-gpu1.patch` still applies cleanly to `3e237a4`.
The installed DLL is this build. The shipped `settings.ini` keys are in effect (`bAsyncUnpack = 1`,
`bFrameGenOnSecondaryGPU = 1`, `bSendHudlessFrame = 1`); the MCM override has no GPU1 keys.

3840×2160, 4 lanes, 126.6 MB/frame, steady windows 16:20:30–16:21:10:

| base | presented | generated | copy 3090 | unpack 9060 | direct list | Present block | gate pending |
|---|---|---|---|---|---|---|---|
| 66.0–69.8 | 122.4–131.9 | 47–50% | 10.16–10.41 ms | 9.98–10.43 ms | 0.00–0.07 ms | 4.7–9.3 ms avg | 0 / 600–698 |

The previous 4K record on the serialised path was 44.1–48.1 base / 88.1–96.2 presented at the same
payload, so this is **+~40% base and ~+37% presented**. 1440p already hit the 142 panel cap; 4K now
does what 1440p used to.

The timeline line proves the lane streaming. Steady frame, ms after Present:

```
3090 copy 4.7-15.0 | 9060 unpack 7.3-17.6 | 9060 list 17.7-17.7
```

The 9060 starts reading **2.6 ms after the 3090's copy starts** — one lane, 31.6 MB ˜ 2.6 ms — and
overlaps 7.7 ms of the 10.3 ms copy. Before this change the read could not start until the whole
transfer had landed at 15.0. It ends 2.6 ms after the copy does.

Everything else confirms the model:
- `direct list` ˜ 0.00–0.07 ms, so FSR3's interpolation really is off our queue — the correction
  below was right, and the 9060's direct queue is not a factor.
- `D3D11 gate pending 0/698`: the 3090 never once queued behind a copy.
- `max lag 2`, `cpu waits 0`, link steady at ~12 GB/s; no `[E]`/`[W]`/CRITICAL lines in the run.

Two 10 s windows reported `generated 0%` and `27%` while base stayed at ~69 fps — that is
`gameActive` false (Pip-Boy/map/pause), not a stall. Menus still skip FG even with
`bDisableInMenus = 0`. Generation starts right after the first `PostDisplay captured HUDLess`.

Remaining cost: `FFX Present` still blocks the render thread 4.7–9.3 ms per frame, so the chain,
not the 3090, is still the limit. See "Next steps after the first test".

### Third change: early transfer (`bEarlyTransfer = 1`, default) — confirmed by the 16:49 run

"Next steps after the first test" #2, plus the switch to A/B it. Depth and motion vectors are
final after the world pass and the HUD-less frame after its capture, so the bridge copies those
lanes at those hooks instead of at Present; only the finished frame waits for Present.

- `CrossAdapterBridge::LaneTiming { kWorld, kHudless, kPresent }` per lane. Registration order is
  `final frame` first (it is created with the interop at swap-chain creation), then `depth`,
  `motion vectors`, `HUD-less colour`; the display GPU reads by timing, not by index.
- `PrepareFrame` runs on the first hook of a frame: waits for the slot's previous copy-done,
  resets its allocator, queues the consumed wait. `SubmitLane` submits one lane and signals its
  own fence value, `TransferGroup` does the lanes of one timing, and `Transfer` at Present copies
  whatever is left, signals copy-done and finishes the stats. A frame whose hooks did not run is
  still copied completely at Present.
- `DX12SwapChain::TransferEarlyLanes` signals the D3D11 producer fence with `++fenceValue` (the
  old code signalled the value before incrementing, i.e. 0 on the first frame) and calls
  `TransferGroup`. Hooks: `Upscaling::PostAlpha` (world lanes), `PostDisplay` (HUD-less).
- Lane fence values are allocated in submission order (`nextLaneValue`), not `frame*8 + lane`, so
  the shared fence cannot go backwards when the HUD-less group signals before the finished frame.
- The copy timer takes two marks per phase (world, HUD-less, Present) and sums only the phases
  that ran, so the number stays copy-engine busy time and not the frame span. `3090 copy` in the
  timeline is the earliest phase start relative to Present; it can be negative or slightly
  positive depending on how far the render thread is ahead of the GPU.

First run (16:43) found an ordering bug: `Unpack` iterated lanes by registration index, so the
9060 waited for the *finished frame* (lane 0, signalled at Present) before reading anything. The
timeline showed the read starting after the whole copy (`9060 unpack 12.6–22.3` vs `3090 copy
2.0–10.4`), base fps fell to 50.5–57.6, and the copy timer printed nonsense (80 ms avg, 8.5 s max)
because its marks assumed the last index was the last lane. Fixed 16:46: `Unpack` reads the
non-present lanes first, and the timer uses the per-phase marks above.

Measured (16:49, fixed build, 4K, steady windows):

| base | presented | generated | copy 3090 | unpack 9060 | chain (list end) | Present block |
|---|---|---|---|---|---|---|
| 65.4–69.8 | 122.9–137.1 | 50% | 10.23–10.54 ms | 10.44–10.54 ms | 13.3 ms | 4.18–5.22 ms avg |

Before this change the same code path measured chain 17.4 ms and Present block 7.3–9.3 ms, so it
took ~4 ms off the chain and ~3.5 ms off the render-thread block. Steady timeline:
`3090 copy -0.6–10.2 | 9060 unpack 2.7–13.2 | 9060 list 13.3` — the copy starts just before
Present and the 9060 begins reading ~3 ms after it instead of ~7.3 ms.

Base fps is unchanged at 65–70 because the pipeline is no longer the limit: ~13.3 ms of chain
against a ~14.5 ms frame, so the 3090's own render time is the ceiling now. Presented is exactly
2× base at 50% generated, `max lag 2`, `cpu waits 0`, `gate pending 0/698`. The last ~3% to 142 is
the 3090's render cost at 4K — a game-settings or upscaling question (to-do #2), not transport.

Remaining option if more is ever wanted: an earlier hook for depth/MVs than `PostAlpha` would
start the copies sooner, but those buffers are then pre-reticle-fix, so treat it as a trade, not a
free win. The ~12 GB/s link, not the 9060, is the floor.

Deferred: "Next steps" #3 (row strips). It is masked in this design — the 9060 reads the
already-copied lanes for ~7.8 ms after Present and the finished frame has landed by ~7.3 ms, so
strips cannot shorten the critical path. They only pay off once the *read* is also submitted
early (unpack during rendering), which is the next option if the new log still shows a tail.

### Correction to the "9060 is the bottleneck" analysis below

Two things in that analysis do not hold up against the code:

- **FSR3 frame generation does not run after the unpack on the same queue.** The plugin
  configures FFX with `allowAsyncWorkloads = true` and a `frameGenerationCallback`, so the
  interpolation runs on FFX's own **async compute queue** from its presenter thread. Only the
  *prepare* pass (`DispatchDescFrameGenerationPrepare`) is on our direct list. So the 9060's
  direct queue was "unpack + prepare", not "unpack + FSR3".
- **The ~9.4 ms FSR3 figure was back-solved from the 1440p run, but that run was display-capped.**
  141.8 presented on a 144 Hz panel is the refresh limit, so 70.9 says nothing about FSR3 cost.
  FSR3 FG on a 9060 XT should be a few ms even at 4K. The `direct list` line now measures it:
  0.00–0.07 ms at 4K, because only the prepare pass is on our queue; the interpolation itself is
  on FFX's async queue.

### What actually holds the 3090 back (confirmed by the new log lines — see top)

Not a GPU-side wait on its own copy: the 3090's copy runs on a separate DMA queue and the
D3D11 gate only waits for the copy of the frame *before*. But the CPU is throttled:

- The FFX swap chain has `BufferCount = 2` replacement back buffers. At the end of
  `FrameInterpolationSwapChainDX12::Present(N)` it CPU-waits on `replacementBufferFence` for the
  buffer frame N+1 will use, i.e. until **the real frame N-1 has been presented** (FFX
  v1.1.4 source, `Present()` last line; `presentInterpolated()` sets the availability value).
- So the render thread — and therefore the 3090's next frame — cannot start until frame N-1 has
  gone through the whole chain: render ? 3090 copy (~10 ms at 4K) ? 9060 read (~10 ms) ?
  prepare ? interpolate ? present interpolated ? pace ? present real.
- With two frames in flight the period is roughly `(render + copy + read + FSR3 + pacing) / 2`,
  and pacing itself is about half a period, so roughly `(render + copy + read + FSR3) / 1.5`.
  That fits both measurements without assuming a 10 ms FSR3 (4K: (˜9 + 10.3 + 10.5 + ˜2) / 1.5 ˜
  21 ms ? 47.6 fps; 1440p would allow > 71 and is display-capped). It also explains why lowering
  3090 settings moved 4K only from 46.6 to 48.1: render time is only one term in the sum.
- Consequence: **the PCIe latency is what costs fps, not the 9060's throughput**. Moving the read
  to a copy queue by itself would change little; overlapping the two PCIe trips (streaming) is
  what shortens the chain. Rough expectation: copy+read 20.5 ms ? ~13 ms, i.e. ~21 ms ? ~16 ms
  per frame, **~60 base / ~120 presented at 4K** — not 71/142. Treat as an estimate.
  **Measured (16:19 run):** 66–70 base / 122–132 presented, chain 17.4 ms from Present to the end
  of the 9060's list — at the better end of this estimate.

How to read the new lines to confirm or refute this:
- `render thread blocked: FFX Present` several ms per frame ? the 3090 is being starved by
  the chain (this model). Near 0 ? the 3090 itself is the limit.
- `timeline`: the 9060 unpack should start ~3 ms after the 3090 copy starts (one lane), not after
  it ends. If it starts only after the copy ends, the lane fences are not overlapping.
- `display GPU: unpack` much longer than `copy on render GPU` ? concurrent read+write over the
  root complex is slowing both; then try `bAsyncUnpack = 0` for comparison.

### Next steps after the first test

1. **Tested (see "First measured run"):** Present still blocks 4.7–9.3 ms, so the chain still
   limits. The remaining lever is a third frame in flight (`BufferCount = 3`, `kSlots = 3`, and
   every `[2]` array indexed by `frameIndex` in `Upscaling`, `DX12SwapChain`, `FidelityFX`), so
   Present(N) waits for frame N-2. **It costs about one game frame of added input latency
   (~14–15 ms at 4K/67 fps), on top of the ~1 frame FSR3 already adds.** At 132/142 presented
   that trades ~7% more fps for noticeably heavier controls — try it only as an opt-in setting.
   The latency-neutral alternatives are steps 2 (send depth/MV early) and 3 (finer lane splits);
   they shorten the chain instead of buffering more.
2. **Done in the third change (`bEarlyTransfer = 1`, see top) — built, not yet run.** Depth and
   motion vectors are copied at `PostAlpha`, the HUD-less frame at `PostDisplay`, so only the
   finished frame is left for Present; expected to take ~5–7 ms off the 4K chain.
3. Finer streaming (row strips): **deferred** — with the early transfer the 9060 already starts
   on ready data, so strips cannot shorten the critical path until the unpack is also submitted
   early. Revisit only if the new timeline shows the 9060 waiting on the finished frame.

## Goal

- 71 base fps on the render GPU, doubled to ~142 on the display GPU (144 Hz panel).
- Prefer image quality: keep the extra HUD-less frame on. The no-HUD-less mode saves ~25% PCIe but
  the user finds the HUD smearing unacceptable at 4K.
- 4K output is the ideal; 1440p output is the working fallback.

## Hardware and topology

| | |
|---|---|
| Render GPU | NVIDIA RTX 3090, **no display attached**, PCIe 3.0 x16 |
| Display / FG GPU | AMD RX 9060 XT, drives the only monitor (AOC 32M2V, 3840×2160 @ 144 Hz) |
| CPU / platform | Intel i9-7940X, X299 — PCIe 3.0 throughout |
| Measured transport | ~12 GB/s in both directions (~10.1–10.6 ms for 126.6 MB) |
| OS | Windows 10 19045, Visual Studio 18.11 / MSVC 14.52, CMake 4.3.1 |

Fallout 4 is forced to the 3090 with the Windows "High performance" GPU preference
(`HKCU\Software\Microsoft\DirectX\UserGpuPreferences`), but **the game ignores it** — it enumerates
adapters and takes the display GPU. The plugin therefore creates the game's D3D11 device on the
headless GPU itself (see `PickRenderAdapter`).

## Architecture as built

Two D3D12 devices:

- **interop device** on the render GPU: opens the game's D3D11 shared textures and the producer
  fence; owns the transfer copy queue.
- **FG device** on the display GPU: owns the FSR3 context, the swap chain, the present, and the
  direct queue that unpacks and generates.

Per frame, four lanes are copied (each double-buffered, two slots):

| lane | 4K size | 1440p size |
|---|---|---|
| final frame (with HUD, unpacked into the back buffer) | 33.18 MB | 14.75 MB |
| HUD-less colour (extra UI-free frame, optional) | 33.18 MB | 14.75 MB |
| depth (R32) | 33.18 MB | 14.75 MB |
| motion vectors (RG16) | 33.18 MB | 14.75 MB |
| **total** | **126.6 MB** | **56.2 MB** |

Synchronisation chain: D3D11 renders ? signals producer fence ? render-GPU copy queue waits and
copies into the cross-adapter heap ? signals transfer fence ? display-GPU direct queue waits and
unpacks ? FSR3 ? present ? signals consumed fence ? render GPU may reuse the slot. The render GPU
only ever waits for the copy of the frame *before* the one it is working on.

## Measured results (first session, serialised unpack; † async build; ‡ async + early transfer)

| Resolution | Lanes | Payload | Copy | Base | Presented | Generated |
|---|---|---|---|---|---|---|
| 2560×1440 | 4 (extra frame on) | 56.2 MB | 4.53 ms | 70.2–70.9 | 140.4–141.8 | 50% |
| 3840×2160 | 4 (extra frame on) | 126.6 MB | 10.06–10.33 ms | 44.1–48.1 | 88.1–96.2 | 50% |
| 3840×2160 | 3 (extra frame off) | 94.9 MB | 7.55–7.74 ms | 40.8–44.2 | 81.5–88.4 | 50% |
| 3840×2160 † | 4 (extra frame on) | 126.6 MB | 10.16–10.41 ms | 66.0–69.8 | 122.4–131.9 | 47–50% |
| 3840×2160 ‡ | 4 (extra frame on) | 126.6 MB | 10.23–10.54 ms | 65.4–69.8 | 122.9–137.1 | 50% |

† The 16:15 async-unpack build, first run 16:19.
‡ The 16:46 early-transfer build, run 16:49 — chain 13.3 ms (was 17.4), Present block 4.2–5.2 ms
(was 7.3–9.3). Base is now limited by the 3090's own render time; see "Third change".

`max lag` stayed at 2 and `cpu waits` at 0 in every run. Representative log:

```
[FG-GPU1] presented 141.8 fps | game 70.9 fps | generated 50%          (1440p)
[FG-GPU1] 70.9 game fps | 56.2 MB/frame = 3.90 GB/s | copy 4.53 ms (~12.13 GB/s link) | max lag 2 | cpu waits 0

[FG-GPU1] presented 96.2 fps | game 48.1 fps | generated 50%           (4K)
[FG-GPU1] 48.1 game fps | 126.6 MB/frame = 5.95 GB/s | copy 10.25 ms (~12.06 GB/s link) | max lag 2 | cpu waits 0
```

### Why 4K is capped at ~48 (first session's analysis — see the correction at the top)

The 9060 does two things per input frame, serialised on its direct queue:

```
per-frame cost ˜ payload / 12 GB/s  +  FSR3 (~9.4–10.5 ms)
```

- 1440p: 4.7 + 9.4 = 14.1 ms ? 70.9 fps ? (matches the log)
- 4K: 10.5 + 10.5 = 21.0 ms ? 47.6 fps ? (matches the log)

That is why reducing 3090 settings from base 46.6 to 48.1 barely moved it: the 3090 stopped being
the limit and the 9060 took over. The copy itself is only ~47% duty at 4K, and the render thread
never blocks (`cpu waits 0`).

## What changed this session

Relative to the original Claude prototype (`fo4-fg-gpu1/` at the start of the session):

### Tooling
- `build-local.ps1`:
  - **Fixed the crash that stopped the first run**: `git rev-parse HEAD` writes to stderr in a
    fresh repo, and Windows PowerShell 5.1 turns that into a terminating `NativeCommandError`
    under `$ErrorActionPreference = Stop` (even with `2>$null`). Now uses
    `git rev-parse --verify --quiet HEAD`.
  - **Self-relaunches under PowerShell 7** when started from 5.1 (`pwsh` on PATH), preserving
    parameters and exit code. If `pwsh` is missing it warns and continues on 5.1.
  - `$ProgressPreference = 'SilentlyContinue'` (the 250 MB FidelityFX download is much faster).
  - VS lookup checks that `vswhere` returned a path before calling `.Split()`.
- Workflow moved to `.github/workflows/fo4-fg-gpu1.yml` (the project copy was removed).
- `CLAUDE.md` added with the "use pwsh" convention.
- `README.md` updated: build on this PC, role selection, `bSendHudlessFrame`, performance budget.
- `fg-gpu1.patch` regenerated from the built tree; `git apply --check` passes against a pristine
  `3e237a4` checkout.

### Plugin
- `Upscaling.cpp`:
  - Fixed the compile error: `dx12SwapChain` was declared inside the resource loop but used after
    it; hoisted to function scope.
  - `bSendHudlessFrame` (renamed from `bTransportHudLess`; the old key is still read as a fallback):
    when off, the HUD-less lane is not registered, `HUDLessBufferShared12` is nulled (FSR3 then
    uses the back buffer and smears the UI), and `PostDisplay` skips the now-unused 4K D3D11 copy.
  - Logs the setting and the lane total.
- `CrossAdapterBridge.h/.cpp`:
  - `OwnsMonitor` now requires `AttachedToDesktop` and breaks on enumeration errors.
  - `HasAttachedOutput` + a one-shot per-adapter output dump (`game window monitor`, `output[i]
    "\\.\DISPLAY1" attached=… monitor=…`, `"<gpu>" outputs: N`).
  - `SelectSecondaryAdapter` picks: name filter ? adapter that owns the window monitor ? adapter
    with an attached display ? only other hardware adapter. The `[display]` marker is new.
  - `PickRenderAdapter`: if the game picked the display GPU and exactly one hardware adapter has
    no display attached, render on that one. Guarded so it cannot invert roles if detection fails.
- `DX11Hooks.cpp`: uses `PickRenderAdapter` to choose the adapter passed to `D3D11CreateDevice`
  (logs `the game picked the display GPU; rendering on "…" instead`).
- `DX12SwapChain.h/.cpp`: every 10 s logs
  `presented X fps | game Y fps | generated Z%` using `GetLastPresentCount()` deltas.
- `settings.ini`: `bFrameGenOnSecondaryGPU = 1`, `sSecondaryGPUName =`,
  **`bSendHudlessFrame = 1`**.

### Current machine state (important)

- The **installed** game DLL is the 17:46 early-unpack build (the zip in `D:\fgb\FO4-FG-GPU1.zip`),
  with the shipped settings: `bSendHudlessFrame = 1`, `iHalfResDepthMV = 1`, `bEarlyUnpack = 1`.
- The game currently runs **4K, fullscreen**, at deliberately **low test settings**. MCM override has
  `bFrameLimitMode=0`, `bDisableInMenus=0`, `bEnableDebugLogging=1`, `iFrameGenFrames=0`.
- Frame cap: HighFPSPhysicsFix `InGameFPS=71.0`, `FramerateLimitMode=0` (limiter before Present).
- The repo is still uncommitted: `fo4-fg-gpu1/`, `.github/`, `CLAUDE.md`, `research/` are untracked.

## Build and install

Build (2 min incremental; ~7 min from a cold vcpkg cache):

```powershell
pwsh -ExecutionPolicy Bypass -File D:\projects\mgpu-dlss\fo4-fg-gpu1\build-local.ps1
```

- Work tree: `D:\fgb\FO4Upscaling` (pinned `3e237a44269a8dc8cab54aab4ef0aae83c284a43`).
- vcpkg: `D:\fgb\vcpkg` at `b472291f295551b7127359ea38fdd2ca092f6f1b`.
- Output: `D:\fgb\package\Data\...`; installable zip `D:\fgb\FO4-FG-GPU1.zip`.
- `-Clean` forces a CMake rebuild.
- Build is clean: 0 compiler warnings under `/W4 /WX`; DLL exports
  `F4SEPlugin_Load/Query/Version`.

Install: Vortex ? install the zip over the existing mod. Game folder:
`C:\SteamLibrary\steamapps\common\Fallout 4`. Log:
`C:\Users\PA\Documents\My Games\Fallout4\F4SE\AAAFrameGeneration.log` (truncated per run).

## Test protocol

1. Install the current `D:\fgb\FO4-FG-GPU1.zip`.
2. Monitor must be on the **9060**; keep ReShade **off** (`dxgi.dll` renamed) for now — AutoHDR
   changes the back-buffer format and produces the known blue/purple garbage.
3. Launch through F4SE, windowed/borderless, play 30–40 s.
4. Send the `[FG-GPU1]` lines. The useful ones: `game window monitor`, adapter lines with
   `[display]`, `SELECTED`, `the game picked the display GPU`, `swap chain created`,
   lane lines, and the 10 s `game fps` / `presented` pairs.

Quick A/B to prove the 3090 is not the limit: set `bFrameGenerationMode = 0` and
`bFrameGenOnSecondaryGPU = 0`, run at 4K. Base should return to ~71 (the pre-mod figure).

## Next to-dos, in order

### 1. Overlap the 9060's unpack with FSR3 — **done, built and measured** (see top)

Idea: the 9060 currently reads the staging buffers into its mirrors on the **direct queue**, then
FSR3 runs on the same queue — 21 ms serialised at 4K. Move the unpack to a **copy queue on the FG
device** and fence it; the copy for frame N then runs while FSR3 computes frame N-1.

Expected: per-frame cost becomes `max(read ~10.5 ms, FSR3 ~10.5 ms)` plus sync ˜ 11–13 ms, so the
9060 allows ~80–90 fps and the 3090 becomes the limit again. At the user's reduced 4K settings that
should be **71 base / 142 presented with the extra UI-free frame on**.

Sketch:
- Add to `CrossAdapterBridge`: `copyQueueB`, per-slot copy allocators, one copy command list,
  `unpackFenceB` + per-slot values.
- Replace `RecordUnpack`/`RecordUnpackTo` with `BeginUnpack(slot, finalLane, backBuffer)`: record
  barriers + copies on the copy list, execute on `copyQueueB`, signal `unpackFenceB`.
- In `DX12SwapChain::Present`, call `BeginUnpack` and then `commandQueue->Wait(unpackFenceB, value)`
  before the FSR3 list is executed.
- Move `SignalConsumed` to `copyQueueB` (the staging slot is free once the copy has read it).
- Before resetting a slot's copy allocator, CPU-wait on its previous `unpackFenceB` value (same
  pattern as the existing `WaitCpu(copyDoneFenceA, previous)`).
- Watch: cross-queue resource states on the mirrors and the back buffer, the swapchain present
  ordering, `max lag`, `cpu waits`, and whether DMA + compute actually overlap on the 9060.
- Keep a setting to fall back to the serialised path if it regresses.

### 2. Upscaling on the 3090 (alternative route to 4K/142)

Render 1440p (or 1080p) and upscale to 4K with FO4Upscaling's `Upscaling.dll` (DLSS/FSR), then FG
on the 9060. Payload becomes 4K final frame + render-res depth/MV/HUD (~58–77 MB, 5–6.4 ms) and
FSR3 runs its optical flow at render resolution.

Work: make the FG plugin and the upscaler share the same D3D11 device and agree on
`kMain`/`kFrameBuffer` sizes; verify the lane factory handles mixed resolutions (it already sizes
each lane independently). Do **not** use doodlum's `Upscaling.dll` — take the matching one from the
same `3e237a4` tree. Watch for Community Shaders conflicts.

### 3. Polish / risks

- **Runtime transport failure has no recovery.** `CrossAdapterBridge::Fail` sets `active = false`
  but `secondaryGPU` stays true, so `Present` skips both the bridge and the single-GPU path: the
  screen freezes and only a restart recovers. At minimum document it; optionally attempt a degraded
  present.
- ~~Expose `bSendHudlessFrame` (and the GPU1 mode) in MCM~~ — done: every setting is in MCM.
- Commit and push the repo (workflow included). The GitHub build is manual only
  (`workflow_dispatch`; Actions ? FO4 FG on GPU1 ? Run workflow); pushes do not build.
- Consider measuring the 9060's FSR3 cost directly (timestamps around the direct-queue work) to
  validate the overlap estimate before building it.
- Optional: log the 9060's unpack time separately, so "read vs FG" is not inferred.

## Gotchas

- **`powershell` is always Windows PowerShell 5.1; `pwsh` is 7.** `build-local.ps1` re-launches
  itself under `pwsh`, but ad-hoc commands should use `pwsh`.
- **FO4 ignores the Windows GPU preference.** The plugin forces the D3D11 device onto the headless
  GPU. If that log line disappears, the game is rendering on the 9060 and the pipeline is inverted.
- **Monitor detection**: `DXGI_OUTPUT_DESC.Monitor == HMONITOR` produced false positives in the game
  process (once marking the headless 3090 as the display GPU). Current code uses attached-output
  counting and still logs the raw output table for diagnosis. If roles ever flip, read those lines.
- **MCM override wins**: `Data\MCM\Settings\FrameGeneration.ini` overrides the shipped
  `Data\MCM\Config\FrameGeneration\settings.ini` per key. A missing key falls through to the shipped
  file. MCM may rewrite its override on exit, so prefer editing the shipped file for one-off tests.
- **`iFrameGenFrames` is irrelevant to FSR3** (2× fixed); it only applies to DLSS-G MFG.
- The log is truncated every launch, so grab it after the run you care about.
- Reinstalling the mod replaces the shipped `settings.ini`; re-apply manual edits.

## File map

| Path | What |
|---|---|
| `fo4-fg-gpu1/overlay/` | Modified/new upstream files copied over the pinned checkout (FrameGeneration and Upscaling plugins) |
| `D:\fgb\FO4-FG-GPU1-fsr41.zip` | Installable zip with FSR 4.1 upscaling on the 9060 (+ matching `Upscaling.dll`) |
| `fo4-fg-gpu1/build-local.ps1` | Local build + package script (self-relaunches under pwsh 7) |
| `fo4-fg-gpu1/fg-gpu1.patch` | Full diff against `3e237a4`, applies cleanly |
| `fo4-fg-gpu1/README.md` | User-facing docs, performance budget, settings |
| `fo4-fg-gpu1/SESSION-NOTES.md` | This file |
| `.github/workflows/fo4-fg-gpu1.yml` | GitHub Actions build (not yet committed) |
| `CLAUDE.md` | Agent note: use `pwsh` |
| `D:\fgb\package` | Built mod tree |
| `D:\fgb\FO4-FG-GPU1.zip` | Installable zip (newest build) |
| `D:\fgb\FO4Upscaling` | Pinned upstream checkout with the overlay applied |
| `C:\Users\PA\Documents\My Games\Fallout4\F4SE\AAAFrameGeneration.log` | Plugin log |

## Key log line reference

```
[FG-GPU1] game window monitor 0x10001 "\\.\DISPLAY1"
[FG-GPU1] adapter[0] "AMD Radeon RX 9060 XT" ... [game window's monitor] [display]
[FG-GPU1]   output[0] "\\.\DISPLAY1" attached=1 monitor=0x10001
[FG-GPU1] adapter[1] "NVIDIA GeForce RTX 3090" ... [game]
[FG-GPU1]   "NVIDIA GeForce RTX 3090" outputs: 0
[FG-GPU1] the game picked the display GPU; rendering on "NVIDIA GeForce RTX 3090" instead
[FG-GPU1] SELECTED "AMD Radeon RX 9060 XT" (owns the monitor the game window is on)
[FG-GPU1] interop device on "NVIDIA...", frame generation + present on "AMD..."
[FG-GPU1] lane[N] '<name>' 3840x2160 ... timing=world|hudless|present (bytes, running total)
[FG-GPU1] <fps> game fps | <MB>/frame = <GB/s> | copy on render GPU avg <ms> max <ms> | max lag N | cpu waits N
[FG-GPU1] timeline ms after Present: 3090 copy a-b | 9060 unpack c-d | 9060 list e-f   (a is negative with bEarlyTransfer=1)
[FG-GPU1] presented <fps> | game <fps> | generated <percent>
```
