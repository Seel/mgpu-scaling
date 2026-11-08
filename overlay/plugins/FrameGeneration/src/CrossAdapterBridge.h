#pragma once

// CrossAdapterBridge - frame generation on a second GPU.
//
// The game renders with D3D11 on the render GPU (e.g. RTX 3090). Stock
// AAAFrameGeneration opens the game's shared D3D11 textures on a D3D12 device
// on that SAME adapter and runs FSR3 FG + presents there. In secondary-GPU mode
// there are two D3D12 devices:
//
//   device A  "interop"  render GPU   opens the D3D11 NT-handle textures (as before)
//   device B  "fg"       display GPU  FFX FG context, FFX swap chain, present
//
// and this class moves every input from A to B once per game frame:
//
//   D3D11 Signal(producer) -> A copy queue: Wait(producer), Wait(consumed[slot]),
//   then per lane: CopyTextureRegion texture -> row-major buffer placed in a
//   D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER heap, Signal(transfer = frame*kMaxLanes+lane);
//   finally Signal(copyDone).
//
//   Async unpack (bAsyncUnpack=1, default): B copy queue, per lane:
//   Wait(transfer of that lane), buffer -> texture (mirror or back buffer). A lane is
//   unpacked as soon as it has crossed, so the two PCIe trips overlap instead of adding
//   up, and B's direct queue never carries the 100+ MB read. Then Signal(consumed) +
//   Signal(unpack); B direct queue Wait(unpack) before the FFX work of the frame.
//   With bEarlyUnpack the reads of the early groups (depth/MVs, HUD-less) are submitted at
//   their hooks, right after their copies, so B reads them before Present is even called.
//
//   Serialised unpack (bAsyncUnpack=0, the original path): B direct queue
//   Wait(transfer of the last lane), unpack on the frame's direct list, FFX,
//   Signal(consumed).
//
//   D3D11 context: Wait(copyDone of the previous transfer) before the game
//   writes the slot again, so the render GPU never waits on PCIe for the
//   frame it is working on.
//
// Direct GPU copy (bDirectGPUCopy, async unpack only; PeerCopy.h): instead of the cross-adapter
// heap in system memory, A's copy queue writes each lane straight into a block of B's VRAM (B's
// Vulkan device maps it through the resizable BAR, A opens the mapping with
// OpenExistingHeapFromAddress), plus a check value behind the data. On B's copy queue, per lane:
// Wait(transfer), Signal(landed) -> Vulkan copies the block into a D3D12 landing heap and signals
// copied -> Wait(copied), then the usual unpack, from the landing heap. One PCIe trip, no system RAM.
// The cross-adapter heaps stay allocated. Guard: at every frame start the CPU reads the check values
// of the copies that finished, through the block's mapping (which follows the block wherever the
// driver keeps it). A value missing where A wrote it means the block moved: frames go back through
// system memory and frame generation is off for the rest of the session (DirectCopyTripped).
//
// Everything runs on the game's render thread (Present and the render hooks).
// No method throws; failures are logged and reported through the return value.

#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <winrt/base.h>

#include <array>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "FrameSlots.h"
#include "PeerCopy.h"

class CrossAdapterBridge
{
public:
	// When a lane's source texture becomes final within the frame. The bridge starts the
	// cross-PCIe copy at the hook that marks it instead of waiting for Present:
	//   kWorld    depth and motion vectors, final after the world pass (PostAlpha)
	//   kHudless  the UI-free frame, final after the HUD-less capture (PostDisplay)
	//   kPresent  the finished frame, final only at Present
	enum class LaneTiming : std::uint8_t { kWorld = 0, kHudless = 1, kPresent = 2 };

	static constexpr UINT kSlots = kMaxFrameSlots;  // per-frame inputs, indexed by frameIndex; SlotCount() in use
	static constexpr UINT kMaxLanes = 8;  // per-lane transfer fence values are allocated in submission order

	// Timestamp marks on B's direct list (MarkDisplayList).
	static constexpr UINT kMarkListBegin = 0;
	static constexpr UINT kMarkAfterUnpack = 1;  // after the serialised unpack; unused in async mode
	static constexpr UINT kMarkListEnd = 2;      // also resolves the slot's queries
	static constexpr UINT kMarkUpsampleBegin = 3;  // depth rebuild (iHalfResDepthMV = 1); runs before the
	static constexpr UINT kMarkUpsampleEnd = 4;    // frame's list on the same queue, so its Resolve covers it

	// Copy-timer marks on the render GPU's copy queue: two per phase, in frame order.
	static constexpr UINT kMarkWorld = 0;
	static constexpr UINT kMarkHudless = 2;
	static constexpr UINT kMarkPresent = 4;

	static CrossAdapterBridge* GetSingleton()
	{
		static CrossAdapterBridge singleton;
		return &singleton;
	}

	// Picks the adapter that frame generation should run and present on.
	// Never the game's adapter, never a software adapter. With a name filter, the
	// single hardware adapter whose description contains it (case-insensitive).
	// Without one, the adapter that owns the monitor a_window is on; failing that,
	// the only remaining hardware adapter. Ambiguity returns null rather than guessing.
	static winrt::com_ptr<IDXGIAdapter1> SelectSecondaryAdapter(IDXGIAdapter* a_gameAdapter,
		HWND a_window, const std::string& a_nameFilter);

	static std::string AdapterName(IDXGIAdapter* a_adapter);

	// The game is usually placed on the GPU that drives the display, because old games
	// enumerate adapters and take the first one. For secondary-GPU mode the game must render
	// on the headless GPU instead. Returns that GPU when a_gameAdapter is the display GPU and
	// there is exactly one hardware adapter with no display attached; otherwise null.
	static winrt::com_ptr<IDXGIAdapter1> PickRenderAdapter(IDXGIAdapter* a_gameAdapter);

	// a_deviceA: on the game's adapter. a_deviceB: on the secondary adapter.
	// a_device11/a_context11: the game's D3D11 device and immediate context (adapter A).
	// a_asyncUnpack: unpack on a copy queue of B (true) or on the caller's direct list (false).
	// a_slots: frames in flight (the FFX swap chain's buffer count), 2 or 3.
	// a_directCopy: bDirectGPUCopy (needs a_asyncUnpack); falls back to system memory, logged, if it
	// cannot be set up.
	bool Initialize(ID3D12Device* a_deviceA, ID3D12Device* a_deviceB,
		ID3D11Device5* a_device11, ID3D11DeviceContext4* a_context11, bool a_asyncUnpack, UINT a_slots,
		bool a_directCopy);
	[[nodiscard]] UINT SlotCount() const noexcept { return slotCount; }

	// bDirectGPUCopy carries the frames (set up for every lane, guard not tripped).
	[[nodiscard]] bool IsDirectCopy() const noexcept { return directCopy; }
	// The guard found a VRAM block of the display GPU moved (or a Vulkan submission failed): frames cross
	// through system memory again and frame generation stays off for the rest of the session.
	[[nodiscard]] bool DirectCopyTripped() const noexcept { return directCopyTripped; }

	// Once B's direct queue (the FFX game queue) exists. Enables its timestamps and the
	// fence that tells the unpack when the previous frame's direct-queue work is done.
	void SetDisplayQueue(ID3D12CommandQueue* a_queueB);

	[[nodiscard]] bool IsActive() const noexcept { return active; }
	[[nodiscard]] bool IsAsyncUnpack() const noexcept { return asyncUnpack; }

	// Registers one per-frame input. a_sources[slot] (SlotCount() of them) are single-subresource
	// textures on device A. With a_createMirror, same-sized textures are created on
	// device B and returned by Mirror(); without it, the caller names a destination
	// (the back buffer) per frame. Returns the lane index, or -1 (logged; the bridge
	// is deactivated).
	int AddLane(const char* a_name, const std::array<ID3D12Resource*, kSlots>& a_sources, bool a_createMirror,
		LaneTiming a_timing = LaneTiming::kPresent);

	[[nodiscard]] ID3D12Resource* Mirror(int a_lane, UINT a_slot) const noexcept;

	// Once per game frame, after the D3D11 context signalled a_producerFenceA
	// (the fence object opened on device A) to a_producerValue. Finishes the frame's
	// transfer: copies every lane the early hooks did not, signals copy-done (the D3D11
	// gate and the next reuse of the slot wait for it) and updates the statistics.
	HRESULT Transfer(ID3D12Fence* a_producerFenceA, UINT64 a_producerValue, UINT a_slot);

	// Called from the frame hook at which this lane group's sources become final (depth and
	// motion vectors after the world pass, HUD-less after its capture). The first call of a
	// frame also starts it (slot waits, allocator reset). No-op with no lanes of that timing.
	HRESULT TransferGroup(ID3D12Fence* a_producerFenceA, UINT64 a_producerValue, UINT a_slot,
		LaneTiming a_timing);

	[[nodiscard]] bool HasLanes(LaneTiming a_timing) const noexcept;

	// --- Async path (IsAsyncUnpack) ---------------------------------------------------
	// After Transfer: B's copy queue unpacks every lane of a_slot as soon as it lands
	// (mirrors, and a_backBuffer for a_backBufferLane), releases the staging slot to A,
	// and a_queueB waits for the unpack. Call before executing the frame's direct list.
	HRESULT Unpack(UINT a_slot, int a_backBufferLane, ID3D12Resource* a_backBuffer, ID3D12CommandQueue* a_queueB);

	// Early unpack (bEarlyUnpack): right after TransferGroup at a frame hook, B's copy queue is
	// given the read of that group's lanes too, so it starts as soon as they land instead of
	// at Present. Unpack then reads only what is left (the finished frame). No-op for kPresent,
	// on the serialised path, or when no lane of that timing was sent this frame.
	HRESULT UnpackGroup(UINT a_slot, LaneTiming a_timing);

	// --- Serialised path (!IsAsyncUnpack) ---------------------------------------------
	// a_queueB waits for the whole transfer of this frame.
	HRESULT WaitTransfer(ID3D12CommandQueue* a_queueB);

	// On B's command list for this frame: staging -> mirror for every mirrored lane.
	void RecordUnpack(ID3D12GraphicsCommandList* a_listB, UINT a_slot);

	// On B's command list: staging -> a_destB for an unmirrored lane. a_destB is in
	// a_destState before and after.
	void RecordUnpackTo(ID3D12GraphicsCommandList* a_listB, int a_lane, UINT a_slot,
		ID3D12Resource* a_destB, D3D12_RESOURCE_STATES a_destState);

	// After B executed the list that unpacked this frame.
	HRESULT SignalConsumed(ID3D12CommandQueue* a_queueB);

	// --- Half-resolution depth (iHalfResDepthMV = 1) ------------------------------------
	// Depth crosses PCIe at half resolution and B rebuilds it at a_width x a_height (R32_FLOAT)
	// with the compute shader in a_shaderPath, so FSR3 keeps working at full resolution.
	// Call after SetDisplayQueue and before the depth lane is added. False (logged) leaves
	// nothing behind; the caller then sends depth at full resolution.
	bool CreateDepthUpsampler(UINT a_width, UINT a_height, const wchar_t* a_shaderPath);

	// The half-resolution depth lane; its mirrors become the rebuild's source.
	void SetDepthUpsampleLane(int a_lane);

	// The rebuilt full-resolution depth FSR3 reads for a_slot; null when not rebuilding.
	[[nodiscard]] ID3D12Resource* UpsampledDepth(UINT a_slot) const noexcept;

	// Serialised path: on the frame's direct list, after RecordUnpack. (Async path: Unpack
	// submits it on its own as soon as the depth lane has landed, while the rest still crosses.)
	void RecordDepthUpsample(ID3D12GraphicsCommandList* a_listB, UINT a_slot);

	// --- FSR 4.1 upscaling on B (async path) ------------------------------------------
	// a_queue waits until B's copy queue has read these lanes of the frame being assembled.
	// a_queued is false (and nothing is queued) while any of them has not been given to the copy
	// queue yet this frame; the caller then tries again after Unpack at Present.
	HRESULT WaitForLaneReads(ID3D12CommandQueue* a_queue, const int* a_lanes, size_t a_count, bool& a_queued);

	// CPU: the direct list that last used a_slot has finished (allocator reuse). Normally already true.
	HRESULT WaitDisplayListDone(UINT a_slot);
	// Non-blocking version, for reading timestamps.
	[[nodiscard]] bool DisplayListDone(UINT a_slot) const noexcept;

	// --- Both paths -------------------------------------------------------------------
	// Timestamp on the frame's direct list on B (kMarkListBegin / kMarkAfterUnpack / kMarkListEnd).
	void MarkDisplayList(ID3D12GraphicsCommandList* a_listB, UINT a_slot, UINT a_mark);

	// After the frame's direct list was executed on a_queueB.
	HRESULT SignalDisplayListDone(ID3D12CommandQueue* a_queueB);

	// CPU time the render thread spent blocked in the FFX swap chain's Present and in
	// the frame-latency wait after it. Both are back-pressure from the display GPU.
	void AddPresentBlock(double a_presentMs, double a_latencyWaitMs);

	// After Present, before the game's D3D11 work for the next frame is recorded:
	// the D3D11 context waits (on the GPU) for the transfer that last read
	// a_nextSlot, which the next frame is about to overwrite. With strict
	// alternation that is the previous frame's transfer, so the render GPU does
	// not wait for the copy of the frame it just finished.
	HRESULT GateD3D11(UINT a_nextSlot);

	// Every ~10 s: payload size, copy time, queue lag, per-frame timeline.
	void LogStats();

	void Shutdown() noexcept;

	// The render thread holds this while it uses the bridge for a frame (DX12SwapChain::Present and
	// TransferEarlyLanes), so ReleaseDirectCopyForExit, which runs on whichever thread ends the process,
	// never lands in the middle of one. Uncontended in normal play.
	[[nodiscard]] std::unique_lock<std::recursive_timed_mutex> LockFrame()
	{
		return std::unique_lock<std::recursive_timed_mutex>(frameMutex);
	}

	// bDirectGPUCopy, at process exit (hooked ExitProcess / TerminateProcess, before any driver unloads):
	// waits up to 2 s each for the copies in flight, closes the render GPU's views of the display GPU's
	// VRAM blocks, then frees the blocks. The first build had no such step: the destructors freed the
	// blocks first, while the drivers were already unloading, and the PC hung on Alt+F4 (2026-11-08).
	// No-op without direct copy.
	void ReleaseDirectCopyForExit(const char* a_why) noexcept;

private:
	struct Lane
	{
		std::string name;
		LaneTiming timing = LaneTiming::kPresent;
		D3D12_RESOURCE_DESC desc{};
		D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};  // Offset relative to the slot base
		UINT64 slotStride = 0;
		UINT64 bytes = 0;
		std::array<winrt::com_ptr<ID3D12Resource>, kSlots> sourceA;
		winrt::com_ptr<ID3D12Heap> heapA;
		winrt::com_ptr<ID3D12Heap> heapB;
		winrt::com_ptr<ID3D12Resource> stagingA;
		winrt::com_ptr<ID3D12Resource> stagingB;
		std::array<winrt::com_ptr<ID3D12Resource>, kSlots> mirrorB;
		bool mismatchLogged = false;

		// bDirectGPUCopy: B's VRAM block as A sees it, B's landing heap, and the block's CPU mapping.
		// Null/-1 when the lane crosses through system memory only.
		winrt::com_ptr<ID3D12Heap> peerHeapA;
		winrt::com_ptr<ID3D12Resource> peerStagingA;
		winrt::com_ptr<ID3D12Heap> landingHeapB;
		winrt::com_ptr<ID3D12Resource> landingB;
		const volatile std::uint8_t* peerMapped = nullptr;
		int peerLane = -1;
		UINT64 checkOffset = 0;                    // the check value, behind the data in each slot
		std::array<UINT64, kSlots> checkPending{};  // value A wrote into a slot and nobody checked yet
		std::array<bool, kSlots> checkLate{};       // ...and it was missing at the first look
	};

	// GPU timestamps for one queue, kSlots x marks, converted to QPC ticks through
	// GetClockCalibration so the two GPUs and the CPU share one timeline.
	struct GpuTimer
	{
		static constexpr UINT kMaxMarks = 6;

		winrt::com_ptr<ID3D12QueryHeap> heap;
		winrt::com_ptr<ID3D12Resource> readback;
		UINT marks = 0;
		UINT64 frequency = 0;
		bool calibrated = false;
		UINT64 calibrationGpu = 0;
		UINT64 calibrationQpc = 0;
		std::array<UINT, kSlots> written{};  // bit per mark recorded since the slot was last read

		bool Create(ID3D12Device* a_device, ID3D12CommandQueue* a_queue, bool a_copyQueue, UINT a_marks);
		void Calibrate(ID3D12CommandQueue* a_queue);
		void Mark(ID3D12GraphicsCommandList* a_list, UINT a_slot, UINT a_mark);
		void Resolve(ID3D12GraphicsCommandList* a_list, UINT a_slot);
		// QPC ticks of every mark of a_slot; a_valid has a bit per mark that was written.
		// Clears the slot. False when nothing was recorded or the readback failed.
		bool Read(UINT a_slot, std::array<double, kMaxMarks>& a_qpc, UINT& a_valid);
		[[nodiscard]] bool Valid() const noexcept { return heap && readback && frequency; }
		void Reset() noexcept;
	};

	// iHalfResDepthMV = 1: B's compute pass from the half-resolution depth mirror to a
	// full-resolution depth texture per slot.
	struct DepthUpsampler
	{
		winrt::com_ptr<ID3D12RootSignature> rootSignature;
		winrt::com_ptr<ID3D12PipelineState> pipeline;
		winrt::com_ptr<ID3D12DescriptorHeap> descriptors;  // per slot: SRV (half-res mirror), UAV (target)
		UINT descriptorSize = 0;
		std::array<winrt::com_ptr<ID3D12Resource>, kSlots> target;
		// Async path: its own direct list, submitted ahead of the frame's list.
		std::array<winrt::com_ptr<ID3D12CommandAllocator>, kSlots> allocators;
		winrt::com_ptr<ID3D12GraphicsCommandList> list;
		winrt::com_ptr<ID3D12Fence> sourceReadyFence;  // B copy queue: the depth lane of frame N has landed
		int lane = -1;
		UINT width = 0;
		UINT height = 0;

		[[nodiscard]] bool Ready() const noexcept { return pipeline && lane >= 0; }
	};

	struct Average
	{
		double sum = 0.0;
		double max = 0.0;
		UINT64 count = 0;
		void Add(double a_value)
		{
			max = (count == 0 || a_value > max) ? a_value : max;
			sum += a_value;
			++count;
		}
		[[nodiscard]] double Mean() const { return count ? sum / static_cast<double>(count) : 0.0; }
	};

	HRESULT CreateSharedFencePair(UINT64 a_initial, winrt::com_ptr<ID3D12Fence>& a_onA,
		winrt::com_ptr<ID3D12Fence>& a_onB);
	HRESULT CreateSharedHeapPair(UINT64 a_size, winrt::com_ptr<ID3D12Heap>& a_onA,
		winrt::com_ptr<ID3D12Heap>& a_onB);
	HRESULT WaitCpu(ID3D12Fence* a_fence, UINT64 a_value);
	void Fail(const char* a_where, HRESULT a_hr);

	// Records a_source(slot) -> a_dest with a_destState <-> COPY_DEST barriers. a_source is the lane's
	// staging buffer on B (cross-adapter heap) or its landing heap (direct copy).
	// False (logged once per lane) when a_dest does not match the lane's size/pitch.
	bool RecordLaneCopy(ID3D12GraphicsCommandList* a_list, Lane& a_lane, UINT a_slot, ID3D12Resource* a_dest,
		D3D12_RESOURCE_STATES a_destState, ID3D12Resource* a_source);

	// --- bDirectGPUCopy ---------------------------------------------------------------------
	// The lane's VRAM block and landing heap (PeerCopy) and A's view of the block. False (logged) when
	// any of it fails; the caller then turns the direct copy off.
	bool AddDirectLane(Lane& a_lane, UINT64 a_heapSize);
	// Set-up failure: system memory for the rest of the session, frame generation unaffected.
	void DisableDirectCopy();
	// The guard: system memory from the next frame on, frame generation off for the session.
	void TripDirectCopy(const std::string& a_why);
	// At a frame start: reads the check values of the copies that finished. a_reusedSlot's copies are
	// known to be done (PrepareFrame waited), so a value missing there trips the guard; elsewhere it
	// gets a second look one frame later.
	void CheckDirectCopy(UINT a_reusedSlot);
	// B's copy queue, after Wait(transfer of the lane): Signal(landed), the Vulkan copy, Wait(copied).
	HRESULT QueueDirectCopy(const Lane& a_lane, UINT a_slot);

	// Reads the timestamps the previous use of a_slot left behind (never blocks).
	void CollectTimings(UINT a_slot);
	void CalibrateTimers();

	// First transfer of a frame: waits for the slot's previous copy, resets the slot's
	// allocator and queues the consumed wait. Later phases of the same frame reuse it.
	HRESULT PrepareFrame(UINT a_slot);

	// One lane: texture -> staging on the copy queue, then its own transfer-fence value.
	// a_markBase is kMarkWorld/kMarkHudless/kMarkPresent; the first and last lane of a phase
	// bracket its timestamps, so the rendering between the phases is not counted as copy time.
	HRESULT SubmitLane(Lane& a_lane, size_t a_laneIndex, UINT a_slot, UINT a_markBase, bool a_firstInPhase,
		bool a_lastInPhase);

	// First read of a frame on B's copy queue (at a hook or at Present): waits for the slot's
	// previous unpack, resets its allocator, queues the wait for the slot's previous direct list.
	// Closes a read that an earlier frame started but never finished.
	HRESULT BeginUnpack(UINT a_slot);
	// One read list per lane on B's copy queue, each waiting for its own transfer value.
	// a_markBase is kMarkWorld/kMarkHudless/kMarkPresent; a_resolve on the frame's last call.
	HRESULT SubmitUnpackLanes(UINT a_slot, const size_t* a_order, size_t a_count, UINT a_markBase, bool a_resolve,
		int a_backBufferLane, ID3D12Resource* a_backBuffer);

	// Depth rebuild: barriers + dispatch on a_list (timestamps included).
	void RecordUpsampleCommands(ID3D12GraphicsCommandList* a_list, UINT a_slot);
	// Async path: B's direct queue waits for the depth lane only, then runs the rebuild.
	HRESULT SubmitDepthUpsample(UINT a_slot, UINT64 a_frame, ID3D12CommandQueue* a_queueB);
	void ResetUpsampler() noexcept;

	bool active = false;
	bool asyncUnpack = true;

	winrt::com_ptr<ID3D12Device> deviceA;
	winrt::com_ptr<ID3D12Device> deviceB;
	winrt::com_ptr<ID3D11DeviceContext4> context11;

	winrt::com_ptr<ID3D12CommandQueue> copyQueueA;
	std::array<winrt::com_ptr<ID3D12CommandAllocator>, kSlots> allocatorsA;
	winrt::com_ptr<ID3D12GraphicsCommandList> listA;

	// Async unpack on the display GPU.
	winrt::com_ptr<ID3D12CommandQueue> copyQueueB;
	std::array<winrt::com_ptr<ID3D12CommandAllocator>, kSlots> allocatorsB;
	winrt::com_ptr<ID3D12GraphicsCommandList> listB;
	winrt::com_ptr<ID3D12Fence> unpackFenceB;  // B copy queue signals, B direct queue waits
	std::array<UINT64, kSlots> slotUnpack{};   // frame whose unpack last used each slot's allocator
	bool unpackStarted = false;                // a frame's read has been submitted in part (BeginUnpack)
	UINT64 unpackFrame = 0;                    // ...for this frame
	std::array<UINT64, kMaxLanes> laneUnpackFrame{};  // frame whose read of each lane was last submitted
	winrt::com_ptr<ID3D12Fence> readFenceB;           // B copy queue signals after each lane's read
	UINT64 readValue = 0;
	std::array<UINT64, kMaxLanes> laneReadValue{};    // readFenceB value of each lane's latest read

	// B's direct queue (FFX game queue). Not owned.
	ID3D12CommandQueue* displayQueue = nullptr;
	winrt::com_ptr<ID3D12Fence> displayDoneFenceB;  // B direct queue signals after each frame's list
	UINT64 displayDoneValue = 0;
	std::array<UINT64, kSlots> slotDisplayDone{};

	// A signals, B waits. Cross-adapter. Per-lane values (see LaneValue).
	winrt::com_ptr<ID3D12Fence> transferFenceA;
	winrt::com_ptr<ID3D12Fence> transferFenceB;
	// B signals, A waits. Cross-adapter.
	winrt::com_ptr<ID3D12Fence> consumedFenceA;
	winrt::com_ptr<ID3D12Fence> consumedFenceB;
	// A signals, D3D11 waits. Same adapter.
	winrt::com_ptr<ID3D12Fence> copyDoneFenceA;
	winrt::com_ptr<ID3D11Fence> copyDoneFence11;

	HANDLE fenceEvent = nullptr;

	UINT slotCount = 2;                                  // frames in flight (Initialize)
	UINT currentSlot = 0;                                // slot of the latest Transfer
	UINT64 transferValue = 0;                            // frame number of the latest Transfer
	std::array<UINT64, kSlots> slotTransfer{};           // frame that last used each slot
	std::array<LARGE_INTEGER, kSlots> slotPresentQpc{};  // CPU time Transfer was called for that frame

	// Early transfer: the frame is started by the first hook that has data ready.
	bool frameStarted = false;                          // a frame hook ran before Present
	UINT frameSlot = 0;                                 // slot of the frame being assembled
	UINT64 nextLaneValue = 0;                           // next per-lane transfer fence value
	std::array<UINT64, kMaxLanes> laneValue{};          // value signalled for each lane this frame
	std::array<UINT64, kMaxLanes> laneFrame{};          // frame that last copied each lane

	// bDirectGPUCopy: the Vulkan side owns the display GPU's VRAM blocks. Declared before `lanes`, so a
	// destructor run releases the render GPU's heaps on those blocks (in `lanes`) first, and only then
	// the blocks: never the other way round (see ReleaseDirectCopyForExit).
	PeerCopy peer;

	std::vector<Lane> lanes;
	UINT64 bytesPerFrame = 0;

	// bDirectGPUCopy
	static constexpr UINT64 kCheckStride = 256;  // per lane and slot in the upload buffer; also the space behind the data
	std::recursive_timed_mutex frameMutex;  // LockFrame / ReleaseDirectCopyForExit
	bool directCopy = false;         // lanes cross through B's VRAM blocks (all set up, guard not tripped)
	bool directCopyTripped = false;  // the guard tripped this session
	bool frameDirect = false;        // the path of the frame being assembled, fixed in PrepareFrame
	UINT64 peerValue = 0;            // PeerCopy::Fence() values: landed/copied, two per lane per frame
	winrt::com_ptr<ID3D12Resource> checkUploadA;  // A: the check values A copies behind each lane's data
	UINT64* checkCpu = nullptr;                   // its persistent mapping

	DepthUpsampler upsampler;

	GpuTimer timerCopyA;    // render GPU copy queue: first lane start, last lane end
	GpuTimer timerCopyB;    // display GPU copy queue (async unpack): two marks per phase, like timerCopyA
	GpuTimer timerDisplay;  // display GPU direct queue: list begin, after unpack, list end

	// Statistics since the last LogStats.
	UINT64 statFrames = 0;
	Average statCopyA;        // ms, render GPU copy (all lanes)
	Average statUnpack;       // ms, display GPU unpack (copy queue, or the serialised part of the direct list)
	Average statDisplayList;  // ms, display GPU direct list
	// Timeline in ms after the CPU entered Present for that frame (both GPUs calibrated to QPC).
	Average statCopyAStart, statCopyAEnd, statUnpackStart, statUnpackEnd, statListStart, statListEnd;
	Average statUpsample;      // ms, depth rebuild on the display GPU (iHalfResDepthMV = 1)
	Average statUpsampleEnd;   // ms after Present at which it finished
	Average statPresentBlock;  // ms the render thread sat in the FFX swap chain's Present
	Average statLatencyWait;   // ms in the frame-latency wait after it
	Average statPeerCopy;      // ms, bDirectGPUCopy: B's Vulkan copies of a frame into the landing heaps
	UINT64 statDirectFrames = 0;  // frames that crossed directly
	UINT64 statDirectChecks = 0;  // check values found where A wrote them
	UINT64 statDirectLate = 0;    // ...missing at the first look (found one frame later)
	double statCpuSubmitMs = 0.0;
	UINT64 statMaxLag = 0;
	UINT64 statCpuWaits = 0;
	UINT64 statGatePending = 0;  // GateD3D11 found the copy it queues a wait for still running
	UINT64 statGateCalls = 0;
	LARGE_INTEGER statLastLog{};
	LARGE_INTEGER qpcFrequency{};
};
