#pragma once

// UITiles - FG-GPU1, plans/UI-TILES.md part 2, step 1: measure only, nothing crosses PCIe differently.
//
// With FSR 4.1 on the display GPU the render GPU fills the frame buffer from the stretched scene at
// PostDisplay (Upscaling::FillFrameBufferFromScene) and the game draws its UI over that. At Present
// the finished frame therefore differs from the stretch (Upscaling::sceneBlitTarget) exactly where
// the UI drew. UITilesCS.hlsl counts the 32x32 and 16x16 tiles that differ, on the render GPU; the
// counts are read back a few frames later without waiting and logged every 10 s as "[UI-TILES]",
// with what sending only those tiles would cost. With the HUD hidden (console: tm) the count must
// be ~0, or something other than the UI writes the frame buffer after PostDisplay.
//
// Runs on upscaled frames only (menus and loading screens are rendered natively and have no
// background to compare with), not with ENB.

#include <d3d11.h>
#include <winrt/base.h>

#include <array>
#include <cstdint>
#include <vector>

class UITiles
{
public:
	static UITiles* GetSingleton()
	{
		static UITiles singleton;
		return &singleton;
	}

	// Every Present, on the render thread. a_measure: this frame was upscaled on the display GPU and
	// a_background holds its stretch. Reads back earlier frames' counts first.
	void Frame(ID3D11Device* a_device, ID3D11DeviceContext* a_context, bool a_measure,
		ID3D11ShaderResourceView* a_finished, ID3D11Texture2D* a_background, UINT a_width, UINT a_height);

private:
	static constexpr UINT kRing = 8;  // read-back slots: counts arrive ~2-3 frames late

	struct Slot
	{
		winrt::com_ptr<ID3D11Buffer> staging;
		winrt::com_ptr<ID3D11Query> disjoint;
		winrt::com_ptr<ID3D11Query> begin;
		winrt::com_ptr<ID3D11Query> end;
	};

	bool Initialize(ID3D11Device* a_device, ID3D11Texture2D* a_background, UINT a_width, UINT a_height);
	void Measure(ID3D11DeviceContext* a_context, ID3D11ShaderResourceView* a_finished);
	void Collect(ID3D11DeviceContext* a_context);
	void LogStats();

	bool initialized = false;
	bool failed = false;
	UINT width = 0;
	UINT height = 0;
	UINT tilesX = 0;  // 32x32 tiles
	UINT tilesY = 0;

	winrt::com_ptr<ID3D11ComputeShader> shader;
	winrt::com_ptr<ID3D11ShaderResourceView> backgroundSRV;
	ID3D11Texture2D* background = nullptr;  // Upscaling's, not owned
	winrt::com_ptr<ID3D11Buffer> counter;
	winrt::com_ptr<ID3D11UnorderedAccessView> counterUAV;
	std::array<Slot, kRing> ring;
	uint64_t submitted = 0;  // measurements recorded
	uint64_t collected = 0;  // measurements read back (always <= submitted)

	// This 10 s window.
	std::vector<uint32_t> statTiles32;
	std::vector<uint32_t> statTiles16;
	uint64_t statPixels = 0;
	double statPassSum = 0.0;
	double statPassMax = 0.0;
	uint32_t statPassCount = 0;
	uint32_t statSkipped = 0;  // ring full: a frame not measured
	LARGE_INTEGER statLastLog{};
};
