#pragma once

// The upscaler's part of the plugin entry (what Upscaling.dll's XSEPlugin.cpp did). Kept free of
// the upscaler's own headers, so the entry point can include it next to the frame-generation ones.

namespace up
{
	// F4SEPlugin_Load, after F4SE::Init.
	void OnLoad();
	// kPostLoad: installs the D3D11 device hook and the render hooks, unless another Upscaling.dll
	// is loaded. These ran in Upscaling.dll's own F4SEPlugin_Load, i.e. after every plugin that
	// sorts before "Upscaling.dll" (Community Shaders among them) had installed its hooks; kPostLoad
	// keeps that order now that the code loads first.
	bool OnPostLoad();
	// kGameDataReady.
	void OnDataLoaded();
}
