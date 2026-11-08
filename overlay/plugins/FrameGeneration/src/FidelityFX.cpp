#include "FidelityFX.h"

#include "Upscaling.h"

#include "DX12SwapChain.h"
#include "MGPUScaling.h"

#include <atomic>
#include <cmath>
#include <cwctype>
#include <format>
#include <initializer_list>
#include <vector>

ffxFunctions ffxModule;

namespace
{
	constexpr const wchar_t* kFfxFolder = mgpu::kFidelityFXDir;
	constexpr const wchar_t* kLegacyDll = L"amd_fidelityfx_dx12.dll";
	constexpr const wchar_t* kLoaderDll = L"amd_fidelityfx_loader_dx12.dll";
	constexpr const wchar_t* kFrameGenDll = L"amd_fidelityfx_framegeneration_dx12.dll";
	constexpr const wchar_t* kUpscalerDll = L"amd_fidelityfx_upscaler_dx12.dll";

	std::string Narrow(const wchar_t* a_text)
	{
		if (!a_text || !*a_text)
			return {};
		const int size = WideCharToMultiByte(CP_UTF8, 0, a_text, -1, nullptr, 0, nullptr, nullptr);
		if (size <= 1)
			return {};
		std::string out(static_cast<size_t>(size - 1), '\0');
		WideCharToMultiByte(CP_UTF8, 0, a_text, -1, out.data(), size, nullptr, nullptr);
		return out;
	}

	// FFX runtime messages, SDK 2.4 only. FFX calls this from its own threads too; the log sinks
	// are the thread-safe (_mt) spdlog sinks.
	void FfxMessage(uint32_t a_type, const wchar_t* a_message)
	{
		static std::atomic<uint32_t> count{ 0 };
		const uint32_t n = count.fetch_add(1);
		if (n > 200)
			return;
		if (n == 200) {
			REX::WARN("[FSR] further FFX runtime messages are not logged");
			return;
		}
		if (a_type == FFX_API_MESSAGE_TYPE_ERROR)
			REX::ERROR("[FSR] runtime: {}", Narrow(a_message));
		else
			REX::WARN("[FSR] runtime: {}", Narrow(a_message));
	}

	// Generated-frame dispatches. The FFX swap chain calls this from its own thread when it
	// presents; a failure here means that frame shows no generated frame, nothing else.
	std::atomic<uint64_t> callbackOk{ 0 };
	std::atomic<uint64_t> callbackFailed{ 0 };
	std::atomic<uint32_t> callbackLastError{ 0 };

	ffxReturnCode_t FrameGenerationCallback(ffxDispatchDescFrameGeneration* a_params, void* a_userCtx)
	{
		const ffxReturnCode_t rc = ffxModule.Dispatch(reinterpret_cast<ffxContext*>(a_userCtx), &a_params->header);
		if (rc == FFX_API_RETURN_OK) {
			callbackOk.fetch_add(1, std::memory_order_relaxed);
		} else {
			callbackLastError.store(rc, std::memory_order_relaxed);
			if (callbackFailed.fetch_add(1, std::memory_order_relaxed) == 0)
				REX::ERROR("[FSR] generating a frame failed: {} ({}); frames are shown without generation while this lasts",
					ffxm::ReturnCodeName(rc), rc);
		}
		return rc;
	}

	// Links the non-null headers in order; returns the first. The SDK's LinkHeaders needs a fixed
	// set of descriptors, and which optional ones are chained here depends on the runtime.
	ffxApiHeader* Chain(std::initializer_list<ffxApiHeader*> a_headers)
	{
		ffxApiHeader* first = nullptr;
		ffxApiHeader* last = nullptr;
		for (auto* header : a_headers) {
			if (!header)
				continue;
			header->pNext = nullptr;
			if (last)
				last->pNext = header;
			else
				first = header;
			last = header;
		}
		return first;
	}

	struct ProviderList
	{
		std::vector<uint64_t> ids;
		std::vector<std::string> names;  // copied: the runtime may reuse its name buffers
	};

	ProviderList QueryProviders(uint64_t a_createDescType, ID3D12Device* a_device)
	{
		ProviderList list;
		ffx::QueryDescGetVersions query{};
		query.createDescType = a_createDescType;
		query.device = a_device;
		uint64_t count = 0;
		query.outputCount = &count;
		if (ffxm::QueryGlobal(query) != ffx::ReturnCode::Ok || count == 0)
			return list;

		std::vector<uint64_t> ids(count, 0);
		std::vector<const char*> names(count, nullptr);
		query.versionIds = ids.data();
		query.versionNames = names.data();
		if (ffxm::QueryGlobal(query) != ffx::ReturnCode::Ok)
			return list;

		for (uint64_t i = 0; i < count && i < ids.size(); ++i) {
			list.ids.push_back(ids[i]);
			list.names.emplace_back(names[i] ? names[i] : "?");
		}
		return list;
	}

	std::string Join(const ProviderList& a_list)
	{
		if (a_list.names.empty())
			return "none";
		std::string out;
		for (const auto& name : a_list.names) {
			if (!out.empty())
				out += ", ";
			out += name;
		}
		return out;
	}

	bool HasMajor(const ProviderList& a_list, char a_major)
	{
		for (const auto& name : a_list.names)
			if (!name.empty() && name[0] == a_major)
				return true;
		return false;
	}

	// iFSR*Provider = 1 pins the newest 3.x provider (the lists come newest first). Only IDs the
	// runtime itself returned are used, as AMD requires. 0 = no override (newest offered).
	uint64_t PickOverride(const ProviderList& a_list, int a_preference, const char* a_what)
	{
		if (a_preference != 1)
			return 0;
		for (size_t i = 0; i < a_list.ids.size(); ++i)
			if (a_list.names[i].starts_with("3."))
				return a_list.ids[i];
		REX::WARN("[FSR] no 3.x {} provider offered; using the newest", a_what);
		return 0;
	}

	std::string ProviderName(ffx::Context& a_context)
	{
		ffx::InitHelper<ffxQueryGetProviderVersion> query{};
		if (ffxm::Query(a_context, query) != ffx::ReturnCode::Ok || !query.versionName)
			return "unknown";
		return query.versionName;
	}

	void LogModule(const wchar_t* a_name)
	{
		HMODULE handle = GetModuleHandleW(a_name);
		if (!handle) {
			REX::INFO("[FSR] {} is not loaded", Narrow(a_name));
			return;
		}
		wchar_t path[MAX_PATH]{};
		GetModuleFileNameW(handle, path, MAX_PATH);
		std::wstring lower = path;
		for (auto& c : lower)
			c = static_cast<wchar_t>(towlower(c));
		// The SDK 2.4 loader looks for effect DLLs next to Fallout4.exe before its own folder.
		if (lower.find(L"\\f4se\\plugins\\mgpuscaling\\fidelityfx\\") == std::wstring::npos)
			REX::WARN("[FSR] {} was loaded from {}, not from this plugin's FidelityFX folder (another mod's copy?)",
				Narrow(a_name), Narrow(path));
		else
			REX::INFO("[FSR] {} loaded from {}", Narrow(a_name), Narrow(path));
	}

	float Component(const __m128& a_v, int a_i)
	{
		alignas(16) float f[4];
		_mm_store_ps(f, a_v);
		return f[a_i];
	}

	void StoreUnit(float (&a_out)[3], const __m128& a_v)
	{
		alignas(16) float f[4];
		_mm_store_ps(f, a_v);
		const float length = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
		const float scale = length > 1e-6f ? 1.0f / length : 0.0f;
		a_out[0] = f[0] * scale;
		a_out[1] = f[1] * scale;
		a_out[2] = f[2] * scale;
	}
}

const char* ffxm::ReturnCodeName(uint32_t a_rc)
{
	switch (a_rc) {
	case FFX_API_RETURN_OK:
		return "OK";
	case FFX_API_RETURN_ERROR:
		return "ERROR";
	case FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE:
		return "UNKNOWN_DESCTYPE";
	case FFX_API_RETURN_ERROR_RUNTIME_ERROR:
		return "RUNTIME_ERROR";
	case FFX_API_RETURN_NO_PROVIDER:
		return "NO_PROVIDER";
	case FFX_API_RETURN_ERROR_MEMORY:
		return "ERROR_MEMORY";
	case FFX_API_RETURN_ERROR_PARAMETER:
		return "ERROR_PARAMETER";
	case FFX_API_RETURN_PROVIDER_NO_SUPPORT_NEW_DESCTYPE:
		return "PROVIDER_NO_SUPPORT_NEW_DESCTYPE";
	default:
		return "?";
	}
}

const char* FidelityFX::RuntimeName() const
{
	return runtime == Runtime::kSDK24 ? "FSR SDK 2.4" : "FidelityFX SDK 1.1.4 (FSR 3.1.4)";
}

bool FidelityFX::LoadRuntime(Runtime a_runtime)
{
	const std::wstring path = std::wstring(kFfxFolder) + (a_runtime == Runtime::kSDK24 ? kLoaderDll : kLegacyDll);
	HMODULE handle = LoadLibraryW(path.c_str());
	if (!handle) {
		const DWORD error = GetLastError();
		REX::WARN("[FSR] could not load {} (error {})", Narrow(path.c_str()), error);
		return false;
	}

	ffxFunctions functions{};
	ffxLoadFunctions(&functions, handle);
	if (!functions.CreateContext || !functions.DestroyContext || !functions.Configure || !functions.Query || !functions.Dispatch) {
		REX::WARN("[FSR] {} does not export the FFX API", Narrow(path.c_str()));
		FreeLibrary(handle);
		return false;
	}

	module = handle;
	ffxModule = functions;
	runtime = a_runtime;
	REX::INFO("[FSR] runtime: {} ({})", RuntimeName(), Narrow(path.c_str()));

	if (runtime == Runtime::kSDK24) {
		// Route FFX's own error/warning messages into this log.
		ffx::ConfigureDescGlobalDebug debug{};
		debug.effectId = FFX_API_EFFECT_ID_FRAMEGENERATION;
		debug.fpMessage = FfxMessage;
		debug.debugLevel = FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_WARNINGS;
		auto rc = ffxModule.Configure(nullptr, &debug.header);
		if (rc != FFX_API_RETURN_OK) {
			debug.effectId = FFX_API_EFFECT_ID_GENERAL;
			rc = ffxModule.Configure(nullptr, &debug.header);
		}
		if (rc != FFX_API_RETURN_OK)
			REX::INFO("[FSR] runtime messages are not available ({})", ffxm::ReturnCodeName(rc));
	}
	return true;
}

void FidelityFX::LoadFFX()
{
	const auto wanted = Upscaling::GetSingleton()->settings.fsrRuntime == 1 ? Runtime::kSDK24 : Runtime::kLegacy;
	if (LoadRuntime(wanted))
		return;
	if (wanted == Runtime::kSDK24) {
		REX::WARN("[FSR] iFSRRuntime = 1 but the FSR SDK 2.4 runtime did not load; using FidelityFX SDK 1.1.4");
		LoadRuntime(Runtime::kLegacy);
	}
}

void FidelityFX::LogEnvironment(HWND a_window)
{
	// Driver-Assisted Frame Pacing (swap chain 4.0.0+) is chosen by the driver without telling the
	// application. Of its conditions, only these two are visible from here: the window must cover
	// the display, and VSync must be off (logged at Present).
	RECT client{};
	POINT origin{ 0, 0 };
	MONITORINFO monitor{};
	monitor.cbSize = sizeof(monitor);
	if (!GetClientRect(a_window, &client) || !ClientToScreen(a_window, &origin) ||
		!GetMonitorInfoW(MonitorFromWindow(a_window, MONITOR_DEFAULTTONEAREST), &monitor))
		return;

	const auto& m = monitor.rcMonitor;
	const bool covers = origin.x <= m.left && origin.y <= m.top && origin.x + client.right >= m.right && origin.y + client.bottom >= m.bottom;
	REX::INFO("[FSR] window {}x{} at ({},{}), display {}x{} at ({},{}): {}", client.right, client.bottom, origin.x, origin.y,
		m.right - m.left, m.bottom - m.top, m.left, m.top,
		covers ? "covers the display" :
				 "does NOT cover the display - driver-assisted frame pacing needs fullscreen or a borderless window covering it");
}

bool FidelityFX::CreateContexts(IDXGIFactory5* a_dxgiFactory, DXGI_SWAP_CHAIN_DESC1* a_desc, HWND a_window,
	ID3D12CommandQueue* a_queue, IDXGISwapChain4** a_swapChain)
{
	auto upscaling = Upscaling::GetSingleton();
	auto device = DX12SwapChain::GetSingleton()->d3d12Device.get();
	const bool sdk24 = runtime == Runtime::kSDK24;

	// ---- swap chain
	ffx::CreateContextDescFrameGenerationSwapChainForHwndDX12 swapChainDesc{};
	swapChainDesc.desc = a_desc;
	swapChainDesc.dxgiFactory = a_dxgiFactory;
	swapChainDesc.fullscreenDesc = nullptr;
	swapChainDesc.gameQueue = a_queue;
	swapChainDesc.hwnd = a_window;
	swapChainDesc.swapchain = a_swapChain;

	uint32_t rc = FFX_API_RETURN_OK;
	if (sdk24) {
		const auto offered = QueryProviders(FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12, device);
		REX::INFO("[FSR] swap chain providers offered: {}", Join(offered));

		ffx::CreateContextDescFrameGenerationSwapChainVersionDX12 version{};
		version.version = FFX_FRAMEGENERATION_SWAPCHAIN_DX12_VERSION;
		ffx::CreateContextDescOverrideVersion pin{};
		pin.versionId = PickOverride(offered, upscaling->settings.fsrSwapChainProvider, "swap chain");

		rc = ffxModule.CreateContext(&swapChainContext,
			Chain({ &swapChainDesc.header, &version.header, pin.versionId ? &pin.header : nullptr }), nullptr);
	} else {
		rc = ffxModule.CreateContext(&swapChainContext, Chain({ &swapChainDesc.header }), nullptr);
	}
	if (rc != FFX_API_RETURN_OK || !*a_swapChain) {
		REX::ERROR("[FSR] {}: creating the frame generation swap chain failed: {} ({})", RuntimeName(), ffxm::ReturnCodeName(rc), rc);
		return false;
	}

	std::string swapChainProvider = "3.1.4";
	if (sdk24) {
		swapChainProvider = ProviderName(swapChainContext);
		REX::INFO("[FSR] swap chain provider: {}{}", swapChainProvider,
			swapChainProvider.starts_with("3") ? " (no driver-assisted frame pacing; that needs 4.0.0 or newer)" :
												 " (driver-assisted frame pacing capable)");
	}

	// The format the swap chain really has. An add-on can change it while FFX creates it: ReShade's
	// AutoHDR turns it into R10G10B10A2 (HDR10) or R16G16B16A16_FLOAT (scRGB). FFX creates its
	// back buffers from the real one, so frame generation and everything that writes the back
	// buffer must use this format, not the one the game asked for. GetDesc1 cannot tell: ReShade
	// reports the requested format when an add-on changed it. The back buffer itself can.
	displayFormat = a_desc->Format;
	winrt::com_ptr<ID3D12Resource> backBuffer;
	if (SUCCEEDED((*a_swapChain)->GetBuffer(0, IID_PPV_ARGS(backBuffer.put()))) && backBuffer) {
		const DXGI_FORMAT bufferFormat = backBuffer->GetDesc().Format;
		if (bufferFormat != DXGI_FORMAT_UNKNOWN)
			displayFormat = bufferFormat;
	} else {
		REX::WARN("[FSR] could not read the swap chain's back buffer; assuming format {}", static_cast<int>(displayFormat));
	}
	// The HUD-less copy is RGBA8 (see the hudless descriptor below); only SDK 2.4 can be told so.
	hudlessFormatKnown = sdk24 || displayFormat == DXGI_FORMAT_R8G8B8A8_UNORM;
	if (displayFormat != a_desc->Format) {
		REX::WARN("[FSR] the swap chain was created as format {} but is format {} (an HDR add-on such as ReShade's AutoHDR "
				  "changed it); frame generation and everything this plugin writes into the back buffers follow it. Change "
				  "HDR settings only with the game closed",
			static_cast<int>(a_desc->Format), static_cast<int>(displayFormat));
		if (!hudlessFormatKnown)
			REX::ERROR("[FSR] {} cannot be told that the UI-free frame stays RGBA8 under a format {} back buffer, so frame "
					   "generation stays off this session (frames are shown as rendered). Use iFSRRuntime = 1 with HDR "
					   "add-ons",
				RuntimeName(), static_cast<int>(displayFormat));
	}

	// ---- frame generation
	ffx::CreateContextDescFrameGeneration createFg{};
	createFg.displaySize = { a_desc->Width, a_desc->Height };
	createFg.maxRenderSize = createFg.displaySize;
	createFg.flags = FFX_FRAMEGENERATION_ENABLE_ASYNC_WORKLOAD_SUPPORT;
	// SDR content in whatever container the swap chain has: HDR is applied after frame generation
	// (ReShade at the real Present), so FFX_FRAMEGENERATION_ENABLE_HIGH_DYNAMIC_RANGE stays off.
	createFg.backBufferFormat = ffxApiGetSurfaceFormatDX12(displayFormat);

	ffx::CreateBackendDX12Desc backend{};
	backend.device = device;

	if (sdk24) {
		const auto offered = QueryProviders(FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION, device);
		REX::INFO("[FSR] frame generation providers offered: {}", Join(offered));
		if (!HasMajor(offered, '4'))
			REX::INFO("[FSR] FSR 4 (ML) frame generation is not offered on this system. AMD requires Windows 11 and an "
					  "RX 9000-series GPU for it; the analytical FSR 3 interpolator is used");

		ffx::CreateContextDescFrameGenerationVersion version{};
		version.version = FFX_FRAMEGENERATION_VERSION;

		// The HUD-less copy is always RGBA8 (Upscaling::CreateFrameGenerationResources, and the FSR 4.1
		// output on the second GPU). Declare it when the back buffer is something else, e.g. BGRA8 or
		// the 10-bit/16-bit format of an HDR add-on.
		ffx::CreateContextDescFrameGenerationHudless hudless{};
		hudless.hudlessBackBufferFormat = ffxApiGetSurfaceFormatDX12(DXGI_FORMAT_R8G8B8A8_UNORM);
		const bool declareHudless = hudless.hudlessBackBufferFormat != createFg.backBufferFormat;

		ffx::CreateContextDescOverrideVersion pin{};
		pin.versionId = PickOverride(offered, upscaling->settings.fsrFrameGenProvider, "frame generation");

		rc = ffxModule.CreateContext(&frameGenContext,
			Chain({ &createFg.header, &backend.header, &version.header, declareHudless ? &hudless.header : nullptr,
				pin.versionId ? &pin.header : nullptr }),
			nullptr);
	} else {
		rc = ffxModule.CreateContext(&frameGenContext, Chain({ &createFg.header, &backend.header }), nullptr);
	}
	if (rc != FFX_API_RETURN_OK) {
		REX::ERROR("[FSR] {}: creating the frame generation context failed: {} ({})", RuntimeName(), ffxm::ReturnCodeName(rc), rc);
		return false;
	}

	if (sdk24) {
		const auto provider = ProviderName(frameGenContext);
		REX::INFO("[FSR] frame generation provider: {} ({})", provider,
			provider.starts_with("4") ? "FSR 4 ML frame generation" : "analytical FSR 3 interpolation");
		REX::INFO("[FSR] in use: swap chain {} + frame generation {}", swapChainProvider, provider);
		LogModule(kLoaderDll);
		LogModule(kFrameGenDll);
		LogEnvironment(a_window);
	}
	return true;
}

void FidelityFX::DestroyContexts(IDXGISwapChain4** a_swapChain)
{
	if (frameGenContext) {
		ffxModule.DestroyContext(&frameGenContext, nullptr);
		frameGenContext = nullptr;
	}
	if (swapChainContext) {
		ffxModule.DestroyContext(&swapChainContext, nullptr);
		swapChainContext = nullptr;
	}
	// The context held its own reference; this is the one handed to us at creation.
	if (a_swapChain && *a_swapChain) {
		(*a_swapChain)->Release();
		*a_swapChain = nullptr;
	}
}

bool FidelityFX::CreateUpscaler(ID3D12Device* a_device, uint32_t a_renderWidth, uint32_t a_renderHeight,
	uint32_t a_displayWidth, uint32_t a_displayHeight, bool a_debugChecking)
{
	if (runtime != Runtime::kSDK24) {
		REX::ERROR("[FSR4] FSR 4.1 upscaling needs the FSR SDK 2.4 runtime (iFSRRuntime = 1); this session runs {}",
			RuntimeName());
		return false;
	}
	if (upscaleContext)
		return true;

	// The FSR 4 providers are only offered for a device that can run them, so query with it.
	const auto offered = QueryProviders(FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE, a_device);
	REX::INFO("[FSR4] upscaler providers offered for the display GPU: {}", Join(offered));
	uint64_t fsr4 = 0;
	for (size_t i = 0; i < offered.ids.size(); ++i) {
		if (offered.names[i].starts_with("4.")) {
			fsr4 = offered.ids[i];  // newest first
			break;
		}
	}
	if (!fsr4) {
		REX::ERROR("[FSR4] no FSR 4 upscaler is offered for this GPU and driver (AMD lists RX 6000 or newer and a current "
				   "Adrenalin driver); upscaling on the second GPU is off");
		return false;
	}

	// Fallout 4's frame is upscaled after its imagespace, i.e. tonemapped LDR in a UNORM target:
	// perceptual colour. Depth is 0 at the near plane (not inverted), motion vectors are at render
	// resolution and carry no jitter (the same flags the upscaler uses for FSR 3.1).
	ffx::CreateContextDescUpscale create{};
	create.flags = FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE | FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;
	if (a_debugChecking)
		create.flags |= FFX_UPSCALE_ENABLE_DEBUG_CHECKING;
	create.maxRenderSize = { a_renderWidth, a_renderHeight };
	create.maxUpscaleSize = { a_displayWidth, a_displayHeight };
	create.fpMessage = FfxMessage;

	ffx::CreateContextDescUpscaleVersion version{};
	version.version = FFX_UPSCALER_VERSION;
	ffx::CreateBackendDX12Desc backend{};
	backend.device = a_device;
	ffx::CreateContextDescOverrideVersion pin{};
	pin.versionId = fsr4;

	const auto rc = ffxModule.CreateContext(&upscaleContext,
		Chain({ &create.header, &version.header, &backend.header, &pin.header }), nullptr);
	if (rc != FFX_API_RETURN_OK) {
		upscaleContext = nullptr;
		REX::ERROR("[FSR4] creating the upscaler context ({}x{} -> {}x{}) failed: {} ({})", a_renderWidth, a_renderHeight,
			a_displayWidth, a_displayHeight, ffxm::ReturnCodeName(rc), rc);
		return false;
	}

	upscalerProvider = ProviderName(upscaleContext);
	FfxApiEffectMemoryUsage memory{};
	ffx::QueryDescUpscaleGetGPUMemoryUsage memoryQuery{};
	memoryQuery.gpuMemoryUsageUpscaler = &memory;
	const bool haveMemory = ffxm::Query(upscaleContext, memoryQuery) == ffx::ReturnCode::Ok;
	REX::INFO("[FSR4] upscaler {} on the display GPU: {}x{} -> {}x{}{}{}", upscalerProvider, a_renderWidth, a_renderHeight,
		a_displayWidth, a_displayHeight,
		haveMemory ? std::format(", {:.1f} MB", static_cast<double>(memory.totalUsageInBytes) / (1024.0 * 1024.0)) : std::string{},
		a_debugChecking ? ", API checks on (bEnableDebugLogging)" : "");
	LogModule(kUpscalerDll);
	return true;
}

ffx::ReturnCode FidelityFX::DispatchUpscale(ffx::DispatchDescUpscale& a_dispatch)
{
	if (!upscaleContext)
		return ffx::ReturnCode::Error;
	return ffxm::Dispatch(upscaleContext, a_dispatch);
}

void FidelityFX::CreateSwapChain(IDXGIFactory5* a_dxgiFactory, DXGI_SWAP_CHAIN_DESC1* a_desc, HWND a_window,
	ID3D12CommandQueue* a_queue, IDXGISwapChain4** a_swapChain)
{
	window = a_window;
	if (CreateContexts(a_dxgiFactory, a_desc, a_window, a_queue, a_swapChain))
		return;

	if (runtime == Runtime::kSDK24) {
		REX::ERROR("[FSR] falling back to FidelityFX SDK 1.1.4 for this session (set iFSRRuntime = 0 to start with it)");
		DestroyContexts(a_swapChain);
		if (LoadRuntime(Runtime::kLegacy) && CreateContexts(a_dxgiFactory, a_desc, a_window, a_queue, a_swapChain))
			return;
	}
	REX::CRITICAL("[FidelityFX] Failed to create the frame generation swap chain or context!");
}

[[nodiscard]] static RE::BSGraphics::State* State_GetSingleton()
{
	REL::Relocation<RE::BSGraphics::State*> singleton{ REL::ID({ 600795, 2704621, 2704621 }) };
	return singleton.get();
}

[[nodiscard]] static RE::BSGraphics::RenderTargetManager* RenderTargetManager_GetSingleton()
{
	REL::Relocation<RE::BSGraphics::RenderTargetManager*> singleton{ REL::ID({ 1508457, 2666735, 2666735 }) };
	return singleton.get();
}

namespace
{
	float worldFov = 1.0f;  // FSR 3.1's long-standing value until the world pass has been sampled
	bool haveWorldFov = false;
	int fovLogLines = 0;
}

void FidelityFX::SampleWorldCameraFov(float a_aspect, const char* a_where)
{
	// The renderer's *current* camera state belongs to whichever pass ran last (a 2D camera in the
	// world-pass hooks and after imagespace, a narrow ~24 deg one at Present), but cameraDataCache
	// holds every camera of the frame. The world camera is the one rendered with the upscaler's
	// jitter and a perspective projection for the display aspect (P[0][0] * aspect == P[1][1]);
	// projMat is a real projection (it matches inverse(view) * viewProjUnjittered). If several
	// qualify, the widest is taken (UI/model cameras are narrower).
	static auto gameViewport = State_GetSingleton();

	struct Candidate
	{
		float p11 = 0.0f;
		float p00a = 0.0f;
		bool jitter = false;
		bool perspective = false;
	};
	const auto inspect = [a_aspect](const RE::BSGraphics::CameraStateData& a_state) {
		Candidate c;
		c.p11 = std::fabs(Component(a_state.camViewData.projMat[1], 1));
		c.p00a = std::fabs(Component(a_state.camViewData.projMat[0], 0)) * a_aspect;
		c.jitter = a_state.useJitter;
		c.perspective = std::isfinite(c.p11) && std::isfinite(c.p00a) && c.p11 > 0.1f && c.p11 < 20.0f &&
		                std::fabs(c.p00a - c.p11) <= 0.03f * c.p11;
		return c;
	};

	const auto& cache = gameViewport->cameraDataCache;
	const std::uint32_t count = static_cast<std::uint32_t>(cache.size());
	float best = 0.0f;
	for (std::uint32_t i = 0; i < count && i < 64; ++i) {
		const Candidate c = inspect(cache[i]);
		if (c.jitter && c.perspective)
			best = (std::max)(best, 2.0f * std::atan(1.0f / c.p11));
	}

	const auto report = [&](const char* a_what, float a_fov) {
		if (fovLogLines > 40)
			return;
		if (fovLogLines++ == 40) {
			REX::INFO("[FSR] further camera FOV changes are not logged");
			return;
		}
		// For comparison: the vertical FOV the game's FOV setting gives if it is a horizontal angle at
		// the display aspect, or at 4:3 (the older Creation Engine convention).
		auto camera = RE::PlayerCamera::GetSingleton();
		const float worldDeg = camera ? camera->worldFOV : 0.0f;
		const float halfTan = std::tan(worldDeg * 0.5f / 57.29578f);
		const float atDisplay = 2.0f * std::atan(halfTan / a_aspect) * 57.29578f;
		const float at43 = 2.0f * std::atan(halfTan * 0.75f) * 57.29578f;
		REX::INFO("[FSR] camera vertical FOV {}: {:.3f} rad ({:.1f} deg) at {}; PlayerCamera worldFOV {:.1f} = {:.1f} deg "
				  "vertical if horizontal at the display aspect, {:.1f} deg at 4:3; firstPersonFOV {:.1f}",
			a_what, a_fov, a_fov * 57.29578f, a_where, worldDeg, atDisplay, at43, camera ? camera->firstPersonFOV : 0.0f);
	};

	// The camera list, once and whenever no world camera is found, so the choice can be checked.
	static bool listed = false;
	if (!listed || best == 0.0f) {
		static int listings = 0;
		if (listings < 3) {
			++listings;
			listed = true;
			for (std::uint32_t i = 0; i < count && i < 16; ++i) {
				const Candidate c = inspect(cache[i]);
				REX::INFO("[FSR] camera {} of {}: jitter {} P[1][1] {:.4f} P[0][0]*aspect {:.4f} -> {}", i, count, c.jitter ? 1 : 0,
					c.p11, c.p00a, c.perspective ? std::format("{:.1f} deg vertical", 2.0f * std::atan(1.0f / c.p11) * 57.29578f) :
												   std::string("not a perspective projection"));
			}
		}
	}

	if (best == 0.0f) {
		static bool loggedMissing = false;
		if (!loggedMissing) {
			report("not found (no jittered perspective camera), keeping", worldFov);
			loggedMissing = true;
		}
		return;
	}
	// Logged when it moves by more than a degree (aiming down sights, scopes, 1st/3rd person).
	if (!haveWorldFov || std::fabs(best - worldFov) > 0.01745f) {
		worldFov = best;
		report(haveWorldFov ? "changed" : "first sample", best);
	}
	worldFov = best;
	haveWorldFov = true;
}

float FidelityFX::VerticalFov()
{
	return worldFov;
}

void FidelityFX::Present(bool a_useFrameGeneration, const FrameOverride* a_override)
{
	auto upscaling = Upscaling::GetSingleton();
	auto dx12SwapChain = DX12SwapChain::GetSingleton();
	auto commandList = dx12SwapChain->commandLists[dx12SwapChain->frameIndex].get();

	// FG-GPU1 upscaling: the UI-free frame is the FSR 4.1 output, not the render-resolution lane.
	ID3D12Resource* HUDLessColor = a_override ? a_override->hudless :
											  upscaling->HUDLessBufferShared12[dx12SwapChain->frameIndex].get();

	// Test branch (test/mv-freeze-hud-bars): bSendHudlessFrame = 0. FFX still needs a colour
	// resource to interpolate. Handing it none left the frame generation context without its
	// scene — the screen went black and only the UI composite survived. So give it a copy of the
	// composited back buffer instead: the UI difference is then zero, no UI is extracted, and the
	// HUD is interpolated together with the scene, which is what the switch means on one GPU.
	if (a_useFrameGeneration && !upscaling->settings.sendHudlessFrame) {
		// FFX was told the UI-free input is RGBA8 (CreateContextDescFrameGenerationHudless, when the
		// back buffer is another format) and a plain copy can only land in a matching format, so the
		// stand-in works while the back buffer really is RGBA8. Under an HDR add-on it does not:
		// show the composed frame without generation rather than a black screen.
		if (dx12SwapChain->displayFormat != DXGI_FORMAT_R8G8B8A8_UNORM) {
			static bool warned = false;
			if (!warned) {
				warned = true;
				REX::WARN("[FSR] bSendHudlessFrame = 0 (test branch) needs an RGBA8 back buffer, this one is format {}; "
						  "frame generation stays off while the switch is 0 (disable ReShade's AutoHDR to run the test)",
					static_cast<int>(dx12SwapChain->displayFormat));
			}
			a_useFrameGeneration = false;
		} else if (ID3D12Resource* standIn = dx12SwapChain->HudlessStandInFor(dx12SwapChain->frameIndex)) {
			ID3D12Resource* composed = dx12SwapChain->swapChainBuffers[dx12SwapChain->frameIndex].get();
			D3D12_RESOURCE_BARRIER toCopy[2]{};
			for (auto& barrier : toCopy) {
				barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
				barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
				barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			}
			toCopy[0].Transition.pResource = composed;
			toCopy[0].Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
			toCopy[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
			toCopy[1].Transition.pResource = standIn;
			toCopy[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
			toCopy[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
			commandList->ResourceBarrier(2, toCopy);
			commandList->CopyResource(standIn, composed);

			D3D12_RESOURCE_BARRIER toPresent[2]{};
			for (auto& barrier : toPresent) {
				barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
				barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
				barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			}
			toPresent[0].Transition.pResource = composed;
			toPresent[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
			toPresent[0].Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
			toPresent[1].Transition.pResource = standIn;
			toPresent[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
			toPresent[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
			commandList->ResourceBarrier(2, toPresent);

			HUDLessColor = standIn;
		}
	}
	auto depth = upscaling->depthBufferShared12[dx12SwapChain->frameIndex].get();
	auto motionVectors = upscaling->motionVectorBufferShared12[dx12SwapChain->frameIndex].get();

	ffx::ConfigureDescFrameGeneration configParameters{};

	if (a_useFrameGeneration) {
		configParameters.frameGenerationEnabled = true;

		configParameters.frameGenerationCallback = FrameGenerationCallback;
		configParameters.frameGenerationCallbackUserContext = &frameGenContext;

		configParameters.HUDLessColor = HUDLessColor ? ffxApiGetResourceDX12(HUDLessColor) : FfxApiResource({});
	}
	else {
		configParameters.frameGenerationEnabled = false;

		configParameters.frameGenerationCallbackUserContext = nullptr;
		configParameters.frameGenerationCallback = nullptr;

		configParameters.HUDLessColor = FfxApiResource({});
	}

	configParameters.presentCallback = nullptr;
	configParameters.presentCallbackUserContext = nullptr;

	static uint64_t frameID = 0;
	configParameters.frameID = frameID;
	configParameters.swapChain = dx12SwapChain->swapChain;
	configParameters.onlyPresentGenerated = false;
	configParameters.allowAsyncWorkloads = true;
	configParameters.flags = 0;

	// Full-screen generation rect starts at origin
	configParameters.generationRect.left = 0;
	configParameters.generationRect.top = 0;
	configParameters.generationRect.width = dx12SwapChain->swapChainDesc.Width;
	configParameters.generationRect.height = dx12SwapChain->swapChainDesc.Height;

	if (ffxm::Configure(frameGenContext, configParameters) != ffx::ReturnCode::Ok) {
		REX::CRITICAL("[FidelityFX] Failed to configure frame generation!");
	}

	static LARGE_INTEGER frequency = []() {
		LARGE_INTEGER freq;
		QueryPerformanceFrequency(&freq);
		return freq;
		}();

	static LARGE_INTEGER lastFrameTime = []() {
		LARGE_INTEGER time;
		QueryPerformanceCounter(&time);
		return time;
		}();

	LARGE_INTEGER currentFrameTime;
	QueryPerformanceCounter(&currentFrameTime);

	float deltaTime = static_cast<float>(currentFrameTime.QuadPart - lastFrameTime.QuadPart) / static_cast<float>(frequency.QuadPart);

	lastFrameTime = currentFrameTime;

	// SDK 2.4: tell FSR when generation restarts (menus, loading) instead of letting it
	// interpolate across the gap. The 1.1.4 prepare struct has no working reset field.
	static bool wasGenerating = false;
	const bool resetHistory = a_useFrameGeneration && !wasGenerating;
	wasGenerating = a_useFrameGeneration;

	if (a_useFrameGeneration) {
		static auto gameViewport = State_GetSingleton();
		static auto renderTargetManager = RenderTargetManager_GetSingleton();

		auto screenSize = float2(float(gameViewport->screenWidth), float(gameViewport->screenHeight));
		auto renderSize = float2(screenSize.x * renderTargetManager->dynamicWidthRatio, screenSize.y * renderTargetManager->dynamicHeightRatio);
		uint renderWidth = static_cast<uint>(renderSize.x);
		uint renderHeight = static_cast<uint>(renderSize.y);
		float jitterScale = 1.0f;

		// FG-GPU1 iHalfResDepthMV = 2: depth and motion vectors crossed PCIe at half resolution,
		// so FSR3 works at half the render size. The motion vectors are in UV units (scale ==
		// render size), so their scale follows the render size; the jitter is in render pixels.
		if (upscaling->halfResMode == 2) {
			renderWidth = (renderWidth + 1) / 2;
			renderHeight = (renderHeight + 1) / 2;
			renderSize = float2(static_cast<float>(renderWidth), static_cast<float>(renderHeight));
			jitterScale = 0.5f;
		}

		float2 jitter;
		jitter.x = -gameViewport->offsetX * screenSize.x / 2.0f;
		jitter.y = gameViewport->offsetY * screenSize.y / 2.0f;
		float2 jitterOffset(-jitter.x / renderTargetManager->dynamicWidthRatio * jitterScale,
			-jitter.y / renderTargetManager->dynamicHeightRatio * jitterScale);

		// FG-GPU1 upscaling: depth and motion vectors are the render-resolution lanes, and the size
		// and jitter come from the upscaler for this frame instead of the dynamic-resolution state,
		// which other hooks change around Present.
		if (a_override) {
			renderWidth = a_override->renderWidth;
			renderHeight = a_override->renderHeight;
			renderSize = float2(static_cast<float>(renderWidth), static_cast<float>(renderHeight));
			jitterOffset = float2(-a_override->jitterX, -a_override->jitterY);
		}

		// The fields both prepare structs share, filled the same way for both runtimes.
		auto fillCommon = [&](auto& a_prepare) {
			a_prepare.commandList = commandList;
			a_prepare.motionVectorScale.x = renderSize.x;
			a_prepare.motionVectorScale.y = renderSize.y;
			a_prepare.renderSize.width = renderWidth;
			a_prepare.renderSize.height = renderHeight;
			a_prepare.jitterOffset.x = jitterOffset.x;
			a_prepare.jitterOffset.y = jitterOffset.y;
			a_prepare.frameTimeDelta = deltaTime * 1000.f;
			a_prepare.cameraNear = *(float*)REL::ID({ 57985, 2712882, 2712882 }).address();
			a_prepare.cameraFar = *(float*)REL::ID({ 958877, 2712883, 2712883 }).address();
			a_prepare.cameraFovAngleVertical = 1.0f;
			a_prepare.viewSpaceToMetersFactor = 0.01428222656f;
			a_prepare.frameID = frameID;
			a_prepare.depth = ffxApiGetResourceDX12(depth);
			a_prepare.motionVectors = ffxApiGetResourceDX12(motionVectors);
		};

		ffx::ReturnCode prepareResult;
		if (runtime == Runtime::kSDK24) {
			// PrepareV2 carries the camera FSR 4 ML frame generation needs (FSR 3.1.6 accepts it too).
			ffx::DispatchDescFrameGenerationPrepareV2 dispatchParameters{};
			fillCommon(dispatchParameters);
			dispatchParameters.reset = resetHistory;

			// Fallout 4 renders camera-relative: the view matrix carries no translation, and
			// posAdjust is the camera's world position (the DLSS-G path uses it the same way).
			const auto& cameraState = gameViewport->cameraState;
			const auto& view = cameraState.camViewData;
			dispatchParameters.cameraPosition[0] = cameraState.posAdjust.x;
			dispatchParameters.cameraPosition[1] = cameraState.posAdjust.y;
			dispatchParameters.cameraPosition[2] = cameraState.posAdjust.z;
			StoreUnit(dispatchParameters.cameraUp, view.viewUp);
			StoreUnit(dispatchParameters.cameraRight, view.viewRight);
			StoreUnit(dispatchParameters.cameraForward, view.viewDir);

			dispatchParameters.cameraFovAngleVertical = VerticalFov();

			prepareResult = ffxm::Dispatch(frameGenContext, dispatchParameters);
		} else {
#pragma warning(push)
#pragma warning(disable: 4996)  // deprecated in 2.x headers; the only prepare struct 1.1.4 knows
			ffxDispatchDescFrameGenerationPrepare dispatchParameters{};
			dispatchParameters.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE;
			fillCommon(dispatchParameters);
			prepareResult = ffxm::Dispatch(frameGenContext, dispatchParameters);
#pragma warning(pop)
		}

		if (prepareResult != ffx::ReturnCode::Ok) {
			REX::CRITICAL("[FidelityFX] Failed to dispatch frame generation!");
		}
	}

	frameID++;

	// Generated-frame dispatch failures (and, once, the SDK 2.4 tally) every 10 s.
	static LARGE_INTEGER reportStart = currentFrameTime;
	static bool reportedOnce = false;
	if (static_cast<double>(currentFrameTime.QuadPart - reportStart.QuadPart) >= 10.0 * static_cast<double>(frequency.QuadPart)) {
		const uint64_t ok = callbackOk.exchange(0);
		const uint64_t failed = callbackFailed.exchange(0);
		if (failed > 0) {
			const uint32_t last = callbackLastError.load();
			REX::ERROR("[FSR] generated frames in the last 10 s: {} dispatched, {} failed (last error {} ({}))", ok, failed,
				ffxm::ReturnCodeName(last), last);
		} else if (runtime == Runtime::kSDK24 && !reportedOnce) {
			REX::INFO("[FSR] generated frames in the first 10 s: {} dispatched, none failed", ok);
		}
		if (runtime == Runtime::kSDK24 && !reportedOnce && window)
			LogEnvironment(window);
		reportedOnce = true;
		reportStart = currentFrameTime;
	}
}
