#include "UITiles.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <cstring>
#include <format>
#include <string>

#include "MGPUScaling.h"

// Upscaling.cpp (FrameGeneration): compiles a shader file on the game's D3D11 device.
ID3D11DeviceChild* CompileShader(const wchar_t* FilePath, const char* ProgramType, const char* Program,
	const D3D_SHADER_MACRO* Defines);

namespace
{
	constexpr UINT kTile = 32;
	constexpr double kBytesPerTile32 = kTile * kTile * 4.0;  // RGBA8
	constexpr double kBytesPerTile16 = 16 * 16 * 4.0;
	constexpr double kMB = 1024.0 * 1024.0;

	uint32_t Percentile(std::vector<uint32_t>& a_values, double a_fraction)
	{
		if (a_values.empty())
			return 0;
		const auto index = static_cast<std::size_t>(a_fraction * static_cast<double>(a_values.size() - 1) + 0.5);
		std::nth_element(a_values.begin(), a_values.begin() + index, a_values.end());
		return a_values[index];
	}
}

void UITiles::Frame(ID3D11Device* a_device, ID3D11DeviceContext* a_context, bool a_measure,
	ID3D11ShaderResourceView* a_finished, ID3D11Texture2D* a_background, UINT a_width, UINT a_height)
{
	if (failed)
		return;

	Collect(a_context);

	if (a_measure && a_finished && a_background) {
		if (!initialized && !Initialize(a_device, a_background, a_width, a_height)) {
			failed = true;
			return;
		}
		if (a_background == background && a_width == width && a_height == height)
			Measure(a_context, a_finished);
	}

	LogStats();
}

bool UITiles::Initialize(ID3D11Device* a_device, ID3D11Texture2D* a_background, UINT a_width, UINT a_height)
{
	initialized = true;
	width = a_width;
	height = a_height;
	tilesX = (width + kTile - 1) / kTile;
	tilesY = (height + kTile - 1) / kTile;
	background = a_background;

	const std::string w = std::to_string(width);
	const std::string h = std::to_string(height);
	const D3D_SHADER_MACRO defines[] = { { "WIDTH", w.c_str() }, { "HEIGHT", h.c_str() }, { nullptr, nullptr } };
	shader.attach(static_cast<ID3D11ComputeShader*>(CompileShader(MGPU_SHADER(L"UITilesCS.hlsl"), "cs_5_0", "main", defines)));
	if (!shader) {
		REX::ERROR("[UI-TILES] UITilesCS.hlsl did not compile; UI tiles are not measured");
		return false;
	}

	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
	srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Texture2D.MipLevels = 1;
	HRESULT hr = a_device->CreateShaderResourceView(a_background, &srvDesc, backgroundSRV.put());
	if (FAILED(hr)) {
		REX::ERROR("[UI-TILES] no view of the scene stretch ({:#x}); UI tiles are not measured", static_cast<uint32_t>(hr));
		return false;
	}

	D3D11_BUFFER_DESC counterDesc{};
	counterDesc.ByteWidth = 16;
	counterDesc.Usage = D3D11_USAGE_DEFAULT;
	counterDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
	counterDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
	hr = a_device->CreateBuffer(&counterDesc, nullptr, counter.put());
	if (SUCCEEDED(hr)) {
		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
		uavDesc.Format = DXGI_FORMAT_R32_TYPELESS;
		uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
		uavDesc.Buffer.NumElements = 4;
		uavDesc.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
		hr = a_device->CreateUnorderedAccessView(counter.get(), &uavDesc, counterUAV.put());
	}

	D3D11_BUFFER_DESC stagingDesc{};
	stagingDesc.ByteWidth = 16;
	stagingDesc.Usage = D3D11_USAGE_STAGING;
	stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	const D3D11_QUERY_DESC disjointDesc{ D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
	const D3D11_QUERY_DESC timestampDesc{ D3D11_QUERY_TIMESTAMP, 0 };
	for (auto& slot : ring) {
		if (SUCCEEDED(hr))
			hr = a_device->CreateBuffer(&stagingDesc, nullptr, slot.staging.put());
		if (SUCCEEDED(hr))
			hr = a_device->CreateQuery(&disjointDesc, slot.disjoint.put());
		if (SUCCEEDED(hr))
			hr = a_device->CreateQuery(&timestampDesc, slot.begin.put());
		if (SUCCEEDED(hr))
			hr = a_device->CreateQuery(&timestampDesc, slot.end.put());
	}
	if (FAILED(hr)) {
		REX::ERROR("[UI-TILES] counter or read-back buffers not created ({:#x}); UI tiles are not measured",
			static_cast<uint32_t>(hr));
		return false;
	}

	QueryPerformanceCounter(&statLastLog);
	REX::INFO("[UI-TILES] measuring which tiles of the finished {}x{} frame hold UI ({}x{} tiles of 32x32) on upscaled "
			  "frames; logged every 10 s. Measurement only: the whole frame still crosses",
		width, height, tilesX, tilesY);
	return true;
}

void UITiles::Measure(ID3D11DeviceContext* a_context, ID3D11ShaderResourceView* a_finished)
{
	if (submitted - collected >= kRing) {
		++statSkipped;  // read-backs are late; keep the ring's oldest results instead
		return;
	}
	Slot& slot = ring[submitted % kRing];

	// The finished frame may still be bound as the render target; D3D11 reads zeros from a resource
	// bound for output (as in Upscaling::GenerateUIBuffer at this point of the frame).
	a_context->OMSetRenderTargets(0, nullptr, nullptr);

	a_context->Begin(slot.disjoint.get());
	a_context->End(slot.begin.get());

	const UINT zero[4] = {};
	a_context->ClearUnorderedAccessViewUint(counterUAV.get(), zero);
	ID3D11ShaderResourceView* views[2] = { a_finished, backgroundSRV.get() };
	a_context->CSSetShaderResources(0, 2, views);
	ID3D11UnorderedAccessView* uavs[1] = { counterUAV.get() };
	a_context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
	a_context->CSSetShader(shader.get(), nullptr, 0);
	a_context->Dispatch(tilesX, tilesY, 1);

	ID3D11ShaderResourceView* nullViews[2] = {};
	a_context->CSSetShaderResources(0, 2, nullViews);
	ID3D11UnorderedAccessView* nullUavs[1] = {};
	a_context->CSSetUnorderedAccessViews(0, 1, nullUavs, nullptr);
	a_context->CSSetShader(nullptr, nullptr, 0);

	a_context->End(slot.end.get());
	a_context->End(slot.disjoint.get());
	a_context->CopyResource(slot.staging.get(), counter.get());
	++submitted;
}

void UITiles::Collect(ID3D11DeviceContext* a_context)
{
	while (collected < submitted) {
		Slot& slot = ring[collected % kRing];
		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (a_context->Map(slot.staging.get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped) != S_OK)
			return;  // still in flight (DXGI_ERROR_WAS_STILL_DRAWING); later ones are too
		uint32_t counts[3];
		std::memcpy(counts, mapped.pData, sizeof(counts));
		a_context->Unmap(slot.staging.get(), 0);

		statTiles32.push_back(counts[0]);
		statTiles16.push_back(counts[1]);
		statPixels += counts[2];

		// The queries were ended before the copy that just finished, so they are done too.
		D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
		UINT64 begin = 0;
		UINT64 end = 0;
		if (a_context->GetData(slot.disjoint.get(), &disjoint, sizeof(disjoint), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
			a_context->GetData(slot.begin.get(), &begin, sizeof(begin), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
			a_context->GetData(slot.end.get(), &end, sizeof(end), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
			!disjoint.Disjoint && disjoint.Frequency && end >= begin) {
			const double ms = static_cast<double>(end - begin) * 1000.0 / static_cast<double>(disjoint.Frequency);
			statPassSum += ms;
			statPassMax = (std::max)(statPassMax, ms);
			++statPassCount;
		}
		++collected;
	}
}

void UITiles::LogStats()
{
	LARGE_INTEGER now{};
	LARGE_INTEGER frequency{};
	QueryPerformanceCounter(&now);
	QueryPerformanceFrequency(&frequency);
	if (static_cast<double>(now.QuadPart - statLastLog.QuadPart) < 10.0 * static_cast<double>(frequency.QuadPart))
		return;
	statLastLog = now;
	if (statTiles32.empty() && statSkipped == 0)
		return;  // menus, loading: nothing upscaled in this window

	const auto frames = static_cast<uint32_t>(statTiles32.size());
	const uint32_t total32 = tilesX * tilesY;
	const uint32_t total16 = total32 * 4;
	double sum32 = 0.0;
	double sum16 = 0.0;
	uint32_t max32 = 0;
	uint32_t max16 = 0;
	uint32_t empty = 0;
	for (uint32_t i = 0; i < frames; ++i) {
		sum32 += statTiles32[i];
		sum16 += statTiles16[i];
		max32 = (std::max)(max32, statTiles32[i]);
		max16 = (std::max)(max16, statTiles16[i]);
		empty += statTiles32[i] == 0;
	}
	const double avg32 = frames ? sum32 / frames : 0.0;
	const double avg16 = frames ? sum16 / frames : 0.0;
	const uint32_t p50 = Percentile(statTiles32, 0.50);
	const uint32_t p95 = Percentile(statTiles32, 0.95);
	const uint32_t p95q = Percentile(statTiles16, 0.95);
	const double pixelsAvg = frames ? static_cast<double>(statPixels) / frames : 0.0;

	// What would cross per frame: the tiles plus a 16-bit tile map. The whole frame is width x height x 4.
	const double map32 = total32 * 2.0;
	const double map16 = total16 * 2.0;
	const double fullMB = width * static_cast<double>(height) * 4.0 / kMB;

	REX::INFO("[UI-TILES] {} upscaled frames | 32x32 tiles with UI: avg {:.0f} p50 {} p95 {} max {} of {} ({:.1f}% avg, {:.1f}% max) "
			  "| 16x16: avg {:.0f} p95 {} max {} of {} | UI pixels avg {:.0f} ({:.0f}% of the 32x32 tiles' area) | frames without UI {} "
			  "| would send avg {:.2f} MB (32x32) / {:.2f} MB (16x16), max {:.2f} / {:.2f}, whole frame {:.2f} MB "
			  "| pass on render GPU avg {:.3f} ms max {:.3f} ms{}",
		frames, avg32, p50, p95, max32, total32, 100.0 * avg32 / total32, 100.0 * max32 / total32,
		avg16, p95q, max16, total16, pixelsAvg, avg32 > 0.0 ? 100.0 * pixelsAvg / (avg32 * kTile * kTile) : 0.0, empty,
		(avg32 * kBytesPerTile32 + map32) / kMB, (avg16 * kBytesPerTile16 + map16) / kMB,
		(max32 * kBytesPerTile32 + map32) / kMB, (max16 * kBytesPerTile16 + map16) / kMB, fullMB,
		statPassCount ? statPassSum / statPassCount : 0.0, statPassMax,
		statSkipped ? std::format(" | {} frames not measured (read-back late)", statSkipped) : std::string());

	statTiles32.clear();
	statTiles16.clear();
	statPixels = 0;
	statPassSum = 0.0;
	statPassMax = 0.0;
	statPassCount = 0;
	statSkipped = 0;
}
