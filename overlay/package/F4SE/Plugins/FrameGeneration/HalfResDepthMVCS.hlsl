// FG-GPU1 (iHalfResDepthMV = 1 or 2): writes the depth that crosses PCIe at half resolution,
// and the motion vectors at half (HALF_MV=1) or full (HALF_MV=0) resolution, in the same pass
// that fills the shared buffers. One thread per 2x2 block of the full-resolution frame, so it
// reads exactly what the full-resolution shaders read and writes less.
//
//   depth          the nearest of the four samples (FO4 depth: 0 = near, 1 = far), so thin
//                  foreground objects survive, like FSR3's own nearest-depth dilation.
//   HALF_MV=1      the motion vector of that same nearest sample.
//   HALF_MV=0      all four motion vectors, unchanged.
//   ALPHA_MASK=1   the transparent-surface mask of GenerateSharedBuffersCS (the PostAlpha path);
//   ALPHA_MASK=0   plain copy (the path without a reticle pass), t0/t1 unused.

#ifndef ALPHA_MASK
#	define ALPHA_MASK 1
#endif
#ifndef HALF_MV
#	define HALF_MV 0
#endif

Texture2D<float4> InputTexturePreAlpha : register(t0);
Texture2D<float4> InputTextureAfterAlpha : register(t1);
Texture2D<float2> InputMotionVectors : register(t2);
Texture2D<float> InputDepth : register(t3);

RWTexture2D<float2> OutputMotionVectors : register(u0);
RWTexture2D<float> OutputDepth : register(u1);

[numthreads(8, 8, 1)] void main(uint3 DTid : SV_DispatchThreadID)
{
	uint width, height;
	InputMotionVectors.GetDimensions(width, height);
	if (DTid.x * 2 >= width || DTid.y * 2 >= height)
		return;

	// Out-of-range loads return 0, which is the nearest possible depth; clamp instead.
	const uint2 last = uint2(width - 1, height - 1);

	float nearestDepth = 2.0;
	float2 nearestMotion = 0.0;

	[unroll] for (uint i = 0; i < 4; ++i)
	{
		const uint2 pos = min(DTid.xy * 2 + uint2(i & 1, i >> 1), last);

		float depth = InputDepth[pos];
		float2 motion = InputMotionVectors[pos];

#if ALPHA_MASK
		float3 difference = abs(InputTexturePreAlpha[pos].xyz - InputTextureAfterAlpha[pos].xyz);
		float mask = max(difference.x, max(difference.y, difference.z));
		mask *= 1000.0;
		mask = 1.0 - saturate(mask);

		motion = lerp(0.0, motion, mask);
		depth = lerp(min(depth, 0.1), depth, mask);
#endif

#if !HALF_MV
		OutputMotionVectors[pos] = motion;
#endif

		if (depth < nearestDepth) {
			nearestDepth = depth;
			nearestMotion = motion;
		}
	}

	OutputDepth[DTid.xy] = nearestDepth;
#if HALF_MV
	OutputMotionVectors[DTid.xy] = nearestMotion;
#endif
}
