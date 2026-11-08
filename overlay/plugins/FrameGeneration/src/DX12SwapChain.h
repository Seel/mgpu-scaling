#pragma once

#include <Windows.Foundation.h>
#include <stdio.h>
#include <winrt/base.h>
#include <wrl\client.h>
#include <wrl\wrappers\corewrappers.h>

#include <d3d11_4.h>
#include <d3d12.h>

#include <array>
#include <string>

#include "Buffer.h"
#include "CrossAdapterBridge.h"

class WrappedResource
{
public:
	WrappedResource(D3D11_TEXTURE2D_DESC a_texDesc, ID3D11Device5* a_d3d11Device, ID3D12Device* a_d3d12Device);

	ID3D11Texture2D* resource11 = nullptr;
	ID3D11ShaderResourceView* srv = nullptr;
	ID3D11UnorderedAccessView* uav = nullptr;
	ID3D11RenderTargetView* rtv = nullptr;
	winrt::com_ptr<ID3D12Resource> resource;
};

struct DXGISwapChainProxy : IDXGISwapChain
{
public:
	DXGISwapChainProxy(IDXGISwapChain4* a_swapChain);

	IDXGISwapChain4* swapChain;

	/****IUnknown****/
	virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObj) override;
	virtual ULONG STDMETHODCALLTYPE AddRef() override;
	virtual ULONG STDMETHODCALLTYPE Release() override;

	/****IDXGIObject****/
	virtual HRESULT STDMETHODCALLTYPE SetPrivateData(_In_ REFGUID Name, UINT DataSize, _In_reads_bytes_(DataSize) const void* pData) override;
	virtual HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(_In_ REFGUID Name, _In_opt_ const IUnknown* pUnknown) override;
	virtual HRESULT STDMETHODCALLTYPE GetPrivateData(_In_ REFGUID Name, _Inout_ UINT* pDataSize, _Out_writes_bytes_(*pDataSize) void* pData) override;
	virtual HRESULT STDMETHODCALLTYPE GetParent(_In_ REFIID riid, _COM_Outptr_ void** ppParent) override;

	/****IDXGIDeviceSubObject****/
	virtual HRESULT STDMETHODCALLTYPE GetDevice(_In_ REFIID riid, _COM_Outptr_ void** ppDevice) override;

	/****IDXGISwapChain****/
	virtual HRESULT STDMETHODCALLTYPE Present(UINT SyncInterval, UINT Flags);
	virtual HRESULT STDMETHODCALLTYPE GetBuffer(UINT Buffer, _In_ REFIID riid, _COM_Outptr_ void** ppSurface);
	virtual HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL Fullscreen, _In_opt_ IDXGIOutput* pTarget);
	virtual HRESULT STDMETHODCALLTYPE GetFullscreenState(_Out_opt_ BOOL* pFullscreen, _COM_Outptr_opt_result_maybenull_ IDXGIOutput** ppTarget);
	virtual HRESULT STDMETHODCALLTYPE GetDesc(_Out_ DXGI_SWAP_CHAIN_DESC* pDesc);
	virtual HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags);
	virtual HRESULT STDMETHODCALLTYPE ResizeTarget(_In_ const DXGI_MODE_DESC* pNewTargetParameters);
	virtual HRESULT STDMETHODCALLTYPE GetContainingOutput(_COM_Outptr_ IDXGIOutput** ppOutput);
	virtual HRESULT STDMETHODCALLTYPE GetFrameStatistics(_Out_ DXGI_FRAME_STATISTICS* pStats);
	virtual HRESULT STDMETHODCALLTYPE GetLastPresentCount(_Out_ UINT* pLastPresentCount);
};

// The render GPU's slack: D3D11 timestamps on the game's context at the end of each frame (top of
// Present), at the first command recorded after Present returned (the GPU only reaches it once the
// next frame's work is submitted), and at the end of the next frame. The gap between the first two
// is time the render GPU had nothing to execute: it finished the frame before the game had sent
// any of the next one (the game was waiting in Present, simulating, or held by the frame limiter).
// Read back a few frames later without waiting; reported every 10 s.
class RenderGpuTimer
{
public:
	void FrameEnd(ID3D11Device* a_device, ID3D11DeviceContext* a_context);  // top of Present
	void NextFrameStart(ID3D11DeviceContext* a_context);                     // last thing before Present returns
	void Collect(ID3D11DeviceContext* a_context);
	// "idle between frames ..." for the 10 s line; empty when nothing was measured.
	std::string TakeSummary();

private:
	static constexpr UINT kRecords = 8;
	struct Record
	{
		winrt::com_ptr<ID3D11Query> disjoint;
		winrt::com_ptr<ID3D11Query> end;
		winrt::com_ptr<ID3D11Query> start;
		winrt::com_ptr<ID3D11Query> nextEnd;
		bool open = false;     // end issued, start/nextEnd still to come
		bool started = false;  // start issued
		bool pending = false;  // closed, waiting for its data
	};
	bool Create(ID3D11Device* a_device);

	std::array<Record, kRecords> records;
	UINT current = kRecords;  // the open record, kRecords if none
	UINT next = 0;
	bool created = false;
	bool failed = false;

	double idleSum = 0.0, idleMax = 0.0, spanSum = 0.0, spanMax = 0.0;
	UINT64 samples = 0, idleFrames = 0, skipped = 0;
};

class DX12SwapChain
{
public:
	static DX12SwapChain* GetSingleton()
	{
		static DX12SwapChain singleton;
		return &singleton;
	}

	winrt::com_ptr<ID3D12Device> d3d12Device;
	winrt::com_ptr<ID3D12CommandQueue> commandQueue;
	// Frames in flight: the FFX swap chain's buffer count and the number of every per-frame
	// resource (FrameSlots.h). 3 with bExtraFrameInFlight in second-GPU mode, else 2.
	UINT frameSlots = 2;

	winrt::com_ptr<ID3D12CommandAllocator> commandAllocators[kMaxFrameSlots];
	winrt::com_ptr<ID3D12GraphicsCommandList4> commandLists[kMaxFrameSlots];

	IDXGISwapChain4* swapChain;

	// The swap chain as the game asked for it: its format is the one the game renders (RGBA8) and
	// the D3D11 side (interop textures, lanes) uses.
	DXGI_SWAP_CHAIN_DESC1 swapChainDesc;
	// The format the back buffers really have (read from the buffers: ReShade's GetDesc1 reports the
	// requested one). An HDR add-on (ReShade's AutoHDR) can make it R10G10B10A2 or
	// R16G16B16A16_FLOAT; everything that writes the back buffer follows it.
	DXGI_FORMAT displayFormat = DXGI_FORMAT_UNKNOWN;

	Texture2D* swapChainBufferProxy;
	WrappedResource* swapChainBufferProxyENB;

	WrappedResource* swapChainBufferWrapped[kMaxFrameSlots]{};

	winrt::com_ptr<ID3D11Device5> d3d11Device;
	winrt::com_ptr<ID3D11DeviceContext4> d3d11Context;

	winrt::com_ptr<ID3D11Fence> d3d11Fence;
	winrt::com_ptr<ID3D12Fence> d3d12Fence;

	winrt::com_ptr<ID3D12Resource> swapChainBuffers[kMaxFrameSlots];

	// Test branch: with bSendHudlessFrame = 0 in second-GPU mode, FFX still gets a colour resource
	// to interpolate: a copy of the composited back buffer (so the UI difference is zero and the
	// HUD ends up inside the interpolation, as it is on one GPU). Created on first use.
	winrt::com_ptr<ID3D12Resource> hudlessStandIn[kMaxFrameSlots];
	ID3D12Resource* HudlessStandInFor(UINT a_slot);

	UINT frameIndex = 0;
	UINT64 fenceValue = 0;

	// Diagnostics: game frames rendered vs frames actually presented (the game's own plus
	// FSR3-generated ones), reported every 10 s by Present().
	UINT lastPresentCount = 0;
	UINT64 statGameFrames = 0;
	UINT64 statPresentedFrames = 0;
	LARGE_INTEGER statPresentStart{};
	RenderGpuTimer renderGpuTimer;

	LARGE_INTEGER qpf;

	DXGISwapChainProxy* swapChainProxy = nullptr;

	// Secondary-GPU mode (bFrameGenOnSecondaryGPU). d3d12Device is then the device on the
	// display GPU that runs FFX frame generation and owns the swap chain; interopDevice is
	// on the game's adapter and is the one that opens the D3D11 shared textures/fence.
	// In single-GPU mode interopDevice is null and GetInteropDevice() returns d3d12Device.
	winrt::com_ptr<ID3D12Device> interopDevice;
	bool secondaryGPU = false;
	int finalFrameLane = -1;

	ID3D12Device* GetInteropDevice() const { return interopDevice ? interopDevice.get() : d3d12Device.get(); }

	void CreateD3D12Device(IDXGIAdapter* a_adapter);
	// Tries the two-device setup; falls back to CreateD3D12Device(a_gameAdapter) on any failure.
	void CreateD3D12DevicesSecondaryGPU(IDXGIAdapter* a_gameAdapter, HWND a_window);
	void CreateD3D12CommandQueues();
	void CreateSwapChain(IDXGIFactory5* a_dxgiFactory, DXGI_SWAP_CHAIN_DESC swapChainDesc);
	void CreateSwapChainFSR3(IDXGIFactory5* a_dxgiFactory, DXGI_SWAP_CHAIN_DESC a_swapChainDesc);
	void CreateSwapChainDLSSG(IDXGIFactory5* a_dxgiFactory, DXGI_SWAP_CHAIN_DESC a_swapChainDesc);
	bool CreateSwapChainXeSS(IDXGIFactory5* a_dxgiFactory, DXGI_SWAP_CHAIN_DESC a_swapChainDesc);

	void CreateInterop();

	DXGISwapChainProxy* GetSwapChainProxy();
	void SetD3D11Device(ID3D11Device* a_d3d11Device);
	void SetD3D11DeviceContext(ID3D11DeviceContext* a_d3d11Context);

	HRESULT GetBuffer(void** ppSurface);
	// Frame generation on a second GPU: called from the frame hook at which a group of lanes
	// becomes final (world pass, HUD-less capture), so their PCIe copy starts before Present.
	void TransferEarlyLanes(CrossAdapterBridge::LaneTiming a_timing);
	HRESULT Present(UINT SyncInterval, UINT Flags);
	HRESULT GetDevice(_In_ REFIID riid, _COM_Outptr_ void** ppDevice);
};
