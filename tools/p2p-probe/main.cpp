// p2p-probe - can the render GPU's copy engine write straight into the display GPU's VRAM?
//
// The display GPU (AMD) maps a VRAM allocation for the CPU through its resizable BAR. That
// mapping is an ordinary virtual address in this process. The render GPU (NVIDIA) is then asked
// to treat that address as host memory (Vulkan VK_EXT_external_memory_host, D3D12
// OpenExistingHeapFromAddress). If the kernel lets it, the NVIDIA copy engine's writes go to the
// AMD card's BAR: a peer-to-peer PCIe write that never touches system memory.
//
// Modes (see README.md):
//   (none)      allocations and import calls only; no GPU writes across the bus
//   --dma       the copies, timed and checked on the AMD side
//   --bench     every direction at the mod's lane sizes on D3D12 copy queues
//   --interop   which way the AMD D3D12 device can use the block (bDirectGPUCopy uses route 3)
//   --evict     --interop, then VRAM pressure on the AMD GPU: does the block keep its address?
//   --pipeline  the mod's per-frame sequence 300 times, the AMD side queued before the NVIDIA write

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan_core.h>
#include <vulkan/vulkan_win32.h>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <functional>
#include <immintrin.h>
#include <iterator>
#include <thread>
#include <vector>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;

namespace
{
	constexpr VkDeviceSize kSize = 64ull << 20;  // 64 MiB per transfer
	constexpr uint32_t kReps = 8;
	constexpr uint64_t kTimeoutNs = 10'000'000'000ull;
	constexpr DWORD kTimeoutMs = 10'000;
	constexpr uint32_t kVendorAmd = 0x1002;
	constexpr uint32_t kVendorNvidia = 0x10DE;
	constexpr uint32_t kSeedVulkan = 0x5EEDF00Du;
	constexpr uint32_t kSeedD3D12 = 0xD3D12C0Du;

#define VK_INSTANCE_FNS(X)                                                                                         \
	X(vkDestroyInstance)                                                                                           \
	X(vkEnumeratePhysicalDevices)                                                                                  \
	X(vkGetPhysicalDeviceProperties)                                                                               \
	X(vkGetPhysicalDeviceProperties2)                                                                              \
	X(vkGetPhysicalDeviceMemoryProperties)                                                                         \
	X(vkGetPhysicalDeviceMemoryProperties2)                                                                        \
	X(vkGetPhysicalDeviceQueueFamilyProperties)                                                                    \
	X(vkEnumerateDeviceExtensionProperties)                                                                        \
	X(vkGetPhysicalDeviceExternalBufferProperties)                                                                 \
	X(vkCreateDevice)                                                                                              \
	X(vkGetDeviceProcAddr)                                                                                         \
	X(vkGetDeviceQueue)                                                                                            \
	X(vkDestroyDevice)                                                                                             \
	X(vkDeviceWaitIdle)                                                                                            \
	X(vkAllocateMemory)                                                                                            \
	X(vkFreeMemory)                                                                                                \
	X(vkMapMemory)                                                                                                 \
	X(vkUnmapMemory)                                                                                               \
	X(vkCreateBuffer)                                                                                              \
	X(vkDestroyBuffer)                                                                                             \
	X(vkGetBufferMemoryRequirements)                                                                               \
	X(vkBindBufferMemory)                                                                                          \
	X(vkCreateCommandPool)                                                                                         \
	X(vkDestroyCommandPool)                                                                                        \
	X(vkAllocateCommandBuffers)                                                                                    \
	X(vkBeginCommandBuffer)                                                                                        \
	X(vkEndCommandBuffer)                                                                                          \
	X(vkResetCommandBuffer)                                                                                        \
	X(vkCmdCopyBuffer)                                                                                             \
	X(vkCmdFillBuffer)                                                                                             \
	X(vkCmdWriteTimestamp)                                                                                         \
	X(vkCmdResetQueryPool)                                                                                         \
	X(vkCmdPipelineBarrier)                                                                                        \
	X(vkCreateQueryPool)                                                                                           \
	X(vkDestroyQueryPool)                                                                                          \
	X(vkGetQueryPoolResults)                                                                                       \
	X(vkQueueSubmit)                                                                                               \
	X(vkCreateFence)                                                                                               \
	X(vkDestroyFence)                                                                                              \
	X(vkWaitForFences)                                                                                             \
	X(vkResetFences)                                                                                               \
	X(vkCreateSemaphore)                                                                                           \
	X(vkDestroySemaphore)

#define VK_DECLARE(name) PFN_##name name = nullptr;
	PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;
	PFN_vkCreateInstance vkCreateInstance = nullptr;
	VK_INSTANCE_FNS(VK_DECLARE)
	PFN_vkGetMemoryHostPointerPropertiesEXT vkGetMemoryHostPointerPropertiesEXT = nullptr;
	PFN_vkGetMemoryWin32HandleKHR vkGetMemoryWin32HandleKHR = nullptr;
	PFN_vkGetMemoryWin32HandlePropertiesKHR vkGetMemoryWin32HandlePropertiesKHR = nullptr;
	PFN_vkImportSemaphoreWin32HandleKHR vkImportSemaphoreWin32HandleKHR = nullptr;
	PFN_vkSetDeviceMemoryPriorityEXT vkSetDeviceMemoryPriorityEXT = nullptr;
#undef VK_DECLARE

	uint32_t Pattern(uint64_t a_index, uint32_t a_seed)
	{
		uint32_t x = static_cast<uint32_t>(a_index) * 2654435761u ^ a_seed;
		x ^= x >> 15;
		x *= 0x2c1b3c6du;
		x ^= x >> 12;
		return x;
	}

	const char* VkName(VkResult a_result)
	{
		switch (a_result) {
		case VK_SUCCESS: return "VK_SUCCESS";
		case VK_TIMEOUT: return "VK_TIMEOUT";
		case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
		case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
		case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
		case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
		case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
		case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
		case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
		case VK_ERROR_INVALID_EXTERNAL_HANDLE: return "VK_ERROR_INVALID_EXTERNAL_HANDLE";
		case VK_ERROR_UNKNOWN: return "VK_ERROR_UNKNOWN";
		default: return "VkResult(other)";
		}
	}

	const char* HandleTypeName(VkExternalMemoryHandleTypeFlagBits a_type)
	{
		return a_type == VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT ? "HOST_ALLOCATION" :
		                                                                        "HOST_MAPPED_FOREIGN_MEMORY";
	}

	struct Gpu
	{
		const char* label = "";
		VkPhysicalDevice phys = VK_NULL_HANDLE;
		VkPhysicalDeviceProperties props{};
		VkPhysicalDeviceMemoryProperties mem{};
		VkDevice dev = VK_NULL_HANDLE;
		uint32_t family = UINT32_MAX;
		VkQueue queue = VK_NULL_HANDLE;
		VkCommandPool pool = VK_NULL_HANDLE;
		VkCommandBuffer cmd = VK_NULL_HANDLE;
		VkFence fence = VK_NULL_HANDLE;
		bool budget = false;
	};

	struct Buffer
	{
		VkBuffer buffer = VK_NULL_HANDLE;
		VkDeviceMemory memory = VK_NULL_HANDLE;
		bool ownsMemory = false;
		void* mapped = nullptr;

		void Destroy(const Gpu& a_gpu)
		{
			if (buffer) vkDestroyBuffer(a_gpu.dev, buffer, nullptr);
			if (ownsMemory && memory) vkFreeMemory(a_gpu.dev, memory, nullptr);
			*this = {};
		}
	};

	bool HasExtension(VkPhysicalDevice a_phys, const char* a_name)
	{
		uint32_t count = 0;
		vkEnumerateDeviceExtensionProperties(a_phys, nullptr, &count, nullptr);
		std::vector<VkExtensionProperties> exts(count);
		vkEnumerateDeviceExtensionProperties(a_phys, nullptr, &count, exts.data());
		for (const auto& e : exts) {
			if (std::strcmp(e.extensionName, a_name) == 0) return true;
		}
		return false;
	}

	// a_compute: an async-compute family (COMPUTE without GRAPHICS), as bDirectGPUCopy uses; else graphics.
	bool CreateDevice(Gpu& a_gpu, const std::vector<const char*>& a_exts, bool a_compute = false)
	{
		uint32_t count = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(a_gpu.phys, &count, nullptr);
		std::vector<VkQueueFamilyProperties> families(count);
		vkGetPhysicalDeviceQueueFamilyProperties(a_gpu.phys, &count, families.data());
		for (uint32_t i = 0; i < count; ++i) {
			const VkQueueFlags flags = families[i].queueFlags;
			const bool fits = a_compute ? ((flags & VK_QUEUE_COMPUTE_BIT) && !(flags & VK_QUEUE_GRAPHICS_BIT)) :
			                              (flags & VK_QUEUE_GRAPHICS_BIT) != 0;
			if (fits && families[i].timestampValidBits) {
				a_gpu.family = i;
				break;
			}
		}
		if (a_gpu.family == UINT32_MAX) {
			std::printf("  %s: no %s queue with timestamps\n", a_gpu.label, a_compute ? "async-compute" : "graphics");
			return false;
		}
		std::printf("%s Vulkan queue: family %u (%s)\n", a_gpu.label, a_gpu.family, a_compute ? "async compute" : "graphics");

		const float priority = 1.0f;
		VkDeviceQueueCreateInfo qci{ VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
		qci.queueFamilyIndex = a_gpu.family;
		qci.queueCount = 1;
		qci.pQueuePriorities = &priority;
		VkPhysicalDeviceVulkan12Features f12{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
		f12.timelineSemaphore = VK_TRUE;
		VkPhysicalDevicePageableDeviceLocalMemoryFeaturesEXT pageable{
			VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PAGEABLE_DEVICE_LOCAL_MEMORY_FEATURES_EXT
		};
		pageable.pageableDeviceLocalMemory = VK_TRUE;
		for (const char* e : a_exts) {
			if (std::strcmp(e, VK_EXT_PAGEABLE_DEVICE_LOCAL_MEMORY_EXTENSION_NAME) == 0) f12.pNext = &pageable;
		}
		VkDeviceCreateInfo dci{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
		dci.pNext = &f12;
		dci.queueCreateInfoCount = 1;
		dci.pQueueCreateInfos = &qci;
		dci.enabledExtensionCount = static_cast<uint32_t>(a_exts.size());
		dci.ppEnabledExtensionNames = a_exts.data();
		VkResult r = vkCreateDevice(a_gpu.phys, &dci, nullptr, &a_gpu.dev);
		if (r != VK_SUCCESS) {
			std::printf("  %s: vkCreateDevice %s\n", a_gpu.label, VkName(r));
			return false;
		}
		vkGetDeviceQueue(a_gpu.dev, a_gpu.family, 0, &a_gpu.queue);

		VkCommandPoolCreateInfo pci{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
		pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		pci.queueFamilyIndex = a_gpu.family;
		vkCreateCommandPool(a_gpu.dev, &pci, nullptr, &a_gpu.pool);
		VkCommandBufferAllocateInfo cai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
		cai.commandPool = a_gpu.pool;
		cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		cai.commandBufferCount = 1;
		vkAllocateCommandBuffers(a_gpu.dev, &cai, &a_gpu.cmd);
		VkFenceCreateInfo fci{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
		vkCreateFence(a_gpu.dev, &fci, nullptr, &a_gpu.fence);
		return true;
	}

	void DestroyDevice(Gpu& a_gpu)
	{
		if (!a_gpu.dev) return;
		vkDeviceWaitIdle(a_gpu.dev);
		vkDestroyFence(a_gpu.dev, a_gpu.fence, nullptr);
		vkDestroyCommandPool(a_gpu.dev, a_gpu.pool, nullptr);
		vkDestroyDevice(a_gpu.dev, nullptr);
		a_gpu.dev = VK_NULL_HANDLE;
	}

	void Begin(Gpu& a_gpu)
	{
		vkResetCommandBuffer(a_gpu.cmd, 0);
		VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
		bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		vkBeginCommandBuffer(a_gpu.cmd, &bi);
	}

	VkResult Submit(Gpu& a_gpu)
	{
		vkEndCommandBuffer(a_gpu.cmd);
		VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
		si.commandBufferCount = 1;
		si.pCommandBuffers = &a_gpu.cmd;
		VkResult r = vkQueueSubmit(a_gpu.queue, 1, &si, a_gpu.fence);
		if (r != VK_SUCCESS) return r;
		r = vkWaitForFences(a_gpu.dev, 1, &a_gpu.fence, VK_TRUE, kTimeoutNs);
		if (r == VK_SUCCESS) vkResetFences(a_gpu.dev, 1, &a_gpu.fence);
		return r;
	}

	void TransferBarrier(Gpu& a_gpu)
	{
		VkMemoryBarrier mb{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
		mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
		vkCmdPipelineBarrier(a_gpu.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0,
			nullptr, 0, nullptr);
	}

	uint32_t FindType(const Gpu& a_gpu, uint32_t a_bits, VkMemoryPropertyFlags a_want, VkMemoryPropertyFlags a_avoid)
	{
		for (uint32_t i = 0; i < a_gpu.mem.memoryTypeCount; ++i) {
			const VkMemoryPropertyFlags f = a_gpu.mem.memoryTypes[i].propertyFlags;
			if ((a_bits & (1u << i)) && (f & a_want) == a_want && !(f & a_avoid)) return i;
		}
		return UINT32_MAX;
	}

	// Allocates (a_existing == null) or binds to a_existing of memory type a_existingType.
	bool MakeBuffer(Gpu& a_gpu, const char* a_what, VkBufferUsageFlags a_usage, VkMemoryPropertyFlags a_want,
		VkMemoryPropertyFlags a_avoid, Buffer& a_out, VkDeviceMemory a_existing = VK_NULL_HANDLE,
		uint32_t a_existingType = UINT32_MAX, const void* a_bufferNext = nullptr)
	{
		VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
		bci.pNext = a_bufferNext;
		bci.size = kSize;
		bci.usage = a_usage;
		bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		VkResult r = vkCreateBuffer(a_gpu.dev, &bci, nullptr, &a_out.buffer);
		if (r != VK_SUCCESS) {
			std::printf("  %s: %s vkCreateBuffer %s\n", a_gpu.label, a_what, VkName(r));
			return false;
		}
		VkMemoryRequirements req{};
		vkGetBufferMemoryRequirements(a_gpu.dev, a_out.buffer, &req);
		if (a_existing) {
			if (!(req.memoryTypeBits & (1u << a_existingType))) {
				std::printf("  %s: %s cannot use memory type %u (buffer allows 0x%x)\n", a_gpu.label, a_what,
					a_existingType, req.memoryTypeBits);
				return false;
			}
			a_out.memory = a_existing;
		} else {
			const uint32_t type = FindType(a_gpu, req.memoryTypeBits, a_want, a_avoid);
			if (type == UINT32_MAX) {
				std::printf("  %s: %s no memory type with flags 0x%x\n", a_gpu.label, a_what, a_want);
				return false;
			}
			VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
			ai.allocationSize = req.size;
			ai.memoryTypeIndex = type;
			r = vkAllocateMemory(a_gpu.dev, &ai, nullptr, &a_out.memory);
			if (r != VK_SUCCESS) {
				std::printf("  %s: %s vkAllocateMemory %s\n", a_gpu.label, a_what, VkName(r));
				return false;
			}
			a_out.ownsMemory = true;
			if (a_want & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
				vkMapMemory(a_gpu.dev, a_out.memory, 0, VK_WHOLE_SIZE, 0, &a_out.mapped);
			}
		}
		r = vkBindBufferMemory(a_gpu.dev, a_out.buffer, a_out.memory, 0);
		if (r != VK_SUCCESS) {
			std::printf("  %s: %s vkBindBufferMemory %s\n", a_gpu.label, a_what, VkName(r));
			return false;
		}
		return true;
	}

	double HeapUsageMiB(const Gpu& a_gpu, uint32_t a_heap)
	{
		if (!a_gpu.budget) return -1.0;
		VkPhysicalDeviceMemoryBudgetPropertiesEXT b{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT };
		VkPhysicalDeviceMemoryProperties2 p2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2 };
		p2.pNext = &b;
		vkGetPhysicalDeviceMemoryProperties2(a_gpu.phys, &p2);
		return static_cast<double>(b.heapUsage[a_heap]) / (1024.0 * 1024.0);
	}

	// kReps copies a_src -> a_dst between two timestamps. False on a GPU failure (already printed).
	bool TimedCopies(Gpu& a_gpu, const char* a_what, VkBuffer a_src, VkBuffer a_dst)
	{
		VkQueryPoolCreateInfo qpci{ VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
		qpci.queryType = VK_QUERY_TYPE_TIMESTAMP;
		qpci.queryCount = 2;
		VkQueryPool pool = VK_NULL_HANDLE;
		vkCreateQueryPool(a_gpu.dev, &qpci, nullptr, &pool);

		Begin(a_gpu);
		vkCmdResetQueryPool(a_gpu.cmd, pool, 0, 2);
		vkCmdWriteTimestamp(a_gpu.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, pool, 0);
		const VkBufferCopy region{ 0, 0, kSize };
		for (uint32_t i = 0; i < kReps; ++i) {
			if (i) TransferBarrier(a_gpu);
			vkCmdCopyBuffer(a_gpu.cmd, a_src, a_dst, 1, &region);
		}
		vkCmdWriteTimestamp(a_gpu.cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, pool, 1);
		const VkResult r = Submit(a_gpu);
		if (r != VK_SUCCESS) {
			std::printf("  %s: %s FAILED: %s\n", a_gpu.label, a_what, VkName(r));
			return false;
		}
		uint64_t ts[2]{};
		vkGetQueryPoolResults(a_gpu.dev, pool, 0, 2, sizeof ts, ts, sizeof(uint64_t),
			VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
		vkDestroyQueryPool(a_gpu.dev, pool, nullptr);
		const double ms = static_cast<double>(ts[1] - ts[0]) * a_gpu.props.limits.timestampPeriod / 1e6;
		const double gbps = static_cast<double>(kSize) * kReps / (ms / 1000.0) / 1e9;
		std::printf("  %s: %s %u x %llu MiB: %.2f ms per 64 MiB = %.2f GB/s\n", a_gpu.label, a_what, kReps,
			static_cast<unsigned long long>(kSize >> 20), ms / kReps, gbps);
		return true;
	}

	bool ClearOnAmd(Gpu& a_amd, VkBuffer a_buffer)
	{
		Begin(a_amd);
		vkCmdFillBuffer(a_amd.cmd, a_buffer, 0, VK_WHOLE_SIZE, 0);
		const VkResult r = Submit(a_amd);
		if (r != VK_SUCCESS) std::printf("  AMD clear: %s\n", VkName(r));
		return r == VK_SUCCESS;
	}

	// The AMD GPU copies its VRAM buffer into host memory; every dword is compared with the pattern.
	bool VerifyOnAmd(Gpu& a_amd, VkBuffer a_vram, Buffer& a_readback, uint32_t a_seed)
	{
		Begin(a_amd);
		const VkBufferCopy region{ 0, 0, kSize };
		vkCmdCopyBuffer(a_amd.cmd, a_vram, a_readback.buffer, 1, &region);
		const VkResult r = Submit(a_amd);
		if (r != VK_SUCCESS) {
			std::printf("  AMD readback: %s\n", VkName(r));
			return false;
		}
		const auto* data = static_cast<const uint32_t*>(a_readback.mapped);
		const size_t count = static_cast<size_t>(kSize / 4);
		size_t bad = 0;
		size_t firstBad = SIZE_MAX;
		for (size_t i = 0; i < count; ++i) {
			if (data[i] != Pattern(i, a_seed)) {
				if (firstBad == SIZE_MAX) firstBad = i;
				++bad;
			}
		}
		if (bad == 0) {
			std::printf("  AMD readback: all %zu dwords match - the data is in the 9060's VRAM\n", count);
		} else {
			std::printf("  AMD readback: %zu of %zu dwords wrong (first at %zu: 0x%08x, expected 0x%08x)\n", bad, count,
				firstBad, data[firstBad], Pattern(firstBad, a_seed));
		}
		return bad == 0;
	}

	// ---- D3D12 on the NVIDIA GPU ------------------------------------------------------------

	struct D3D12Gpu
	{
		ComPtr<ID3D12Device3> dev;
		ComPtr<ID3D12CommandQueue> queue;
		ComPtr<ID3D12CommandAllocator> allocator;
		ComPtr<ID3D12GraphicsCommandList> list;
		ComPtr<ID3D12Fence> fence;
		UINT64 fenceValue = 0;
		HANDLE event = nullptr;

		bool Create(uint32_t a_vendor)
		{
			ComPtr<IDXGIFactory1> factory;
			if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;
			for (UINT i = 0;; ++i) {
				ComPtr<IDXGIAdapter1> adapter;
				if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;
				DXGI_ADAPTER_DESC1 desc{};
				adapter->GetDesc1(&desc);
				if (desc.VendorId != a_vendor || (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) continue;
				if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev)))) {
					std::wprintf(L"  D3D12 device on %ls\n", desc.Description);
					break;
				}
			}
			if (!dev) return false;
			D3D12_COMMAND_QUEUE_DESC qd{};
			qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
			dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue));
			dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
			dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list));
			list->Close();
			dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
			event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
			return queue && list && fence;
		}

		void Begin()
		{
			allocator->Reset();
			list->Reset(allocator.Get(), nullptr);
		}

		bool Execute(const char* a_what)
		{
			HRESULT hr = list->Close();
			if (FAILED(hr)) {
				std::printf("  D3D12 %s: Close 0x%08lx\n", a_what, static_cast<unsigned long>(hr));
				return false;
			}
			ID3D12CommandList* lists[] = { list.Get() };
			queue->ExecuteCommandLists(1, lists);
			queue->Signal(fence.Get(), ++fenceValue);
			fence->SetEventOnCompletion(fenceValue, event);
			if (WaitForSingleObject(event, kTimeoutMs) != WAIT_OBJECT_0) {
				std::printf("  D3D12 %s: TIMEOUT after %lu ms, removed reason 0x%08lx\n", a_what, kTimeoutMs,
					static_cast<unsigned long>(dev->GetDeviceRemovedReason()));
				return false;
			}
			hr = dev->GetDeviceRemovedReason();
			if (FAILED(hr)) {
				std::printf("  D3D12 %s: device removed 0x%08lx\n", a_what, static_cast<unsigned long>(hr));
				return false;
			}
			return true;
		}

		ComPtr<ID3D12Resource> MakeBuffer(D3D12_HEAP_TYPE a_type, D3D12_RESOURCE_STATES a_state)
		{
			D3D12_HEAP_PROPERTIES hp{};
			hp.Type = a_type;
			D3D12_RESOURCE_DESC rd{};
			rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			rd.Width = kSize;
			rd.Height = 1;
			rd.DepthOrArraySize = 1;
			rd.MipLevels = 1;
			rd.SampleDesc.Count = 1;
			rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			ComPtr<ID3D12Resource> r;
			dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, a_state, nullptr, IID_PPV_ARGS(&r));
			return r;
		}
	};

	void PrintHeap(ID3D12Heap* a_heap)
	{
		const D3D12_HEAP_DESC d = a_heap->GetDesc();
		std::printf("    heap: %.1f MiB, type %d, cpu page %d, pool L%d, flags 0x%x\n",
			static_cast<double>(d.SizeInBytes) / (1024.0 * 1024.0), static_cast<int>(d.Properties.Type),
			static_cast<int>(d.Properties.CPUPageProperty), static_cast<int>(d.Properties.MemoryPoolPreference) - 1,
			static_cast<unsigned>(d.Flags));
	}

	// The D3D12 copy test: pattern -> 3090 VRAM -> a buffer placed in a_heap (the 9060's VRAM).
	bool D3D12Dma(D3D12Gpu& a_nv, ID3D12Heap* a_heap)
	{
		const D3D12_HEAP_DESC hd = a_heap->GetDesc();
		D3D12_RESOURCE_DESC rd{};
		rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		rd.Width = kSize;
		rd.Height = 1;
		rd.DepthOrArraySize = 1;
		rd.MipLevels = 1;
		rd.SampleDesc.Count = 1;
		rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		if (hd.Flags & D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER) rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;
		ComPtr<ID3D12Resource> dst;
		HRESULT hr = a_nv.dev->CreatePlacedResource(a_heap, 0, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr,
			IID_PPV_ARGS(&dst));
		if (FAILED(hr)) {
			std::printf("  D3D12: CreatePlacedResource in the opened heap 0x%08lx\n", static_cast<unsigned long>(hr));
			return false;
		}

		ComPtr<ID3D12Resource> upload = a_nv.MakeBuffer(D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
		ComPtr<ID3D12Resource> src = a_nv.MakeBuffer(D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON);
		D3D12_HEAP_PROPERTIES rbp{};
		rbp.Type = D3D12_HEAP_TYPE_READBACK;
		D3D12_RESOURCE_DESC rbd = rd;
		rbd.Width = 2 * sizeof(UINT64);
		rbd.Flags = D3D12_RESOURCE_FLAG_NONE;
		ComPtr<ID3D12Resource> stamps;
		a_nv.dev->CreateCommittedResource(&rbp, D3D12_HEAP_FLAG_NONE, &rbd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
			IID_PPV_ARGS(&stamps));
		D3D12_QUERY_HEAP_DESC qhd{ D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 2, 0 };
		ComPtr<ID3D12QueryHeap> queries;
		a_nv.dev->CreateQueryHeap(&qhd, IID_PPV_ARGS(&queries));
		if (!upload || !src || !stamps || !queries) {
			std::printf("  D3D12: helper resources failed\n");
			return false;
		}

		void* mapped = nullptr;
		upload->Map(0, nullptr, &mapped);
		auto* words = static_cast<uint32_t*>(mapped);
		for (size_t i = 0; i < kSize / 4; ++i) words[i] = Pattern(i, kSeedD3D12);
		upload->Unmap(0, nullptr);

		a_nv.Begin();
		a_nv.list->CopyBufferRegion(src.Get(), 0, upload.Get(), 0, kSize);
		if (!a_nv.Execute("upload")) return false;

		// Both buffers decayed to COMMON; the copies promote them implicitly.
		a_nv.Begin();
		a_nv.list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
		for (uint32_t i = 0; i < kReps; ++i) {
			if (i) {
				D3D12_RESOURCE_BARRIER b{};
				b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
				a_nv.list->ResourceBarrier(1, &b);
			}
			a_nv.list->CopyBufferRegion(dst.Get(), 0, src.Get(), 0, kSize);
		}
		a_nv.list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
		a_nv.list->ResolveQueryData(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, stamps.Get(), 0);
		if (!a_nv.Execute("P2P copies")) return false;

		UINT64 freq = 0;
		a_nv.queue->GetTimestampFrequency(&freq);
		UINT64* ts = nullptr;
		const D3D12_RANGE readRange{ 0, 2 * sizeof(UINT64) };
		stamps->Map(0, &readRange, reinterpret_cast<void**>(&ts));
		const double ms = static_cast<double>(ts[1] - ts[0]) * 1000.0 / static_cast<double>(freq);
		const D3D12_RANGE noWrite{ 0, 0 };
		stamps->Unmap(0, &noWrite);
		std::printf("  D3D12 3090 -> 9060 VRAM %u x %llu MiB: %.2f ms per 64 MiB = %.2f GB/s\n", kReps,
			static_cast<unsigned long long>(kSize >> 20), ms / kReps,
			static_cast<double>(kSize) * kReps / (ms / 1000.0) / 1e9);
		return true;
	}

	// ---- --bench: every direction at the mod's lane sizes, D3D12 copy queues ---------------

	struct DxQueue
	{
		ComPtr<ID3D12CommandQueue> queue;
		ComPtr<ID3D12CommandAllocator> allocator;
		ComPtr<ID3D12GraphicsCommandList> list;
		ComPtr<ID3D12QueryHeap> queries;
		ComPtr<ID3D12Resource> stamps;
		UINT64 frequency = 0;
	};

	struct Dx
	{
		ComPtr<ID3D12Device3> dev;
		DxQueue copy;
		DxQueue direct;
		ComPtr<ID3D12Fence> fence;
		UINT64 value = 0;
		HANDLE event = nullptr;

		bool InitQueue(DxQueue& a_q, D3D12_COMMAND_LIST_TYPE a_type)
		{
			D3D12_COMMAND_QUEUE_DESC qd{};
			qd.Type = a_type;
			if (FAILED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&a_q.queue)))) return false;
			dev->CreateCommandAllocator(a_type, IID_PPV_ARGS(&a_q.allocator));
			dev->CreateCommandList(0, a_type, a_q.allocator.Get(), nullptr, IID_PPV_ARGS(&a_q.list));
			a_q.list->Close();
			D3D12_QUERY_HEAP_DESC qhd{};
			qhd.Type = a_type == D3D12_COMMAND_LIST_TYPE_COPY ? D3D12_QUERY_HEAP_TYPE_COPY_QUEUE_TIMESTAMP :
			                                                    D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
			qhd.Count = 2;
			if (FAILED(dev->CreateQueryHeap(&qhd, IID_PPV_ARGS(&a_q.queries)))) return false;
			a_q.stamps = Buffer(D3D12_HEAP_TYPE_READBACK, 2 * sizeof(UINT64), D3D12_RESOURCE_STATE_COPY_DEST);
			a_q.queue->GetTimestampFrequency(&a_q.frequency);
			return a_q.list && a_q.stamps && a_q.frequency;
		}

		bool Create(uint32_t a_vendor)
		{
			ComPtr<IDXGIFactory1> factory;
			if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;
			for (UINT i = 0;; ++i) {
				ComPtr<IDXGIAdapter1> adapter;
				if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;
				DXGI_ADAPTER_DESC1 desc{};
				adapter->GetDesc1(&desc);
				if (desc.VendorId != a_vendor || (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) continue;
				if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev)))) break;
			}
			if (!dev) return false;
			dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
			event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
			return InitQueue(copy, D3D12_COMMAND_LIST_TYPE_COPY) && InitQueue(direct, D3D12_COMMAND_LIST_TYPE_DIRECT);
		}

		ComPtr<ID3D12Resource> Buffer(D3D12_HEAP_TYPE a_type, UINT64 a_size, D3D12_RESOURCE_STATES a_state)
		{
			D3D12_HEAP_PROPERTIES hp{};
			hp.Type = a_type;
			D3D12_RESOURCE_DESC rd{};
			rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			rd.Width = a_size;
			rd.Height = 1;
			rd.DepthOrArraySize = 1;
			rd.MipLevels = 1;
			rd.SampleDesc.Count = 1;
			rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			ComPtr<ID3D12Resource> r;
			dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, a_state, nullptr, IID_PPV_ARGS(&r));
			return r;
		}

		ComPtr<ID3D12Resource> Placed(ID3D12Heap* a_heap)
		{
			D3D12_RESOURCE_DESC rd{};
			rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			rd.Width = kSize;
			rd.Height = 1;
			rd.DepthOrArraySize = 1;
			rd.MipLevels = 1;
			rd.SampleDesc.Count = 1;
			rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			if (a_heap->GetDesc().Flags & D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER) {
				rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;
			}
			ComPtr<ID3D12Resource> r;
			dev->CreatePlacedResource(a_heap, 0, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&r));
			return r;
		}

		bool Run(DxQueue& a_q)
		{
			if (FAILED(a_q.list->Close())) return false;
			ID3D12CommandList* lists[] = { a_q.list.Get() };
			a_q.queue->ExecuteCommandLists(1, lists);
			a_q.queue->Signal(fence.Get(), ++value);
			fence->SetEventOnCompletion(value, event);
			if (WaitForSingleObject(event, kTimeoutMs) != WAIT_OBJECT_0) {
				std::printf("  TIMEOUT, removed reason 0x%08lx\n", static_cast<unsigned long>(dev->GetDeviceRemovedReason()));
				return false;
			}
			return SUCCEEDED(dev->GetDeviceRemovedReason());
		}

		// Fills a DEFAULT buffer with the pattern through an upload buffer.
		bool Fill(ID3D12Resource* a_dst, uint32_t a_seed)
		{
			ComPtr<ID3D12Resource> upload = Buffer(D3D12_HEAP_TYPE_UPLOAD, kSize, D3D12_RESOURCE_STATE_GENERIC_READ);
			if (!upload) return false;
			void* mapped = nullptr;
			upload->Map(0, nullptr, &mapped);
			auto* words = static_cast<uint32_t*>(mapped);
			for (size_t i = 0; i < kSize / 4; ++i) words[i] = Pattern(i, a_seed);
			upload->Unmap(0, nullptr);
			copy.allocator->Reset();
			copy.list->Reset(copy.allocator.Get(), nullptr);
			copy.list->CopyBufferRegion(a_dst, 0, upload.Get(), 0, kSize);
			return Run(copy);
		}

		// ms per copy of a_size bytes, a_reps copies back to back; negative on failure.
		double Time(DxQueue& a_q, ID3D12Resource* a_dst, ID3D12Resource* a_src, UINT64 a_size, UINT a_reps)
		{
			a_q.allocator->Reset();
			a_q.list->Reset(a_q.allocator.Get(), nullptr);
			a_q.list->EndQuery(a_q.queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
			for (UINT i = 0; i < a_reps; ++i) a_q.list->CopyBufferRegion(a_dst, 0, a_src, 0, a_size);
			a_q.list->EndQuery(a_q.queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
			a_q.list->ResolveQueryData(a_q.queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, a_q.stamps.Get(), 0);
			if (!Run(a_q)) return -1.0;
			UINT64* ts = nullptr;
			const D3D12_RANGE readRange{ 0, 2 * sizeof(UINT64) };
			a_q.stamps->Map(0, &readRange, reinterpret_cast<void**>(&ts));
			const double ms = static_cast<double>(ts[1] - ts[0]) * 1000.0 / static_cast<double>(a_q.frequency);
			const D3D12_RANGE noWrite{ 0, 0 };
			a_q.stamps->Unmap(0, &noWrite);
			return ms / a_reps;
		}

		ComPtr<ID3D12Resource> readback;

		// a_src (a_bytes) -> a readback buffer on a_q; every dword is compared with the pattern.
		bool Verify(DxQueue& a_q, ID3D12Resource* a_src, uint32_t a_seed, const char* a_what, UINT64 a_bytes = kSize,
			bool a_quiet = false)
		{
			if (!readback) readback = Buffer(D3D12_HEAP_TYPE_READBACK, kSize, D3D12_RESOURCE_STATE_COPY_DEST);
			a_q.allocator->Reset();
			a_q.list->Reset(a_q.allocator.Get(), nullptr);
			a_q.list->CopyBufferRegion(readback.Get(), 0, a_src, 0, a_bytes);
			if (!Run(a_q)) {
				std::printf("    %s: FAILED\n", a_what);
				return false;
			}
			void* mapped = nullptr;
			const D3D12_RANGE readRange{ 0, static_cast<SIZE_T>(kSize) };
			readback->Map(0, &readRange, &mapped);
			const auto* words = static_cast<const uint32_t*>(mapped);
			size_t bad = 0;
			size_t firstBad = SIZE_MAX;
			for (size_t i = 0; i < a_bytes / 4; ++i) {
				if (words[i] != Pattern(i, a_seed)) {
					if (firstBad == SIZE_MAX) firstBad = i;
					++bad;
				}
			}
			const uint32_t got = bad ? words[firstBad] : 0;
			const D3D12_RANGE noWrite{ 0, 0 };
			readback->Unmap(0, &noWrite);
			if (bad) {
				std::printf("    %s: %zu of %llu dwords WRONG (first at %zu: 0x%08x, expected 0x%08x)\n", a_what, bad,
					static_cast<unsigned long long>(a_bytes / 4), firstBad, got, Pattern(firstBad, a_seed));
			} else if (!a_quiet) {
				std::printf("    %s: all %llu dwords correct\n", a_what, static_cast<unsigned long long>(a_bytes / 4));
			}
			return bad == 0;
		}

		bool CopyOnce(DxQueue& a_q, ID3D12Resource* a_dst, ID3D12Resource* a_src)
		{
			a_q.allocator->Reset();
			a_q.list->Reset(a_q.allocator.Get(), nullptr);
			a_q.list->CopyBufferRegion(a_dst, 0, a_src, 0, kSize);
			return Run(a_q);
		}
	};

	// Reads every 1024th dword of a BAR mapping with the CPU (uncached reads over PCIe).
	void SampleBar(const char* a_what, const void* a_bar, uint32_t a_seed)
	{
		const auto* words = static_cast<const volatile uint32_t*>(a_bar);
		size_t checked = 0;
		size_t bad = 0;
		for (size_t i = 0; i < kSize / 4; i += 1024, ++checked) {
			if (words[i] != Pattern(i, a_seed)) ++bad;
		}
		std::printf("  check %s: %zu of %zu sampled dwords correct\n", a_what, checked - bad, checked);
	}

	constexpr UINT64 kBenchSizes[] = { 1ull << 20, 4ull << 20, 14'745'600ull, 33'177'600ull, 64ull << 20 };
	constexpr size_t kBenchCount = std::size(kBenchSizes);
	constexpr UINT kBenchReps = 16;

	struct BenchRow
	{
		const char* name = "";
		double ms[kBenchCount]{};
		bool ok = false;
	};

	BenchRow BenchPath(const char* a_name, Dx& a_gpu, DxQueue& a_q, ID3D12Resource* a_dst, ID3D12Resource* a_src)
	{
		BenchRow row;
		row.name = a_name;
		if (!a_dst || !a_src) {
			std::printf("  %-44s skipped (resource missing)\n", a_name);
			return row;
		}
		if (a_gpu.Time(a_q, a_dst, a_src, kSize, 2) < 0) {  // warm-up: page tables, clocks
			std::printf("  %-44s FAILED\n", a_name);
			return row;
		}
		std::printf("  %-44s", a_name);
		for (size_t i = 0; i < kBenchCount; ++i) {
			row.ms[i] = a_gpu.Time(a_q, a_dst, a_src, kBenchSizes[i], kBenchReps);
			if (row.ms[i] < 0) {
				std::printf(" FAILED\n");
				return row;
			}
			std::printf(" %6.2f ms %5.1f |", row.ms[i], static_cast<double>(kBenchSizes[i]) / (row.ms[i] / 1000.0) / 1e9);
		}
		std::printf("\n");
		row.ok = true;
		return row;
	}

	int RunBench(void* a_barAmd, void* a_barNv)
	{
		constexpr uint32_t kSeedA = 0xA0A0A0A0u;
		constexpr uint32_t kSeedB = 0xB0B0B0B0u;
		Dx a;
		Dx b;
		if (!a.Create(kVendorNvidia) || !b.Create(kVendorAmd)) {
			std::printf("  D3D12 devices failed\n");
			return 1;
		}

		ComPtr<ID3D12Resource> vramA = a.Buffer(D3D12_HEAP_TYPE_DEFAULT, kSize, D3D12_RESOURCE_STATE_COMMON);
		ComPtr<ID3D12Resource> vramB = b.Buffer(D3D12_HEAP_TYPE_DEFAULT, kSize, D3D12_RESOURCE_STATE_COMMON);
		ComPtr<ID3D12Resource> vramB2 = b.Buffer(D3D12_HEAP_TYPE_DEFAULT, kSize, D3D12_RESOURCE_STATE_COMMON);
		if (!vramA || !vramB || !vramB2 || !a.Fill(vramA.Get(), kSeedA) || !b.Fill(vramB.Get(), kSeedB)) {
			std::printf("  VRAM buffers failed\n");
			return 1;
		}

		// Today's transport: a cross-adapter heap in system memory, created on the 3090, opened on the 9060.
		ComPtr<ID3D12Resource> sysOnA;
		ComPtr<ID3D12Resource> sysOnB;
		{
			D3D12_HEAP_DESC hd{};
			hd.SizeInBytes = kSize;
			hd.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
			hd.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
			hd.Flags = D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER;
			ComPtr<ID3D12Heap> heapA;
			ComPtr<ID3D12Heap> heapB;
			HANDLE handle = nullptr;
			if (SUCCEEDED(a.dev->CreateHeap(&hd, IID_PPV_ARGS(&heapA))) &&
				SUCCEEDED(a.dev->CreateSharedHandle(heapA.Get(), nullptr, GENERIC_ALL, nullptr, &handle)) &&
				SUCCEEDED(b.dev->OpenSharedHandle(handle, IID_PPV_ARGS(&heapB)))) {
				sysOnA = a.Placed(heapA.Get());
				sysOnB = b.Placed(heapB.Get());
			}
			if (handle) CloseHandle(handle);
		}

		// Peer heaps: each GPU opens the other GPU's BAR mapping as if it were system memory.
		ComPtr<ID3D12Resource> peerOnA;  // 9060 VRAM, seen by the 3090
		ComPtr<ID3D12Resource> peerOnB;  // 3090 VRAM, seen by the 9060
		{
			ComPtr<ID3D12Heap> heap;
			HRESULT hr = a.dev->OpenExistingHeapFromAddress(a_barAmd, IID_PPV_ARGS(&heap));
			std::printf("  3090 opens the 9060's BAR mapping: 0x%08lx\n", static_cast<unsigned long>(hr));
			if (heap) peerOnA = a.Placed(heap.Get());
			heap.Reset();
			hr = b.dev->OpenExistingHeapFromAddress(a_barNv, IID_PPV_ARGS(&heap));
			std::printf("  9060 opens the 3090's BAR mapping: 0x%08lx\n", static_cast<unsigned long>(hr));
			if (heap) peerOnB = b.Placed(heap.Get());
		}

		std::printf("\n  %-44s", "ms per copy, GB/s");
		for (UINT64 size : kBenchSizes) std::printf(" %8.1f MB      |", static_cast<double>(size) / 1e6);
		std::printf("\n");

		BenchRow rows[8];
		rows[0] = BenchPath("today 1: 3090 -> system RAM (copy queue)", a, a.copy, sysOnA.Get(), vramA.Get());
		rows[1] = BenchPath("today 2: system RAM -> 9060 (copy queue)", b, b.copy, vramB2.Get(), sysOnB.Get());
		rows[2] = BenchPath("P2P: 3090 writes 9060 VRAM (copy queue)", a, a.copy, peerOnA.Get(), vramA.Get());
		rows[3] = BenchPath("P2P: 3090 writes 9060 VRAM (direct queue)", a, a.direct, peerOnA.Get(), vramA.Get());
		if (peerOnA) SampleBar("9060 VRAM after the 3090's writes", a_barAmd, kSeedA);
		rows[7] = BenchPath("local: 9060 VRAM -> VRAM (copy queue)", b, b.copy, vramB2.Get(), vramB.Get());
		// Directions never tried before this run: reads across PCIe, and the 9060 as the writer.
		rows[4] = BenchPath("P2P: 3090 reads 9060 VRAM (copy queue)", a, a.copy, vramA.Get(), peerOnA.Get());
		rows[5] = BenchPath("P2P: 9060 writes 3090 VRAM (copy queue)", b, b.copy, peerOnB.Get(), vramB.Get());
		if (peerOnB) SampleBar("3090 VRAM after the 9060's writes", a_barNv, kSeedB);
		rows[6] = BenchPath("P2P: 9060 reads 3090 VRAM (copy queue)", b, b.copy, vramB2.Get(), peerOnB.Get());

		// The mod's 4K FSR 4.1 Quality payload: three 14.7 MB lanes (scene, depth, motion vectors) + the
		// 33.2 MB finished frame. Index 2 = 14.7 MB, 3 = 33.2 MB.
		if (rows[0].ok && rows[1].ok && rows[2].ok && rows[7].ok) {
			auto lanes = [](const BenchRow& r) { return 3.0 * r.ms[2] + r.ms[3]; };
			std::printf("\n  4K FSR 4.1 Quality payload (3 x 14.7 MB + 33.2 MB = 77.4 MB):\n");
			std::printf("    today: 3090 copy %.2f ms, 9060 read %.2f ms; the finished frame alone %.2f + %.2f = %.2f ms\n",
				lanes(rows[0]), lanes(rows[1]), rows[0].ms[3], rows[1].ms[3], rows[0].ms[3] + rows[1].ms[3]);
			std::printf("    P2P:   3090 copy %.2f ms, 9060 local copy %.2f ms; the finished frame alone %.2f + %.2f = %.2f ms\n",
				lanes(rows[2]), lanes(rows[7]), rows[2].ms[3], rows[7].ms[3], rows[2].ms[3] + rows[7].ms[3]);
		}
		return 0;
	}

	// ---- --interop: can the 9060's D3D12 device use a CPU-mappable block of its own VRAM? --------

	struct AmdBlock
	{
		const char* how = "";
		void* ptr = nullptr;                 // CPU mapping through the 9060's BAR (what the 3090 opens)
		VkDeviceMemory memory = VK_NULL_HANDLE;
		VkBuffer dedicated = VK_NULL_HANDLE;
		ComPtr<ID3D12Heap> heap;             // on the 9060's D3D12 device, when the block is a heap
		ComPtr<ID3D12Resource> view;         // buffer over the block on the 9060's D3D12 device

		[[nodiscard]] ID3D12Pageable* Pageable() const
		{
			return heap ? static_cast<ID3D12Pageable*>(heap.Get()) : static_cast<ID3D12Pageable*>(view.Get());
		}
	};

	void PrintTypes(const Gpu& a_gpu, uint32_t a_bits)
	{
		for (uint32_t i = 0; i < a_gpu.mem.memoryTypeCount; ++i) {
			if (!(a_bits & (1u << i))) continue;
			const auto f = a_gpu.mem.memoryTypes[i].propertyFlags;
			std::printf("      type %u:%s%s%s\n", i, (f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ? " DEVICE_LOCAL" : "",
				(f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) ? " HOST_VISIBLE" : "",
				(f & VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD) ? " DEVICE_COHERENT_AMD" : "");
		}
	}

	// Route 1: D3D12 owns the VRAM (a shared heap); Vulkan imports it only to map it for the CPU.
	bool BlockFromD3D12(Gpu& a_amd, Dx& a_b, AmdBlock& a_out)
	{
		std::printf("\n  route 1: the 9060's D3D12 creates a shared heap, Vulkan imports and maps it\n");
		D3D12_HEAP_DESC hd{};
		hd.SizeInBytes = kSize;
		hd.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
		hd.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
		hd.Flags = D3D12_HEAP_FLAG_SHARED;
		ComPtr<ID3D12Heap> heap;
		HANDLE handle = nullptr;
		HRESULT hr = a_b.dev->CreateHeap(&hd, IID_PPV_ARGS(&heap));
		if (SUCCEEDED(hr)) hr = a_b.dev->CreateSharedHandle(heap.Get(), nullptr, GENERIC_ALL, nullptr, &handle);
		if (FAILED(hr)) {
			std::printf("    shared heap: 0x%08lx\n", static_cast<unsigned long>(hr));
			return false;
		}
		VkMemoryWin32HandlePropertiesKHR props{ VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR };
		VkResult r = vkGetMemoryWin32HandlePropertiesKHR(a_amd.dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_HEAP_BIT, handle,
			&props);
		std::printf("    Vulkan import properties: %s, memory types offered 0x%x\n", VkName(r), props.memoryTypeBits);
		PrintTypes(a_amd, props.memoryTypeBits);
		const uint32_t type = FindType(a_amd, props.memoryTypeBits,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD);
		if (r != VK_SUCCESS || type == UINT32_MAX) {
			std::printf("    -> no CPU-mappable type offered for a D3D12 heap\n");
			CloseHandle(handle);
			return false;
		}
		VkImportMemoryWin32HandleInfoKHR imp{ VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR };
		imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_HEAP_BIT;
		imp.handle = handle;
		VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
		ai.pNext = &imp;
		ai.allocationSize = kSize;
		ai.memoryTypeIndex = type;
		r = vkAllocateMemory(a_amd.dev, &ai, nullptr, &a_out.memory);
		CloseHandle(handle);
		std::printf("    import as type %u: %s\n", type, VkName(r));
		if (r != VK_SUCCESS) return false;
		r = vkMapMemory(a_amd.dev, a_out.memory, 0, VK_WHOLE_SIZE, 0, &a_out.ptr);
		std::printf("    map: %s\n", VkName(r));
		if (r != VK_SUCCESS) return false;
		a_out.how = "D3D12 heap, mapped by Vulkan";
		a_out.heap = heap;
		a_out.view = a_b.Placed(heap.Get());
		return a_out.view != nullptr;
	}

	// Route 2: Vulkan owns the VRAM (CPU-mappable type), the 9060's D3D12 device opens the exported handle.
	// Every CPU-mappable VRAM type is tried, plain and as a dedicated allocation.
	bool BlockFromVulkan(Gpu& a_amd, Dx& a_b, AmdBlock& a_out)
	{
		std::printf("\n  route 2: Vulkan allocates CPU-mappable VRAM and exports it, the 9060's D3D12 opens it\n");
		VkPhysicalDeviceExternalBufferInfo info{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO };
		info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
		info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
		VkExternalBufferProperties ep{ VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES };
		vkGetPhysicalDeviceExternalBufferProperties(a_amd.phys, &info, &ep);
		const auto features = ep.externalMemoryProperties.externalMemoryFeatures;
		std::printf("    OPAQUE_WIN32 export: %s%s\n",
			(features & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) ? "supported" : "NOT supported",
			(features & VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT) ? " (dedicated allocations only)" : "");
		if (!(features & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT)) return false;

		constexpr VkMemoryPropertyFlags kWant = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
		for (uint32_t type = 0; type < a_amd.mem.memoryTypeCount; ++type) {
			if ((a_amd.mem.memoryTypes[type].propertyFlags & kWant) != kWant) continue;
			for (int dedicated = 0; dedicated < 2; ++dedicated) {
				AmdBlock block;
				VkExternalMemoryBufferCreateInfo emb{ VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO };
				emb.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
				if (dedicated) {
					VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
					bci.pNext = &emb;
					bci.size = kSize;
					bci.usage = info.usage;
					vkCreateBuffer(a_amd.dev, &bci, nullptr, &block.dedicated);
				}
				VkMemoryDedicatedAllocateInfo ded{ VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO };
				ded.buffer = block.dedicated;
				VkExportMemoryAllocateInfo exp{ VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO };
				exp.pNext = dedicated ? &ded : nullptr;
				exp.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
				VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
				ai.pNext = &exp;
				ai.allocationSize = kSize;
				ai.memoryTypeIndex = type;
				VkResult r = vkAllocateMemory(a_amd.dev, &ai, nullptr, &block.memory);
				std::printf("    exportable allocation, type %u%s: %s\n", type, dedicated ? " (dedicated)" : "",
					VkName(r));
				auto cleanup = [&] {
					if (block.memory) vkFreeMemory(a_amd.dev, block.memory, nullptr);
					if (block.dedicated) vkDestroyBuffer(a_amd.dev, block.dedicated, nullptr);
				};
				if (r != VK_SUCCESS) {
					cleanup();
					continue;
				}
				if (block.dedicated) vkBindBufferMemory(a_amd.dev, block.dedicated, block.memory, 0);
				r = vkMapMemory(a_amd.dev, block.memory, 0, VK_WHOLE_SIZE, 0, &block.ptr);
				VkMemoryGetWin32HandleInfoKHR gi{ VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR };
				gi.memory = block.memory;
				gi.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
				HANDLE handle = nullptr;
				if (r == VK_SUCCESS) r = vkGetMemoryWin32HandleKHR(a_amd.dev, &gi, &handle);
				if (r != VK_SUCCESS) {
					std::printf("      map / export: %s\n", VkName(r));
					cleanup();
					continue;
				}
				HRESULT hr = a_b.dev->OpenSharedHandle(handle, IID_PPV_ARGS(&block.heap));
				if (FAILED(hr)) hr = a_b.dev->OpenSharedHandle(handle, IID_PPV_ARGS(&block.view));
				CloseHandle(handle);
				std::printf("      D3D12 OpenSharedHandle: 0x%08lx\n", static_cast<unsigned long>(hr));
				if (block.heap) block.view = a_b.Placed(block.heap.Get());
				if (!block.view) {
					block.heap.Reset();
					cleanup();
					continue;
				}
				block.how = "Vulkan allocation, opened by D3D12";
				a_out = block;
				return true;
			}
		}
		return false;
	}

	// Route 3: the block stays a plain (non-exported) Vulkan allocation. The 9060's own Vulkan queue copies it
	// into a D3D12 heap that Vulkan imported (that direction works), and D3D12 reads the heap. Per frame, the
	// two APIs synchronise through a D3D12 fence imported into Vulkan as a timeline semaphore.
	struct Route3
	{
		VkDeviceMemory blockMem = VK_NULL_HANDLE;
		VkBuffer blockBuf = VK_NULL_HANDLE;
		void* ptr = nullptr;
		ComPtr<ID3D12Heap> landHeap;
		ComPtr<ID3D12Resource> landOnB;
		VkDeviceMemory landMem = VK_NULL_HANDLE;
		VkBuffer landBuf = VK_NULL_HANDLE;
		ComPtr<ID3D12Fence> fence;
		VkSemaphore semaphore = VK_NULL_HANDLE;
		UINT64 value = 0;
	};

	bool SetUpRoute3(Gpu& a_amd, uint32_t a_type, Dx& a_b, Route3& a_r)
	{
		std::printf("\n  route 3: a plain Vulkan block; the 9060's Vulkan copies it into a D3D12 heap it imported\n");
		VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
		ai.allocationSize = kSize;
		ai.memoryTypeIndex = a_type;
		VkResult r = vkAllocateMemory(a_amd.dev, &ai, nullptr, &a_r.blockMem);
		if (r == VK_SUCCESS) r = vkMapMemory(a_amd.dev, a_r.blockMem, 0, VK_WHOLE_SIZE, 0, &a_r.ptr);
		VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
		bci.size = kSize;
		bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
		if (r == VK_SUCCESS) r = vkCreateBuffer(a_amd.dev, &bci, nullptr, &a_r.blockBuf);
		if (r == VK_SUCCESS) r = vkBindBufferMemory(a_amd.dev, a_r.blockBuf, a_r.blockMem, 0);
		std::printf("    block (type %u, mapped at %p): %s\n", a_type, a_r.ptr, VkName(r));
		if (r != VK_SUCCESS) return false;

		D3D12_HEAP_DESC hd{};
		hd.SizeInBytes = kSize;
		hd.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
		hd.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
		hd.Flags = D3D12_HEAP_FLAG_SHARED;
		HANDLE handle = nullptr;
		HRESULT hr = a_b.dev->CreateHeap(&hd, IID_PPV_ARGS(&a_r.landHeap));
		if (SUCCEEDED(hr)) hr = a_b.dev->CreateSharedHandle(a_r.landHeap.Get(), nullptr, GENERIC_ALL, nullptr, &handle);
		if (FAILED(hr)) {
			std::printf("    D3D12 shared heap: 0x%08lx\n", static_cast<unsigned long>(hr));
			return false;
		}
		VkMemoryWin32HandlePropertiesKHR props{ VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR };
		vkGetMemoryWin32HandlePropertiesKHR(a_amd.dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_HEAP_BIT, handle, &props);
		const uint32_t landType = FindType(a_amd, props.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
			VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD);
		VkImportMemoryWin32HandleInfoKHR imp{ VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR };
		imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_HEAP_BIT;
		imp.handle = handle;
		VkMemoryAllocateInfo li{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
		li.pNext = &imp;
		li.allocationSize = kSize;
		li.memoryTypeIndex = landType;
		r = landType == UINT32_MAX ? VK_ERROR_FEATURE_NOT_PRESENT : vkAllocateMemory(a_amd.dev, &li, nullptr, &a_r.landMem);
		CloseHandle(handle);
		VkExternalMemoryBufferCreateInfo emb{ VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO };
		emb.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_HEAP_BIT;
		bci.pNext = &emb;
		if (r == VK_SUCCESS) r = vkCreateBuffer(a_amd.dev, &bci, nullptr, &a_r.landBuf);
		if (r == VK_SUCCESS) r = vkBindBufferMemory(a_amd.dev, a_r.landBuf, a_r.landMem, 0);
		std::printf("    D3D12 heap imported into Vulkan as type %u: %s\n", landType, VkName(r));
		if (r != VK_SUCCESS) return false;
		a_r.landOnB = a_b.Placed(a_r.landHeap.Get());

		HANDLE fenceHandle = nullptr;
		hr = a_b.dev->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&a_r.fence));
		if (SUCCEEDED(hr)) hr = a_b.dev->CreateSharedHandle(a_r.fence.Get(), nullptr, GENERIC_ALL, nullptr, &fenceHandle);
		VkSemaphoreTypeCreateInfo sti{ VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
		sti.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
		VkSemaphoreCreateInfo sci{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
		sci.pNext = &sti;
		r = FAILED(hr) ? VK_ERROR_INITIALIZATION_FAILED : vkCreateSemaphore(a_amd.dev, &sci, nullptr, &a_r.semaphore);
		if (r == VK_SUCCESS) {
			VkImportSemaphoreWin32HandleInfoKHR isi{ VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR };
			isi.semaphore = a_r.semaphore;
			isi.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;
			isi.handle = fenceHandle;
			r = vkImportSemaphoreWin32HandleKHR(a_amd.dev, &isi);
		}
		if (fenceHandle) CloseHandle(fenceHandle);
		std::printf("    D3D12 fence imported into Vulkan as a timeline semaphore: %s\n", VkName(r));
		return r == VK_SUCCESS && a_r.landOnB;
	}

	// One frame of route 3 after the 3090 has written the block: D3D12 signals "landed" (in the mod: after its
	// queue waited for the 3090's transfer fence), Vulkan waits for it on the GPU, copies the block into the
	// D3D12 heap and signals "copied", and both D3D12 queues wait for that on the GPU.
	bool Route3Frame(Gpu& a_amd, Dx& a_b, Route3& a_r)
	{
		const UINT64 landed = ++a_r.value;
		const UINT64 copied = ++a_r.value;
		a_b.copy.queue->Signal(a_r.fence.Get(), landed);
		Begin(a_amd);
		const VkBufferCopy region{ 0, 0, kSize };
		vkCmdCopyBuffer(a_amd.cmd, a_r.blockBuf, a_r.landBuf, 1, &region);
		vkEndCommandBuffer(a_amd.cmd);
		VkTimelineSemaphoreSubmitInfo tsi{ VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO };
		tsi.waitSemaphoreValueCount = 1;
		tsi.pWaitSemaphoreValues = &landed;
		tsi.signalSemaphoreValueCount = 1;
		tsi.pSignalSemaphoreValues = &copied;
		const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
		VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
		si.pNext = &tsi;
		si.waitSemaphoreCount = 1;
		si.pWaitSemaphores = &a_r.semaphore;
		si.pWaitDstStageMask = &stage;
		si.commandBufferCount = 1;
		si.pCommandBuffers = &a_amd.cmd;
		si.signalSemaphoreCount = 1;
		si.pSignalSemaphores = &a_r.semaphore;
		VkResult r = vkQueueSubmit(a_amd.queue, 1, &si, a_amd.fence);
		if (r == VK_SUCCESS) {
			a_b.copy.queue->Wait(a_r.fence.Get(), copied);
			a_b.direct.queue->Wait(a_r.fence.Get(), copied);
			r = vkWaitForFences(a_amd.dev, 1, &a_amd.fence, VK_TRUE, kTimeoutNs);
			if (r == VK_SUCCESS) vkResetFences(a_amd.dev, 1, &a_amd.fence);
		}
		if (r != VK_SUCCESS) std::printf("    Vulkan copy: %s\n", VkName(r));
		return r == VK_SUCCESS;
	}

	bool TestRoute3(Dx& a_a, Dx& a_b, Gpu& a_amd, Route3& a_r)
	{
		std::printf("\n  testing route 3\n");
		ComPtr<ID3D12Heap> heapA;
		HRESULT hr = a_a.dev->OpenExistingHeapFromAddress(a_r.ptr, IID_PPV_ARGS(&heapA));
		std::printf("    3090 opens the block: 0x%08lx\n", static_cast<unsigned long>(hr));
		if (FAILED(hr)) return false;
		ComPtr<ID3D12Resource> peerOnA = a_a.Placed(heapA.Get());
		ComPtr<ID3D12Resource> vramA = a_a.Buffer(D3D12_HEAP_TYPE_DEFAULT, kSize, D3D12_RESOURCE_STATE_COMMON);
		bool allOk = true;
		for (uint32_t round = 0; round < 4; ++round) {
			const uint32_t seed = 0x53000000u + round;
			std::printf("    round %u: the 3090 writes a new pattern\n", round + 1);
			if (!a_a.Fill(vramA.Get(), seed) || !a_a.CopyOnce(a_a.copy, peerOnA.Get(), vramA.Get())) return false;
			if (!Route3Frame(a_amd, a_b, a_r)) return false;
			allOk &= a_b.Verify(a_b.copy, a_r.landOnB.Get(), seed, "9060 D3D12 copy queue reads the D3D12 heap");
			allOk &= a_b.Verify(a_b.direct, a_r.landOnB.Get(), seed, "9060 D3D12 direct queue reads the D3D12 heap");
		}
		TimedCopies(a_amd, "9060 Vulkan copy, block -> D3D12 heap", a_r.blockBuf, a_r.landBuf);
		return allOk;
	}

	// The 9060's D3D12 alone, without Vulkan (expected to fail on Windows 10).
	void D3D12Only(Dx& a_b)
	{
		std::printf("\n  route 0: the 9060's D3D12 alone\n");
		D3D12_FEATURE_DATA_D3D12_OPTIONS16 o16{};
		HRESULT hr = a_b.dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS16, &o16, sizeof o16);
		std::printf("    GPU upload heaps: query 0x%08lx, supported %d\n", static_cast<unsigned long>(hr),
			SUCCEEDED(hr) ? o16.GPUUploadHeapSupported : 0);
		D3D12_HEAP_PROPERTIES hp{};
		hp.Type = D3D12_HEAP_TYPE_CUSTOM;
		hp.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE;
		hp.MemoryPoolPreference = D3D12_MEMORY_POOL_L1;
		D3D12_RESOURCE_DESC rd{};
		rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		rd.Width = kSize;
		rd.Height = 1;
		rd.DepthOrArraySize = 1;
		rd.MipLevels = 1;
		rd.SampleDesc.Count = 1;
		rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		ComPtr<ID3D12Resource> r;
		hr = a_b.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr,
			IID_PPV_ARGS(&r));
		std::printf("    custom heap in VRAM with a CPU mapping (L1 + write-combine): 0x%08lx\n",
			static_cast<unsigned long>(hr));
	}

	// The whole path the mod would use: the 3090 writes the block, the 9060's D3D12 reads it.
	bool TestBlock(Dx& a_a, Dx& a_b, AmdBlock& a_block)
	{
		std::printf("\n  testing %s (CPU pointer %p)\n", a_block.how, a_block.ptr);
		ComPtr<ID3D12Heap> heapA;
		HRESULT hr = a_a.dev->OpenExistingHeapFromAddress(a_block.ptr, IID_PPV_ARGS(&heapA));
		std::printf("    3090 opens it: 0x%08lx\n", static_cast<unsigned long>(hr));
		if (FAILED(hr)) return false;
		ComPtr<ID3D12Resource> peerOnA = a_a.Placed(heapA.Get());
		ComPtr<ID3D12Resource> vramA = a_a.Buffer(D3D12_HEAP_TYPE_DEFAULT, kSize, D3D12_RESOURCE_STATE_COMMON);
		ComPtr<ID3D12Resource> vramB = a_b.Buffer(D3D12_HEAP_TYPE_DEFAULT, kSize, D3D12_RESOURCE_STATE_COMMON);
		if (!peerOnA || !vramA || !vramB) return false;

		// Four rounds with a new pattern each: from round 2 on, the 9060 has already read the block,
		// so stale caches on its side would show up as wrong data.
		bool allOk = true;
		for (uint32_t round = 0; round < 4; ++round) {
			const uint32_t seed = 0x51000000u + round;
			std::printf("    round %u: the 3090 writes a new pattern\n", round + 1);
			if (!a_a.Fill(vramA.Get(), seed) || !a_a.CopyOnce(a_a.copy, peerOnA.Get(), vramA.Get())) return false;
			allOk &= a_b.Verify(a_b.copy, a_block.view.Get(), seed, "9060 D3D12 copy queue reads it");
			allOk &= a_b.Verify(a_b.direct, a_block.view.Get(), seed, "9060 D3D12 direct queue reads it");
		}

		const double copyMs = a_b.Time(a_b.copy, vramB.Get(), a_block.view.Get(), kSize, 16);
		const double directMs = a_b.Time(a_b.direct, vramB.Get(), a_block.view.Get(), kSize, 16);
		std::printf("    9060 reads the block into its own VRAM: copy queue %.2f ms (%.1f GB/s), direct queue %.2f ms "
		            "(%.1f GB/s) per 64 MiB\n",
			copyMs, static_cast<double>(kSize) / (copyMs / 1000.0) / 1e9, directMs,
			static_cast<double>(kSize) / (directMs / 1000.0) / 1e9);

		ComPtr<ID3D12Device1> dev1;
		a_b.dev.As(&dev1);
		ID3D12Pageable* pageable = a_block.Pageable();
		const D3D12_RESIDENCY_PRIORITY priority = D3D12_RESIDENCY_PRIORITY_MAXIMUM;
		hr = dev1 ? dev1->SetResidencyPriority(1, &pageable, &priority) : E_NOINTERFACE;
		std::printf("    SetResidencyPriority(MAXIMUM) on the 9060's view: 0x%08lx\n", static_cast<unsigned long>(hr));

		// The canary the mod could check each frame: one uncached CPU read through the BAR mapping.
		LARGE_INTEGER f, t0, t1;
		QueryPerformanceFrequency(&f);
		const auto* words = static_cast<const volatile uint32_t*>(a_block.ptr);
		const size_t last = kSize / 4 - 1;
		uint32_t sum = 0;
		QueryPerformanceCounter(&t0);
		for (int i = 0; i < 1000; ++i) sum += words[last - static_cast<size_t>(i) * 1024];
		QueryPerformanceCounter(&t1);
		std::printf("    canary: one CPU read through the mapping takes %.2f us (last dword %s)\n",
			static_cast<double>(t1.QuadPart - t0.QuadPart) * 1e6 / static_cast<double>(f.QuadPart) / 1000.0,
			words[last] == Pattern(last, 0x51000003u) ? "matches" : "DIFFERS");
		(void)sum;
		return allOk;
	}

	// ---- --evict: what happens to the block when the 9060 runs out of VRAM? ---------------------

	struct VramInfo
	{
		double budgetGiB = 0;
		double localGiB = 0;
		double nonLocalGiB = 0;
	};

	VramInfo QueryVram(ID3D12Device* a_dev)
	{
		VramInfo info;
		ComPtr<IDXGIFactory4> factory;
		ComPtr<IDXGIAdapter3> adapter;
		if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) ||
			FAILED(factory->EnumAdapterByLuid(a_dev->GetAdapterLuid(), IID_PPV_ARGS(&adapter)))) {
			return info;
		}
		DXGI_QUERY_VIDEO_MEMORY_INFO local{};
		DXGI_QUERY_VIDEO_MEMORY_INFO nonLocal{};
		adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local);
		adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nonLocal);
		constexpr double kGiB = 1024.0 * 1024.0 * 1024.0;
		info.budgetGiB = static_cast<double>(local.Budget) / kGiB;
		info.localGiB = static_cast<double>(local.CurrentUsage) / kGiB;
		info.nonLocalGiB = static_cast<double>(nonLocal.CurrentUsage) / kGiB;
		return info;
	}

	// Writes the pattern through the CPU mapping, which always follows the block wherever VidMm keeps it.
	void CpuWrite(void* a_ptr, uint32_t a_seed)
	{
		auto* words = static_cast<uint32_t*>(a_ptr);
		for (size_t i = 0; i < kSize / 4; ++i) words[i] = Pattern(i, a_seed);
		_mm_sfence();
	}

	// The CPU writes a pattern into the block through its mapping; the 3090 READS the block through the
	// address it locked before the pressure (a read cannot damage anything if that address went stale).
	bool EvictTest(Dx& a_a, Dx& a_b, void* a_ptr, const std::function<void(bool)>& a_setPriority, bool a_maxPriority)
	{
		std::printf("\n  pressure test, residency priority %s\n", a_maxPriority ? "MAXIMUM" : "default");
		a_setPriority(a_maxPriority);

		ComPtr<ID3D12Heap> heapA;
		if (FAILED(a_a.dev->OpenExistingHeapFromAddress(a_ptr, IID_PPV_ARGS(&heapA)))) return false;
		ComPtr<ID3D12Resource> peerOnA = a_a.Placed(heapA.Get());
		ComPtr<ID3D12Resource> vramA = a_a.Buffer(D3D12_HEAP_TYPE_DEFAULT, kSize, D3D12_RESOURCE_STATE_COMMON);

		const uint32_t before = a_maxPriority ? 0xE1E10002u : 0xE1E10001u;
		const uint32_t after = a_maxPriority ? 0xE2E20002u : 0xE2E20001u;
		CpuWrite(a_ptr, before);
		if (!a_a.CopyOnce(a_a.copy, vramA.Get(), peerOnA.Get())) return false;
		a_a.Verify(a_a.copy, vramA.Get(), before, "before: the 3090 reads what the CPU wrote");

		VramInfo v = QueryVram(a_b.dev.Get());
		std::printf("    9060 before: budget %.2f GiB, this process %.2f GiB in VRAM, %.2f GiB in system RAM\n",
			v.budgetGiB, v.localGiB, v.nonLocalGiB);

		// Fill the 9060 well past its budget with 512 MiB buffers the GPU then touches, so they are the
		// most recently used allocations and the block is the least.
		std::vector<ComPtr<ID3D12Resource>> pressure;
		constexpr UINT64 kChunk = 512ull << 20;
		const double targetGiB = v.budgetGiB + 4.0;
		D3D12_HEAP_PROPERTIES hp{};
		hp.Type = D3D12_HEAP_TYPE_DEFAULT;
		D3D12_RESOURCE_DESC rd{};
		rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		rd.Width = kChunk;
		rd.Height = 1;
		rd.DepthOrArraySize = 1;
		rd.MipLevels = 1;
		rd.SampleDesc.Count = 1;
		rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		while (static_cast<double>(pressure.size()) * 0.5 < targetGiB && pressure.size() < 64) {
			ComPtr<ID3D12Resource> r;
			if (FAILED(a_b.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON,
					nullptr, IID_PPV_ARGS(&r)))) {
				break;
			}
			pressure.push_back(r);
		}
		ComPtr<ID3D12Resource> touchSrc = a_b.Buffer(D3D12_HEAP_TYPE_DEFAULT, 1 << 20, D3D12_RESOURCE_STATE_COMMON);
		a_b.copy.allocator->Reset();
		a_b.copy.list->Reset(a_b.copy.allocator.Get(), nullptr);
		for (auto& r : pressure) a_b.copy.list->CopyBufferRegion(r.Get(), 0, touchSrc.Get(), 0, 1 << 20);
		const bool touched = a_b.Run(a_b.copy);
		v = QueryVram(a_b.dev.Get());
		std::printf("    pressure: %zu x 512 MiB = %.1f GiB allocated%s; 9060 now: %.2f GiB in VRAM, %.2f GiB in system RAM\n",
			pressure.size(), static_cast<double>(pressure.size()) * 0.5, touched ? "" : " (touch FAILED)", v.localGiB,
			v.nonLocalGiB);

		// A new pattern through the CPU mapping (it lands wherever the block is now), then the 3090 reads its address.
		CpuWrite(a_ptr, after);
		if (!a_a.CopyOnce(a_a.copy, vramA.Get(), peerOnA.Get())) return false;
		const bool same = a_a.Verify(a_a.copy, vramA.Get(), after, "after: the 3090 reads the block at its old address");
		std::printf("    => %s\n", same ? "the block stayed where the 3090 expects it" :
		                                  "THE BLOCK MOVED: 3090 writes would have hit whatever is at the old address");
		pressure.clear();
		return same;
	}

	// ---- --pipeline: the mod's per-frame sequence, many times -----------------------------------
	//
	// 3090 copy queue: (gate) copy the frame into the 9060's block + a check value behind it, signal the
	// cross-adapter transfer fence. 9060 copy queue: wait for that fence, signal "landed"; Vulkan: wait
	// "landed", copy block -> D3D12 heap, signal "copied"; 9060 copy queue: wait "copied", read the heap.
	// Everything on the 9060 is queued BEFORE the 3090 is let go (the gate opens from another thread
	// 2 ms later), so the 9060 really waits on the GPU, as in the game. Measures whether vkQueueSubmit
	// blocks until its wait is signalled, whether the 9060 ever reads stale data, and how long after the
	// 3090's fence completes the check value is visible to the CPU.
	int RunPipeline(Gpu& a_amd, uint32_t a_amdType, uint32_t a_rounds)
	{
		constexpr UINT64 kLane = 33'177'600ull;  // the 4K finished frame
		constexpr UINT64 kCheckOffset = kLane;   // 256-aligned
		constexpr uint32_t kSeeds[2] = { 0x7E570000u, 0x7E57FFFFu };
		Dx a;
		Dx b;
		if (!a.Create(kVendorNvidia) || !b.Create(kVendorAmd)) return 1;

		ComPtr<ID3D12Fence> transferA;
		ComPtr<ID3D12Fence> transferB;
		ComPtr<ID3D12Fence> gate;
		{
			HANDLE handle = nullptr;
			if (FAILED(a.dev->CreateFence(0, D3D12_FENCE_FLAG_SHARED | D3D12_FENCE_FLAG_SHARED_CROSS_ADAPTER,
					IID_PPV_ARGS(&transferA))) ||
				FAILED(a.dev->CreateSharedHandle(transferA.Get(), nullptr, GENERIC_ALL, nullptr, &handle)) ||
				FAILED(b.dev->OpenSharedHandle(handle, IID_PPV_ARGS(&transferB))) ||
				FAILED(a.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)))) {
				std::printf("  fences failed\n");
				return 1;
			}
			CloseHandle(handle);
		}
		Route3 r3;
		if (!SetUpRoute3(a_amd, a_amdType, b, r3)) return 1;
		ComPtr<ID3D12Heap> heapA;
		if (FAILED(a.dev->OpenExistingHeapFromAddress(r3.ptr, IID_PPV_ARGS(&heapA)))) return 1;
		ComPtr<ID3D12Resource> peerOnA = a.Placed(heapA.Get());
		ComPtr<ID3D12Resource> src[2];
		for (int i = 0; i < 2; ++i) {
			src[i] = a.Buffer(D3D12_HEAP_TYPE_DEFAULT, kSize, D3D12_RESOURCE_STATE_COMMON);
			if (!src[i] || !a.Fill(src[i].Get(), kSeeds[i])) return 1;
		}
		ComPtr<ID3D12Resource> checkUpload = a.Buffer(D3D12_HEAP_TYPE_UPLOAD, 256, D3D12_RESOURCE_STATE_GENERIC_READ);
		UINT64* checkCpu = nullptr;
		checkUpload->Map(0, nullptr, reinterpret_cast<void**>(&checkCpu));

		VkCommandBuffer copyCmd = VK_NULL_HANDLE;
		{
			VkCommandBufferAllocateInfo cai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
			cai.commandPool = a_amd.pool;
			cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
			cai.commandBufferCount = 1;
			vkAllocateCommandBuffers(a_amd.dev, &cai, &copyCmd);
			VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
			bi.flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;
			vkBeginCommandBuffer(copyCmd, &bi);
			const VkBufferCopy region{ 0, 0, kLane };
			vkCmdCopyBuffer(copyCmd, r3.blockBuf, r3.landBuf, 1, &region);
			vkEndCommandBuffer(copyCmd);
		}

		const auto* checkGpu =
			reinterpret_cast<const volatile UINT64*>(static_cast<const uint8_t*>(r3.ptr) + kCheckOffset);
		LARGE_INTEGER f;
		QueryPerformanceFrequency(&f);
		auto us = [&](const LARGE_INTEGER& a_from, const LARGE_INTEGER& a_to) {
			return static_cast<double>(a_to.QuadPart - a_from.QuadPart) * 1e6 / static_cast<double>(f.QuadPart);
		};
		uint32_t wrong = 0, checkLate = 0, checkNever = 0;
		double submitMax = 0, submitSum = 0, lateMax = 0;
		std::printf("\n  %u frames of 33.2 MB through the whole chain\n", a_rounds);
		for (uint32_t round = 1; round <= a_rounds; ++round) {
			const int which = round & 1;
			*checkCpu = round;  // the previous round's copy finished (its fence was waited for below)
			a.copy.allocator->Reset();
			a.copy.list->Reset(a.copy.allocator.Get(), nullptr);
			a.copy.list->CopyBufferRegion(peerOnA.Get(), 0, src[which].Get(), 0, kLane);
			a.copy.list->CopyBufferRegion(peerOnA.Get(), kCheckOffset, checkUpload.Get(), 0, sizeof(UINT64));
			a.copy.list->Close();
			ID3D12CommandList* lists[] = { a.copy.list.Get() };
			a.copy.queue->Wait(gate.Get(), round);
			a.copy.queue->ExecuteCommandLists(1, lists);
			a.copy.queue->Signal(transferA.Get(), round);

			const UINT64 landed = ++r3.value;
			const UINT64 copied = ++r3.value;
			b.copy.queue->Wait(transferB.Get(), round);
			b.copy.queue->Signal(r3.fence.Get(), landed);

			std::thread opener([&gate, round] {
				Sleep(2);
				gate->Signal(round);
			});
			VkTimelineSemaphoreSubmitInfo tsi{ VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO };
			tsi.waitSemaphoreValueCount = 1;
			tsi.pWaitSemaphoreValues = &landed;
			tsi.signalSemaphoreValueCount = 1;
			tsi.pSignalSemaphoreValues = &copied;
			const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
			VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
			si.pNext = &tsi;
			si.waitSemaphoreCount = 1;
			si.pWaitSemaphores = &r3.semaphore;
			si.pWaitDstStageMask = &stage;
			si.commandBufferCount = 1;
			si.pCommandBuffers = &copyCmd;
			si.signalSemaphoreCount = 1;
			si.pSignalSemaphores = &r3.semaphore;
			LARGE_INTEGER t0, t1;
			QueryPerformanceCounter(&t0);
			const VkResult vr = vkQueueSubmit(a_amd.queue, 1, &si, a_amd.fence);
			QueryPerformanceCounter(&t1);
			const double submitUs = us(t0, t1);
			submitSum += submitUs;
			submitMax = (std::max)(submitMax, submitUs);
			b.copy.queue->Wait(r3.fence.Get(), copied);
			opener.join();
			if (vr != VK_SUCCESS) {
				std::printf("  round %u: vkQueueSubmit %s\n", round, VkName(vr));
				return 1;
			}

			// The moment the 3090's fence says the frame is written, is the check value already visible?
			LARGE_INTEGER s0, s1;
			QueryPerformanceCounter(&s0);
			while (transferA->GetCompletedValue() < round) {
				QueryPerformanceCounter(&s1);
				if (us(s0, s1) > 2e6) {
					std::printf("  round %u: the 3090's copy never finished\n", round);
					return 1;
				}
			}
			QueryPerformanceCounter(&s0);
			if (*checkGpu != round) {
				++checkLate;
				do {
					QueryPerformanceCounter(&s1);
				} while (*checkGpu != round && us(s0, s1) < 10000.0);
				if (*checkGpu != round) ++checkNever;
				lateMax = (std::max)(lateMax, us(s0, s1));
			}

			char what[64];
			std::snprintf(what, sizeof what, "round %u: 9060 D3D12 reads the frame", round);
			if (!b.Verify(b.copy, r3.landOnB.Get(), kSeeds[which], what, kLane, true)) ++wrong;
			if (vkWaitForFences(a_amd.dev, 1, &a_amd.fence, VK_TRUE, kTimeoutNs) != VK_SUCCESS) {
				std::printf("  round %u: Vulkan copy timed out\n", round);
				return 1;
			}
			vkResetFences(a_amd.dev, 1, &a_amd.fence);
		}
		// TimedCopies copies kSize; the block and the landing heap are both 64 MiB.
		TimedCopies(a_amd, "9060 Vulkan copy, block -> D3D12 heap", r3.blockBuf, r3.landBuf);
		std::printf("  frames with wrong data on the 9060: %u of %u\n", wrong, a_rounds);
		std::printf("  vkQueueSubmit while its wait was still 2 ms away: avg %.0f us, max %.0f us (%s)\n",
			submitSum / a_rounds, submitMax, submitMax > 1500 ? "BLOCKS until signalled" : "returns at once");
		std::printf("  check value not yet visible when the 3090's fence completed: %u of %u frames (max %.1f us late, "
		            "%u never within 10 ms)\n",
			checkLate, a_rounds, lateMax, checkNever);
		return wrong ? 1 : 0;
	}

	int RunInterop(Gpu& a_amd, uint32_t a_amdType, bool a_evict)
	{
		Dx a;
		Dx b;
		if (!a.Create(kVendorNvidia) || !b.Create(kVendorAmd)) {
			std::printf("  D3D12 devices failed\n");
			return 1;
		}
		D3D12Only(b);
		AmdBlock fromD3D12;
		AmdBlock fromVulkan;
		Route3 r3;
		const bool route1 = BlockFromD3D12(a_amd, b, fromD3D12);
		const bool route2 = BlockFromVulkan(a_amd, b, fromVulkan);
		const bool route3 = SetUpRoute3(a_amd, a_amdType, b, r3);
		std::printf("\n  routes: 1 (D3D12 heap + Vulkan map) %s, 2 (Vulkan export + D3D12 open) %s, "
		            "3 (Vulkan block + Vulkan copy into a D3D12 heap) %s\n",
			route1 ? "OK" : "failed", route2 ? "OK" : "failed", route3 ? "OK" : "failed");
		bool ok1 = route1 && TestBlock(a, b, fromD3D12);
		bool ok2 = route2 && TestBlock(a, b, fromVulkan);
		bool ok3 = route3 && TestRoute3(a, b, a_amd, r3);
		std::printf("\n  verified end to end: route 1 %s, route 2 %s, route 3 %s\n", ok1 ? "yes" : "no",
			ok2 ? "yes" : "no", ok3 ? "yes" : "no");

		if (a_evict && (ok2 || ok3)) {
			void* ptr = ok2 ? fromVulkan.ptr : r3.ptr;
			const VkDeviceMemory memory = ok2 ? fromVulkan.memory : r3.blockMem;
			ID3D12Pageable* pageable = ok2 ? fromVulkan.Pageable() : nullptr;
			ComPtr<ID3D12Device1> dev1;
			b.dev.As(&dev1);
			auto setPriority = [&](bool a_max) {
				if (vkSetDeviceMemoryPriorityEXT) vkSetDeviceMemoryPriorityEXT(a_amd.dev, memory, a_max ? 1.0f : 0.5f);
				if (pageable && dev1) {
					const D3D12_RESIDENCY_PRIORITY p = a_max ? D3D12_RESIDENCY_PRIORITY_MAXIMUM : D3D12_RESIDENCY_PRIORITY_NORMAL;
					dev1->SetResidencyPriority(1, &pageable, &p);
				}
			};
			// The 9060 copies the block once, so it is back in VRAM before the second test.
			auto pageIn = [&] {
				if (ok3) Route3Frame(a_amd, b, r3);
				else b.Verify(b.copy, fromVulkan.view.Get(), 0, "page-in read (pattern irrelevant)");
			};
			std::printf("\n[evict] block from route %d; priority via %s\n", ok2 ? 2 : 3,
				vkSetDeviceMemoryPriorityEXT ? "vkSetDeviceMemoryPriorityEXT" : "nothing (extension missing)");
			const bool keptDefault = EvictTest(a, b, ptr, setPriority, false);
			pageIn();
			const bool keptMax = EvictTest(a, b, ptr, setPriority, true);
			std::printf("\n  block kept its address under pressure: default priority %s, maximum priority %s\n",
				keptDefault ? "yes" : "NO", keptMax ? "yes" : "NO");
		}
		return (ok1 || ok2 || ok3) ? 0 : 1;
	}
}

// ---- --contention: does the copy engine's outgoing traffic slow the 3090's other PCIe traffic? -----------------
//
// In game, the 3090 renders while its copy engine sends the lanes out. The game reads system memory every frame
// (dynamic vertex/constant buffers, streaming), and on PCIe a read request may not pass the posted writes ahead of
// it on the same link. Writes into the 9060's VRAM drain slower than writes into system RAM (10.2 vs 13.2 GB/s
// through the X299 root complex), so they could hold those reads up longer. Measured here: system-RAM reads on the
// 3090's direct queue (bulk 256 KB copies and tiny 256 B ones) alone, while the copy queue writes system RAM, and
// while it writes the 9060's VRAM.
namespace
{
	int RunContention(void* a_barAmd)
	{
		Dx a;
		if (!a.Create(kVendorNvidia)) return 1;

		ComPtr<ID3D12Resource> vramSrc = a.Buffer(D3D12_HEAP_TYPE_DEFAULT, kSize, D3D12_RESOURCE_STATE_COMMON);
		ComPtr<ID3D12Resource> vramDst = a.Buffer(D3D12_HEAP_TYPE_DEFAULT, kSize, D3D12_RESOURCE_STATE_COMMON);
		ComPtr<ID3D12Resource> upload = a.Buffer(D3D12_HEAP_TYPE_UPLOAD, kSize, D3D12_RESOURCE_STATE_GENERIC_READ);
		// Write targets: a cross-adapter heap in system RAM (today's bridge) and the 9060's BAR block (direct copy).
		ComPtr<ID3D12Resource> sysTarget;
		ComPtr<ID3D12Resource> peerTarget;
		{
			D3D12_HEAP_DESC hd{};
			hd.SizeInBytes = kSize;
			hd.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
			hd.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
			hd.Flags = D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER;
			ComPtr<ID3D12Heap> heap;
			if (SUCCEEDED(a.dev->CreateHeap(&hd, IID_PPV_ARGS(&heap)))) sysTarget = a.Placed(heap.Get());
			ComPtr<ID3D12Heap> peerHeap;
			if (SUCCEEDED(a.dev->OpenExistingHeapFromAddress(a_barAmd, IID_PPV_ARGS(&peerHeap))))
				peerTarget = a.Placed(peerHeap.Get());
		}
		ComPtr<ID3D12Fence> backgroundFence;
		a.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&backgroundFence));
		UINT64 backgroundValue = 0;
		HANDLE backgroundEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		if (!vramSrc || !vramDst || !upload || !sysTarget || !peerTarget || !backgroundFence ||
			!a.Fill(vramSrc.Get(), 0xC0FFEEu)) {
			std::printf("  resources failed\n");
			return 1;
		}

		// The measurement on the direct queue: [0,1] 256 x 256 KB system-RAM reads, [2,3] 4000 x 256 B ones.
		ComPtr<ID3D12QueryHeap> queries;
		D3D12_QUERY_HEAP_DESC qhd{ D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 4, 0 };
		a.dev->CreateQueryHeap(&qhd, IID_PPV_ARGS(&queries));
		ComPtr<ID3D12Resource> stamps = a.Buffer(D3D12_HEAP_TYPE_READBACK, 4 * sizeof(UINT64), D3D12_RESOURCE_STATE_COPY_DEST);
		constexpr UINT kBulk = 256;
		constexpr UINT64 kBulkSize = kSize / kBulk;
		constexpr UINT kSmall = 4000;
		constexpr UINT64 kSmallSize = 256;

		struct Result
		{
			double bulkGBps = 0, smallUs = 0, backgroundGBps = 0;
		};
		auto measure = [&](ID3D12Resource* a_backgroundTarget, Result& a_out) -> bool {
			constexpr UINT kBackgroundCopies = 30;  // ~150-200 ms of writes, far longer than the measurement
			LARGE_INTEGER f{}, b0{}, b1{};
			QueryPerformanceFrequency(&f);
			if (a_backgroundTarget) {
				a.copy.allocator->Reset();
				a.copy.list->Reset(a.copy.allocator.Get(), nullptr);
				for (UINT i = 0; i < kBackgroundCopies; ++i)
					a.copy.list->CopyBufferRegion(a_backgroundTarget, 0, vramSrc.Get(), 0, kSize);
				a.copy.list->Close();
				ID3D12CommandList* lists[] = { a.copy.list.Get() };
				QueryPerformanceCounter(&b0);
				a.copy.queue->ExecuteCommandLists(1, lists);
				a.copy.queue->Signal(backgroundFence.Get(), ++backgroundValue);
				Sleep(5);  // let the writes get going before the reads start
			}
			a.direct.allocator->Reset();
			a.direct.list->Reset(a.direct.allocator.Get(), nullptr);
			a.direct.list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
			for (UINT i = 0; i < kBulk; ++i)
				a.direct.list->CopyBufferRegion(vramDst.Get(), i * kBulkSize, upload.Get(), i * kBulkSize, kBulkSize);
			a.direct.list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
			for (UINT i = 0; i < kSmall; ++i)
				a.direct.list->CopyBufferRegion(vramDst.Get(), (i % 4096) * kSmallSize, upload.Get(), (i % 4096) * kSmallSize,
					kSmallSize);
			a.direct.list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2);
			a.direct.list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 3);
			a.direct.list->ResolveQueryData(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 4, stamps.Get(), 0);
			if (!a.Run(a.direct)) return false;
			if (a_backgroundTarget) {
				const bool stillWriting = backgroundFence->GetCompletedValue() < backgroundValue;
				backgroundFence->SetEventOnCompletion(backgroundValue, backgroundEvent);
				WaitForSingleObject(backgroundEvent, kTimeoutMs);
				QueryPerformanceCounter(&b1);
				const double ms = static_cast<double>(b1.QuadPart - b0.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart);
				a_out.backgroundGBps = static_cast<double>(kSize) * kBackgroundCopies / (ms / 1000.0) / 1e9;
				if (!stillWriting) std::printf("  (the background writes ended before the reads did; numbers understate it)\n");
			}
			UINT64* ts = nullptr;
			const D3D12_RANGE readRange{ 0, 4 * sizeof(UINT64) };
			stamps->Map(0, &readRange, reinterpret_cast<void**>(&ts));
			const double toMs = 1000.0 / static_cast<double>(a.direct.frequency);
			const double bulkMs = static_cast<double>(ts[1] - ts[0]) * toMs;
			const double smallMs = static_cast<double>(ts[2] - ts[1]) * toMs;
			const D3D12_RANGE noWrite{ 0, 0 };
			stamps->Unmap(0, &noWrite);
			a_out.bulkGBps = static_cast<double>(kSize) / (bulkMs / 1000.0) / 1e9;
			a_out.smallUs = smallMs * 1000.0 / kSmall;
			return true;
		};

		std::printf("\n  3090 reading system RAM on its direct queue (what the game does every frame), while its copy queue:\n");
		std::printf("  %-38s %16s %22s %18s\n", "", "bulk 256 KB reads", "tiny 256 B reads", "copy queue writes");
		const struct
		{
			const char* name;
			ID3D12Resource* target;
		} modes[] = { { "is idle", nullptr }, { "writes system RAM (today's bridge)", sysTarget.Get() },
			{ "writes the 9060's VRAM (direct copy)", peerTarget.Get() }, { "is idle (again)", nullptr },
			{ "writes system RAM (again)", sysTarget.Get() }, { "writes the 9060's VRAM (again)", peerTarget.Get() } };
		for (const auto& mode : modes) {
			Result r;
			if (!measure(mode.target, r)) return 1;
			std::printf("  %-38s %11.2f GB/s %15.2f us each", mode.name, r.bulkGBps, r.smallUs);
			if (mode.target) std::printf(" %13.2f GB/s", r.backgroundGBps);
			std::printf("\n");
		}
		CloseHandle(backgroundEvent);
		return 0;
	}
}

#ifdef P2P_EXIT_HOOK
// ---- --exit-hook, --exit-gpu: the mod's release at process exit (CrossAdapterBridge::ReleaseDirectCopyForExit) --
//
// The first build of bDirectGPUCopy hung the PC on Alt+F4: at process exit the Vulkan blocks were freed while the
// render GPU still had them open, during the drivers' own unload. The fix hooks ntdll!RtlExitUserProcess and
// kernelbase!TerminateProcess with the same detour library and the same prologue check as the mod, and closes the
// render GPU's view first. --exit-hook checks the hooks fire (no GPU work at all); --exit-gpu exits with 40 P2P
// copies still running and lets the hook wait for them, close the NVIDIA heap, then free the AMD block.
#include "Detours.h"

namespace
{
	using ExitUserProcessFn = void(NTAPI*)(LONG);
	using TerminateProcessFn = BOOL(WINAPI*)(HANDLE, UINT);
	ExitUserProcessFn exitOriginal = nullptr;
	TerminateProcessFn terminateOriginal = nullptr;
	std::function<void(const char*)> exitRelease;  // what the hook does before the process ends

	void NTAPI ExitHook(LONG a_status)
	{
		std::printf("  [hook] RtlExitUserProcess(%ld) reached the hook\n", a_status);
		if (exitRelease) exitRelease("RtlExitUserProcess");
		exitOriginal(a_status);
	}

	BOOL WINAPI TerminateHook(HANDLE a_process, UINT a_code)
	{
		if (a_process == GetCurrentProcess() || GetProcessId(a_process) == GetCurrentProcessId()) {
			std::printf("  [hook] TerminateProcess(self, %u) reached the hook\n", a_code);
			if (exitRelease) exitRelease("TerminateProcess");
		}
		return terminateOriginal(a_process, a_code);
	}

	// Same check as the mod: the first 5 bytes must be whole prologue instructions that are not RIP-relative.
	size_t PrologueLength(const uint8_t* a_code)
	{
		const uint8_t b0 = a_code[0], b1 = a_code[1], b2 = a_code[2];
		if (b0 >= 0x50 && b0 <= 0x57) return 1;
		if ((b0 == 0x40 || b0 == 0x41) && b1 >= 0x50 && b1 <= 0x57) return 2;
		if (b0 == 0x48 && b1 == 0x83 && b2 == 0xEC) return 4;
		if (b0 == 0x48 && b1 == 0x81 && b2 == 0xEC) return 7;
		if ((b0 == 0x48 || b0 == 0x4C) && b1 == 0x89 && (b2 & 0xC7) == 0x44 && a_code[3] == 0x24) return 5;
		if ((b0 & 0xF0) == 0x40 && (b1 == 0x8B || b1 == 0x89) && (b2 & 0xC0) == 0xC0) return 3;
		if ((b0 == 0x8B || b0 == 0x89) && (b1 & 0xC0) == 0xC0) return 2;
		return 0;
	}

	bool InstallExitHooks()
	{
		const auto exitEntry =
			reinterpret_cast<uintptr_t>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlExitUserProcess"));
		const auto terminateEntry =
			reinterpret_cast<uintptr_t>(GetProcAddress(GetModuleHandleW(L"kernelbase.dll"), "TerminateProcess"));
		for (const uintptr_t entry : { exitEntry, terminateEntry }) {
			const auto* code = reinterpret_cast<const uint8_t*>(entry);
			size_t covered = 0;
			while (entry && covered < 5) {
				const size_t length = PrologueLength(code + covered);
				if (!length) break;
				covered += length;
			}
			if (!entry || covered < 5) {
				std::printf("  entry %p is not a known prologue; no hook\n", code);
				return false;
			}
		}
		exitOriginal = reinterpret_cast<ExitUserProcessFn>(
			Detours::X64::DetourFunction(exitEntry, reinterpret_cast<uintptr_t>(&ExitHook)));
		terminateOriginal = reinterpret_cast<TerminateProcessFn>(
			Detours::X64::DetourFunction(terminateEntry, reinterpret_cast<uintptr_t>(&TerminateHook)));
		const bool patched = *reinterpret_cast<const uint8_t*>(exitEntry) == 0xE9 &&
		                     *reinterpret_cast<const uint8_t*>(terminateEntry) == 0xE9;
		std::printf("  hooks: RtlExitUserProcess %s, TerminateProcess %s, entries patched: %s\n",
			exitOriginal ? "ok" : "FAILED", terminateOriginal ? "ok" : "FAILED", patched ? "yes" : "NO");
		return exitOriginal && terminateOriginal && patched;
	}

	// The realistic case: the NVIDIA GPU still writing into the AMD block when the process ends.
	int RunExitGpu(Gpu& a_amd, uint32_t a_amdType, bool a_terminate)
	{
		static Dx a;
		static Dx b;
		static Route3 r3;
		static ComPtr<ID3D12Heap> heapA;
		static ComPtr<ID3D12Resource> peerOnA;
		static ComPtr<ID3D12Resource> src;
		if (!a.Create(kVendorNvidia) || !b.Create(kVendorAmd) || !SetUpRoute3(a_amd, a_amdType, b, r3)) return 1;
		if (FAILED(a.dev->OpenExistingHeapFromAddress(r3.ptr, IID_PPV_ARGS(&heapA)))) return 1;
		peerOnA = a.Placed(heapA.Get());
		src = a.Buffer(D3D12_HEAP_TYPE_DEFAULT, kSize, D3D12_RESOURCE_STATE_COMMON);
		if (!peerOnA || !src || !a.Fill(src.Get(), 0xE817E817u)) return 1;
		if (!InstallExitHooks()) {
			std::printf("  hooks not installed: cleaning up by hand instead of exiting with the block open\n");
			peerOnA.Reset();
			heapA.Reset();
			return 1;
		}

		static Gpu* amd = &a_amd;
		exitRelease = [](const char* a_why) {
			LARGE_INTEGER f, t0, t1;
			QueryPerformanceFrequency(&f);
			QueryPerformanceCounter(&t0);
			// 1. wait for the NVIDIA copies (2 s at most), 2. close the NVIDIA view, 3. free the AMD block.
			bool idle = a.fence->GetCompletedValue() >= a.value;
			if (!idle && SUCCEEDED(a.fence->SetEventOnCompletion(a.value, a.event)))
				idle = WaitForSingleObject(a.event, 2000) == WAIT_OBJECT_0;
			peerOnA.Reset();
			heapA.Reset();
			if (idle) {
				vkDeviceWaitIdle(amd->dev);
				vkDestroyBuffer(amd->dev, r3.blockBuf, nullptr);
				vkUnmapMemory(amd->dev, r3.blockMem);
				vkFreeMemory(amd->dev, r3.blockMem, nullptr);
			}
			QueryPerformanceCounter(&t1);
			std::printf("  [hook] %s: NVIDIA copies %s, NVIDIA view closed, AMD block %s (%.1f ms)\n", a_why,
				idle ? "finished" : "STILL RUNNING after 2 s", idle ? "freed" : "left to Windows",
				static_cast<double>(t1.QuadPart - t0.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart));
		};

		// ~260 ms of P2P writes, not waited for.
		a.copy.allocator->Reset();
		a.copy.list->Reset(a.copy.allocator.Get(), nullptr);
		for (int i = 0; i < 40; ++i) a.copy.list->CopyBufferRegion(peerOnA.Get(), 0, src.Get(), 0, kSize);
		a.copy.list->Close();
		ID3D12CommandList* lists[] = { a.copy.list.Get() };
		a.copy.queue->ExecuteCommandLists(1, lists);
		a.copy.queue->Signal(a.fence.Get(), ++a.value);
		std::printf("  40 x 64 MiB P2P copies queued; NVIDIA fence at %llu of %llu; ending the process now (%s)\n",
			static_cast<unsigned long long>(a.fence->GetCompletedValue()), static_cast<unsigned long long>(a.value),
			a_terminate ? "TerminateProcess" : "ExitProcess");
		if (a_terminate) TerminateProcess(GetCurrentProcess(), 0);
		ExitProcess(0);
	}
}
#endif

int wmain(int argc, wchar_t** argv)
{
#ifdef P2P_EXIT_HOOK
	for (int i = 1; i < argc; ++i) {
		const bool viaTerminate = std::wcscmp(argv[i], L"--exit-hook-terminate") == 0;
		if (viaTerminate || std::wcscmp(argv[i], L"--exit-hook") == 0) {
			std::setvbuf(stdout, nullptr, _IONBF, 0);
			std::printf("p2p-probe: exit hooks only, no GPU work (%s)\n",
				viaTerminate ? "TerminateProcess" : "ExitProcess");
			if (!InstallExitHooks()) return 1;
			exitRelease = [](const char* a_why) { std::printf("  [hook] release would run here (%s)\n", a_why); };
			if (viaTerminate) TerminateProcess(GetCurrentProcess(), 3);
			ExitProcess(2);
		}
	}
#endif
	bool dma = false;
	bool bench = false;
	bool interop = false;
	bool evict = false;
	bool pipeline = false;
	bool computeQueue = false;
	bool contention = false;
	for (int i = 1; i < argc; ++i) {
		if (std::wcscmp(argv[i], L"--contention") == 0) contention = true;
		if (std::wcscmp(argv[i], L"--pipeline") == 0) pipeline = true;
		if (std::wcscmp(argv[i], L"--compute") == 0) computeQueue = true;
		if (std::wcscmp(argv[i], L"--dma") == 0) dma = true;
		if (std::wcscmp(argv[i], L"--bench") == 0) bench = true;
		if (std::wcscmp(argv[i], L"--interop") == 0) interop = true;
		if (std::wcscmp(argv[i], L"--evict") == 0) interop = evict = true;
	}
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	std::printf("p2p-probe: NVIDIA writes into AMD VRAM through the AMD BAR mapping (%s)\n\n",
		dma ? "--dma: copies ENABLED" : "import calls only, no GPU writes");

	HMODULE loader = LoadLibraryW(L"vulkan-1.dll");
	if (!loader) {
		std::printf("vulkan-1.dll not found\n");
		return 1;
	}
	vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(loader, "vkGetInstanceProcAddr"));
	vkCreateInstance = reinterpret_cast<PFN_vkCreateInstance>(vkGetInstanceProcAddr(nullptr, "vkCreateInstance"));

	VkApplicationInfo app{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
	app.pApplicationName = "p2p-probe";
	app.apiVersion = VK_API_VERSION_1_2;
	VkInstanceCreateInfo ici{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
	ici.pApplicationInfo = &app;
	VkInstance instance = VK_NULL_HANDLE;
	VkResult r = vkCreateInstance(&ici, nullptr, &instance);
	if (r != VK_SUCCESS) {
		std::printf("vkCreateInstance %s\n", VkName(r));
		return 1;
	}
#define VK_LOAD(name) name = reinterpret_cast<PFN_##name>(vkGetInstanceProcAddr(instance, #name));
	VK_INSTANCE_FNS(VK_LOAD)
#undef VK_LOAD

	Gpu amd;
	Gpu nv;
	amd.label = "AMD";
	nv.label = "NVIDIA";
	{
		uint32_t count = 0;
		vkEnumeratePhysicalDevices(instance, &count, nullptr);
		std::vector<VkPhysicalDevice> phys(count);
		vkEnumeratePhysicalDevices(instance, &count, phys.data());
		for (VkPhysicalDevice p : phys) {
			VkPhysicalDeviceProperties props{};
			vkGetPhysicalDeviceProperties(p, &props);
			Gpu* target = props.vendorID == kVendorAmd ? &amd : props.vendorID == kVendorNvidia ? &nv : nullptr;
			if (!target || target->phys) continue;
			target->phys = p;
			target->props = props;
			vkGetPhysicalDeviceMemoryProperties(p, &target->mem);
		}
	}
	if (!amd.phys || !nv.phys) {
		std::printf("need one AMD and one NVIDIA GPU\n");
		return 1;
	}
	std::printf("Vulkan: %s / %s\n", nv.props.deviceName, amd.props.deviceName);

	if (!HasExtension(nv.phys, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME)) {
		std::printf("NVIDIA has no VK_EXT_external_memory_host\n");
		return 1;
	}
	amd.budget = HasExtension(amd.phys, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
	if (!CreateDevice(nv, { VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME })) return 1;
	{
		std::vector<const char*> amdExts{ VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
			VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME };
		if (amd.budget) amdExts.push_back(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
		const bool priority = HasExtension(amd.phys, VK_EXT_MEMORY_PRIORITY_EXTENSION_NAME) &&
		                      HasExtension(amd.phys, VK_EXT_PAGEABLE_DEVICE_LOCAL_MEMORY_EXTENSION_NAME);
		if (priority) {
			amdExts.push_back(VK_EXT_MEMORY_PRIORITY_EXTENSION_NAME);
			amdExts.push_back(VK_EXT_PAGEABLE_DEVICE_LOCAL_MEMORY_EXTENSION_NAME);
		}
		std::printf("AMD Vulkan: VK_EXT_pageable_device_local_memory %s\n", priority ? "available" : "NOT available");
		if (!CreateDevice(amd, amdExts, computeQueue)) return 1;
	}
	vkImportSemaphoreWin32HandleKHR = reinterpret_cast<PFN_vkImportSemaphoreWin32HandleKHR>(
		vkGetDeviceProcAddr(amd.dev, "vkImportSemaphoreWin32HandleKHR"));
	vkSetDeviceMemoryPriorityEXT =
		reinterpret_cast<PFN_vkSetDeviceMemoryPriorityEXT>(vkGetDeviceProcAddr(amd.dev, "vkSetDeviceMemoryPriorityEXT"));
	vkGetMemoryHostPointerPropertiesEXT = reinterpret_cast<PFN_vkGetMemoryHostPointerPropertiesEXT>(
		vkGetDeviceProcAddr(nv.dev, "vkGetMemoryHostPointerPropertiesEXT"));
	vkGetMemoryWin32HandleKHR =
		reinterpret_cast<PFN_vkGetMemoryWin32HandleKHR>(vkGetDeviceProcAddr(amd.dev, "vkGetMemoryWin32HandleKHR"));
	vkGetMemoryWin32HandlePropertiesKHR = reinterpret_cast<PFN_vkGetMemoryWin32HandlePropertiesKHR>(
		vkGetDeviceProcAddr(amd.dev, "vkGetMemoryWin32HandlePropertiesKHR"));

	// ---- what the NVIDIA driver says it can import ----
	VkPhysicalDeviceExternalMemoryHostPropertiesEXT hostProps{
		VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT
	};
	VkPhysicalDeviceProperties2 props2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
	props2.pNext = &hostProps;
	vkGetPhysicalDeviceProperties2(nv.phys, &props2);
	std::printf("\n[1] NVIDIA import capabilities\n  minImportedHostPointerAlignment %llu\n",
		static_cast<unsigned long long>(hostProps.minImportedHostPointerAlignment));
	const VkExternalMemoryHandleTypeFlagBits handleTypes[] = { VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
		VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_MAPPED_FOREIGN_MEMORY_BIT_EXT };
	for (VkExternalMemoryHandleTypeFlagBits ht : handleTypes) {
		VkPhysicalDeviceExternalBufferInfo info{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO };
		info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
		info.handleType = ht;
		VkExternalBufferProperties ep{ VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES };
		vkGetPhysicalDeviceExternalBufferProperties(nv.phys, &info, &ep);
		const auto features = ep.externalMemoryProperties.externalMemoryFeatures;
		std::printf("  %-27s buffer import %s (features 0x%x)\n", HandleTypeName(ht),
			(features & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) ? "supported" : "NOT supported",
			static_cast<unsigned>(features));
	}

	// ---- the AMD VRAM allocation, mapped through the BAR ----
	std::printf("\n[2] AMD VRAM mapped for the CPU\n");
	const uint32_t amdType = FindType(amd, ~0u,
		VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
		VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD);
	if (amdType == UINT32_MAX) {
		std::printf("  no DEVICE_LOCAL | HOST_VISIBLE memory type on the AMD GPU (Resizable BAR off?)\n");
		return 1;
	}
	const uint32_t amdHeap = amd.mem.memoryTypes[amdType].heapIndex;
	const double usageBefore = HeapUsageMiB(amd, amdHeap);
	VkDeviceMemory amdMem = VK_NULL_HANDLE;
	{
		VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
		ai.allocationSize = kSize;
		ai.memoryTypeIndex = amdType;
		r = vkAllocateMemory(amd.dev, &ai, nullptr, &amdMem);
		if (r != VK_SUCCESS) {
			std::printf("  vkAllocateMemory %s\n", VkName(r));
			return 1;
		}
	}
	void* bar = nullptr;
	r = vkMapMemory(amd.dev, amdMem, 0, VK_WHOLE_SIZE, 0, &bar);
	if (r != VK_SUCCESS) {
		std::printf("  vkMapMemory %s\n", VkName(r));
		return 1;
	}
	const double usageAfter = HeapUsageMiB(amd, amdHeap);
	std::printf("  memory type %u (heap %u, %.2f GiB), mapped at %p (alignment %llu ok: %s)\n", amdType, amdHeap,
		static_cast<double>(amd.mem.memoryHeaps[amdHeap].size) / (1u << 30), bar,
		static_cast<unsigned long long>(hostProps.minImportedHostPointerAlignment),
		(reinterpret_cast<uintptr_t>(bar) % hostProps.minImportedHostPointerAlignment) == 0 ? "yes" : "NO");
	if (usageBefore >= 0) {
		std::printf("  AMD VRAM heap usage %.1f -> %.1f MiB (+%.1f): the allocation is in VRAM\n", usageBefore,
			usageAfter, usageAfter - usageBefore);
	}
	{
		MEMORY_BASIC_INFORMATION mbi{};
		VirtualQuery(bar, &mbi, sizeof mbi);
		const char* type = mbi.Type == MEM_PRIVATE ? "MEM_PRIVATE" :
		                   mbi.Type == MEM_MAPPED  ? "MEM_MAPPED" :
		                   mbi.Type == MEM_IMAGE   ? "MEM_IMAGE" :
		                                             "?";
		std::printf("  VirtualQuery: allocation base %p, region %.1f MiB, state 0x%lx, type %s, protect 0x%lx\n",
			mbi.AllocationBase, static_cast<double>(mbi.RegionSize) / (1024.0 * 1024.0), mbi.State, type, mbi.Protect);
	}

	// A plain VirtualAlloc block: the control. Both import paths must accept this one.
	void* plain = VirtualAlloc(nullptr, kSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

	// ---- Vulkan import on the NVIDIA GPU ----
	std::printf("\n[3] Vulkan: NVIDIA imports the pointer\n");
	VkExternalMemoryHandleTypeFlagBits vkHandleType{};
	uint32_t vkImportType = UINT32_MAX;
	auto tryImport = [&](const char* a_what, void* a_ptr, VkExternalMemoryHandleTypeFlagBits a_ht, uint32_t* a_typeOut,
						 VkDeviceMemory* a_keep) -> bool {
		VkMemoryHostPointerPropertiesEXT hp{ VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT };
		VkResult res = vkGetMemoryHostPointerPropertiesEXT(nv.dev, a_ht, a_ptr, &hp);
		std::printf("  %-14s %-27s properties: %s, memoryTypeBits 0x%x", a_what, HandleTypeName(a_ht), VkName(res),
			hp.memoryTypeBits);
		if (res != VK_SUCCESS || !hp.memoryTypeBits) {
			std::printf("\n");
			return false;
		}
		uint32_t type = 0;
		while (!(hp.memoryTypeBits & (1u << type))) ++type;
		VkImportMemoryHostPointerInfoEXT imp{ VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT };
		imp.handleType = a_ht;
		imp.pHostPointer = a_ptr;
		VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
		ai.pNext = &imp;
		ai.allocationSize = kSize;
		ai.memoryTypeIndex = type;
		VkDeviceMemory mem = VK_NULL_HANDLE;
		res = vkAllocateMemory(nv.dev, &ai, nullptr, &mem);
		std::printf(" | import as type %u (flags 0x%x): %s\n", type, nv.mem.memoryTypes[type].propertyFlags,
			VkName(res));
		if (res != VK_SUCCESS) return false;
		if (a_typeOut) *a_typeOut = type;
		if (a_keep) {
			*a_keep = mem;
		} else {
			vkFreeMemory(nv.dev, mem, nullptr);
		}
		return true;
	};
	for (VkExternalMemoryHandleTypeFlagBits ht : handleTypes) {
		tryImport("VirtualAlloc", plain, ht, nullptr, nullptr);
	}
	for (VkExternalMemoryHandleTypeFlagBits ht : handleTypes) {
		uint32_t type = UINT32_MAX;
		if (tryImport("AMD BAR", bar, ht, &type, nullptr) && vkImportType == UINT32_MAX) {
			vkHandleType = ht;
			vkImportType = type;
		}
	}

	// ---- D3D12 OpenExistingHeapFromAddress on the NVIDIA GPU ----
	std::printf("\n[4] D3D12: NVIDIA opens the pointer as a heap (OpenExistingHeapFromAddress)\n");
	D3D12Gpu d3d;
	bool d3dOpened = false;
	if (!d3d.Create(kVendorNvidia)) {
		std::printf("  no D3D12 device on the NVIDIA GPU\n");
	} else {
		ComPtr<ID3D12Heap> heap;
		HRESULT hr = d3d.dev->OpenExistingHeapFromAddress(plain, IID_PPV_ARGS(&heap));
		std::printf("  VirtualAlloc   0x%08lx\n", static_cast<unsigned long>(hr));
		if (heap) PrintHeap(heap.Get());
		heap.Reset();
		hr = d3d.dev->OpenExistingHeapFromAddress(bar, IID_PPV_ARGS(&heap));
		std::printf("  AMD BAR        0x%08lx\n", static_cast<unsigned long>(hr));
		if (heap) PrintHeap(heap.Get());
		d3dOpened = SUCCEEDED(hr);
	}

	auto printUsage = [&](const char* a_when) {
		const double now = HeapUsageMiB(amd, amdHeap);
		if (now >= 0) std::printf("  AMD VRAM heap usage %s: %.1f MiB (%s)\n", a_when, now,
			now >= usageAfter - 1.0 ? "allocation still in VRAM" : "DROPPED - evicted?");
	};
	printUsage("after the imports");

#ifdef P2P_EXIT_HOOK
	for (int i = 1; i < argc; ++i) {
		const bool viaTerminate = std::wcscmp(argv[i], L"--exit-gpu-terminate") == 0;
		if (viaTerminate || std::wcscmp(argv[i], L"--exit-gpu") == 0) {
			std::printf("\n[exit-gpu] the process ends while the 3090 still writes into the 9060's block\n");
			return RunExitGpu(amd, amdType, viaTerminate);
		}
	}
#endif

	if (pipeline) {
		std::printf("\n[pipeline] the mod's per-frame sequence, with the 9060's work queued before the 3090 writes\n");
		return RunPipeline(amd, amdType, 300);
	}

	if (contention) {
		std::printf("\n[contention] the copy engine's writes vs the 3090's own system-RAM reads\n");
		return RunContention(bar);
	}

	if (interop) {
		std::printf("\n[interop] can the 9060's D3D12 device use a block of its VRAM that the 3090 can write?\n");
		return RunInterop(amd, amdType, evict);
	}

	if (bench) {
		std::printf("\n[bench] D3D12 buffer copies, %u per size after a warm-up; the 3090's VRAM is mapped through its BAR "
		            "by Vulkan\n",
			kBenchReps);
		const uint32_t nvType = FindType(nv, ~0u,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			0);
		void* barNv = nullptr;
		VkDeviceMemory nvMem = VK_NULL_HANDLE;
		if (nvType != UINT32_MAX) {
			VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
			ai.allocationSize = kSize;
			ai.memoryTypeIndex = nvType;
			if (vkAllocateMemory(nv.dev, &ai, nullptr, &nvMem) == VK_SUCCESS) {
				vkMapMemory(nv.dev, nvMem, 0, VK_WHOLE_SIZE, 0, &barNv);
			}
		}
		if (!barNv) {
			std::printf("  no CPU-visible VRAM on the 3090\n");
			return 1;
		}
		const int result = RunBench(bar, barNv);
		printUsage("after the bench");
		return result;
	}

	if (!dma) {
		std::printf("\nNo copies made. ");
		if (vkImportType != UINT32_MAX || d3dOpened) {
			std::printf("An import of the AMD BAR pointer succeeded; rerun with --dma to make the 3090 write into it.\n");
		} else {
			std::printf("Neither API on the 3090 accepts the 9060's BAR mapping.\n");
		}
		return 0;
	}

	// ---- --dma: the copies ----
	Buffer amdView;
	Buffer amdReadback;
	if (!MakeBuffer(amd, "AMD VRAM view", VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, 0, 0,
			amdView, amdMem, amdType) ||
		!MakeBuffer(amd, "AMD readback",
			VK_BUFFER_USAGE_TRANSFER_DST_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD, amdReadback)) {
		return 1;
	}

	Buffer staging;
	Buffer src;
	Buffer sysDst;
	if (!MakeBuffer(nv, "staging", VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
			staging) ||
		!MakeBuffer(nv, "3090 VRAM source", VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, src) ||
		!MakeBuffer(nv, "system memory", VK_BUFFER_USAGE_TRANSFER_DST_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
			sysDst)) {
		return 1;
	}
	{
		auto* words = static_cast<uint32_t*>(staging.mapped);
		for (size_t i = 0; i < kSize / 4; ++i) words[i] = Pattern(i, kSeedVulkan);
		Begin(nv);
		const VkBufferCopy region{ 0, 0, kSize };
		vkCmdCopyBuffer(nv.cmd, staging.buffer, src.buffer, 1, &region);
		if (Submit(nv) != VK_SUCCESS) return 1;
	}

	std::printf("\n[5] Baseline: the 3090 writes system memory (what the bridge does today)\n");
	if (!TimedCopies(nv, "3090 VRAM -> system memory", src.buffer, sysDst.buffer)) return 1;

	if (vkImportType != UINT32_MAX) {
		std::printf("\n[6] Vulkan: the 3090 writes the 9060's VRAM through its BAR\n");
		VkDeviceMemory imported = VK_NULL_HANDLE;
		if (!tryImport("AMD BAR", bar, vkHandleType, nullptr, &imported)) return 1;
		VkExternalMemoryBufferCreateInfo emb{ VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO };
		emb.handleTypes = static_cast<VkExternalMemoryHandleTypeFlags>(vkHandleType);
		Buffer p2pDst;
		if (!MakeBuffer(nv, "imported BAR", VK_BUFFER_USAGE_TRANSFER_DST_BIT, 0, 0, p2pDst, imported, vkImportType, &emb)) {
			return 1;
		}
		if (!ClearOnAmd(amd, amdView.buffer)) return 1;
		if (!TimedCopies(nv, "3090 VRAM -> 9060 VRAM (P2P)", src.buffer, p2pDst.buffer)) return 1;
		printUsage("after the copies");
		VerifyOnAmd(amd, amdView.buffer, amdReadback, kSeedVulkan);
		p2pDst.Destroy(nv);
		vkFreeMemory(nv.dev, imported, nullptr);
	}

	if (d3dOpened) {
		std::printf("\n[7] D3D12: the 3090 writes the 9060's VRAM through its BAR\n");
		ComPtr<ID3D12Heap> heap;
		if (FAILED(d3d.dev->OpenExistingHeapFromAddress(bar, IID_PPV_ARGS(&heap)))) return 1;
		if (!ClearOnAmd(amd, amdView.buffer)) return 1;
		if (D3D12Dma(d3d, heap.Get())) {
			printUsage("after the copies");
			VerifyOnAmd(amd, amdView.buffer, amdReadback, kSeedD3D12);
		}
	}

	std::printf("\ndone\n");
	sysDst.Destroy(nv);
	src.Destroy(nv);
	staging.Destroy(nv);
	amdReadback.Destroy(amd);
	amdView.Destroy(amd);
	vkUnmapMemory(amd.dev, amdMem);
	vkFreeMemory(amd.dev, amdMem, nullptr);
	DestroyDevice(nv);
	DestroyDevice(amd);
	vkDestroyInstance(instance, nullptr);
	VirtualFree(plain, 0, MEM_RELEASE);
	return 0;
}
