# bridge-load

Reproduces the second-GPU bridge's **host-memory** traffic without the game, so you can
ask one question in isolation: does writing and reading the frame's payload through
system memory cost the game frames?

The bridge moves ~77 MB per 4K frame into a cross-adapter heap in system memory on the
render GPU and reads it back out on the display GPU. That is DRAM traffic, not just PCIe
traffic, and DRAM is shared with the game's own thread. This tool produces the same
traffic on one GPU:

```
cycle:   system memory ──▶ VRAM      (copy engine reads host DRAM)
         VRAM ──▶ system memory      (copy engine writes host DRAM)
         ...idle until the next cycle starts
```

One cycle at the defaults moves **154 MB of host DRAM traffic in ~11.6 ms**, then idles
for the rest of a 26.3 ms frame — the same burst-then-gap shape the bridge produces.

## Build

```powershell
pwsh -ExecutionPolicy Bypass -File D:\projects\mgpu-scaling\tools\bridge-load\build.ps1
```

Loads the Visual Studio environment the same way `build-local.ps1` does and compiles the
single `main.cpp` with `cl /W4 /WX`. Output: `bridge-load.exe` beside the script.

## Run

```powershell
# what the mod does at 4K / 38 game fps, on the 9060 XT
.\bridge-load.exe -Gpu 9060 -PayloadMB 77 -Fps 38

# double the payload, to see whether the effect scales with traffic
.\bridge-load.exe -Gpu 9060 -PayloadMB 154 -Fps 38

# one direction only, to separate the writes from the reads
.\bridge-load.exe -Gpu 9060 -Mode up
.\bridge-load.exe -Gpu 9060 -Mode down

# peak, no pacing: how much the link can actually do
.\bridge-load.exe -Gpu 9060 -Fps 0 -Seconds 5
```

| option | default | |
|---|---|---|
| `-Gpu` | `9060` | adapter description substring |
| `-PayloadMB` | `77` | bytes per direction, decimal MB (the FSR 4.1 4K payload) |
| `-Fps` | `38` | cycle rate to pace to; `0` = unpaced |
| `-Seconds` | `0` | stop after this long; `0` = until Ctrl+C |
| `-Mode` | `both` | `both` = 154 MB/cycle, `up` or `down` = 77 MB/cycle |
| `-BurstGapMs` | `0` | idle between the two copies, to spread the burst |
| `-List` | | list adapters and exit |

Ctrl+C prints the run summary and exits.

## Measured here (i9-7940X, X299, PCIe 3.0 x16, RX 9060 XT)

```
paced 38 Hz:   up 5.76 ms   down 5.90 ms   busy 44%   5.85 GB/s host DRAM
unpaced:       86 cycles/s   13.3 GB/s    (77 MB in 5.7 ms → link-limited)
```

13.3 GB/s is a PCIe 3.0 x16 link doing DMA, so the 9060 XT is on CPU lanes at x16 and
the payload fits its frame with room to spare. The mod's own 10.5 ms copy of 126.6 MB
(12.1 GB/s) is the same link speed, not a cross-adapter penalty.

## The A/B

The point is not to watch the number in the tool's output, it is to watch the game's.

1. **Fix everything else.** Same save, same position and look direction, same settings,
   VSync off. Let it settle 10 s before measuring — streaming and shader work dominate
   the first seconds after a load.
2. **Keep the cabling identical across arms.** If the monitor is on the 9060 for A, it
   must be on the 9060 for B. Otherwise you measure two changes.
3. **Arm A** — game alone, no second-GPU traffic. Presents on the 3090
   (`bFrameGenOnSecondaryGPU = 0`, or the mod off entirely).
4. **Arm B** — same spot, `bridge-load.exe -Gpu 9060 -PayloadMB 77 -Fps 38` running in a
   second window.
5. **Arm C** (optional) — `-PayloadMB 154`, twice the traffic. A dose-response is much
   harder to explain away than a single delta.
6. Capture 30–60 s per arm with `../presentmon-capture.ps1`, summarise with
   `../presentmon-summary.ps1`, and repeat A after C to catch session drift (A-B-C-A).

What to compare, in order of usefulness:

- **CPU frame time / `msBetweenPresents` distribution**, not just average fps. Contention
  shows up as a fatter tail before it shows up in the mean.
- **1% low.** A latency-sensitive main thread loses its worst frames first.
- **Average fps.** The blunt instrument; a 1–2% delta here is noise.

Interpretation:

- B within ~1–2% of A, and C within ~2–3%: host-memory contention is not costing you
  frames at this traffic level. That also covers the mod's own traffic, which is the same
  order of magnitude.
- B or C consistently worse, with the gap growing from B to C: contention is real, and
  payload reduction (`iHalfResDepthMV`, `bSendHudlessFrame = 0`, packing motion vectors)
  buys real frames, not just copy time.
- A and B identical but the game still feels worse with the mod on: look at the copy and
  present timing lines in `MGPUScaling.log` instead — that would be chain latency, not
  memory.

## Limits

- It exercises **one** link (the 9060's), where the bridge uses two (3090 writes, 9060
  reads). The DRAM traffic — the thing being tested — is the same 77 MB each way per
  frame; only the PCIe timing differs.
- It does not touch the 3090, the game's own allocations, or the desktop compositor.
- The destination is a `READBACK` heap and the source an `UPLOAD` heap, where the bridge
  uses a `SHARED_CROSS_ADAPTER` heap. Both are host DRAM; the copy engine reads and
  writes them at link speed either way (measured above).
- It keeps one core busy only in the pacing wait, which is a kernel wait, not a spin, so
  it does not compete for CPU time with the game.
