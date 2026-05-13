#ifndef __SKIN_DATA_HLSLI__
#define __SKIN_DATA_HLSLI__

// Shared Skin per-geometry cbuffer — available in both VS and PS.
// Contains per-actor wetness parameters and per-bone wetness history.

#define SKIN_MAX_BONE_GROUPS 20  // 20 * 4 = 80 bones max

namespace Skin
{
	cbuffer SkinPerGeometry : register(b7)
	{
		float4 skinPerGeometry;                        // x=sweatWet, y=waterWet, z=actorPosZ, w=waterDepth
		float4 skinBoneWetness[SKIN_MAX_BONE_GROUPS];  // per-bone wetness packed as float4 (80 bones)
		float4 skinBoneWetnessParams;                  // x=hasBoneWetness(bool), y=unused, z=unused, w=unused
	};

	// Look up wetness for a specific bone index (0-79).
	// Bone wetness is packed 4 per float4.
	float GetBoneWet(uint boneIdx)
	{
		return skinBoneWetness[boneIdx >> 2][boneIdx & 3];
	}

	// Compute interpolated bone wetness from 4 bone indices and weights.
	// boneRowIndices are the row-indices into the Bones[] array (each bone = 3 rows).
	// Divide by 3 to get actual bone index.
	float ComputeInterpolatedBoneWetness(int4 boneRowIndices, float4 boneWeights)
	{
		int4 boneIdx = boneRowIndices / 3;

		float wet = 0;
		wet += GetBoneWet(boneIdx.x) * boneWeights.x;
		wet += GetBoneWet(boneIdx.y) * boneWeights.y;
		wet += GetBoneWet(boneIdx.z) * boneWeights.z;
		wet += GetBoneWet(boneIdx.w) * boneWeights.w;

		return saturate(wet);
	}
}

#endif  // __SKIN_DATA_HLSLI__
