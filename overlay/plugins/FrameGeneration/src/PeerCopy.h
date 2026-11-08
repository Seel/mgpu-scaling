#pragma once

// PeerCopy - the display GPU's half of the direct GPU-to-GPU copy (bDirectGPUCopy).
//
// The render GPU's copy engine can write straight into the display GPU's VRAM: the display GPU maps a
// block of its VRAM for the CPU through its resizable BAR, and the render GPU's D3D12 device opens that
// CPU address with OpenExistingHeapFromAddress as if it were system memory. Its writes then cross PCIe
// once, card to card, and never touch system RAM (the cross-adapter heap costs two trips through it).
//
// On Windows 10, D3D12 cannot create VRAM the CPU can map on a discrete GPU (GPU upload heaps need
// Windows 11 24H2), and AMD's Vulkan driver does not export CPU-mappable VRAM to D3D12. So this class
// keeps a small Vulkan device on the display GPU that
//   - allocates one CPU-mappable VRAM block per lane and maps it (the render GPU writes there),
//   - imports one D3D12 heap per lane, created on the display GPU's D3D12 device: the landing heap the
//     bridge's unpack reads instead of the cross-adapter heap,
//   - copies a lane's slot from the block into its landing heap when asked (~150 GB/s, local VRAM).
// Per frame it synchronises with D3D12 through one D3D12 fence on the display GPU, imported into Vulkan
// as a timeline semaphore: the bridge's copy queue signals "landed", Vulkan waits for that on the GPU,
// copies and signals "copied", and the copy queue waits for "copied" before it unpacks.
//
// tools/p2p-probe checked each step on the RTX 3090 + RX 9060 XT this was built for (--interop, --evict,
// --pipeline --compute: 300 frames with the Vulkan copy queued before the render GPU wrote, no wrong
// data, vkQueueSubmit returning at once).
//
// Everything runs on the game's render thread. No method throws.

#include <d3d12.h>
#include <winrt/base.h>

#include <cstdint>
#include <memory>

class PeerCopy
{
public:
	PeerCopy();
	~PeerCopy();
	PeerCopy(const PeerCopy&) = delete;
	PeerCopy& operator=(const PeerCopy&) = delete;

	// Loads vulkan-1.dll and creates the Vulkan device on a_deviceB's adapter (matched by LUID) and the
	// shared fence. False (logged) when anything is missing; nothing is left behind.
	bool Initialize(ID3D12Device* a_deviceB, UINT a_slots);
	[[nodiscard]] bool Ready() const noexcept { return state != nullptr; }

	// One lane: a CPU-mapped VRAM block of a_size bytes (a_mapped, the address the render GPU opens) and a
	// D3D12 heap of the same size on the display GPU (a_landing), plus one recorded copy per slot of
	// a_bytes at a_slot * a_slotStride. Returns the lane's index here, or -1 (logged).
	int AddLane(const char* a_name, UINT64 a_size, UINT64 a_slotStride, UINT64 a_bytes, void*& a_mapped,
		winrt::com_ptr<ID3D12Heap>& a_landing);

	// Queues on the GPU: wait until Fence() reaches a_landed, copy the lane's slot into its landing heap,
	// signal a_copied. Returns at once; false (logged once) when the submission failed.
	bool SubmitCopy(int a_lane, UINT a_slot, UINT64 a_landed, UINT64 a_copied);

	// The display GPU's D3D12 fence that Vulkan waits on and signals.
	[[nodiscard]] ID3D12Fence* Fence() const noexcept;

	// GPU time of the last copies of a_slot, summed over the lanes. False while one is still running or
	// when there are no timestamps.
	bool CopyTimeMs(UINT a_slot, double& a_ms);

	[[nodiscard]] const char* QueueName() const noexcept;

	// Waits for the Vulkan queue (unblocking it if the D3D12 side never signalled) and frees everything.
	// Call only after the render GPU released the heaps it opened on the blocks.
	void Shutdown() noexcept;

private:
	struct State;
	std::unique_ptr<State> state;
};
