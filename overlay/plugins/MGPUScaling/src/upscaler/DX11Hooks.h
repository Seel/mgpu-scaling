#pragma once

// MGPU Scaling: FO4Upscaling's Upscaling plugin, merged into this DLL. Its own namespace keeps
// its classes (Upscaling, FidelityFX, Streamline, Buffer.h, DX11Hooks) apart from the
// frame-generation part's classes of the same names.
namespace up
{

namespace DX11Hooks
{
	void Install();
}

}  // namespace up
