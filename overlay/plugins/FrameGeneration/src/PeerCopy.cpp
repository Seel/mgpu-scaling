#include "PeerCopy.h"

#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan_core.h>
#include <vulkan/vulkan_win32.h>

#include <array>
#include <cstring>
#include <string>
#include <tuple>
#include <vector>

#include "FrameSlots.h"

namespace
{
	constexpr UINT kMaxPeerLanes = 8;  // CrossAdapterBridge::kMaxLanes
	constexpr UINT64 kHeapAlignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
	// Without Resizable BAR (Smart Access Memory) only 256 MB of VRAM is CPU-visible, in a heap of its own.
	constexpr VkDeviceSize kMinBarHeap = 2ull << 30;

	const char* VkName(VkResult a_result)
	{
		switch (a_result) {
		case VK_SUCCESS:
			return "VK_SUCCESS";
		case VK_NOT_READY:
			return "VK_NOT_READY";
		case VK_TIMEOUT:
			return "VK_TIMEOUT";
		case VK_ERROR_OUT_OF_HOST_MEMORY:
			return "VK_ERROR_OUT_OF_HOST_MEMORY";
		case VK_ERROR_OUT_OF_DEVICE_MEMORY:
			return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
		case VK_ERROR_INITIALIZATION_FAILED:
			return "VK_ERROR_INITIALIZATION_FAILED";
		case VK_ERROR_DEVICE_LOST:
			return "VK_ERROR_DEVICE_LOST";
		case VK_ERROR_MEMORY_MAP_FAILED:
			return "VK_ERROR_MEMORY_MAP_FAILED";
		case VK_ERROR_EXTENSION_NOT_PRESENT:
			return "VK_ERROR_EXTENSION_NOT_PRESENT";
		case VK_ERROR_FEATURE_NOT_PRESENT:
			return "VK_ERROR_FEATURE_NOT_PRESENT";
		case VK_ERROR_INCOMPATIBLE_DRIVER:
			return "VK_ERROR_INCOMPATIBLE_DRIVER";
		case VK_ERROR_INVALID_EXTERNAL_HANDLE:
			return "VK_ERROR_INVALID_EXTERNAL_HANDLE";
		default:
			return "VkResult (other)";
		}
	}

	bool Failed(const char* a_what, VkResult a_result)
	{
		REX::ERROR("[FG-GPU1] direct GPU copy: {} ({}); frames cross through system memory instead", a_what,
			VkName(a_result));
		return false;
	}

	// True during the DLL detach phase of a process that is ending (ntdll!RtlDllShutdownInProgress).
	bool ProcessIsEnding() noexcept
	{
		using ShutdownInProgressFn = BOOLEAN(NTAPI*)();
		static const auto shutdownInProgress = reinterpret_cast<ShutdownInProgressFn>(
			GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlDllShutdownInProgress"));
		return shutdownInProgress && shutdownInProgress();
	}

	bool Failed(const char* a_what, HRESULT a_hr)
	{
		REX::ERROR("[FG-GPU1] direct GPU copy: {} (hr={:#010x}); frames cross through system memory instead", a_what,
			static_cast<std::uint32_t>(a_hr));
		return false;
	}
}

#define PEER_VK_INSTANCE_FUNCTIONS(X)        \
	X(vkDestroyInstance)                     \
	X(vkEnumeratePhysicalDevices)            \
	X(vkGetPhysicalDeviceProperties2)        \
	X(vkGetPhysicalDeviceMemoryProperties)   \
	X(vkGetPhysicalDeviceQueueFamilyProperties) \
	X(vkEnumerateDeviceExtensionProperties)  \
	X(vkCreateDevice)                        \
	X(vkGetDeviceProcAddr)

#define PEER_VK_DEVICE_FUNCTIONS(X)        \
	X(vkGetDeviceQueue)                    \
	X(vkDestroyDevice)                     \
	X(vkDeviceWaitIdle)                    \
	X(vkAllocateMemory)                    \
	X(vkFreeMemory)                        \
	X(vkMapMemory)                         \
	X(vkUnmapMemory)                       \
	X(vkCreateBuffer)                      \
	X(vkDestroyBuffer)                     \
	X(vkBindBufferMemory)                  \
	X(vkCreateCommandPool)                 \
	X(vkDestroyCommandPool)                \
	X(vkAllocateCommandBuffers)            \
	X(vkBeginCommandBuffer)                \
	X(vkEndCommandBuffer)                  \
	X(vkCmdCopyBuffer)                     \
	X(vkCmdWriteTimestamp)                 \
	X(vkCmdResetQueryPool)                 \
	X(vkCreateQueryPool)                   \
	X(vkDestroyQueryPool)                  \
	X(vkGetQueryPoolResults)               \
	X(vkQueueSubmit)                       \
	X(vkCreateSemaphore)                   \
	X(vkDestroySemaphore)                  \
	X(vkGetMemoryWin32HandlePropertiesKHR) \
	X(vkImportSemaphoreWin32HandleKHR)

struct PeerCopy::State
{
#define PEER_DECLARE(name) PFN_##name name = nullptr;
	PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;
	PFN_vkCreateInstance vkCreateInstance = nullptr;
	PFN_vkEnumerateInstanceVersion vkEnumerateInstanceVersion = nullptr;
	PEER_VK_INSTANCE_FUNCTIONS(PEER_DECLARE)
	PEER_VK_DEVICE_FUNCTIONS(PEER_DECLARE)
#undef PEER_DECLARE

	struct Lane
	{
		std::string name;
		VkDeviceMemory block = VK_NULL_HANDLE;    // CPU-mapped VRAM the render GPU writes into
		VkBuffer blockBuffer = VK_NULL_HANDLE;
		VkDeviceMemory landing = VK_NULL_HANDLE;  // the D3D12 landing heap, imported
		VkBuffer landingBuffer = VK_NULL_HANDLE;
		std::array<VkCommandBuffer, kMaxFrameSlots> copies{};
		std::array<bool, kMaxFrameSlots> stamped{};  // timestamps written since they were last read
	};

	HMODULE loader = nullptr;
	VkInstance instance = VK_NULL_HANDLE;
	VkPhysicalDevice physical = VK_NULL_HANDLE;
	VkPhysicalDeviceMemoryProperties memory{};
	VkDevice device = VK_NULL_HANDLE;
	VkQueue queue = VK_NULL_HANDLE;
	uint32_t family = 0;
	bool computeQueue = false;
	bool memoryPriority = false;
	double nsPerTick = 0.0;  // 0: no timestamps
	uint64_t timestampMask = ~0ull;
	VkCommandPool pool = VK_NULL_HANDLE;
	VkQueryPool queries = VK_NULL_HANDLE;  // two per lane and slot
	winrt::com_ptr<ID3D12Device> deviceB;
	winrt::com_ptr<ID3D12Fence> fence;       // on the display GPU's D3D12 device...
	VkSemaphore semaphore = VK_NULL_HANDLE;  // ...and the same fence in Vulkan
	HANDLE event = nullptr;
	UINT64 lastCopied = 0;  // the highest value Vulkan was asked to signal
	UINT slots = 2;
	bool submitFailed = false;
	std::vector<Lane> lanes;

	~State() { Destroy(); }

	// Every function the teardown calls. A driver that failed half-way through loading them leaks instead.
	[[nodiscard]] bool CanDestroy() const noexcept
	{
		return device && vkDeviceWaitIdle && vkDestroyDevice && vkDestroyBuffer && vkUnmapMemory && vkFreeMemory &&
		       vkDestroySemaphore && vkDestroyQueryPool && vkDestroyCommandPool;
	}

	void DestroyLane(Lane& a_lane) noexcept
	{
		if (!CanDestroy())
			return;
		if (a_lane.blockBuffer)
			vkDestroyBuffer(device, a_lane.blockBuffer, nullptr);
		if (a_lane.landingBuffer)
			vkDestroyBuffer(device, a_lane.landingBuffer, nullptr);
		if (a_lane.block) {
			vkUnmapMemory(device, a_lane.block);
			vkFreeMemory(device, a_lane.block, nullptr);
		}
		if (a_lane.landing)
			vkFreeMemory(device, a_lane.landing, nullptr);
		a_lane = {};  // command buffers go with the pool
	}

	void Destroy() noexcept
	{
		// At DLL_PROCESS_DETACH of a process that is ending, the Vulkan driver and the D3D12 drivers (loaded
		// after this DLL) have already been detached: calling into them is unsafe. Leave everything to
		// Windows' process teardown; the bridge's exit hook has normally released it all before this.
		if (ProcessIsEnding()) {
			std::ignore = fence.detach();
			std::ignore = deviceB.detach();
			device = VK_NULL_HANDLE;
			instance = VK_NULL_HANDLE;
			lanes.clear();
			return;
		}
		if (CanDestroy()) {
			// The Vulkan queue may wait for a "landed" that the display GPU's copy queue never signals (a
			// transfer that never finished). Give it two seconds, then release those waits from the CPU so
			// the device can go idle.
			if (fence && lastCopied && fence->GetCompletedValue() < lastCopied) {
				if (event && SUCCEEDED(fence->SetEventOnCompletion(lastCopied, event)))
					WaitForSingleObject(event, 2000);
				if (fence->GetCompletedValue() < lastCopied)
					fence->Signal(lastCopied);
			}
			vkDeviceWaitIdle(device);
			for (auto& lane : lanes)
				DestroyLane(lane);
			lanes.clear();
			if (semaphore)
				vkDestroySemaphore(device, semaphore, nullptr);
			if (queries)
				vkDestroyQueryPool(device, queries, nullptr);
			if (pool)
				vkDestroyCommandPool(device, pool, nullptr);
			vkDestroyDevice(device, nullptr);
		}
		device = VK_NULL_HANDLE;
		lanes.clear();
		semaphore = VK_NULL_HANDLE;
		queries = VK_NULL_HANDLE;
		pool = VK_NULL_HANDLE;
		fence = nullptr;
		if (instance && vkDestroyInstance) {
			vkDestroyInstance(instance, nullptr);
		}
		instance = VK_NULL_HANDLE;
		if (event) {
			CloseHandle(event);
			event = nullptr;
		}
		deviceB = nullptr;
		// vulkan-1.dll stays loaded: unloading the loader and the driver mid-session gains nothing.
	}
};

PeerCopy::PeerCopy() = default;
PeerCopy::~PeerCopy() = default;

bool PeerCopy::Initialize(ID3D12Device* a_deviceB, UINT a_slots)
{
	Shutdown();
	if (!a_deviceB)
		return false;

	auto s = std::make_unique<State>();
	s->deviceB.copy_from(a_deviceB);
	s->slots = (std::min)(a_slots, kMaxFrameSlots);

	s->loader = LoadLibraryW(L"vulkan-1.dll");
	if (!s->loader)
		return Failed("vulkan-1.dll not found", VK_ERROR_INITIALIZATION_FAILED);
	s->vkGetInstanceProcAddr =
		reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(s->loader, "vkGetInstanceProcAddr"));
	if (!s->vkGetInstanceProcAddr)
		return Failed("vulkan-1.dll has no vkGetInstanceProcAddr", VK_ERROR_INITIALIZATION_FAILED);
	s->vkCreateInstance =
		reinterpret_cast<PFN_vkCreateInstance>(s->vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance"));
	s->vkEnumerateInstanceVersion = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
		s->vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceVersion"));
	uint32_t loaderVersion = VK_API_VERSION_1_0;
	if (s->vkEnumerateInstanceVersion)
		s->vkEnumerateInstanceVersion(&loaderVersion);
	if (!s->vkCreateInstance || loaderVersion < VK_API_VERSION_1_2)
		return Failed("the Vulkan loader is older than 1.2", VK_ERROR_INCOMPATIBLE_DRIVER);

	// Implicit layers (overlays, capture tools, ReShade's Vulkan layer) have no business in this device.
	// The loader reads this while the instance is created; the previous value is put back after.
	std::array<wchar_t, 512> previous{};
	const DWORD previousLength = GetEnvironmentVariableW(L"VK_LOADER_LAYERS_DISABLE", previous.data(),
		static_cast<DWORD>(previous.size()));
	const bool hadPrevious = previousLength > 0 && previousLength < previous.size();
	SetEnvironmentVariableW(L"VK_LOADER_LAYERS_DISABLE", L"~implicit~");
	VkApplicationInfo app{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
	app.pApplicationName = "MGPU Scaling direct GPU copy";
	app.apiVersion = VK_API_VERSION_1_2;
	VkInstanceCreateInfo instanceInfo{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
	instanceInfo.pApplicationInfo = &app;
	VkResult result = s->vkCreateInstance(&instanceInfo, nullptr, &s->instance);
	SetEnvironmentVariableW(L"VK_LOADER_LAYERS_DISABLE", hadPrevious ? previous.data() : nullptr);
	if (result != VK_SUCCESS)
		return Failed("vkCreateInstance", result);

#define PEER_LOAD_INSTANCE(name) \
	s->name = reinterpret_cast<PFN_##name>(s->vkGetInstanceProcAddr(s->instance, #name));
	PEER_VK_INSTANCE_FUNCTIONS(PEER_LOAD_INSTANCE)
#undef PEER_LOAD_INSTANCE
	if (!s->vkCreateDevice || !s->vkGetDeviceProcAddr || !s->vkGetPhysicalDeviceProperties2)
		return Failed("Vulkan instance functions missing", VK_ERROR_INITIALIZATION_FAILED);

	// The display GPU, by the LUID of its D3D12 device.
	const LUID luid = a_deviceB->GetAdapterLuid();
	VkPhysicalDeviceProperties properties{};
	{
		uint32_t count = 0;
		s->vkEnumeratePhysicalDevices(s->instance, &count, nullptr);
		std::vector<VkPhysicalDevice> devices(count);
		s->vkEnumeratePhysicalDevices(s->instance, &count, devices.data());
		for (VkPhysicalDevice candidate : devices) {
			VkPhysicalDeviceIDProperties id{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
			VkPhysicalDeviceProperties2 properties2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
			properties2.pNext = &id;
			s->vkGetPhysicalDeviceProperties2(candidate, &properties2);
			if (id.deviceLUIDValid && std::memcmp(id.deviceLUID, &luid, sizeof(luid)) == 0) {
				s->physical = candidate;
				properties = properties2.properties;
				break;
			}
		}
	}
	if (!s->physical)
		return Failed("no Vulkan device on the display GPU's adapter", VK_ERROR_INITIALIZATION_FAILED);
	if (properties.apiVersion < VK_API_VERSION_1_2)
		return Failed("the display GPU's Vulkan driver is older than 1.2", VK_ERROR_INCOMPATIBLE_DRIVER);

	bool memoryWin32 = false;
	bool semaphoreWin32 = false;
	{
		uint32_t count = 0;
		s->vkEnumerateDeviceExtensionProperties(s->physical, nullptr, &count, nullptr);
		std::vector<VkExtensionProperties> extensions(count);
		s->vkEnumerateDeviceExtensionProperties(s->physical, nullptr, &count, extensions.data());
		for (const auto& extension : extensions) {
			memoryWin32 |= std::strcmp(extension.extensionName, VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME) == 0;
			semaphoreWin32 |= std::strcmp(extension.extensionName, VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME) == 0;
			s->memoryPriority |= std::strcmp(extension.extensionName, VK_EXT_MEMORY_PRIORITY_EXTENSION_NAME) == 0;
		}
	}
	if (!memoryWin32 || !semaphoreWin32)
		return Failed("the display GPU's Vulkan driver cannot share memory and fences with D3D12",
			VK_ERROR_EXTENSION_NOT_PRESENT);

	// An async-compute queue if there is one: it copies with shaders at ~150 GB/s and does not take turns
	// with frame generation's work on the graphics queue. Otherwise the graphics queue.
	{
		uint32_t count = 0;
		s->vkGetPhysicalDeviceQueueFamilyProperties(s->physical, &count, nullptr);
		std::vector<VkQueueFamilyProperties> families(count);
		s->vkGetPhysicalDeviceQueueFamilyProperties(s->physical, &count, families.data());
		uint32_t graphics = UINT32_MAX;
		uint32_t compute = UINT32_MAX;
		for (uint32_t index = 0; index < count; ++index) {
			const VkQueueFlags flags = families[index].queueFlags;
			if ((flags & VK_QUEUE_GRAPHICS_BIT) && graphics == UINT32_MAX)
				graphics = index;
			if ((flags & VK_QUEUE_COMPUTE_BIT) && !(flags & VK_QUEUE_GRAPHICS_BIT) && compute == UINT32_MAX)
				compute = index;
		}
		if (compute == UINT32_MAX && graphics == UINT32_MAX)
			return Failed("no compute or graphics queue on the display GPU", VK_ERROR_FEATURE_NOT_PRESENT);
		s->computeQueue = compute != UINT32_MAX;
		s->family = s->computeQueue ? compute : graphics;
		const uint32_t validBits = families[s->family].timestampValidBits;
		if (validBits && properties.limits.timestampPeriod > 0.0f) {
			s->nsPerTick = static_cast<double>(properties.limits.timestampPeriod);
			s->timestampMask = validBits >= 64 ? ~0ull : ((1ull << validBits) - 1);
		}
	}

	{
		const float priority = 1.0f;
		VkDeviceQueueCreateInfo queueInfo{ VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
		queueInfo.queueFamilyIndex = s->family;
		queueInfo.queueCount = 1;
		queueInfo.pQueuePriorities = &priority;
		std::vector<const char*> extensions{ VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
			VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME };
		// The blocks get the highest residency priority. VK_EXT_pageable_device_local_memory is NOT
		// enabled: it would allow the driver to page device-local memory out, the opposite of what we need.
		VkPhysicalDeviceMemoryPriorityFeaturesEXT priorityFeatures{
			VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PRIORITY_FEATURES_EXT
		};
		priorityFeatures.memoryPriority = VK_TRUE;
		VkPhysicalDeviceVulkan12Features features12{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
		features12.timelineSemaphore = VK_TRUE;
		if (s->memoryPriority) {
			extensions.push_back(VK_EXT_MEMORY_PRIORITY_EXTENSION_NAME);
			features12.pNext = &priorityFeatures;
		}
		VkDeviceCreateInfo deviceInfo{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
		deviceInfo.pNext = &features12;
		deviceInfo.queueCreateInfoCount = 1;
		deviceInfo.pQueueCreateInfos = &queueInfo;
		deviceInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
		deviceInfo.ppEnabledExtensionNames = extensions.data();
		result = s->vkCreateDevice(s->physical, &deviceInfo, nullptr, &s->device);
		if (result != VK_SUCCESS)
			return Failed("vkCreateDevice", result);
	}

#define PEER_LOAD_DEVICE(name) s->name = reinterpret_cast<PFN_##name>(s->vkGetDeviceProcAddr(s->device, #name));
	PEER_VK_DEVICE_FUNCTIONS(PEER_LOAD_DEVICE)
#undef PEER_LOAD_DEVICE
#define PEER_CHECK_DEVICE(name) || !s->name
	if (false PEER_VK_DEVICE_FUNCTIONS(PEER_CHECK_DEVICE))
		return Failed("Vulkan device functions missing", VK_ERROR_INITIALIZATION_FAILED);
#undef PEER_CHECK_DEVICE

	s->vkGetDeviceQueue(s->device, s->family, 0, &s->queue);
	s->vkGetPhysicalDeviceMemoryProperties(s->physical, &s->memory);

	VkCommandPoolCreateInfo poolInfo{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
	poolInfo.queueFamilyIndex = s->family;
	result = s->vkCreateCommandPool(s->device, &poolInfo, nullptr, &s->pool);
	if (result != VK_SUCCESS)
		return Failed("vkCreateCommandPool", result);

	if (s->nsPerTick > 0.0) {
		VkQueryPoolCreateInfo queryInfo{ VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
		queryInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
		queryInfo.queryCount = kMaxPeerLanes * kMaxFrameSlots * 2;
		if (s->vkCreateQueryPool(s->device, &queryInfo, nullptr, &s->queries) != VK_SUCCESS) {
			s->queries = VK_NULL_HANDLE;
			s->nsPerTick = 0.0;
		}
	}

	// The fence both APIs use: created on the display GPU's D3D12 device, imported as a timeline semaphore.
	{
		HRESULT hr = a_deviceB->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(s->fence.put()));
		HANDLE handle = nullptr;
		if (SUCCEEDED(hr))
			hr = a_deviceB->CreateSharedHandle(s->fence.get(), nullptr, GENERIC_ALL, nullptr, &handle);
		if (FAILED(hr))
			return Failed("shared fence on the display GPU", hr);
		VkSemaphoreTypeCreateInfo typeInfo{ VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
		typeInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
		VkSemaphoreCreateInfo semaphoreInfo{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
		semaphoreInfo.pNext = &typeInfo;
		result = s->vkCreateSemaphore(s->device, &semaphoreInfo, nullptr, &s->semaphore);
		if (result == VK_SUCCESS) {
			VkImportSemaphoreWin32HandleInfoKHR import{ VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR };
			import.semaphore = s->semaphore;
			import.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;
			import.handle = handle;
			result = s->vkImportSemaphoreWin32HandleKHR(s->device, &import);
		}
		CloseHandle(handle);
		if (result != VK_SUCCESS)
			return Failed("importing the display GPU's D3D12 fence into Vulkan", result);
	}
	s->event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

	REX::INFO("[FG-GPU1] direct GPU copy: Vulkan {}.{}.{} on \"{}\", {} (family {}), memory priority {}, timestamps {}",
		VK_API_VERSION_MAJOR(properties.apiVersion), VK_API_VERSION_MINOR(properties.apiVersion),
		VK_API_VERSION_PATCH(properties.apiVersion), static_cast<const char*>(properties.deviceName),
		s->computeQueue ? "async compute queue" : "graphics queue", s->family, s->memoryPriority ? "on" : "unavailable",
		s->nsPerTick > 0.0 ? "on" : "off");
	state = std::move(s);
	return true;
}

int PeerCopy::AddLane(const char* a_name, UINT64 a_size, UINT64 a_slotStride, UINT64 a_bytes, void*& a_mapped,
	winrt::com_ptr<ID3D12Heap>& a_landing)
{
	a_mapped = nullptr;
	a_landing = nullptr;
	if (!state || state->lanes.size() >= kMaxPeerLanes || !a_size || a_bytes > a_slotStride ||
		a_slotStride * state->slots > a_size)
		return -1;
	auto& s = *state;
	State::Lane lane;
	lane.name = a_name;
	const auto failed = [&](const char* a_what, auto a_code) {
		Failed(a_what, a_code);
		s.DestroyLane(lane);
		a_mapped = nullptr;
		return -1;
	};

	// The block: VRAM the CPU can map, which with Resizable BAR is the whole of VRAM.
	uint32_t blockType = UINT32_MAX;
	constexpr VkMemoryPropertyFlags kBlockFlags =
		VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	for (uint32_t index = 0; index < s.memory.memoryTypeCount; ++index) {
		const VkMemoryType& type = s.memory.memoryTypes[index];
		if ((type.propertyFlags & kBlockFlags) == kBlockFlags &&
			!(type.propertyFlags & VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD)) {
			blockType = index;
			break;
		}
	}
	if (blockType == UINT32_MAX)
		return failed("the display GPU offers no CPU-visible VRAM", VK_ERROR_FEATURE_NOT_PRESENT);
	const VkMemoryHeap& blockHeap = s.memory.memoryHeaps[s.memory.memoryTypes[blockType].heapIndex];
	if (blockHeap.size < kMinBarHeap) {
		REX::ERROR("[FG-GPU1] direct GPU copy: only {} MB of the display GPU's VRAM is CPU-visible; turn on Resizable BAR "
				   "(Smart Access Memory) in the BIOS",
			blockHeap.size >> 20);
		return failed("Resizable BAR is off on the display GPU", VK_ERROR_FEATURE_NOT_PRESENT);
	}

	VkMemoryPriorityAllocateInfoEXT priority{ VK_STRUCTURE_TYPE_MEMORY_PRIORITY_ALLOCATE_INFO_EXT };
	priority.priority = 1.0f;
	VkMemoryAllocateInfo blockInfo{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	blockInfo.pNext = s.memoryPriority ? &priority : nullptr;
	blockInfo.allocationSize = a_size;
	blockInfo.memoryTypeIndex = blockType;
	VkResult result = s.vkAllocateMemory(s.device, &blockInfo, nullptr, &lane.block);
	if (result != VK_SUCCESS)
		return failed("allocating the CPU-visible VRAM block", result);
	result = s.vkMapMemory(s.device, lane.block, 0, VK_WHOLE_SIZE, 0, &a_mapped);
	if (result != VK_SUCCESS) {
		a_mapped = nullptr;
		return failed("mapping the VRAM block", result);
	}

	VkBufferCreateInfo bufferInfo{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
	bufferInfo.size = a_size;
	bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	result = s.vkCreateBuffer(s.device, &bufferInfo, nullptr, &lane.blockBuffer);
	if (result == VK_SUCCESS)
		result = s.vkBindBufferMemory(s.device, lane.blockBuffer, lane.block, 0);
	if (result != VK_SUCCESS)
		return failed("the VRAM block's buffer", result);

	// The landing heap: on the display GPU's D3D12 device, where the bridge's unpack reads it.
	winrt::com_ptr<ID3D12Heap> landingHeap;
	{
		D3D12_HEAP_DESC heapDesc{};
		heapDesc.SizeInBytes = a_size;
		heapDesc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
		heapDesc.Alignment = kHeapAlignment;
		heapDesc.Flags = D3D12_HEAP_FLAG_SHARED;
		HRESULT hr = s.deviceB->CreateHeap(&heapDesc, IID_PPV_ARGS(landingHeap.put()));
		HANDLE handle = nullptr;
		if (SUCCEEDED(hr))
			hr = s.deviceB->CreateSharedHandle(landingHeap.get(), nullptr, GENERIC_ALL, nullptr, &handle);
		if (FAILED(hr))
			return failed("the D3D12 landing heap on the display GPU", hr);

		VkMemoryWin32HandlePropertiesKHR handleProperties{ VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR };
		result = s.vkGetMemoryWin32HandlePropertiesKHR(s.device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_HEAP_BIT, handle,
			&handleProperties);
		uint32_t landingType = UINT32_MAX;
		for (uint32_t index = 0; result == VK_SUCCESS && index < s.memory.memoryTypeCount; ++index) {
			const VkMemoryPropertyFlags flags = s.memory.memoryTypes[index].propertyFlags;
			if ((handleProperties.memoryTypeBits & (1u << index)) && (flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) &&
				!(flags & VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD)) {
				landingType = index;
				break;
			}
		}
		if (result == VK_SUCCESS && landingType == UINT32_MAX)
			result = VK_ERROR_FEATURE_NOT_PRESENT;
		if (result == VK_SUCCESS) {
			VkImportMemoryWin32HandleInfoKHR import{ VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR };
			import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_HEAP_BIT;
			import.handle = handle;
			VkMemoryAllocateInfo landingInfo{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
			landingInfo.pNext = &import;
			landingInfo.allocationSize = a_size;
			landingInfo.memoryTypeIndex = landingType;
			result = s.vkAllocateMemory(s.device, &landingInfo, nullptr, &lane.landing);
		}
		CloseHandle(handle);
		if (result != VK_SUCCESS)
			return failed("importing the landing heap into Vulkan", result);

		VkExternalMemoryBufferCreateInfo external{ VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO };
		external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_HEAP_BIT;
		VkBufferCreateInfo landingBufferInfo = bufferInfo;
		landingBufferInfo.pNext = &external;
		landingBufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
		result = s.vkCreateBuffer(s.device, &landingBufferInfo, nullptr, &lane.landingBuffer);
		if (result == VK_SUCCESS)
			result = s.vkBindBufferMemory(s.device, lane.landingBuffer, lane.landing, 0);
		if (result != VK_SUCCESS)
			return failed("the landing heap's buffer", result);
	}

	// One recorded copy per slot, between two timestamps. A slot's copy is only submitted again once the
	// previous one finished (the bridge's unpack of that slot waited for it), so they are never reused while
	// pending; SIMULTANEOUS_USE keeps that legal even if it were not.
	const auto laneIndex = static_cast<uint32_t>(s.lanes.size());
	VkCommandBufferAllocateInfo allocateInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
	allocateInfo.commandPool = s.pool;
	allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocateInfo.commandBufferCount = s.slots;
	result = s.vkAllocateCommandBuffers(s.device, &allocateInfo, lane.copies.data());
	if (result != VK_SUCCESS)
		return failed("allocating the copy command buffers", result);
	for (UINT slot = 0; slot < s.slots; ++slot) {
		VkCommandBuffer copy = lane.copies[slot];
		VkCommandBufferBeginInfo beginInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
		beginInfo.flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;
		s.vkBeginCommandBuffer(copy, &beginInfo);
		const uint32_t query = (laneIndex * kMaxFrameSlots + slot) * 2;
		if (s.queries) {
			s.vkCmdResetQueryPool(copy, s.queries, query, 2);
			s.vkCmdWriteTimestamp(copy, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, s.queries, query);
		}
		const VkBufferCopy region{ a_slotStride * slot, a_slotStride * slot, a_bytes };
		s.vkCmdCopyBuffer(copy, lane.blockBuffer, lane.landingBuffer, 1, &region);
		if (s.queries)
			s.vkCmdWriteTimestamp(copy, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, s.queries, query + 1);
		result = s.vkEndCommandBuffer(copy);
		if (result != VK_SUCCESS)
			return failed("recording the copy", result);
	}

	REX::INFO("[FG-GPU1] direct GPU copy: lane '{}': {:.1f} MB VRAM block (CPU-visible, mapped at {}) + {:.1f} MB landing "
			  "heap on the display GPU",
		a_name, static_cast<double>(a_size) / (1024.0 * 1024.0), a_mapped, static_cast<double>(a_size) / (1024.0 * 1024.0));
	a_landing = landingHeap;
	s.lanes.push_back(std::move(lane));
	return static_cast<int>(laneIndex);
}

bool PeerCopy::SubmitCopy(int a_lane, UINT a_slot, UINT64 a_landed, UINT64 a_copied)
{
	if (!state || a_lane < 0 || static_cast<size_t>(a_lane) >= state->lanes.size() || a_slot >= state->slots)
		return false;
	auto& s = *state;
	auto& lane = s.lanes[static_cast<size_t>(a_lane)];

	VkTimelineSemaphoreSubmitInfo timeline{ VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO };
	timeline.waitSemaphoreValueCount = 1;
	timeline.pWaitSemaphoreValues = &a_landed;
	timeline.signalSemaphoreValueCount = 1;
	timeline.pSignalSemaphoreValues = &a_copied;
	const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
	VkSubmitInfo submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
	submit.pNext = &timeline;
	submit.waitSemaphoreCount = 1;
	submit.pWaitSemaphores = &s.semaphore;
	submit.pWaitDstStageMask = &waitStage;
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &lane.copies[a_slot];
	submit.signalSemaphoreCount = 1;
	submit.pSignalSemaphores = &s.semaphore;
	const VkResult result = s.vkQueueSubmit(s.queue, 1, &submit, VK_NULL_HANDLE);
	if (result != VK_SUCCESS) {
		if (!s.submitFailed)
			REX::ERROR("[FG-GPU1] direct GPU copy: vkQueueSubmit for lane '{}' failed ({})", lane.name, VkName(result));
		s.submitFailed = true;
		return false;
	}
	s.lastCopied = (std::max)(s.lastCopied, a_copied);
	lane.stamped[a_slot] = s.queries != VK_NULL_HANDLE;
	return true;
}

ID3D12Fence* PeerCopy::Fence() const noexcept
{
	return state ? state->fence.get() : nullptr;
}

bool PeerCopy::CopyTimeMs(UINT a_slot, double& a_ms)
{
	a_ms = 0.0;
	if (!state || !state->queries || a_slot >= state->slots)
		return false;
	auto& s = *state;
	std::array<uint64_t, kMaxPeerLanes * 2> ticks{};
	bool any = false;
	for (size_t index = 0; index < s.lanes.size(); ++index) {
		if (!s.lanes[index].stamped[a_slot])
			continue;
		const auto query = static_cast<uint32_t>((index * kMaxFrameSlots + a_slot) * 2);
		// Without WAIT: VK_NOT_READY while that copy has not run yet; try again on the slot's next use.
		if (s.vkGetQueryPoolResults(s.device, s.queries, query, 2, sizeof(uint64_t) * 2, &ticks[index * 2],
				sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
			return false;
		any = true;
	}
	if (!any)
		return false;
	for (size_t index = 0; index < s.lanes.size(); ++index) {
		if (!s.lanes[index].stamped[a_slot])
			continue;
		s.lanes[index].stamped[a_slot] = false;
		const uint64_t elapsed = (ticks[index * 2 + 1] - ticks[index * 2]) & s.timestampMask;
		a_ms += static_cast<double>(elapsed) * s.nsPerTick / 1e6;
	}
	return true;
}

const char* PeerCopy::QueueName() const noexcept
{
	if (!state)
		return "none";
	return state->computeQueue ? "async compute queue" : "graphics queue";
}

void PeerCopy::Shutdown() noexcept
{
	state.reset();
}
