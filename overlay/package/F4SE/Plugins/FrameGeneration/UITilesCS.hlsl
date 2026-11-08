// FG-GPU1 UI tiles, measurement only (plans/UI-TILES.md part 2, step 1).
//
// On a frame upscaled on the display GPU, the frame buffer was filled from the stretched scene
// (Upscaling::FillFrameBufferFromScene) and the UI was drawn over it, so the finished frame differs
// from that background exactly where something was drawn after PostDisplay. One 32x32 group per
// tile: counts the 32x32 tiles and the 16x16 quarter tiles with any differing pixel, and the
// differing pixels themselves.
//
// Defines: WIDTH, HEIGHT (the frame's size).

Texture2D<float4> Finished : register(t0);    // the frame as presented (scene stretch + UI)
Texture2D<float4> Background : register(t1);  // the stretch alone (Upscaling::sceneBlitTarget)

// [0] 32x32 tiles with UI, [1] 16x16 tiles with UI, [2] pixels with UI. Cleared before the dispatch.
RWByteAddressBuffer Counters : register(u0);

groupshared uint quarters;
groupshared uint pixels;

[numthreads(32, 32, 1)] void main(uint3 dtid : SV_DispatchThreadID, uint3 gtid : SV_GroupThreadID, uint gi : SV_GroupIndex)
{
	if (gi == 0) {
		quarters = 0;
		pixels = 0;
	}
	GroupMemoryBarrierWithGroupSync();

	// Both are RGBA8_UNORM read the same way, so equal bytes give equal floats: an exact compare.
	// Alpha is left out: the display GPU's composite uses only the colour.
	if (dtid.x < WIDTH && dtid.y < HEIGHT && any(Finished[dtid.xy].rgb != Background[dtid.xy].rgb)) {
		InterlockedOr(quarters, 1u << ((gtid.y / 16) * 2 + gtid.x / 16));
		InterlockedAdd(pixels, 1);
	}
	GroupMemoryBarrierWithGroupSync();

	if (gi == 0 && quarters != 0) {
		Counters.InterlockedAdd(0, 1);
		Counters.InterlockedAdd(4, countbits(quarters));
		Counters.InterlockedAdd(8, pixels);
	}
}
