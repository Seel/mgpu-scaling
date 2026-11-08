# Plan: run the display GPU's side of each frame on its own thread

Status: planned 2026-11-07, not started. Do both parts of [UI-TILES.md](UI-TILES.md) first and
measure again: if the tiles bring the game to the 71 cap with CS on, this may not be needed.
New setting, off by default: `bDisplayThread` in `[FrameGeneration]` (restart).

## Why

The game's render thread calls our `DX12SwapChain::Present`, which today does everything for the
frame, both GPUs, in one go, and ends in FFX's `Present`. FFX's Present for frame N first waits
(`FrameInterpolationSwapchainDX12.cpp:2188`, `compositionFenceCPU`) until FFX's presenter thread
has composited frame N-1's real frame, which it does only after pacing out N-1's generated frame.
While the render thread sits there, the game cannot simulate or record frame N+1, so the 3090 runs
dry:

```
2026-11-07, CS on, 2 frames in flight:  62.9 game fps, FFX Present block 6.9 ms, 3090 idle ~3.2 ms/frame
                    3 frames in flight: 66.6-68.5 fps, FFX Present block 3.6-5.6 ms, 3090 idle 2.2-2.6 ms
3090 from a frame's first command to its end: 12.0-12.8 ms (71 fps needs 14.1)
```

`bExtraFrameInFlight` removed the back-buffer wait at the end of FFX's Present; the wait at its
start stays, whatever the buffer count. A thread of our own that calls FFX's Present lets the game
thread hand the frame over and return at once; the game then waits only if it gets more than the
slots allow ahead of the display GPU.

## Today's per-frame work in `DX12SwapChain::Present` (game thread)

1. D3D11: copy the finished frame into the slot's shared texture; `Signal(d3d11Fence)`.
2. Bridge A side: `Transfer` (render GPU copy queue).
3. Bridge B side: `Unpack` (display GPU copy queue) or the serialised path.
4. Display GPU direct list: reset, `MarkDisplayList`, `SecondaryUpscale::FinishSubmit` and
   `RecordPresent` (composite / conversion), FFX configure + `PrepareV2` dispatch
   (`FidelityFX::Present`), close, `ExecuteCommandLists`, `SignalDisplayListDone`.
5. `swapChain->Present` (FFX) — the blocking call.
6. Stats (`GetLastPresentCount`, the 10 s lines), `frameIndex = GetCurrentBackBufferIndex()`,
   `GateD3D11(next slot)`, `upscaling->Reset()`, frame limiters, `renderGpuTimer.NextFrameStart`.

At frame hooks, also on the game thread: `TransferEarlyLanes` → bridge `TransferGroup` (A) and
`UnpackGroup` (B, `bEarlyUnpack`); PostDisplay → `SecondaryUpscale::BeginFrame` (FSR 4.1 dispatch
on the display GPU's direct queue, after `WaitForLaneReads` and a CPU `WaitDisplayListDone`).

## Design

### Split

- **Game thread** keeps 1, 2, the hooks, and 6 minus the FFX/stats parts. At the end of Present it
  fills a `FramePacket` and pushes it to a bounded queue, then does `GateD3D11`, `Reset`, the
  limiters, and returns.
- **Display thread** (new, created with the FFX swap chain): pops packets in order and does 3, 4, 5
  and the display-side stats for that packet.

`FramePacket` holds everything the display side reads from live game state today, captured on the
game thread at Present:
- slot (frame index), frame number, `SyncInterval`, `Flags`;
- `frameUpscaled[slot]` and `externalFrame` (render size, jitter, sharpness) — today reset right
  after use;
- the frame-generation decision (`frameGenerationMode`, `main->gameActive`, menu mode, the UI
  movement check) and `halfResMode`;
- what `FidelityFX::Present` reads: `screenWidth/Height`, `offsetX/offsetY` (jitter),
  `dynamicWidthRatio/HeightRatio`, camera near/far (the two `REL::ID` floats), `cameraState.posAdjust`,
  `camViewData.viewUp/viewRight/viewDir`, `FidelityFX::VerticalFov()`, frame time delta;
- `upscaling->ReloadSettingsIfNeeded()` stays on the game thread; the packet carries the values.

`FidelityFX::Present` takes the packet instead of reading globals; its function statics
(`frameID`, `lastFrameTime`, `wasGenerating`, report timers) become display-thread state.

### Slot index without asking FFX

The game thread needs the next frame's slot before the display thread has presented the current
one. FFX sets `replacementSwapBufferIndex = presentCount % gameBufferCount`, so the slot is the
frame counter modulo `frameSlots`. The game thread keeps its own `gameFrameIndex`
(used by the hooks instead of `frameIndex`); the display thread checks
`swapChain->GetCurrentBackBufferIndex() == packet.slot` before recording and logs a CRITICAL once
if they ever differ.

### Back-pressure

Queue capacity `frameSlots - 1` packets (1 with 2 slots, 2 with 3): a slot is reused only after
its previous frame left the display thread. The game thread blocks on a condition variable when
the queue is full and times the wait (new stat: `game thread waited for the display thread avg/max
ms`); this replaces `render thread blocked: FFX Present`. The GPU-side guards stay as they are
(`GateD3D11`, the bridge's consumed fences).

### Shared state and locking

- **Bridge B side** (`Unpack`, `UnpackGroup`, `WaitForLaneReads`, `RecordUnpack*`,
  `SignalConsumed`, `SignalDisplayListDone`, `WaitDisplayListDone`/`DisplayListDone`,
  `MarkDisplayList`, the display-side timers and stats): called from both threads (hooks: game;
  Present: display). One `std::mutex` around the B-side entry points. Never hold it across FFX
  Present or a CPU wait that the other thread could need to finish.
- **Bridge A side** (`Transfer`, `TransferGroup`, `GateD3D11`, the A-side stats): game thread only.
- **`SecondaryUpscale`**: `BeginFrame`/`TrySubmit` (game thread, PostDisplay) and
  `FinishSubmit`/`RecordPresent`/`EndFrame`/`LogStats` (display thread) share `list`,
  `allocators`, `pending`/`submitted`/`params`, timestamps. One mutex, or per-slot command lists so
  the two threads never touch the same slot.
- **D3D11 immediate context**: game thread only. The display thread must never touch it (it is
  not thread-safe); everything D3D11 in Present stays on the game thread.
- **FFX**: the frame-generation context and the swap chain only from the display thread; the FSR
  4.1 upscale context only from wherever `TrySubmit` runs (under the `SecondaryUpscale` lock).
  The game's own calls on our `DXGISwapChainProxy` (`GetDesc`, `GetBuffer`, ...) stay on the game
  thread; FFX/DXGI getters are safe to call concurrently with Present.
- Errors: no exception may leave the display thread (`DX::ThrowIfFailed` sites there catch, log
  CRITICAL and mark the transport down); the game thread then skips pushing packets.
- Shutdown: stop flag + notify + join with a timeout before the FFX contexts are destroyed; the
  game thread must not wait forever on a full queue during exit (timeout and drop).

### What stays the same

FFX's own presenter thread, pacing and ReShade (which runs there); the frame limiters and
HighFPSPhysicsFix on the game thread; `bExtraFrameInFlight` (works with both; with 2 slots the
queue holds one frame, i.e. the game may run one frame ahead of the display thread).

## Steps

1. Introduce `FramePacket` and make `FidelityFX::Present`, `SecondaryUpscale::RecordPresent`
   and the display half of `DX12SwapChain::Present` read from it, still called inline on the game
   thread (no threads yet). Behaviour must be identical: one test run, compare the 10 s lines.
2. `gameFrameIndex` for the game thread; check against FFX's index each frame (still inline).
3. Locks for the bridge B side and `SecondaryUpscale` (still inline: they must be uncontended).
4. The display thread, the bounded queue, back-pressure, shutdown, error path; behind
   `bDisplayThread`.
5. Stats: game-thread wait, display-thread time per frame, queue depth; keep the existing lines.
6. Tests below; PresentMon A/B `bDisplayThread` 0/1 (with and without `bExtraFrameInFlight`,
   with the UI tiles if they shipped).

## Tests and acceptance

- `render GPU: idle between frames` close to 0 below the cap, base fps up to the cap with CS on.
- Frame pacing: PresentMon on-screen frame-time sd and p99 not worse than now; generated still 50%.
- Input latency: feels no worse than `bExtraFrameInFlight = 1` (the queue adds at most one frame).
- Robustness: menus, loading screens, Pip-Boy, alt-tab, quitting to desktop and to the main menu,
  loading another save — no hangs, no CRITICAL lines; `bDisplayThread = 0` exactly as before.
- No D3D11 call from the display thread (add a debug assert on the thread id in the D3D11 paths).

## Risks / open questions

- Ordering across the two threads on the display GPU's queues: early unpacks of frame N+1 (game
  thread) can now be submitted before frame N's direct list (display thread). They use another
  slot and wait on their own fences, so this should be fine, but verify the bridge's fence values
  stay monotonic per queue under the lock.
- `WaitDisplayListDone` in `SecondaryUpscale::TrySubmit` is a CPU wait on the game thread for the
  slot's previous direct list (submitted by the display thread): with the queue bounded by the
  slot count this should return immediately; log if it ever waits.
- FFX may expect Present to be called from the thread that created the swap chain; if anything
  misbehaves, create the FFX swap chain context from the display thread too.
- More latency than now when the game runs ahead; acceptable only if fps and pacing improve.
