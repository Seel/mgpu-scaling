#include "Upscaler.h"

#include "DX11Hooks.h"
#include "Upscaling.h"

#include "ENB/ENBSeriesAPI.h"
#include "MGPUScaling.h"

namespace up
{
	bool enbLoaded = false;

	namespace
	{
		bool installed = false;
	}

	void OnLoad()
	{
		if (ENB_API::RequestENBAPI()) {
			REX::INFO("[ENB] ENB detected — native AA mode (DLAA/FSR)");
			enbLoaded = true;
		} else {
			REX::INFO("[ENB] ENB not detected — full pipeline active");
		}
	}

	bool OnPostLoad()
	{
		// doodlum's Upscaling and FO4Upscaling's own build are both Upscaling.dll. Both would hook
		// the render calls this part hooks, so only one of them may run.
		if (GetModuleHandleW(L"Upscaling.dll")) {
			REX::CRITICAL("[INIT] another Upscaling.dll is loaded (doodlum's Upscaling, or FO4Upscaling's). MGPU Scaling "
						  "includes the upscaler: disable that mod. This session the upscaler here, and with it FSR 4.1 on the "
						  "second GPU, is off; frame generation still runs");
			mgpu::SetUpscalerActive(false);
			return false;
		}

		REX::INFO("[INIT] Installing DX11 hooks...");
		DX11Hooks::Install();
		REX::INFO("[INIT] DX11 hooks installed");

		REX::INFO("[INIT] Installing upscaling hooks...");
		Upscaling::InstallHooks();
		REX::INFO("[INIT] Upscaling hooks installed");

		installed = true;
		mgpu::SetUpscalerActive(true);
		return true;
	}

	void OnDataLoaded()
	{
		if (!installed)
			return;
		REX::INFO("[INIT] Data loaded");
		Upscaling::GetSingleton()->OnDataLoaded();
	}
}
