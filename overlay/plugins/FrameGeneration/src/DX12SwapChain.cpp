#include "DX12SwapChain.h"

#include <dxgi1_6.h>

#include <algorithm>
#include <format>

#include "CrossAdapterBridge.h"
#include "FidelityFX.h"
#include "SecondaryUpscale.h"
#include "Streamline.h"
#include "UICompositor.h"
#include "UITiles.h"
#include "Upscaling.h"
#include "XeSSFG.h"

extern bool enbLoaded;

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

void DX12SwapChain::CreateD3D12Device(IDXGIAdapter* a_adapter)
{
	DX::ThrowIfFailed(D3D12CreateDevice(a_adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&d3d12Device)));
	REX::INFO("[FG] D3D12 device created");
}

void DX12SwapChain::CreateD3D12DevicesSecondaryGPU(IDXGIAdapter* a_gameAdapter, HWND a_window)
{
	auto upscaling = Upscaling::GetSingleton();
	auto bridge = CrossAdapterBridge::GetSingleton();

	const auto fallBack = [&](const char* a_reason) {
		REX::ERROR("[FG-GPU1] {}; running frame generation on the game's GPU instead", a_reason);
		bridge->Shutdown();
		interopDevice = nullptr;
		d3d12Device = nullptr;
		secondaryGPU = false;
		CreateD3D12Device(a_gameAdapter);
	};

	REX::INFO("[FG-GPU1] game renders on \"{}\"", CrossAdapterBridge::AdapterName(a_gameAdapter));

	auto secondary = CrossAdapterBridge::SelectSecondaryAdapter(a_gameAdapter, a_window, upscaling->settings.secondaryGPUName);
	if (!secondary)
		return fallBack("no secondary GPU selected");

	HRESULT hr = D3D12CreateDevice(a_gameAdapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(interopDevice.put()));
	if (FAILED(hr))
		return fallBack("D3D12CreateDevice on the game's GPU failed");

	hr = D3D12CreateDevice(secondary.get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(d3d12Device.put()));
	if (FAILED(hr))
		return fallBack("D3D12CreateDevice on the secondary GPU failed");

	if (!bridge->Initialize(interopDevice.get(), d3d12Device.get(), d3d11Device.get(), d3d11Context.get(),
			upscaling->settings.asyncUnpack, frameSlots, upscaling->settings.directGpuCopy))
		return fallBack("cross-adapter bridge initialisation failed");

	secondaryGPU = true;
	REX::INFO("[FG-GPU1] interop device on \"{}\", frame generation + present on \"{}\"",
		CrossAdapterBridge::AdapterName(a_gameAdapter), CrossAdapterBridge::AdapterName(secondary.get()));
}

void DX12SwapChain::CreateD3D12CommandQueues()
{
	D3D12_COMMAND_QUEUE_DESC queueDesc = {};
	queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
	queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
	queueDesc.NodeMask = 0;

	DX::ThrowIfFailed(d3d12Device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&commandQueue)));

	for (UINT i = 0; i < frameSlots; i++) {
		DX::ThrowIfFailed(d3d12Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&commandAllocators[i])));
		DX::ThrowIfFailed(d3d12Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, commandAllocators[i].get(), nullptr, IID_PPV_ARGS(&commandLists[i])));
		commandLists[i]->Close();
	}
	if (secondaryGPU)
		CrossAdapterBridge::GetSingleton()->SetDisplayQueue(commandQueue.get());
	REX::INFO("[FG] D3D12 command queues created");
}

void DX12SwapChain::TransferEarlyLanes(CrossAdapterBridge::LaneTiming a_timing)
{
	auto upscaling = Upscaling::GetSingleton();
	auto bridge = CrossAdapterBridge::GetSingleton();
	const auto bridgeLock = bridge->LockFrame();  // ReleaseDirectCopyForExit waits for this
	if (!secondaryGPU || !bridge->IsActive() || !upscaling->settings.earlyTransfer)
		return;
	if (!d3d11Context || !d3d11Fence || !d3d12Fence || !bridge->HasLanes(a_timing))
		return;

	// The D3D11 signal orders the copy after the game's work that produced these lanes.
	if (FAILED(d3d11Context->Signal(d3d11Fence.get(), ++fenceValue)))
		return;
	if (FAILED(bridge->TransferGroup(d3d12Fence.get(), fenceValue, frameIndex, a_timing)))
		return;

	// bEarlyUnpack: queue the display GPU's read of these lanes now too, so it starts as soon as
	// they land instead of when Present is called. Present then reads only the finished frame.
	if (upscaling->settings.earlyUnpack && bridge->IsAsyncUnpack())
		bridge->UnpackGroup(frameIndex, a_timing);
}

void DX12SwapChain::CreateSwapChain(IDXGIFactory5* a_dxgiFactory, DXGI_SWAP_CHAIN_DESC a_swapChainDesc)
{
	swapChainDesc = {};
	swapChainDesc.BufferCount = frameSlots;
	swapChainDesc.Width = a_swapChainDesc.BufferDesc.Width;
	swapChainDesc.Height = a_swapChainDesc.BufferDesc.Height;
	swapChainDesc.Format = a_swapChainDesc.BufferDesc.Format;
	swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	swapChainDesc.SampleDesc.Count = 1;

	BOOL allowTearing = FALSE;
	DX::ThrowIfFailed(a_dxgiFactory->CheckFeatureSupport(
		DXGI_FEATURE_PRESENT_ALLOW_TEARING,
		&allowTearing,
		sizeof(allowTearing)
	));

	swapChainDesc.Flags = allowTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;

	auto upscaling = Upscaling::GetSingleton();

	if (upscaling->activeFrameGenType == Upscaling::FrameGenType::kDLSSG) {
		CreateSwapChainDLSSG(a_dxgiFactory, a_swapChainDesc);
	} else if (upscaling->activeFrameGenType == Upscaling::FrameGenType::kXeSSFG) {
		if (!CreateSwapChainXeSS(a_dxgiFactory, a_swapChainDesc)) {
			REX::WARN("[FG] XeSS-FG swap chain failed, falling back to FSR3");
			upscaling->activeFrameGenType = Upscaling::FrameGenType::kFSR3;
			CreateSwapChainFSR3(a_dxgiFactory, a_swapChainDesc);
		}
	} else {
		CreateSwapChainFSR3(a_dxgiFactory, a_swapChainDesc);
	}

	for (UINT slot = 0; slot < frameSlots; ++slot)
		DX::ThrowIfFailed(swapChain->GetBuffer(slot, IID_PPV_ARGS(&swapChainBuffers[slot])));

	frameIndex = swapChain->GetCurrentBackBufferIndex();

	// What this plugin copies into is the back buffers, so their format decides. (GetDesc1 cannot
	// tell: ReShade reports the format the game asked for when an add-on such as AutoHDR changed it.)
	const DXGI_FORMAT bufferFormat = swapChainBuffers[0]->GetDesc().Format;
	if (displayFormat != DXGI_FORMAT_UNKNOWN && displayFormat != bufferFormat)
		REX::CRITICAL("[FG] frame generation was set up for format {}, but the back buffers are format {}; generated frames "
					  "may be wrong",
			static_cast<int>(displayFormat), static_cast<int>(bufferFormat));
	displayFormat = bufferFormat;
	if (displayFormat != swapChainDesc.Format)
		REX::INFO("[FG] back buffers: format {}, the game renders format {}", static_cast<int>(displayFormat),
			static_cast<int>(swapChainDesc.Format));
	// Single-GPU mode: the game's finished frame cannot be copied into back buffers of another format
	// (an HDR add-on changed them); the conversion pass second-GPU mode uses runs on this GPU instead.
	// Its frames are bound in CreateInterop. Without it the plain copy stays, and the picture is wrong.
	if (displayFormat != swapChainDesc.Format && !secondaryGPU && swapChain) {
		const bool converting = SecondaryUpscale::GetSingleton()->InitializeLocalConversion(d3d12Device.get(),
			swapChainDesc.Width, swapChainDesc.Height, displayFormat, swapChainDesc.Format, frameSlots);
		if (converting && upscaling->activeFrameGenType != Upscaling::FrameGenType::kFSR3)
			REX::WARN("[FG] {} frame generation is not told that its UI-free frame stays format {} under the format {} "
					  "back buffers; generated frames may be wrong. Use iFrameGenType = 0 (FSR3) with HDR add-ons",
				upscaling->activeFrameGenType == Upscaling::FrameGenType::kDLSSG ? "DLSS-G" : "XeSS",
				static_cast<int>(swapChainDesc.Format), static_cast<int>(displayFormat));
	}

	// FG-GPU1: FSR 4.1 upscaling on this GPU when the upscaler is set to it, and/or the conversion
	// into back buffers whose format an HDR add-on changed. Decided here, before the lanes exist,
	// because they are sized for it.
	if (upscaling->activeFrameGenType == Upscaling::FrameGenType::kFSR3 && secondaryGPU && swapChain &&
		CrossAdapterBridge::GetSingleton()->IsActive())
		SecondaryUpscale::GetSingleton()->Initialize(d3d12Device.get(), commandQueue.get(), swapChainDesc.Width,
			swapChainDesc.Height, displayFormat);

	swapChainProxy = new DXGISwapChainProxy(swapChain);
}

void DX12SwapChain::CreateSwapChainFSR3(IDXGIFactory5* a_dxgiFactory, DXGI_SWAP_CHAIN_DESC a_swapChainDesc)
{
	auto fidelityFX = FidelityFX::GetSingleton();
	REX::INFO("[FG] Creating FSR3 swap chain via FFX ({})", fidelityFX->RuntimeName());

	// Swap chain context + frame generation context; with iFSRRuntime = 1 this logs the providers
	// offered/selected and falls back to SDK 1.1.4 if the 2.4 runtime cannot create them.
	fidelityFX->CreateSwapChain(a_dxgiFactory, &swapChainDesc, a_swapChainDesc.OutputWindow, commandQueue.get(), &swapChain);
	REX::INFO("[FG] FSR3 swap chain created: {}x{} ({})", swapChainDesc.Width, swapChainDesc.Height, fidelityFX->RuntimeName());
	// The format frame generation was created for; CreateSwapChain checks it against the buffers.
	if (fidelityFX->displayFormat != DXGI_FORMAT_UNKNOWN)
		displayFormat = fidelityFX->displayFormat;
}

void DX12SwapChain::CreateSwapChainDLSSG(IDXGIFactory5* a_dxgiFactory, DXGI_SWAP_CHAIN_DESC a_swapChainDesc)
{
	REX::INFO("[DLSSG] Creating standard D3D12 swap chain for Streamline interception");

	winrt::com_ptr<IDXGISwapChain1> swapChain1;
	DX::ThrowIfFailed(a_dxgiFactory->CreateSwapChainForHwnd(
		commandQueue.get(),
		a_swapChainDesc.OutputWindow,
		&swapChainDesc,
		nullptr,
		nullptr,
		swapChain1.put()
	));

	DX::ThrowIfFailed(swapChain1->QueryInterface(IID_PPV_ARGS(&swapChain)));

	REX::INFO("[DLSSG] D3D12 swap chain created: {}x{}", swapChainDesc.Width, swapChainDesc.Height);
}

bool DX12SwapChain::CreateSwapChainXeSS(IDXGIFactory5* a_dxgiFactory, DXGI_SWAP_CHAIN_DESC a_swapChainDesc)
{
	REX::INFO("[XeSS-FG] Creating XeSS-FG proxy swap chain");

	auto xess = XeSSFG::GetSingleton();
	if (!xess->InitSwapChain(commandQueue.get(), a_dxgiFactory, swapChainDesc, a_swapChainDesc.OutputWindow, &swapChain)) {
		return false;
	}

	REX::INFO("[XeSS-FG] Swap chain created: {}x{}", swapChainDesc.Width, swapChainDesc.Height);
	return true;
}

void DX12SwapChain::CreateInterop()
{
	// The D3D11 producer fence and every D3D11 shared texture live on the game's adapter,
	// so they are opened on the interop device (== d3d12Device in single-GPU mode).
	auto interop = GetInteropDevice();

	HANDLE sharedFenceHandle;
	DX::ThrowIfFailed(interop->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&d3d12Fence)));
	DX::ThrowIfFailed(interop->CreateSharedHandle(d3d12Fence.get(), nullptr, GENERIC_ALL, nullptr, &sharedFenceHandle));
	DX::ThrowIfFailed(d3d11Device->OpenSharedFence(sharedFenceHandle, IID_PPV_ARGS(&d3d11Fence)));
	CloseHandle(sharedFenceHandle);

	D3D11_TEXTURE2D_DESC texDesc11{};
	texDesc11.Width = swapChainDesc.Width;
	texDesc11.Height = swapChainDesc.Height;
	texDesc11.MipLevels = 1;
	texDesc11.ArraySize = 1;
	texDesc11.Format = swapChainDesc.Format;
	texDesc11.SampleDesc.Count = 1;
	texDesc11.SampleDesc.Quality = 0;
	texDesc11.Usage = D3D11_USAGE_DEFAULT;
	texDesc11.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
	texDesc11.CPUAccessFlags = 0;
	texDesc11.MiscFlags = 0;

	// Create interop textures
	if (enbLoaded) {
		swapChainBufferProxyENB = new WrappedResource(texDesc11, d3d11Device.get(), interop);
	} else {
		swapChainBufferProxy = new Texture2D(texDesc11);

		// Create SRV for the proxy backbuffer — needed by GenerateUIBuffer compute shader
		D3D11_SHADER_RESOURCE_VIEW_DESC proxySrvDesc{};
		proxySrvDesc.Format = texDesc11.Format;
		proxySrvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		proxySrvDesc.Texture2D.MostDetailedMip = 0;
		proxySrvDesc.Texture2D.MipLevels = 1;
		swapChainBufferProxy->CreateSRV(proxySrvDesc);
	}

	for (UINT slot = 0; slot < frameSlots; ++slot)
		swapChainBufferWrapped[slot] = new WrappedResource(texDesc11, d3d11Device.get(), interop);

	if (secondaryGPU) {
		// The finished frame crosses to the secondary GPU and is unpacked straight into its back buffer.
		// With FSR 4.1 upscaling there it goes to a mirror instead: the UI is composited from it. So
		// it does when the back buffer has another format (HDR add-on): a shader converts it.
		auto secondaryUpscale = SecondaryUpscale::GetSingleton();
		std::array<ID3D12Resource*, CrossAdapterBridge::kSlots> sources{};
		for (UINT slot = 0; slot < frameSlots; ++slot)
			sources[slot] = swapChainBufferWrapped[slot]->resource.get();
		finalFrameLane = CrossAdapterBridge::GetSingleton()->AddLane("final frame", sources, secondaryUpscale->OwnsPresent());
		if (finalFrameLane < 0)
			REX::CRITICAL("[FG-GPU1] the finished frame cannot be transported; the screen will stay black. Set bFrameGenOnSecondaryGPU=0");
		else if (secondaryUpscale->OwnsPresent())
			secondaryUpscale->SetFinalLane(finalFrameLane);
	} else if (SecondaryUpscale::GetSingleton()->Converting()) {
		// Single GPU, back buffers in another format (HDR add-on): Present converts the finished frame
		// from these into the back buffer instead of copying it.
		std::array<ID3D12Resource*, SecondaryUpscale::kSlots> frames{};
		for (UINT slot = 0; slot < frameSlots; ++slot)
			frames[slot] = swapChainBufferWrapped[slot]->resource.get();
		SecondaryUpscale::GetSingleton()->SetLocalFrames(frames);
	}
}

ID3D12Resource* DX12SwapChain::HudlessStandInFor(UINT a_slot)
{
	if (a_slot >= frameSlots || !d3d12Device || !swapChainBuffers[a_slot])
		return nullptr;

	if (!hudlessStandIn[a_slot]) {
		D3D12_RESOURCE_DESC desc = swapChainBuffers[a_slot]->GetDesc();
		// A plain texture: only ever a copy destination and a shader-read source.
		desc.Flags = D3D12_RESOURCE_FLAG_NONE;
		desc.Alignment = 0;
		desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

		const CD3DX12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE_DEFAULT);
		const HRESULT hr = d3d12Device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
			D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(hudlessStandIn[a_slot].put()));
		if (FAILED(hr)) {
			REX::CRITICAL("[FG-GPU1] bSendHudlessFrame=0: the stand-in colour texture could not be created (hr={:#010x})",
				static_cast<std::uint32_t>(hr));
			return nullptr;
		}
		REX::INFO("[FG-GPU1] bSendHudlessFrame=0: slot {} gets a {}x{} stand-in copy of the composed frame for FFX",
			a_slot, desc.Width, desc.Height);
	}
	return hudlessStandIn[a_slot].get();
}

DXGISwapChainProxy* DX12SwapChain::GetSwapChainProxy()
{
	return swapChainProxy;
}

void DX12SwapChain::SetD3D11Device(ID3D11Device* a_d3d11Device)
{
	DX::ThrowIfFailed(a_d3d11Device->QueryInterface(IID_PPV_ARGS(&d3d11Device)));
}

void DX12SwapChain::SetD3D11DeviceContext(ID3D11DeviceContext* a_d3d11Context)
{
	DX::ThrowIfFailed(a_d3d11Context->QueryInterface(IID_PPV_ARGS(&d3d11Context)));
}

HRESULT DX12SwapChain::GetBuffer(void** ppSurface)
{
	if (enbLoaded)
		*ppSurface = swapChainBufferProxyENB->resource11;
	else
		*ppSurface = swapChainBufferProxy->resource.get();
	return S_OK;
}

HRESULT DX12SwapChain::Present(UINT SyncInterval, UINT Flags)
{
	// The whole frame's use of the bridge: ReleaseDirectCopyForExit (process exit, any thread) waits for it.
	const auto bridgeLock = CrossAdapterBridge::GetSingleton()->LockFrame();
	auto upscalingForUI = Upscaling::GetSingleton();

	// UI tiles, measurement only (plans/UI-TILES.md part 2 step 1): on frames upscaled on the display
	// GPU, count the tiles where the UI changed the stretched scene. Before the frame-end timestamp,
	// so the 3090's frame time includes the pass.
	if (secondaryGPU && !enbLoaded && swapChainBufferProxy) {
		UITiles::GetSingleton()->Frame(d3d11Device.get(), d3d11Context.get(), upscalingForUI->frameUpscaled[frameIndex],
			swapChainBufferProxy->srv.get(), upscalingForUI->sceneBlitTarget ? upscalingForUI->sceneBlitTarget->resource.get() : nullptr,
			swapChainDesc.Width, swapChainDesc.Height);
	}

	// The game's work for this frame is all recorded: timestamp its end on the render GPU.
	renderGpuTimer.Collect(d3d11Context.get());
	renderGpuTimer.FrameEnd(d3d11Device.get(), d3d11Context.get());

	// For DLSS-G: generate UI extraction buffer, then present the HUDLess (clean scene)
	// to the swap chain instead of the full frame. DLSS-G warps whatever is in the swap chain,
	// so presenting HUDLess prevents UI from being warped. UIColorAndAlpha tells DLSS-G
	// to composite UI on top of both real and interpolated frames.
	if (upscalingForUI->activeFrameGenType == Upscaling::FrameGenType::kDLSSG &&
		upscalingForUI->setupBuffers && upscalingForUI->imagespaceComplete)
	{
		upscalingForUI->GenerateUIBuffer();

		// Copy HUDLess (clean scene, no UI) to the D3D12 swap chain — NOT the proxy backbuffer
		d3d11Context->CopyResource(swapChainBufferWrapped[frameIndex]->resource11,
			upscalingForUI->HUDLessBufferShared[frameIndex]->resource.get());

		static bool loggedOnce = false;
		if (!loggedOnce) {
			REX::INFO("[FG] DLSS-G: presenting HUDLess to swap chain, UI composited via D3D12 compositor");
			loggedOnce = true;
		}
	}
	else
	{
		// FSR3 / XeSS / fallback: copy proxy backbuffer (full frame with UI) as before
		if (enbLoaded)
			d3d11Context->CopyResource(swapChainBufferWrapped[frameIndex]->resource11, swapChainBufferProxyENB->resource11);
		else
			d3d11Context->CopyResource(swapChainBufferWrapped[frameIndex]->resource11, swapChainBufferProxy->resource.get());
	}

	auto bridge = CrossAdapterBridge::GetSingleton();
	const bool crossAdapter = secondaryGPU && bridge->IsActive();
	if (secondaryGPU && !crossAdapter) {
		static bool loggedOnce = false;
		if (!loggedOnce) {
			REX::CRITICAL("[FG-GPU1] transport is down (see earlier errors); nothing new reaches the screen");
			loggedOnce = true;
		}
	}

	// Wait for D3D11 to finish
	DX::ThrowIfFailed(d3d11Context->Signal(d3d11Fence.get(), ++fenceValue));
	if (crossAdapter) {
		// Render-GPU copy queue waits for D3D11 and copies every lane into the cross-adapter
		// heap, lane by lane. Then either our copy queue unpacks each lane as it lands and
		// the direct queue waits for that (async), or the direct queue waits for the whole
		// transfer and unpacks itself below (serialised).
		if (SUCCEEDED(bridge->Transfer(d3d12Fence.get(), fenceValue, frameIndex))) {
			if (bridge->IsAsyncUnpack())
				bridge->Unpack(frameIndex, finalFrameLane, swapChainBuffers[frameIndex].get(), commandQueue.get());
			else
				bridge->WaitTransfer(commandQueue.get());
		}
	} else if (!secondaryGPU) {
		DX::ThrowIfFailed(commandQueue->Wait(d3d12Fence.get(), fenceValue));
	}

	// New frame, reset
	DX::ThrowIfFailed(commandAllocators[frameIndex]->Reset());
	DX::ThrowIfFailed(commandLists[frameIndex]->Reset(commandAllocators[frameIndex].get(), nullptr));

	if (crossAdapter) {
		bridge->MarkDisplayList(commandLists[frameIndex].get(), frameIndex, CrossAdapterBridge::kMarkListBegin);
		if (!bridge->IsAsyncUnpack()) {
			// HUD-less colour, depth and motion vectors into their mirrors on this GPU,
			// the finished frame into the back buffer.
			bridge->RecordUnpack(commandLists[frameIndex].get(), frameIndex);
			// A mirrored finished frame (upscaling, or a converted swap-chain format) was read by
			// RecordUnpack; RecordPresent below puts it into the back buffer.
			if (!bridge->Mirror(finalFrameLane, frameIndex))
				bridge->RecordUnpackTo(commandLists[frameIndex].get(), finalFrameLane, frameIndex,
					swapChainBuffers[frameIndex].get(), D3D12_RESOURCE_STATE_PRESENT);
			// iHalfResDepthMV = 1: full-resolution depth for FSR3 from the half-resolution mirror.
			bridge->RecordDepthUpsample(commandLists[frameIndex].get(), frameIndex);
		}
		bridge->MarkDisplayList(commandLists[frameIndex].get(), frameIndex, CrossAdapterBridge::kMarkAfterUnpack);
	}

	// FG-GPU1 FSR 4.1 upscaling on this GPU: the finished frame was unpacked into a mirror. Put its
	// UI over the FSR 4.1 result, or show it as rendered (menus, loading), in the back buffer; with a
	// swap chain in another format (HDR add-on) both are converted on the way.
	auto secondaryUpscale = SecondaryUpscale::GetSingleton();
	const bool presentPass = crossAdapter && secondaryUpscale->HasFinalMirror();
	const bool upscaleLanes = presentPass && secondaryUpscale->Planned();
	bool upscaledFrame = false;
	if (presentPass) {
		upscaledFrame = upscaleLanes && upscalingForUI->frameUpscaled[frameIndex] && secondaryUpscale->FinishSubmit(frameIndex);
		secondaryUpscale->RecordPresent(commandLists[frameIndex].get(), frameIndex, upscaledFrame,
			swapChainBuffers[frameIndex].get());
	}

	// Transfer frame to swap chain buffer (single-GPU path). Back buffers in another format than the
	// game's (an HDR add-on such as ReShade's AutoHDR changed them) get it through the conversion
	// pass; the content stays SDR, frame generation reads its RGBA8 HUD-less copy as before.
	if (!secondaryGPU && secondaryUpscale->ConvertsLocally()) {
		secondaryUpscale->RecordLocalConversion(commandLists[frameIndex].get(), frameIndex, swapChainBuffers[frameIndex].get());
	} else if (!secondaryGPU) {
		auto srcResource = swapChainBufferWrapped[frameIndex]->resource.get();
		auto dstResource = swapChainBuffers[frameIndex].get();

		D3D12_RESOURCE_BARRIER barriers[2] = {
			CD3DX12_RESOURCE_BARRIER::Transition(srcResource, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE),
			CD3DX12_RESOURCE_BARRIER::Transition(dstResource, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST)
		};
		commandLists[frameIndex]->ResourceBarrier(2, barriers);

		commandLists[frameIndex]->CopyResource(dstResource, srcResource);

		D3D12_RESOURCE_BARRIER postBarriers[2] = {
			CD3DX12_RESOURCE_BARRIER::Transition(srcResource, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON),
			CD3DX12_RESOURCE_BARRIER::Transition(dstResource, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT)
		};
		commandLists[frameIndex]->ResourceBarrier(2, postBarriers);
	}

	auto upscaling = Upscaling::GetSingleton();
	upscaling->ReloadSettingsIfNeeded();

	bool useFrameGenerationThisFrame = false;
	bool isDLSSGFrame = false;
	bool isXeSSFrame = false;
	static uint32_t xessFrameId = 0;

	if (auto main = RE::Main::GetSingleton()) {
		if (auto ui = RE::UI::GetSingleton()) {
			bool menuBlock = upscaling->settings.disableInMenus && main->inMenuMode;
			useFrameGenerationThisFrame = upscaling->settings.frameGenerationMode && main->gameActive && !menuBlock && !ui->movementToDirectionalCount;
		}
	}
	// bDirectGPUCopy's guard tripped: the frames cross through system memory again, and frame generation
	// stays off for the rest of the session so the change is impossible to miss (the log says why).
	if (crossAdapter && bridge->DirectCopyTripped())
		useFrameGenerationThisFrame = false;
	// FSR SDK 1.1.4 under a back buffer an HDR add-on changed cannot be told the UI-free frame's
	// format (FidelityFX::CreateContexts logged it): no frame generation, frames shown as rendered.
	if (upscaling->activeFrameGenType == Upscaling::FrameGenType::kFSR3 && !FidelityFX::GetSingleton()->hudlessFormatKnown)
		useFrameGenerationThisFrame = false;

	if (upscaling->activeFrameGenType == Upscaling::FrameGenType::kDLSSG) {
		auto dlssg = StreamlineFG::GetSingleton();

		// Toggle DLSS-G on/off only on state changes (matches XeSS pattern)
		static bool dlssgWasEnabled = false;
		if (useFrameGenerationThisFrame != dlssgWasEnabled) {
			dlssg->SetEnabled(useFrameGenerationThisFrame);
			dlssgWasEnabled = useFrameGenerationThisFrame;
		}

		// Per-frame sequence matching NVIDIA sample ordering:
		// 1. Frame token → 2. Sleep → 3. Sim markers → 4. Constants+tags → 5. Render markers → 6. Present markers
		dlssg->AcquireFrameToken();

		if (dlssg->slReflexSleep && dlssg->frameToken)
			dlssg->slReflexSleep(*dlssg->frameToken);

		dlssg->SetPCLMarker(sl::PCLMarker::eSimulationStart);
		dlssg->SetPCLMarker(sl::PCLMarker::eSimulationEnd);

		// Set constants and tag resources
		static auto gameViewport = State_GetSingleton();
		static auto renderTargetManager = RenderTargetManager_GetSingleton();

		auto screenSize = float2(float(gameViewport->screenWidth), float(gameViewport->screenHeight));

		float2 jitter;
		jitter.x = -gameViewport->offsetX * screenSize.x / 2.0f;
		jitter.y = gameViewport->offsetY * screenSize.y / 2.0f;

		float cameraNear = *(float*)REL::ID({ 57985, 2712882, 2712882 }).address();
		float cameraFar = *(float*)REL::ID({ 958877, 2712883, 2712883 }).address();

		auto& camView = gameViewport->cameraState.camViewData;
		auto& camState = gameViewport->cameraState;

		StreamlineFG::CameraData camera;
		camera.viewMat = camView.viewMat;
		camera.viewProjUnjittered = camView.viewProjUnjittered;
		camera.currentViewProjUnjittered = camView.currentViewProjUnjittered;
		camera.previousViewProjUnjittered = camView.previousViewProjUnjittered;
		camera.viewUp = &camView.viewUp;
		camera.viewRight = &camView.viewRight;
		camera.viewDir = &camView.viewDir;
		camera.posX = camState.posAdjust.x;
		camera.posY = camState.posAdjust.y;
		camera.posZ = camState.posAdjust.z;

		dlssg->Present(
			commandLists[frameIndex].get(),
			upscaling->depthBufferShared12[frameIndex].get(),
			upscaling->motionVectorBufferShared12[frameIndex].get(),
			upscaling->HUDLessBufferShared12[frameIndex].get(),
			upscaling->UIColorAlphaShared12[frameIndex].get(),
			screenSize, jitter,
			cameraNear, cameraFar, camera);

		isDLSSGFrame = true;
	} else if (upscaling->activeFrameGenType == Upscaling::FrameGenType::kXeSSFG) {
		auto xess = XeSSFG::GetSingleton();
		if (xess->initialized) {
			// Toggle enable only on state changes
			static bool xessFGWasEnabled = false;
			if (useFrameGenerationThisFrame != xessFGWasEnabled) {
				xess->SetEnabled(useFrameGenerationThisFrame ? 1 : 0);
				xessFGWasEnabled = useFrameGenerationThisFrame;
			}

			xess->BeginFrame(xessFrameId);
			xess->SetMarker(XELL_SIMULATION_END, xessFrameId);

			isXeSSFrame = true;
			xessFrameId++;
		}
	} else {
		FidelityFX::FrameOverride frameOverride{};
		const FidelityFX::FrameOverride* frameOverridePtr = nullptr;
		if (upscaleLanes) {
			// The depth and motion-vector lanes hold the render-resolution scene, so only upscaled
			// frames can be interpolated; their UI-free frame is the FSR 4.1 result.
			useFrameGenerationThisFrame = useFrameGenerationThisFrame && upscaledFrame;
			if (upscaledFrame) {
				frameOverride.hudless = secondaryUpscale->Output(frameIndex);
				frameOverride.renderWidth = secondaryUpscale->RenderWidth();
				frameOverride.renderHeight = secondaryUpscale->RenderHeight();
				frameOverride.jitterX = upscaling->externalFrame.jitterX;
				frameOverride.jitterY = upscaling->externalFrame.jitterY;
				frameOverridePtr = &frameOverride;
			}
		}
		FidelityFX::GetSingleton()->Present(useFrameGenerationThisFrame, frameOverridePtr);
	}

	if (crossAdapter)
		bridge->MarkDisplayList(commandLists[frameIndex].get(), frameIndex, CrossAdapterBridge::kMarkListEnd);

	DX::ThrowIfFailed(commandLists[frameIndex]->Close());

	// Bracket GPU work submission with render markers
	if (isDLSSGFrame) StreamlineFG::GetSingleton()->SetPCLMarker(sl::PCLMarker::eRenderSubmitStart);
	if (isXeSSFrame) XeSSFG::GetSingleton()->SetMarker(XELL_RENDERSUBMIT_START, xessFrameId - 1);

	ID3D12CommandList* commandListsToExecute[] = { commandLists[frameIndex].get() };
	commandQueue->ExecuteCommandLists(1, commandListsToExecute);

	if (crossAdapter) {
		// Serialised: the staging area of this slot has been unpacked by this list; the
		// render GPU may overwrite it. (Async: the unpack queue already signalled that.)
		if (!bridge->IsAsyncUnpack())
			bridge->SignalConsumed(commandQueue.get());
		bridge->SignalDisplayListDone(commandQueue.get());
	}
	if (presentPass) {
		secondaryUpscale->EndFrame(frameIndex, upscaledFrame);
		secondaryUpscale->LogStats();
	}
	// The upscaler reports the next frame before it renders it.
	upscaling->frameUpscaled[frameIndex] = false;
	upscaling->externalFrame.valid = false;
	upscaling->externalFrame.captured = false;

	// XeSS-FG: tag resources AFTER main command list execute (matching sample flow)
	// Uses the other command list slot for tagging, submitted before Present
	if (isXeSSFrame && useFrameGenerationThisFrame) {
		auto xess = XeSSFG::GetSingleton();
		int tagListIdx = (frameIndex + 1) % 2;

		// NOTE: Resetting commandAllocators[tagListIdx] here without an explicit GPU fence wait.
		// This is safe in practice because the tag list from the previous frame was submitted
		// before Present, and the frame latency waitable object + Present sync guarantee the GPU
		// has retired it by the time we reach this point in the next frame.
		DX::ThrowIfFailed(commandAllocators[tagListIdx]->Reset());
		DX::ThrowIfFailed(commandLists[tagListIdx]->Reset(commandAllocators[tagListIdx].get(), nullptr));

		static auto gameViewport = State_GetSingleton();
		auto screenSize = float2(float(gameViewport->screenWidth), float(gameViewport->screenHeight));

		float2 jitterNorm;
		jitterNorm.x = -gameViewport->offsetX / 2.0f;
		jitterNorm.y = gameViewport->offsetY / 2.0f;

		// Frame time delta
		static LARGE_INTEGER frequency = []() {
			LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f;
		}();
		static LARGE_INTEGER lastXeSSFrame = []() {
			LARGE_INTEGER t; QueryPerformanceCounter(&t); return t;
		}();
		LARGE_INTEGER currentTime;
		QueryPerformanceCounter(&currentTime);
		float deltaMs = static_cast<float>(currentTime.QuadPart - lastXeSSFrame.QuadPart) /
			static_cast<float>(frequency.QuadPart) * 1000.0f;
		lastXeSSFrame = currentTime;

		// Extract row-major view and projection matrices
		auto& camView = gameViewport->cameraState.camViewData;
		alignas(16) float viewMat[16];
		alignas(16) float projMat[16];
		for (int i = 0; i < 4; i++)
			_mm_store_ps(&viewMat[i * 4], camView.viewMat[i]);

		DirectX::XMMATRIX view = DirectX::XMLoadFloat4x4((DirectX::XMFLOAT4X4*)camView.viewMat);
		DirectX::XMMATRIX vpUnjittered = DirectX::XMLoadFloat4x4((DirectX::XMFLOAT4X4*)camView.viewProjUnjittered);
		DirectX::XMMATRIX invView = DirectX::XMMatrixInverse(nullptr, view);
		DirectX::XMMATRIX proj = DirectX::XMMatrixMultiply(invView, vpUnjittered);
		DirectX::XMStoreFloat4x4((DirectX::XMFLOAT4X4*)projMat, proj);

		xess->TagResources(xessFrameId - 1,
			commandLists[tagListIdx].get(),
			upscaling->depthBufferShared12[frameIndex].get(),
			upscaling->motionVectorBufferShared12[frameIndex].get(),
			upscaling->HUDLessBufferShared12[frameIndex].get(),
			screenSize, jitterNorm,
			deltaMs, viewMat, projMat, false);

		DX::ThrowIfFailed(commandLists[tagListIdx]->Close());

		ID3D12CommandList* tagCmdLists[] = { commandLists[tagListIdx].get() };
		commandQueue->ExecuteCommandLists(1, tagCmdLists);
	}

	if (isDLSSGFrame) StreamlineFG::GetSingleton()->SetPCLMarker(sl::PCLMarker::eRenderSubmitEnd);
	if (isXeSSFrame) XeSSFG::GetSingleton()->SetMarker(XELL_RENDERSUBMIT_END, xessFrameId - 1);

	// Fix FPS cap being e.g. 55 instead of 60
	if (!upscaling->highFPSPhysicsFixLoaded && SyncInterval > 0)
		SyncInterval = 1;

	// FSR SDK 2.4: the 4.0.0 swap chain's driver-assisted frame pacing switches itself off while
	// VSync is on, and nothing else reports that.
	if (SyncInterval > 0 && upscaling->activeFrameGenType == Upscaling::FrameGenType::kFSR3 &&
		FidelityFX::GetSingleton()->runtime == FidelityFX::Runtime::kSDK24) {
		static bool loggedVSync = false;
		if (!loggedVSync) {
			REX::INFO("[FSR] VSync is on (SyncInterval {}): driver-assisted frame pacing stays off. Set iPresentInterval=0 "
					  "in Fallout4Prefs.ini to allow it", SyncInterval);
			loggedVSync = true;
		}
	}

	// Latch UI frame index for the compositor before Streamline's Present
	// The real Present hook needs to know which UIColorAlpha buffer was last written
	if (isDLSSGFrame) UICompositor::GetSingleton()->SetUIFrameIndex(frameIndex);

	// Bracket Present with markers
	if (isDLSSGFrame) StreamlineFG::GetSingleton()->SetPCLMarker(sl::PCLMarker::ePresentStart);
	if (isXeSSFrame) XeSSFG::GetSingleton()->SetMarker(XELL_PRESENT_START, xessFrameId - 1);

	// The FFX swap chain blocks here until the frame before this one has been presented
	// (two replacement back buffers). Timed, because while the render thread waits here
	// the 3090 gets no new work.
	LARGE_INTEGER presentStart{};
	QueryPerformanceCounter(&presentStart);
	DX::ThrowIfFailed(swapChain->Present(SyncInterval, Flags));
	LARGE_INTEGER presentEnd{};
	QueryPerformanceCounter(&presentEnd);

	// Diagnostics: the swap chain counts every Present, including the frames FSR3 inserts,
	// so this is the only place that shows whether frame generation actually doubles.
	{
		UINT presentCount = 0;
		if (SUCCEEDED(swapChain->GetLastPresentCount(&presentCount))) {
			if (lastPresentCount && presentCount > lastPresentCount)
				statPresentedFrames += presentCount - lastPresentCount;
			lastPresentCount = presentCount;
		}
		++statGameFrames;

		static LARGE_INTEGER frequency = []() {
			LARGE_INTEGER f;
			QueryPerformanceFrequency(&f);
			return f;
		}();

		LARGE_INTEGER now{};
		QueryPerformanceCounter(&now);
		if (!statPresentStart.QuadPart)
			statPresentStart = now;

		const double elapsedMs = static_cast<double>(now.QuadPart - statPresentStart.QuadPart) * 1000.0 /
		                         static_cast<double>(frequency.QuadPart);
		if (elapsedMs >= 10000.0) {
			const double gameFps = static_cast<double>(statGameFrames) * 1000.0 / elapsedMs;
			const double presentedFps = static_cast<double>(statPresentedFrames) * 1000.0 / elapsedMs;
			REX::INFO("[FG-GPU1] presented {:.1f} fps | game {:.1f} fps | generated {:.0f}%", presentedFps, gameFps,
				presentedFps > 0.0 ? (presentedFps - gameFps) / presentedFps * 100.0 : 0.0);
			if (const auto gpu = renderGpuTimer.TakeSummary(); !gpu.empty())
				REX::INFO("[FG-GPU1] render GPU: {}", gpu);
			statGameFrames = 0;
			statPresentedFrames = 0;
			statPresentStart = now;
		}
	}

	if (isDLSSGFrame) StreamlineFG::GetSingleton()->SetPCLMarker(sl::PCLMarker::ePresentEnd);
	if (isXeSSFrame) XeSSFG::GetSingleton()->SetMarker(XELL_PRESENT_END, xessFrameId - 1);

	// Wait for previous frame to have finished (handle may be null if swap chain
	// was created without DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT —
	// FSR3/DLSS-G wrapped swap chains manage latency internally)
	auto frameLatencyWaitableObject = swapChain->GetFrameLatencyWaitableObject();
	if (frameLatencyWaitableObject) {
		WaitForSingleObjectEx(frameLatencyWaitableObject, INFINITE, TRUE);
		// FSR SDK 2.x swap chains return a fresh duplicate on every call, which the caller must
		// close (null here unless the swap chain is created with the waitable-object flag).
		if (upscaling->activeFrameGenType == Upscaling::FrameGenType::kFSR3 &&
			FidelityFX::GetSingleton()->runtime == FidelityFX::Runtime::kSDK24)
			CloseHandle(frameLatencyWaitableObject);
	}

	if (crossAdapter) {
		LARGE_INTEGER waitEnd{};
		QueryPerformanceCounter(&waitEnd);
		static const double qpcToMs = [] {
			LARGE_INTEGER f;
			QueryPerformanceFrequency(&f);
			return 1000.0 / static_cast<double>(f.QuadPart);
		}();
		bridge->AddPresentBlock(static_cast<double>(presentEnd.QuadPart - presentStart.QuadPart) * qpcToMs,
			static_cast<double>(waitEnd.QuadPart - presentEnd.QuadPart) * qpcToMs);
	}

	// Update the frame index
	frameIndex = swapChain->GetCurrentBackBufferIndex();

	// Before the game's D3D11 work writes the next slot (Reset() below clears it): make the
	// D3D11 context wait, on the GPU, for the render-GPU copy that last read that slot.
	if (crossAdapter) {
		bridge->GateD3D11(frameIndex);
		bridge->LogStats();
	}

	// Clear resources
	upscaling->Reset();

	// Fix game running too fast
	if (!upscaling->highFPSPhysicsFixLoaded)
		upscaling->GameFrameLimiter();

	// If VSync is disabled and HighFPSPhysicsFix isn't handling pacing, use our limiter.
	// Skip when HFPF is loaded — it handles pacing correctly including loading screens.
	if (SyncInterval == 0 && !upscaling->highFPSPhysicsFixLoaded)
		upscaling->FrameLimiter(useFrameGenerationThisFrame);

	// Everything the game records from here on is the next frame.
	renderGpuTimer.NextFrameStart(d3d11Context.get());

	return S_OK;
}

HRESULT DX12SwapChain::GetDevice(REFIID uuid, void** ppDevice)
{
	if (uuid == __uuidof(ID3D11Device) || uuid == __uuidof(ID3D11Device1) || uuid == __uuidof(ID3D11Device2) || uuid == __uuidof(ID3D11Device3) || uuid == __uuidof(ID3D11Device4) || uuid == __uuidof(ID3D11Device5)) {
		*ppDevice = d3d11Device.get();
		return S_OK;
	}

	return swapChain->GetDevice(uuid, ppDevice);
}

// ---------------------------------------------------------------------------------------
// RenderGpuTimer

bool RenderGpuTimer::Create(ID3D11Device* a_device)
{
	D3D11_QUERY_DESC disjointDesc{ D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
	D3D11_QUERY_DESC timestampDesc{ D3D11_QUERY_TIMESTAMP, 0 };
	for (auto& record : records) {
		if (FAILED(a_device->CreateQuery(&disjointDesc, record.disjoint.put())) ||
			FAILED(a_device->CreateQuery(&timestampDesc, record.end.put())) ||
			FAILED(a_device->CreateQuery(&timestampDesc, record.start.put())) ||
			FAILED(a_device->CreateQuery(&timestampDesc, record.nextEnd.put()))) {
			REX::INFO("[FG-GPU1] render GPU timestamps unavailable; its idle time will not be reported");
			return false;
		}
	}
	return true;
}

void RenderGpuTimer::FrameEnd(ID3D11Device* a_device, ID3D11DeviceContext* a_context)
{
	if (failed || !a_device || !a_context)
		return;
	if (!created) {
		created = true;
		failed = !Create(a_device);
		if (failed)
			return;
	}

	// Close the record that has been waiting for this frame's end.
	if (current < kRecords) {
		auto& record = records[current];
		if (record.started) {
			a_context->End(record.nextEnd.get());
			a_context->End(record.disjoint.get());
			record.pending = true;
		} else {
			a_context->End(record.disjoint.get());  // never started: discard
		}
		record.open = false;
		record.started = false;
		current = kRecords;
	}

	// Open the next one, unless its previous use has not been read yet (skip this frame).
	auto& record = records[next];
	if (record.pending)
		return;
	a_context->Begin(record.disjoint.get());
	a_context->End(record.end.get());
	record.open = true;
	current = next;
	next = (next + 1) % kRecords;
}

void RenderGpuTimer::NextFrameStart(ID3D11DeviceContext* a_context)
{
	if (failed || current >= kRecords || !a_context)
		return;
	auto& record = records[current];
	if (!record.open || record.started)
		return;
	a_context->End(record.start.get());
	record.started = true;
}

void RenderGpuTimer::Collect(ID3D11DeviceContext* a_context)
{
	if (failed || !a_context)
		return;
	for (auto& record : records) {
		if (!record.pending)
			continue;
		D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
		if (a_context->GetData(record.disjoint.get(), &disjoint, sizeof(disjoint), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
			continue;  // not finished yet
		UINT64 end = 0, start = 0, nextEnd = 0;
		const bool ready = a_context->GetData(record.end.get(), &end, sizeof(end), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
		                   a_context->GetData(record.start.get(), &start, sizeof(start), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
		                   a_context->GetData(record.nextEnd.get(), &nextEnd, sizeof(nextEnd), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK;
		if (!ready)
			continue;
		record.pending = false;
		if (disjoint.Disjoint || !disjoint.Frequency || start < end || nextEnd < start)
			continue;
		const double toMs = 1000.0 / static_cast<double>(disjoint.Frequency);
		const double idle = static_cast<double>(start - end) * toMs;
		const double span = static_cast<double>(nextEnd - start) * toMs;
		// Loading screens and the first frame after them take seconds; they would swamp the averages.
		if (idle > 100.0 || span > 100.0) {
			++skipped;
			continue;
		}
		idleSum += idle;
		idleMax = (std::max)(idleMax, idle);
		spanSum += span;
		spanMax = (std::max)(spanMax, span);
		++samples;
		if (idle >= 0.5)
			++idleFrames;
	}
}

std::string RenderGpuTimer::TakeSummary()
{
	if (!samples)
		return {};
	const double n = static_cast<double>(samples);
	std::string out = std::format("idle between frames avg {:.2f} ms max {:.2f} ms ({:.0f}% of frames idle >= 0.5 ms) | "
								  "frame from its first command to its end avg {:.2f} ms max {:.2f} ms ({} frames measured{})",
		idleSum / n, idleMax, 100.0 * static_cast<double>(idleFrames) / n, spanSum / n, spanMax, samples,
		skipped ? std::format(", {} over 100 ms left out", skipped) : std::string{});
	idleSum = idleMax = spanSum = spanMax = 0.0;
	samples = idleFrames = skipped = 0;
	return out;
}

WrappedResource::WrappedResource(D3D11_TEXTURE2D_DESC a_texDesc, ID3D11Device5* a_d3d11Device, ID3D12Device* a_d3d12Device)
{
	// Create D3D11 shared texture directly instead of wrapping D3D12 resource
	a_texDesc.MiscFlags |= D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
	DX::ThrowIfFailed(a_d3d11Device->CreateTexture2D(&a_texDesc, nullptr, &resource11));

	// Get shared handle from D3D11 texture to enable D3D12 access
	winrt::com_ptr<IDXGIResource1> dxgiResource;
	DX::ThrowIfFailed(resource11->QueryInterface(IID_PPV_ARGS(dxgiResource.put())));
	HANDLE sharedHandle = nullptr;
	DX::ThrowIfFailed(dxgiResource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &sharedHandle));

	// Open the shared D3D11 texture as D3D12 resource
	DX::ThrowIfFailed(a_d3d12Device->OpenSharedHandle(sharedHandle, IID_PPV_ARGS(resource.put())));
	CloseHandle(sharedHandle);

	if (a_texDesc.BindFlags & D3D11_BIND_SHADER_RESOURCE) {
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		srvDesc.Format = a_texDesc.Format;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MostDetailedMip = 0;
		srvDesc.Texture2D.MipLevels = 1;

		DX::ThrowIfFailed(a_d3d11Device->CreateShaderResourceView(resource11, &srvDesc, &srv));
	}

	if (a_texDesc.BindFlags & D3D11_BIND_UNORDERED_ACCESS) {
		if (a_texDesc.ArraySize > 1) {
			D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
			uavDesc.Format = a_texDesc.Format;
			uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
			uavDesc.Texture2DArray.FirstArraySlice = 0;
			uavDesc.Texture2DArray.ArraySize = a_texDesc.ArraySize;

			DX::ThrowIfFailed(a_d3d11Device->CreateUnorderedAccessView(resource11, &uavDesc, &uav));
		}
		else {
			D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
			uavDesc.Format = a_texDesc.Format;
			uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
			uavDesc.Texture2D.MipSlice = 0;

			DX::ThrowIfFailed(a_d3d11Device->CreateUnorderedAccessView(resource11, &uavDesc, &uav));
		}
	}

	if (a_texDesc.BindFlags & D3D11_BIND_RENDER_TARGET) {
		D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = {};
		rtvDesc.Format = a_texDesc.Format;
		rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
		rtvDesc.Texture2D.MipSlice = 0;
		DX::ThrowIfFailed(a_d3d11Device->CreateRenderTargetView(resource11, &rtvDesc, &rtv));
	}
}

DXGISwapChainProxy::DXGISwapChainProxy(IDXGISwapChain4* a_swapChain)
{
	swapChain = a_swapChain;
}

/****IUknown****/
HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::QueryInterface(REFIID riid, void** ppvObj)
{
	auto ret = swapChain->QueryInterface(riid, ppvObj);
	if (*ppvObj)
		*ppvObj = this;
	return ret;
}

ULONG STDMETHODCALLTYPE DXGISwapChainProxy::AddRef()
{
	return swapChain->AddRef();
}

ULONG STDMETHODCALLTYPE DXGISwapChainProxy::Release()
{
	return swapChain->Release();
}

/****IDXGIObject****/
HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::SetPrivateData(_In_ REFGUID Name, UINT DataSize, _In_reads_bytes_(DataSize) const void* pData)
{
	return swapChain->SetPrivateData(Name, DataSize, pData);
}

HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::SetPrivateDataInterface(_In_ REFGUID Name, _In_opt_ const IUnknown* pUnknown)
{
	return swapChain->SetPrivateDataInterface(Name, pUnknown);
}

HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::GetPrivateData(_In_ REFGUID Name, _Inout_ UINT* pDataSize, _Out_writes_bytes_(*pDataSize) void* pData)
{
	return swapChain->GetPrivateData(Name, pDataSize, pData);
}

HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::GetParent(_In_ REFIID riid, _COM_Outptr_ void** ppParent)
{
	return swapChain->GetParent(riid, ppParent);
}

/****IDXGIDeviceSubObject****/
HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::GetDevice(_In_ REFIID riid, _COM_Outptr_ void** ppDevice)
{
	return DX12SwapChain::GetSingleton()->GetDevice(riid, ppDevice);
}

/****IDXGISwapChain****/
HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::Present(UINT SyncInterval, UINT Flags)
{
	return DX12SwapChain::GetSingleton()->Present(SyncInterval, Flags);
}

HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::GetBuffer(UINT, _In_ REFIID, _COM_Outptr_ void** ppSurface)
{
	return DX12SwapChain::GetSingleton()->GetBuffer(ppSurface);
}

HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::SetFullscreenState(BOOL, _In_opt_ IDXGIOutput*)
{
	return S_OK;
}

HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::GetFullscreenState(_Out_opt_ BOOL* pFullscreen, _COM_Outptr_opt_result_maybenull_ IDXGIOutput** ppTarget)
{
	return swapChain->GetFullscreenState(pFullscreen, ppTarget);
}

HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::GetDesc(_Out_ DXGI_SWAP_CHAIN_DESC* pDesc)
{
	const HRESULT hr = swapChain->GetDesc(pDesc);
	// GetBuffer hands out the game-side texture, which keeps the game's format even when an HDR
	// add-on changed the real swap chain's; report that one, so D3D11 code (the game, overlays)
	// creates views that match the buffer it gets.
	if (SUCCEEDED(hr) && pDesc)
		pDesc->BufferDesc.Format = DX12SwapChain::GetSingleton()->swapChainDesc.Format;
	return hr;
}

HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::ResizeBuffers(UINT , UINT , UINT , DXGI_FORMAT , UINT )
{
	return S_OK;
}

HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::ResizeTarget(_In_ const DXGI_MODE_DESC*)
{
	return S_OK;
}

HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::GetContainingOutput(_COM_Outptr_ IDXGIOutput** ppOutput)
{
	return swapChain->GetContainingOutput(ppOutput);
}

HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::GetFrameStatistics(_Out_ DXGI_FRAME_STATISTICS* pStats)
{
	return swapChain->GetFrameStatistics(pStats);
}

HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::GetLastPresentCount(_Out_ UINT* pLastPresentCount)
{
	return swapChain->GetLastPresentCount(pLastPresentCount);
}
