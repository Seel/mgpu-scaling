#pragma once

// MGPU Scaling: one F4SE plugin, AAAMGPUScaling.dll, for what used to be two FG-GPU1 builds of
// FO4Upscaling's plugins:
//
//   frame generation  plugins/FrameGeneration/src (was AAAFrameGeneration.dll): the D3D12 proxy
//                     swap chain, FSR frame generation, the second-GPU transport and FSR 4.1
//                     upscaling on the second GPU.
//   upscaler          src/upscaler, namespace up (was Upscaling.dll): render scale, jitter, TAA,
//                     mip bias and the dynamic-resolution fixes, plus FSR 3.1/DLSS on the game's GPU.
//
// One DLL, one log (MGPUScaling.log), one MCM page and one settings file with a section per part,
// and its own data folder, so it shares no file with doodlum's Upscaling or FO4Upscaling's
// AAAFrameGeneration and those mods can simply be disabled. The "AAA" keeps it the first F4SE
// plugin to load, which the frame-generation part's D3D11 device hook needs (see XSEPlugin.cpp).

#include <SimpleIni.h>

// Everything the package installs under Data\F4SE\Plugins\MGPUScaling. Wide string literals, so
// MGPU_SHADER(L"X.hlsl") is one literal.
#define MGPU_PLUGIN_DIR L"Data\\F4SE\\Plugins\\MGPUScaling\\"
#define MGPU_SHADER(a_file) MGPU_PLUGIN_DIR L"Shaders\\" a_file

namespace mgpu
{
	inline constexpr const wchar_t* kFidelityFXDir = MGPU_PLUGIN_DIR L"FidelityFX\\";

	// Shipped defaults, then the user's MCM changes on top.
	inline constexpr const char* kDefaultSettings = "Data\\MCM\\Config\\MGPUScaling\\settings.ini";
	inline constexpr const char* kUserSettings = "Data\\MCM\\Settings\\MGPUScaling.ini";
	inline constexpr const char* kFrameGenerationSection = "FrameGeneration";
	inline constexpr const char* kUpscalingSection = "Upscaling";

	// Both files into a_ini, defaults first.
	void LoadSettings(CSimpleIniA& a_ini);

	// Before F4SE::Init (the log level): bEnableDebugLogging in the [Upscaling] section.
	[[nodiscard]] bool DebugLoggingRequested();

	// Once at load, after the log exists: when the user has no MGPUScaling.ini yet, copies the
	// settings this plugin knows from the old plugins' MCM files (FrameGeneration.ini and
	// Upscaling.ini, [Settings]) into it, so MCM and the plugin both show them. The old files are
	// not touched.
	void MigrateSettings();

	// The upscaler's hooks are installed. False when another Upscaling.dll is loaded (both would
	// hook the same render calls), and then the second GPU cannot upscale either.
	[[nodiscard]] bool UpscalerActive() noexcept;
	void SetUpscalerActive(bool a_active) noexcept;
}
