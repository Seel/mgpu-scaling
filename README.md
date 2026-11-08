# MGPU Scaling: upscaling and frame generation on a second GPU (Fallout 4)

Fallout 4 keeps rendering on the **RTX 3090**. FSR3 frame generation, FSR 4.1 upscaling and the final present run on the **RX 9060 XT**, which is the card the monitor is plugged into. Nothing is copied back to the 3090.

MGPU Scaling is one F4SE plugin, `AAAMGPUScaling.dll`. It was developed as "FG-GPU1": a modified `AAAFrameGeneration.dll` plus a matching `Upscaling.dll` from [northaxosky/FO4Upscaling](https://github.com/northaxosky/FO4Upscaling) (pinned at `3e237a4`). Both are now built into the one DLL, with one MCM page, one settings file, one log and its own data folder. It replaces FO4Upscaling's frame generation, the FG-GPU1 package and doodlum's Upscaling; disable those mods (it refuses to run beside `AAAFrameGeneration.dll`, and turns its upscaler off beside any other `Upscaling.dll`). Log lines and code still say `FG-GPU1` for the second-GPU parts.

The upstream frame generation plugin creates its D3D12 device on the game's GPU. This version creates two devices:

| | GPU | Does |
|---|---|---|
| render GPU | 3090 (not the monitor) | The game renders here. A **copy queue** copies each frame into cross-adapter heaps. |
| display GPU | 9060 XT (monitor) | Unpacks the copies, runs FSR3 FG, owns the swap chain and presents. |

The roles are picked automatically, not from a setting. The display GPU is the one the monitor is
plugged into (the 9060), read from Windows' display configuration (`QueryDisplayConfig`), which
names the adapter that scans each monitor out. DXGI's own output list cannot be used for this: on
Windows 11, a game with a GPU preference (Settings → System → Display → Graphics, or the default
high-performance GPU chosen there) sees the monitors listed under the preferred GPU, whichever card
the cable is in. With the 3090 preferred, Fallout 4 saw the monitor under the 3090; the plugin
used to trust that, put the game on the 9060 and frame generation on the 3090, and the game crashed
at its first frame (`DXGI_ERROR_DEVICE_REMOVED` while creating the shared buffers).

Fallout 4 renders on whichever GPU Windows prefers for it:
- **3090 preferred (recommended on Windows 11):** the game starts on the 3090, which drives no
  display, and stays there: `the game picked "NVIDIA GeForce RTX 3090", which drives no display; it
  renders there and "AMD Radeon RX 9060 XT" presents`.
- **9060 preferred, or no preference** (Windows 10 ignored the preference for Fallout 4, so this was
  always the case there): the game starts on the 9060, and the plugin creates its D3D11 device on the
  3090 instead: `the game picked "AMD Radeon RX 9060 XT", which drives the display; ...` followed by
  `rendering on "NVIDIA GeForce RTX 3090" instead of the game's pick`.

If detection ever gets it wrong, `sSecondaryGPUName` (e.g. `9060`) names the GPU that presents; the
game is then moved off it when it started there.

Each game frame crosses PCIe once, in one direction. The payload is the finished frame, the HUD-less frame, depth and motion vectors (about 4 B/px each; ~133 MB at 4K if the motion-vector target is 4 B/px). Without this mod, Windows already copies the finished frame from the 3090 to the 9060 XT so you can see it. The new copies are HUD-less, depth and motion vectors.

Synchronisation never makes the 3090 wait for the copy of the frame it just finished. Its next frame waits only for the copy of the frame *before* that.

## Files

The files used to live in the `fo4-fg-gpu1/` folder of the `mgpu-dlss` repository; since the move
to this repository they are at the root, and the paths below are relative to it.

| Path | What |
|---|---|
| `overlay/` | Full copies of the changed/new upstream files. The build copies them over the pinned checkout. |
| `overlay/plugins/MGPUScaling/` | The plugin target (`AAAMGPUScaling.dll`): its entry point and settings code, and in `src/upscaler/` FO4Upscaling's Upscaling plugin in `namespace up` |
| `overlay/plugins/FrameGeneration/src/` | The frame-generation part (changed upstream files plus the new second-GPU code); compiled into the same DLL |
| `overlay/include/ffx2/` | FSR SDK 2.4 API, frame-generation and upscaler headers (unmodified, MIT), used for both FSR runtimes |
| `overlay/include/vulkan/`, `overlay/include/vk_video/` | Khronos Vulkan headers 1.3.268 (unmodified, Apache-2.0 OR MIT), for `bDirectGPUCopy`; `vulkan-1.dll` is loaded at run time |
| `fg-gpu1.patch` | The same changes as a diff against `3e237a4`, for review (without the vendored SDK and Vulkan headers and licence text; the upscaler files show as copies of `plugins/Upscaling/src`) |
| `build-local.ps1` | Builds and packages the mod on this PC with the installed Visual Studio |
| `tools/presentmon-capture.ps1`, `tools/presentmon-summary.ps1` | Frame-pacing A/B: record a PresentMon capture of the game, and compare captures (on-screen time, jitter, 1% low, frame types, present mode) |
| `tools/bridge-load/` | Standalone D3D12 load generator: replays the bridge's host-memory traffic (77 MB up + 77 MB down per frame) on one GPU, to A/B whether that traffic costs the game fps |
| `tools/save-runs.ps1` | Keeps each game run's `MGPUScaling.log`, `ReShade.log` and MCM settings in `D:\fgb\runs\<time>-direct<0|1>-<hdr|sdr>` for back-to-back A/B runs (the logs are overwritten on every start) |
| `tools/p2p-probe/` | Standalone test bed for `bDirectGPUCopy`: can the 3090 write into the 9060's VRAM, how fast in every direction, how the 9060's D3D12 can read it, does the block move under VRAM pressure, and the mod's per-frame sequence 300 times |
| `SESSION-NOTES.md` | Session handoff: measurements, findings, next steps |
| `plans/` | Planned work not started yet: `UI-TILES.md` (freeze the HUD with a motion-vector mask, then send only the UI tiles of the finished frame), `DISPLAY-THREAD.md` (FFX Present on its own thread) |
| `.github/workflows/build.yml` | GitHub Actions build (manual only) |

The changes:
- `CrossAdapterBridge.h/.cpp` are new: adapter choice, cross-adapter heaps and fences, the copy queue, unpacking, stats.
- `PeerCopy.h/.cpp` are new: the 9060's Vulkan side of `bDirectGPUCopy` (CPU-visible VRAM blocks the 3090 writes into, the copy into D3D12 landing heaps, the shared fence).
- `SecondaryUpscale.h/.cpp` and `SecondaryUpscaleCS.hlsl` are new: FSR 4.1 upscaling and the UI composite on the second GPU, and the conversion into a swap chain an HDR add-on changed (on the second GPU, or on the game's GPU in single-GPU mode).
- `DX11Hooks.cpp`, `DX12SwapChain.*`, `FidelityFX.*`, `Upscaling.*` are small edits.
- `plugins/MGPUScaling/src/upscaler/` is FO4Upscaling's `Upscaling` plugin, wrapped in `namespace up` (its classes have the same names as the frame-generation part's) and patched: upscale method 3 renders for the second GPU's FSR 4.1. Its entry point is `Upscaler.cpp`.
- `plugins/MGPUScaling/src/XSEPlugin.cpp` replaces both plugins' entry points and installs their hooks in the order the two DLLs did (see the comment there); `MGPUScaling.h/.cpp` hold the paths, the settings loader and the one-time settings migration.
- `FrameSlots.h`: frames in flight, 2 or 3 (`bExtraFrameInFlight`); every per-frame resource is sized from it.
- `include/PCH.h`: `stl::detour_thunk` chains behind a function another plugin hooked first instead of copying that plugin's jump unrelocated (Community Shaders' frame annotations crashed the game otherwise).
- The top-level `CMakeLists.txt` adds the `MGPUScaling` target and drops `AAAFrameGeneration`; MCM's `config.json`/`settings.ini` are one page set with `[FrameGeneration]` and `[Upscaling]` sections.

## Build on this PC

Visual Studio 2026 (or 2022), including the Build Tools edition, with the C++ workload and Git are
enough; the script loads the developer environment itself. Run it from an ordinary PowerShell window:

```powershell
pwsh -ExecutionPolicy Bypass -File D:\projects\mgpu-scaling\build-local.ps1
```

`pwsh` is PowerShell 7. Windows PowerShell 5.1 (`powershell.exe`) still works — the script detects
it and re-runs itself under `pwsh` when that is installed — but 7 is what it is developed and tested
on. `powershell.exe` is permanently 5.1; installing 7 does not change what that name means.

It clones FO4Upscaling at the pinned commit into `D:\fgb` (resetting its source folders to that
commit first), applies `overlay/`, builds `AAAMGPUScaling.dll` in Release (`/W4 /WX`) and writes a
ready-to-install folder to `D:\fgb\package` and the same as `D:\fgb\MGPU-Scaling.zip`, including the
FidelityFX runtimes. First run takes 20–40 min because vcpkg compiles the dependencies once; later
runs take a few minutes. `-Clean` forces a rebuild.

Verified on this machine with the VS 18.11 Build Tools / MSVC 14.51 / CMake 4.3.1: configure, build
and package complete with 0 compiler warnings, and the resulting DLL exports the three F4SE entry
points. The newest `VC\Tools\MSVC` folder must be a complete toolset; a half-installed newer one (no
`bin\HostX64\x64\cl.exe`) makes vcpkg fail to detect a compiler, and the script stops with that path
before it does.

## Build on GitHub

1. Commit and push; `.github/workflows/build.yml` is already in place.
2. On GitHub, open **Actions → MGPU Scaling** and use **Run workflow**. It is manual only; pushes do not start a build.
3. When it's green, download the **MGPU-Scaling** artifact. It's a zip laid out as a mod (`Data\F4SE\Plugins\AAAMGPUScaling.dll`, …).

## Install

1. In Vortex, install `MGPU-Scaling.zip` as a mod (or copy its `Data` folder into the game folder). **Disable** the FG-GPU1 package, FO4Upscaling's frame generation mod and doodlum's Upscaling: MGPU Scaling replaces all three and shares no file with them. If `AAAFrameGeneration.dll` is still installed, MGPU Scaling shows a message at start and stays off; if another `Upscaling.dll` is installed, its upscaler (and FSR 4.1 on the second GPU) stays off and the log says why.

   Package layout: `Data\F4SE\Plugins\AAAMGPUScaling.dll`, `Data\F4SE\Plugins\MGPUScaling\Shaders\*.hlsl`, `Data\F4SE\Plugins\MGPUScaling\FidelityFX\*` and `Data\MCM\Config\MGPUScaling\`. The log is `Documents\My Games\Fallout4\F4SE\MGPUScaling.log`. The "AAA" keeps it the first F4SE plugin to load, which its D3D11 device hook needs.
2. Every setting below is also in the Mod Configuration Menu, page **MGPU Scaling** (sub-pages Frame Generation, Second GPU, Upscaling); entries marked "(restart)" apply after a game restart, the rest within about a second. The shipped defaults are `Data\MCM\Config\MGPUScaling\settings.ini`; MCM writes your changes to `Data\MCM\Settings\MGPUScaling.ini`, which **overrides** them, so edit that file by hand. Frame generation's keys are in its `[FrameGeneration]` section, the upscaler's in `[Upscaling]`.

   On the first start without `MGPUScaling.ini`, the plugin copies your settings from the old `Data\MCM\Settings\FrameGeneration.ini` and `Upscaling.ini` (`[Settings]`, only the keys this plugin knows) into it and logs `[INIT] carried N settings over`. The old files are left alone.

```ini
[FrameGeneration]
bFrameGenerationMode = 1
bFrameGenOnSecondaryGPU = 1
sSecondaryGPUName =          ; optional, e.g. 9060: the GPU that presents; the game renders on the other. Only if auto-selection refuses or is wrong
bSendHudlessFrame = 1        ; 1 = extra UI-free copy (clean HUD); 0 = ~25% less PCIe, HUD smears
bAsyncUnpack = 1             ; 1 = 9060 reads on a copy queue, lane by lane (default); 0 = original serial path
bEarlyTransfer = 1           ; 1 = depth/MVs/HUD-less cross during the frame (default); 0 = all at Present
bEarlyUnpack = 1             ; 1 = the 9060 reads them as they land, before Present (default); 0 = reads queued at Present
iHalfResDepthMV = 1          ; 0 = full res; 1 = half-res depth, rebuilt on the 9060 (default); 2 = half-res depth + MVs
bExtraFrameInFlight = 0      ; 1 = 3 frames in flight instead of 2: more throughput when the chain, not the 3090, limits; +1 frame latency (restart)
bDirectGPUCopy = 0           ; 1 = EXPERIMENTAL: the 3090 writes straight into the 9060's VRAM, no system RAM; guarded (restart)
iFrameGenType = 0            ; ignored in GPU1 mode (always FSR3)
iFSRRuntime = 1              ; 0 = FidelityFX SDK 1.1.4 (FSR 3.1.4); 1 = FSR SDK 2.4, newest provider offered (default)
iFSRSwapChainProvider = 0    ; SDK 2.4: 0 = newest swap chain (4.0.0 on RX 9000); 1 = newest 3.x (3.1.7), for A/B
iFSRFrameGenProvider = 0     ; SDK 2.4: 0 = newest interpolator (4.0.2 ML on Win11 + RX 9000, else 3.1.6); 1 = 3.x

[Upscaling]
iUpscaleMethodPreference = 3 ; 0 = off, 1 = FSR 3.1, 2 = DLSS, 3 = FSR 4.1 on the second GPU (restart to switch to/from 3)
iQualityMode = 1             ; 1 Quality, 2 Balanced, 3 Performance, 4 Ultra Performance, 0 Native AA
fSharpness = 0.5
```

`iFSRRuntime` picks the FSR runtime; see [FSR SDK 2.4](#fsr-sdk-24-ifsrruntime) below. It applies
in both GPU1 and single-GPU FSR3 mode.

`bSendHudlessFrame` is the "extra UI-free frame" switch. On (default) the 3090 sends a second copy
of each frame with the HUD removed; FSR3 uses it so the HUD is not interpolated. Off removes that
copy from the PCIe budget (~25% less traffic at 4K) but the HUD is then interpolated with the scene
and smears a little during camera movement. With FSR 4.1 on the second GPU there is no second copy
(the upscale result is the UI-free frame), so the switch only decides what frame generation is given:
off hands it a copy of the composed frame instead, and the HUD is interpolated with the scene — see
[FSR 4.1 upscaling on the second GPU](#fsr-41-upscaling-on-the-second-gpu).

`bAsyncUnpack` changes how the 9060 reads what arrives. On (default) it reads on its own copy
queue and starts on each buffer (final frame, HUD-less, depth, motion vectors) as soon as that
buffer has crossed, while the 3090 is still sending the next one; the FSR3 queue only waits for
the read to finish. Off is the original path: wait for all four, then read them on the FSR3 queue.
Use it as an A/B switch; both log the same timing lines.

`bEarlyTransfer` changes *when* the buffers cross. On (default) depth and motion vectors start
crossing as soon as the world pass has finished them, and the HUD-less frame as soon as it has
been captured; only the finished frame waits for Present. The 3090's copy at Present drops to one
lane (~2.6 ms at 4K instead of ~10.3 ms), and the 9060 can start reading the earlier lanes while
the finished frame is still crossing. The copy engine is still serial and shares the PCIe link, so
the gain is bounded by how early in the frame those hooks run. Off is the pre-optimisation
behaviour: all four lanes cross at Present.

`bEarlyUnpack` does the same for the 9060's side. With it off, the 9060's reads were only queued
when Present was called, so lanes that had crossed 3–6 ms earlier sat unread until then. On
(default), each group's read is queued at the same hook as its copy, so the 9060 reads depth,
motion vectors and the HUD-less frame while the game is still finishing the frame, and only the
finished frame is read after Present. Needs `bAsyncUnpack = 1` and `bEarlyTransfer = 1`. It is
re-read every ~60 frames, so it can be flipped while the game runs for an A/B in one session.

`iHalfResDepthMV` sends depth (and optionally motion vectors) at half resolution. Only the
generated frames use them; real frames and the HUD-less colour stay full resolution.
- `1` (default): depth crosses at half resolution (the nearest depth of each 2×2 block) and the
  9060 rebuilds it to full resolution: bilinear on smooth surfaces, the block's own sample at
  object edges, so no blended depth appears on silhouettes. FSR3 keeps working at full resolution
  and still reads every motion vector; only its depth tests are coarser at edges. The rebuild
  starts as soon as the depth lane lands and runs while the other lanes are still crossing.
  Saves 23.7 MB/frame (~1.9 ms) at 4K.
- `2`: motion vectors too (the one of the nearest-depth sample of each block). FSR3 then works
  at half resolution, as it does behind "Performance" upscaling. Saves 47.5 MB/frame (~3.9 ms).
  There is no "motion vectors only" option: FSR3 works at the depth resolution, so half-res
  motion vectors with full-res depth is not a layout it accepts.
- `0`: both at full resolution, as before.

The 3090 shrinks the buffers in the same pass that already fills the shared buffers, so mode 1
and 2 add no pass there; mode 1 adds one ~0.1–0.2 ms compute pass on the 9060, off the critical
path. If anything in the setup fails, the log says so and the plugin uses mode 0.

3. **First test with the fewest moving parts:**
   - Ideally run the first test without ReShade at all (rename `dxgi.dll` temporarily), then with ReShade and without `AutoHDR.addon64`, then with it; see [HDR and ReShade](#hdr-and-reshade-autohdr).
   - ENB is off on your install (`d3d11.dll` is absent), which is required. GPU1 mode doesn't support ENB yet.
4. Launch through F4SE. Borderless/windowed (the plugin only engages for windowed swap chains, as upstream). The monitor must be connected to the 9060 XT; the plugin logs which GPU it renders on and which one presents.

## What to look for

The log is usually `Documents\My Games\Fallout4\F4SE\MGPUScaling.log` (frame generation and the upscaler write to the same file; the upscaler's lines start with `[UPSCALE]`, `[RES]`, `[HOOK]` and so on). All second-GPU lines start with `[FG-GPU1]`:

```
[FG-GPU1] "NVIDIA GeForce RTX 3090" drives no display, but DXGI lists a monitor under it for this process (the Windows GPU preference does that); going by the display configuration
[FG-GPU1] "AMD Radeon RX 9060 XT" drives a display, but DXGI lists no monitor under it for this process (...)
[FG-GPU1] the game picked "NVIDIA GeForce RTX 3090", which drives no display; it renders there and "AMD Radeon RX 9060 XT" presents
[FG-GPU1] game renders on "NVIDIA GeForce RTX 3090"
[FG-GPU1] game window monitor 0x10001 "\\.\DISPLAY5"
[FG-GPU1] display configuration: "\\.\DISPLAY5" is plugged into luid=0x00000000-0x000191ce
[FG-GPU1] adapter[0] "NVIDIA GeForce RTX 3090" ... luid=0x00000000-0x000169d3 ... [game] [DXGI lists the monitor here: GPU preference, ignored]
[FG-GPU1]   output[0] "\\.\DISPLAY5" attached=1 monitor=0x10001
[FG-GPU1]   "NVIDIA GeForce RTX 3090" outputs: 1
[FG-GPU1] adapter[1] "AMD Radeon RX 9060 XT" ... luid=0x00000000-0x000191ce ... [game window's monitor] [display] [DXGI hides its monitor: GPU preference, ignored]
[FG-GPU1]   "AMD Radeon RX 9060 XT" outputs: 0
[FG-GPU1] SELECTED "AMD Radeon RX 9060 XT" (owns the monitor the game window is on)
[FG-GPU1] bridge ready: render GPU copy queue -> cross-adapter heap -> display GPU copy queue, lane by lane (bAsyncUnpack=1)
[FG-GPU1] lane[0] 'final frame' 3840x2160 ... timing=present  lane[1] 'depth' ... timing=world  lane[2] 'motion vectors' ... timing=world  lane[3] 'HUD-less colour' ... timing=hudless
[FG-GPU1] 38.1 game fps | 126.6 MB/frame = 4.70 GB/s | copy on render GPU avg 10.10 ms max 11.67 ms (~12.24 GB/s link) | submit 0.086 ms | max lag 2 | cpu waits 0
[FG-GPU1] presented 74.8 fps | game 38.1 fps | generated 49%
```

The first lines are what Windows 11 with the 3090 as the preferred GPU prints (written from the code;
that setup was tested in game, but its log was not kept): the `output[...]` / `outputs:` lines are DXGI's view, which the GPU preference
rearranges, and the bracketed notes say where it was overruled. Without a GPU preference the two
views agree and those notes and the first two lines do not appear.

The stats lines repeat every 10 s — the bridge's line and the presented/game line have separate
timers, so they appear next to each other. Watch these numbers:
- **copy … ms:** how long the render GPU's copy engine needs per frame. It must stay well under one frame time. It is ~10.5 ms for 126.6 MB (~12 GB/s) whichever GPU runs the copy queue — that is the cross-adapter heap path's ceiling, not one GPU's PCIe link.
- **max lag:** should be 1–2.
- **cpu waits:** should stay at or near 0.
- **presented vs game fps:** `presented` should be close to double `game` (generated ≈ 50%). If `generated` is ~0%, FSR3 accepted the frame but did not insert one — the display GPU is not keeping up.
- **lane count:** with `bSendHudlessFrame = 0` the extra UI-free copy is skipped: three lanes instead of four, 93.4 MB at 4K or 42.2 MB at 1440p.

Three more lines every 10 s show where a frame's time goes:

```
[FG-GPU1] display GPU: unpack (copy queue) avg 10.40 ms max 11.2 ms | direct list avg 1.30 ms max 2.0 ms
[FG-GPU1] timeline ms after Present: 3090 copy 1.2-11.5 | 9060 unpack 4.0-14.1 | 9060 list 14.2-15.5
[FG-GPU1] render thread blocked: FFX Present avg 6.10 ms max 9.0 ms | latency wait avg 0.00 ms max 0.0 ms | D3D11 gate pending 0/480
```

(The numbers above are illustrative, not measured.)

- **unpack:** how long the 9060 needs to read the payload. **direct list:** the 9060's FSR3
  queue work per frame (FSR3's prepare pass; the interpolation itself runs on FFX's own async
  compute queue and is not in this number).
- **timeline:** one frame's path on a single clock (both GPUs are calibrated to the CPU's QPC).
  `3090 copy` starting late means the 3090 was still rendering that frame when the CPU called
  Present. With `bAsyncUnpack = 1` the 9060 unpack should *start* a few ms after the 3090 copy
  starts, not after it ends. With `bEarlyTransfer = 1` the `3090 copy` start is negative: the
  first lanes cross before Present. With `bEarlyUnpack = 1` the `9060 unpack` start is negative
  too; its end is the finished frame's read. Both copy times sum only the phases that ran
  (world, HUD-less, Present), not the idle time between them.
- **render thread blocked:** time the game's render thread spends inside the FFX swap chain's
  Present. That call returns only when the previous frame has been put on screen, so while it
  waits the 3090 receives no new work. A large value here means the 3090 is being held back by
  the transfer → 9060 → present chain, not by its own rendering.
- **D3D11 gate pending:** how often the 3090's next frame had to queue behind a copy that was
  still running. Should be ~0.

One more line, from GPU timestamps on the 3090 itself:

```
[FG-GPU1] render GPU: idle between frames avg 2.40 ms max 6.10 ms (95% of frames idle >= 0.5 ms) | frame from its first command to its end avg 13.10 ms max 15.20 ms (620 frames measured)
```

- **idle between frames:** how long the 3090 had nothing to do between finishing a frame and
  receiving the first work of the next one, i.e. while the game sat in Present, simulated the
  next frame, or slept in a frame limiter. Near 0 below the frame cap means the 3090 itself is the
  limit; several ms below the cap means it is waiting (for the chain, or for the CPU). At the
  frame cap it is simply the headroom.
- **frame from its first command to its end:** the 3090's time per frame once work arrives
  (includes gaps inside the frame where the CPU had not sent the next draws yet).

With `iHalfResDepthMV = 1` one more line appears:

```
[FG-GPU1] depth rebuild on display GPU avg 0.15 ms max 0.30 ms | done 3.0 ms after Present
```

`done` should be earlier than the `9060 unpack` end in the timeline; then the rebuild costs the
chain nothing.

If anything fails during setup, the plugin logs why and falls back to running FG on the 3090. Failures that happen after the swap chain exists are logged as `CRITICAL`.

## FSR SDK 2.4 (`iFSRRuntime`)

The package ships two FSR runtimes in `Data\F4SE\Plugins\MGPUScaling\FidelityFX\`:

| `iFSRRuntime` | DLLs | what you get |
|---|---|---|
| `0` | `amd_fidelityfx_dx12.dll` (FidelityFX SDK 1.1.4) | FSR 3.1.4: what this mod was developed and measured with |
| `1` (default) | `amd_fidelityfx_loader_dx12.dll` + `amd_fidelityfx_framegeneration_dx12.dll` (FSR SDK 2.4) | the newest frame generation the driver and Windows offer |

With 2.4, AMD's runtime offers different parts depending on the machine. The plugin takes the newest
of each unless you pin one:

| part | Windows 10, RX 9060 XT, Adrenalin 26.10.1 (probed) | Windows 11, RX 9000 |
|---|---|---|
| swap chain (pacing) | **4.0.0**, or 3.1.7 | 4.0.0, or 3.1.7 |
| interpolator | **3.1.6** only (analytical, same kind as 3.1.4) | **4.0.2** (FSR 4 ML), or 3.1.6 |

The Windows 10 column was probed before this PC was upgraded to Windows 11; the Windows 11 column is
AMD's documented behaviour and has not been re-probed on it. The first runs after the upgrade do not
count: with the roles inverted (see the top), they asked the 3090 and got `3.1.7` / `3.1.6`. The first
run with the 9060 presenting (2026-11-09) confirmed the Windows 11 column: `swap chain providers
offered: 4.0.0, 3.1.7`, `frame generation providers offered: 4.0.2, 3.1.6`, in use 4.0.0 + 4.0.2.

So on Windows 10 the image-quality upgrade (ML frame generation) is **not available**; AMD's runtime
does not even list it. What 2.4 can still bring there is the 4.0.0 swap chain's
**driver-assisted frame pacing**, where the AMD driver times the presents instead of a user-mode
busy-wait. AMD enables it on its own, with no API flag and no report, when all of these hold:
RX 9000-series GPU, Adrenalin 26.10.1+, swap chain 4.0.0+, fullscreen or a borderless window
covering the display, VSync off, and presenting inside the display's VRR range (AMD says it is tuned
for Windows 11 with hardware-accelerated GPU scheduling). The log reports the two conditions the
plugin can see: whether the window covers the display, and VSync. To see whether pacing improved,
compare frame-time variance (e.g. PresentMon's `msBetweenPresents`) against `iFSRSwapChainProvider = 1`,
which keeps everything else the same but uses the 3.1.7 swap chain and its own pacing.

What the plugin does differently with 2.4 (the 1.1.4 path is untouched):
- adds the version descriptors 2.x needs to enable its newer API;
- dispatches `PrepareV2` with the camera's world position (`posAdjust`) and its up/right/forward
  vectors (FSR 4 ML needs them; FSR 3.1.6 accepts them), the vertical FOV from the projection
  matrix instead of a fixed 1 rad, and `reset` when generation restarts after a menu or loading;
- declares the HUD-less copy's RGBA8 format when the back buffer is something else;
- logs which providers were offered and which are used, where the DLLs were loaded from, FSR's own
  error/warning messages, and failed generated-frame dispatches.

If the 2.4 DLLs are missing or a context cannot be created, the plugin logs why and runs that
session on 1.1.4. The 2.4 loader looks for `amd_fidelityfx_framegeneration_dx12.dll` next to
`Fallout4.exe` *before* its own folder, so a copy left there by another mod (OptiScaler, for
example) wins; the log warns when that happens.

```
[FSR] runtime: FSR SDK 2.4 (Data\F4SE\Plugins\MGPUScaling\FidelityFX\amd_fidelityfx_loader_dx12.dll)
[FSR] swap chain providers offered: 4.0.0, 3.1.7
[FSR] swap chain provider: 4.0.0 (driver-assisted frame pacing capable)
[FSR] frame generation providers offered: 3.1.6
[FSR] FSR 4 (ML) frame generation is not offered on this system. AMD requires Windows 11 and an RX 9000-series GPU for it; ...
[FSR] in use: swap chain 4.0.0 + frame generation 3.1.6
[FSR] window 3840x2160 at (0,0), display 3840x2160 at (0,0): covers the display
[FSR] generated frames in the first 10 s: 690 dispatched, none failed
```

(Expected lines, not a capture; the provider lists are what the Windows 10 probe returned.)

## Frame generation is not upscaling

GPU1 mode on its own renders the game at whatever resolution the game asks for, on the 3090, and
doubles the presented frames on the 9060. It does **not** upscale. If the game renders 4K natively,
the 3090's frame rate is the ceiling: 38 game fps becomes ~75 presented fps, not 100+. To raise the
base frame rate, let the 3090 render fewer pixels: either FSR 4.1 on the 9060 (next section), or
the upscaler's DLSS/FSR 3.1 on the 3090 (`[Upscaling] iUpscaleMethodPreference = 2` or `1`);
FSR3 FG then interpolates the upscaled output.

## FSR 4.1 upscaling on the second GPU

The 9060 XT can also run **AMD FSR Upscaling 4.1** (the ML upscaler, SDK 2.4; it works on
Windows 10). The 3090 then renders the 3D scene at the quality mode's resolution (Quality: 2560×1440
for 4K) and the 9060 upscales it to 4K before frame generation. The 3090 keeps drawing the UI at 4K,
so text stays sharp. This frees most of the 3090's 4K render time for heavier graphics settings
and mods. The RTX 3090 itself is not offered FSR 4.

Switch it on in the upscaler's settings (MCM → MGPU Scaling → Upscaling → Upscale Method
Preference → "AMD FSR 4.1 (second GPU)", or `iUpscaleMethodPreference = 3` in the `[Upscaling]`
section of `Data\MCM\Settings\MGPUScaling.ini`) and restart. It is the shipped default. The
upscaler is part of the same DLL (FO4Upscaling's `Upscaling` at the same commit, plus this mode); no
other `Upscaling.dll` may be installed (doodlum's cannot do this, and two would hook the same calls).
Needs `iFSRRuntime = 1` and `bAsyncUnpack = 1`. `iQualityMode` picks the render resolution
(1 Quality 1.5×, 2 Balanced 1.7×, 3 Performance 2×, 0 Native AA). Changing the method or the
quality mode needs a restart: the transport and the FSR 4.1 context are sized for it at startup.

How a frame goes:

| step | GPU | what |
|---|---|---|
| scene | 3090 | the upscaler renders at e.g. 2560×1440 into the corner of the 4K targets, with a sub-pixel jitter each frame and **TAA off** (FSR 4.1 is the anti-aliasing) |
| world hook | 3090 → 9060 | render-resolution depth and motion vectors cross PCIe |
| end of imagespace | 3090 → 9060 | the render-resolution scene (UI-free) crosses; the 3090 fills its 4K frame buffer from it with a plain bilinear stretch, so the UI is drawn over a background |
| as soon as those land | 9060 | **FSR 4.1** 1440p → 4K on the direct queue, while the 3090 still draws the UI |
| Present | 3090 → 9060 | the finished 4K frame (stretch + UI) crosses |
| Present | 9060 | the UI is composited over the FSR 4.1 result (the 9060 rebuilds the same stretch, keeps what the UI changed); frame generation gets the FSR 4.1 result as its UI-free frame |

Payload at 4K Quality: 14.75 MB scene + 14.75 MB depth + 14.75 MB motion vectors + 33.2 MB finished
frame ≈ **77 MB/frame** (126.6 MB at native 4K). `iHalfResDepthMV` is ignored here (depth and motion
vectors already cross at the render resolution). `bSendHudlessFrame = 0` is not ignored: frame
generation is then handed a copy of the composed frame instead of the FSR 4.1 result, so the HUD is
interpolated with the scene, and `bFreezeHudMotionVectors` zeroes the motion vectors under the HUD to
hold it still. With the UI-free frame on, that switch only freezes the scene behind the HUD (visible
through translucent HUD parts), because the HUD itself is composited after generation. The copy is a
plain GPU copy into an RGBA8 texture, so it needs an RGBA8 back buffer: under an HDR add-on the
frame is shown without frame generation instead (logged), not black. AMD lists FSR 4.1 at ~3.4 ms on
a 9060 XT at 4K; a probe on this PC measured 3.3 ms.

Menus the upscaler renders natively (Pip-Boy, containers, loading) are shown as rendered, without
frame generation. If anything is missing (another `Upscaling.dll` installed, SDK 1.1.4, no FSR 4
provider), the log says why and the game renders at full resolution as before. An HDR back buffer
no longer turns it off; see the next section.

```
[FSR4] upscaler providers offered for the display GPU: 4.1.2, 3.1.6, 2.3.4
[FSR4] upscaler 4.1.2 on the display GPU: 2560x1440 -> 3840x2160, 317.5 MB
[FSR4] upscaling on the display GPU: the game renders 2560x1440 (Quality), FSR 4.1.2 upscales to 3840x2160
[FSR4] lanes bound: scene, depth and motion vectors 2560x1440 -> FSR 4.1 3840x2160; the UI is composited over it
[FSR4] 2560x1440 -> 3840x2160: 710 frames upscaled, 0 at full resolution, 1 history resets | FSR 4.1.2 on the display GPU avg 3.30 ms max 3.80 ms | UI composite + copy avg 0.25 ms max 0.40 ms
```

(Expected lines, not a capture.) The upscaler's side, in the same log:
`[UPSCALE] FSR 4.1 on the second GPU is ready: rendering at the quality mode's resolution`.

## HDR and ReShade (AutoHDR)

ReShade attaches to the only real swap chain in the process: the one FFX creates on the 9060. Its
effects therefore already run **after** FSR 4.1, after the UI composite and after frame generation,
on real and generated frames alike (twice per game frame, so effects cost double).

ReShade's AutoHDR add-on changes that swap chain to `R10G10B10A2_UNORM` (HDR10) or
`R16G16B16A16_FLOAT` (scRGB, `UseHDR10=0`) while FFX creates it. Before this version the plugin
kept writing 8-bit frames into it with plain copies, which do not convert: colour garbage,
freezes, and possibly the crash after loading a save. Now:

- the plugin reads the format the swap chain's back buffers really have after creation and logs it
  (`[FSR] the swap chain was created as format 28 but is format 24 ...`). It reads the buffers, not
  `GetDesc1`: ReShade reports the format the game asked for there when an add-on changed it (the
  first MGPU Scaling build trusted `GetDesc1`, missed the change and showed a black screen);
- frame generation is created for that format, told that its UI-free frame stays RGBA8, and left
  in SDR mode (the content is SDR in a 10/16-bit container; HDR is applied afterwards by
  `AdvancedAutoHDR.fx`/`AutoHDR.fx` in ReShade);
- the UI composite writes the swap chain's format, and native frames (menus, loading) and every
  frame without FSR 4.1 go through a small conversion pass on the 9060
  (`[FG-GPU1] the swap chain is R10G10B10A2_UNORM (HDR10) ... every frame is converted`);
- the game-side lanes stay RGBA8, so nothing changes on the 3090 or across PCIe.

**Single-GPU mode** (`bFrameGenOnSecondaryGPU = 0`, game and monitor on the same GPU) works the same
way, on that GPU: FFX's swap chain is the one ReShade attaches to, frame generation is created for
the format AutoHDR gave it with an RGBA8 UI-free frame, and the finished frame goes into the back
buffer through the same conversion pass (`Copy` in `SecondaryUpscaleCS.hlsl`: one full-screen read,
write and copy; not measured yet, expected well under 1 ms at 4K) instead of a plain copy, with
frame generation on or off. The log says
`[FG] the swap chain is R10G10B10A2_UNORM (HDR10) (format 24), the game renders R8G8B8A8_UNORM
(format 28): every frame is converted on the game's GPU ...` at startup and
`[FG] first frame converted into the ... back buffer on the game's GPU` once frames flow. (Before
this, single-GPU mode logged `[FG] the swap chain is format 24 but the game renders format 28 ...
only bFrameGenOnSecondaryGPU = 1 converts between them` and showed a wrong picture.) FSR 4.1 is a
second-GPU feature and is not involved; the upscaler on the game's GPU (`iUpscaleMethodPreference`
1 or 2) renders before all of this and is unaffected. DLSS-G and XeSS frame generation
(`iFrameGenType` 1/2, not shipped in this package) get the converted frame too but are not told the
UI-free frame's format; the log warns.

Limits: this needs `iFSRRuntime = 1`. The 1.1.4 runtime cannot be told that the UI-free frame stays
RGBA8, so under an HDR back buffer frame generation stays off for the session (logged as an error;
the picture is right, just not doubled). Giving FFX no UI-free frame instead leaves it without a
scene (black screen, seen while testing `bSendHudlessFrame = 0`). **Change HDR settings
only with the game closed**: toggling HDR in the AutoHDR overlay resizes the swap chain behind FFX
and this plugin, which cannot see it (ReShade hides the format change), so expect a wrong picture
until a restart. ReShade
effects that sample the back buffer as sRGB warn that a 10-bit buffer has no sRGB view (CAS, MagicHDR
in this install); the HDR-aware ones (`AdvancedAutoHDR`, `lilium_*`) are unaffected. Use scRGB if
the others look wrong in HDR10.

The game-side `GetDesc` of the swap chain now reports the game's own RGBA8 format, matching the
texture `GetBuffer` returns, instead of the real (10-bit) one; D3D11 overlays create matching views.

## Performance budget

The payload and copy time scale with resolution and with the number of lanes (one lane per buffer,
two slots each):

| target | lanes | payload | copy |
|---|---|---|---|
| 3840×2160 | 4 (all) | 126.6 MB | ~10.5 ms |
| 3840×2160 | 4, `iHalfResDepthMV = 1` | 102.8 MB | ~8.5 ms (estimate) |
| 3840×2160 | 4, `iHalfResDepthMV = 2` | 79.1 MB | ~6.6 ms (estimate) |
| 3840×2160 | 3 (no extra UI frame) | 93.4 MB | ~7.8 ms |
| 2560×1440 | 4 (all) | 56.3 MB | ~4.7 ms |
| 2560×1440 | 3 (no extra UI frame) | 42.2 MB | ~3.5 ms |

The copy runs at ~12 GB/s whichever GPU drives it, so those are the practical numbers. The
display GPU then reads the same payload back out of system memory (~the same time again).

What limits the frame rate is less each step's duration than the chain they form. The FFX swap
chain has two back buffers, so its Present for frame N returns only once frame N-1 is on screen.
Until then the game's render thread, and therefore the 3090, gets no new work. Everything between
"3090 finished frame N-1" and "frame N-1 presented" (copy, read, FSR3, pacing) is on the critical
path. That is why `bAsyncUnpack` streams the buffers lane by lane: the 9060's read overlaps the
3090's copy instead of following it. See SESSION-NOTES.md for the numbers.

`bExtraFrameInFlight = 1` gives the FFX swap chain three back buffers, and every per-frame resource
a third slot. Present for frame N then waits for frame N-2 instead of N-1, so the chain's latency
stops setting the frame rate as long as it fits in two frame times; the limit becomes the slowest
single stage (the 3090, the CPU, the copy engine or the 9060). The price is one frame more between
input and screen (~14 ms at 71 fps), and one more slot of every lane (~75 MB of staging memory and
~75 MB on each GPU at 4K Quality). Worth it when `render GPU: idle between frames` shows the 3090
waiting several ms per frame below the frame cap; useless when the 3090 itself is at its limit.

### Host-memory cost

The payload is not just PCIe traffic. Every byte is written to system memory by the render GPU's
copy engine and read back out by the display GPU's, so a 4K frame at 38 fps is ~4.8 GB/s written
and ~4.8 GB/s read — DRAM the game's own thread is also using. D3D12's own route makes that unavoidable:
[cross-adapter resources "are only supported in system memory"](https://learn.microsoft.com/en-us/windows/win32/direct3d12/shared-heaps),
in `D3D12_MEMORY_POOL_L0`. No Windows API offers a peer-to-peer transfer between GPUs of different
vendors (CUDA's peer access does not exist under WDDM, AMD's DirectGMA was workstation-only and is
gone), but one works anyway on this PC: see [Direct GPU-to-GPU copy](#direct-gpu-to-gpu-copy-bdirectgpucopy-experimental).

`tools/bridge-load/` replays exactly that traffic on one GPU with the same burst-then-gap timing,
so the question can be answered with the mod out of the loop: run the game on the 3090, run the
tool on the 9060, compare `msBetweenPresents`, 1% low and average fps at 77 MB and at 154 MB per
frame. Measured on this PC it does 13.3 GB/s unpaced (5.7 ms up, 5.9 ms down per 77 MB) and 44%
duty at 38 Hz. For other settings, payload MB × game fps × 2 ≈ GB/s of DRAM traffic.

## Direct GPU-to-GPU copy (`bDirectGPUCopy`, experimental)

Off by default. With it on, the 3090 writes each lane **straight into the 9060's VRAM**: one PCIe
trip instead of two, and none of the payload touches system RAM.

How: the 9060 has Resizable BAR on, so its whole VRAM can be mapped for the CPU. A small Vulkan
device on the 9060 (`PeerCopy`) allocates one such block per lane and maps it; the 3090's D3D12
device opens that CPU address with `OpenExistingHeapFromAddress` as if it were system memory, and
its copy engine's writes then go over PCIe to the 9060. No driver documents this; both simply
accept the address. The 9060's D3D12 device (FFX) cannot see Vulkan's block on Windows 10, so the
9060's Vulkan copies each lane from the block into a D3D12 heap it imported (local VRAM, ~150 GB/s,
~0.2 ms for the 33 MB finished frame), and the usual unpack reads from there. A D3D12 fence imported
into Vulkan as a timeline semaphore orders the two per lane: the copy queue signals "landed" once
the 3090's transfer fence passes, Vulkan copies and signals "copied", the copy queue unpacks.

| 77 MB FSR 4.1 payload (p2p-probe `--bench`) | system memory | direct |
|---|---|---|
| 3090 copy engine | 5.9 ms (13.2 GB/s) | 7.6 ms (10.2 GB/s) |
| 9060 read | 5.6 ms (across PCIe) | ~0.5 ms (local) |
| finished frame, last on the chain | 2.52 + 2.41 = 4.9 ms | 3.24 + ~0.2 = ~3.5 ms |
| system RAM traffic | 154 MB per frame | none |

So about 1.4 ms off the end of the chain, plus the system RAM contention (~3% of frame time when
the game is CPU-bound, see [Host-memory cost](#host-memory-cost)). The 3090's copy engine is busier
(card-to-card writes run ~23% slower than writes to RAM on X299), but it runs beside rendering. Each
step was tried first with `tools/p2p-probe` on this PC, including the mod's per-frame sequence 300
times with the 9060's work queued before the 3090 writes (0 wrong frames).

**Guard.** The danger is the 9060's driver moving a block while the 3090 still writes to its old
address. Under 19.5 GB of VRAM pressure on the 16 GB card the blocks did not move (`--evict`), and
they get Vulkan's highest memory priority; the guard covers the rest. The 3090 writes a check value
(the lane's transfer number) behind each lane's data; at every frame start the CPU reads the values
of the copies that finished, through the block's CPU mapping, which follows the block wherever the
driver keeps it (one uncached read per lane, ~1–2 µs). If a value is missing where the 3090 wrote it,
or a Vulkan submission fails, the frames go back through system memory from the next frame on
(those heaps stay allocated for this) and **frame generation is switched off for the rest of the
session**, so it cannot go unnoticed:

```
[FG-GPU1] direct GPU copy guard: lane 'final frame' slot 1: the render GPU wrote check value ... into the display GPU's VRAM block, but the block holds ... Frames cross through system memory from the next frame on and frame generation is OFF for the rest of the session. ...
```

**At exit.** The 3090's view of a block must close before the block is freed, and both while the
drivers are still loaded: the first build did it the other way round in its destructors, after the
drivers had begun unloading, and the PC hung on Alt+F4. The plugin now hooks the two ways a process
ends, `ntdll!RtlExitUserProcess` (behind `ExitProcess` and the CRT's `exit`) and
`kernelbase!TerminateProcess` (Fallout4.exe imports both). There it waits up to 2 s for the copies in
flight, closes the 3090's views, then frees the blocks, and logs it. The render thread holds a lock
for each frame's bridge work, so this never lands mid-frame. The hooks are only installed when the
entry points start with known prologue instructions (the detour library cannot relocate RIP-relative
ones); if they cannot be installed, the direct copy stays off. After the plugin's own DLL detach
has begun, nothing calls into the Vulkan driver any more.

Needs `bAsyncUnpack = 1`, Resizable BAR (Smart Access Memory) on the 9060 and its Vulkan driver.
If any of it is missing, or any setup step fails, the log says which and the frames go through
system memory as before, with frame generation unaffected. Costs ~2× the payload × frames in flight
of extra VRAM on the 9060 (block + landing heap; ~460 MB at 4K FSR 4.1 Quality with 3 frames in
flight). Not tested: sleep/resume or a driver reset while the game runs.

```
[Frame Generation] bDirectGPUCopy: true (...)
[FG-GPU1] direct GPU copy: Vulkan 1.4.x on "AMD Radeon RX 9060 XT", async compute queue (family 1), memory priority on, timestamps on
[FG-GPU1] bridge ready: render GPU copy queue -> the display GPU's VRAM, directly (bDirectGPUCopy=1) -> display GPU copy queue, lane by lane (bAsyncUnpack=1)
[FG-GPU1] direct GPU copy: lane 'final frame': 95.1 MB VRAM block (CPU-visible, mapped at 0x...) + 95.1 MB landing heap on the display GPU
[FG-GPU1] lane[0] 'final frame' 3840x2160 ... (direct to the display GPU's VRAM)
...every 10 s:
[FG-GPU1] direct GPU copy: 660/660 frames into the display GPU's VRAM | its Vulkan copy into the landing heaps (async compute queue) avg 0.5 ms ... | check values found 2640 (late 0)
```

(Expected lines, not a capture.) The 10 s line's `copy on render GPU` then shows the card-to-card
rate (~10 GB/s), and `display GPU: unpack` the local copy out of the landing heap.

## Known risks / first things to check

- **FSR SDK 2.4 has had one in-game run.** At 4K it used swap chain 4.0.0 + FSR 3.1.6, doubled
  64 → 128 fps with no errors and felt smooth, but it has not yet been A/B-tested against 1.1.4 or
  the 3.1.7 swap chain at the same settings. If 2.4 misbehaves (no generated frames,
  `[FSR] generating a frame failed`, stutter, a crash), set `iFSRRuntime = 0` to go back to
  exactly the previous behaviour.

- **`bDirectGPUCopy` is new.** First in-game run (2026-11-08, CS, ReShade AutoHDR, FSR 4.1 Quality, 3 frames in flight): every frame crossed directly at the 70.9 fps cap, check values never late, but **Alt+F4 hung the PC** (no bugcheck, power button). The first build freed the 9060's VRAM blocks in its destructors, after the drivers had started unloading and while the 3090 still had them open. The current build releases them in the right order before anything unloads (see "At exit" in [Direct GPU-to-GPU copy](#direct-gpu-to-gpu-copy-bdirectgpucopy-experimental)); `tools/p2p-probe --exit-gpu` checks that with copies in flight. Not yet re-tested in game: after quitting, the log should end with `direct GPU copy: the game is exiting; closed the render GPU's views ...`. If frame generation turns itself off, look for `direct GPU copy guard`; for anything else odd, set it back to 0. It creates a Vulkan instance in the game process with implicit layers disabled (`VK_LOADER_LAYERS_DISABLE=~implicit~` while it is created), so overlays' Vulkan layers stay out.
- **Cross-adapter heap 3090 → 9060 XT.** MGPU Bridge proved AMD → NVIDIA. This direction should work the same way, and if it fails the code tries the reverse automatically. If both fail, the log shows the HRESULT and frame generation stays on the 3090.
- **A failure after startup is not recovered.** Once the bridge is up, the first failed HRESULT — including the 2 s timeout waiting for the previous frame's copy if the secondary GPU's queue stalls — disables the transport for the rest of the session. `secondaryGPU` stays selected, so the game keeps running but no new frame reaches the screen (`CRITICAL [FG-GPU1] transport is down`). Restart the game; there is no automatic single-GPU fallback after this point.
- **HDR add-ons are followed at startup only.** A swap-chain format set while FFX creates it is handled (previous section); one changed later (HDR toggled in game) is not seen and needs a restart. The finished-frame copy refuses formats it cannot copy into (it used to compare only size and row pitch, which 8-bit and 10-bit share), so if the conversion is not set up for some reason the screen stays black and the log has `lane 'final frame' is ... format 28 but its destination is ... format 24; skipping the copy`. In single-GPU mode a conversion that could not be set up is logged as an `[FG]` error and the plain copy stays (wrong colours, as before this version).
- **Other upscaling / frame generation plugins.** `AAAFrameGeneration.dll` (FO4Upscaling, old FG-GPU1) keeps MGPU Scaling off for the session (message box); another `Upscaling.dll` (doodlum's, FO4Upscaling's) turns MGPU Scaling's upscaler off. Remove them. The upscaler's hooks are installed at F4SE's PostLoad, i.e. after every plugin's own load, where `Upscaling.dll` used to install them; plugins that sort after "Upscaling" and hook the same render calls would now be nested inside it.
- **FO4 Community Shaders**: its current source disables all of CS when an `Upscaling.dll` below plugin version 1.2 is installed (it takes it for doodlum's); the old FG-GPU1 `Upscaling.dll` was version 1.0 (the CS build installed here, `bcad629`, predates that check). MGPU Scaling ships no `Upscaling.dll`. Keep CS's own Upscaling and FrameGeneration features off: they would create a second swap chain and a second ReShade runtime on the 3090. CS's **frame annotations** (`[diagnostics] frame_annotations`) hook `DrawWorld::Forward` and `DrawWorld::Imagespace` before this plugin does; FO4Upscaling's detour library cannot sit on top of another plugin's hook (it copies the hook's jump unrelocated), which crashed the game on the first world frame. MGPU Scaling now chains behind such a hook instead and logs `[HOOK] ... was already hooked by another plugin ...; chained behind it`.
- **PCIe budget at 4K.** Start at 2560×1440 if the copy time is too high. The log prints the exact payload per lane; colour, HUD-less colour and depth are 4 B/px, but if the game's motion-vector target is RGBA16F (8 B/px) the total is ~166 MB/frame at 4K rather than the 133 MB quoted above. Trimming the depth/motion copies to the render resolution is the next optimisation (not done yet).
- **DLSS-G / XeSS runtime DLLs are not in this package.** GPU1 mode forces FSR3 and never loads Streamline or XeSS, so this is fine. Switching `bFrameGenOnSecondaryGPU = 0` and `iFrameGenType = 1/2` would need those DLLs from the upstream package.
- **Frame cap.** `bFrameLimitMode = 1` caps the base frame rate to about half the refresh rate (≈72 fps at 144 Hz) when VSync is off. That's upstream behaviour.

## Licence

This repository is GPL-3.0; see [LICENSE](LICENSE). Upstream FO4Upscaling is GPL-3.0 with the
exceptions in its `EXCEPTIONS.md`. The README there says MIT, but `LICENSE`/`COPYING` are GPL-3.0.
The files under `overlay/` and the patch are derived from it and carry the same licence.
