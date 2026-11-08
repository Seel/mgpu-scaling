// bridge-load - reproduce the second-GPU bridge's host-memory traffic without the game.
//
// The bridge writes the frame's lanes into system memory from the render GPU and reads
// them back out on the display GPU. Per 4K frame that is ~77 MB written to DRAM and
// ~77 MB read back, in a burst, once per game frame. This tool produces the same DRAM
// traffic with one GPU and no game:
//
//   cycle:  system memory -> VRAM   (copy engine reads host DRAM, writes VRAM)
//           VRAM -> system memory   (copy engine reads VRAM, writes host DRAM)
//   then idle until the next cycle starts, so the traffic arrives in the same bursty
//   shape a frame does.
//
// One cycle with -PayloadMB 77 and -Mode both moves 154 MB of host DRAM traffic and
// 154 MB across one PCIe link. Run it beside the game (game on the other GPU) and the
// only thing it can change is host memory and the one link it uses.
//
// Built by build.ps1; see README.md.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _WIN32_WINNT 0x0A00

#include <windows.h>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <string>
#include <vector>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

using Microsoft::WRL::ComPtr;

namespace
{

std::atomic<bool> g_stop{ false };

BOOL WINAPI OnCtrl(DWORD a_type)
{
	if (a_type == CTRL_C_EVENT || a_type == CTRL_BREAK_EVENT || a_type == CTRL_CLOSE_EVENT) {
		g_stop = true;
		return TRUE;
	}
	return FALSE;
}

double Now()
{
	static const double freq = [] {
		LARGE_INTEGER f{};
		QueryPerformanceFrequency(&f);
		return double(f.QuadPart);
	}();
	LARGE_INTEGER c{};
	QueryPerformanceCounter(&c);
	return double(c.QuadPart) / freq;
}

void Fail(const wchar_t* a_what, HRESULT a_hr)
{
	std::fwprintf(stderr, L"bridge-load: %ls failed (hr=0x%08X)\n", a_what, static_cast<unsigned>(a_hr));
	std::exit(2);
}

void Check(HRESULT a_hr, const wchar_t* a_what)
{
	if (FAILED(a_hr))
		Fail(a_what, a_hr);
}

void Usage()
{
	std::fwprintf(stdout,
		L"bridge-load - host-memory traffic for the second-GPU bridge, without the game\n"
		L"\n"
		L"  bridge-load [-Gpu <substring>] [-PayloadMB <MB>] [-Fps <Hz>] [-Seconds <s>]\n"
		L"              [-Mode both|up|down] [-BurstGapMs <ms>] [-List] [-Help]\n"
		L"\n"
		L"  -Gpu         adapter description substring; default \"9060\" (the display GPU)\n"
		L"  -PayloadMB   bytes per direction, decimal MB; default 77 (the FSR 4.1 4K payload)\n"
		L"  -Fps         cycles per second to pace to; default 38 (the game's frame rate);\n"
		L"               0 = as fast as the copies allow, no pacing\n"
		L"  -Seconds     stop after this long; default 0 = until Ctrl+C\n"
		L"  -Mode        both = up then down (154 MB/cycle, the bridge's traffic);\n"
		L"               up = host->VRAM only; down = VRAM->host only\n"
		L"  -BurstGapMs  idle between the up and the down copy; default 0\n"
		L"  -List        list adapters and exit\n");
}

struct Options
{
	std::wstring gpu = L"9060";
	double payloadMB = 77.0;
	double fps = 38.0;
	double seconds = 0.0;
	std::wstring mode = L"both";
	double burstGapMs = 0.0;
	bool list = false;
	bool helpShown = false;
};

std::wstring Lower(std::wstring a_s)
{
	std::transform(a_s.begin(), a_s.end(), a_s.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
	return a_s;
}

[[nodiscard]] bool ParseOptions(int a_argc, wchar_t** a_argv, Options& a_out)
{
	for (int i = 1; i < a_argc; ++i) {
		const std::wstring arg = a_argv[i];
		const auto next = [&](const wchar_t* a_name) -> const wchar_t* {
			if (i + 1 >= a_argc) {
				std::fwprintf(stderr, L"bridge-load: %ls needs a value\n", a_name);
				return nullptr;
			}
			return a_argv[++i];
		};

		if (arg == L"-Help" || arg == L"--help" || arg == L"-h") {
			Usage();
			a_out.helpShown = true;
			return false;
		}
		if (arg == L"-List") {
			a_out.list = true;
			continue;
		}
		if (arg == L"-Gpu") {
			const wchar_t* v = next(L"-Gpu");
			if (!v) return false;
			a_out.gpu = v;
		} else if (arg == L"-PayloadMB") {
			const wchar_t* v = next(L"-PayloadMB");
			if (!v) return false;
			a_out.payloadMB = _wtof(v);
		} else if (arg == L"-Fps") {
			const wchar_t* v = next(L"-Fps");
			if (!v) return false;
			a_out.fps = _wtof(v);
		} else if (arg == L"-Seconds") {
			const wchar_t* v = next(L"-Seconds");
			if (!v) return false;
			a_out.seconds = _wtof(v);
		} else if (arg == L"-Mode") {
			const wchar_t* v = next(L"-Mode");
			if (!v) return false;
			a_out.mode = Lower(v);
		} else if (arg == L"-BurstGapMs") {
			const wchar_t* v = next(L"-BurstGapMs");
			if (!v) return false;
			a_out.burstGapMs = _wtof(v);
		} else {
			std::fwprintf(stderr, L"bridge-load: unknown argument '%ls' (try -Help)\n", arg.c_str());
			return false;
		}
	}

	if (a_out.mode != L"both" && a_out.mode != L"up" && a_out.mode != L"down") {
		std::fwprintf(stderr, L"bridge-load: -Mode must be both, up or down\n");
		return false;
	}
	if (a_out.payloadMB <= 0.0 || a_out.fps < 0.0 || a_out.seconds < 0.0 || a_out.burstGapMs < 0.0) {
		std::fwprintf(stderr, L"bridge-load: -PayloadMB must be > 0; -Fps, -Seconds, -BurstGapMs >= 0\n");
		return false;
	}
	return true;
}

ComPtr<IDXGIAdapter1> PickAdapter(IDXGIFactory1* a_factory, const std::wstring& a_name, bool a_listOnly, bool& a_found)
{
	const std::wstring wanted = Lower(a_name);
	ComPtr<IDXGIAdapter1> picked;
	a_found = false;

	std::fwprintf(stdout, L"adapters:\n");
	for (UINT i = 0;; ++i) {
		ComPtr<IDXGIAdapter1> adapter;
		if (a_factory->EnumAdapters1(i, adapter.ReleaseAndGetAddressOf()) == DXGI_ERROR_NOT_FOUND)
			break;

		DXGI_ADAPTER_DESC1 desc{};
		adapter->GetDesc1(&desc);
		const bool software = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;

		std::fwprintf(stdout, L"  [%u] %-40ls %6.0f MB  LUID %08lX:%08lX%s\n", i, desc.Description,
			double(desc.DedicatedVideoMemory) / (1024.0 * 1024.0), desc.AdapterLuid.HighPart,
			desc.AdapterLuid.LowPart, software ? L"  (software)" : L"");

		if (software || picked)
			continue;
		if (Lower(desc.Description).find(wanted) != std::wstring::npos) {
			picked = adapter;
			a_found = true;
		}
	}
	if (a_listOnly)
		std::fwprintf(stdout, L"\n");
	else if (!picked)
		std::fwprintf(stderr, L"bridge-load: no hardware adapter matches '%ls'\n", a_name.c_str());
	return picked;
}

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* a_resource, D3D12_RESOURCE_STATES a_before, D3D12_RESOURCE_STATES a_after)
{
	D3D12_RESOURCE_BARRIER b{};
	b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
	b.Transition.pResource = a_resource;
	b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	b.Transition.StateBefore = a_before;
	b.Transition.StateAfter = a_after;
	return b;
}

ComPtr<ID3D12Resource> MakeBuffer(ID3D12Device* a_device, UINT64 a_size, D3D12_HEAP_TYPE a_heap, D3D12_RESOURCE_STATES a_state)
{
	D3D12_HEAP_PROPERTIES props{};
	props.Type = a_heap;
	props.CreationNodeMask = 1;
	props.VisibleNodeMask = 1;

	D3D12_RESOURCE_DESC desc{};
	desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	desc.Width = a_size;
	desc.Height = 1;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.Format = DXGI_FORMAT_UNKNOWN;
	desc.SampleDesc.Count = 1;
	desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	desc.Flags = D3D12_RESOURCE_FLAG_NONE;

	ComPtr<ID3D12Resource> resource;
	Check(a_device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc, a_state, nullptr,
			  IID_PPV_ARGS(resource.ReleaseAndGetAddressOf())),
		L"CreateCommittedResource");
	resource->SetName(a_heap == D3D12_HEAP_TYPE_UPLOAD	  ? L"host source"
					  : a_heap == D3D12_HEAP_TYPE_READBACK ? L"host destination"
														   : L"vram");
	return resource;
}

struct Traffic
{
	UINT64 cycles = 0;
	double upSumMs = 0.0;
	double upMaxMs = 0.0;
	double downSumMs = 0.0;
	double downMaxMs = 0.0;
	UINT64 latePeriods = 0;
};

} // namespace

int wmain(int argc, wchar_t** argv)
{
	Options opts;
	if (!ParseOptions(argc, argv, opts))
		return opts.helpShown ? 0 : 1;

	SetConsoleCtrlHandler(OnCtrl, TRUE);

	ComPtr<IDXGIFactory1> factory;
	Check(CreateDXGIFactory1(IID_PPV_ARGS(factory.ReleaseAndGetAddressOf())), L"CreateDXGIFactory1");

	bool found = false;
	ComPtr<IDXGIAdapter1> adapter = PickAdapter(factory.Get(), opts.gpu, opts.list, found);
	if (opts.list)
		return 0;
	if (!adapter)
		return 1;

	DXGI_ADAPTER_DESC1 adapterDesc{};
	adapter->GetDesc1(&adapterDesc);

	ComPtr<ID3D12Device> device;
	Check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(device.ReleaseAndGetAddressOf())),
		L"D3D12CreateDevice");

	D3D12_COMMAND_QUEUE_DESC queueDesc{};
	queueDesc.Type = D3D12_COMMAND_LIST_TYPE_COPY;
	ComPtr<ID3D12CommandQueue> queue;
	Check(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(queue.ReleaseAndGetAddressOf())), L"CreateCommandQueue");

	ComPtr<ID3D12CommandAllocator> allocator;
	Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(allocator.ReleaseAndGetAddressOf())),
		L"CreateCommandAllocator");

	ComPtr<ID3D12GraphicsCommandList> list;
	Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, allocator.Get(), nullptr,
			  IID_PPV_ARGS(list.ReleaseAndGetAddressOf())),
		L"CreateCommandList");
	Check(list->Close(), L"Close");

	ComPtr<ID3D12Fence> fence;
	Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.ReleaseAndGetAddressOf())), L"CreateFence");
	const HANDLE fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	if (!fenceEvent)
		Fail(L"CreateEventW", HRESULT_FROM_WIN32(GetLastError()));
	UINT64 fenceValue = 0;

	// Buffers must be a whole number of 64 KiB so the copies are single large DMAs.
	const UINT64 payload = (UINT64(std::llround(opts.payloadMB * 1e6)) + 65535) & ~UINT64(65535);

	ComPtr<ID3D12Resource> upload = MakeBuffer(device.Get(), payload, D3D12_HEAP_TYPE_UPLOAD,
		D3D12_RESOURCE_STATE_GENERIC_READ);
	ComPtr<ID3D12Resource> vram = MakeBuffer(device.Get(), payload, D3D12_HEAP_TYPE_DEFAULT,
		D3D12_RESOURCE_STATE_COMMON);
	ComPtr<ID3D12Resource> readback = MakeBuffer(device.Get(), payload, D3D12_HEAP_TYPE_READBACK,
		D3D12_RESOURCE_STATE_COPY_DEST);

	D3D12_RESOURCE_STATES vramState = D3D12_RESOURCE_STATE_COMMON;

	const auto submitUp = [&] {
		Check(allocator->Reset(), L"Reset(allocator)");
		Check(list->Reset(allocator.Get(), nullptr), L"Reset(list)");
		if (vramState != D3D12_RESOURCE_STATE_COPY_DEST) {
			const D3D12_RESOURCE_BARRIER b = Transition(vram.Get(), vramState, D3D12_RESOURCE_STATE_COPY_DEST);
			list->ResourceBarrier(1, &b);
			vramState = D3D12_RESOURCE_STATE_COPY_DEST;
		}
		list->CopyBufferRegion(vram.Get(), 0, upload.Get(), 0, payload);
		Check(list->Close(), L"Close");
		ID3D12CommandList* lists[] = { list.Get() };
		queue->ExecuteCommandLists(1, lists);
	};

	const auto submitDown = [&] {
		Check(allocator->Reset(), L"Reset(allocator)");
		Check(list->Reset(allocator.Get(), nullptr), L"Reset(list)");
		if (vramState != D3D12_RESOURCE_STATE_COPY_SOURCE) {
			const D3D12_RESOURCE_BARRIER b = Transition(vram.Get(), vramState, D3D12_RESOURCE_STATE_COPY_SOURCE);
			list->ResourceBarrier(1, &b);
			vramState = D3D12_RESOURCE_STATE_COPY_SOURCE;
		}
		list->CopyBufferRegion(readback.Get(), 0, vram.Get(), 0, payload);
		Check(list->Close(), L"Close");
		ID3D12CommandList* lists[] = { list.Get() };
		queue->ExecuteCommandLists(1, lists);
	};

	const auto waitGpu = [&] {
		++fenceValue;
		Check(queue->Signal(fence.Get(), fenceValue), L"Signal");
		if (fence->GetCompletedValue() < fenceValue) {
			Check(fence->SetEventOnCompletion(fenceValue, fenceEvent), L"SetEventOnCompletion");
			WaitForSingleObject(fenceEvent, INFINITE);
		}
	};

	const bool doUp = opts.mode != L"down";
	const bool doDown = opts.mode != L"up";
	const double mbPerCycle = opts.payloadMB * ((doUp && doDown) ? 2.0 : 1.0);
	const double period = opts.fps > 0.0 ? 1.0 / opts.fps : 0.0;

	// High-resolution waitable timer: pacing without Sleep()'s 15.6 ms granularity and
	// without timeBeginPeriod(), which would change the whole system's timer behaviour
	// under the game.
	const HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
		TIMER_ALL_ACCESS);

	const auto waitUntil = [&](double a_deadline) {
		const double remaining = a_deadline - Now();
		if (remaining <= 0.0)
			return false;
		LARGE_INTEGER due{};
		due.QuadPart = -std::max<LONGLONG>(1, static_cast<LONGLONG>(remaining * 1e7));
		if (timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
			WaitForSingleObject(timer, INFINITE);
		} else {
			Sleep(static_cast<DWORD>(remaining * 1000.0));
		}
		return true;
	};

	std::fwprintf(stdout, L"\nbridge-load: %ls\n", adapterDesc.Description);
	if (opts.fps > 0.0) {
		std::fwprintf(stdout, L"  paced %.1f Hz (%.2f ms period), gap %.1f ms, payload %.1f MB, mode %ls\n",
			opts.fps, period * 1000.0, opts.burstGapMs, opts.payloadMB, opts.mode.c_str());
	} else {
		std::fwprintf(stdout, L"  unpaced, payload %.1f MB, mode %ls\n", opts.payloadMB, opts.mode.c_str());
	}
	std::fwprintf(stdout, L"  %.1f MB per cycle -> %.1f MB host DRAM written + read per second at that rate\n\n",
		mbPerCycle, mbPerCycle * (opts.fps > 0.0 ? opts.fps : 1.0));

	Traffic total;
	double windowStart = Now();
	const double runStart = windowStart;
	Traffic window;
	UINT64 windowCycles = 0;

	double nextStart = Now();
	while (!g_stop) {
		if (opts.seconds > 0.0 && Now() - runStart >= opts.seconds)
			break;

		const double cycleStart = nextStart;
		++total.cycles;
		++windowCycles;

		if (doUp) {
			const double t0 = Now();
			submitUp();
			waitGpu();
			const double ms = (Now() - t0) * 1000.0;
			total.upSumMs += ms;
			total.upMaxMs = std::max(total.upMaxMs, ms);
			window.upSumMs += ms;
			window.upMaxMs = std::max(window.upMaxMs, ms);
		}
		if (opts.burstGapMs > 0.0)
			Sleep(static_cast<DWORD>(opts.burstGapMs));
		if (doDown) {
			const double t0 = Now();
			submitDown();
			waitGpu();
			const double ms = (Now() - t0) * 1000.0;
			total.downSumMs += ms;
			total.downMaxMs = std::max(total.downMaxMs, ms);
			window.downSumMs += ms;
			window.downMaxMs = std::max(window.downMaxMs, ms);
		}

		const double now = Now();
		nextStart = cycleStart + (period > 0.0 ? period : 0.0);
		if (period > 0.0) {
			if (nextStart > now) {
				waitUntil(nextStart);
			} else {
				++total.latePeriods;
				nextStart = now;
			}
		}

		const double elapsed = Now() - windowStart;
		if (elapsed >= 1.0) {
			const double rate = double(windowCycles) / elapsed;
			const double upAvg = windowCycles ? window.upSumMs / windowCycles : 0.0;
			const double downAvg = windowCycles ? window.downSumMs / windowCycles : 0.0;
			const double busy = period > 0.0 ? (upAvg + downAvg) / (period * 1000.0) * 100.0 : 0.0;
			std::fwprintf(stdout, L"[%6.1fs] %5.1f cyc/s", Now() - runStart, rate);
			if (doUp)
				std::fwprintf(stdout, L" | up %6.2f ms max %6.2f", upAvg, window.upMaxMs);
			if (doDown)
				std::fwprintf(stdout, L" | down %6.2f ms max %6.2f", downAvg, window.downMaxMs);
			std::fwprintf(stdout, L" | %5.2f GB/s host DRAM | busy %4.1f%%", rate * mbPerCycle / 1000.0, busy);
			if (total.latePeriods)
				std::fwprintf(stdout, L" | late %llu", static_cast<unsigned long long>(total.latePeriods));
			std::fwprintf(stdout, L"\n");

			window = Traffic{};
			windowCycles = 0;
			windowStart = Now();
		}
	}

	const double runSeconds = std::max(1e-9, Now() - runStart);
	const double movedGB = double(total.cycles) * mbPerCycle / 1000.0;
	std::fwprintf(stdout, L"\n%llu cycles in %.1f s: %.1f GB host DRAM traffic, %.2f GB/s average",
		static_cast<unsigned long long>(total.cycles), runSeconds, movedGB, movedGB / runSeconds);
	if (total.cycles) {
		if (doUp)
			std::fwprintf(stdout, L" | up avg %.2f ms", total.upSumMs / double(total.cycles));
		if (doDown)
			std::fwprintf(stdout, L" | down avg %.2f ms", total.downSumMs / double(total.cycles));
	}
	if (total.latePeriods)
		std::fwprintf(stdout, L" | %llu periods overran (the copies do not fit at -Fps %.1f)",
			static_cast<unsigned long long>(total.latePeriods), opts.fps);
	std::fwprintf(stdout, L"\n");

	if (timer)
		CloseHandle(timer);
	CloseHandle(fenceEvent);
	return 0;
}
