#include "SecondaryUpscale.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <iterator>
#include <string>

#include "CrossAdapterBridge.h"
#include "FidelityFX.h"
#include "MGPUScaling.h"
#include "Upscaling.h"

namespace
{
	constexpr const wchar_t* kShaderPath = MGPU_SHADER(L"SecondaryUpscaleCS.hlsl");

	D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* a_resource, D3D12_RESOURCE_STATES a_before,
		D3D12_RESOURCE_STATES a_after)
	{
		D3D12_RESOURCE_BARRIER barrier{};
		barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barrier.Transition.pResource = a_resource;
		barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		barrier.Transition.StateBefore = a_before;
		barrier.Transition.StateAfter = a_after;
		return barrier;
	}


	// FSR's quality modes, the ratios ffxFsr3GetUpscaleRatioFromQualityMode returns in the upscaler.
	constexpr float kUpscaleRatio[] = { 1.0f, 1.5f, 1.7f, 2.0f, 3.0f };
	constexpr const char* kQualityName[] = { "Native AA", "Quality", "Balanced", "Performance", "Ultra Performance" };
}

const char* SecondaryUpscale::FormatName(DXGI_FORMAT a_format)
{
	switch (a_format) {
	case DXGI_FORMAT_R8G8B8A8_UNORM:
		return "R8G8B8A8_UNORM";
	case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
		return "R8G8B8A8_UNORM_SRGB";
	case DXGI_FORMAT_B8G8R8A8_UNORM:
		return "B8G8R8A8_UNORM";
	case DXGI_FORMAT_R10G10B10A2_UNORM:
		return "R10G10B10A2_UNORM (HDR10)";
	case DXGI_FORMAT_R16G16B16A16_FLOAT:
		return "R16G16B16A16_FLOAT (scRGB)";
	default:
		return "another format";
	}
}

bool SecondaryUpscale::Initialize(ID3D12Device* a_device, ID3D12CommandQueue* a_queue, UINT a_displayWidth,
	UINT a_displayHeight, DXGI_FORMAT a_displayFormat)
{
	if (planned || converting || !a_device || !a_queue)
		return planned;

	displayWidth = a_displayWidth;
	displayHeight = a_displayHeight;
	displayFormat = a_displayFormat;
	device.copy_from(a_device);
	queue = a_queue;

	// The game's frames are RGBA8. Any other swap-chain format (an HDR add-on promoted it while FFX
	// created it) needs a shader pass, because copies do not convert.
	const bool convert = a_displayFormat != DXGI_FORMAT_R8G8B8A8_UNORM;
	if (convert) {
		if (!Storable(a_device, a_displayFormat)) {
			REX::CRITICAL("[FG-GPU1] the swap chain is format {} ({}), which the display GPU cannot write from a shader; "
						  "frames cannot be shown. Disable the add-on that changes it (e.g. ReShade's AutoHDR)",
				static_cast<int>(a_displayFormat), FormatName(a_displayFormat));
			return false;
		}
		REX::INFO("[FG-GPU1] the swap chain is {} (format {}), the game renders R8G8B8A8_UNORM: every frame is converted "
				  "on the display GPU",
			FormatName(a_displayFormat), static_cast<int>(a_displayFormat));
	}

	if (PlanUpscale()) {
		if (FidelityFX::GetSingleton()->CreateUpscaler(a_device, renderWidth, renderHeight, displayWidth, displayHeight,
				Upscaling::GetSingleton()->settings.debugLogging) &&
			CreatePipeline(true)) {
			planned = true;
			converting = convert;
			QueryPerformanceCounter(&statLastLog);
			REX::INFO("[FSR4] upscaling on the display GPU: the game renders {}x{} ({}), FSR {} upscales to {}x{}{}", renderWidth,
				renderHeight, kQualityName[qualityMode], FidelityFX::GetSingleton()->upscalerProvider, displayWidth, displayHeight,
				convert ? std::format(", composited in {}", FormatName(a_displayFormat)) : std::string{});
			return true;
		}
	}

	// Not upscaling: with a non-RGBA8 swap chain the finished frame still needs the conversion pass.
	if (convert) {
		renderWidth = displayWidth;
		renderHeight = displayHeight;
		if (CreatePipeline(false)) {
			converting = true;
			REX::INFO("[FG-GPU1] frames reach the {} back buffer through a conversion pass on the display GPU",
				FormatName(a_displayFormat));
		} else {
			REX::CRITICAL("[FG-GPU1] the conversion pass for the {} swap chain could not be set up; frames cannot be shown. "
						  "Disable the add-on that changes the format (e.g. ReShade's AutoHDR)",
				FormatName(a_displayFormat));
		}
	}
	return false;
}

bool SecondaryUpscale::Storable(ID3D12Device* a_device, DXGI_FORMAT a_format)
{
	D3D12_FEATURE_DATA_FORMAT_SUPPORT support{};
	support.Format = a_format;
	return SUCCEEDED(a_device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))) &&
	       (support.Support1 & D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW) &&
	       (support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE);
}

bool SecondaryUpscale::InitializeLocalConversion(ID3D12Device* a_device, UINT a_width, UINT a_height,
	DXGI_FORMAT a_displayFormat, DXGI_FORMAT a_sourceFormat, UINT a_slots)
{
	if (planned || converting || !a_device)
		return false;

	// Single-GPU mode: no upscaling here, only the pass second-GPU mode uses for frames it does not
	// upscale. The game's frame is read as it is (t1) and stored in the swap chain's format; the
	// copy engine moves the result into the back buffer.
	if (!Storable(a_device, a_displayFormat)) {
		REX::ERROR("[FG] the swap chain is format {} ({}), which this GPU cannot write from a shader; frames are copied "
				   "into it unconverted, so the picture will be wrong. Disable the add-on that changes it (e.g. ReShade's "
				   "AutoHDR)",
			static_cast<int>(a_displayFormat), FormatName(a_displayFormat));
		return false;
	}

	displayWidth = renderWidth = a_width;
	displayHeight = renderHeight = a_height;
	displayFormat = a_displayFormat;
	sourceFormat = a_sourceFormat;
	localSlots = std::clamp(a_slots, 1u, kSlots);
	device.copy_from(a_device);
	if (!CreatePipeline(false)) {
		REX::ERROR("[FG] the conversion pass for the {} swap chain could not be set up; frames are copied into it "
				   "unconverted, so the picture will be wrong. Disable the add-on that changes the format (e.g. ReShade's "
				   "AutoHDR)",
			FormatName(a_displayFormat));
		return false;
	}
	local = true;
	converting = true;
	REX::INFO("[FG] the swap chain is {} (format {}), the game renders {} (format {}): every frame is converted on the "
			  "game's GPU on its way into the back buffer (single-GPU mode)",
		FormatName(a_displayFormat), static_cast<int>(a_displayFormat), FormatName(a_sourceFormat),
		static_cast<int>(a_sourceFormat));
	return true;
}

bool SecondaryUpscale::SetLocalFrames(const std::array<ID3D12Resource*, kSlots>& a_frames)
{
	if (!local || !converting)
		return false;
	const auto refuse = [this](const char* a_reason) {
		REX::ERROR("[FG] {}; frames are copied into the {} swap chain unconverted, so the picture will be wrong", a_reason,
			FormatName(displayFormat));
		return false;
	};
	for (UINT slot = 0; slot < localSlots; ++slot) {
		if (!a_frames[slot])
			return refuse("a finished-frame buffer is missing");
		const auto desc = a_frames[slot]->GetDesc();
		if (desc.Width != displayWidth || desc.Height != displayHeight || desc.Format != sourceFormat)
			return refuse("the finished-frame buffers do not match the swap chain's size or the game's format");
		if (desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE)
			return refuse("the finished-frame buffers cannot be read by a shader");
	}
	localFrames = a_frames;
	if (!WriteCopyDescriptors(localFrames, sourceFormat))
		return refuse("the conversion pass's descriptors could not be written");
	localBound = true;
	return true;
}

void SecondaryUpscale::RecordLocalConversion(ID3D12GraphicsCommandList* a_list, UINT a_slot, ID3D12Resource* a_backBuffer)
{
	if (!ConvertsLocally() || !a_list || !a_backBuffer || a_slot >= localSlots)
		return;
	RecordCopyThroughComposite(a_list, a_slot, localFrames[a_slot], a_backBuffer);
	if (!loggedLocalFrame) {
		REX::INFO("[FG] first frame converted into the {} back buffer on the game's GPU (frame slot {})",
			FormatName(displayFormat), a_slot);
		loggedLocalFrame = true;
	}
}

bool SecondaryUpscale::PlanUpscale()
{
	// The upscaler's own settings decide, read the way it reads them (shipped defaults, then the
	// user's MCM file), so both parts agree on the render size before the game renders a frame.
	CSimpleIniA ini;
	ini.SetUnicode();
	mgpu::LoadSettings(ini);
	const long method = ini.GetLongValue(mgpu::kUpscalingSection, "iUpscaleMethodPreference", 2);
	if (method != 3) {
		REX::INFO("[FSR4] [Upscaling] iUpscaleMethodPreference = {} (3 = FSR 4.1 on the display GPU): not upscaling here",
			method);
		return false;
	}
	qualityMode = std::clamp(static_cast<int>(ini.GetLongValue(mgpu::kUpscalingSection, "iQualityMode", 1)), 0, 4);

	// The upscaler renders the frames this upscales; it is off when another Upscaling.dll is loaded.
	if (!mgpu::UpscalerActive()) {
		REX::ERROR("[FSR4] iUpscaleMethodPreference = 3, but the upscaler is off (another Upscaling.dll is loaded, see the "
				   "[INIT] line); the game renders at full resolution");
		return false;
	}

	auto bridge = CrossAdapterBridge::GetSingleton();
	if (!bridge->IsActive() || !bridge->IsAsyncUnpack()) {
		REX::ERROR("[FSR4] upscaling on the display GPU needs the cross-adapter transport with bAsyncUnpack = 1");
		return false;
	}

	// The arithmetic Upscaling.dll uses for its render targets and jitter.
	const float scale = 1.0f / kUpscaleRatio[qualityMode];
	renderWidth = static_cast<UINT>(static_cast<float>(displayWidth) * scale);
	renderHeight = static_cast<UINT>(static_cast<float>(displayHeight) * scale);
	return true;
}

bool SecondaryUpscale::CreateComputePipeline(const char* a_entry, winrt::com_ptr<ID3D12PipelineState>& a_pipeline)
{
	const std::string renderW = std::to_string(renderWidth);
	const std::string renderH = std::to_string(renderHeight);
	const std::string displayW = std::to_string(displayWidth);
	const std::string displayH = std::to_string(displayHeight);
	const D3D_SHADER_MACRO defines[] = { { "RENDER_WIDTH", renderW.c_str() }, { "RENDER_HEIGHT", renderH.c_str() },
		{ "DISPLAY_WIDTH", displayW.c_str() }, { "DISPLAY_HEIGHT", displayH.c_str() }, { nullptr, nullptr } };

	winrt::com_ptr<ID3DBlob> shader;
	winrt::com_ptr<ID3DBlob> errors;
	HRESULT hr = D3DCompileFromFile(kShaderPath, defines, D3D_COMPILE_STANDARD_FILE_INCLUDE, a_entry, "cs_5_0",
		D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, shader.put(), errors.put());
	if (FAILED(hr)) {
		if (errors)
			REX::ERROR("[FSR4] {}", static_cast<const char*>(errors->GetBufferPointer()));
		REX::ERROR("[FSR4] compiling SecondaryUpscaleCS.hlsl ({}) failed (hr={:#010x})", a_entry, static_cast<std::uint32_t>(hr));
		return false;
	}

	D3D12_COMPUTE_PIPELINE_STATE_DESC pipelineDesc{};
	pipelineDesc.pRootSignature = rootSignature.get();
	pipelineDesc.CS = { shader->GetBufferPointer(), shader->GetBufferSize() };
	hr = device->CreateComputePipelineState(&pipelineDesc, IID_PPV_ARGS(a_pipeline.put()));
	if (FAILED(hr)) {
		REX::ERROR("[FSR4] CreateComputePipelineState ({}) failed (hr={:#010x})", a_entry, static_cast<std::uint32_t>(hr));
		return false;
	}
	return true;
}

bool SecondaryUpscale::CreatePipeline(bool a_upscale)
{
	const auto fail = [a_upscale](const char* a_what, HRESULT a_hr) {
		REX::ERROR("[FSR4] {} failed (hr={:#010x}); {}", a_what, static_cast<std::uint32_t>(a_hr),
			a_upscale ? "upscaling on the display GPU is off" : "the format conversion is off");
		return false;
	};

	HRESULT hr = S_OK;

	// A failed upscaling setup is followed by the conversion-only one: start from nothing.
	rootSignature = nullptr;
	pipeline = nullptr;
	copyPipeline = nullptr;
	descriptors = nullptr;
	composite = nullptr;
	for (auto& output : outputs)
		output = nullptr;

	// One table per slot: t0-t2, u0.
	D3D12_DESCRIPTOR_RANGE ranges[2]{};
	ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	ranges[0].NumDescriptors = 3;
	ranges[0].OffsetInDescriptorsFromTableStart = 0;
	ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
	ranges[1].NumDescriptors = 1;
	ranges[1].OffsetInDescriptorsFromTableStart = 3;

	D3D12_ROOT_PARAMETER parameter{};
	parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	parameter.DescriptorTable.NumDescriptorRanges = 2;
	parameter.DescriptorTable.pDescriptorRanges = ranges;
	parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

	D3D12_ROOT_SIGNATURE_DESC rootDesc{};
	rootDesc.NumParameters = 1;
	rootDesc.pParameters = &parameter;

	winrt::com_ptr<ID3DBlob> serialized;
	winrt::com_ptr<ID3DBlob> rootErrors;
	hr = D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, serialized.put(), rootErrors.put());
	if (FAILED(hr))
		return fail("D3D12SerializeRootSignature", hr);
	hr = device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
		IID_PPV_ARGS(rootSignature.put()));
	if (FAILED(hr))
		return fail("CreateRootSignature", hr);

	if (a_upscale && !CreateComputePipeline("Composite", pipeline))
		return false;
	// Native frames into a back buffer the copy engine cannot convert to. (sourceFormat is the game's
	// RGBA8 except in single-GPU mode, which comes here only when the two formats differ.)
	if (displayFormat != sourceFormat && !CreateComputePipeline("Copy", copyPipeline))
		return false;

	D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
	heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	heapDesc.NumDescriptors = kSlots * kDescriptorsPerSlot;
	heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	hr = device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(descriptors.put()));
	if (FAILED(hr))
		return fail("CreateDescriptorHeap", hr);
	descriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

	// FSR 4.1 outputs (one per slot: frame generation reads a frame's while the next is upscaled)
	// and the composite. COMMON between uses, like the bridge's mirrors. The outputs stay RGBA8
	// like the game's frames (frame generation is told so); the composite has the swap chain's
	// format because it is copied into the back buffer.
	D3D12_RESOURCE_DESC desc{};
	desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	desc.Width = displayWidth;
	desc.Height = displayHeight;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
	D3D12_HEAP_PROPERTIES defaultHeap{};
	defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
	if (a_upscale) {
		for (UINT slot = 0; slot < Slots(); ++slot) {
			auto& output = outputs[slot];
			hr = device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
				IID_PPV_ARGS(output.put()));
			if (FAILED(hr))
				return fail("CreateCommittedResource(FSR 4.1 output)", hr);
			output->SetName(L"FG-GPU1 FSR 4.1 output");
		}
	}
	desc.Format = displayFormat;
	hr = device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
		IID_PPV_ARGS(composite.put()));
	if (FAILED(hr))
		return fail("CreateCommittedResource(composite)", hr);
	composite->SetName(L"FG-GPU1 composite (display format)");

	// The upscale list and timestamps belong to upscaling; the conversion pass records on the
	// frame's own list.
	if (!a_upscale)
		return true;

	for (auto& allocator : allocators) {
		hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(allocator.put()));
		if (FAILED(hr))
			return fail("CreateCommandAllocator", hr);
	}
	hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators[0].get(), nullptr, IID_PPV_ARGS(list.put()));
	if (SUCCEEDED(hr))
		hr = list->Close();
	if (FAILED(hr))
		return fail("CreateCommandList", hr);

	// Timestamps are optional.
	if (SUCCEEDED(queue->GetTimestampFrequency(&timestampFrequency)) && timestampFrequency) {
		D3D12_QUERY_HEAP_DESC queryDesc{};
		queryDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
		queryDesc.Count = kSlots * kMarks;
		D3D12_RESOURCE_DESC bufferDesc{};
		bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		bufferDesc.Width = sizeof(UINT64) * kSlots * kMarks;
		bufferDesc.Height = 1;
		bufferDesc.DepthOrArraySize = 1;
		bufferDesc.MipLevels = 1;
		bufferDesc.SampleDesc.Count = 1;
		bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		D3D12_HEAP_PROPERTIES readbackHeap{};
		readbackHeap.Type = D3D12_HEAP_TYPE_READBACK;
		if (FAILED(device->CreateQueryHeap(&queryDesc, IID_PPV_ARGS(queryHeap.put()))) ||
			FAILED(device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &bufferDesc,
				D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(readback.put())))) {
			queryHeap = nullptr;
			readback = nullptr;
			REX::INFO("[FSR4] timestamps unavailable; upscale time will not be reported");
		}
	}
	return true;
}

bool SecondaryUpscale::ReadyFor(uint32_t a_width, uint32_t a_height)
{
	// Lanes are bound at the first frame hook, which comes after this question for that frame.
	if (!planned || failed || !CrossAdapterBridge::GetSingleton()->IsActive())
		return false;
	if (a_width == renderWidth && a_height == renderHeight)
		return true;
	if (!loggedSizeMismatch) {
		REX::WARN("[FSR4] Upscaling.dll now renders {}x{}, but this session's buffers and FSR 4.1 context are {}x{} "
				  "(iQualityMode changed?); full resolution until the game is restarted",
			a_width, a_height, renderWidth, renderHeight);
		loggedSizeMismatch = true;
	}
	return false;
}

UINT SecondaryUpscale::Slots() const noexcept
{
	return local ? localSlots : CrossAdapterBridge::GetSingleton()->SlotCount();
}

bool SecondaryUpscale::HasFinalMirror() const noexcept
{
	auto bridge = CrossAdapterBridge::GetSingleton();
	if (!OwnsPresent() || finalLane < 0)
		return false;
	for (UINT slot = 0; slot < Slots(); ++slot)
		if (!bridge->Mirror(finalLane, slot))
			return false;
	return true;
}

void SecondaryUpscale::SetFinalLane(int a_lane)
{
	finalLane = a_lane;
	// Conversion only: no other lane is bound, so the finished frame's descriptors are written now.
	if (converting && !planned && !WriteFinishedDescriptors()) {
		converting = false;
		REX::CRITICAL("[FG-GPU1] the finished frame has no mirror on the display GPU; the {} swap chain gets no frames",
			FormatName(displayFormat));
	}
}

bool SecondaryUpscale::WriteFinishedDescriptors()
{
	auto bridge = CrossAdapterBridge::GetSingleton();
	if (!HasFinalMirror())
		return false;
	std::array<ID3D12Resource*, kSlots> finished{};
	for (UINT slot = 0; slot < Slots(); ++slot)
		finished[slot] = bridge->Mirror(finalLane, slot);
	return WriteCopyDescriptors(finished, DXGI_FORMAT_R8G8B8A8_UNORM);
}

bool SecondaryUpscale::WriteCopyDescriptors(const std::array<ID3D12Resource*, kSlots>& a_finished, DXGI_FORMAT a_format)
{
	if (!descriptors || !composite)
		return false;
	for (UINT slot = 0; slot < Slots(); ++slot) {
		if (!a_finished[slot])
			return false;
		D3D12_CPU_DESCRIPTOR_HANDLE handle = descriptors->GetCPUDescriptorHandleForHeapStart();
		handle.ptr += static_cast<SIZE_T>(slot * kDescriptorsPerSlot) * descriptorSize;
		// t0 and t2 are not read by Copy; null descriptors keep the table fully initialised.
		ID3D12Resource* sources[3] = { nullptr, a_finished[slot], nullptr };
		for (auto* source : sources) {
			D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
			srv.Format = a_format;
			srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			srv.Texture2D.MipLevels = 1;
			device->CreateShaderResourceView(source, &srv, handle);
			handle.ptr += descriptorSize;
		}
		D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
		uav.Format = displayFormat;
		uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
		device->CreateUnorderedAccessView(composite.get(), nullptr, &uav, handle);
	}
	return true;
}

bool SecondaryUpscale::BindLanes(int a_sceneLane, int a_depthLane, int a_motionLane)
{
	if (!planned || failed)
		return false;
	auto bridge = CrossAdapterBridge::GetSingleton();
	if (!HasFinalMirror()) {
		Fail("the finished frame has no mirror on the display GPU");
		return false;
	}

	sceneLane = a_sceneLane;
	depthLane = a_depthLane;
	motionLane = a_motionLane;
	for (UINT slot = 0; slot < Slots(); ++slot) {
		ID3D12Resource* scene = bridge->Mirror(sceneLane, slot);
		ID3D12Resource* finished = bridge->Mirror(finalLane, slot);
		if (!scene || !finished || !bridge->Mirror(depthLane, slot) || !bridge->Mirror(motionLane, slot)) {
			Fail("a lane has no mirror on the display GPU");
			return false;
		}
		const auto sceneDesc = scene->GetDesc();
		if (sceneDesc.Width != renderWidth || sceneDesc.Height != renderHeight) {
			Fail("the scene lane is not at the render resolution");
			return false;
		}

		D3D12_CPU_DESCRIPTOR_HANDLE handle = descriptors->GetCPUDescriptorHandleForHeapStart();
		handle.ptr += static_cast<SIZE_T>(slot * kDescriptorsPerSlot) * descriptorSize;
		ID3D12Resource* sources[3] = { scene, finished, outputs[slot].get() };
		for (auto* source : sources) {
			D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
			srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			srv.Texture2D.MipLevels = 1;
			device->CreateShaderResourceView(source, &srv, handle);
			handle.ptr += descriptorSize;
		}
		D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
		uav.Format = displayFormat;
		uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
		device->CreateUnorderedAccessView(composite.get(), nullptr, &uav, handle);
	}
	bound = true;
	REX::INFO("[FSR4] lanes bound: scene, depth and motion vectors {}x{} -> FSR 4.1 {}x{}; the UI is composited over it",
		renderWidth, renderHeight, displayWidth, displayHeight);
	return true;
}

void SecondaryUpscale::Fail(const char* a_reason)
{
	if (!failed)
		REX::CRITICAL("[FSR4] {}; upscaling on the display GPU is off for this session (frames are shown at the render "
					  "resolution until a restart, without frame generation)",
			a_reason);
	failed = true;
	pending = {};
	submitted = {};
}

void SecondaryUpscale::BeginFrame(UINT a_slot, float a_jitterX, float a_jitterY, float a_sharpness)
{
	if (!Ready() || a_slot >= kSlots)
		return;

	auto& p = params[a_slot];
	p.jitterX = a_jitterX;
	p.jitterY = a_jitterY;
	p.sharpness = std::clamp(a_sharpness, 0.0f, 1.0f);
	p.cameraNear = *(float*)REL::ID({ 57985, 2712882, 2712882 }).address();
	p.cameraFar = *(float*)REL::ID({ 958877, 2712883, 2712883 }).address();
	p.fovY = FidelityFX::VerticalFov();  // latched in the world pass

	pending[a_slot] = true;
	submitted[a_slot] = false;
	TrySubmit(a_slot);
}

bool SecondaryUpscale::FinishSubmit(UINT a_slot)
{
	if (!Ready() || a_slot >= kSlots)
		return false;
	if (pending[a_slot] && !TrySubmit(a_slot) && pending[a_slot]) {
		// Unpack queued every read of the frame, so this only happens if a lane was not sent.
		REX::WARN("[FSR4] the scene, depth or motion-vector lane did not reach the display GPU this frame; shown without upscaling");
		pending[a_slot] = false;
	}
	return submitted[a_slot];
}

bool SecondaryUpscale::TrySubmit(UINT a_slot)
{
	auto bridge = CrossAdapterBridge::GetSingleton();
	const int needed[] = { sceneLane, depthLane, motionLane };
	bool queued = false;
	HRESULT hr = bridge->WaitForLaneReads(queue, needed, std::size(needed), queued);
	if (FAILED(hr)) {
		Fail("waiting for the lane reads");
		return false;
	}
	if (!queued)
		return false;  // not read yet this frame: Present tries again after Unpack

	// The slot's allocator and timestamps were last used two frames ago.
	if (FAILED(bridge->WaitDisplayListDone(a_slot))) {
		Fail("waiting for the slot's previous frame");
		return false;
	}
	CollectTimings(a_slot);

	auto* allocator = allocators[a_slot].get();
	hr = allocator->Reset();
	if (SUCCEEDED(hr))
		hr = list->Reset(allocator, nullptr);
	if (FAILED(hr)) {
		Fail("resetting the upscale list");
		return false;
	}

	ID3D12Resource* scene = bridge->Mirror(sceneLane, a_slot);
	ID3D12Resource* depth = bridge->Mirror(depthLane, a_slot);
	ID3D12Resource* motion = bridge->Mirror(motionLane, a_slot);
	ID3D12Resource* output = outputs[a_slot].get();

	if (queryHeap)
		list->EndQuery(queryHeap.get(), D3D12_QUERY_TYPE_TIMESTAMP, a_slot * kMarks + kMarkUpscaleBegin);

	const D3D12_RESOURCE_BARRIER before[4] = {
		Transition(scene, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
		Transition(depth, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
		Transition(motion, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
		Transition(output, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
	};
	list->ResourceBarrier(4, before);

	LARGE_INTEGER now{};
	LARGE_INTEGER frequency{};
	QueryPerformanceCounter(&now);
	QueryPerformanceFrequency(&frequency);
	const double deltaMs = lastDispatch.QuadPart ?
	                           static_cast<double>(now.QuadPart - lastDispatch.QuadPart) * 1000.0 / static_cast<double>(frequency.QuadPart) :
	                           0.0;
	lastDispatch = now;
	// History is only valid across consecutive upscaled frames (menus and loading render natively).
	const bool reset = !lastFrameUpscaled || deltaMs <= 0.0 || deltaMs > 250.0;
	if (reset)
		++statResets;

	const auto& p = params[a_slot];
	ffx::DispatchDescUpscale dispatch{};
	dispatch.commandList = list.get();
	dispatch.color = ffxApiGetResourceDX12(scene, FFX_API_RESOURCE_STATE_COMPUTE_READ);
	dispatch.depth = ffxApiGetResourceDX12(depth, FFX_API_RESOURCE_STATE_COMPUTE_READ);
	dispatch.motionVectors = ffxApiGetResourceDX12(motion, FFX_API_RESOURCE_STATE_COMPUTE_READ);
	dispatch.output = ffxApiGetResourceDX12(output, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
	// Upscaling.dll's conventions for FSR: jitter in render pixels, negated; motion vectors in UV
	// units, so their scale is the render size.
	dispatch.jitterOffset.x = -p.jitterX;
	dispatch.jitterOffset.y = -p.jitterY;
	dispatch.motionVectorScale.x = static_cast<float>(renderWidth);
	dispatch.motionVectorScale.y = static_cast<float>(renderHeight);
	dispatch.renderSize = { renderWidth, renderHeight };
	dispatch.upscaleSize = { displayWidth, displayHeight };
	dispatch.enableSharpening = p.sharpness > 0.0f;
	dispatch.sharpness = p.sharpness;
	dispatch.frameTimeDelta = static_cast<float>(reset ? 0.0 : deltaMs);
	dispatch.preExposure = 1.0f;
	dispatch.reset = reset;
	dispatch.cameraNear = p.cameraNear;
	dispatch.cameraFar = p.cameraFar;
	dispatch.cameraFovAngleVertical = p.fovY;
	dispatch.viewSpaceToMetersFactor = 0.01428222656f;
	dispatch.flags = FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB;

	const auto rc = FidelityFX::GetSingleton()->DispatchUpscale(dispatch);

	const D3D12_RESOURCE_BARRIER after[4] = {
		Transition(scene, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
		Transition(depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
		Transition(motion, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
		Transition(output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON)
	};
	list->ResourceBarrier(4, after);

	if (queryHeap) {
		list->EndQuery(queryHeap.get(), D3D12_QUERY_TYPE_TIMESTAMP, a_slot * kMarks + kMarkUpscaleEnd);
		written[a_slot] |= (1u << kMarkUpscaleBegin) | (1u << kMarkUpscaleEnd);
	}

	hr = list->Close();
	if (FAILED(hr)) {
		Fail("closing the upscale list");
		return false;
	}
	if (rc != ffx::ReturnCode::Ok) {
		REX::ERROR("[FSR4] the upscale dispatch failed: {} ({})", ffxm::ReturnCodeName(static_cast<uint32_t>(rc)),
			static_cast<uint32_t>(rc));
		Fail("FSR 4.1 dispatch");
		return false;
	}

	ID3D12CommandList* lists[] = { list.get() };
	queue->ExecuteCommandLists(1, lists);

	if (!loggedFirstDispatch) {
		REX::INFO("[FSR4] first upscale: {}x{} -> {}x{}, jitter ({:.3f}, {:.3f}), sharpness {:.2f}, near {} far {} fovY {:.3f}",
			renderWidth, renderHeight, displayWidth, displayHeight, -p.jitterX, -p.jitterY, p.sharpness, p.cameraNear,
			p.cameraFar, p.fovY);
		loggedFirstDispatch = true;
	}

	pending[a_slot] = false;
	submitted[a_slot] = true;
	return true;
}

void SecondaryUpscale::RecordPresent(ID3D12GraphicsCommandList* a_list, UINT a_slot, bool a_upscaled,
	ID3D12Resource* a_backBuffer)
{
	if (!HasFinalMirror() || !a_list || !a_backBuffer || a_slot >= kSlots)
		return;
	auto bridge = CrossAdapterBridge::GetSingleton();
	ID3D12Resource* finished = bridge->Mirror(finalLane, a_slot);
	const bool compositeUi = a_upscaled && Ready();

	// A native frame does not pass TrySubmit, which collects the slot's previous timestamps; do it
	// here, and drop them if that frame is somehow still running, so two frames never mix.
	if (!a_upscaled) {
		CollectTimings(a_slot);
		written[a_slot] = 0;
	}

	if (queryHeap)
		a_list->EndQuery(queryHeap.get(), D3D12_QUERY_TYPE_TIMESTAMP, a_slot * kMarks + kMarkPresentBegin);

	if (compositeUi) {
		ID3D12Resource* scene = bridge->Mirror(sceneLane, a_slot);
		ID3D12Resource* output = outputs[a_slot].get();
		const D3D12_RESOURCE_BARRIER before[4] = {
			Transition(scene, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
			Transition(finished, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
			Transition(output, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
			Transition(composite.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
		};
		a_list->ResourceBarrier(4, before);

		ID3D12DescriptorHeap* heaps[] = { descriptors.get() };
		a_list->SetDescriptorHeaps(1, heaps);
		a_list->SetComputeRootSignature(rootSignature.get());
		a_list->SetPipelineState(pipeline.get());
		D3D12_GPU_DESCRIPTOR_HANDLE table = descriptors->GetGPUDescriptorHandleForHeapStart();
		table.ptr += static_cast<UINT64>(a_slot * kDescriptorsPerSlot) * descriptorSize;
		a_list->SetComputeRootDescriptorTable(0, table);
		a_list->Dispatch((displayWidth + 7) / 8, (displayHeight + 7) / 8, 1);

		const D3D12_RESOURCE_BARRIER after[5] = {
			Transition(scene, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
			Transition(finished, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
			Transition(output, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
			Transition(composite.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE),
			Transition(a_backBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST)
		};
		a_list->ResourceBarrier(5, after);
		a_list->CopyResource(a_backBuffer, composite.get());
		const D3D12_RESOURCE_BARRIER done[2] = {
			Transition(composite.get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON),
			Transition(a_backBuffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT)
		};
		a_list->ResourceBarrier(2, done);
	} else if (copyPipeline) {
		// Rendered natively (menus, loading), not upscalable, or not upscaling at all, into a back
		// buffer of another format (HDR add-on): converted on the way.
		RecordCopyThroughComposite(a_list, a_slot, finished, a_backBuffer);
	} else {
		// Rendered natively (menus, loading) or not upscalable: shown as the render GPU finished it.
		const D3D12_RESOURCE_BARRIER before[2] = {
			Transition(finished, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE),
			Transition(a_backBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST)
		};
		a_list->ResourceBarrier(2, before);
		a_list->CopyResource(a_backBuffer, finished);
		const D3D12_RESOURCE_BARRIER after[2] = {
			Transition(finished, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON),
			Transition(a_backBuffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT)
		};
		a_list->ResourceBarrier(2, after);
	}

	if (queryHeap) {
		a_list->EndQuery(queryHeap.get(), D3D12_QUERY_TYPE_TIMESTAMP, a_slot * kMarks + kMarkPresentEnd);
		written[a_slot] |= (1u << kMarkPresentBegin) | (1u << kMarkPresentEnd);
		if (compositeUi)
			written[a_slot] |= 1u << 31;  // the present marks time a composite, not a plain copy
		a_list->ResolveQueryData(queryHeap.get(), D3D12_QUERY_TYPE_TIMESTAMP, a_slot * kMarks, kMarks, readback.get(),
			sizeof(UINT64) * a_slot * kMarks);
	}
}

void SecondaryUpscale::RecordCopyThroughComposite(ID3D12GraphicsCommandList* a_list, UINT a_slot,
	ID3D12Resource* a_finished, ID3D12Resource* a_backBuffer)
{
	// A swap-chain buffer is a render target, not a UAV, so the shader writes the composite and the
	// copy engine moves it (same format) into the back buffer.
	const D3D12_RESOURCE_BARRIER before[2] = {
		Transition(a_finished, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
		Transition(composite.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
	};
	a_list->ResourceBarrier(2, before);

	ID3D12DescriptorHeap* heaps[] = { descriptors.get() };
	a_list->SetDescriptorHeaps(1, heaps);
	a_list->SetComputeRootSignature(rootSignature.get());
	a_list->SetPipelineState(copyPipeline.get());
	D3D12_GPU_DESCRIPTOR_HANDLE table = descriptors->GetGPUDescriptorHandleForHeapStart();
	table.ptr += static_cast<UINT64>(a_slot * kDescriptorsPerSlot) * descriptorSize;
	a_list->SetComputeRootDescriptorTable(0, table);
	a_list->Dispatch((displayWidth + 7) / 8, (displayHeight + 7) / 8, 1);

	const D3D12_RESOURCE_BARRIER after[3] = {
		Transition(a_finished, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
		Transition(composite.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE),
		Transition(a_backBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST)
	};
	a_list->ResourceBarrier(3, after);
	a_list->CopyResource(a_backBuffer, composite.get());
	const D3D12_RESOURCE_BARRIER done[2] = {
		Transition(composite.get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON),
		Transition(a_backBuffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT)
	};
	a_list->ResourceBarrier(2, done);
}

ID3D12Resource* SecondaryUpscale::Output(UINT a_slot) const noexcept
{
	return a_slot < kSlots ? outputs[a_slot].get() : nullptr;
}

void SecondaryUpscale::EndFrame(UINT a_slot, bool a_upscaled)
{
	if (a_slot >= kSlots)
		return;
	pending[a_slot] = false;
	submitted[a_slot] = false;
	lastFrameUpscaled = a_upscaled;
	if (!planned)
		return;
	if (a_upscaled)
		++statUpscaled;
	else
		++statNative;
}

void SecondaryUpscale::CollectTimings(UINT a_slot)
{
	if (!queryHeap || !readback || !written[a_slot])
		return;
	if (!CrossAdapterBridge::GetSingleton()->DisplayListDone(a_slot))
		return;  // keep the marks for the next try

	const UINT marks = written[a_slot];
	written[a_slot] = 0;
	D3D12_RANGE range{ sizeof(UINT64) * a_slot * kMarks, sizeof(UINT64) * (a_slot + 1) * kMarks };
	void* data = nullptr;
	if (FAILED(readback->Map(0, &range, &data)) || !data)
		return;
	UINT64 ticks[kMarks]{};
	std::memcpy(ticks, static_cast<const std::uint8_t*>(data) + range.Begin, sizeof(ticks));
	D3D12_RANGE nothing{ 0, 0 };
	readback->Unmap(0, &nothing);

	const double toMs = 1000.0 / static_cast<double>(timestampFrequency);
	const auto span = [&](UINT a_begin, UINT a_end, Average& a_stat) {
		if ((marks & (1u << a_begin)) && (marks & (1u << a_end)) && ticks[a_end] >= ticks[a_begin])
			a_stat.Add(static_cast<double>(ticks[a_end] - ticks[a_begin]) * toMs);
	};
	span(kMarkUpscaleBegin, kMarkUpscaleEnd, statUpscale);
	if (marks & (1u << 31))
		span(kMarkPresentBegin, kMarkPresentEnd, statComposite);
}

void SecondaryUpscale::LogStats()
{
	if (!planned)
		return;
	LARGE_INTEGER now{};
	LARGE_INTEGER frequency{};
	QueryPerformanceCounter(&now);
	QueryPerformanceFrequency(&frequency);
	if (static_cast<double>(now.QuadPart - statLastLog.QuadPart) < 10.0 * static_cast<double>(frequency.QuadPart))
		return;
	statLastLog = now;

	REX::INFO("[FSR4] {}x{} -> {}x{}: {} frames upscaled, {} at full resolution, {} history resets | FSR {} on the display GPU "
			  "avg {:.2f} ms max {:.2f} ms | UI composite + copy avg {:.2f} ms max {:.2f} ms{}",
		renderWidth, renderHeight, displayWidth, displayHeight, statUpscaled, statNative, statResets,
		FidelityFX::GetSingleton()->upscalerProvider, statUpscale.Mean(), statUpscale.max, statComposite.Mean(),
		statComposite.max, failed ? " | OFF (see the CRITICAL line above)" : "");
	statUpscale = {};
	statComposite = {};
	statUpscaled = 0;
	statNative = 0;
	statResets = 0;
}

// ---------------------------------------------------------------------------------------
// The upscaler calls these every frame; see up::Upscaling::SecondaryGPU. (They were
// AAAFrameGeneration.dll's exports for Upscaling.dll; now both are in one DLL.)

extern "C" bool FGGPU1_SecondaryUpscalerReady(uint32_t a_renderWidth, uint32_t a_renderHeight)
{
	return SecondaryUpscale::GetSingleton()->ReadyFor(a_renderWidth, a_renderHeight);
}

extern "C" void FGGPU1_SetSecondaryUpscale(bool a_active, uint32_t a_renderWidth,
	uint32_t a_renderHeight, float a_jitterX, float a_jitterY, float a_sharpness)
{
	auto& frame = Upscaling::GetSingleton()->externalFrame;
	frame.valid = true;
	frame.active = a_active;
	frame.width = a_renderWidth;
	frame.height = a_renderHeight;
	frame.jitterX = a_jitterX;
	frame.jitterY = a_jitterY;
	frame.sharpness = a_sharpness;
	frame.captured = false;
}
