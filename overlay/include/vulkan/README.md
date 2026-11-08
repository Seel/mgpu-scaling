# Vulkan headers (vendored)

Khronos Vulkan-Headers v1.3.268 (`VK_HEADER_VERSION 268`): `vk_platform.h`, `vulkan_core.h`,
`vulkan_win32.h` and the four `vk_video` codec headers `vulkan_core.h` includes. Unmodified.
Licence: Apache-2.0 OR MIT (see `LICENSE.md` and the SPDX line in each header).

Used by `plugins/FrameGeneration/src/PeerCopy.cpp` (bDirectGPUCopy) and `tools/p2p-probe`, which load
`vulkan-1.dll` at run time (`VK_NO_PROTOTYPES`); nothing links against a Vulkan SDK.
