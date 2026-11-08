# p2p-probe

Can the render GPU write **straight into the display GPU's VRAM**, without going through system
memory? This is the test bed for `bDirectGPUCopy` (the mod's `PeerCopy`): every step the mod relies on
was tried here first, on the RTX 3090 (render) + RX 9060 XT (display) this was built for.

The trick: the display GPU maps a VRAM block for the CPU through its **Resizable BAR**. That mapping is
an ordinary address in the process, and the render GPU's D3D12 device accepts it in
`OpenExistingHeapFromAddress` (and Vulkan in `VK_EXT_external_memory_host`) as if it were system
memory. Its copy engine's writes then go over PCIe to the other card. No vendor ships a cross-vendor
peer-to-peer API on Windows; this is not one either, just an address both drivers accept.

## Build

```powershell
pwsh -ExecutionPolicy Bypass -File D:\projects\mgpu-scaling\tools\p2p-probe\build.ps1
```

`cl /W4 /WX`, Vulkan headers from `overlay\include` (vendored), `vulkan-1.dll` loaded at run time.

## Run

| mode | what it does | risk |
|---|---|---|
| (none) | allocates and maps the AMD block, asks both NVIDIA APIs to import it | none, no GPU writes |
| `--dma` | NVIDIA copies 64 MiB into the AMD block (Vulkan and D3D12), AMD checks every dword | P2P writes |
| `--bench` | every direction at the mod's lane sizes on D3D12 copy queues | P2P reads and writes both ways |
| `--interop` | how the AMD D3D12 device can use the block (three routes) | P2P writes |
| `--evict` | `--interop`, then 19.5 GiB of VRAM pressure on the AMD GPU: does the block move? | reads only across PCIe; the desktop may stutter for a few seconds |
| `--pipeline [--compute]` | the mod's per-frame sequence 300 times, the AMD side queued before the NVIDIA write; `--compute` uses an async-compute queue as the mod does | P2P writes |
| `--contention` | the 3090 reads system RAM on its direct queue (bulk 256 KB and tiny 256 B copies, like the game's dynamic buffers) while its copy queue is idle, writes system RAM, or writes the 9060's VRAM | P2P writes |
| `--exit-hook`, `--exit-hook-terminate` | installs the mod's exit hooks (same detour library, same prologue check) and ends the process through ExitProcess / TerminateProcess: do the hooks fire and pass the exit code on? | none, no GPU work |
| `--exit-gpu`, `--exit-gpu-terminate` | ends the process while 40 x 64 MiB P2P copies are still running; the hook waits for them, closes the NVIDIA view, then frees the AMD block (the mod's `ReleaseDirectCopyForExit`) | P2P writes; the order that hung the PC is NOT exercised |

The exit modes need the detour library from the mod's build tree (`build.ps1 -DetoursDir`, default
`D:\fgb\FO4Upscaling\include\detours`); without it the probe builds without them. The probe is built
with `/MD` because that library is.

Anything unsupported by the root complex could in principle hang or crash Windows; on X299 none
of it did, and the System log showed no WHEA/PCIe events.

## Measured (2026-11-07: i9-7940X, X299, PCIe 3.0 x16 both, Win10 19045, NVIDIA 591.86, AMD 26.10.1)

Imports: both NVIDIA APIs accept the AMD BAR pointer (`HOST_ALLOCATION`; `HOST_MAPPED_FOREIGN_MEMORY`
is not supported). The AMD block stays in VRAM after the imports.

`--bench`, D3D12 buffer copies, ms per copy and GB/s:

| path | 14.7 MB | 33.2 MB | GB/s |
|---|---|---|---|
| 3090 -> system RAM (today's bridge, step 1) | 1.12 | 2.52 | 13.2 |
| system RAM -> 9060 (step 2) | 1.07 | 2.41 | 13.7 |
| **3090 writes 9060 VRAM** (copy or direct queue) | 1.44 | 3.24 | 10.2 |
| 9060 writes 3090 VRAM | 1.49 | 3.37 | 9.9 |
| 3090 reads 9060 VRAM | 3.50 | 7.94 | 4.2 |
| 9060 reads 3090 VRAM | 2.95 | 6.66 | 5.0 |
| 9060 VRAM -> VRAM (D3D12 copy queue) | 0.18 | 0.39 | ~84 |
| 9060 VRAM -> VRAM (Vulkan, graphics or async compute) | | | ~150 |

Writes across the X299 root complex run ~23% slower than writes to system RAM; reads are slow, so the
render GPU always pushes. For the 77 MB FSR 4.1 payload the finished frame (last on the chain) goes from
2.52 + 2.41 = 4.93 ms to ~3.24 + 0.22 ms, and system RAM traffic from 154 MB per frame to none.

`--interop`: on Windows 10 the AMD D3D12 device has no VRAM the CPU can map (GPU upload heaps need
Windows 11 24H2; a custom L1 + write-combine heap is `E_INVALIDARG`). A D3D12 heap imported into Vulkan
is offered only non-mappable types; a CPU-mappable Vulkan block cannot be exported
(`VK_ERROR_OUT_OF_DEVICE_MEMORY`, every type, plain or dedicated). What works (**route 3**, what the mod
does): a plain Vulkan block the NVIDIA GPU writes, which AMD's Vulkan copies into a D3D12 heap Vulkan
imported, synchronised through a D3D12 fence imported as a timeline semaphore. 4 rounds of new
patterns read correctly on the AMD D3D12 copy and direct queues (no stale caches).

`--evict`: with 19.5 GiB allocated on the 16 GB card (and the block untouched), the block kept its
address at default and at maximum priority.

`--contention` (2026-11-08): the 3090's own system-RAM reads while its copy queue writes

| copy queue | bulk reads | 256 B reads | its writes |
|---|---|---|---|
| idle | 12.2 GB/s | 0.03 us | |
| writes system RAM (cross-adapter heap) | 3.7 GB/s | 0.31 us | 13.1 GB/s |
| writes the 9060's VRAM | 5.7 GB/s | 0.10 us | 10.2 GB/s |

Writes to system RAM compete with the reads at the memory side; card-to-card writes leave them more
room. So the direct copy does not hold up the game's own PCIe reads more than the old path.

Exit (2026-11-08, after the mod's first build hung the PC on Alt+F4): `--exit-hook` and
`--exit-hook-terminate` both reach the hook and keep their exit codes; `--exit-gpu` and
`--exit-gpu-terminate` waited 263 ms for the copies in flight, closed the NVIDIA view, freed the AMD
block and exited cleanly, no WHEA/GPU events.

`--pipeline --compute`: 300 frames of 33.2 MB, the AMD side (D3D12 wait, Vulkan copy) queued 2 ms
before the NVIDIA copy was released: 0 frames wrong, `vkQueueSubmit` returned at once (avg 18 us, it
does not block until its wait is signalled), and the check value behind the data was visible to the
CPU the moment the NVIDIA fence completed in every frame.
