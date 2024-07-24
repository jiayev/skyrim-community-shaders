#define SL_INCL_STRUCT
#include "Skylighting.hlsli"

#include "../Common/DeferredShared.hlsli"
#include "../Common/FrameBuffer.hlsl"
#include "../Common/Spherical Harmonics/SphericalHarmonics.hlsli"

cbuffer SkylightingCB : register(b1)
{
	SkylightingSettings settings;
};

Texture2D<unorm float> srcOcclusionDepth : register(t0);

RWTexture3D<sh2> outProbeArray : register(u0);
RWTexture3D<uint> outAccumFramesArray : register(u1);

SamplerState samplerPointClamp : register(s0);

#define ARRAY_DIM uint3(128, 128, 64)
#define ARRAY_SIZE float3(8192, 8192, 8192 * 0.5)

[numthreads(8, 8, 1)] void main(uint3 dtid
								: SV_DispatchThreadID) {
	uint3 cellID = (int3(dtid) - settings.ArrayOrigin.xyz) % ARRAY_DIM;
	bool isValid = all(cellID >= max(0, settings.ValidMargin.xyz)) && all(cellID <= ARRAY_DIM - 1 + min(0, settings.ValidMargin.xyz));  // check if the cell is newly added

	float3 cellCentreMS = cellID + 0.5 - ARRAY_DIM / 2;
	cellCentreMS = cellCentreMS / ARRAY_DIM * ARRAY_SIZE + settings.PosOffset.xyz;

	float3 cellCentreOS = mul(settings.OcclusionViewProj, float4(cellCentreMS, 1)).xyz;
	cellCentreOS.y = -cellCentreOS.y;
	float2 occlusionUV = cellCentreOS.xy * 0.5 + 0.5;

	if (all(occlusionUV > 0) && all(occlusionUV < 1)) {
		uint accumFrames = isValid ? (outAccumFramesArray[dtid] + 1) : 1;
		if (accumFrames < 255) {
			float occlusionDepth = srcOcclusionDepth.SampleLevel(samplerPointClamp, occlusionUV, 0);
			bool visible = cellCentreOS.z < occlusionDepth;

			sh2 occlusionSH = shScale(shEvaluate(settings.OcclusionDir.xyz), float(visible));
			if (isValid) {
				float lerpFactor = rcp(accumFrames);
				occlusionSH = shAdd(shScale(outProbeArray[dtid], 1 - lerpFactor), shScale(occlusionSH, lerpFactor));
			}
			outProbeArray[dtid] = occlusionSH;
			outAccumFramesArray[dtid] = accumFrames;
		}
	} else if (!isValid) {
		outProbeArray[dtid] = float4(1, 0, 1, 0);
		outAccumFramesArray[dtid] = 0;
	}
}