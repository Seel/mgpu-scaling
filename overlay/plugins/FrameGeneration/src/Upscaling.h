#pragma once

#include "Buffer.h"
#include "FrameSlots.h"

#include "SimpleIni.h"

class Upscaling
{
public:
	enum class FrameGenType : int { kFSR3 = 0, kDLSSG = 1, kXeSSFG = 2 };

	static Upscaling* GetSingleton()
	{
		static Upscaling singleton;
		return &singleton;
	}

	struct Settings
	{
		bool frameGenerationMode = true;
		bool frameLimitMode = true;
		bool disableInMenus = true;  ///< Disable frame generation in menus to avoid interpolation artifacts
		bool debugLogging = false;
		int frameGenType = 0;    // 0=FSR3, 1=DLSS-G, 2=XeSS-FG
		int frameGenFrames = 1;  // 1=2x, 2=3x, 3=4x (MFG, RTX 50+ only)
		bool secondaryGPU = false;       ///< Run FSR3 FG and present on a second GPU (the one driving the monitor)
		std::string secondaryGPUName;    ///< Optional: substring of the secondary GPU's name, if auto-selection is ambiguous
		bool sendHudlessFrame = true;    ///< Send the extra UI-free frame (off = less PCIe, HUD smears)
		// Test (branch test/mv-freeze-hud-bars): zero the motion vectors under the HUD bars so
		// frame generation holds them still. Re-read with the other settings, so it can be
		// flipped in the ini while the game runs.
		bool freezeHudMotionVectors = true;
		bool asyncUnpack = true;         ///< Secondary GPU unpacks on a copy queue, lane by lane (off = on the direct queue)
		bool earlyTransfer = true;       ///< Copy depth/MVs/HUD-less at their frame hooks, not at Present
		bool earlyUnpack = true;         ///< ...and queue the display GPU's read of them there too
		int halfResDepthMV = 1;          ///< 0 full, 1 depth at half res (rebuilt on the display GPU), 2 depth + MVs at half res
		int fsrRuntime = 1;              ///< 0 FidelityFX SDK 1.1.4 (FSR 3.1.4), 1 FSR SDK 2.4 (newest provider offered)
		int fsrSwapChainProvider = 0;    ///< SDK 2.4 only: 0 newest offered, 1 newest 3.x (A/B against 4.0.0 pacing)
		int fsrFrameGenProvider = 0;     ///< SDK 2.4 only: 0 newest offered (4.x ML where available), 1 newest 3.x
		bool extraFrameInFlight = false; ///< Second-GPU mode: 3 swap-chain buffers instead of 2 (FrameSlots.h); restart
		bool directGpuCopy = false;      ///< Second-GPU mode: the render GPU writes into the display GPU's VRAM (PeerCopy.h); restart
	};

	Settings settings;

	// What CreateFrameGenerationResources set up from settings.halfResDepthMV (0 when not on a
	// second GPU or when setup failed). Fixed for the session: the buffers and lanes are sized for it.
	int halfResMode = 0;
	// Whether CreateFrameGenerationResources registered the HUD-less lane (settings.sendHudlessFrame
	// at that time). Fixed for the session, so changing the setting in MCM waits for a restart
	// instead of leaving FSR3 with a lane that is no longer filled.
	bool hudlessActive = true;

	// FG-GPU1: FSR 4.1 upscaling on the second GPU (SecondaryUpscale). Upscaling.dll reports each
	// frame, before it renders, whether it renders it for the second GPU to upscale, at which size
	// and with which jitter (render pixels). Cleared at Present, so a frame it did not report is native.
	struct ExternalFrame
	{
		bool valid = false;
		bool active = false;
		uint32_t width = 0;
		uint32_t height = 0;
		float jitterX = 0.0f;
		float jitterY = 0.0f;
		float sharpness = 0.0f;
		bool captured = false;  // PostDisplay already took the scene (it must not take the stretch back)
	};
	ExternalFrame externalFrame;
	// The lanes carry the render-resolution scene, depth and motion vectors (fixed for the session).
	bool secondaryUpscaleLanes = false;
	UINT laneWidth = 0;
	UINT laneHeight = 0;
	// PostDisplay copied this frame's scene for the second GPU (per frameIndex, cleared at Present).
	bool frameUpscaled[kMaxFrameSlots] = {};
	// Render GPU: the full-size frame buffer is filled from the scene before the UI is drawn.
	Texture2D* sceneBlitTarget = nullptr;
	ID3D11ComputeShader* sceneBlitCS = nullptr;
	bool CreateSceneBlit();
	void FillFrameBufferFromScene(ID3D11DeviceContext* a_context, ID3D11Resource* a_frameBuffer);

	bool highFPSPhysicsFixLoaded = false;

	FrameGenType activeFrameGenType = FrameGenType::kFSR3;
	bool d3d12Interop = false;
	double refreshRate = 0.0f;

	// One per frame in flight (DX12SwapChain::frameSlots), indexed by frameIndex.
	Texture2D* HUDLessBufferShared[kMaxFrameSlots]{};
	Texture2D* depthBufferShared[kMaxFrameSlots]{};
	Texture2D* motionVectorBufferShared[kMaxFrameSlots]{};
	Texture2D* UIColorAlphaShared[kMaxFrameSlots]{};

	winrt::com_ptr<ID3D12Resource> HUDLessBufferShared12[kMaxFrameSlots];
	winrt::com_ptr<ID3D12Resource> depthBufferShared12[kMaxFrameSlots];
	winrt::com_ptr<ID3D12Resource> motionVectorBufferShared12[kMaxFrameSlots];
	winrt::com_ptr<ID3D12Resource> UIColorAlphaShared12[kMaxFrameSlots];

	// Test: the all-zero texture ZeroHudMotionVectors copies from, and its one-time log flag.
	winrt::com_ptr<ID3D11Texture2D> mvFreezeZeroTexture;
	bool mvFreezeLogged = false;

	ID3D11ComputeShader* copyDepthToSharedBufferCS = nullptr;
	ID3D11ComputeShader* generateSharedBuffersCS = nullptr;
	ID3D11ComputeShader* generateUIBufferCS = nullptr;
	ID3D11ComputeShader* halfResPostAlphaCS = nullptr;  // halfResMode: GenerateSharedBuffersCS at half res
	ID3D11ComputeShader* halfResCopyCS = nullptr;       // halfResMode: the no-reticle copy at half res

	bool setupBuffers = false;

	bool imagespaceComplete = false;

	void LoadSettings();
	void ReloadSettingsIfNeeded();

	void PostPostLoad();

	void CreateFrameGenerationResources();
	// Decides halfResMode and compiles what it needs; falls back to 0 (logged) on any failure.
	void SetupHalfResDepthMV(UINT a_width, UINT a_height);
	void PreAlpha();
	void PostAlpha();
	void CopyBuffersToSharedResources();
	// Test (branch test/mv-freeze-hud-bars): zero the motion vectors under the HUD rectangles so
	// frame generation holds those pixels still. Rectangles are in display (swap chain) pixels.
	void ZeroHudMotionVectors(ID3D11DeviceContext* a_context);
	void GenerateUIBuffer();

	static void TimerSleepQPC(int64_t targetQPC);

	void FrameLimiter(bool a_useFrameGeneration);

	void GameFrameLimiter();

	static double GetRefreshRate(HWND a_window);

	void PostDisplay();

	void Reset();

	static void InstallHooks();
};
