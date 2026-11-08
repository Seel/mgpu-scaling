#include "MGPUScaling.h"

#include <atomic>
#include <filesystem>
#include <iterator>
#include <string_view>
#include <system_error>

namespace
{
	std::atomic_bool upscalerActive{ false };

	struct LegacyFile
	{
		const char* path;
		const char* section;  // in MGPUScaling.ini
		const char* const* keys;
		std::size_t keyCount;
	};

	// Every key the two old plugins read from their [Settings] section.
	constexpr const char* kFrameGenerationKeys[] = { "bFrameGenerationMode", "bFrameLimitMode", "bDisableInMenus",
		"iFrameGenType", "iFrameGenFrames", "iFSRRuntime", "iFSRSwapChainProvider", "iFSRFrameGenProvider",
		"bEnableDebugLogging", "bFrameGenOnSecondaryGPU", "sSecondaryGPUName", "bSendHudlessFrame", "bTransportHudLess",
		"bAsyncUnpack", "bEarlyTransfer", "bEarlyUnpack", "iHalfResDepthMV" };
	constexpr const char* kUpscalingKeys[] = { "iUpscaleMethodPreference", "iQualityMode", "fSharpness", "bEnableDebugLogging" };
}

void mgpu::LoadSettings(CSimpleIniA& a_ini)
{
	a_ini.LoadFile(kDefaultSettings);
	a_ini.LoadFile(kUserSettings);
}

bool mgpu::DebugLoggingRequested()
{
	CSimpleIniA ini;
	ini.SetUnicode();
	LoadSettings(ini);
	return ini.GetBoolValue(kUpscalingSection, "bEnableDebugLogging", false);
}

void mgpu::MigrateSettings()
{
	std::error_code error;
	if (std::filesystem::exists(kUserSettings, error))
		return;

	// The MCM files the FG-GPU1 builds of AAAFrameGeneration.dll and Upscaling.dll used. Upscaling.ini
	// is shared with doodlum's Upscaling MCM page, so only this plugin's own keys are taken.
	const LegacyFile legacy[] = {
		{ "Data\\MCM\\Settings\\FrameGeneration.ini", kFrameGenerationSection, kFrameGenerationKeys, std::size(kFrameGenerationKeys) },
		{ "Data\\MCM\\Settings\\Upscaling.ini", kUpscalingSection, kUpscalingKeys, std::size(kUpscalingKeys) },
	};

	CSimpleIniA out;
	out.SetUnicode();
	int copied = 0;
	for (const auto& file : legacy) {
		CSimpleIniA in;
		in.SetUnicode();
		if (in.LoadFile(file.path) < 0)
			continue;
		for (std::size_t i = 0; i < file.keyCount; ++i) {
			const char* value = in.GetValue("Settings", file.keys[i], nullptr);
			if (!value)
				continue;
			// bTransportHudLess is the old name of bSendHudlessFrame.
			const char* key = std::string_view(file.keys[i]) == "bTransportHudLess" ? "bSendHudlessFrame" : file.keys[i];
			if (key != file.keys[i] && out.GetValue(file.section, key, nullptr))
				continue;
			out.SetValue(file.section, key, value);
			++copied;
		}
	}
	if (copied == 0)
		return;

	// No UTF-8 signature: MCM reads this file too.
	if (out.SaveFile(kUserSettings, false) < 0) {
		REX::WARN("[INIT] could not write {}; the settings from the old FrameGeneration.ini / Upscaling.ini are not carried over",
			kUserSettings);
		return;
	}
	REX::INFO("[INIT] carried {} settings over from Data\\MCM\\Settings\\FrameGeneration.ini and Upscaling.ini into {} "
			  "(the old files are left as they are)",
		copied, kUserSettings);
}

bool mgpu::UpscalerActive() noexcept
{
	return upscalerActive.load(std::memory_order_relaxed);
}

void mgpu::SetUpscalerActive(bool a_active) noexcept
{
	upscalerActive.store(a_active, std::memory_order_relaxed);
}
