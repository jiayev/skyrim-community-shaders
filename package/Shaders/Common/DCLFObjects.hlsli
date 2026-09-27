#ifndef __DCLF_OBJECTS_DEPENDENCY_HLSL__
#define __DCLF_OBJECTS_DEPENDENCY_HLSL__

// Drawcall Limit Fix draws its permutations indirectly, and the handful of values that differ between the
// objects sharing one pipeline come from a GPU-resident table indexed by the draw's own object index
// rather than from per-draw constant buffers. That is what lets the PerGeometry buffer become
// per-pipeline and, once nothing else in the binding record is per-object, lets the record itself
// deduplicate to one per (material, pipeline) pair.
//
// This lives in its own header rather than in Lighting.hlsl because Color.hlsli consumes the emissive
// multiplier and is included before the point where the block used to sit.
#if defined(DCLF_BINDLESS)

struct DCLFObjectRecord
{
	float4 World[3];          // row_major float3x4, absolute: the vertex stage subtracts the drawing camera's eye
	float4 PreviousWorld[3];  // likewise, against the previous eye
	float4 MaterialData;
	float4 EmitColor;         // emissive in xyz, the per-object w of SSRParams in w
	// The values that used to reach the shader as constant buffers of their own: Light Limit Fix's room
	// index and shadow bit mask (b3), the alpha test reference (b11) and Linear Lighting's emissive
	// multiplier (b8). Only read when DCLF_BINDLESS_DRAW is also defined; the record carries them either
	// way so that the two builds share one layout.
	int RoomIndex;
	uint ShadowBitMask;
	float AlphaTestRef;
	float EmissiveMult;
	// Tree animation (technique 12). Per object: under DCLF_BINDLESS the PerGeometry buffer is one
	// block for the whole pipeline, and a tree's wind amplitude and clock are its own.
	float4 DCLFTreeParams;
	float4 DCLFWindTimers;  // xy used
	// Skinning: this object's bone palette rows in DCLFBones (three float4 rows a bone, current palette
	// at DCLFBoneOffset, the previous frame's at DCLFPreviousBoneOffset), absolute like World.
	uint DCLFBoneOffset;
	uint DCLFPreviousBoneOffset;
	uint DCLFBoneRows;
	// The object's rows of further per-object PerGeometry values in the same buffer, after every
	// palette: LandBlendParams, the three rows of TextureProj, then ProjectedUVParams, 2 and 3.
	uint DCLFExtraOffset;
	// Advanced Skin's SkinPerGeometry (b7): the owning actor's wetness, zero for everything else. Only read
	// when DCLF_BINDLESS_DRAW is also defined (Skin.hlsli).
	float4 DCLFSkinPerGeometry;
};

// The indirect draw's push data. The first two words are the binding record's address, which the pipeline
// layout consumes to resolve this draw's buffers and descriptors; only the object word is read here.
cbuffer DCLFPushData : register(b190)
{
	uint2 DCLFRecordAddress : packoffset(c0.x);
	uint DCLFObjectWord : packoffset(c0.z);
};

// The object word: the object's index, and in the top bit kObjectSunMiss (Records.h), which BuildDrawsCS sets on
// a synthetic pass carrying the sun's bits whose bound meets none of this frame's cascades.
static const uint DCLFObjectIndex = DCLFObjectWord & 0x7FFFFFFFu;
static const bool DCLFSunMiss = (DCLFObjectWord & 0x80000000u) != 0;

StructuredBuffer<DCLFObjectRecord> DCLFObjects : register(t127);
// The epoch's row buffer (IndirectDraws: kBonesBufferRegister): every skinned object's bone palette
// rows end to end, current then previous, and after them the per-object extras rows (DCLFExtraOffset).
StructuredBuffer<float4> DCLFBones : register(t126);

#endif  // DCLF_BINDLESS
#endif  // __DCLF_OBJECTS_DEPENDENCY_HLSL__
