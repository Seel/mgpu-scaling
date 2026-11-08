cmake_minimum_required(VERSION 3.21)

project(
	FO4Upscaling
	LANGUAGES CXX
)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_DEBUG OFF)

set(Boost_USE_STATIC_LIBS ON)
set(Boost_USE_STATIC_RUNTIME ON)

# Shared dependency: CommonLibF4
add_subdirectory(extern/CommonLibF4 CommonLibF4 EXCLUDE_FROM_ALL)

# Shared vcpkg packages
find_package(directxtk CONFIG REQUIRED)
find_package(directxtex CONFIG REQUIRED)
find_package(directx-headers CONFIG REQUIRED)
find_package(magic_enum CONFIG REQUIRED)

# Plugin targets
# MGPU Scaling (FG-GPU1): plugins/MGPUScaling builds AAAMGPUScaling.dll from the frame-generation
# sources in plugins/FrameGeneration/src and the upscaler in plugins/MGPUScaling/src/upscaler. The
# separate AAAFrameGeneration target is not configured: its sources need MGPUScaling.h now.
# plugins/Upscaling stays: it defines the FidelityFX SDK 1.x targets the upscaler links (its own
# Upscaling target still builds from the unmodified upstream sources, but is not packaged).
add_subdirectory(plugins/Upscaling)
add_subdirectory(plugins/MotionVectorFixes)
add_subdirectory(plugins/MGPUScaling)
