// FG-GPU1 (iHalfResDepthMV = 1): runs on the frame-generation GPU. Rebuilds full-resolution
// depth from the half-resolution depth that crossed PCIe, so FSR3 still works at full
// resolution and reads every motion vector; only its depth tests see the coarser depth.
//
// Smooth surfaces (floors, walls) are interpolated bilinearly. Where the four neighbouring
// samples differ a lot - an object edge - blending would invent a depth that belongs to neither
// surface and FSR3 would see a false edge or disocclusion there, so the pixel takes the sample
// of its own 2x2 block instead (which already holds that block's nearest depth).

Texture2D<float> HalfDepth : register(t0);
RWTexture2D<float> FullDepth : register(u0);

// FO4 depth runs 0 (near) .. 1 (far) and 1 - depth is close to proportional to 1/distance.
// Neighbours whose 1/distance differs by more than this fraction count as an edge.
static const float kEdgeThreshold = 0.05;

[numthreads(8, 8, 1)] void main(uint3 DTid : SV_DispatchThreadID)
{
	uint fullWidth, fullHeight, halfWidth, halfHeight;
	FullDepth.GetDimensions(fullWidth, fullHeight);
	HalfDepth.GetDimensions(halfWidth, halfHeight);
	if (DTid.x >= fullWidth || DTid.y >= fullHeight)
		return;

	const int2 last = int2(halfWidth - 1, halfHeight - 1);

	// Position in half-resolution texels: full pixel 2i sits at i - 0.25, 2i + 1 at i + 0.25.
	const float2 position = (float2(DTid.xy) + 0.5) * 0.5 - 0.5;
	const int2 base = int2(floor(position));
	const float2 weight = position - float2(base);

	const float d00 = HalfDepth[clamp(base, int2(0, 0), last)];
	const float d10 = HalfDepth[clamp(base + int2(1, 0), int2(0, 0), last)];
	const float d01 = HalfDepth[clamp(base + int2(0, 1), int2(0, 0), last)];
	const float d11 = HalfDepth[clamp(base + int2(1, 1), int2(0, 0), last)];

	const float w00 = 1.0 - d00;
	const float w10 = 1.0 - d10;
	const float w01 = 1.0 - d01;
	const float w11 = 1.0 - d11;
	const float nearest = max(max(w00, w10), max(w01, w11));
	const float farthest = min(min(w00, w10), min(w01, w11));

	float depth;
	if (nearest <= farthest * (1.0 + kEdgeThreshold))
		depth = lerp(lerp(d00, d10, weight.x), lerp(d01, d11, weight.x), weight.y);
	else
		depth = HalfDepth[min(DTid.xy / 2, uint2(last))];

	FullDepth[DTid.xy] = depth;
}
