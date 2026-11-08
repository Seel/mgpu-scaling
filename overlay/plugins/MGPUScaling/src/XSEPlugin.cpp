// MGPU Scaling's F4SE entry point. It does what the two plugins' own entry points did:
//
//   F4SEPlugin_Load   the frame-generation part's settings and its D3D11CreateDeviceAndSwapChain
//                     hook (as AAAFrameGeneration.dll did, first of all plugins)
//   kPostLoad         the upscaler's D3D11 hook and render hooks (Upscaling.dll installed them in its
//                     own Load, after the plugins that sort before it, Community Shaders included;
//                     kPostLoad comes after every plugin's Load, which keeps them outside those)
//   kPostPostLoad     the frame-generation part's render hooks (as before)
//   kGameDataReady    the upscaler's menu sink and game settings (as before)
//
// Why "AAA": the frame-generation part creates the game's D3D11 device itself and does not call
// the next hook in the import table, so its hook must be the first one installed (the innermost).
// F4SE loads plugins in file-name order.

#include "MGPUScaling.h"

#include "DX11Hooks.h"  // frame generation (plugins/FrameGeneration/src)
#include "Upscaling.h"  // frame generation's state, not the upscaler
#include "upscaler/Upscaler.h"

// OG F4SE uses F4SEPlugin_Query, NG uses F4SEPlugin_Version. Export both.
extern "C" DLLEXPORT bool F4SEAPI F4SEPlugin_Query(const F4SE::QueryInterface*, F4SE::PluginInfo* a_info)
{
	a_info->infoVersion = F4SE::PluginInfo::kVersion;
	a_info->name = Plugin::NAME.data();
	a_info->version = Plugin::VERSION[0];
	return true;
}

extern "C" DLLEXPORT constinit auto F4SEPlugin_Version = []() noexcept {
	F4SE::PluginVersionData data{};

	data.PluginVersion(Plugin::VERSION);
	data.PluginName(Plugin::NAME.data());
	data.AuthorName("");
	data.UsesAddressLibrary(true);
	data.UsesSigScanning(false);
	data.IsLayoutDependent(true);
	data.HasNoStructUse(false);
	data.CompatibleVersions({ F4SE::RUNTIME_LATEST });

	return data;
}();

namespace
{
	void MessageHandler(F4SE::MessagingInterface::Message* a_message)
	{
		switch (a_message->type) {
		case F4SE::MessagingInterface::kPostLoad:
			up::OnPostLoad();
			break;
		case F4SE::MessagingInterface::kPostPostLoad:
			Upscaling::GetSingleton()->PostPostLoad();
			break;
		case F4SE::MessagingInterface::kGameDataReady:
			up::OnDataLoaded();
			break;
		default:
			break;
		}
	}

	void WarnUser(const wchar_t* a_text)
	{
		MessageBoxW(nullptr, a_text, L"MGPU Scaling", MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
	}
}

extern "C" DLLEXPORT bool F4SEAPI F4SEPlugin_Load(const F4SE::LoadInterface* a_f4se)
{
	F4SE::InitInfo initInfo{};
	initInfo.logName = "MGPUScaling";
	initInfo.logLevel = mgpu::DebugLoggingRequested() ? REX::ELogLevel::Debug : REX::ELogLevel::Info;
	initInfo.trampoline = true;
	initInfo.trampolineSize = 1 << 11;  // the two plugins asked for 1 KiB each
	F4SE::Init(a_f4se, initInfo);

#ifndef NDEBUG
	while (!REX::W32::IsDebuggerPresent()) {};
#endif

	// FO4Upscaling's AAAFrameGeneration.dll sorts before this plugin and hooks the same D3D11 call;
	// its hook would create the device and this one would never run.
	if (GetModuleHandleW(L"AAAFrameGeneration.dll")) {
		REX::CRITICAL("[INIT] AAAFrameGeneration.dll (FO4Upscaling's frame generation, or the old FG-GPU1 build) is loaded. "
					  "MGPU Scaling replaces it: disable that mod. MGPU Scaling does nothing this session");
		WarnUser(L"AAAFrameGeneration.dll is installed as well.\n\n"
				 L"MGPU Scaling replaces it (frame generation and upscaling in one plugin). Disable the other frame "
				 L"generation mod (FO4Upscaling / FG-GPU1) in your mod manager.\n\n"
				 L"MGPU Scaling stays off for this session.");
		return false;
	}

	mgpu::MigrateSettings();

	// Frame generation, as AAAFrameGeneration.dll did.
	Upscaling::GetSingleton()->LoadSettings();
	DX11Hooks::Install();

	// The upscaler, as Upscaling.dll did; its hooks follow at kPostLoad.
	up::OnLoad();

	const auto messaging = F4SE::GetMessagingInterface();
	messaging->RegisterListener(MessageHandler);

	return true;
}
