// FG-GPU1: FSR 4.1 upscaling on the second GPU.
//
// The render GPU draws the scene at render resolution into the top-left corner of the full-size
// frame buffer. The UI then needs a full-size background to blend over, and the second GPU needs
// the UI without that background:
//
//   Blit       (render GPU, D3D11) fills the frame buffer from the render-resolution scene, so the
//              UI is drawn over it at full resolution. It is never shown as it is.
//   Composite  (display GPU, D3D12) puts the UI on top of the FSR 4.1 result. It rebuilds the same
//              background from the same scene with the same arithmetic and keeps what the UI changed:
//                  out = final + (upscaled - background) * (1 - alpha)
//              with alpha the smallest UI coverage that explains final - background. Where there is
//              no UI, final == background and out == upscaled; under opaque UI, out == final.
//
//   Copy       (display GPU, D3D12) the finished frame unchanged into the composite, for a swap chain
//              in another format than the game's RGBA8 (an HDR add-on promoted it): copies cannot
//              convert, a shader store can. Native frames, and every frame without upscaling.
//
// Compiled with RENDER_WIDTH, RENDER_HEIGHT, DISPLAY_WIDTH, DISPLAY_HEIGHT defined. All entry
// points use the same registers; Blit only reads t0, Copy only t1, and both write u0.

static const uint2 RenderSize = uint2(RENDER_WIDTH, RENDER_HEIGHT);
static const uint2 DisplaySize = uint2(DISPLAY_WIDTH, DISPLAY_HEIGHT);

Texture2D<float4> Scene : register(t0);     // the render-resolution scene (the lane both GPUs have)
Texture2D<float4> Final : register(t1);     // Composite: the finished frame from the render GPU
Texture2D<float4> Upscaled : register(t2);  // Composite: the FSR 4.1 output
RWTexture2D<float4> Output : register(u0);

// Bilinear from render to display resolution with plain loads and precise arithmetic (no fused
// or reordered operations), so both GPUs get the same background from the same scene. Pixel
// centres map onto pixel centres.
float4 SceneBilinear(uint2 a_pixel)
{
	precise float2 scale = float2(RenderSize) / float2(DisplaySize);
	precise float2 position = (float2(a_pixel) + 0.5) * scale - 0.5;
	position = clamp(position, 0.0, float2(RenderSize - 1));

	uint2 i0 = uint2(position);  // position >= 0, so truncation is floor
	uint2 i1 = min(i0 + 1, RenderSize - 1);
	precise float2 f = position - float2(i0);

	float4 c00 = Scene.Load(int3(i0.x, i0.y, 0));
	float4 c10 = Scene.Load(int3(i1.x, i0.y, 0));
	float4 c01 = Scene.Load(int3(i0.x, i1.y, 0));
	float4 c11 = Scene.Load(int3(i1.x, i1.y, 0));

	precise float4 top = c00 + (c10 - c00) * f.x;
	precise float4 bottom = c01 + (c11 - c01) * f.x;
	precise float4 result = top + (bottom - top) * f.y;
	return result;
}

[numthreads(8, 8, 1)] void Blit(uint3 a_id : SV_DispatchThreadID)
{
	if (any(a_id.xy >= DisplaySize))
		return;
	Output[a_id.xy] = SceneBilinear(a_id.xy);
}

[numthreads(8, 8, 1)] void Composite(uint3 a_id : SV_DispatchThreadID)
{
	if (any(a_id.xy >= DisplaySize))
		return;

	// The render GPU stored its background in an 8-bit target before drawing the UI over it.
	float3 background = round(saturate(SceneBilinear(a_id.xy).rgb) * 255.0) / 255.0;
	float3 final = Final.Load(int3(a_id.xy, 0)).rgb;
	float3 upscaled = Upscaled.Load(int3(a_id.xy, 0)).rgb;

	// UI with coverage a and colour c moves a channel by a * (c - background), and |c - background|
	// is at most max(background, 1 - background): this is the smallest a that explains the change.
	// Any error is (upscaled - background) * (alpha - a): fine scene detail under translucent UI.
	float3 change = abs(final - background);
	float3 room = max(max(background, 1.0 - background), 1.0 / 255.0);
	float3 coverage = change / room;
	float alpha = saturate(max(coverage.r, max(coverage.g, coverage.b)));

	Output[a_id.xy] = float4(saturate(final + (upscaled - background) * (1.0 - alpha)), 1.0);
}

[numthreads(8, 8, 1)] void Copy(uint3 a_id : SV_DispatchThreadID)
{
	if (any(a_id.xy >= DisplaySize))
		return;
	Output[a_id.xy] = float4(Final.Load(int3(a_id.xy, 0)).rgb, 1.0);
}
