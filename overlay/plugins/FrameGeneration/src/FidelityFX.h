#pragma once

// FSR SDK 2.4 API headers, vendored unmodified under include/ffx2 (MIT). They describe both
// runtimes this plugin can load: the struct type IDs and layouts used with FidelityFX SDK 1.1.4
// are unchanged in 2.4, so the old runtime is driven with the same headers and simply never
// receives the structs that only 2.x knows (version descriptors, PrepareV2, HUD-less format).
#include <ffx2/api/include/ffx_api.hpp>
#include <ffx2/api/include/ffx_api_loader.h>
#include <ffx2/api/include/ffx_api_types.h>
#include <ffx2/api/include/dx12/ffx_api_dx12.hpp>
#include <ffx2/framegeneration/include/ffx_framegeneration.hpp>
#include <ffx2/framegeneration/include/dx12/ffx_api_framegeneration_dx12.hpp>
#include <ffx2/upscalers/include/ffx_upscale.hpp>

#include <string>

#include "Buffer.h"

// Entry points of whichever runtime DLL LoadFFX() loaded. The SDK's own ffx:: call helpers call
// the ffx* exports directly (meant for linking an import library), so calls go through here.
extern ffxFunctions ffxModule;

namespace ffxm
{
	template <class... Desc>
	inline ffx::ReturnCode CreateContext(ffx::Context& a_context, Desc&... a_desc)
	{
		return static_cast<ffx::ReturnCode>(ffxModule.CreateContext(&a_context, ffx::LinkHeaders(a_desc.header...), nullptr));
	}

	template <class... Desc>
	inline ffx::ReturnCode Configure(ffx::Context& a_context, Desc&... a_desc)
	{
		return static_cast<ffx::ReturnCode>(ffxModule.Configure(&a_context, ffx::LinkHeaders(a_desc.header...)));
	}

	template <class... Desc>
	inline ffx::ReturnCode Dispatch(ffx::Context& a_context, Desc&... a_desc)
	{
		return static_cast<ffx::ReturnCode>(ffxModule.Dispatch(&a_context, ffx::LinkHeaders(a_desc.header...)));
	}

	template <class... Desc>
	inline ffx::ReturnCode Query(ffx::Context& a_context, Desc&... a_desc)
	{
		return static_cast<ffx::ReturnCode>(ffxModule.Query(&a_context, ffx::LinkHeaders(a_desc.header...)));
	}

	template <class... Desc>
	inline ffx::ReturnCode QueryGlobal(Desc&... a_desc)
	{
		return static_cast<ffx::ReturnCode>(ffxModule.Query(nullptr, ffx::LinkHeaders(a_desc.header...)));
	}

	const char* ReturnCodeName(uint32_t a_rc);
}

class FidelityFX
{
public:
	static FidelityFX* GetSingleton()
	{
		static FidelityFX singleton;
		return &singleton;
	}

	// iFSRRuntime. kLegacy is what this plugin shipped with and is driven exactly as before.
	enum class Runtime : int
	{
		kLegacy = 0,  // FidelityFX SDK 1.1.4: amd_fidelityfx_dx12.dll, FSR 3.1.4
		kSDK24 = 1    // FSR SDK 2.4: amd_fidelityfx_loader_dx12.dll + amd_fidelityfx_framegeneration_dx12.dll
	};

	HMODULE module = nullptr;
	Runtime runtime = Runtime::kLegacy;

	ffx::Context swapChainContext{};
	ffx::Context frameGenContext{};
	HWND window = nullptr;
	// The swap chain's real format, read from its back buffer after creation (an HDR add-on may have
	// changed it from the game's RGBA8). Frame generation is created with it.
	DXGI_FORMAT displayFormat = DXGI_FORMAT_UNKNOWN;
	// Frame generation knows the RGBA8 HUD-less frame's format: SDK 2.4 is told it, SDK 1.1.4 assumes
	// the back buffer's. False with 1.1.4 under a back buffer of another format (HDR add-on); frame
	// generation then stays off (DX12SwapChain::Present), because giving FFX no HUD-less frame
	// instead leaves it without a scene (black screen, found on test/mv-freeze-hud-bars).
	bool hudlessFormatKnown = true;

	// FG-GPU1: FSR 4.1 upscaling on the display GPU (SDK 2.4 only; see SecondaryUpscale).
	ffx::Context upscaleContext{};
	std::string upscalerProvider;

	// What frame generation reads instead of the plugin's own buffers on a frame the display GPU
	// upscaled: the FSR 4.1 output as the UI-free frame, and the size and jitter it was rendered at.
	struct FrameOverride
	{
		ID3D12Resource* hudless = nullptr;
		uint32_t renderWidth = 0;
		uint32_t renderHeight = 0;
		float jitterX = 0.0f;  // render pixels, as Upscaling.dll applied it
		float jitterY = 0.0f;
	};

	void LoadFFX();
	// Creates the FFX frame-generation swap chain and the frame generation context. With the SDK
	// 2.4 runtime a creation failure falls back to SDK 1.1.4 once, so the game still starts.
	void CreateSwapChain(IDXGIFactory5* a_dxgiFactory, DXGI_SWAP_CHAIN_DESC1* a_desc, HWND a_window,
		ID3D12CommandQueue* a_queue, IDXGISwapChain4** a_swapChain);
	void Present(bool a_useFrameGeneration, const FrameOverride* a_override = nullptr);

	// FSR 4.x upscaler on a_device for a_render -> a_display. Only the SDK 2.4 runtime, and only an
	// FSR 4 provider: false (logged) when the runtime, GPU or driver does not offer one.
	bool CreateUpscaler(ID3D12Device* a_device, uint32_t a_renderWidth, uint32_t a_renderHeight, uint32_t a_displayWidth,
		uint32_t a_displayHeight, bool a_debugChecking);
	ffx::ReturnCode DispatchUpscale(ffx::DispatchDescUpscale& a_dispatch);

	// The world camera's vertical field of view in radians (FSR upscaling and frame generation).
	// The renderer's camera state belongs to whichever pass ran last (a 2D camera after imagespace,
	// another one at Present), so it is sampled in the world pass and latched; 1 rad until then.
	static void SampleWorldCameraFov(float a_aspect, const char* a_where);
	static float VerticalFov();

	[[nodiscard]] const char* RuntimeName() const;

private:
	bool LoadRuntime(Runtime a_runtime);
	bool CreateContexts(IDXGIFactory5* a_dxgiFactory, DXGI_SWAP_CHAIN_DESC1* a_desc, HWND a_window,
		ID3D12CommandQueue* a_queue, IDXGISwapChain4** a_swapChain);
	void DestroyContexts(IDXGISwapChain4** a_swapChain);
	void LogEnvironment(HWND a_window);
};
