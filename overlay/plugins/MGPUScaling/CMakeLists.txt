# MGPU Scaling: AAAMGPUScaling.dll, the frame-generation plugin (plugins/FrameGeneration/src, the
# FG-GPU1 build of AAAFrameGeneration) and the upscaler (src/upscaler, the FG-GPU1 build of
# Upscaling, in namespace up) as one F4SE plugin. Both plugins' own XSEPlugin.cpp are replaced by
# src/XSEPlugin.cpp.
set(PLUGIN_NAME "MGPUScaling")

add_library(${PLUGIN_NAME} SHARED)

set(FRAMEGEN_DIR "${CMAKE_SOURCE_DIR}/plugins/FrameGeneration/src")
file(GLOB FRAMEGEN_SOURCES
	LIST_DIRECTORIES false
	CONFIGURE_DEPENDS
	"${FRAMEGEN_DIR}/*.cpp"
	"${FRAMEGEN_DIR}/*.h"
)
list(FILTER FRAMEGEN_SOURCES EXCLUDE REGEX "/XSEPlugin\.cpp$")

file(GLOB_RECURSE PLUGIN_SOURCES
	LIST_DIRECTORIES false
	CONFIGURE_DEPENDS
	"${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/src/*.h"
)

target_sources(${PLUGIN_NAME} PRIVATE ${FRAMEGEN_SOURCES} ${PLUGIN_SOURCES})
source_group("FrameGeneration" FILES ${FRAMEGEN_SOURCES})

include(Common)
configure_xse_plugin(${PLUGIN_NAME} 2 0 0)

# "AAA": the frame-generation part's D3D11 device hook must be installed before any other plugin's.
set_target_properties(${PLUGIN_NAME} PROPERTIES OUTPUT_NAME "AAAMGPUScaling")

# src/ (MGPUScaling.h) comes from configure_xse_plugin. The frame-generation headers are found
# through this path; the upscaler's resolve next to the files that include them, so the two sets
# of same-named headers (Upscaling.h, FidelityFX.h, Streamline.h, DX11Hooks.h, Buffer.h) never mix.
target_include_directories(${PLUGIN_NAME} PRIVATE
	${FRAMEGEN_DIR}
	${CMAKE_SOURCE_DIR}/extern/Streamline/external/ngx-sdk/include
)

# Streamline and XeSS are interface targets that skip themselves when defined already.
add_subdirectory(${CMAKE_SOURCE_DIR}/cmake/Streamline ${CMAKE_CURRENT_BINARY_DIR}/Streamline)
add_subdirectory(${CMAKE_SOURCE_DIR}/cmake/XeSS ${CMAKE_CURRENT_BINARY_DIR}/XeSS)

# The union of both plugins' libraries. The FidelityFX SDK 1.x targets (the upscaler's FSR 3.1 on
# the game's GPU) are defined by plugins/Upscaling, which the top-level CMakeLists adds first.
target_link_libraries(${PLUGIN_NAME} PRIVATE
	debug ${CMAKE_SOURCE_DIR}/include/detours/Debug/detours.lib
	optimized ${CMAKE_SOURCE_DIR}/include/detours/Release/detours.lib
	Microsoft::DirectXTK
	Microsoft::DirectXTex
	Microsoft::DirectX-Headers
	d3d12.lib
	magic_enum::magic_enum
	d3dcompiler.lib
	Streamline
	${CMAKE_SOURCE_DIR}/extern/Streamline/external/ngx-sdk/lib/Windows_x86_64/nvsdk_ngx_d.lib
	XeSS
	ffx_backend_dx11_x64
	ffx_fsr3_x64
)
