#include "CrossAdapterBridge.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <format>

namespace
{
	constexpr UINT64 kHeapAlignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;  // 64 KiB

	UINT64 AlignUp(UINT64 a_value, UINT64 a_alignment)
	{
		return (a_value + a_alignment - 1) & ~(a_alignment - 1);
	}

	bool LuidEqual(const LUID& a_left, const LUID& a_right)
	{
		return a_left.LowPart == a_right.LowPart && a_left.HighPart == a_right.HighPart;
	}

	std::string Narrow(const wchar_t* a_text)
	{
		std::string out;
		for (; a_text && *a_text; ++a_text)
			out += (*a_text < 0x80) ? static_cast<char>(*a_text) : '?';
		return out;
	}

	std::string Lower(std::string a_text)
	{
		std::transform(a_text.begin(), a_text.end(), a_text.begin(),
			[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return a_text;
	}

	bool OwnsMonitor(IDXGIAdapter1* a_adapter, HMONITOR a_monitor)
	{
		if (!a_monitor)
			return false;
		for (UINT index = 0;; ++index) {
			winrt::com_ptr<IDXGIOutput> output;
			const HRESULT hr = a_adapter->EnumOutputs(index, output.put());
			if (hr == DXGI_ERROR_NOT_FOUND)
				return false;
			if (FAILED(hr))
				return false;
			DXGI_OUTPUT_DESC desc{};
			// An output that is not attached to the desktop cannot be the monitor the game
			// window is on; its Monitor field is not a live handle.
			if (output && SUCCEEDED(output->GetDesc(&desc)) && desc.AttachedToDesktop && desc.Monitor == a_monitor)
				return true;
		}
	}

	bool HasAttachedOutput(IDXGIAdapter1* a_adapter)
	{
		for (UINT index = 0;; ++index) {
			winrt::com_ptr<IDXGIOutput> output;
			const HRESULT hr = a_adapter->EnumOutputs(index, output.put());
			if (hr == DXGI_ERROR_NOT_FOUND)
				return false;
			if (FAILED(hr))
				return false;
			DXGI_OUTPUT_DESC desc{};
			if (SUCCEEDED(output->GetDesc(&desc)) && desc.AttachedToDesktop)
				return true;
		}
	}

	void LogOutputs(IDXGIAdapter1* a_adapter, const std::string& a_name)
	{
		UINT count = 0;
		for (UINT index = 0;; ++index) {
			winrt::com_ptr<IDXGIOutput> output;
			const HRESULT hr = a_adapter->EnumOutputs(index, output.put());
			if (hr == DXGI_ERROR_NOT_FOUND)
				break;
			if (FAILED(hr)) {
				REX::WARN("[FG-GPU1]   output[{}] EnumOutputs failed hr={:#010x}", index, static_cast<std::uint32_t>(hr));
				break;
			}
			DXGI_OUTPUT_DESC desc{};
			if (SUCCEEDED(output->GetDesc(&desc))) {
				REX::INFO("[FG-GPU1]   output[{}] \"{}\" attached={} monitor={:#x}", index, Narrow(desc.DeviceName),
					desc.AttachedToDesktop ? 1 : 0, reinterpret_cast<std::uintptr_t>(desc.Monitor));
				++count;
			}
		}
		REX::INFO("[FG-GPU1]   \"{}\" outputs: {}", a_name, count);
	}

	D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* a_resource, D3D12_RESOURCE_STATES a_before,
		D3D12_RESOURCE_STATES a_after)
	{
		D3D12_RESOURCE_BARRIER barrier{};
		barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
		barrier.Transition.pResource = a_resource;
		barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		barrier.Transition.StateBefore = a_before;
		barrier.Transition.StateAfter = a_after;
		return barrier;
	}

	double QpcMs(const LARGE_INTEGER& a_from, const LARGE_INTEGER& a_to, const LARGE_INTEGER& a_frequency)
	{
		return static_cast<double>(a_to.QuadPart - a_from.QuadPart) * 1000.0 /
		       static_cast<double>(a_frequency.QuadPart);
	}

	// ---- bDirectGPUCopy: process-exit hooks --------------------------------------------------
	//
	// The render GPU's heaps on the display GPU's VRAM blocks must be closed while both drivers are still
	// loaded and before the blocks go. At DLL_PROCESS_DETACH the drivers (loaded after this DLL) have
	// already been detached, and TerminateProcess runs no detach at all. So the two ways a process ends
	// are hooked: ntdll!RtlExitUserProcess (behind ExitProcess and the CRT's exit) and
	// kernelbase!TerminateProcess. Fallout4.exe imports both.

	using ExitUserProcessFn = void(NTAPI*)(LONG);
	using TerminateProcessFn = BOOL(WINAPI*)(HANDLE, UINT);
	ExitUserProcessFn exitUserProcessOriginal = nullptr;
	TerminateProcessFn terminateProcessOriginal = nullptr;

	void NTAPI ExitUserProcessHook(LONG a_status)
	{
		CrossAdapterBridge::GetSingleton()->ReleaseDirectCopyForExit("the game is exiting");
		exitUserProcessOriginal(a_status);
	}

	BOOL WINAPI TerminateProcessHook(HANDLE a_process, UINT a_exitCode)
	{
		if (a_process == GetCurrentProcess() || GetProcessId(a_process) == GetCurrentProcessId())
			CrossAdapterBridge::GetSingleton()->ReleaseDirectCopyForExit("the game is terminating itself");
		return terminateProcessOriginal(a_process, a_exitCode);
	}

	// The detour library copies the entry's first instructions (a 5-byte jump's worth) into its trampoline
	// without fixing RIP-relative ones. Only hook an entry whose first instructions are ones known not to
	// be relative: the usual prologue forms. (Win10 19045: RtlExitUserProcess starts push rbx / sub rsp,
	// TerminateProcess mov [rsp+8],rbx.) Anything else: no hook, and then no direct copy.
	size_t PrologueInstructionLength(const std::uint8_t* a_code)
	{
		const std::uint8_t b0 = a_code[0];
		const std::uint8_t b1 = a_code[1];
		const std::uint8_t b2 = a_code[2];
		if (b0 >= 0x50 && b0 <= 0x57)
			return 1;  // push r64
		if ((b0 == 0x40 || b0 == 0x41) && b1 >= 0x50 && b1 <= 0x57)
			return 2;  // push r64 with REX
		if (b0 == 0x48 && b1 == 0x83 && b2 == 0xEC)
			return 4;  // sub rsp, imm8
		if (b0 == 0x48 && b1 == 0x81 && b2 == 0xEC)
			return 7;  // sub rsp, imm32
		if ((b0 == 0x48 || b0 == 0x4C) && b1 == 0x89 && (b2 & 0xC7) == 0x44 && a_code[3] == 0x24)
			return 5;  // mov [rsp+disp8], r64
		if ((b0 & 0xF0) == 0x40 && (b1 == 0x8B || b1 == 0x89) && (b2 & 0xC0) == 0xC0)
			return 3;  // mov r64, r64
		if ((b0 == 0x8B || b0 == 0x89) && (b1 & 0xC0) == 0xC0)
			return 2;  // mov r32, r32
		return 0;
	}

	bool SafeToDetour(std::uintptr_t a_entry)
	{
		if (stl::detail::HookedEntryTarget(a_entry))
			return true;  // another hook's jump: DetourOrChain chains behind it, nothing is copied
		const auto* code = reinterpret_cast<const std::uint8_t*>(a_entry);
		size_t covered = 0;
		while (covered < 5) {
			const size_t length = PrologueInstructionLength(code + covered);
			if (!length)
				return false;
			covered += length;
		}
		return true;
	}

	bool InstallExitHooks()
	{
		static bool installed = false;
		if (installed)
			return true;
		const auto exitEntry = reinterpret_cast<std::uintptr_t>(
			GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlExitUserProcess"));
		const auto terminateEntry = reinterpret_cast<std::uintptr_t>(
			GetProcAddress(GetModuleHandleW(L"kernelbase.dll"), "TerminateProcess"));
		if (!exitEntry || !terminateEntry) {
			REX::ERROR("[FG-GPU1] direct GPU copy: RtlExitUserProcess / TerminateProcess not found");
			return false;
		}
		for (const auto entry : { exitEntry, terminateEntry }) {
			if (!SafeToDetour(entry)) {
				const auto* code = reinterpret_cast<const std::uint8_t*>(entry);
				REX::ERROR("[FG-GPU1] direct GPU copy: cannot hook the process exit safely on this Windows build (entry at "
						   "{:#x} starts {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X})",
					entry, code[0], code[1], code[2], code[3], code[4], code[5], code[6], code[7]);
				return false;
			}
		}
		exitUserProcessOriginal = reinterpret_cast<ExitUserProcessFn>(stl::detail::DetourOrChain(exitEntry,
			reinterpret_cast<std::uintptr_t>(&ExitUserProcessHook), "RtlExitUserProcess"));
		terminateProcessOriginal = reinterpret_cast<TerminateProcessFn>(stl::detail::DetourOrChain(terminateEntry,
			reinterpret_cast<std::uintptr_t>(&TerminateProcessHook), "TerminateProcess"));
		installed = exitUserProcessOriginal && terminateProcessOriginal;
		if (!installed)
			REX::ERROR("[FG-GPU1] direct GPU copy: hooking the process exit failed");
		return installed;
	}

	const char* TimingName(CrossAdapterBridge::LaneTiming a_timing)
	{
		switch (a_timing) {
		case CrossAdapterBridge::LaneTiming::kWorld:
			return "world";
		case CrossAdapterBridge::LaneTiming::kHudless:
			return "hudless";
		default:
			return "present";
		}
	}
}

winrt::com_ptr<IDXGIAdapter1> CrossAdapterBridge::PickRenderAdapter(IDXGIAdapter* a_gameAdapter)
{
	if (!a_gameAdapter)
		return nullptr;

	DXGI_ADAPTER_DESC gameDesc{};
	if (FAILED(a_gameAdapter->GetDesc(&gameDesc)))
		return nullptr;

	winrt::com_ptr<IDXGIFactory1> factory;
	if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(factory.put()))))
		return nullptr;

	winrt::com_ptr<IDXGIAdapter1> display;
	std::vector<winrt::com_ptr<IDXGIAdapter1>> headless;
	for (UINT index = 0;; ++index) {
		winrt::com_ptr<IDXGIAdapter1> adapter;
		if (factory->EnumAdapters1(index, adapter.put()) == DXGI_ERROR_NOT_FOUND)
			break;
		DXGI_ADAPTER_DESC1 desc{};
		if (FAILED(adapter->GetDesc1(&desc)))
			continue;
		if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0 || desc.VendorId == 0x1414)
			continue;
		if (HasAttachedOutput(adapter.get()))
			display = adapter;
		else
			headless.push_back(adapter);
	}

	// Only act when the split is unambiguous: one GPU drives the display, one drives nothing.
	if (!display || headless.size() != 1)
		return nullptr;

	DXGI_ADAPTER_DESC1 displayDesc{};
	if (FAILED(display->GetDesc1(&displayDesc)))
		return nullptr;

	// Only move rendering when the game picked the display GPU. The target has no display
	// attached, so this cannot invert the intended roles if detection is off.
	if (!LuidEqual(gameDesc.AdapterLuid, displayDesc.AdapterLuid))
		return nullptr;

	return headless.front();
}

std::string CrossAdapterBridge::AdapterName(IDXGIAdapter* a_adapter){
	DXGI_ADAPTER_DESC desc{};
	if (!a_adapter || FAILED(a_adapter->GetDesc(&desc)))
		return "<unknown>";
	return Narrow(desc.Description);
}

winrt::com_ptr<IDXGIAdapter1> CrossAdapterBridge::SelectSecondaryAdapter(IDXGIAdapter* a_gameAdapter,
	HWND a_window, const std::string& a_nameFilter)
{
	DXGI_ADAPTER_DESC gameDesc{};
	if (!a_gameAdapter || FAILED(a_gameAdapter->GetDesc(&gameDesc))) {
		REX::ERROR("[FG-GPU1] Cannot read the game's adapter; staying on one GPU");
		return nullptr;
	}

	winrt::com_ptr<IDXGIFactory1> factory;
	if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(factory.put())))) {
		REX::ERROR("[FG-GPU1] CreateDXGIFactory1 failed; staying on one GPU");
		return nullptr;
	}

	const HMONITOR monitor = a_window ? MonitorFromWindow(a_window, MONITOR_DEFAULTTONEAREST) : nullptr;
	const std::string filter = Lower(a_nameFilter);

	{
		MONITORINFOEXW info{};
		info.cbSize = sizeof(info);
		const bool resolved = monitor && GetMonitorInfoW(monitor, &info);
		REX::INFO("[FG-GPU1] game window monitor {:#x} \"{}\"", reinterpret_cast<std::uintptr_t>(monitor),
			resolved ? Narrow(info.szDevice) : "unresolved");
	}

	std::vector<winrt::com_ptr<IDXGIAdapter1>> candidates;
	std::vector<winrt::com_ptr<IDXGIAdapter1>> named;
	std::vector<winrt::com_ptr<IDXGIAdapter1>> displaying;
	std::vector<winrt::com_ptr<IDXGIAdapter1>> withOutputs;

	for (UINT index = 0;; ++index) {
		winrt::com_ptr<IDXGIAdapter1> adapter;
		if (factory->EnumAdapters1(index, adapter.put()) == DXGI_ERROR_NOT_FOUND)
			break;
		DXGI_ADAPTER_DESC1 desc{};
		if (FAILED(adapter->GetDesc1(&desc)))
			continue;

		const std::string name = Narrow(desc.Description);
		const bool software = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0 || desc.VendorId == 0x1414;
		const bool isGame = LuidEqual(desc.AdapterLuid, gameDesc.AdapterLuid);
		const bool ownsMonitor = OwnsMonitor(adapter.get(), monitor);
		const bool hasOutput = HasAttachedOutput(adapter.get());

		REX::INFO("[FG-GPU1] adapter[{}] \"{}\" vendor={:#06x} luid={:#010x}-{:#010x} vram={}MB{}{}{}{}",
			index, name, desc.VendorId,
			static_cast<std::uint32_t>(desc.AdapterLuid.HighPart), desc.AdapterLuid.LowPart,
			desc.DedicatedVideoMemory / (1024 * 1024),
			isGame ? " [game]" : "", software ? " [software]" : "", ownsMonitor ? " [game window's monitor]" : "",
			hasOutput ? " [display]" : "");
		LogOutputs(adapter.get(), name);

		if (software || isGame)
			continue;
		candidates.push_back(adapter);
		if (!filter.empty() && Lower(name).find(filter) != std::string::npos)
			named.push_back(adapter);
		if (ownsMonitor)
			displaying.push_back(adapter);
		if (hasOutput)
			withOutputs.push_back(adapter);
	}

	const auto pick = [](const std::vector<winrt::com_ptr<IDXGIAdapter1>>& a_list, const char* a_rule)
		-> winrt::com_ptr<IDXGIAdapter1> {
		if (a_list.size() != 1)
			return nullptr;
		REX::INFO("[FG-GPU1] SELECTED \"{}\" ({})", AdapterName(a_list.front().get()), a_rule);
		return a_list.front();
	};

	if (!filter.empty()) {
		if (auto adapter = pick(named, "sSecondaryGPUName match"))
			return adapter;
		REX::ERROR("[FG-GPU1] sSecondaryGPUName=\"{}\" matched {} non-game hardware adapters (need exactly 1); staying on one GPU",
			a_nameFilter, named.size());
		return nullptr;
	}
	if (auto adapter = pick(displaying, "owns the monitor the game window is on"))
		return adapter;
	if (auto adapter = pick(withOutputs, "drives a display"))
		return adapter;
	if (auto adapter = pick(candidates, "only other hardware adapter"))
		return adapter;

	REX::ERROR("[FG-GPU1] {} candidate adapters and none is unambiguous; set sSecondaryGPUName. Staying on one GPU",
		candidates.size());
	return nullptr;
}

void CrossAdapterBridge::Fail(const char* a_where, HRESULT a_hr)
{
	if (active)
		REX::CRITICAL("[FG-GPU1] {} failed (hr={:#010x}); cross-adapter transport disabled", a_where,
			static_cast<std::uint32_t>(a_hr));
	active = false;
}

HRESULT CrossAdapterBridge::CreateSharedFencePair(UINT64 a_initial, winrt::com_ptr<ID3D12Fence>& a_onA,
	winrt::com_ptr<ID3D12Fence>& a_onB)
{
	// Create on one GPU, open on the other. Render GPU -> secondary first; the other
	// direction is the one MGPU Bridge proved for AMD -> NVIDIA, kept as a fallback.
	const auto attempt = [a_initial](ID3D12Device* a_owner, ID3D12Device* a_other,
							 winrt::com_ptr<ID3D12Fence>& a_created, winrt::com_ptr<ID3D12Fence>& a_opened) {
		a_created = nullptr;
		a_opened = nullptr;
		HRESULT hr = a_owner->CreateFence(a_initial,
			D3D12_FENCE_FLAG_SHARED | D3D12_FENCE_FLAG_SHARED_CROSS_ADAPTER, IID_PPV_ARGS(a_created.put()));
		if (FAILED(hr))
			return hr;
		HANDLE handle = nullptr;
		hr = a_owner->CreateSharedHandle(a_created.get(), nullptr, GENERIC_ALL, nullptr, &handle);
		if (FAILED(hr))
			return hr;
		hr = a_other->OpenSharedHandle(handle, IID_PPV_ARGS(a_opened.put()));
		CloseHandle(handle);
		return hr;
	};
	HRESULT hr = attempt(deviceA.get(), deviceB.get(), a_onA, a_onB);
	if (FAILED(hr)) {
		REX::WARN("[FG-GPU1] cross-adapter fence render->secondary failed (hr={:#010x}), trying secondary->render",
			static_cast<std::uint32_t>(hr));
		hr = attempt(deviceB.get(), deviceA.get(), a_onB, a_onA);
	}
	return hr;
}

HRESULT CrossAdapterBridge::CreateSharedHeapPair(UINT64 a_size, winrt::com_ptr<ID3D12Heap>& a_onA,
	winrt::com_ptr<ID3D12Heap>& a_onB)
{
	D3D12_HEAP_DESC heapDesc{};
	heapDesc.SizeInBytes = a_size;
	heapDesc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
	heapDesc.Alignment = kHeapAlignment;
	heapDesc.Flags = D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER;

	const auto attempt = [&heapDesc](ID3D12Device* a_owner, ID3D12Device* a_other,
							 winrt::com_ptr<ID3D12Heap>& a_created, winrt::com_ptr<ID3D12Heap>& a_opened) {
		a_created = nullptr;
		a_opened = nullptr;
		HRESULT hr = a_owner->CreateHeap(&heapDesc, IID_PPV_ARGS(a_created.put()));
		if (FAILED(hr))
			return hr;
		HANDLE handle = nullptr;
		hr = a_owner->CreateSharedHandle(a_created.get(), nullptr, GENERIC_ALL, nullptr, &handle);
		if (FAILED(hr))
			return hr;
		hr = a_other->OpenSharedHandle(handle, IID_PPV_ARGS(a_opened.put()));
		CloseHandle(handle);
		return hr;
	};
	HRESULT hr = attempt(deviceA.get(), deviceB.get(), a_onA, a_onB);
	if (FAILED(hr)) {
		REX::WARN("[FG-GPU1] cross-adapter heap render->secondary failed (hr={:#010x}), trying secondary->render",
			static_cast<std::uint32_t>(hr));
		hr = attempt(deviceB.get(), deviceA.get(), a_onB, a_onA);
	}
	return hr;
}

HRESULT CrossAdapterBridge::WaitCpu(ID3D12Fence* a_fence, UINT64 a_value)
{
	if (a_fence->GetCompletedValue() >= a_value)
		return S_OK;
	++statCpuWaits;
	HRESULT hr = a_fence->SetEventOnCompletion(a_value, fenceEvent);
	if (FAILED(hr))
		return hr;
	if (WaitForSingleObject(fenceEvent, 2000) != WAIT_OBJECT_0)
		return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
	return S_OK;
}

// ---------------------------------------------------------------------------------------
// GpuTimer

bool CrossAdapterBridge::GpuTimer::Create(ID3D12Device* a_device, ID3D12CommandQueue* a_queue, bool a_copyQueue,
	UINT a_marks)
{
	Reset();
	if (!a_device || !a_queue || a_marks == 0 || a_marks > kMaxMarks)
		return false;

	if (a_copyQueue) {
		D3D12_FEATURE_DATA_D3D12_OPTIONS3 options3{};
		if (FAILED(a_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS3, &options3, sizeof(options3))) ||
			!options3.CopyQueueTimestampQueriesSupported)
			return false;
	}

	UINT64 ticksPerSecond = 0;
	if (FAILED(a_queue->GetTimestampFrequency(&ticksPerSecond)) || !ticksPerSecond)
		return false;

	D3D12_QUERY_HEAP_DESC heapDesc{};
	heapDesc.Type = a_copyQueue ? D3D12_QUERY_HEAP_TYPE_COPY_QUEUE_TIMESTAMP : D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
	heapDesc.Count = kSlots * a_marks;

	D3D12_HEAP_PROPERTIES readbackProps{};
	readbackProps.Type = D3D12_HEAP_TYPE_READBACK;
	D3D12_RESOURCE_DESC bufferDesc{};
	bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	bufferDesc.Width = sizeof(UINT64) * kSlots * a_marks;
	bufferDesc.Height = 1;
	bufferDesc.DepthOrArraySize = 1;
	bufferDesc.MipLevels = 1;
	bufferDesc.SampleDesc.Count = 1;
	bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

	if (FAILED(a_device->CreateQueryHeap(&heapDesc, IID_PPV_ARGS(heap.put()))) ||
		FAILED(a_device->CreateCommittedResource(&readbackProps, D3D12_HEAP_FLAG_NONE, &bufferDesc,
			D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(readback.put())))) {
		Reset();
		return false;
	}

	marks = a_marks;
	frequency = ticksPerSecond;
	Calibrate(a_queue);
	return true;
}

void CrossAdapterBridge::GpuTimer::Calibrate(ID3D12CommandQueue* a_queue)
{
	if (!a_queue || !frequency)
		return;
	UINT64 gpu = 0;
	UINT64 cpu = 0;
	if (SUCCEEDED(a_queue->GetClockCalibration(&gpu, &cpu))) {
		calibrationGpu = gpu;
		calibrationQpc = cpu;
		calibrated = true;
	}
}

void CrossAdapterBridge::GpuTimer::Mark(ID3D12GraphicsCommandList* a_list, UINT a_slot, UINT a_mark)
{
	if (!Valid() || !a_list || a_slot >= kSlots || a_mark >= marks)
		return;
	a_list->EndQuery(heap.get(), D3D12_QUERY_TYPE_TIMESTAMP, a_slot * marks + a_mark);
	written[a_slot] |= 1u << a_mark;
}

void CrossAdapterBridge::GpuTimer::Resolve(ID3D12GraphicsCommandList* a_list, UINT a_slot)
{
	if (!Valid() || !a_list || a_slot >= kSlots)
		return;
	a_list->ResolveQueryData(heap.get(), D3D12_QUERY_TYPE_TIMESTAMP, a_slot * marks, marks, readback.get(),
		sizeof(UINT64) * a_slot * marks);
}

bool CrossAdapterBridge::GpuTimer::Read(UINT a_slot, std::array<double, kMaxMarks>& a_qpc, UINT& a_valid)
{
	a_valid = 0;
	if (!Valid() || a_slot >= kSlots || !written[a_slot])
		return false;
	const UINT mask = written[a_slot];
	written[a_slot] = 0;

	const D3D12_RANGE range{ sizeof(UINT64) * a_slot * marks, sizeof(UINT64) * (a_slot * marks + marks) };
	void* mapped = nullptr;
	if (FAILED(readback->Map(0, &range, &mapped)))
		return false;
	const auto* ticks = reinterpret_cast<const UINT64*>(static_cast<const std::uint8_t*>(mapped) + range.Begin);

	LARGE_INTEGER qpcPerSecond{};
	QueryPerformanceFrequency(&qpcPerSecond);
	const double qpcPerTick = static_cast<double>(qpcPerSecond.QuadPart) / static_cast<double>(frequency);
	for (UINT mark = 0; mark < marks; ++mark) {
		if (!(mask & (1u << mark)))
			continue;
		// Signed: a timestamp may be slightly older than the latest calibration.
		const auto sinceCalibration = static_cast<std::int64_t>(ticks[mark] - calibrationGpu);
		a_qpc[mark] = static_cast<double>(calibrationQpc) + static_cast<double>(sinceCalibration) * qpcPerTick;
	}

	const D3D12_RANGE nothingWritten{ 0, 0 };
	readback->Unmap(0, &nothingWritten);
	a_valid = mask;
	return true;
}

void CrossAdapterBridge::GpuTimer::Reset() noexcept
{
	heap = nullptr;
	readback = nullptr;
	marks = 0;
	frequency = 0;
	calibrated = false;
	calibrationGpu = 0;
	calibrationQpc = 0;
	written = {};
}

// ---------------------------------------------------------------------------------------
// Setup

bool CrossAdapterBridge::Initialize(ID3D12Device* a_deviceA, ID3D12Device* a_deviceB,
	ID3D11Device5* a_device11, ID3D11DeviceContext4* a_context11, bool a_asyncUnpack, UINT a_slots,
	bool a_directCopy)
{
	Shutdown();
	if (!a_deviceA || !a_deviceB || !a_device11 || !a_context11)
		return false;
	slotCount = std::clamp(a_slots, 2u, kSlots);

	deviceA.copy_from(a_deviceA);
	deviceB.copy_from(a_deviceB);
	context11.copy_from(a_context11);
	active = true;
	asyncUnpack = a_asyncUnpack;

	const auto check = [this](HRESULT a_hr, const char* a_what) {
		if (FAILED(a_hr))
			Fail(a_what, a_hr);
		return SUCCEEDED(a_hr);
	};

	D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
	if (SUCCEEDED(deviceB->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options))))
		REX::INFO("[FG-GPU1] secondary device: CrossNodeSharingTier={} CrossAdapterRowMajorTextureSupported={}",
			static_cast<int>(options.CrossNodeSharingTier), options.CrossAdapterRowMajorTextureSupported ? 1 : 0);

	D3D12_COMMAND_QUEUE_DESC queueDesc{};
	queueDesc.Type = D3D12_COMMAND_LIST_TYPE_COPY;
	queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
	if (!check(deviceA->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(copyQueueA.put())), "copy queue on the render GPU"))
		return false;
	copyQueueA->SetName(L"FG-GPU1 transfer (render GPU copy queue)");

	for (auto& allocator : allocatorsA) {
		if (!check(deviceA->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(allocator.put())),
				"copy allocator"))
			return false;
	}
	if (!check(deviceA->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, allocatorsA[0].get(), nullptr,
				   IID_PPV_ARGS(listA.put())),
			"copy command list"))
		return false;
	listA->Close();

	if (!check(CreateSharedFencePair(0, transferFenceA, transferFenceB), "cross-adapter transfer fence"))
		return false;
	if (!check(CreateSharedFencePair(0, consumedFenceA, consumedFenceB), "cross-adapter consumed fence"))
		return false;

	if (!check(deviceA->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(copyDoneFenceA.put())), "copy-done fence"))
		return false;
	{
		HANDLE handle = nullptr;
		if (!check(deviceA->CreateSharedHandle(copyDoneFenceA.get(), nullptr, GENERIC_ALL, nullptr, &handle),
				"copy-done fence handle"))
			return false;
		const HRESULT hr = a_device11->OpenSharedFence(handle, IID_PPV_ARGS(copyDoneFence11.put()));
		CloseHandle(handle);
		if (!check(hr, "opening the copy-done fence on D3D11"))
			return false;
	}

	fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	if (!fenceEvent) {
		Fail("CreateEvent", HRESULT_FROM_WIN32(GetLastError()));
		return false;
	}

	// Async unpack: a copy queue on the display GPU. Anything missing here is not fatal;
	// the serialised path (unpack on the direct queue) still works.
	if (asyncUnpack) {
		const auto fallBack = [this](const char* a_what, HRESULT a_hr) {
			REX::WARN("[FG-GPU1] {} failed (hr={:#010x}); unpacking on the display GPU's direct queue instead",
				a_what, static_cast<std::uint32_t>(a_hr));
			asyncUnpack = false;
			listB = nullptr;
			for (auto& allocator : allocatorsB)
				allocator = nullptr;
			copyQueueB = nullptr;
			unpackFenceB = nullptr;
			readFenceB = nullptr;
		};
		HRESULT hr = deviceB->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(copyQueueB.put()));
		if (FAILED(hr)) {
			fallBack("copy queue on the display GPU", hr);
		} else {
			copyQueueB->SetName(L"FG-GPU1 unpack (display GPU copy queue)");
			for (auto& allocator : allocatorsB) {
				if (SUCCEEDED(hr))
					hr = deviceB->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(allocator.put()));
			}
			if (SUCCEEDED(hr))
				hr = deviceB->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, allocatorsB[0].get(), nullptr,
					IID_PPV_ARGS(listB.put()));
			if (SUCCEEDED(hr))
				hr = listB->Close();
			if (SUCCEEDED(hr))
				hr = deviceB->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(unpackFenceB.put()));
			if (SUCCEEDED(hr))
				hr = deviceB->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(readFenceB.put()));
			if (FAILED(hr))
				fallBack("display GPU copy list/fence", hr);
		}
	}

	// Timestamps are optional; the transport works without them.
	if (!timerCopyA.Create(deviceA.get(), copyQueueA.get(), true, 6))
		REX::INFO("[FG-GPU1] render GPU copy-queue timestamps unavailable; copy time will not be reported");
	if (asyncUnpack && !timerCopyB.Create(deviceB.get(), copyQueueB.get(), true, 6))
		REX::INFO("[FG-GPU1] display GPU copy-queue timestamps unavailable; unpack time will not be reported");

	QueryPerformanceFrequency(&qpcFrequency);
	QueryPerformanceCounter(&statLastLog);

	// bDirectGPUCopy: the display GPU's Vulkan side, and the check values A writes behind each lane.
	// Lanes get their blocks in AddLane; any failure there turns the direct copy off for the session.
	if (a_directCopy) {
		if (!asyncUnpack) {
			REX::WARN("[FG-GPU1] bDirectGPUCopy needs bAsyncUnpack = 1; frames cross through system memory");
		} else if (peer.Initialize(deviceB.get(), slotCount)) {
			D3D12_HEAP_PROPERTIES uploadProps{};
			uploadProps.Type = D3D12_HEAP_TYPE_UPLOAD;
			D3D12_RESOURCE_DESC uploadDesc{};
			uploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			uploadDesc.Width = kCheckStride * kMaxLanes * kSlots;
			uploadDesc.Height = 1;
			uploadDesc.DepthOrArraySize = 1;
			uploadDesc.MipLevels = 1;
			uploadDesc.SampleDesc.Count = 1;
			uploadDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			HRESULT hr = deviceA->CreateCommittedResource(&uploadProps, D3D12_HEAP_FLAG_NONE, &uploadDesc,
				D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(checkUploadA.put()));
			if (SUCCEEDED(hr))
				hr = checkUploadA->Map(0, nullptr, reinterpret_cast<void**>(&checkCpu));
			if (FAILED(hr)) {
				REX::ERROR("[FG-GPU1] direct GPU copy: the check-value buffer failed (hr={:#010x}); frames cross through "
						   "system memory instead",
					static_cast<std::uint32_t>(hr));
				DisableDirectCopy();
			} else if (!InstallExitHooks()) {
				// Without a clean release at exit the render GPU would still hold the blocks when Windows
				// tears the process down; that is what hung the PC on Alt+F4 with the first build.
				REX::ERROR("[FG-GPU1] direct GPU copy: no safe release at exit; frames cross through system memory instead");
				DisableDirectCopy();
			} else {
				directCopy = true;
			}
		}
	}

	REX::INFO("[FG-GPU1] bridge ready: render GPU copy queue -> {} -> {}",
		directCopy ? "the display GPU's VRAM, directly (bDirectGPUCopy=1)" : "cross-adapter heap in system memory",
		asyncUnpack ? "display GPU copy queue, lane by lane (bAsyncUnpack=1)" :
					  "display GPU direct queue, after the whole transfer (bAsyncUnpack=0)");
	return true;
}

void CrossAdapterBridge::SetDisplayQueue(ID3D12CommandQueue* a_queueB)
{
	if (!active || !a_queueB)
		return;
	displayQueue = a_queueB;
	displayDoneValue = 0;
	slotDisplayDone = {};
	displayDoneFenceB = nullptr;
	const HRESULT hr = deviceB->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(displayDoneFenceB.put()));
	if (FAILED(hr)) {
		displayDoneFenceB = nullptr;
		REX::WARN("[FG-GPU1] display-list fence failed (hr={:#010x}); timings of the display GPU's list are off",
			static_cast<std::uint32_t>(hr));
	}
	if (!timerDisplay.Create(deviceB.get(), a_queueB, false, 5))
		REX::INFO("[FG-GPU1] display GPU direct-queue timestamps unavailable");
}

int CrossAdapterBridge::AddLane(const char* a_name, const std::array<ID3D12Resource*, kSlots>& a_sources,
	bool a_createMirror, LaneTiming a_timing)
{
	if (!active)
		return -1;
	for (UINT slot = 0; slot < slotCount; ++slot) {
		if (!a_sources[slot]) {
			Fail("AddLane (null source)", E_POINTER);
			return -1;
		}
	}
	if (lanes.size() >= kMaxLanes) {
		Fail("AddLane (too many lanes)", E_INVALIDARG);
		return -1;
	}

	Lane lane;
	lane.name = a_name;
	lane.timing = a_timing;
	for (UINT slot = 0; slot < slotCount; ++slot)
		lane.sourceA[slot].copy_from(a_sources[slot]);
	lane.desc = a_sources[0]->GetDesc();

	if (lane.desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || lane.desc.MipLevels != 1 ||
		lane.desc.DepthOrArraySize != 1 || lane.desc.SampleDesc.Count != 1) {
		REX::CRITICAL("[FG-GPU1] lane '{}' is not a single-subresource 2D texture", lane.name);
		Fail("AddLane (unsupported texture)", E_INVALIDARG);
		return -1;
	}

	D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprintA{};
	D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprintB{};
	UINT64 totalA = 0;
	UINT64 totalB = 0;
	deviceA->GetCopyableFootprints(&lane.desc, 0, 1, 0, &footprintA, nullptr, nullptr, &totalA);
	deviceB->GetCopyableFootprints(&lane.desc, 0, 1, 0, &footprintB, nullptr, nullptr, &totalB);
	if (footprintA.Footprint.RowPitch != footprintB.Footprint.RowPitch || totalA != totalB || totalA == 0) {
		REX::CRITICAL("[FG-GPU1] lane '{}': footprints differ between GPUs (pitch {} vs {}, size {} vs {})",
			lane.name, footprintA.Footprint.RowPitch, footprintB.Footprint.RowPitch, totalA, totalB);
		Fail("AddLane (footprint mismatch)", E_FAIL);
		return -1;
	}
	lane.footprint = footprintA;
	lane.bytes = totalA;
	// bDirectGPUCopy: room for the check value behind the data. Both paths use the same layout, so a
	// frame can cross either way.
	lane.checkOffset = AlignUp(totalA, kCheckStride);
	lane.slotStride = AlignUp(directCopy ? lane.checkOffset + kCheckStride : totalA, kHeapAlignment);

	const UINT64 heapSize = lane.slotStride * slotCount;
	HRESULT hr = CreateSharedHeapPair(heapSize, lane.heapA, lane.heapB);
	if (FAILED(hr)) {
		Fail("cross-adapter heap (CreateHeap SHARED | SHARED_CROSS_ADAPTER / OpenSharedHandle)", hr);
		return -1;
	}

	D3D12_RESOURCE_DESC bufferDesc{};
	bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	bufferDesc.Width = heapSize;
	bufferDesc.Height = 1;
	bufferDesc.DepthOrArraySize = 1;
	bufferDesc.MipLevels = 1;
	bufferDesc.SampleDesc.Count = 1;
	bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	bufferDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;

	hr = deviceA->CreatePlacedResource(lane.heapA.get(), 0, &bufferDesc, D3D12_RESOURCE_STATE_COMMON, nullptr,
		IID_PPV_ARGS(lane.stagingA.put()));
	if (FAILED(hr)) {
		Fail("CreatePlacedResource(staging) on the render GPU", hr);
		return -1;
	}
	hr = deviceB->CreatePlacedResource(lane.heapB.get(), 0, &bufferDesc, D3D12_RESOURCE_STATE_COMMON, nullptr,
		IID_PPV_ARGS(lane.stagingB.put()));
	if (FAILED(hr)) {
		Fail("CreatePlacedResource(staging) on the secondary GPU", hr);
		return -1;
	}

	if (a_createMirror) {
		D3D12_RESOURCE_DESC mirrorDesc = lane.desc;
		mirrorDesc.Alignment = 0;
		mirrorDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
		mirrorDesc.Flags &= ~(D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER);
		D3D12_HEAP_PROPERTIES defaultHeap{};
		defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
		for (UINT slot = 0; slot < slotCount; ++slot) {
			hr = deviceB->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &mirrorDesc,
				D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(lane.mirrorB[slot].put()));
			if (FAILED(hr)) {
				Fail("CreateCommittedResource(mirror) on the secondary GPU", hr);
				return -1;
			}
		}
	}

	// bDirectGPUCopy: the lane's block in B's VRAM. A lane that cannot have one turns the direct copy
	// off for every lane (a frame crosses one way or the other as a whole).
	if (directCopy && !AddDirectLane(lane, heapSize))
		DisableDirectCopy();

	bytesPerFrame += lane.bytes;
	REX::INFO("[FG-GPU1] lane[{}] '{}' {}x{} format={} pitch={} bytes={} ({:.1f} MB/frame total) timing={}{}{}",
		lanes.size(), lane.name, lane.desc.Width, lane.desc.Height, static_cast<int>(lane.desc.Format),
		lane.footprint.Footprint.RowPitch, lane.bytes, static_cast<double>(bytesPerFrame) / (1024.0 * 1024.0),
		TimingName(a_timing), a_createMirror ? "" : " (unpacked into the back buffer)",
		lane.peerLane >= 0 ? " (direct to the display GPU's VRAM)" : "");

	lanes.push_back(std::move(lane));
	return static_cast<int>(lanes.size() - 1);
}

// ---------------------------------------------------------------------------------------
// bDirectGPUCopy

bool CrossAdapterBridge::AddDirectLane(Lane& a_lane, UINT64 a_heapSize)
{
	const auto failed = [&](const char* a_what, HRESULT a_hr) {
		REX::ERROR("[FG-GPU1] direct GPU copy: lane '{}': {} failed (hr={:#010x}); every lane crosses through system "
				   "memory instead",
			a_lane.name, a_what, static_cast<std::uint32_t>(a_hr));
		a_lane.peerStagingA = nullptr;
		a_lane.peerHeapA = nullptr;  // before the block it was opened on goes (DisableDirectCopy)
		a_lane.landingB = nullptr;
		a_lane.landingHeapB = nullptr;
		a_lane.peerMapped = nullptr;
		a_lane.peerLane = -1;
		return false;
	};

	void* mapped = nullptr;
	const int peerLane = peer.AddLane(a_lane.name.c_str(), a_heapSize, a_lane.slotStride, a_lane.bytes, mapped,
		a_lane.landingHeapB);
	if (peerLane < 0 || !mapped || !a_lane.landingHeapB)
		return failed("the display GPU's VRAM block (Vulkan)", E_FAIL);
	a_lane.peerMapped = static_cast<const volatile std::uint8_t*>(mapped);

	// The render GPU opens the block's CPU address as if it were system memory; its copy engine's writes
	// then go to the display GPU's BAR.
	winrt::com_ptr<ID3D12Device3> deviceA3;
	HRESULT hr = deviceA->QueryInterface(IID_PPV_ARGS(deviceA3.put()));
	if (SUCCEEDED(hr))
		hr = deviceA3->OpenExistingHeapFromAddress(mapped, IID_PPV_ARGS(a_lane.peerHeapA.put()));
	if (FAILED(hr))
		return failed("OpenExistingHeapFromAddress on the render GPU", hr);
	const D3D12_HEAP_DESC peerDesc = a_lane.peerHeapA->GetDesc();
	if (peerDesc.SizeInBytes < a_heapSize)
		return failed("the render GPU's view of the block (too small)", E_FAIL);

	D3D12_RESOURCE_DESC bufferDesc{};
	bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	bufferDesc.Width = a_heapSize;
	bufferDesc.Height = 1;
	bufferDesc.DepthOrArraySize = 1;
	bufferDesc.MipLevels = 1;
	bufferDesc.SampleDesc.Count = 1;
	bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	bufferDesc.Flags = (peerDesc.Flags & D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER) ? D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER :
	                                                                             D3D12_RESOURCE_FLAG_NONE;
	hr = deviceA->CreatePlacedResource(a_lane.peerHeapA.get(), 0, &bufferDesc, D3D12_RESOURCE_STATE_COMMON, nullptr,
		IID_PPV_ARGS(a_lane.peerStagingA.put()));
	if (FAILED(hr))
		return failed("the render GPU's buffer on the block", hr);

	bufferDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
	hr = deviceB->CreatePlacedResource(a_lane.landingHeapB.get(), 0, &bufferDesc, D3D12_RESOURCE_STATE_COMMON, nullptr,
		IID_PPV_ARGS(a_lane.landingB.put()));
	if (FAILED(hr))
		return failed("the landing buffer on the display GPU", hr);

	a_lane.peerLane = peerLane;
	return true;
}

void CrossAdapterBridge::DisableDirectCopy()
{
	directCopy = false;
	// Before any frame ran, the blocks can go now (the render GPU's heaps on them first). Once frames
	// have used them, they stay until Shutdown: copies may still be in flight.
	if (transferValue != 0)
		return;
	for (auto& lane : lanes) {
		lane.peerStagingA = nullptr;
		lane.peerHeapA = nullptr;
		lane.landingB = nullptr;
		lane.landingHeapB = nullptr;
		lane.peerMapped = nullptr;
		lane.peerLane = -1;
	}
	peer.Shutdown();
	checkCpu = nullptr;
	checkUploadA = nullptr;
}

void CrossAdapterBridge::ReleaseDirectCopyForExit(const char* a_why) noexcept
{
	// On the render thread (where FO4 quits) the lock is free or already ours; on another thread wait for
	// the frame being assembled to finish. If that takes over 2 s, leave things alone rather than pull
	// the heaps out from under it.
	std::unique_lock<std::recursive_timed_mutex> lock(frameMutex, std::defer_lock);
	if (!lock.try_lock_for(std::chrono::seconds(2))) {
		REX::WARN("[FG-GPU1] direct GPU copy: {}, but the render thread kept the bridge for 2 s; not released", a_why);
		return;
	}

	bool open = false;
	for (const auto& lane : lanes)
		open |= lane.peerHeapA != nullptr || lane.landingHeapB != nullptr;
	if (!open && !peer.Ready())
		return;
	directCopy = false;

	// Everything the GPUs were given: the render GPU's lane copies (each signals the transfer fence), and
	// the display GPU's reads (each waited for its Vulkan copy before it ran, then signalled readFenceB).
	bool idle = true;
	if (transferFenceA && nextLaneValue)
		idle &= SUCCEEDED(WaitCpu(transferFenceA.get(), nextLaneValue));
	if (readFenceB && readValue)
		idle &= SUCCEEDED(WaitCpu(readFenceB.get(), readValue));

	// The render GPU's views first (this ends its lock on the blocks' pages), then the display GPU's side.
	for (auto& lane : lanes) {
		lane.peerStagingA = nullptr;
		lane.peerHeapA = nullptr;
	}
	for (auto& lane : lanes) {
		lane.landingB = nullptr;
		lane.landingHeapB = nullptr;
		lane.peerMapped = nullptr;
		lane.peerLane = -1;
		lane.checkPending = {};
	}
	// The Vulkan side (it waits for its own queue). If the GPUs were not idle its copies may still read
	// the blocks: those are then left to Windows' teardown, which is safe now that the render GPU has let go.
	if (idle)
		peer.Shutdown();
	checkCpu = nullptr;
	checkUploadA = nullptr;
	REX::INFO("[FG-GPU1] direct GPU copy: {}; closed the render GPU's views of the display GPU's VRAM, then {}", a_why,
		idle ? "freed the blocks" : "left the blocks to Windows (the GPUs were still busy after 2 s)");
}

void CrossAdapterBridge::TripDirectCopy(const std::string& a_why)
{
	if (directCopyTripped)
		return;
	directCopyTripped = true;
	directCopy = false;
	REX::CRITICAL("[FG-GPU1] direct GPU copy guard: {}. Frames cross through system memory from the next frame on and "
				  "frame generation is OFF for the rest of the session. Restart the game; set bDirectGPUCopy = 0 if this "
				  "happens again.",
		a_why);
}

void CrossAdapterBridge::CheckDirectCopy(UINT a_reusedSlot)
{
	const UINT64 finished = transferFenceA->GetCompletedValue();
	for (auto& lane : lanes) {
		if (!lane.peerMapped)
			continue;
		for (UINT slot = 0; slot < slotCount; ++slot) {
			const UINT64 expected = lane.checkPending[slot];
			if (!expected || expected > finished)
				continue;  // nothing to check, or that copy is still running
			// An uncached read through the block's CPU mapping (~1-2 us): it reaches the block wherever the
			// display GPU's driver keeps it now, while the render GPU wrote to where it was when opened.
			const auto* where =
				reinterpret_cast<const volatile UINT64*>(lane.peerMapped + lane.slotStride * slot + lane.checkOffset);
			const UINT64 seen = *where;
			if (seen == expected) {
				lane.checkPending[slot] = 0;
				++statDirectChecks;
				continue;
			}
			// The p2p-probe never saw a value arrive after its fence; still, one frame's grace, except for the
			// slot about to be overwritten, whose copy PrepareFrame has just waited for.
			if (slot != a_reusedSlot && !lane.checkLate[slot]) {
				lane.checkLate[slot] = true;
				++statDirectLate;
				continue;
			}
			TripDirectCopy(std::format("lane '{}' slot {}: the render GPU wrote check value {} into the display GPU's "
									   "VRAM block, but the block holds {} - it is no longer where the render GPU writes",
				lane.name, slot, expected, seen));
			return;
		}
	}
}

HRESULT CrossAdapterBridge::QueueDirectCopy(const Lane& a_lane, UINT a_slot)
{
	ID3D12Fence* fence = peer.Fence();
	const UINT64 landed = ++peerValue;
	const UINT64 copied = ++peerValue;
	HRESULT hr = fence ? copyQueueB->Signal(fence, landed) : E_POINTER;
	if (FAILED(hr)) {
		Fail("direct copy: landed Signal", hr);
		return hr;
	}
	// Nothing would signal "copied" if the Vulkan submission failed: this queue does it, so it carries on
	// (the lane shows stale data this once) and the guard sends the next frames through system memory.
	if (directCopyTripped || !peer.SubmitCopy(a_lane.peerLane, a_slot, landed, copied)) {
		TripDirectCopy(std::format("the display GPU's Vulkan copy of lane '{}' could not be submitted", a_lane.name));
		hr = copyQueueB->Signal(fence, copied);
	}
	if (SUCCEEDED(hr))
		hr = copyQueueB->Wait(fence, copied);
	if (FAILED(hr))
		Fail("direct copy: copied Signal/Wait", hr);
	return hr;
}

ID3D12Resource* CrossAdapterBridge::Mirror(int a_lane, UINT a_slot) const noexcept
{
	if (a_lane < 0 || static_cast<size_t>(a_lane) >= lanes.size() || a_slot >= kSlots)
		return nullptr;
	return lanes[static_cast<size_t>(a_lane)].mirrorB[a_slot].get();
}

// ---------------------------------------------------------------------------------------
// Per frame

HRESULT CrossAdapterBridge::PrepareFrame(UINT a_slot)
{
	if (frameStarted) {
		if (frameSlot == a_slot)
			return S_OK;
		REX::WARN("[FG-GPU1] frame slot changed from {} to {} mid-frame; the unfinished frame is dropped",
			frameSlot, a_slot);
	}

	// The allocator and timestamps of this slot were last used by frame slotTransfer[a_slot].
	const UINT64 previous = slotTransfer[a_slot];
	if (previous) {
		const HRESULT hr = WaitCpu(copyDoneFenceA.get(), previous);
		if (FAILED(hr)) {
			Fail("waiting for the previous copy of this slot", hr);
			return hr;
		}
		CollectTimings(a_slot);
	}

	// bDirectGPUCopy: the guard looks at what the finished copies left behind, then this frame's path
	// is fixed. A frame never switches path half-way.
	if (directCopy)
		CheckDirectCopy(a_slot);
	frameDirect = directCopy;

	const UINT64 frame = ++transferValue;
	HRESULT hr = allocatorsA[a_slot]->Reset();
	if (FAILED(hr)) {
		Fail("resetting the copy allocator", hr);
		return hr;
	}

	// The display GPU finished reading this slot's staging area the last time round.
	if (previous) {
		hr = copyQueueA->Wait(consumedFenceA.get(), previous);
		if (FAILED(hr)) {
			Fail("copy queue Wait (consumed)", hr);
			return hr;
		}
	}

	currentSlot = a_slot;
	frameSlot = a_slot;
	frameStarted = true;
	slotTransfer[a_slot] = frame;
	return S_OK;
}

HRESULT CrossAdapterBridge::SubmitLane(Lane& a_lane, size_t a_laneIndex, UINT a_slot, UINT a_markBase,
	bool a_firstInPhase, bool a_lastInPhase)
{
	auto* allocator = allocatorsA[a_slot].get();
	HRESULT hr = listA->Reset(allocator, nullptr);
	if (FAILED(hr)) {
		Fail("resetting the copy list", hr);
		return hr;
	}

	if (a_firstInPhase)
		timerCopyA.Mark(listA.get(), a_slot, a_markBase);

	// bDirectGPUCopy: into the display GPU's VRAM block instead of the cross-adapter heap.
	const bool direct = frameDirect && a_lane.peerStagingA;
	ID3D12Resource* staging = direct ? a_lane.peerStagingA.get() : a_lane.stagingA.get();

	D3D12_TEXTURE_COPY_LOCATION dst{};
	dst.pResource = staging;
	dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	dst.PlacedFootprint = a_lane.footprint;
	dst.PlacedFootprint.Offset = a_lane.slotStride * a_slot;

	D3D12_TEXTURE_COPY_LOCATION src{};
	src.pResource = a_lane.sourceA[a_slot].get();
	src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	src.SubresourceIndex = 0;

	// Sources are D3D11-shared textures in COMMON; a copy queue promotes them to
	// COPY_SOURCE implicitly and they decay back when the list completes.
	listA->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

	// Values are allocated in submission order rather than per lane index: the early groups
	// signal before the Present group, and a shared fence must never go backwards.
	const UINT64 value = nextLaneValue + 1;

	// The guard's check value: this lane's transfer value, behind the data. The upload slot was last
	// read by this slot's previous copy, which PrepareFrame waited for.
	if (direct) {
		const UINT64 uploadOffset = kCheckStride * (a_laneIndex * kSlots + a_slot);
		checkCpu[uploadOffset / sizeof(UINT64)] = value;
		listA->CopyBufferRegion(staging, a_lane.slotStride * a_slot + a_lane.checkOffset, checkUploadA.get(),
			uploadOffset, sizeof(UINT64));
		a_lane.checkPending[a_slot] = value;
		a_lane.checkLate[a_slot] = false;
	}

	if (a_lastInPhase) {
		timerCopyA.Mark(listA.get(), a_slot, a_markBase + 1);
		// The Present phase is always last in the frame, so it resolves the slot's queries.
		if (a_markBase == kMarkPresent)
			timerCopyA.Resolve(listA.get(), a_slot);
	}

	hr = listA->Close();
	if (FAILED(hr)) {
		Fail("closing the copy list", hr);
		return hr;
	}
	ID3D12CommandList* lists[] = { listA.get() };
	copyQueueA->ExecuteCommandLists(1, lists);

	nextLaneValue = value;
	hr = copyQueueA->Signal(transferFenceA.get(), value);
	if (FAILED(hr)) {
		Fail("transfer fence Signal", hr);
		return hr;
	}
	laneValue[a_laneIndex] = value;
	laneFrame[a_laneIndex] = transferValue;
	return S_OK;
}

bool CrossAdapterBridge::HasLanes(LaneTiming a_timing) const noexcept
{
	for (const auto& lane : lanes)
		if (lane.timing == a_timing)
			return true;
	return false;
}

HRESULT CrossAdapterBridge::TransferGroup(ID3D12Fence* a_producerFenceA, UINT64 a_producerValue, UINT a_slot,
	LaneTiming a_timing)
{
	if (!active)
		return E_FAIL;
	if (a_slot >= kSlots || !a_producerFenceA) {
		Fail("TransferGroup (bad arguments)", E_INVALIDARG);
		return E_INVALIDARG;
	}
	if (!HasLanes(a_timing))
		return S_OK;  // e.g. the HUD-less group with bSendHudlessFrame = 0

	LARGE_INTEGER submitStart{};
	QueryPerformanceCounter(&submitStart);

	HRESULT hr = PrepareFrame(a_slot);
	if (FAILED(hr))
		return hr;

	// The game's D3D11 work that produced these lanes, as of the hook that just ran.
	hr = copyQueueA->Wait(a_producerFenceA, a_producerValue);
	if (FAILED(hr)) {
		Fail("copy queue Wait (producer)", hr);
		return hr;
	}

	const UINT markBase = a_timing == LaneTiming::kWorld ? kMarkWorld :
		a_timing == LaneTiming::kHudless                  ? kMarkHudless :
															kMarkPresent;
	size_t firstIndex = lanes.size();
	size_t lastIndex = 0;
	for (size_t index = 0; index < lanes.size(); ++index) {
		if (lanes[index].timing != a_timing || laneFrame[index] == transferValue)
			continue;
		if (firstIndex == lanes.size())
			firstIndex = index;
		lastIndex = index;
	}

	bool copied = false;
	for (size_t index = 0; index < lanes.size(); ++index) {
		if (lanes[index].timing != a_timing || laneFrame[index] == transferValue)
			continue;
		hr = SubmitLane(lanes[index], index, a_slot, markBase, index == firstIndex, index == lastIndex);
		if (FAILED(hr))
			return hr;
		copied = true;
	}

	if (copied) {
		LARGE_INTEGER submitEnd{};
		QueryPerformanceCounter(&submitEnd);
		statCpuSubmitMs += QpcMs(submitStart, submitEnd, qpcFrequency);
	}
	return S_OK;
}

HRESULT CrossAdapterBridge::Transfer(ID3D12Fence* a_producerFenceA, UINT64 a_producerValue, UINT a_slot)
{
	if (!active)
		return E_FAIL;
	if (a_slot >= kSlots || !a_producerFenceA || lanes.empty()) {
		Fail("Transfer (bad arguments or no lanes)", E_INVALIDARG);
		return E_INVALIDARG;
	}

	LARGE_INTEGER submitStart{};
	QueryPerformanceCounter(&submitStart);

	HRESULT hr = PrepareFrame(a_slot);
	if (FAILED(hr))
		return hr;

	// The finished frame is only final now. A frame whose early hooks did not run also copies
	// its remaining lanes here, so the transport never depends on them having run.
	hr = copyQueueA->Wait(a_producerFenceA, a_producerValue);
	if (FAILED(hr)) {
		Fail("copy queue Wait (producer)", hr);
		return hr;
	}

	// Whatever is left crosses in the Present phase, including early-timing lanes whose hooks
	// did not run; the last one resolves the slot's copy timestamps.
	size_t firstIndex = lanes.size();
	size_t lastIndex = 0;
	for (size_t index = 0; index < lanes.size(); ++index) {
		if (laneFrame[index] == transferValue)
			continue;
		if (firstIndex == lanes.size())
			firstIndex = index;
		lastIndex = index;
	}

	for (size_t index = 0; index < lanes.size(); ++index) {
		if (laneFrame[index] == transferValue)
			continue;
		hr = SubmitLane(lanes[index], index, a_slot, kMarkPresent, index == firstIndex, index == lastIndex);
		if (FAILED(hr))
			return hr;
	}

	hr = copyQueueA->Signal(copyDoneFenceA.get(), transferValue);
	if (FAILED(hr)) {
		Fail("copy-done fence Signal", hr);
		return hr;
	}

	if ((statFrames & 63) == 0)
		CalibrateTimers();

	frameStarted = false;
	slotPresentQpc[a_slot] = submitStart;
	if (frameDirect)
		++statDirectFrames;

	LARGE_INTEGER submitEnd{};
	QueryPerformanceCounter(&submitEnd);
	statCpuSubmitMs += QpcMs(submitStart, submitEnd, qpcFrequency);
	++statFrames;
	const UINT64 consumed = consumedFenceA->GetCompletedValue();
	statMaxLag = (std::max)(statMaxLag, transferValue > consumed ? transferValue - consumed : 0);
	return S_OK;
}

namespace
{
	// The format family a copy keeps the bits of: copies may change the type (UNORM/SRGB/TYPELESS)
	// but never convert, so RGBA8 -> R10G10B10A2 (same size per pixel) is not a copy.
	DXGI_FORMAT CopyFamily(DXGI_FORMAT a_format)
	{
		switch (a_format) {
		case DXGI_FORMAT_R8G8B8A8_TYPELESS:
		case DXGI_FORMAT_R8G8B8A8_UNORM:
		case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
		case DXGI_FORMAT_R8G8B8A8_UINT:
		case DXGI_FORMAT_R8G8B8A8_SNORM:
		case DXGI_FORMAT_R8G8B8A8_SINT:
			return DXGI_FORMAT_R8G8B8A8_TYPELESS;
		case DXGI_FORMAT_B8G8R8A8_TYPELESS:
		case DXGI_FORMAT_B8G8R8A8_UNORM:
		case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
			return DXGI_FORMAT_B8G8R8A8_TYPELESS;
		case DXGI_FORMAT_R10G10B10A2_TYPELESS:
		case DXGI_FORMAT_R10G10B10A2_UNORM:
		case DXGI_FORMAT_R10G10B10A2_UINT:
			return DXGI_FORMAT_R10G10B10A2_TYPELESS;
		case DXGI_FORMAT_R16G16B16A16_TYPELESS:
		case DXGI_FORMAT_R16G16B16A16_FLOAT:
		case DXGI_FORMAT_R16G16B16A16_UNORM:
		case DXGI_FORMAT_R16G16B16A16_UINT:
		case DXGI_FORMAT_R16G16B16A16_SNORM:
		case DXGI_FORMAT_R16G16B16A16_SINT:
			return DXGI_FORMAT_R16G16B16A16_TYPELESS;
		case DXGI_FORMAT_R16G16_TYPELESS:
		case DXGI_FORMAT_R16G16_FLOAT:
		case DXGI_FORMAT_R16G16_UNORM:
		case DXGI_FORMAT_R16G16_UINT:
		case DXGI_FORMAT_R16G16_SNORM:
		case DXGI_FORMAT_R16G16_SINT:
			return DXGI_FORMAT_R16G16_TYPELESS;
		case DXGI_FORMAT_R32_TYPELESS:
		case DXGI_FORMAT_R32_FLOAT:
		case DXGI_FORMAT_R32_UINT:
		case DXGI_FORMAT_R32_SINT:
			return DXGI_FORMAT_R32_TYPELESS;
		default:
			return a_format;
		}
	}
}

bool CrossAdapterBridge::RecordLaneCopy(ID3D12GraphicsCommandList* a_list, Lane& a_lane, UINT a_slot,
	ID3D12Resource* a_dest, D3D12_RESOURCE_STATES a_destState, ID3D12Resource* a_source)
{
	const D3D12_RESOURCE_DESC destDesc = a_dest->GetDesc();
	D3D12_PLACED_SUBRESOURCE_FOOTPRINT destFootprint{};
	deviceB->GetCopyableFootprints(&destDesc, 0, 1, 0, &destFootprint, nullptr, nullptr, nullptr);
	if (destDesc.Width != a_lane.desc.Width || destDesc.Height != a_lane.desc.Height ||
		destFootprint.Footprint.RowPitch != a_lane.footprint.Footprint.RowPitch ||
		CopyFamily(destDesc.Format) != CopyFamily(a_lane.desc.Format)) {
		// e.g. an HDR add-on changed the swap chain to R10G10B10A2 (same pitch, so only the format
		// check catches it) or a 64-bit format. SecondaryUpscale converts those; this copy would
		// only reinterpret the bits.
		if (!a_lane.mismatchLogged) {
			REX::ERROR("[FG-GPU1] lane '{}' is {}x{} format {} but its destination is {}x{} format {}; skipping the copy",
				a_lane.name, a_lane.desc.Width, a_lane.desc.Height, static_cast<int>(a_lane.desc.Format), destDesc.Width,
				destDesc.Height, static_cast<int>(destDesc.Format));
			a_lane.mismatchLogged = true;
		}
		return false;
	}

	// COMMON (== PRESENT) <-> COPY_DEST is also legal on a copy list.
	auto barrier = Transition(a_dest, a_destState, D3D12_RESOURCE_STATE_COPY_DEST);
	a_list->ResourceBarrier(1, &barrier);

	D3D12_TEXTURE_COPY_LOCATION dst{};
	dst.pResource = a_dest;
	dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	dst.SubresourceIndex = 0;

	D3D12_TEXTURE_COPY_LOCATION src{};
	src.pResource = a_source;
	src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	src.PlacedFootprint = a_lane.footprint;
	src.PlacedFootprint.Offset = a_lane.slotStride * a_slot;
	// Same width/height; a typeless/UNORM/SRGB difference between the back buffer and the
	// D3D11 texture is allowed by CopyTextureRegion as long as the formats are copy-compatible.
	src.PlacedFootprint.Footprint.Format = destDesc.Format;

	a_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

	barrier = Transition(a_dest, D3D12_RESOURCE_STATE_COPY_DEST, a_destState);
	a_list->ResourceBarrier(1, &barrier);
	return true;
}

HRESULT CrossAdapterBridge::Unpack(UINT a_slot, int a_backBufferLane, ID3D12Resource* a_backBuffer,
	ID3D12CommandQueue* a_queueB)
{
	if (!active)
		return E_FAIL;
	if (!asyncUnpack || !copyQueueB || a_slot >= kSlots || !a_queueB || lanes.empty() ||
		slotTransfer[a_slot] != transferValue) {
		Fail("Unpack (bad arguments)", E_INVALIDARG);
		return E_INVALIDARG;
	}

	LARGE_INTEGER submitStart{};
	QueryPerformanceCounter(&submitStart);

	const UINT64 frame = transferValue;
	HRESULT hr = BeginUnpack(a_slot);
	if (FAILED(hr))
		return hr;

	// Whatever the early hooks did not already read, in the order the lanes become available:
	// the finished frame is registered first (at interop creation) but is the last to cross.
	std::array<size_t, kMaxLanes> order{};
	size_t orderCount = 0;
	for (size_t pass = 0; pass < 2; ++pass) {
		for (size_t index = 0; index < lanes.size(); ++index) {
			const bool present = lanes[index].timing == LaneTiming::kPresent;
			if (present != (pass == 1) || laneUnpackFrame[index] == frame)
				continue;
			order[orderCount++] = index;
		}
	}
	hr = SubmitUnpackLanes(a_slot, order.data(), orderCount, kMarkPresent, true, a_backBufferLane, a_backBuffer);
	if (FAILED(hr))
		return hr;

	// The staging slot has been read: the render GPU may overwrite it. Then let the direct
	// queue (FFX prepare, present) start on this frame - after the depth rebuild, which is
	// queued ahead of that wait so it only waits for the depth lane.
	hr = copyQueueB->Signal(consumedFenceB.get(), frame);
	if (SUCCEEDED(hr))
		hr = copyQueueB->Signal(unpackFenceB.get(), frame);
	if (SUCCEEDED(hr) && upsampler.Ready())
		hr = SubmitDepthUpsample(a_slot, frame, a_queueB);
	if (SUCCEEDED(hr))
		hr = a_queueB->Wait(unpackFenceB.get(), frame);
	if (FAILED(hr)) {
		Fail("unpack fence Signal/Wait", hr);
		return hr;
	}
	unpackStarted = false;

	LARGE_INTEGER submitEnd{};
	QueryPerformanceCounter(&submitEnd);
	statCpuSubmitMs += QpcMs(submitStart, submitEnd, qpcFrequency);
	return S_OK;
}

HRESULT CrossAdapterBridge::UnpackGroup(UINT a_slot, LaneTiming a_timing)
{
	if (!active || !asyncUnpack || !copyQueueB || a_timing == LaneTiming::kPresent)
		return S_OK;
	if (a_slot >= kSlots || slotTransfer[a_slot] != transferValue)
		return S_OK;  // no frame in progress on this slot; Present reads everything

	const UINT64 frame = transferValue;
	std::array<size_t, kMaxLanes> order{};
	size_t orderCount = 0;
	for (size_t index = 0; index < lanes.size(); ++index) {
		// Only lanes this frame has already sent and nobody has read yet.
		if (lanes[index].timing != a_timing || laneFrame[index] != frame || laneUnpackFrame[index] == frame)
			continue;
		order[orderCount++] = index;
	}
	if (!orderCount)
		return S_OK;

	LARGE_INTEGER submitStart{};
	QueryPerformanceCounter(&submitStart);

	HRESULT hr = BeginUnpack(a_slot);
	if (SUCCEEDED(hr)) {
		const UINT markBase = a_timing == LaneTiming::kWorld ? kMarkWorld : kMarkHudless;
		hr = SubmitUnpackLanes(a_slot, order.data(), orderCount, markBase, false, -1, nullptr);
	}

	LARGE_INTEGER submitEnd{};
	QueryPerformanceCounter(&submitEnd);
	statCpuSubmitMs += QpcMs(submitStart, submitEnd, qpcFrequency);
	return hr;
}

HRESULT CrossAdapterBridge::BeginUnpack(UINT a_slot)
{
	const UINT64 frame = transferValue;
	if (unpackStarted) {
		if (unpackFrame == frame)
			return S_OK;
		// A frame's read was started at a hook but its Present never came (the slot changed
		// mid-frame). Close it, so the next use of its slot does not wait for it forever.
		HRESULT hr = copyQueueB->Signal(consumedFenceB.get(), unpackFrame);
		if (SUCCEEDED(hr))
			hr = copyQueueB->Signal(unpackFenceB.get(), unpackFrame);
		if (FAILED(hr)) {
			Fail("closing an unfinished unpack", hr);
			return hr;
		}
		unpackStarted = false;
	}

	// This slot's allocator was last used by the unpack of frame slotUnpack[a_slot].
	if (slotUnpack[a_slot]) {
		const HRESULT hr = WaitCpu(unpackFenceB.get(), slotUnpack[a_slot]);
		if (FAILED(hr)) {
			Fail("waiting for the previous unpack of this slot", hr);
			return hr;
		}
	}
	HRESULT hr = allocatorsB[a_slot]->Reset();
	if (FAILED(hr)) {
		Fail("resetting the unpack allocator", hr);
		return hr;
	}

	// The frame that last used this slot may still read its mirrors on the direct queue
	// (FFX's prepare pass) or FFX's interpolation queue (HUD-less colour). Both are done
	// by now from the CPU's side: FFX's Present for the previous frame waited for that
	// frame's interpolation before this frame's hooks could run. This keeps it explicit.
	if (displayDoneFenceB && slotDisplayDone[a_slot]) {
		hr = copyQueueB->Wait(displayDoneFenceB.get(), slotDisplayDone[a_slot]);
		if (FAILED(hr)) {
			Fail("unpack queue Wait (display list)", hr);
			return hr;
		}
	}

	// From here on the slot's allocator belongs to this frame.
	slotUnpack[a_slot] = frame;
	unpackStarted = true;
	unpackFrame = frame;
	return S_OK;
}

HRESULT CrossAdapterBridge::SubmitUnpackLanes(UINT a_slot, const size_t* a_order, size_t a_count, UINT a_markBase,
	bool a_resolve, int a_backBufferLane, ID3D12Resource* a_backBuffer)
{
	auto* allocator = allocatorsB[a_slot].get();
	for (size_t position = 0; position < a_count; ++position) {
		const size_t index = a_order[position];
		auto& lane = lanes[index];
		ID3D12Resource* dest = lane.mirrorB[a_slot].get();
		if (!dest && static_cast<int>(index) == a_backBufferLane)
			dest = a_backBuffer;

		if (laneFrame[index] != unpackFrame) {
			Fail("Unpack (a lane was not transferred for this frame)", E_FAIL);
			return E_FAIL;
		}

		HRESULT hr = listB->Reset(allocator, nullptr);
		if (FAILED(hr)) {
			Fail("resetting the unpack list", hr);
			return hr;
		}
		if (position == 0)
			timerCopyB.Mark(listB.get(), a_slot, a_markBase);

		// bDirectGPUCopy: the lane arrives in its landing heap (local VRAM) instead of the cross-adapter heap.
		const bool direct = frameDirect && lane.landingB;

		// Mirrors are COMMON between uses; the back buffer is PRESENT, which is the same state.
		if (dest)
			RecordLaneCopy(listB.get(), lane, a_slot, dest, D3D12_RESOURCE_STATE_COMMON,
				direct ? lane.landingB.get() : lane.stagingB.get());

		if (position + 1 == a_count) {
			timerCopyB.Mark(listB.get(), a_slot, a_markBase + 1);
			// The Present phase is always the frame's last, so it resolves the slot's queries.
			if (a_resolve)
				timerCopyB.Resolve(listB.get(), a_slot);
		}

		hr = listB->Close();
		if (FAILED(hr)) {
			Fail("closing the unpack list", hr);
			return hr;
		}

		// Start on this lane as soon as it has crossed, while the next one is still in flight.
		hr = copyQueueB->Wait(transferFenceB.get(), laneValue[index]);
		if (FAILED(hr)) {
			Fail("unpack queue Wait (transfer)", hr);
			return hr;
		}
		if (direct) {
			hr = QueueDirectCopy(lane, a_slot);
			if (FAILED(hr))
				return hr;
		}
		ID3D12CommandList* lists[] = { listB.get() };
		copyQueueB->ExecuteCommandLists(1, lists);
		laneUnpackFrame[index] = unpackFrame;

		// FSR 4.1 on this GPU starts as soon as the lanes it reads are in (WaitForLaneReads).
		if (readFenceB) {
			hr = copyQueueB->Signal(readFenceB.get(), ++readValue);
			if (FAILED(hr)) {
				Fail("lane-read fence Signal", hr);
				return hr;
			}
			laneReadValue[index] = readValue;
		}

		// iHalfResDepthMV = 1: the depth rebuild may start now, while the other lanes cross.
		if (upsampler.Ready() && static_cast<int>(index) == upsampler.lane) {
			hr = copyQueueB->Signal(upsampler.sourceReadyFence.get(), unpackFrame);
			if (FAILED(hr)) {
				Fail("depth-ready fence Signal", hr);
				return hr;
			}
		}
	}
	return S_OK;
}

HRESULT CrossAdapterBridge::WaitForLaneReads(ID3D12CommandQueue* a_queue, const int* a_lanes, size_t a_count,
	bool& a_queued)
{
	a_queued = false;
	if (!active)
		return E_FAIL;
	if (!asyncUnpack || !copyQueueB || !readFenceB || !a_queue || !a_lanes)
		return E_INVALIDARG;

	// Only the frame being assembled: its lanes were sent (laneFrame) and their reads queued.
	UINT64 value = 0;
	for (size_t position = 0; position < a_count; ++position) {
		const int lane = a_lanes[position];
		if (lane < 0 || static_cast<size_t>(lane) >= lanes.size())
			return E_INVALIDARG;
		const auto index = static_cast<size_t>(lane);
		if (laneFrame[index] != transferValue || laneUnpackFrame[index] != transferValue)
			return S_OK;
		value = (std::max)(value, laneReadValue[index]);
	}

	const HRESULT hr = a_queue->Wait(readFenceB.get(), value);
	if (FAILED(hr)) {
		Fail("Wait for lane reads", hr);
		return hr;
	}
	a_queued = true;
	return S_OK;
}

HRESULT CrossAdapterBridge::WaitDisplayListDone(UINT a_slot)
{
	if (!active || a_slot >= kSlots || !displayDoneFenceB || !slotDisplayDone[a_slot])
		return S_OK;
	const HRESULT hr = WaitCpu(displayDoneFenceB.get(), slotDisplayDone[a_slot]);
	if (FAILED(hr))
		Fail("waiting for the slot's previous direct list", hr);
	return hr;
}

bool CrossAdapterBridge::DisplayListDone(UINT a_slot) const noexcept
{
	if (!active || a_slot >= kSlots || !displayDoneFenceB)
		return false;
	return displayDoneFenceB->GetCompletedValue() >= slotDisplayDone[a_slot];
}

HRESULT CrossAdapterBridge::WaitTransfer(ID3D12CommandQueue* a_queueB)
{
	if (!active)
		return E_FAIL;
	if (!a_queueB || lanes.empty()) {
		Fail("WaitTransfer (bad arguments)", E_INVALIDARG);
		return E_INVALIDARG;
	}
	const HRESULT hr = a_queueB->Wait(transferFenceB.get(), nextLaneValue);
	if (FAILED(hr))
		Fail("transfer fence Wait", hr);
	return hr;
}

void CrossAdapterBridge::RecordUnpack(ID3D12GraphicsCommandList* a_listB, UINT a_slot)
{
	if (!active || !a_listB || a_slot >= kSlots)
		return;

	std::vector<D3D12_RESOURCE_BARRIER> before;
	std::vector<D3D12_RESOURCE_BARRIER> after;
	for (auto& lane : lanes) {
		if (auto* mirror = lane.mirrorB[a_slot].get()) {
			before.push_back(Transition(mirror, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST));
			after.push_back(Transition(mirror, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON));
		}
	}
	if (before.empty())
		return;

	a_listB->ResourceBarrier(static_cast<UINT>(before.size()), before.data());
	for (auto& lane : lanes) {
		auto* mirror = lane.mirrorB[a_slot].get();
		if (!mirror)
			continue;
		D3D12_TEXTURE_COPY_LOCATION dst{};
		dst.pResource = mirror;
		dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		dst.SubresourceIndex = 0;

		D3D12_TEXTURE_COPY_LOCATION src{};
		src.pResource = lane.stagingB.get();
		src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		src.PlacedFootprint = lane.footprint;
		src.PlacedFootprint.Offset = lane.slotStride * a_slot;

		a_listB->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
	}
	a_listB->ResourceBarrier(static_cast<UINT>(after.size()), after.data());
}

void CrossAdapterBridge::RecordUnpackTo(ID3D12GraphicsCommandList* a_listB, int a_lane, UINT a_slot,
	ID3D12Resource* a_destB, D3D12_RESOURCE_STATES a_destState)
{
	if (!active || !a_listB || !a_destB || a_slot >= kSlots || a_lane < 0 ||
		static_cast<size_t>(a_lane) >= lanes.size())
		return;
	auto& lane = lanes[static_cast<size_t>(a_lane)];
	RecordLaneCopy(a_listB, lane, a_slot, a_destB, a_destState, lane.stagingB.get());
}

HRESULT CrossAdapterBridge::SignalConsumed(ID3D12CommandQueue* a_queueB)
{
	if (!active || !a_queueB)
		return E_FAIL;
	const HRESULT hr = a_queueB->Signal(consumedFenceB.get(), transferValue);
	if (FAILED(hr))
		Fail("consumed fence Signal", hr);
	return hr;
}

// ---------------------------------------------------------------------------------------
// Half-resolution depth (iHalfResDepthMV = 1)

bool CrossAdapterBridge::CreateDepthUpsampler(UINT a_width, UINT a_height, const wchar_t* a_shaderPath)
{
	ResetUpsampler();
	if (!active || !a_width || !a_height || !a_shaderPath)
		return false;

	const auto failed = [this](const char* a_what, HRESULT a_hr) {
		REX::ERROR("[FG-GPU1] depth rebuild on the display GPU: {} failed (hr={:#010x}); sending depth at full resolution",
			a_what, static_cast<std::uint32_t>(a_hr));
		ResetUpsampler();
		return false;
	};

	// The allocator of a slot is reused once that slot's previous direct list is done.
	if (asyncUnpack && !displayDoneFenceB)
		return failed("the display GPU's direct-queue fence (SetDisplayQueue)", E_FAIL);

	winrt::com_ptr<ID3DBlob> shader;
	winrt::com_ptr<ID3DBlob> shaderErrors;
	HRESULT hr = D3DCompileFromFile(a_shaderPath, nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "main", "cs_5_0",
		D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, shader.put(), shaderErrors.put());
	if (FAILED(hr)) {
		if (shaderErrors)
			REX::ERROR("[FG-GPU1] {}", static_cast<const char*>(shaderErrors->GetBufferPointer()));
		return failed("compiling DepthFromHalfResCS.hlsl", hr);
	}

	// t0 = half-resolution depth, u0 = full-resolution depth, one table per slot.
	D3D12_DESCRIPTOR_RANGE ranges[2]{};
	ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	ranges[0].NumDescriptors = 1;
	ranges[0].OffsetInDescriptorsFromTableStart = 0;
	ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
	ranges[1].NumDescriptors = 1;
	ranges[1].OffsetInDescriptorsFromTableStart = 1;

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
		return failed("D3D12SerializeRootSignature", hr);
	hr = deviceB->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
		IID_PPV_ARGS(upsampler.rootSignature.put()));
	if (FAILED(hr))
		return failed("CreateRootSignature", hr);

	D3D12_COMPUTE_PIPELINE_STATE_DESC pipelineDesc{};
	pipelineDesc.pRootSignature = upsampler.rootSignature.get();
	pipelineDesc.CS = { shader->GetBufferPointer(), shader->GetBufferSize() };
	hr = deviceB->CreateComputePipelineState(&pipelineDesc, IID_PPV_ARGS(upsampler.pipeline.put()));
	if (FAILED(hr))
		return failed("CreateComputePipelineState", hr);

	D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
	heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	heapDesc.NumDescriptors = kSlots * 2;
	heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	hr = deviceB->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(upsampler.descriptors.put()));
	if (FAILED(hr))
		return failed("CreateDescriptorHeap", hr);
	upsampler.descriptorSize = deviceB->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

	D3D12_RESOURCE_DESC targetDesc{};
	targetDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	targetDesc.Width = a_width;
	targetDesc.Height = a_height;
	targetDesc.DepthOrArraySize = 1;
	targetDesc.MipLevels = 1;
	targetDesc.Format = DXGI_FORMAT_R32_FLOAT;
	targetDesc.SampleDesc.Count = 1;
	targetDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	targetDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
	D3D12_HEAP_PROPERTIES defaultHeap{};
	defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;

	D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
	uavDesc.Format = DXGI_FORMAT_R32_FLOAT;
	uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;

	for (UINT slot = 0; slot < slotCount; ++slot) {
		// COMMON between uses, like the mirrors: FFX's read promotes it implicitly.
		hr = deviceB->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &targetDesc,
			D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(upsampler.target[slot].put()));
		if (FAILED(hr))
			return failed("CreateCommittedResource(full-resolution depth)", hr);
		D3D12_CPU_DESCRIPTOR_HANDLE handle = upsampler.descriptors->GetCPUDescriptorHandleForHeapStart();
		handle.ptr += static_cast<SIZE_T>(slot * 2 + 1) * upsampler.descriptorSize;
		deviceB->CreateUnorderedAccessView(upsampler.target[slot].get(), nullptr, &uavDesc, handle);

		hr = deviceB->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(upsampler.allocators[slot].put()));
		if (FAILED(hr))
			return failed("CreateCommandAllocator", hr);
	}
	hr = deviceB->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, upsampler.allocators[0].get(), nullptr,
		IID_PPV_ARGS(upsampler.list.put()));
	if (SUCCEEDED(hr))
		hr = upsampler.list->Close();
	if (FAILED(hr))
		return failed("CreateCommandList", hr);
	hr = deviceB->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(upsampler.sourceReadyFence.put()));
	if (FAILED(hr))
		return failed("CreateFence", hr);

	upsampler.width = a_width;
	upsampler.height = a_height;
	return true;
}

void CrossAdapterBridge::SetDepthUpsampleLane(int a_lane)
{
	if (!active || !upsampler.pipeline || a_lane < 0 || static_cast<size_t>(a_lane) >= lanes.size())
		return;
	auto& lane = lanes[static_cast<size_t>(a_lane)];

	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
	srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc.Texture2D.MipLevels = 1;
	for (UINT slot = 0; slot < slotCount; ++slot) {
		if (!lane.mirrorB[slot]) {
			REX::ERROR("[FG-GPU1] depth lane has no mirror on the display GPU; depth rebuild disabled");
			return;
		}
		D3D12_CPU_DESCRIPTOR_HANDLE handle = upsampler.descriptors->GetCPUDescriptorHandleForHeapStart();
		handle.ptr += static_cast<SIZE_T>(slot * 2) * upsampler.descriptorSize;
		deviceB->CreateShaderResourceView(lane.mirrorB[slot].get(), &srvDesc, handle);
	}
	upsampler.lane = a_lane;
	REX::INFO("[FG-GPU1] depth crosses at {}x{} and is rebuilt at {}x{} on the display GPU ({})", lane.desc.Width,
		lane.desc.Height, upsampler.width, upsampler.height,
		asyncUnpack ? "as soon as it lands, while the other lanes still cross" : "on the frame's direct list");
}

ID3D12Resource* CrossAdapterBridge::UpsampledDepth(UINT a_slot) const noexcept
{
	if (!upsampler.Ready() || a_slot >= kSlots)
		return nullptr;
	return upsampler.target[a_slot].get();
}

void CrossAdapterBridge::RecordUpsampleCommands(ID3D12GraphicsCommandList* a_list, UINT a_slot)
{
	ID3D12Resource* source = lanes[static_cast<size_t>(upsampler.lane)].mirrorB[a_slot].get();
	ID3D12Resource* target = upsampler.target[a_slot].get();

	timerDisplay.Mark(a_list, a_slot, kMarkUpsampleBegin);

	// Explicit both ways, so the states are right whether or not the list also unpacked.
	const D3D12_RESOURCE_BARRIER before[2] = {
		Transition(source, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
		Transition(target, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
	};
	a_list->ResourceBarrier(2, before);

	ID3D12DescriptorHeap* heaps[] = { upsampler.descriptors.get() };
	a_list->SetDescriptorHeaps(1, heaps);
	a_list->SetComputeRootSignature(upsampler.rootSignature.get());
	a_list->SetPipelineState(upsampler.pipeline.get());
	D3D12_GPU_DESCRIPTOR_HANDLE table = upsampler.descriptors->GetGPUDescriptorHandleForHeapStart();
	table.ptr += static_cast<UINT64>(a_slot * 2) * upsampler.descriptorSize;
	a_list->SetComputeRootDescriptorTable(0, table);
	a_list->Dispatch((upsampler.width + 7) / 8, (upsampler.height + 7) / 8, 1);

	const D3D12_RESOURCE_BARRIER after[2] = {
		Transition(source, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
		Transition(target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON)
	};
	a_list->ResourceBarrier(2, after);

	timerDisplay.Mark(a_list, a_slot, kMarkUpsampleEnd);
}

void CrossAdapterBridge::RecordDepthUpsample(ID3D12GraphicsCommandList* a_listB, UINT a_slot)
{
	if (!active || asyncUnpack || !a_listB || a_slot >= kSlots || !upsampler.Ready())
		return;
	RecordUpsampleCommands(a_listB, a_slot);
}

HRESULT CrossAdapterBridge::SubmitDepthUpsample(UINT a_slot, UINT64 a_frame, ID3D12CommandQueue* a_queueB)
{
	// This slot's allocator was last used ahead of the slot's previous direct list on the same
	// queue; FFX's Present has normally waited for that frame already, so this does not block.
	if (slotDisplayDone[a_slot]) {
		const HRESULT hr = WaitCpu(displayDoneFenceB.get(), slotDisplayDone[a_slot]);
		if (FAILED(hr)) {
			Fail("waiting for the previous depth rebuild of this slot", hr);
			return hr;
		}
	}

	auto* allocator = upsampler.allocators[a_slot].get();
	HRESULT hr = allocator->Reset();
	if (SUCCEEDED(hr))
		hr = upsampler.list->Reset(allocator, nullptr);
	if (FAILED(hr)) {
		Fail("resetting the depth-rebuild list", hr);
		return hr;
	}
	RecordUpsampleCommands(upsampler.list.get(), a_slot);
	hr = upsampler.list->Close();
	if (FAILED(hr)) {
		Fail("closing the depth-rebuild list", hr);
		return hr;
	}

	// Only the depth lane: the rebuild overlaps the motion vectors, HUD-less and final frame
	// still crossing, and the frame's own list waits for all of them right after this.
	hr = a_queueB->Wait(upsampler.sourceReadyFence.get(), a_frame);
	if (FAILED(hr)) {
		Fail("depth-rebuild Wait", hr);
		return hr;
	}
	ID3D12CommandList* lists[] = { upsampler.list.get() };
	a_queueB->ExecuteCommandLists(1, lists);
	return S_OK;
}

void CrossAdapterBridge::ResetUpsampler() noexcept
{
	upsampler = DepthUpsampler{};
}

void CrossAdapterBridge::MarkDisplayList(ID3D12GraphicsCommandList* a_listB, UINT a_slot, UINT a_mark)
{
	if (!active)
		return;
	timerDisplay.Mark(a_listB, a_slot, a_mark);
	if (a_mark == kMarkListEnd)
		timerDisplay.Resolve(a_listB, a_slot);
}

HRESULT CrossAdapterBridge::SignalDisplayListDone(ID3D12CommandQueue* a_queueB)
{
	if (!active || !a_queueB || !displayDoneFenceB)
		return S_OK;
	const HRESULT hr = a_queueB->Signal(displayDoneFenceB.get(), ++displayDoneValue);
	if (FAILED(hr)) {
		Fail("display-list fence Signal", hr);
		return hr;
	}
	slotDisplayDone[currentSlot] = displayDoneValue;
	return S_OK;
}

void CrossAdapterBridge::AddPresentBlock(double a_presentMs, double a_latencyWaitMs)
{
	if (!active)
		return;
	statPresentBlock.Add(a_presentMs);
	statLatencyWait.Add(a_latencyWaitMs);
}

HRESULT CrossAdapterBridge::GateD3D11(UINT a_nextSlot)
{
	if (!active || a_nextSlot >= kSlots || !slotTransfer[a_nextSlot])
		return S_OK;
	++statGateCalls;
	// Still copying when the wait is queued. The D3D11 GPU timeline is usually behind the CPU,
	// so this does not prove a stall, but if it is high the 3090 is waiting on PCIe.
	if (copyDoneFenceA->GetCompletedValue() < slotTransfer[a_nextSlot])
		++statGatePending;
	const HRESULT hr = context11->Wait(copyDoneFence11.get(), slotTransfer[a_nextSlot]);
	if (FAILED(hr))
		Fail("D3D11 Wait on the copy-done fence", hr);
	return hr;
}

// ---------------------------------------------------------------------------------------
// Statistics

void CrossAdapterBridge::CalibrateTimers()
{
	timerCopyA.Calibrate(copyQueueA.get());
	if (copyQueueB)
		timerCopyB.Calibrate(copyQueueB.get());
	if (displayQueue)
		timerDisplay.Calibrate(displayQueue);
}

void CrossAdapterBridge::CollectTimings(UINT a_slot)
{
	if (a_slot >= kSlots || !slotTransfer[a_slot] || !qpcFrequency.QuadPart)
		return;

	const double toMs = 1000.0 / static_cast<double>(qpcFrequency.QuadPart);
	const double presentQpc = static_cast<double>(slotPresentQpc[a_slot].QuadPart);
	const auto sincePresent = [&](double a_qpc) { return (a_qpc - presentQpc) * toMs; };

	std::array<double, GpuTimer::kMaxMarks> copyA{};
	std::array<double, GpuTimer::kMaxMarks> copyB{};
	std::array<double, GpuTimer::kMaxMarks> display{};
	UINT validA = 0;
	UINT validB = 0;
	UINT validDisplay = 0;

	// The caller already waited for this slot's copy. The other two are checked, never waited
	// for: if they are still running the sample is dropped (and the slot is re-armed).
	timerCopyA.Read(a_slot, copyA, validA);
	if (!asyncUnpack || (unpackFenceB && unpackFenceB->GetCompletedValue() >= slotUnpack[a_slot]))
		timerCopyB.Read(a_slot, copyB, validB);
	else
		timerCopyB.written[a_slot] = 0;
	if (displayDoneFenceB && displayDoneFenceB->GetCompletedValue() >= slotDisplayDone[a_slot])
		timerDisplay.Read(a_slot, display, validDisplay);
	else
		timerDisplay.written[a_slot] = 0;

	constexpr UINT kListBeginEnd = (1u << kMarkListBegin) | (1u << kMarkListEnd);
	constexpr UINT kListUnpack = (1u << kMarkListBegin) | (1u << kMarkAfterUnpack);

	// Both copy queues mark phases kMarkWorld, kMarkHudless, kMarkPresent, two marks each.
	// Only phases that ran count, so the rendering between them is not reported as copy time;
	// start/end are the first phase's start and the last phase's end.
	const auto sumPhases = [](const std::array<double, GpuTimer::kMaxMarks>& a_marks, UINT a_valid, double& a_busy,
							   double& a_start, double& a_end) {
		bool any = false;
		a_busy = 0.0;
		for (UINT phase = 0; phase < 3; ++phase) {
			const UINT bitStart = 1u << (phase * 2);
			const UINT bitEnd = 1u << (phase * 2 + 1);
			if (!(a_valid & bitStart) || !(a_valid & bitEnd))
				continue;
			a_busy += a_marks[phase * 2 + 1] - a_marks[phase * 2];
			if (!any)
				a_start = a_marks[phase * 2];
			a_end = a_marks[phase * 2 + 1];
			any = true;
		}
		return any;
	};

	double busy = 0.0;
	double start = 0.0;
	double end = 0.0;
	if (sumPhases(copyA, validA, busy, start, end)) {
		statCopyA.Add(busy * toMs);
		if (timerCopyA.calibrated) {
			statCopyAStart.Add(sincePresent(start));
			statCopyAEnd.Add(sincePresent(end));
		}
	}

	if (asyncUnpack) {
		if (sumPhases(copyB, validB, busy, start, end)) {
			statUnpack.Add(busy * toMs);
			if (timerCopyB.calibrated) {
				statUnpackStart.Add(sincePresent(start));
				statUnpackEnd.Add(sincePresent(end));
			}
		}
	} else if ((validDisplay & kListUnpack) == kListUnpack) {
		// Serialised: the list starts once the transfer has landed and unpacks first.
		statUnpack.Add((display[kMarkAfterUnpack] - display[kMarkListBegin]) * toMs);
		if (timerDisplay.calibrated) {
			statUnpackStart.Add(sincePresent(display[kMarkListBegin]));
			statUnpackEnd.Add(sincePresent(display[kMarkAfterUnpack]));
		}
	}

	// bDirectGPUCopy: the display GPU's Vulkan copies of this slot's last frame (skipped while still running).
	double peerMs = 0.0;
	if (peer.Ready() && peer.CopyTimeMs(a_slot, peerMs))
		statPeerCopy.Add(peerMs);

	if ((validDisplay & kListBeginEnd) == kListBeginEnd) {
		statDisplayList.Add((display[kMarkListEnd] - display[kMarkListBegin]) * toMs);
		if (timerDisplay.calibrated) {
			statListStart.Add(sincePresent(display[kMarkListBegin]));
			statListEnd.Add(sincePresent(display[kMarkListEnd]));
		}
	}

	constexpr UINT kUpsample = (1u << kMarkUpsampleBegin) | (1u << kMarkUpsampleEnd);
	if ((validDisplay & kUpsample) == kUpsample) {
		statUpsample.Add((display[kMarkUpsampleEnd] - display[kMarkUpsampleBegin]) * toMs);
		if (timerDisplay.calibrated)
			statUpsampleEnd.Add(sincePresent(display[kMarkUpsampleEnd]));
	}
}

void CrossAdapterBridge::LogStats()
{
	if (!active || !statFrames)
		return;
	LARGE_INTEGER now{};
	QueryPerformanceCounter(&now);
	const double elapsedMs = QpcMs(statLastLog, now, qpcFrequency);
	if (elapsedMs < 10000.0)
		return;

	const double fps = static_cast<double>(statFrames) * 1000.0 / elapsedMs;
	const double mbPerFrame = static_cast<double>(bytesPerFrame) / (1024.0 * 1024.0);
	if (statCopyA.count) {
		const double avgCopy = statCopyA.Mean();
		REX::INFO("[FG-GPU1] {:.1f} game fps | {:.1f} MB/frame = {:.2f} GB/s | copy on render GPU avg {:.2f} ms max {:.2f} ms "
				  "(~{:.2f} GB/s link) | submit {:.3f} ms | max lag {} | cpu waits {}",
			fps, mbPerFrame, mbPerFrame * fps / 1024.0, avgCopy, statCopyA.max,
			avgCopy > 0.0 ? mbPerFrame / 1024.0 / (avgCopy / 1000.0) : 0.0,
			statCpuSubmitMs / static_cast<double>(statFrames), statMaxLag, statCpuWaits);
	} else {
		REX::INFO("[FG-GPU1] {:.1f} game fps | {:.1f} MB/frame = {:.2f} GB/s | submit {:.3f} ms | max lag {} | cpu waits {}",
			fps, mbPerFrame, mbPerFrame * fps / 1024.0, statCpuSubmitMs / static_cast<double>(statFrames), statMaxLag,
			statCpuWaits);
	}

	// Display GPU: how long the read takes and how busy the direct queue is.
	REX::INFO("[FG-GPU1] display GPU: unpack ({}) avg {:.2f} ms max {:.2f} ms | direct list avg {:.2f} ms max {:.2f} ms",
		asyncUnpack ? "copy queue" : "direct queue", statUnpack.Mean(), statUnpack.max, statDisplayList.Mean(),
		statDisplayList.max);

	// One frame's path, in ms after the CPU entered Present for it (both GPUs on the CPU clock).
	if (statCopyAStart.count && statListEnd.count) {
		REX::INFO("[FG-GPU1] timeline ms after Present: 3090 copy {:.1f}-{:.1f} | 9060 unpack {:.1f}-{:.1f} | "
				  "9060 list {:.1f}-{:.1f}",
			statCopyAStart.Mean(), statCopyAEnd.Mean(), statUnpackStart.Mean(), statUnpackEnd.Mean(),
			statListStart.Mean(), statListEnd.Mean());
	}

	// iHalfResDepthMV = 1: compare "done" with the unpack end above; earlier means it is hidden.
	if (statUpsample.count) {
		REX::INFO("[FG-GPU1] depth rebuild on display GPU avg {:.2f} ms max {:.2f} ms | done {:.1f} ms after Present",
			statUpsample.Mean(), statUpsample.max, statUpsampleEnd.Mean());
	}

	// Back-pressure on the render thread: while it sits here, the 3090 gets no new work.
	REX::INFO("[FG-GPU1] render thread blocked: FFX Present avg {:.2f} ms max {:.2f} ms | latency wait avg {:.2f} ms "
			  "max {:.2f} ms | D3D11 gate pending {}/{}",
		statPresentBlock.Mean(), statPresentBlock.max, statLatencyWait.Mean(), statLatencyWait.max, statGatePending,
		statGateCalls);

	// bDirectGPUCopy: how many frames went straight into the display GPU's VRAM, and the guard's count.
	if (directCopyTripped) {
		REX::INFO("[FG-GPU1] direct GPU copy: OFF since its guard tripped (see the CRITICAL line); frames cross through "
				  "system memory, frame generation is off");
	} else if (directCopy || statDirectFrames) {
		REX::INFO("[FG-GPU1] direct GPU copy: {}/{} frames into the display GPU's VRAM | its Vulkan copy into the landing "
				  "heaps ({}) avg {:.2f} ms max {:.2f} ms per frame | check values found {} (late {})",
			statDirectFrames, statFrames, peer.QueueName(), statPeerCopy.Mean(), statPeerCopy.max, statDirectChecks,
			statDirectLate);
	}

	statFrames = 0;
	statPeerCopy = {};
	statDirectFrames = 0;
	statDirectChecks = 0;
	statDirectLate = 0;
	statCopyA = {};
	statUnpack = {};
	statDisplayList = {};
	statCopyAStart = {};
	statCopyAEnd = {};
	statUnpackStart = {};
	statUnpackEnd = {};
	statListStart = {};
	statListEnd = {};
	statUpsample = {};
	statUpsampleEnd = {};
	statPresentBlock = {};
	statLatencyWait = {};
	statCpuSubmitMs = 0.0;
	statMaxLag = 0;
	statCpuWaits = 0;
	statGatePending = 0;
	statGateCalls = 0;
	statLastLog = now;
}

void CrossAdapterBridge::Shutdown() noexcept
{
	if (fenceEvent) {
		if (copyQueueA && copyDoneFenceA && transferValue)
			WaitCpu(copyDoneFenceA.get(), transferValue);
		// A read started at a hook whose Present never came: signal it, or the wait below times out.
		if (unpackStarted && copyQueueB && unpackFenceB)
			copyQueueB->Signal(unpackFenceB.get(), unpackFrame);
		const UINT64 lastUnpack = *std::max_element(slotUnpack.begin(), slotUnpack.end());
		if (copyQueueB && unpackFenceB && lastUnpack)
			WaitCpu(unpackFenceB.get(), lastUnpack);
		if (displayDoneFenceB && displayDoneValue)
			WaitCpu(displayDoneFenceB.get(), displayDoneValue);
	}
	active = false;
	asyncUnpack = true;
	ResetUpsampler();
	// The render GPU's heaps on the display GPU's VRAM blocks go first (with the lanes), then the
	// Vulkan side that owns the blocks.
	lanes.clear();
	peer.Shutdown();
	directCopy = false;
	directCopyTripped = false;
	frameDirect = false;
	peerValue = 0;
	checkCpu = nullptr;
	checkUploadA = nullptr;
	bytesPerFrame = 0;
	currentSlot = 0;
	transferValue = 0;
	slotTransfer = {};
	slotPresentQpc = {};
	frameStarted = false;
	frameSlot = 0;
	nextLaneValue = 0;
	laneValue = {};
	laneFrame = {};
	slotUnpack = {};
	unpackStarted = false;
	unpackFrame = 0;
	laneUnpackFrame = {};
	readValue = 0;
	laneReadValue = {};
	slotDisplayDone = {};
	displayDoneValue = 0;
	timerCopyA.Reset();
	timerCopyB.Reset();
	timerDisplay.Reset();
	listA = nullptr;
	for (auto& allocator : allocatorsA)
		allocator = nullptr;
	copyQueueA = nullptr;
	listB = nullptr;
	for (auto& allocator : allocatorsB)
		allocator = nullptr;
	copyQueueB = nullptr;
	unpackFenceB = nullptr;
	readFenceB = nullptr;
	displayQueue = nullptr;
	displayDoneFenceB = nullptr;
	transferFenceA = nullptr;
	transferFenceB = nullptr;
	consumedFenceA = nullptr;
	consumedFenceB = nullptr;
	copyDoneFenceA = nullptr;
	copyDoneFence11 = nullptr;
	if (fenceEvent) {
		CloseHandle(fenceEvent);
		fenceEvent = nullptr;
	}
	context11 = nullptr;
	deviceA = nullptr;
	deviceB = nullptr;
}
