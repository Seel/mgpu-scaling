#pragma once

// SecondaryUpscale - FG-GPU1: FSR 4.1 upscaling on the display GPU.
//
// With the upscaler's iUpscaleMethodPreference = 3, the render GPU draws the 3D scene at the
// quality mode's resolution with a sub-pixel jitter and no TAA (the upscaler, namespace up), and
// skips the upscale. This part then
//
//   render GPU   PostDisplay: copies the render-resolution scene (UI-free) into its lane and fills
//                the full-size frame buffer from it with a plain bilinear blit, so the UI is drawn
//                at full resolution over a background (SecondaryUpscaleCS.hlsl, Blit).
//   display GPU  as soon as the scene, depth and motion-vector lanes are read (before Present with
//                bEarlyUnpack): FSR 4.1 render -> display resolution on the direct queue.
//                At Present: the finished frame (blit + UI) is unpacked into a mirror; the composite
//                puts its UI over the FSR 4.1 result into the back buffer (Composite), and frame
//                generation reads the FSR 4.1 result as the UI-free frame.
//
// Frames Upscaling.dll renders natively (menus, before this is ready) are copied to the back
// buffer as they are, without frame generation: the depth and motion-vector lanes are sized for
// the render resolution. Everything runs on the game's render thread.
//
// Display format: the game renders RGBA8, but the swap chain can be another format, because an
// add-on such as ReShade's AutoHDR switches it to R10G10B10A2 (HDR10) or R16G16B16A16_FLOAT
// (scRGB) while FFX creates it. CopyResource cannot convert, so then the composite is created in
// the swap chain's format and native frames go through it too (Copy). Without FSR 4.1 upscaling
// the same pass is set up on its own ("conversion only"): the finished frame gets a mirror and is
// copied into the back buffer through the composite. With an RGBA8 swap chain nothing changes.
//
// Single-GPU mode (bFrameGenOnSecondaryGPU = 0) uses only that conversion pass, on the game's GPU:
// InitializeLocalConversion at swap-chain creation, SetLocalFrames with the D3D12 side of the game's
// finished frames, then RecordLocalConversion instead of the plain copy into the back buffer.

#include <d3d12.h>
#include <dxgi.h>
#include <winrt/base.h>

#include <array>
#include <cstdint>

#include "FrameSlots.h"

class SecondaryUpscale
{
public:
	static constexpr UINT kSlots = kMaxFrameSlots;  // arrays; the session uses Slots() of them

	static SecondaryUpscale* GetSingleton()
	{
		static SecondaryUpscale singleton;
		return &singleton;
	}

	// At swap-chain creation, after the FSR frame-generation contexts. When the upscaler's settings
	// ask for it and everything it needs is there, creates the FSR 4.1 context, its outputs and the
	// composite. False (logged) leaves the session exactly as before.
	// a_displayFormat is the swap chain's real format (read back after FFX created it), not the
	// format the game asked for. When it is not RGBA8 and upscaling is not set up, the conversion
	// pass is set up on its own (Converting()).
	bool Initialize(ID3D12Device* a_device, ID3D12CommandQueue* a_queue, UINT a_displayWidth, UINT a_displayHeight,
		DXGI_FORMAT a_displayFormat);

	// Single-GPU mode, at swap-chain creation, when the back buffers (a_displayFormat) are not the
	// format the game renders (a_sourceFormat): sets up the conversion pass on the game's GPU for
	// a_slots frames in flight. False (logged): nothing is set up, the caller keeps its plain copy.
	bool InitializeLocalConversion(ID3D12Device* a_device, UINT a_width, UINT a_height, DXGI_FORMAT a_displayFormat,
		DXGI_FORMAT a_sourceFormat, UINT a_slots);
	// The D3D12 side of the game's finished frame per slot (CreateInterop). False (logged) when the
	// conversion cannot read them.
	bool SetLocalFrames(const std::array<ID3D12Resource*, kSlots>& a_frames);
	// Single-GPU conversion set up and bound: RecordLocalConversion replaces the plain copy.
	[[nodiscard]] bool ConvertsLocally() const noexcept { return local && converting && localBound; }
	// On the frame's direct list: the game's finished frame of a_slot converted into a_backBuffer
	// (PRESENT before and after).
	void RecordLocalConversion(ID3D12GraphicsCommandList* a_list, UINT a_slot, ID3D12Resource* a_backBuffer);

	// Set up for it: the lanes are sized for the render resolution and the finished frame is
	// unpacked into a mirror. Fixed for the session.
	[[nodiscard]] bool Planned() const noexcept { return planned; }
	// The swap chain is not RGBA8: every frame reaches the back buffer through the composite, so
	// the finished frame needs a mirror even without upscaling. Fixed for the session.
	[[nodiscard]] bool Converting() const noexcept { return converting; }
	// The finished frame is unpacked into a mirror and RecordPresent puts it into the back buffer.
	[[nodiscard]] bool OwnsPresent() const noexcept { return planned || converting; }
	[[nodiscard]] DXGI_FORMAT DisplayFormat() const noexcept { return displayFormat; }
	// Frames can be upscaled: planned, lanes bound, nothing has failed.
	[[nodiscard]] bool Ready() const noexcept { return planned && bound && !failed; }
	[[nodiscard]] UINT RenderWidth() const noexcept { return renderWidth; }
	[[nodiscard]] UINT RenderHeight() const noexcept { return renderHeight; }

	// The upscaler asks every frame before it renders at a_width x a_height.
	bool ReadyFor(uint32_t a_width, uint32_t a_height);

	// The finished frame's lane (CreateInterop), and the scene/depth/motion-vector lanes
	// (CreateFrameGenerationResources). BindLanes false (logged) means frames cannot be upscaled.
	void SetFinalLane(int a_lane);
	[[nodiscard]] bool HasFinalMirror() const noexcept;
	// Frames in flight this session (the bridge's slots).
	[[nodiscard]] UINT Slots() const noexcept;
	bool BindLanes(int a_sceneLane, int a_depthLane, int a_motionLane);
	void Fail(const char* a_reason);

	// PostDisplay of a frame the render GPU drew for upscaling, after its scene lane was sent.
	// Queues the upscale as soon as the three lanes' reads are queued.
	void BeginFrame(UINT a_slot, float a_jitterX, float a_jitterY, float a_sharpness);
	// Present, after the bridge's Unpack: queues the upscale if BeginFrame could not yet. True when
	// the frame was upscaled.
	bool FinishSubmit(UINT a_slot);
	// On the frame's direct list: the composite (a_upscaled) or a copy of the finished frame into
	// the back buffer (PRESENT before and after); converted to the swap chain's format if needed.
	void RecordPresent(ID3D12GraphicsCommandList* a_list, UINT a_slot, bool a_upscaled, ID3D12Resource* a_backBuffer);
	// The FSR 4.1 result of a_slot: the UI-free frame for frame generation.
	[[nodiscard]] ID3D12Resource* Output(UINT a_slot) const noexcept;
	// After the frame's list was executed.
	void EndFrame(UINT a_slot, bool a_upscaled);

	// Every ~10 s from Present.
	void LogStats();

	static const char* FormatName(DXGI_FORMAT a_format);

private:
	static constexpr UINT kMarkUpscaleBegin = 0;
	static constexpr UINT kMarkUpscaleEnd = 1;
	static constexpr UINT kMarkPresentBegin = 2;
	static constexpr UINT kMarkPresentEnd = 3;
	static constexpr UINT kMarks = 4;
	static constexpr UINT kDescriptorsPerSlot = 4;  // t0 scene, t1 finished frame, t2 FSR output, u0 composite

	struct FrameParams
	{
		float jitterX = 0.0f;
		float jitterY = 0.0f;
		float sharpness = 0.0f;
		float cameraNear = 0.0f;
		float cameraFar = 0.0f;
		float fovY = 1.0f;
	};

	// Reads the upscaler's settings and checks what upscaling here needs; sets the render size.
	bool PlanUpscale();
	// a_upscale: the Composite pass, FSR 4.1 outputs, upscale list and timestamps. Always: the root
	// signature, descriptors and the display-format composite; Copy when the swap chain is not RGBA8.
	bool CreatePipeline(bool a_upscale);
	bool CreateComputePipeline(const char* a_entry, winrt::com_ptr<ID3D12PipelineState>& a_pipeline);
	// a_format can be written from a shader (typed UAV store) on a_device.
	static bool Storable(ID3D12Device* a_device, DXGI_FORMAT a_format);
	bool WriteFinishedDescriptors();
	// Copy's table per slot: t1 = a_finished[slot] (read as a_format), u0 = the composite.
	bool WriteCopyDescriptors(const std::array<ID3D12Resource*, kSlots>& a_finished, DXGI_FORMAT a_format);
	void RecordCopyThroughComposite(ID3D12GraphicsCommandList* a_list, UINT a_slot, ID3D12Resource* a_finished,
		ID3D12Resource* a_backBuffer);
	bool TrySubmit(UINT a_slot);
	void CollectTimings(UINT a_slot);

	bool planned = false;
	bool converting = false;
	bool bound = false;
	bool failed = false;
	DXGI_FORMAT displayFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

	// Single-GPU conversion: the game's finished frames on the same device, bound by SetLocalFrames.
	bool local = false;
	bool localBound = false;
	bool loggedLocalFrame = false;
	UINT localSlots = 2;
	DXGI_FORMAT sourceFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
	std::array<ID3D12Resource*, kSlots> localFrames{};  // not owned (DX12SwapChain's WrappedResources)

	UINT renderWidth = 0;
	UINT renderHeight = 0;
	UINT displayWidth = 0;
	UINT displayHeight = 0;
	int qualityMode = 1;

	int finalLane = -1;
	int sceneLane = -1;
	int depthLane = -1;
	int motionLane = -1;

	winrt::com_ptr<ID3D12Device> device;
	ID3D12CommandQueue* queue = nullptr;  // the FFX game queue, not owned

	std::array<winrt::com_ptr<ID3D12Resource>, kSlots> outputs;  // FSR 4.1 results (UI-free, display size)
	winrt::com_ptr<ID3D12Resource> composite;                    // UI over the FSR 4.1 result, copied to the back buffer
	                                                             // (display format)

	winrt::com_ptr<ID3D12RootSignature> rootSignature;
	winrt::com_ptr<ID3D12PipelineState> pipeline;      // Composite (upscaling only)
	winrt::com_ptr<ID3D12PipelineState> copyPipeline;  // Copy: finished frame -> composite (non-RGBA8 swap chain)
	winrt::com_ptr<ID3D12DescriptorHeap> descriptors;
	UINT descriptorSize = 0;

	// The upscale's own list per slot, submitted ahead of the frame's list on the same queue.
	std::array<winrt::com_ptr<ID3D12CommandAllocator>, kSlots> allocators;
	winrt::com_ptr<ID3D12GraphicsCommandList> list;

	std::array<bool, kSlots> pending{};    // BeginFrame ran, upscale not queued yet
	std::array<bool, kSlots> submitted{};  // upscale queued for this frame
	std::array<FrameParams, kSlots> params{};
	bool lastFrameUpscaled = false;
	LARGE_INTEGER lastDispatch{};

	// Timestamps on the direct queue (durations only).
	winrt::com_ptr<ID3D12QueryHeap> queryHeap;
	winrt::com_ptr<ID3D12Resource> readback;
	UINT64 timestampFrequency = 0;
	std::array<UINT, kSlots> written{};  // bit per mark recorded since the slot was last read

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

	Average statUpscale;    // ms, FSR 4.1 on the display GPU
	Average statComposite;  // ms, composite + copy into the back buffer
	UINT64 statUpscaled = 0;
	UINT64 statNative = 0;
	UINT64 statResets = 0;
	LARGE_INTEGER statLastLog{};
	bool loggedFirstDispatch = false;
	bool loggedSizeMismatch = false;
};
