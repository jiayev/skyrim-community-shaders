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
	// The specular and envmap LOD fades, made by the draw from the frame's camera (Lighting.hlsl, DCLFFrameLighting c6-c12):
	// the fade node's world bound centre, and its LOD type (bits 0-3) and which fades apply (bit 4 specular, 5 envmap, 6
	// SSRParams.w with specular) in the word; 0 when nothing fades and MaterialData's fades are the property's.
	float3 DCLFLodFadeNode;
	uint DCLFLodFadeFlags;
	uint4 DCLFReserved[2];  // to 256 bytes: a constant-buffer block a row
};

// The indirect draw's push data (DrawPipelines.h, kDrawPushWords). The first four words are its rows' addresses (the
// pipeline row's, then the material row's), which the pipeline layout consumes to resolve this draw's buffers and
// descriptors; only the object word is read here.
cbuffer DCLFPushData : register(b190)
{
	uint2 DCLFPipelineRowAddress : packoffset(c0.x);
	uint2 DCLFMaterialRowAddress : packoffset(c0.z);
	uint DCLFObjectWord : packoffset(c1.x);
};

// The object word (Records.h): the object's index in bits 0-25; the local shadow lights BuildDrawsCS selected for the draw
// in bits 26-29 (Light Limit Fix's ShadowBitMask, whose lights are shadow mask channels 0-3); and in the top bit
// kObjectSunMiss, which BuildDrawsCS sets on a pass carrying the sun's bits whose bound meets none of this frame's cascades.
static const uint DCLFObjectIndex = DCLFObjectWord & 0x03FFFFFFu;
static const uint DCLFLocalShadowMask = (DCLFObjectWord >> 26) & 0xFu;
static const bool DCLFSunMiss = (DCLFObjectWord & 0x80000000u) != 0;

// The object rows, by the draw's object index. Read as a structured buffer: reading the row as a constant buffer at its
// address (a per-draw push address) measured no faster on NVIDIA (dclf-architecture.md, "Object rows").
StructuredBuffer<DCLFObjectRecord> DCLFObjects : register(t127);
// The epoch's row buffer (IndirectDraws: kBonesBufferRegister): every skinned object's bone palette
// rows end to end, current then previous, and after them the per-object extras rows (DCLFExtraOffset).
StructuredBuffer<float4> DCLFBones : register(t126);

#endif  // DCLF_BINDLESS
#endif  // __DCLF_OBJECTS_DEPENDENCY_HLSL__
