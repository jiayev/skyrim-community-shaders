// SPDX-License-Identifier: MIT AND (LicenseRef-GPL-3.0-with-Modding-Exception WITH GPL-3.0-linking-source-exception)
// Original code Copyright (c) 2022 Ilya Perapechka (MIT), modifications GPL-3.0 with Modding Exception (see EXCEPTIONS.md)

#if defined(DCLF_PULLED)
// Drawcall Limit Fix's grass draws (dclf-architecture.md, "Grass: the draws"; Features/Grass/GrassLayout.h): per pass, a multi-draw
// whose count and arguments the cull wrote, one non-indexed instanced draw a bucket's tier, whose instances (firstInstance on) are the
// visible list's entries. The vertex stage fetches its instance, its shape's row, its bucket's, its mesh's index and vertex and the
// cull's InstanceFrame (wind, fade, tier) through the device addresses in the draw row; the pixel stages take the bucket's textures,
// samplers and alpha reference, and the frame's shadow mask, from the rows and the descriptor heap. It binds nothing a draw would
// vary: the engine's per-pass PerGeometry, PerMaterial, alpha reference and permutation blocks become statics set from the rows
// (DCLFGrassStatics), Grass Collision's texture and block come from the draw row (GRASS_COLLISION_EXTERNAL), and Grass
// Optimizations' instance buffer is replaced by the cull's (its path is not taken: the instances are DCLF's).
// The permutation block's values become assignable statics (Permutation.hlsli).
#	define DCLF_PULLED_ROWS
#	undef GRASS_OPTIMIZATIONS
#	define GRASS_COLLISION_EXTERNAL
// No object rows: a family's push data is its own (below), not DCLFObjects.hlsli's, which Light Limit Fix's header would include.
#	undef DCLF_BINDLESS
#	undef DCLF_BINDLESS_DRAW
// The draw's push data (DrawPipelines.h, kDrawPushBinding): the draw row's address, the pass's phase and the colour program (0 base,
// 1 TruePBR). The phase needs no reading: a phase-two draw's firstInstance already starts past phase one's instances.
cbuffer DCLFGrassPush : register(b190)
{
	uint2 DCLFGrassDrawAddress : packoffset(c0.x);
	uint DCLFGrassPhase : packoffset(c0.z);
	uint DCLFGrassProgram : packoffset(c0.w);
	uint2 DCLFGrassUnused : packoffset(c1.x);
};
#	include "Common/DCLFVertexPull.hlsli"
uint64_t DCLFGrassDraw() { return DCLFAddress(DCLFGrassDrawAddress); }
// GrassInstances::DrawRow: the tables' addresses, then the frame's values.
uint64_t DCLFGrassTable(uint a_offset) { return DCLFAddress(vk::RawBufferLoad<uint2>(DCLFGrassDraw() + a_offset, 8)); }
uint DCLFGrassDrawUint(uint a_offset) { return vk::RawBufferLoad<uint>(DCLFGrassDraw() + a_offset, 4); }
float4 DCLFGrassDrawFloat4(uint a_offset) { return asfloat(vk::RawBufferLoad<uint4>(DCLFGrassDraw() + a_offset, 4)); }
static const uint kDCLFGrassInstances = 0, kDCLFGrassShapes = 8, kDCLFGrassBuckets = 16, kDCLFGrassMeshes = 24, kDCLFGrassOwners = 32,
				  kDCLFGrassFrame = 40, kDCLFGrassVisible = 48;
static const uint kDCLFGrassWindVector = 64, kDCLFGrassTimeBase = 80, kDCLFGrassDirLightDirection = 96, kDCLFGrassDirLightColor = 112,
				  kDCLFGrassAmbientColor = 128, kDCLFGrassShadowMaskIndex = 144, kDCLFGrassCollisionIndex = 148, kDCLFGrassCollisionPerFrame = 160;
// CullRow (at 256): AlphaParam1 and AlphaParam2 are its alphaParam1, alphaParam2 (words 9 and 10).
static const uint kDCLFGrassAlphaParams = 256 + 36;
// GrassInstances::BucketRow (160 bytes): mesh[3] by tier, flags, ..., diffuse, depth sampler, colour sampler, alpha reference at 32;
// wavePeriod at 64, pbrFlags at 76, scaleMask at 80, normal, RMAOS, features textures and the PBR sampler at 96, pbrParams1 and 2,
// windDirection at 144.
static const uint kDCLFGrassBucketStride = 208, kDCLFGrassBucketFlags = 12, kDCLFGrassBucketTextures = 32, kDCLFGrassBucketWavePeriod = 64,
				  kDCLFGrassBucketPbrFlags = 76, kDCLFGrassBucketScaleMask = 80, kDCLFGrassBucketPbrTextures = 96, kDCLFGrassBucketPbrParams1 = 112,
				  kDCLFGrassBucketPbrParams2 = 128, kDCLFGrassBucketWindDirection = 144, kDCLFGrassBucketPbrParams3 = 160, kDCLFGrassBucketPbrParams4 = 176, kDCLFGrassBucketExtraTextures = 192;
static const uint kDCLFGrassBucketTruePbr = 1u << 0, kDCLFGrassBucketSphereNormal = 1u << 1;
// GrassInstances::InstanceFrame (8 bytes): windCur and windPrev as halfs; the fade (unorm16) and, in the high 16 bits, the flags (bits 0-1 the
// tier, 2 in collision range, 3 simple shading). The vertex stage takes it unpacked (DCLFGrassFrame: windCur, windPrev, fade as floats, flags).
static const uint kDCLFGrassFrameStride = 8, kDCLFGrassTierMask = 3u, kDCLFGrassInCollisionRange = 1u << 2, kDCLFGrassSimpleShading = 1u << 3;
static const uint kDCLFGrassNone = 0xFFFFFFFFu;
#endif

#include "Common/ColorManagement.hlsli"
#include "Common/FrameBuffer.hlsli"
#include "Common/GBuffer.hlsli"
#include "Common/GrassInstance.hlsli"
#include "Common/Math.hlsli"
#include "Common/MotionBlur.hlsli"
#include "Common/Permutation.hlsli"
#include "Common/Random.hlsli"
#include "Common/Shading.hlsli"
#include "Common/SharedData.hlsli"

#define DEFERRED

#if defined(GRASS_OPTIMIZATIONS) || defined(DCLF_PULLED)
// The instances carry a LOD tier and a far (simple shading) flag, and no WorldView: culled by Grass Optimizations' or by Drawcall
// Limit Fix's compute.
#	define GRASS_CULLED_INSTANCES
#endif

#ifdef GRASS_LIGHTING
#	define GRASS
#endif  // GRASS_LIGHTING

#if !defined(DYNAMIC_CUBEMAPS) && defined(IBL)
#	undef IBL
#endif

#if defined(VSHADER)
struct VS_INPUT
{
	float4 Position: POSITION0;
	float2 TexCoord: TEXCOORD0;
	float4 Normal: NORMAL0;
	float4 Color: COLOR0;
	float4 InstanceData1: TEXCOORD4;
	float4 InstanceData2: TEXCOORD5;
	float4 InstanceData3: TEXCOORD6;
	float4 InstanceData4: TEXCOORD7;
};
#endif

// Both draw paths share the engine vertex layout and interpolate only the outputs needed by this pass.
struct VS_OUTPUT
{
	float4 HPosition: SV_POSITION0;
	float2 TexCoord: TEXCOORD0;
#if defined(RENDER_DEPTH)
	float Fade: TEXCOORD2;
#	ifndef GRASS_OPTIMIZATIONS
	float2 Depth: TEXCOORD4;
#	endif
#else
	// Fade shares the otherwise unused color alpha component.
	float4 Color: COLOR0;
	float3 WorldPosition: POSITION1;
	float3 PreviousWorldPosition: POSITION2;
#	ifdef GRASS_LIGHTING
	float4 VertexNormal: POSITION4;
	// Unnormalized direction around the plant, for rounding clump shading.
	float3 ClumpNormal: POSITION5;
#	else
	float DirLightAngle: TEXCOORD1;
#		ifndef GRASS_CULLED_INSTANCES
	// Preserve the engine's WorldView transform for standard-path geometric normals.
	float3 ViewSpacePosition: TEXCOORD3;
#		endif
#	endif
#endif
#ifdef GRASS_OPTIMIZATIONS
	nointerpolation float IsComplex: TEXCOORD8;
#endif
#if defined(GRASS_CULLED_INSTANCES) && !defined(RENDER_DEPTH)
	nointerpolation float IsFar: TEXCOORD9;
	// 0 = full mesh, 1 = middle LOD mesh, 2 = far LOD mesh.
	nointerpolation float LodTier: TEXCOORD10;
#endif
#if defined(DCLF_PULLED)
	// The instance's bucket, whose row the pixel stages read their textures and samplers from.
	nointerpolation uint Bucket: TEXCOORD11;
#endif
};

#if defined(DCLF_PULLED)
// PerGeometry's values that DCLF's draws read (none of its transforms: an instance's position is its shape's origin plus its own),
// from the draw row (one for the frame: what SetupGeometry writes per pass) and ScaleMask and WindVector.xy (the shapes' rotation)
// from the bucket's row, set by DCLFGrassStatics first in both stages' main. [UNVERIFIED] which of SetupGeometry's b2 values are per
// frame (GrassLayout.h, DrawRow): read as laid out. WindTimer and PreviousWindTimer are what the cull's InstanceFrame wind was made from (the vertex
// stage reads that, not them).
static float4 FogNearColor;  // only w: the row's dirLightColor.w
static float3 WindVector;
static float WindTimer;
static float3 DirLightDirection;
static float PreviousWindTimer;
static float3 DirLightColor;
static float AlphaParam1;
static float3 AmbientColor;
static float AlphaParam2;
static float3 ScaleMask;
static float ShadowClampValue;

// The bucket's row (GrassInstances::BucketRow), set by DCLFGrassStatics.
static uint64_t DCLFGrassBucketRow;

void DCLFGrassStatics(uint a_bucket)
{
	DCLFGrassBucketRow = DCLFGrassTable(kDCLFGrassBuckets) + uint64_t(a_bucket) * kDCLFGrassBucketStride;
	WindVector = float3(asfloat(vk::RawBufferLoad<uint2>(DCLFGrassBucketRow + kDCLFGrassBucketWindDirection, 4)), DCLFGrassDrawFloat4(kDCLFGrassWindVector).z);
	const float2 timeBase = asfloat(vk::RawBufferLoad<uint2>(DCLFGrassDraw() + kDCLFGrassTimeBase, 4));  // timeBase, prevTimeBase
	const float wavePeriod = asfloat(vk::RawBufferLoad<uint>(DCLFGrassBucketRow + kDCLFGrassBucketWavePeriod, 4));
	WindTimer = timeBase.x * wavePeriod;
	PreviousWindTimer = timeBase.y * wavePeriod;
	const float4 dirLightDirection = DCLFGrassDrawFloat4(kDCLFGrassDirLightDirection);  // w ShadowClampValue
	DirLightDirection = dirLightDirection.xyz;
	ShadowClampValue = dirLightDirection.w;
	const float4 dirLightColor = DCLFGrassDrawFloat4(kDCLFGrassDirLightColor);  // w FogNearColor.w
	DirLightColor = dirLightColor.xyz;
	FogNearColor = float4(0, 0, 0, dirLightColor.w);
	AmbientColor = DCLFGrassDrawFloat4(kDCLFGrassAmbientColor).xyz;
	const float2 alphaParams = asfloat(vk::RawBufferLoad<uint2>(DCLFGrassDraw() + kDCLFGrassAlphaParams, 4));
	AlphaParam1 = alphaParams.x;
	AlphaParam2 = alphaParams.y;
	ScaleMask = asfloat(vk::RawBufferLoad<uint3>(DCLFGrassBucketRow + kDCLFGrassBucketScaleMask, 4));
	// What CS's SetupGeometry hook puts in the permutation block for a sphere-normal property (Hooks.cpp, GrassSphereNormal).
	const uint flags = vk::RawBufferLoad<uint>(DCLFGrassBucketRow + kDCLFGrassBucketFlags, 4);
	Permutation::ExtraShaderDescriptor = (flags & kDCLFGrassBucketSphereNormal) ? Permutation::ExtraFlags::GrassSphereNormal : 0;
}
#else
cbuffer PerGeometry : register(b2)
{
	row_major float4x4 WorldViewProj : packoffset(c0);
	row_major float4x4 WorldView : packoffset(c4);
	row_major float4x4 World : packoffset(c8);
	row_major float4x4 PreviousWorld : packoffset(c12);
	float4 FogNearColor : packoffset(c16);
	float3 WindVector : packoffset(c17);
	float WindTimer : packoffset(c17.w);
	float3 DirLightDirection : packoffset(c18);
	float PreviousWindTimer : packoffset(c18.w);
	float3 DirLightColor : packoffset(c19);
	float AlphaParam1 : packoffset(c19.w);
	float3 AmbientColor : packoffset(c20);
	float AlphaParam2 : packoffset(c20.w);
	float3 ScaleMask : packoffset(c21);
	float ShadowClampValue : packoffset(c21.w);
}
#endif  // DCLF_PULLED

#ifdef VSHADER

#	ifdef GRASS_COLLISION
#		if defined(DCLF_PULLED)
// Grass Collision's texture (GRASS_COLLISION_EXTERNAL): its latest published one, in the descriptor heap at the draw row's index.
Texture2D<float4> GrassCollisionTexture()
{
	Texture2D<float4> t = ResourceDescriptorHeap[DCLFGrassDrawUint(kDCLFGrassCollisionIndex)];
	return t;
}
#		endif
#		include "GrassCollision\\GrassCollision.hlsli"
#		if defined(DCLF_PULLED)
// Grass Collision's GrassCollisionPerFrame block (b5), copied into the draw row at kDCLFGrassCollisionPerFrame in its layout.
void DCLFGrassCollisionStatics()
{
	const uint64_t block = DCLFGrassDraw() + kDCLFGrassCollisionPerFrame;
	const uint4 first = vk::RawBufferLoad<uint4>(block, 4);
	const uint4 second = vk::RawBufferLoad<uint4>(block + 16, 4);
	GrassCollision::PosOffset = asfloat(first.xy);
	GrassCollision::ArrayOrigin = first.zw;
	GrassCollision::ValidMargin = asint(second.xy);
	GrassCollision::TimeDelta = asfloat(second.z);
	GrassCollision::BoundingBoxCount = second.w;
	GrassCollision::CameraHeightDelta = asfloat(vk::RawBufferLoad<uint>(block + 32, 4));
}
#		endif
#	endif  // GRASS_COLLISION

#	ifdef GRASS_OPTIMIZATIONS
// One GrassInstance record per drawn instance, written by GrassCullingCS.
ByteAddressBuffer InstanceRecords : register(t2);
// The bucket's captured instances packed by GrassInstance.hlsli, and its cull groups for the cell origins.
ByteAddressBuffer GrassInstances : register(t3);
StructuredBuffer<GrassInstance::CullGroup> GrassGroups : register(t4);
// Classify complex grass per instance using the cached texture normal length.
StructuredBuffer<float> GrassTextureNormalLength : register(t5);

// Match the draw marker, output offset and texture threshold in CullBucketCB.
cbuffer GrassBucket : register(b13)
{
	uint GrassBucketGroupCount : packoffset(c0.x);
	uint GrassOutputOffset : packoffset(c1.y);
	float GrassComplexThreshold : packoffset(c3.z);
	float3 GrassCanopyHeights : packoffset(c5);
};
#	endif

#	if !defined(DCLF_PULLED)
// The engine's per-instance fade, bound by vanilla grass setup.
cbuffer cb7 : register(b7)
{
	float4 cb7[1];
}

cbuffer cb8 : register(b8)
{
	float4 cb8[240];
}
#	endif  // !DCLF_PULLED

// Calculate wind displacement for a grass vertex.
float3 CalculateWindDisplacement(VS_INPUT input, float windTimer)
{
	float windPower = WindVector.z * (GrassInstance::WindScalar(input.InstanceData1.xy, windTimer) *
										 (0.5 * (input.Color.w * input.Color.w)));

	return float3(WindVector.xy, 0) * windPower;
}

#	ifdef GRASS_LIGHTING
// Grass meshes are rooted at the model origin.
float GetCanopyHeight(VS_INPUT input, float canopyHeight)
{
	return saturate(input.Position.z / max(canopyHeight, 1.0));
}

// A pivot below the root gives the clump a dome-shaped normal field.
float3 GetClumpNormal(VS_INPUT input, float3x3 world3x3, float canopyHeight)
{
	return mul(world3x3, input.Position.xyz + float3(0, 0, 0.5 * max(canopyHeight, 1.0)));
}

float4 GetMSPosition(VS_INPUT input, float3x3 world3x3)
#	else
float4 GetMSPosition(VS_INPUT input)
#	endif
{
	float3 inputPosition = input.Position.xyz * (input.InstanceData4.yyy * ScaleMask.xyz + float3(1, 1, 1));

#	ifdef GRASS_LIGHTING
	float3 transformedPosition = mul(world3x3, inputPosition);
	float4 msPosition;
	msPosition.xyz = input.InstanceData1.xyz + transformedPosition;
#	else
	float3 instancePosition;
	instancePosition.z = dot(
		float3(input.InstanceData4.x, input.InstanceData2.w, input.InstanceData3.w), inputPosition);
	instancePosition.x = dot(input.InstanceData2.xyz, inputPosition);
	instancePosition.y = dot(input.InstanceData3.xyz, inputPosition);

	float4 msPosition;
	msPosition.xyz = input.InstanceData1.xyz + instancePosition;
#	endif
	msPosition.w = 1;

	return msPosition;
}

#	if !defined(DCLF_PULLED)
// Engine draws use the instance stream and PerGeometry transforms, including grass not yet resident in a bucket.
VS_OUTPUT VanillaVertex(VS_INPUT input)
{
	VS_OUTPUT vsout = (VS_OUTPUT)0;

#		ifdef GRASS_LIGHTING
	float3x3 world3x3 = float3x3(input.InstanceData2.xyz, input.InstanceData3.xyz, float3(input.InstanceData4.x, input.InstanceData2.w, input.InstanceData3.w));
	float4 msPosition = GetMSPosition(input, world3x3);
#		else
	float4 msPosition = GetMSPosition(input);
#		endif

#		if !defined(RENDER_DEPTH)
	// Save the undisplaced position instead of repeating the instance transform.
	float4 previousMsPosition = msPosition;
#		endif

#		ifdef GRASS_COLLISION
	float3 displacement, previousDisplacement;
#			ifdef GRASS_OPTIMIZATIONS
	GrassCollision::GetDisplacedPosition(input, mul(World, msPosition).xyz, mul(World, float4(input.InstanceData1.xyz, 1.0)).xyz, displacement, previousDisplacement);
#			else
	GrassCollision::GetDisplacedPosition(input, msPosition.xyz, displacement, previousDisplacement);
#			endif
	msPosition.xyz += displacement;
#		endif  // GRASS_COLLISION

	msPosition.xyz += CalculateWindDisplacement(input, WindTimer);

	float4 projSpacePosition = mul(WorldViewProj, msPosition);
	vsout.HPosition = projSpacePosition;
	vsout.TexCoord = input.TexCoord.xy;

#		ifdef GRASS_OPTIMIZATIONS
	// The optimized hook bypasses the engine fade-buffer upload.
	float perInstanceFade = 1.0;
#		else
	float perInstanceFade = dot(cb8[(asuint(cb7[0].x) >> 2)].xyzw, Math::IdentityMatrix[(asint(cb7[0].x) & 3)].xyzw);
#		endif
	float distanceFade = 1 - saturate((length(projSpacePosition.xyz) - AlphaParam1) / AlphaParam2);

#		if defined(RENDER_DEPTH)
#			ifndef GRASS_OPTIMIZATIONS
	vsout.Depth = projSpacePosition.zw;
#			endif
	vsout.Fade = distanceFade * perInstanceFade;
#		else
	vsout.Color = float4(input.InstanceData1.www * input.Color.xyz, distanceFade * perInstanceFade);
	vsout.WorldPosition = mul(World, msPosition).xyz;

#			ifdef GRASS_COLLISION
	previousMsPosition.xyz += previousDisplacement;
#			endif  // GRASS_COLLISION
	previousMsPosition.xyz += CalculateWindDisplacement(input, PreviousWindTimer);
	vsout.PreviousWorldPosition = mul(PreviousWorld, previousMsPosition).xyz;

#			ifdef GRASS_LIGHTING
	// Vertex normal needs to be transformed to world-space for lighting calculations.
	vsout.VertexNormal.xyz = mul(world3x3, input.Normal.xyz * 2.0 - 1.0);
	vsout.VertexNormal.w = GetCanopyHeight(input, Permutation::GrassCanopyHeight);
	vsout.ClumpNormal = GetClumpNormal(input, world3x3, Permutation::GrassCanopyHeight);
#			else
	float3 instanceNormal = float3(input.InstanceData2.z, input.InstanceData3.zw);
	vsout.DirLightAngle = saturate(dot(DirLightDirection.xyz, instanceNormal));
#				ifndef GRASS_OPTIMIZATIONS
	vsout.ViewSpacePosition = mul(WorldView, msPosition).xyz;
#				endif
#			endif
#		endif

#		ifdef GRASS_OPTIMIZATIONS
	// Unknown complexity makes the pixel shader test the texture, as the engine path does.
	vsout.IsComplex = -1.0;
#		endif
	return vsout;
}
#	endif  // !DCLF_PULLED

#	if defined(DCLF_PULLED)
// GO's instance path (below) for one of DCLF's instances: a_origin its shape's (Grass Optimizations' e0.xyz), a_frame the cull's
// InstanceFrame (e1: windCur, windPrev, fade, flags). Precise throughout: the Z-prepass (RENDER_DEPTH) and the colour builds (base and
// TruePBR) each compute the position, and the colour pass tests EQUAL against the Z-prepass's depth; precise does not reach into a
// called function, so the instance transform is written out here and Grass Collision's functions are precise themselves
// (GRASS_COLLISION_EXTERNAL).
VS_OUTPUT DCLFGrassVertex(VS_INPUT input, float3 a_origin, uint4 a_frame, uint a_bucket)
{
	VS_OUTPUT vsout = (VS_OUTPUT)0;
	vsout.TexCoord = input.TexCoord.xy;
	vsout.Bucket = a_bucket;
	const float windCur = asfloat(a_frame.x), windPrev = asfloat(a_frame.y), fade = asfloat(a_frame.z);

	// GetMSPosition's instance transform (both of its forms are these three products).
	precise float3 inputPosition = input.Position.xyz * (input.InstanceData4.yyy * ScaleMask.xyz + float3(1, 1, 1));
	precise float3 instancePosition;
	instancePosition.x = dot(input.InstanceData2.xyz, inputPosition);
	instancePosition.y = dot(input.InstanceData3.xyz, inputPosition);
	instancePosition.z = dot(float3(input.InstanceData4.x, input.InstanceData2.w, input.InstanceData3.w), inputPosition);
	precise float3 msPosition = (input.InstanceData1.xyz + instancePosition) + a_origin;

#		if !defined(RENDER_DEPTH)
	precise float3 previousMsPosition = msPosition;
#		endif

#		ifdef GRASS_COLLISION
	[branch] if ((a_frame.w & kDCLFGrassInCollisionRange) != 0 && DCLFGrassDrawUint(kDCLFGrassCollisionIndex) != kDCLFGrassNone)
	{
		DCLFGrassCollisionStatics();
		precise float3 collisionPos = msPosition - FrameBuffer::CameraPosAdjust.xyz;
		precise float3 collisionCentre = input.InstanceData1.xyz + a_origin - FrameBuffer::CameraPosAdjust.xyz;
		precise float3 displacement, previousDisplacement;
		GrassCollision::GetDisplacedPosition(input, collisionPos, collisionCentre, displacement, previousDisplacement);
		msPosition += displacement;
#			if !defined(RENDER_DEPTH)
		previousMsPosition += previousDisplacement;
#			endif
	}
#		endif

	precise float vertexTerm = WindVector.z * (0.5 * (input.Color.w * input.Color.w));
	msPosition += float3(WindVector.xy, 0) * (windCur * vertexTerm);
	precise float3 eyeRel = msPosition - FrameBuffer::CameraPosAdjust.xyz;
	precise float4 projSpacePosition = mul(FrameBuffer::CameraViewProj, float4(eyeRel, 1.0));
	vsout.HPosition = projSpacePosition;

#		if defined(RENDER_DEPTH)
	vsout.Depth = projSpacePosition.zw;
	vsout.Fade = fade;
#		else
	previousMsPosition += float3(WindVector.xy, 0) * (windPrev * vertexTerm);
	vsout.Color = float4(input.InstanceData1.www * input.Color.xyz, fade);
#			ifndef GRASS_LIGHTING
	float3 instanceNormal = float3(input.InstanceData2.z, input.InstanceData3.zw);
	vsout.DirLightAngle = saturate(dot(DirLightDirection.xyz, instanceNormal));
#			endif
	vsout.WorldPosition = eyeRel;
	vsout.PreviousWorldPosition = previousMsPosition - FrameBuffer::CameraPreviousPosAdjust.xyz;
	vsout.IsFar = (a_frame.w & kDCLFGrassSimpleShading) != 0 ? 1.0 : 0.0;
	vsout.LodTier = float(a_frame.w & kDCLFGrassTierMask);
#			ifdef GRASS_LIGHTING
	float3x3 world3x3 = float3x3(input.InstanceData2.xyz, input.InstanceData3.xyz, float3(input.InstanceData4.x, input.InstanceData2.w, input.InstanceData3.w));
	vsout.VertexNormal.xyz = mul(world3x3, input.Normal.xyz * 2.0 - 1.0);
	vsout.VertexNormal.w = input.Color.w;
#			endif
#		endif

	return vsout;
}

// The draw's instance a_instance (SV_InstanceID counts from the draw's firstInstance: its range of the visible list) is the visible
// list's entry, an instance slot; its owner is its shape. The vertex comes from the bucket's mesh for the instance's tier: R16
// indices, attributes as the engine's input layout for the mesh's vertex layout has them (one stream).
VS_OUTPUT main(uint a_vertex : SV_VertexID, uint a_instance : SV_InstanceID)
{
	const uint index = vk::RawBufferLoad<uint>(DCLFGrassTable(kDCLFGrassVisible) + 4 * uint64_t(a_instance), 4);
	const uint shapeSlot = vk::RawBufferLoad<uint>(DCLFGrassTable(kDCLFGrassOwners) + 4 * uint64_t(index), 4);
	const uint64_t shape = DCLFGrassTable(kDCLFGrassShapes) + 32 * uint64_t(shapeSlot);
	const float3 origin = asfloat(vk::RawBufferLoad<uint3>(shape, 4));
	const uint bucket = vk::RawBufferLoad<uint>(shape + 12, 4);
	DCLFGrassStatics(bucket);
	const uint2 packedFrame = vk::RawBufferLoad<uint2>(DCLFGrassTable(kDCLFGrassFrame) + kDCLFGrassFrameStride * uint64_t(index), 8);
	const uint4 frame = uint4(asuint(f16tof32(packedFrame.x)), asuint(f16tof32(packedFrame.x >> 16)), asuint(float(packedFrame.y & 0xFFFFu) / 65535.0), packedFrame.y >> 16);
	const uint meshSlot = vk::RawBufferLoad<uint>(DCLFGrassBucketRow + 4 * (frame.w & kDCLFGrassTierMask), 4);
	if (meshSlot == kDCLFGrassNone)
		return (VS_OUTPUT)0;
	// GrassInstances::MeshRow: vertex address, index address, vertex layout, index count, vertex stride.
	const uint64_t mesh = DCLFGrassTable(kDCLFGrassMeshes) + 32 * uint64_t(meshSlot);
	const uint4 meshAddresses = vk::RawBufferLoad<uint4>(mesh, 8);
	DCLFDrawVertexLayout = vk::RawBufferLoad<uint2>(mesh + 16, 8);
	const uint indexWord = vk::RawBufferLoad<uint>(DCLFAddress(meshAddresses.zw) + 4 * uint64_t(a_vertex / 2), 4);
	const uint vertexIndex = (a_vertex & 1) ? (indexWord >> 16) : (indexWord & 0xFFFF);
	DCLFStream first, second;
	first.address = DCLFAddress(meshAddresses.xy);
	first.stride = vk::RawBufferLoad<uint>(mesh + 28, 4);
	second.address = 0;
	second.stride = 0;
	DCLFStreamsAt(vertexIndex, first, second);

	VS_INPUT input;
	input.Position = DCLFPullFloat4(0, first, second);
	input.TexCoord = DCLFPullHalf2(1, first, second);
	input.Normal = DCLFPullUnorm4(3, 0, first, second);
	input.Color = DCLFPullUnorm4(5, 0, first, second);
	// The engine's instance record (TEXCOORD4-7, R16G16B16A16_FLOAT each).
	const uint64_t record = DCLFGrassTable(kDCLFGrassInstances) + 32 * uint64_t(index);
	const uint4 low = vk::RawBufferLoad<uint4>(record, 16);
	const uint4 high = vk::RawBufferLoad<uint4>(record + 16, 16);
	input.InstanceData1 = float4(DCLFHalf2(low.x), DCLFHalf2(low.y));
	input.InstanceData2 = float4(DCLFHalf2(low.z), DCLFHalf2(low.w));
	input.InstanceData3 = float4(DCLFHalf2(high.x), DCLFHalf2(high.y));
	input.InstanceData4 = float4(DCLFHalf2(high.z), DCLFHalf2(high.w));
	return DCLFGrassVertex(input, origin, frame, bucket);
}
#	elif defined(GRASS_OPTIMIZATIONS)
// Captured instances retain upstream's vertex input layout.
VS_OUTPUT main(VS_INPUT input, uint instanceID : SV_InstanceID)
{
	// GrassOptimizations binds this marker in place of a bucket slot when it draws through the engine's path.
	[branch] if (GrassBucketGroupCount == GrassInstance::VanillaDrawMarker) return VanillaVertex(input);

	VS_OUTPUT vsout = (VS_OUTPUT)0;

	const uint4 record = InstanceRecords.Load4((GrassOutputOffset + instanceID) * GrassInstance::RecordStride);
	// Decode packed source records into the engine instance layout.
	GrassInstance::Decode(GrassInstances.Load4(record.x * GrassInstance::Stride), input.InstanceData1, input.InstanceData2, input.InstanceData3, input.InstanceData4);

	uint groupIndex, flags;
	float fade;
	GrassInstance::UnpackRecord(record.y, groupIndex, fade, flags);
	const float2 wind = asfloat(record.zw);
	const float3 origin = GrassGroups[groupIndex].Origin;
	const float normalLength = GrassTextureNormalLength[0];
	vsout.IsComplex = normalLength > 0.0 ? (float)(abs(normalLength - 1.0) < GrassComplexThreshold) : -1.0;
	vsout.TexCoord = input.TexCoord.xy;

#		ifdef GRASS_LIGHTING
	float3x3 world3x3 = float3x3(input.InstanceData2.xyz, input.InstanceData3.xyz, float3(input.InstanceData4.x, input.InstanceData2.w, input.InstanceData3.w));
	float4 msPosition = GetMSPosition(input, world3x3);
#		else
	float4 msPosition = GetMSPosition(input);
#		endif
	msPosition.xyz += origin;

#		if !defined(RENDER_DEPTH)
	float4 previousMsPosition = msPosition;
#		endif

#		ifdef GRASS_COLLISION
	[branch] if (flags & GrassInstance::FlagCollision)
	{
		// Captured instances already include the cell origin; do not apply World a second time.
		const float3 collisionPos = msPosition.xyz - FrameBuffer::CameraPosAdjust.xyz;
		const float3 collisionCentre = input.InstanceData1.xyz + origin - FrameBuffer::CameraPosAdjust.xyz;
		float3 displacement, previousDisplacement;
		GrassCollision::GetDisplacedPosition(input, collisionPos, collisionCentre, displacement, previousDisplacement);
		msPosition.xyz += displacement;
#			if !defined(RENDER_DEPTH)
		previousMsPosition.xyz += previousDisplacement;
#			endif
	}
#		endif

	const float vertexTerm = WindVector.z * (0.5 * (input.Color.w * input.Color.w));
	msPosition.xyz += float3(WindVector.xy, 0) * (wind.x * vertexTerm);
	const float3 eyeRel = msPosition.xyz - FrameBuffer::CameraPosAdjust.xyz;
	const float4 projSpacePosition = mul(FrameBuffer::CameraViewProj, float4(eyeRel, 1.0));
	vsout.HPosition = projSpacePosition;

#		if defined(RENDER_DEPTH)
	vsout.Fade = fade;
#		else
	previousMsPosition.xyz += float3(WindVector.xy, 0) * (wind.y * vertexTerm);
	vsout.Color = float4(input.InstanceData1.www * input.Color.xyz, fade);
#			ifndef GRASS_LIGHTING
	float3 instanceNormal = float3(input.InstanceData2.z, input.InstanceData3.zw);
	vsout.DirLightAngle = saturate(dot(DirLightDirection.xyz, instanceNormal));
#			endif
	vsout.WorldPosition = eyeRel;
	vsout.PreviousWorldPosition = previousMsPosition.xyz - FrameBuffer::CameraPreviousPosAdjust.xyz;
	vsout.IsFar = (flags & GrassInstance::FlagSimpleShading) ? 1.0 : 0.0;
	vsout.LodTier = (float)(flags >> GrassInstance::FlagTierShift);
#			ifdef GRASS_LIGHTING
	vsout.VertexNormal.xyz = mul(world3x3, input.Normal.xyz * 2.0 - 1.0);
	const float canopyHeight = GrassCanopyHeights[flags >> GrassInstance::FlagTierShift];
	vsout.VertexNormal.w = GetCanopyHeight(input, canopyHeight);
	vsout.ClumpNormal = GetClumpNormal(input, world3x3, canopyHeight);
#			endif
#		endif

	return vsout;
}
#	else  // GRASS_OPTIMIZATIONS

VS_OUTPUT main(VS_INPUT input)
{
	return VanillaVertex(input);
}

#	endif  // GRASS_OPTIMIZATIONS

#endif  // VSHADER

typedef VS_OUTPUT PS_INPUT;

#ifdef GRASS_LIGHTING
struct PS_OUTPUT
{
#	if defined(RENDER_DEPTH)
	float4 PS: SV_Target0;
#	else
	float4 Diffuse: SV_Target0;
	float2 MotionVectors: SV_Target1;
	float4 NormalGlossiness: SV_Target2;
	float4 Albedo: SV_Target3;
	float4 Specular: SV_Target4;
	float4 Reflectance: SV_Target5;
	float4 Masks: SV_Target6;
	float4 Masks2: SV_Target7;
#	endif  // RENDER_DEPTH
};
#else
struct PS_OUTPUT
{
#	if defined(RENDER_DEPTH)
	float4 PS: SV_Target0;
#	else
	float4 Diffuse: SV_Target0;
	float2 MotionVectors: SV_Target1;
	float4 Normal: SV_Target2;
	float4 Albedo: SV_Target3;
	float4 Masks: SV_Target6;
	float4 Masks2: SV_Target7;
#	endif
};
#endif

#ifdef PSHADER
SamplerState SampShadowMaskSampler : register(s1);
#	if defined(PHYSICAL_SKY)
SamplerState SampPhysicalSkyWrapSampler : register(s7);
#	endif
#	if defined(DCLF_PULLED)
// The pass's textures and samplers by descriptor heap index: the bucket's (its row's, read by DCLFGrassPixelStatics), the frame's
// shadow mask (the draw row's). A multi-draw's waves may mix buckets, so the indices are non-uniform. PerFrame (b0) is not read.
static uint DCLFGrassDiffuse, DCLFGrassSampler, DCLFGrassShadowMask;
static float AlphaTestRefRS;
Texture2D<float4> DCLFGrassTextureAt(uint a_index)
{
	Texture2D<float4> t = ResourceDescriptorHeap[NonUniformResourceIndex(a_index)];
	return t;
}
SamplerState DCLFGrassSamplerAt(uint a_index)
{
	SamplerState s = SamplerDescriptorHeap[NonUniformResourceIndex(a_index)];
	return s;
}
#		define SampBaseSampler DCLFGrassSamplerAt(DCLFGrassSampler)
#		define TexBaseSampler DCLFGrassTextureAt(DCLFGrassDiffuse)
#		define TexShadowMaskSampler DCLFGrassTextureAt(DCLFGrassShadowMask)
#		if defined(GRASS_LIGHTING) && defined(TRUE_PBR)
// PerMaterial (b1) from the bucket's row: TruePBR's material, which GrassShared reads at capture.
static uint PBRFlags;
static float3 PBRParams1;  // roughness scale, specular level
static float4 PBRParams3, PBRParams4;
static uint DCLFGrassFuzz, DCLFGrassEmissive;
#			define SampFuzzSampler DCLFGrassSamplerAt(DCLFGrassPbrSampler)
#			define SampEmissiveSampler DCLFGrassSamplerAt(DCLFGrassPbrSampler)
#			define TexFuzzSampler DCLFGrassTextureAt(DCLFGrassFuzz)
#			define TexEmissiveSampler DCLFGrassTextureAt(DCLFGrassEmissive)
static float4 PBRParams2;  // subsurface color, subsurface opacity
static uint DCLFGrassNormal, DCLFGrassRmaos, DCLFGrassFeatures, DCLFGrassPbrSampler;
#			define SampNormalSampler DCLFGrassSamplerAt(DCLFGrassPbrSampler)
#			define SampRMAOSSampler DCLFGrassSamplerAt(DCLFGrassPbrSampler)
#			define SampSubsurfaceSampler DCLFGrassSamplerAt(DCLFGrassPbrSampler)
#			define TexNormalSampler DCLFGrassTextureAt(DCLFGrassNormal)
#			define TexRMAOSSampler DCLFGrassTextureAt(DCLFGrassRmaos)
#			define TexSubsurfaceSampler DCLFGrassTextureAt(DCLFGrassFeatures)
#		endif

// The pixel stage's statics (DCLFGrassStatics, then the bucket's textures, samplers and alpha reference), first in its main.
void DCLFGrassPixelStatics(uint a_bucket)
{
	DCLFGrassStatics(a_bucket);
	const uint4 textures = vk::RawBufferLoad<uint4>(DCLFGrassBucketRow + kDCLFGrassBucketTextures, 4);  // diffuse, samplers, alpha ref
	DCLFGrassDiffuse = textures.x;
#		if defined(RENDER_DEPTH)
	DCLFGrassSampler = textures.y;
#		else
	DCLFGrassSampler = textures.z;
#		endif
	// [UNVERIFIED] the engine's grass alpha reference (b11): the bucket's, 0.5 until stage 0 logs it.
	AlphaTestRefRS = asfloat(textures.w);
	DCLFGrassShadowMask = DCLFGrassDrawUint(kDCLFGrassShadowMaskIndex);
#		if defined(GRASS_LIGHTING) && defined(TRUE_PBR)
	PBRFlags = vk::RawBufferLoad<uint>(DCLFGrassBucketRow + kDCLFGrassBucketPbrFlags, 4);
	const uint4 pbrTextures = vk::RawBufferLoad<uint4>(DCLFGrassBucketRow + kDCLFGrassBucketPbrTextures, 4);
	DCLFGrassNormal = pbrTextures.x;
	DCLFGrassRmaos = pbrTextures.y;
	DCLFGrassFeatures = pbrTextures.z;
	DCLFGrassPbrSampler = pbrTextures.w;
	PBRParams1 = asfloat(vk::RawBufferLoad<uint3>(DCLFGrassBucketRow + kDCLFGrassBucketPbrParams1, 4));
	PBRParams2 = asfloat(vk::RawBufferLoad<uint4>(DCLFGrassBucketRow + kDCLFGrassBucketPbrParams2, 4));
	PBRParams3 = asfloat(vk::RawBufferLoad<uint4>(DCLFGrassBucketRow + kDCLFGrassBucketPbrParams3, 4));
	PBRParams4 = asfloat(vk::RawBufferLoad<uint4>(DCLFGrassBucketRow + kDCLFGrassBucketPbrParams4, 4));
	const uint2 extraTextures = vk::RawBufferLoad<uint2>(DCLFGrassBucketRow + kDCLFGrassBucketExtraTextures, 4);
	DCLFGrassFuzz = extraTextures.x;
	DCLFGrassEmissive = extraTextures.y;
#		endif
}
#	else
SamplerState SampBaseSampler : register(s0);
#		if defined(GRASS_LIGHTING) && defined(TRUE_PBR)
SamplerState SampNormalSampler : register(s2);
SamplerState SampRMAOSSampler : register(s3);
SamplerState SampSubsurfaceSampler : register(s4);
SamplerState SampFuzzSampler : register(s5);
SamplerState SampEmissiveSampler : register(s6);
#		endif

Texture2D<float4> TexBaseSampler : register(t0);
Texture2D<float4> TexShadowMaskSampler : register(t1);
#		if defined(GRASS_LIGHTING) && defined(TRUE_PBR)
Texture2D<float4> TexNormalSampler : register(t2);
Texture2D<float4> TexRMAOSSampler : register(t3);
// Texture-set slot 8 (featuresTexture0): sRGB subsurface color in RGB, linear opacity in alpha.
Texture2D<float4> TexSubsurfaceSampler : register(t4);
// Texture-set slot 7 (featuresTexture1): sRGB fuzz color in RGB, linear weight in alpha.
Texture2D<float4> TexFuzzSampler : register(t5);
// Texture-set slot 3: linear emissive color in RGB.
Texture2D<float4> TexEmissiveSampler : register(t6);
#		endif

cbuffer PerFrame : register(b0)
{
	float4 cb0_1[2] : packoffset(c0);
	float4 VPOSOffset : packoffset(c2);
	float4 cb0_2[7] : packoffset(c3);
}

cbuffer AlphaTestRefCB : register(b11)
{
	float AlphaTestRefRS : packoffset(c0);
}
#	endif  // DCLF_PULLED

#	if defined(SCREEN_SPACE_SHADOWS)
#		include "ScreenSpaceShadows/ScreenSpaceShadows.hlsli"
#	endif

#	if defined(LIGHT_LIMIT_FIX)
#		include "LightLimitFix/LightLimitFix.hlsli"
#	endif

#	define SampColorSampler SampBaseSampler

#	if defined(SKYLIGHTING)
#		define SKYLIGHTING_SHADOW_VIS
#		include "Skylighting/Skylighting.hlsli"
#	endif

#	if defined(DYNAMIC_CUBEMAPS)
#		include "DynamicCubemaps/DynamicCubemaps.hlsli"
#	endif

#	if defined(IBL)
#		include "IBL/IBL.hlsli"
#	endif

#	if defined(EXP_HEIGHT_FOG)
#		include "ExponentialHeightFog/ExponentialHeightFog.hlsli"
#	endif

#	if defined(PHYSICAL_SKY)
#		include "PhysicalSky/Common.hlsli"
#	endif

#	if defined(PSEUDO_SUN_BOUNCE)
#		include "PseudoSunBounce/sunbounce.hlsli"
#	endif

#	if defined(WATER_EFFECTS)
#		include "WaterEffects/WaterCaustics.hlsli"
#	endif

#	define LinearSampler SampBaseSampler

#	include "Common/ShadowSampling.hlsli"
#	ifdef GRASS_LIGHTING
#		if defined(TRUE_PBR)
#			if !defined(DCLF_PULLED)
cbuffer PerMaterial : register(b1)
{
	uint PBRFlags : packoffset(c0.x);
	float3 PBRParams1 : packoffset(c0.y);  // roughness scale, specular level, emissive scale
	float4 PBRParams2 : packoffset(c1);    // subsurface color, subsurface opacity
	float4 PBRParams3 : packoffset(c2);    // fuzz color, fuzz weight
	float4 PBRParams4 : packoffset(c3);    // subsurface balance, front/back specular weights, back roughness scale
};
#			endif

#		endif
#		include "Common/LightingEval.hlsli"
#		include "GrassLighting/GrassLighting.hlsli"

#		if defined(WETNESS_EFFECTS)
#			include "WetnessEffects/WetnessEffects.hlsli"
#		endif

#		if defined(TRUE_PBR)
PS_OUTPUT main(PS_INPUT input, bool frontFace : SV_IsFrontFace)
{
	PS_OUTPUT psout = (PS_OUTPUT)0;
#			if defined(DCLF_PULLED)
	DCLFGrassPixelStatics(input.Bucket);
#			endif
	float4 baseColor = TexBaseSampler.SampleBias(SampBaseSampler, input.TexCoord.xy, SharedData::MipBias);
	baseColor.xyz = Color::Albedo(ColorManagement::DiffuseToWorking(baseColor.xyz, true));

#			if defined(RENDER_DEPTH)
	float diffuseAlpha = input.Fade * baseColor.w;
#			elif defined(DO_ALPHA_TEST)
	float diffuseAlpha = input.Color.w * baseColor.w;
#			endif
#			if defined(RENDER_DEPTH) || defined(DO_ALPHA_TEST)
	if ((diffuseAlpha - AlphaTestRefRS) < 0)
		discard;
#			endif
#			if defined(RENDER_DEPTH)
#				ifdef GRASS_OPTIMIZATIONS
	psout.PS.xyz = input.HPosition.zzz;
#				else
	psout.PS.xyz = input.Depth.xxx / input.Depth.yyy;
#				endif
	psout.PS.w = diffuseAlpha;
#				if !defined(RENDER_DEPTH)
	psout.Diffuse.rgb = PreExposure::EncodeScene(psout.Diffuse.rgb);
#					if defined(GRASS_LIGHTING)
	psout.Specular.rgb = PreExposure::EncodeScene(psout.Specular.rgb);
#					endif
#				endif
	return psout;
#			else
	if (SharedData::lodBlendingSettings.DisableTerrainVertexColors)
		input.Color.xyz = 1;

	if (SharedData::grassLightingSettings.OverrideComplexGrassSettings)
		baseColor.xyz *= SharedData::grassLightingSettings.BasicGrassBrightness;

#				ifdef GRASS_CULLED_INSTANCES
	const float lodBrightness = input.LodTier > 1.5 ? SharedData::grassLightingSettings.FarLODBrightness : SharedData::grassLightingSettings.MidLODBrightness;
	baseColor.xyz *= lerp(1.0, lodBrightness, saturate(input.LodTier));
#				endif

	psout.MotionVectors = MotionBlur::GetSSMotionVector(float4(input.WorldPosition, 1), float4(input.PreviousWorldPosition, 1));

	float3 viewDirection = -normalize(input.WorldPosition.xyz);
	float3 vertexNormal = normalize(input.VertexNormal.xyz);
	float3 clumpNormal = normalize(input.ClumpNormal);
	float3 faceNormal = PBR::GetThinSurfaceFaceNormal(input.WorldPosition.xyz, viewDirection, vertexNormal);
	const bool sphereNormal = (Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::GrassSphereNormal) != 0;
	// Winding keeps sidedness constant across interpolated blade normals.
	if (!sphereNormal && !frontFace)
		vertexNormal = -vertexNormal;

#				ifdef GRASS_CULLED_INSTANCES
	const bool pbrDetail = input.IsFar <= 0.5;
#				else
	const bool pbrDetail = true;
#				endif
	float3 normal = vertexNormal;
	float4 rawRMAOS = float4(1, 0, 1, 0);
#				ifdef GRASS_CULLED_INSTANCES
	[branch] if (pbrDetail)
#				else
	if (pbrDetail)
#				endif
	{
		float4 normalSample = TexNormalSampler.SampleBias(SampNormalSampler, input.TexCoord.xy, SharedData::MipBias);
		float3x3 tbn = GrassLighting::CalculateTBN(vertexNormal, -input.WorldPosition.xyz, input.TexCoord.xy);
		normal = normalize(mul(GrassLighting::TransformNormal(normalSample.xyz), tbn));
		rawRMAOS = TexRMAOSSampler.SampleBias(SampRMAOSSampler, input.TexCoord.xy, SharedData::MipBias) *
		           float4(PBRParams1.x, 1, 1, PBRParams1.y);
	}
	// Preserve the authored normal-map basis before rounding the shading normal.
	if (!sphereNormal) {
		normal = GrassLighting::GetClumpBlendedNormal(normal, clumpNormal);
	}
	MaterialProperties material = (MaterialProperties)0;
	material.ThinSurfaceSpecularWeight = saturate(frontFace ? PBRParams4.y : PBRParams4.z);
	material.Roughness = clamp(rawRMAOS.x * (frontFace ? 1.0f : PBRParams4.w), PBR::Constants::MinRoughness, PBR::Constants::MaxRoughness);
	material.Metallic = saturate(rawRMAOS.y);
	material.AO = rawRMAOS.z;

	float3 vertexColor = ColorManagement::PBRVertexColorToLinear(input.Color.xyz);
	float vertexAO = max(max(vertexColor.r, vertexColor.g), vertexColor.b);
	vertexColor = Color::LinearSRGBToWorking(vertexColor / max(vertexAO, EPSILON_DIVISION));
	material.BaseColor = Color::LinearToPBRMaterial(Color::PBRMaterialToLinear(baseColor.xyz) * vertexColor);
	material.F0 = lerp(saturate(rawRMAOS.w), material.BaseColor, material.Metallic);
	material.BaseColor *= 1 - material.Metallic;
	material.Thickness = 1.0f;
	[branch] if ((PBRFlags & PBR::Flags::Subsurface) != 0)
	{
		material.SubsurfaceColor = PBRParams2.xyz;
		material.Thickness = PBRParams2.w;
		material.SubsurfaceBalance = PBRParams4.x;
		[branch] if (pbrDetail && (PBRFlags & PBR::Flags::HasFeatureTexture0) != 0)
		{
			float4 subsurface = TexSubsurfaceSampler.Sample(SampSubsurfaceSampler, input.TexCoord.xy);
			material.SubsurfaceColor *= Color::Albedo(Color::LinearSRGBToWorking(subsurface.xyz));
			material.Thickness *= subsurface.w;
		}
		else
		{
			material.SubsurfaceColor *= material.BaseColor;
		}
	}
	[branch] if (pbrDetail && (PBRFlags & PBR::Flags::Fuzz) != 0)
	{
		material.FuzzColor = PBRParams3.xyz;
		material.FuzzWeight = PBRParams3.w;
		[branch] if ((PBRFlags & PBR::Flags::HasFeatureTexture1) != 0)
		{
			float4 fuzz = TexFuzzSampler.Sample(SampFuzzSampler, input.TexCoord.xy);
			material.FuzzColor *= Color::Albedo(Color::LinearSRGBToWorking(fuzz.xyz));
			material.FuzzWeight *= fuzz.w;
		}
	}

	float3 viewPosition = mul(FrameBuffer::CameraView, float4(input.WorldPosition.xyz, 1)).xyz;
	float2 screenUV = FrameBuffer::ViewToUV(viewPosition);
	float screenNoise = Random::InterleavedGradientNoise(input.HPosition.xy, SharedData::FrameCount);
	float3 dirLightColor = Color::DirectionalLight(SharedData::DirLightColor.xyz);
#				if defined(EXP_HEIGHT_FOG)
	if (SharedData::exponentialHeightFogSettings.enabled)
		dirLightColor *= ExponentialHeightFog::GetSunlightFogAttenuation(input.WorldPosition.xyz, FrameBuffer::CameraPosAdjust.xyz);
#				endif
#				if defined(PHYSICAL_SKY)
	if (SharedData::physSkyData.enabled) {
		dirLightColor *= PhysSky::SampleTr(normalize(SharedData::DirLightDirection.xyz), SampShadowMaskSampler);
		dirLightColor *= PhysSky::GetDirlightTransmittance(input.WorldPosition.xyz + FrameBuffer::CameraPosAdjust.xyz, SampPhysicalSkyWrapSampler, true);
	}
#				endif
	float3 originDirLightColor = dirLightColor;
	if (!SharedData::InInterior)
		dirLightColor *= ShadowSampling::GetWorldShadow(input.WorldPosition.xyz, FrameBuffer::CameraPosAdjust.xyz);
#				if defined(WATER_EFFECTS)
	dirLightColor *= WaterEffects::ComputeCaustics(SharedData::GetWaterData(input.WorldPosition.xyz), input.WorldPosition.xyz);
#				endif

	float4 shadowColor = TexShadowMaskSampler.Load(int3(input.HPosition.xy, 0));
	float dirDetailedShadow = SharedData::InInterior ? 1.0 : shadowColor.x;
	float2 screenSpaceShadows = 1.0;
#				if defined(SCREEN_SPACE_SHADOWS)
#					ifdef GRASS_CULLED_INSTANCES
	if (!SharedData::InInterior && input.IsFar <= 0.5)
#					else
	if (!SharedData::InInterior)
#					endif
		screenSpaceShadows = ScreenSpaceShadows::GetScreenSpaceShadows(input.HPosition.xyz, screenUV, screenNoise);
#				endif  // SCREEN_SPACE_SHADOWS

	float dirSoftShadow = dirDetailedShadow;
	float skylightingShadowVisibility = 1.0;
#				if defined(SKYLIGHTING)
	sh2 skylightingSH = Skylighting::Sample(input.WorldPosition.xyz, normal, skylightingShadowVisibility);
	if (!SharedData::InInterior)
		dirSoftShadow = skylightingShadowVisibility;
	float skylightingDiffuse = Skylighting::GetSkylightingDiffuse(skylightingSH, input.WorldPosition.xyz, normal, vertexAO);
	[branch] if ((PBRFlags & PBR::Flags::Subsurface) != 0)
		skylightingDiffuse = PBR::GetFoliageShadowVisibility(skylightingDiffuse, material.Thickness);
#				endif

#				if defined(WETNESS_EFFECTS)
	float nearFactor = smoothstep(4096.0 * 2.5, 0.0, viewPosition.z);
	float waterHeight = SharedData::GetWaterData(input.WorldPosition.xyz).w;
	float3 wetnessWorldPosition = input.WorldPosition.xyz + FrameBuffer::CameraPosAdjust.xyz;
#					if defined(SKYLIGHTING)
	float wetnessOcclusion = saturate(SphericalHarmonics::Unproject(skylightingSH, float3(0, 0, 1)));
#					else
	float wetnessOcclusion = 1.0;
#					endif
	WetnessEffects::SurfaceWetnessState wetnessState = WetnessEffects::GetSurfaceWetnessState(input.WorldPosition.xyz, wetnessWorldPosition, wetnessWorldPosition, normal, vertexNormal, waterHeight, wetnessOcclusion, nearFactor, -1.0, 0.0, false, false);
#				endif

#				if defined(WETNESS_EFFECTS)
	float wetnessPorosity = 1.0;
	WetnessEffects::ApplySurfaceWetnessAlbedo(material.BaseColor, wetnessState, wetnessPorosity);
#				endif

	PBR::ThinSurface grassSurface = PBR::CreateThinSurface(material, clamp(abs(dot(normal, viewDirection)), EPSILON_DOT_CLAMP, 1.0f), pbrDetail);

	float3 wetnessDirectSpecularColor = 0;
#				if defined(SCREEN_SPACE_SHADOWS)
#					ifdef GRASS_OPTIMIZATIONS
	if (!SharedData::InInterior && input.IsFar <= 0.5)
#					else
	if (!SharedData::InInterior)
#					endif
		dirDetailedShadow *= ScreenSpaceShadows::GetThinSurfaceScreenSpaceShadow(input.HPosition.xyz, screenUV, screenNoise,
			dot(faceNormal, SharedData::DirLightDirection.xyz));
#				endif  // SCREEN_SPACE_SHADOWS

	float canopyVisibility = GrassLighting::GetCanopyVisibility(input.VertexNormal.w);
	float dirClumpVisibility = canopyVisibility * (sphereNormal ? 1.0 : GrassLighting::GetClumpLightVisibility(clumpNormal, SharedData::DirLightDirection.xyz));
	dirDetailedShadow *= dirClumpVisibility;
	dirSoftShadow *= dirClumpVisibility;

	float wrap = saturate(SharedData::grassLightingSettings.PBRWrappedLightingAmount);
	DirectLightingOutput totalLighting = (DirectLightingOutput)0;
	DirectContext dirContext = CreateDirectLightingContext(normal, normal, vertexNormal, viewDirection, viewDirection,
		SharedData::DirLightDirection.xyz, SharedData::DirLightDirection.xyz, dirLightColor, dirDetailedShadow, dirSoftShadow);
	DirectLightingOutput dirOutput;
	PBR::GetDirectLightInputThinSurface(dirOutput, dirContext, material, grassSurface, pbrDetail, wrap);
#				if defined(WETNESS_EFFECTS)
	dirOutput.transmission *= WetnessEffects::ApplySurfaceWetnessDirectLighting(wetnessState, viewDirection, SharedData::DirLightDirection.xyz, dirLightColor, dirDetailedShadow, dirOutput.diffuse, dirOutput.specular, wetnessDirectSpecularColor);
#				endif
	totalLighting.diffuse += dirOutput.diffuse;
	totalLighting.specular += dirOutput.specular;
	totalLighting.transmission += dirOutput.transmission;

#				if defined(LIGHT_LIMIT_FIX)
	uint clusterIndex = 0;
	uint lightCount = 0;
	if (LightLimitFix::GetClusterIndex(screenUV, viewPosition.z, clusterIndex)) {
		lightCount = LightLimitFix::lightGrid[clusterIndex].lightCount;
		if (lightCount) {
			uint lightOffset = LightLimitFix::lightGrid[clusterIndex].offset;
			[loop] for (uint i = 0; i < lightCount; ++i)
			{
				LightLimitFix::Light light = LightLimitFix::lights[LightLimitFix::lightList[lightOffset + i]];
				float3 lightVector = light.positionWS.xyz - input.WorldPosition.xyz;
				float lightDist = length(lightVector);
				float attenuation = LightLimitFix::GetAttenuation(lightDist, light);
				if (attenuation < 1e-5)
					continue;
				float3 lightColor = Color::PointLight(light.color.xyz) * attenuation * light.fade;
				float lightShadow = 1.0;
				[branch] if (light.lightFlags & LightLimitFix::LightFlags::LocalShadow)
					lightShadow = LightLimitFix::GetLocalShadow(LinearSampler, light.localShadowIndex, input.WorldPosition.xyz, FrameBuffer::CameraPosAdjust.xyz, float3(0.0, 0.0, 0.0), false, screenNoise);
				else if (light.lightFlags & LightLimitFix::LightFlags::Shadow)
					lightShadow = shadowColor[light.shadowLightIndex];
				float3 lightDirection = lightVector / max(lightDist, EPSILON_DIVISION);
				lightShadow *= canopyVisibility * (sphereNormal ? 1.0 : GrassLighting::GetClumpLightVisibility(clumpNormal, lightDirection));
				DirectContext pointContext = CreateDirectLightingContext(normal, normal, vertexNormal, viewDirection, viewDirection,
					lightDirection, lightDirection, lightColor, lightShadow, lightShadow);
				DirectLightingOutput pointOutput;
				PBR::GetDirectLightInputThinSurface(pointOutput, pointContext, material, grassSurface, pbrDetail, wrap);
#					if defined(WETNESS_EFFECTS)
				pointOutput.transmission *= WetnessEffects::ApplySurfaceWetnessDirectLighting(wetnessState, viewDirection, lightDirection, lightColor, lightShadow, pointOutput.diffuse, pointOutput.specular, wetnessDirectSpecularColor);
#					endif
				totalLighting.diffuse += pointOutput.diffuse;
				totalLighting.specular += pointOutput.specular;
				totalLighting.transmission += pointOutput.transmission;
			}
		}
	}
#				endif

	IndirectLobeWeights indirectLobes;
	IndirectContext indirectContext = CreateIndirectLightingContext(normal, vertexNormal, viewDirection);
	PBR::GetIndirectLobeWeightsThinSurface(indirectLobes, indirectContext, material, grassSurface);
	indirectLobes.diffuse *= canopyVisibility;
	indirectLobes.specular *= canopyVisibility;
	indirectLobes.frontTransmission *= canopyVisibility;
	indirectLobes.backTransmission *= canopyVisibility;

	float3 wetnessReflectance = 0;
#				if defined(WETNESS_EFFECTS)
	wetnessReflectance = WetnessEffects::ApplySurfaceWetnessIndirectLobeWeights(indirectLobes.diffuse, indirectLobes.specular, wetnessState, viewDirection);
	indirectLobes.frontTransmission *= 1.0 - wetnessReflectance;
	indirectLobes.backTransmission *= 1.0 - wetnessReflectance;
#				endif

	float3 directColor = totalLighting.diffuse + totalLighting.transmission;
	float3 directionalAmbientColor = Color::Ambient(max(0, SharedData::GetAmbient(normal)));
#				if defined(IBL)
	if (SharedData::iblSettings.EnableIBL)
		directionalAmbientColor = ImageBasedLighting::GetDiffuseIBL(directionalAmbientColor, -normal, input.WorldPosition.xyz);
#				endif
	if (SharedData::ssgiSettings.EnableIL != 0)
		directionalAmbientColor = 0;

#				if defined(PSEUDO_SUN_BOUNCE)
	float3 specularBounce = 0;
	if (!SharedData::InInterior && SharedData::pseudoSunBounceSettings.intensity > 0.0) {
		float cloudShadows = 1;
#					if defined(CLOUD_SHADOWS)
		cloudShadows = CloudShadows::GetCloudShadowMult(input.WorldPosition.xyz, LinearSampler);
#					endif
		SunBounce::SH2_RGB sunBounceSH = SunBounce::CalcSunBounceSH(SharedData::DirLightDirection.xyz, originDirLightColor * cloudShadows,
			SharedData::pseudoSunBounceSettings.groundAlbedo, SharedData::pseudoSunBounceSettings.wallAlbedo);

		specularBounce = max(0, SunBounce::CalcFauxSpecularBounce(normal, viewDirection, material.Roughness, sunBounceSH)) * SharedData::pseudoSunBounceSettings.intensity;

		sunBounceSH.R = SphericalHarmonics::HanningConvolution(sunBounceSH.R, SharedData::pseudoSunBounceSettings.windowWidth);
		sunBounceSH.G = SphericalHarmonics::HanningConvolution(sunBounceSH.G, SharedData::pseudoSunBounceSettings.windowWidth);
		sunBounceSH.B = SphericalHarmonics::HanningConvolution(sunBounceSH.B, SharedData::pseudoSunBounceSettings.windowWidth);

		float3 bounceLighting;
		bounceLighting.r = SphericalHarmonics::Unproject(sunBounceSH.R, -normal);
		bounceLighting.g = SphericalHarmonics::Unproject(sunBounceSH.G, -normal);
		bounceLighting.b = SphericalHarmonics::Unproject(sunBounceSH.B, -normal);

		bounceLighting = max(0, bounceLighting);
#					if defined(SKYLIGHTING)
		float3 bouncedSkylighting = saturate(MultiBounceAO(SharedData::pseudoSunBounceSettings.groundAlbedo, skylightingDiffuse) * skylightingDiffuse);
		bounceLighting *= bouncedSkylighting;
		specularBounce *= bouncedSkylighting;
#					endif

		directionalAmbientColor += bounceLighting * SharedData::pseudoSunBounceSettings.intensity;
	}
#				endif

	directColor += indirectLobes.diffuse * directionalAmbientColor;
	float3 outputAlbedo = indirectLobes.diffuse;
	directionalAmbientColor *= outputAlbedo;
#				if defined(SKYLIGHTING)
	Skylighting::ApplySkylighting(directColor, directionalAmbientColor, outputAlbedo, skylightingDiffuse);
	sh2 backSkylightingSH = skylightingSH;
	[branch] if (any(indirectLobes.backTransmission > 0.0f))
	{
		float unusedShadowVisibility;
		backSkylightingSH = Skylighting::Sample(input.WorldPosition.xyz, -normal, unusedShadowVisibility, false);
	}
#				endif

	[unroll] for (uint side = 0; side < 2; ++side)
	{
		float3 transmissionWeight = side == 0 ? indirectLobes.frontTransmission : indirectLobes.backTransmission;
		[branch] if (any(transmissionWeight > 0.0f))
		{
			float3 transmissionNormal = side == 0 ? normal : -normal;
			float3 ambientColor = Color::Ambient(max(0, SharedData::GetAmbient(transmissionNormal)));
			float3 transmittedAmbientColor;
			directColor += GetThinSurfaceTransmissionLighting(ambientColor, transmissionWeight, transmissionNormal,
				input.WorldPosition.xyz, vertexAO, material.Thickness, transmittedAmbientColor
#				if defined(SKYLIGHTING)
				,
				side == 0 ? skylightingSH : backSkylightingSH
#				endif
			);
			directionalAmbientColor += transmittedAmbientColor;
		}
	}
	// Deferred diffuse/GI uses the visible normal, so its albedo contains reflection only.

	[branch] if ((PBRFlags & PBR::Flags::HasEmissive) != 0)
	{
		float3 glowColor = ColorManagement::TextureToWorking(TexEmissiveSampler.SampleBias(SampEmissiveSampler, input.TexCoord.xy, SharedData::MipBias).xyz, true) * SharedData::linearLightingSettings.glowmapMult;
		float3 emitVertexColor = ColorManagement::PBRVertexColorToLinear(input.Color.xyz);
		float emitVertexAO = max(max(emitVertexColor.r, emitVertexColor.g), emitVertexColor.b);
		emitVertexColor = emitVertexAO == 0.0f ? 1.0f : emitVertexColor * lerp(1 / max(emitVertexAO, 1e-4), 1, SharedData::truePBRSettings.VertexAOStrength);
		emitVertexColor = Color::LinearSRGBToWorking(emitVertexColor);
		glowColor *= PBRParams1.z;
		directColor += Color::LinearToPBRMaterial(glowColor) * Color::LinearToPBRMaterial(emitVertexColor);
	}

	directColor *= Color::PBRLightingScale;
	directionalAmbientColor *= Color::PBRLightingScale;
	outputAlbedo *= Color::PBRLightingScale;
	totalLighting.specular *= Color::PBRLightingScale;

	float3 outputColor = FogNearColor.w * directColor;
#				if defined(LIGHT_LIMIT_FIX) && defined(LLFDEBUG)
	if (SharedData::lightLimitFixSettings.EnableLightsVisualisation) {
		if (SharedData::lightLimitFixSettings.LightsVisualisationMode < 2) {
			outputColor = Color::TurboColormap(0);
		} else {
			outputColor = Color::TurboColormap((float)lightCount / MAX_CLUSTER_LIGHTS);
		}
	}
#				endif

	float3 outputNormal = normal;
	float outputRoughness = material.Roughness;
#				if defined(WETNESS_EFFECTS)
	WetnessEffects::ApplySurfaceWetnessOutput(wetnessState, outputNormal, outputRoughness);
#				endif
#				if defined(PSEUDO_SUN_BOUNCE)
	totalLighting.specular += specularBounce * (indirectLobes.specular + wetnessReflectance);
#				endif
	float3 normalVS = normalize(FrameBuffer::WorldToView(outputNormal, false));
	psout.Diffuse = float4(outputColor, 1);
	psout.NormalGlossiness = float4(GBuffer::EncodeNormal(normalVS), 1 - outputRoughness, 1);
	psout.Albedo = float4(outputAlbedo, 1);
	psout.Specular = float4(totalLighting.specular + wetnessDirectSpecularColor, 1);
	psout.Reflectance = float4(indirectLobes.specular + wetnessReflectance, 1);
	psout.Masks = float4(0, 0, Color::RGBToYCoCg(directionalAmbientColor).x, 0);
	psout.Masks2 = float4(1.0 - vertexAO, 0, 0, 1);
#				if !defined(RENDER_DEPTH)
	psout.Diffuse.rgb = PreExposure::EncodeScene(psout.Diffuse.rgb);
#					if defined(GRASS_LIGHTING)
	psout.Specular.rgb = PreExposure::EncodeScene(psout.Specular.rgb);
#					endif
#				endif
	return psout;
#			endif
}
#		else
PS_OUTPUT main(PS_INPUT input, bool frontFace : SV_IsFrontFace)
{
	PS_OUTPUT psout = (PS_OUTPUT)0;
#			if defined(DCLF_PULLED)
	DCLFGrassPixelStatics(input.Bucket);
#			endif

#			if defined(SKYLIGHTING_SHADOW_VIS)
	float skylightingShadowVisibility = 1.0;
#			endif

	// TruePBR textures have no complex atlas but share this depth pass, and can pass the corner-texel test by chance.
	const bool pbr = (Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::GrassPBR) != 0;
#			ifdef GRASS_OPTIMIZATIONS
	bool complex = !pbr && input.IsComplex > 0.5;
	[branch] if (!pbr && input.IsComplex < 0.0)
#			else
	bool complex = false;
	[branch] if (!pbr)
#			endif
	{
		float x, y;
		TexBaseSampler.GetDimensions(x, y);
		const float3 complexTest = TexBaseSampler.Load(int3(0, (int)y - 1, 0)).xyz * 2.0 - 1.0;
		complex = abs(length(complexTest) - 1.0) < SharedData::grassLightingSettings.ComplexGrassThreshold;
	}

#			if defined(RENDER_DEPTH)
	// Alpha is the only texture channel needed here; select the atlas half without a sample branch.
	const float2 alphaUV = float2(input.TexCoord.x, input.TexCoord.y * (complex ? 0.5 : 1.0));
	const float baseAlpha = TexBaseSampler.SampleBias(SampBaseSampler, alphaUV, SharedData::MipBias).w;
	const float diffuseAlpha = input.Fade * baseAlpha;
	if ((diffuseAlpha - AlphaTestRefRS) < 0)
		discard;

#				ifdef GRASS_OPTIMIZATIONS
	// The optimized main-view pass uses the rasterizer's depth directly, with no extra interpolator.
	psout.PS.xyz = input.HPosition.zzz;
#				else
	psout.PS.xyz = input.Depth.xxx / input.Depth.yyy;
#				endif
	psout.PS.w = diffuseAlpha;
#			else
	float4 baseColor;
	if (complex) {
		baseColor = TexBaseSampler.SampleBias(SampBaseSampler, float2(input.TexCoord.x, input.TexCoord.y * 0.5), SharedData::MipBias);
	} else {
		baseColor = TexBaseSampler.SampleBias(SampBaseSampler, input.TexCoord.xy, SharedData::MipBias);
	}

#				if defined(DO_ALPHA_TEST)
	float diffuseAlpha = input.Color.w * baseColor.w;
	if ((diffuseAlpha - AlphaTestRefRS) < 0) {
		discard;
	}
#				endif

	baseColor.xyz = Color::Albedo(ColorManagement::DiffuseToWorking(baseColor.xyz));

	if (SharedData::lodBlendingSettings.DisableTerrainVertexColors)
		input.Color.xyz = 1;

#				ifdef GRASS_CULLED_INSTANCES
	// Keep the atlas selection above independent of the distant-detail cutoff.
	const bool complexDetail = complex && input.IsFar <= 0.5;
	float4 specColor = complexDetail ? TexBaseSampler.SampleBias(SampBaseSampler, float2(input.TexCoord.x, 0.5 + input.TexCoord.y * 0.5), SharedData::MipBias) : 1;
#				else
	float4 specColor = complex ? TexBaseSampler.SampleBias(SampBaseSampler, float2(input.TexCoord.x, 0.5 + input.TexCoord.y * 0.5), SharedData::MipBias) : 1;
#				endif

	psout.MotionVectors = MotionBlur::GetSSMotionVector(float4(input.WorldPosition, 1), float4(input.PreviousWorldPosition, 1));

	float3 viewDirection = -normalize(input.WorldPosition.xyz);
	float3 vertexNormal = normalize(input.VertexNormal.xyz);

	float3 viewPosition = mul(FrameBuffer::CameraView, float4(input.WorldPosition.xyz, 1)).xyz;
	float2 screenUV = FrameBuffer::ViewToUV(viewPosition);
	float screenNoise = Random::InterleavedGradientNoise(input.HPosition.xy, SharedData::FrameCount);

	float3 clumpNormal = normalize(input.ClumpNormal);
	float3 faceNormal = PBR::GetThinSurfaceFaceNormal(input.WorldPosition.xyz, viewDirection, vertexNormal);
	const bool sphereNormal = (Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::GrassSphereNormal) != 0;
	const bool useClumpOcclusion = !sphereNormal && !complex;
	// Winding keeps sidedness constant across interpolated blade normals.
	if (!sphereNormal && !frontFace)
		vertexNormal = -vertexNormal;

	float3 normal = vertexNormal;
	float3x3 tbn = 0;

#				ifdef GRASS_CULLED_INSTANCES
	if (complexDetail)
#				else
	if (complex)
#				endif
	{
		float3 normalColor = GrassLighting::TransformNormal(specColor.xyz);
		// world-space -> tangent-space -> world-space.
		// This is because we don't have pre-computed tangents.
		tbn = GrassLighting::CalculateTBN(normal, -input.WorldPosition.xyz, input.TexCoord.xy);
		normal = normalize(mul(normalColor, tbn));
	}

	// Preserve the authored normal-map basis before rounding the shading normal.
	if (!sphereNormal) {
		normal = GrassLighting::GetClumpBlendedNormal(normal, clumpNormal);
	}
	float3 specularNormal = normal;
	if (!sphereNormal)
		specularNormal = PBR::GetThinSurfaceNormal(normal, viewDirection);

	if (!complex || SharedData::grassLightingSettings.OverrideComplexGrassSettings)
		baseColor.xyz *= SharedData::grassLightingSettings.BasicGrassBrightness;

#				ifdef GRASS_CULLED_INSTANCES
	const float lodBrightness = input.LodTier > 1.5 ? SharedData::grassLightingSettings.FarLODBrightness : SharedData::grassLightingSettings.MidLODBrightness;
	baseColor.xyz *= lerp(1.0, lodBrightness, saturate(input.LodTier));
#				endif

#				if defined(VANILLA_FRESNEL)
	const bool enableVanillaFresnel = SharedData::vanillaFresnelSettings.Enable;
	float3 F0 = enableVanillaFresnel ? max(SharedData::vanillaFresnelSettings.MinF0, saturate(specColor.w * SharedData::grassLightingSettings.SpecularStrength * SharedData::vanillaFresnelSettings.BaseF0Multiplier / Math::PI)) : 0.0;
#				else
	float3 F0 = 0.0;
#				endif
	float roughness = saturate(1.0 - SharedData::grassLightingSettings.Glossiness * 0.01);

	const bool hasSubsurface = SharedData::grassLightingSettings.SubsurfaceScatteringAmount > 0.0;
	const float grassOpacity = 1.0 - 0.5 * saturate(SharedData::grassLightingSettings.SubsurfaceScatteringAmount);

	float3 vertexColor = ColorManagement::SRGBToWorking(input.Color.xyz);
	float vertexAO = ColorManagement::SRGBToWorking(max(max(input.Color.r, input.Color.g), input.Color.b));
	vertexColor /= max(vertexAO, EPSILON_DIVISION);

#				if defined(SKYLIGHTING)
	float3 positionMSSkylight = input.WorldPosition.xyz;
	sh2 skylightingSH = Skylighting::Sample(positionMSSkylight, normal
#					if defined(SKYLIGHTING_SHADOW_VIS)
		,
		skylightingShadowVisibility
#					endif
	);
	float skylightingDiffuse = Skylighting::GetSkylightingDiffuse(skylightingSH, positionMSSkylight, normal, vertexAO);
	if (hasSubsurface)
		skylightingDiffuse = PBR::GetFoliageShadowVisibility(skylightingDiffuse, grassOpacity);
#				endif  // SKYLIGHTING

#				if defined(WETNESS_EFFECTS)
	float nearFactor = smoothstep(4096.0 * 2.5, 0.0, viewPosition.z);
	float waterHeight = SharedData::GetWaterData(input.WorldPosition.xyz).w;
	float3 wetnessWorldPosition = input.WorldPosition.xyz + FrameBuffer::CameraPosAdjust.xyz;
#					if defined(SKYLIGHTING)
	float wetnessOcclusion = saturate(SphericalHarmonics::Unproject(skylightingSH, float3(0, 0, 1)));
#					else
	float wetnessOcclusion = 1.0;
#					endif
	WetnessEffects::SurfaceWetnessState wetnessState = WetnessEffects::GetSurfaceWetnessState(input.WorldPosition.xyz, wetnessWorldPosition, wetnessWorldPosition, normal, vertexNormal, waterHeight, wetnessOcclusion, nearFactor, -1.0, 0.0, false, false);
#				endif

#				if defined(WETNESS_EFFECTS)
	float wetnessPorosity = 1.0;
	WetnessEffects::ApplySurfaceWetnessAlbedo(baseColor.xyz, wetnessState, wetnessPorosity);
#				endif

	float3 dirLightColor = Color::DirectionalLight(SharedData::DirLightColor.xyz);
	float3 dirLightColorMultiplier = 1;

#				if defined(EXP_HEIGHT_FOG)
	if (SharedData::exponentialHeightFogSettings.enabled) {
		dirLightColor *= ExponentialHeightFog::GetSunlightFogAttenuation(input.WorldPosition.xyz, FrameBuffer::CameraPosAdjust.xyz);
	}
#				endif

#				if defined(PHYSICAL_SKY)
	if (SharedData::physSkyData.enabled) {
		dirLightColorMultiplier *= PhysSky::SampleTr(normalize(SharedData::DirLightDirection.xyz), SampShadowMaskSampler);
		dirLightColorMultiplier *= PhysSky::GetDirlightTransmittance(input.WorldPosition.xyz + FrameBuffer::CameraPosAdjust.xyz, SampPhysicalSkyWrapSampler, true);
	}
#				endif

	float dirNdotL = dot(normal, SharedData::DirLightDirection.xyz);

	float4 shadowColor = TexShadowMaskSampler.Load(int3(input.HPosition.xy, 0));

	// Apply world shadow (terrain shadows, cloud shadows) directly to light color
	if (!SharedData::InInterior)
		dirLightColor *= ShadowSampling::GetWorldShadow(input.WorldPosition.xyz, FrameBuffer::CameraPosAdjust.xyz);
#				if defined(WATER_EFFECTS)
	dirLightColor *= WaterEffects::ComputeCaustics(SharedData::GetWaterData(input.WorldPosition.xyz), input.WorldPosition.xyz);
#				endif

	float dirShadow = SharedData::InInterior ? 1.0 : shadowColor.x;
	float dirDetailedShadow = dirShadow;
	float2 dirTransmissionShadow = dirShadow;

#				if defined(SCREEN_SPACE_SHADOWS)
#					ifdef GRASS_OPTIMIZATIONS
	if (!SharedData::InInterior && input.IsFar <= 0.5)
#					else
	if (!SharedData::InInterior)
#					endif
		dirDetailedShadow *= ScreenSpaceShadows::GetThinSurfaceScreenSpaceShadow(input.HPosition.xyz, screenUV, screenNoise,
			dot(faceNormal, SharedData::DirLightDirection.xyz));
#				endif  // SCREEN_SPACE_SHADOWS

	float3 diffuseColor = 0;
	float3 specularColor = 0;

	float3 lightsDiffuseColor = 0;
	float3 lightsSpecularColor = 0;
#				if defined(WETNESS_EFFECTS)
	float3 wetnessDirectSpecularColor = 0;
#				endif

	float3 originDirLightColor = dirLightColor;
	dirLightColor *= dirLightColorMultiplier;

#				if defined(SKYLIGHTING)
	dirTransmissionShadow = SharedData::InInterior ? 1.0 : skylightingShadowVisibility;
	sh2 backSkylightingSH = skylightingSH;
	[branch] if (hasSubsurface && SharedData::grassLightingSettings.SubsurfaceBalance > 0.0)
	{
		backSkylightingSH = Skylighting::Sample(positionMSSkylight, -normal, dirTransmissionShadow.y);
	}
#				endif  // SKYLIGHTING

	// Receiver opacity cannot determine whether the object blocking sunlight transmits it.
	dirTransmissionShadow = min(dirTransmissionShadow, dirShadow);

	float canopyVisibility = GrassLighting::GetCanopyVisibility(input.VertexNormal.w);
	float dirClumpVisibility = canopyVisibility * (useClumpOcclusion ? GrassLighting::GetClumpLightVisibility(clumpNormal, SharedData::DirLightDirection.xyz) : 1.0);
	dirDetailedShadow *= dirClumpVisibility;
	dirTransmissionShadow *= dirClumpVisibility;

	float3 albedo = baseColor.xyz * vertexColor;
	float3 reflectionAlbedo, frontTransmissionAlbedo, backTransmissionAlbedo;
	GrassLighting::GetSubsurfaceAlbedos(albedo, SharedData::grassLightingSettings.SubsurfaceScatteringAmount,
		SharedData::grassLightingSettings.SubsurfaceBalance, reflectionAlbedo, frontTransmissionAlbedo, backTransmissionAlbedo);
	float3 shineThroughAlbedo = hasSubsurface ? albedo : 0.0;
	float3 transmissionRadiance;
	PBR::GetThinSurfaceDirectScattering(reflectionAlbedo, frontTransmissionAlbedo, backTransmissionAlbedo, shineThroughAlbedo, grassOpacity, dirLightColor * Color::BRDFScale,
		dirNdotL, 0.0, dirDetailedShadow, dirTransmissionShadow, lightsDiffuseColor, transmissionRadiance);

	float3 dirDiffuseColor = lightsDiffuseColor;
	float3 dirSpecularColor = 0;
#				ifdef GRASS_CULLED_INSTANCES
	if (complexDetail)
#				else
	if (complex)
#				endif
		dirSpecularColor = dirDetailedShadow * GrassLighting::GetLightSpecularInput(SharedData::DirLightDirection.xyz, viewDirection, specularNormal, dirLightColor, roughness, F0) * Color::BRDFScale;
#				if defined(WETNESS_EFFECTS)
	transmissionRadiance *= WetnessEffects::ApplySurfaceWetnessDirectLighting(wetnessState, viewDirection, SharedData::DirLightDirection.xyz, dirLightColor, dirDetailedShadow * Color::PBRLightingCompensation * Color::PBRLightingScale, dirDiffuseColor, dirSpecularColor, wetnessDirectSpecularColor);
#				endif
	lightsDiffuseColor = dirDiffuseColor;
	lightsSpecularColor += dirSpecularColor;

#				if defined(LIGHT_LIMIT_FIX)
	uint clusterIndex = 0;
	uint lightCount = 0;

	if (LightLimitFix::GetClusterIndex(screenUV, viewPosition.z, clusterIndex)) {
		lightCount = LightLimitFix::lightGrid[clusterIndex].lightCount;
		if (lightCount) {
			uint lightOffset = LightLimitFix::lightGrid[clusterIndex].offset;

			[loop] for (uint i = 0; i < lightCount; i++)
			{
				uint clusteredLightIndex = LightLimitFix::lightList[lightOffset + i];
				LightLimitFix::Light light = LightLimitFix::lights[clusteredLightIndex];

				float3 lightDirection = light.positionWS.xyz - input.WorldPosition.xyz;
				float lightDist = length(lightDirection);

				float intensityMultiplier = LightLimitFix::GetAttenuation(lightDist, light);
				if (intensityMultiplier < 1e-5)
					continue;

				float3 lightColor = Color::PointLight(light.color.xyz) * intensityMultiplier * light.fade;
				float lightShadow = 1.0;

				float shadowComponent = 1.0;
				[branch] if (light.lightFlags & LightLimitFix::LightFlags::LocalShadow)
				{
					shadowComponent = LightLimitFix::GetLocalShadow(LinearSampler, light.localShadowIndex, input.WorldPosition.xyz, FrameBuffer::CameraPosAdjust.xyz, float3(0.0, 0.0, 0.0), false, screenNoise);
					lightShadow *= shadowComponent;
				}
				else if (light.lightFlags & LightLimitFix::LightFlags::Shadow)
				{
					shadowComponent = shadowColor[light.shadowLightIndex];
					lightShadow *= shadowComponent;
				}

				float3 normalizedLightDirection = normalize(lightDirection);

				float3 pointLightColor = lightColor;
				lightShadow *= canopyVisibility * (useClumpOcclusion ? GrassLighting::GetClumpLightVisibility(clumpNormal, normalizedLightDirection) : 1.0);
				lightColor *= lightShadow;

				float NdotL = dot(normal, normalizedLightDirection);
				float3 lightDiffuseColor, lightTransmissionColor;
				PBR::GetThinSurfaceDirectScattering(reflectionAlbedo, frontTransmissionAlbedo, backTransmissionAlbedo, shineThroughAlbedo, grassOpacity, lightColor * Color::BRDFScale,
					NdotL, 0.0, 1.0, 1.0, lightDiffuseColor, lightTransmissionColor);
				float3 lightSpecularColor = 0;

#					ifdef GRASS_CULLED_INSTANCES
				if (complexDetail)
#					else
				if (complex)
#					endif
					lightSpecularColor = GrassLighting::GetLightSpecularInput(normalizedLightDirection, viewDirection, specularNormal, lightColor, roughness, F0) * Color::BRDFScale;
#					if defined(WETNESS_EFFECTS)
				lightTransmissionColor *= WetnessEffects::ApplySurfaceWetnessDirectLighting(wetnessState, viewDirection, normalizedLightDirection, pointLightColor, lightShadow * Color::PBRLightingCompensation * Color::PBRLightingScale, lightDiffuseColor, lightSpecularColor, wetnessDirectSpecularColor);
#					endif
				transmissionRadiance += lightTransmissionColor;
				lightsDiffuseColor += lightDiffuseColor;
				lightsSpecularColor += lightSpecularColor;
			}
		}
	}
#				endif  // LIGHT_LIMIT_FIX

	diffuseColor += lightsDiffuseColor;

	float3 directionalAmbientColor = Color::Ambient(max(0, SharedData::GetAmbient(normal)));

#				if defined(IBL)
	if (SharedData::iblSettings.EnableIBL)
		directionalAmbientColor = ImageBasedLighting::GetDiffuseIBL(directionalAmbientColor, -normal, input.WorldPosition.xyz);
#				endif

	if (SharedData::ssgiSettings.EnableIL != 0)
		directionalAmbientColor = 0;

	float3 reflectance = 0;
#				if defined(DYNAMIC_CUBEMAPS) && defined(VANILLA_FRESNEL)
	if (SharedData::vanillaFresnelSettings.Enable) {
		float2 specularBDRF = BRDF::EnvBRDF(roughness, saturate(dot(viewDirection, normal)));
		reflectance = F0 * specularBDRF.x + specularBDRF.y;
	}
#				endif

	float3 diffuseLobeWeight = reflectionAlbedo;
#				if defined(WETNESS_EFFECTS)
	float3 wetnessReflectance = WetnessEffects::ApplySurfaceWetnessIndirectLobeWeights(diffuseLobeWeight, reflectance, wetnessState, viewDirection);
	frontTransmissionAlbedo *= 1.0 - wetnessReflectance;
	backTransmissionAlbedo *= 1.0 - wetnessReflectance;
#				endif

#				if defined(PSEUDO_SUN_BOUNCE)
	float3 specularBounce = 0;
	if (!SharedData::InInterior && SharedData::pseudoSunBounceSettings.intensity > 0.0) {
		float cloudShadows = 1;
#					if defined(CLOUD_SHADOWS)
		cloudShadows = CloudShadows::GetCloudShadowMult(input.WorldPosition.xyz, LinearSampler);
#					endif
		SunBounce::SH2_RGB sunBounceSH = SunBounce::CalcSunBounceSH(SharedData::DirLightDirection.xyz, originDirLightColor * cloudShadows,
			SharedData::pseudoSunBounceSettings.groundAlbedo, SharedData::pseudoSunBounceSettings.wallAlbedo);

		specularBounce = max(0, SunBounce::CalcFauxSpecularBounce(normal, viewDirection, saturate(1 - (0.01 * SharedData::grassLightingSettings.Glossiness)), sunBounceSH)) * SharedData::pseudoSunBounceSettings.intensity;

		sunBounceSH.R = SphericalHarmonics::HanningConvolution(sunBounceSH.R, SharedData::pseudoSunBounceSettings.windowWidth);
		sunBounceSH.G = SphericalHarmonics::HanningConvolution(sunBounceSH.G, SharedData::pseudoSunBounceSettings.windowWidth);
		sunBounceSH.B = SphericalHarmonics::HanningConvolution(sunBounceSH.B, SharedData::pseudoSunBounceSettings.windowWidth);

		float3 bounceLighting;
		bounceLighting.r = SphericalHarmonics::Unproject(sunBounceSH.R, -normal);
		bounceLighting.g = SphericalHarmonics::Unproject(sunBounceSH.G, -normal);
		bounceLighting.b = SphericalHarmonics::Unproject(sunBounceSH.B, -normal);

		bounceLighting = max(0, bounceLighting);
#					if defined(SKYLIGHTING)
		float3 bouncedSkylighting = saturate(MultiBounceAO(SharedData::pseudoSunBounceSettings.groundAlbedo, skylightingDiffuse) * skylightingDiffuse);
		bounceLighting *= bouncedSkylighting;
		specularBounce *= bouncedSkylighting;
#					endif

		directionalAmbientColor += bounceLighting * SharedData::pseudoSunBounceSettings.intensity;
	}
#				endif

	directionalAmbientColor *= diffuseLobeWeight * canopyVisibility;
	diffuseColor += directionalAmbientColor;

#				if defined(SKYLIGHTING)
	Skylighting::ApplySkylighting(diffuseColor, directionalAmbientColor, diffuseLobeWeight, skylightingDiffuse);
#				endif

	[unroll] for (uint side = 0; side < 2; ++side)
	{
		float3 transmissionAlbedo = (side == 0 ? frontTransmissionAlbedo : backTransmissionAlbedo) * canopyVisibility;
		[branch] if (any(transmissionAlbedo > 0.0))
		{
			float3 transmissionNormal = side == 0 ? normal : -normal;
			float3 ambientColor = Color::Ambient(max(0, SharedData::GetAmbient(transmissionNormal)));
			float3 transmittedAmbientColor;
			float3 indirectTransmissionColor = GetThinSurfaceTransmissionLighting(ambientColor, transmissionAlbedo, transmissionNormal,
				input.WorldPosition.xyz, vertexAO, grassOpacity, transmittedAmbientColor
#				if defined(SKYLIGHTING)
				,
				side == 0 ? skylightingSH : backSkylightingSH
#				endif
			);
			diffuseColor += indirectTransmissionColor;
			directionalAmbientColor += transmittedAmbientColor;
		}
	}

	diffuseColor += transmissionRadiance;

	specularColor += lightsSpecularColor;
#				if defined(VANILLA_FRESNEL)
	if (!(SharedData::vanillaFresnelSettings.Enable && SharedData::vanillaFresnelSettings.EnableGGXOnGrass))
#				endif
		specularColor *= specColor.w * SharedData::grassLightingSettings.SpecularStrength;
#				if defined(WETNESS_EFFECTS)
	specularColor += wetnessDirectSpecularColor;
#				endif

#				if defined(LIGHT_LIMIT_FIX) && defined(LLFDEBUG)
	if (SharedData::lightLimitFixSettings.EnableLightsVisualisation) {
		if (SharedData::lightLimitFixSettings.LightsVisualisationMode == 0) {
			diffuseColor.xyz = Color::TurboColormap(0);
		} else if (SharedData::lightLimitFixSettings.LightsVisualisationMode == 1) {
			diffuseColor.xyz = Color::TurboColormap(0);
		} else {
			diffuseColor.xyz = Color::TurboColormap((float)lightCount / MAX_CLUSTER_LIGHTS);
		}
	} else {
		psout.Diffuse = float4(diffuseColor, 1);
	}
#				else
	psout.Diffuse.xyz = FogNearColor.w * diffuseColor;
#				endif

	float3 outputNormal = normal;
	float outputRoughness = roughness;
#				if defined(WETNESS_EFFECTS)
	WetnessEffects::ApplySurfaceWetnessOutput(wetnessState, outputNormal, outputRoughness);
#				endif
	float3 normalVS = normalize(FrameBuffer::WorldToView(outputNormal, false));
#				if defined(WETNESS_EFFECTS)
	psout.Reflectance = float4(reflectance + wetnessReflectance, 1);
#				else
	psout.Reflectance = float4(reflectance, 1);
#				endif
	psout.Albedo = float4(diffuseLobeWeight, 1);
	psout.NormalGlossiness = float4(GBuffer::EncodeNormal(normalVS), 1.0 - outputRoughness, 1);

#				if defined(PSEUDO_SUN_BOUNCE)
	if (!SharedData::InInterior && SharedData::pseudoSunBounceSettings.intensity > 0.0) {
		specularColor += specularBounce * reflectance;
#					if defined(WETNESS_EFFECTS)
		specularColor += specularBounce * wetnessReflectance;
#					endif
	}
#				endif

	psout.Specular = float4(specularColor, 1);
	psout.Masks = float4(0, 0, Color::RGBToYCoCg(directionalAmbientColor).x, 0);
	psout.Masks2 = float4(1.0 - vertexAO, 0, 0, 0);
#			endif
#			if !defined(RENDER_DEPTH)
	psout.Diffuse.rgb = PreExposure::EncodeScene(psout.Diffuse.rgb);
#				if defined(GRASS_LIGHTING)
	psout.Specular.rgb = PreExposure::EncodeScene(psout.Specular.rgb);
#				endif
#			endif
	return psout;
}
#		endif  // TRUE_PBR
#	else
PS_OUTPUT main(PS_INPUT input)
{
	PS_OUTPUT psout;
#		if defined(DCLF_PULLED)
	DCLFGrassPixelStatics(input.Bucket);
#		endif

#		if defined(SKYLIGHTING_SHADOW_VIS)
	float skylightingShadowVisibility = 1.0;
#		endif

#		if defined(RENDER_DEPTH)
	const float baseAlpha = TexBaseSampler.SampleBias(SampBaseSampler, input.TexCoord.xy, SharedData::MipBias).w;
	const float diffuseAlpha = input.Fade * baseAlpha;
	if ((diffuseAlpha - AlphaTestRefRS) < 0) {
		discard;
	}

#			ifdef GRASS_OPTIMIZATIONS
	psout.PS.xyz = input.HPosition.zzz;
#			else
	psout.PS.xyz = input.Depth.xxx / input.Depth.yyy;
#			endif
	psout.PS.w = diffuseAlpha;
#		else
	float4 baseColor = TexBaseSampler.SampleBias(SampBaseSampler, input.TexCoord.xy, SharedData::MipBias);
	baseColor.xyz = Color::Albedo(ColorManagement::DiffuseToWorking(baseColor.xyz));
#			if defined(DO_ALPHA_TEST)
	const float diffuseAlpha = input.Color.w * baseColor.w;
	if ((diffuseAlpha - AlphaTestRefRS) < 0)
		discard;
#			endif

#			ifdef GRASS_CULLED_INSTANCES
	const float lodBrightness = input.LodTier > 1.5 ? SharedData::grassLightingSettings.FarLODBrightness : SharedData::grassLightingSettings.MidLODBrightness;
	baseColor.xyz *= lerp(1.0, lodBrightness, saturate(input.LodTier));
#			endif

	if (SharedData::lodBlendingSettings.DisableTerrainVertexColors)
		input.Color.xyz = 1;

	float3 viewPosition = mul(FrameBuffer::CameraView, float4(input.WorldPosition.xyz, 1)).xyz;
	float2 screenUV = FrameBuffer::ViewToUV(viewPosition);
	float screenNoise = Random::InterleavedGradientNoise(input.HPosition.xy, SharedData::FrameCount);

	float4 shadowColor = TexShadowMaskSampler.Load(int3(input.HPosition.xy, 0));

	float3 dirLightColor = Color::DirectionalLight(SharedData::DirLightColor.xyz);

	// Apply world shadow (terrain shadows, cloud shadows) directly to light color
	if (!SharedData::InInterior)
		dirLightColor *= ShadowSampling::GetWorldShadow(input.WorldPosition.xyz, FrameBuffer::CameraPosAdjust.xyz);
#			if defined(WATER_EFFECTS)
	dirLightColor *= WaterEffects::ComputeCaustics(SharedData::GetWaterData(input.WorldPosition.xyz), input.WorldPosition.xyz);
#			endif

	float dirDetailedShadow = 1.0;

	if (!SharedData::InInterior)
		dirDetailedShadow = shadowColor.x;

#			if defined(SCREEN_SPACE_SHADOWS)
#				ifdef GRASS_CULLED_INSTANCES
	if (!SharedData::InInterior && input.IsFar <= 0.5)
#				else
	if (!SharedData::InInterior)
#				endif
		dirDetailedShadow *= ScreenSpaceShadows::GetScreenSpaceShadow(input.HPosition.xyz, screenUV, screenNoise);
#			endif  // SCREEN_SPACE_SHADOWS

	float3 diffuseColor = dirLightColor * dirDetailedShadow * input.DirLightAngle;

#			if defined(LIGHT_LIMIT_FIX)
	uint clusterIndex = 0;
	uint lightCount = 0;

	if (LightLimitFix::GetClusterIndex(screenUV, viewPosition.z, clusterIndex)) {
		lightCount = LightLimitFix::lightGrid[clusterIndex].lightCount;
		if (lightCount) {
			uint lightOffset = LightLimitFix::lightGrid[clusterIndex].offset;

			[loop] for (uint i = 0; i < lightCount; i++)
			{
				uint clusteredLightIndex = LightLimitFix::lightList[lightOffset + i];
				LightLimitFix::Light light = LightLimitFix::lights[clusteredLightIndex];

				float3 lightDirection = light.positionWS.xyz - input.WorldPosition.xyz;
				float lightDist = length(lightDirection);

				float intensityMultiplier = LightLimitFix::GetAttenuation(lightDist, light);
				if (intensityMultiplier < 1e-5)
					continue;

				float3 lightColor = Color::PointLight(light.color.xyz) * intensityMultiplier * light.fade;

				float lightShadow = 1.0;

				float shadowComponent = 1.0;
				[branch] if (light.lightFlags & LightLimitFix::LightFlags::LocalShadow)
				{
					shadowComponent = LightLimitFix::GetLocalShadow(LinearSampler, light.localShadowIndex, input.WorldPosition.xyz, FrameBuffer::CameraPosAdjust.xyz, float3(0.0, 0.0, 0.0), false, screenNoise);
					lightShadow *= shadowComponent;
				}
				else if (light.lightFlags & LightLimitFix::LightFlags::Shadow)
				{
					shadowComponent = shadowColor[light.shadowLightIndex];
					lightShadow *= shadowComponent;
				}

				lightColor *= lightShadow;

				diffuseColor += lightColor;
			}
		}
	}
#			endif  // LIGHT_LIMIT_FIX

#			ifdef GRASS_CULLED_INSTANCES
	float3 ddx = ddx_coarse(viewPosition);
	float3 ddy = ddy_coarse(viewPosition);
#			else
	float3 ddx = ddx_coarse(input.ViewSpacePosition);
	float3 ddy = ddy_coarse(input.ViewSpacePosition);
#			endif
	float3 normalVS = -normalize(cross(ddx, ddy));
	float3 normal = normalize(FrameBuffer::ViewToWorld(normalVS, false));

	float3 vertexColor = ColorManagement::SRGBToWorking(input.Color.xyz);
	float vertexAO = ColorManagement::SRGBToWorking(max(max(input.Color.r, input.Color.g), input.Color.b));
	vertexColor /= max(vertexAO, EPSILON_DIVISION);

#			if defined(SKYLIGHTING)
	float3 positionMSSkylight = input.WorldPosition.xyz;
	sh2 skylightingSH = Skylighting::Sample(positionMSSkylight, normal
#				if defined(SKYLIGHTING_SHADOW_VIS)
		,
		skylightingShadowVisibility, false
#				endif
	);
	float skylightingDiffuse = Skylighting::GetSkylightingDiffuse(skylightingSH, positionMSSkylight, normal, vertexAO);
#			endif  // SKYLIGHTING

	float3 directionalAmbientColor = Color::Ambient(max(0, AmbientColor.xyz));

#			if defined(IBL)
	if (SharedData::iblSettings.EnableIBL)
		directionalAmbientColor = ImageBasedLighting::GetDiffuseIBL(directionalAmbientColor, -normal, input.WorldPosition.xyz);
#			endif

	float3 albedo = baseColor.xyz * vertexColor;

	diffuseColor += directionalAmbientColor;

	diffuseColor *= albedo;
	directionalAmbientColor *= albedo;

#			if defined(SKYLIGHTING)
	Skylighting::ApplySkylighting(diffuseColor, directionalAmbientColor, albedo, skylightingDiffuse);
#			endif

	psout.Diffuse.xyz = FogNearColor.w * diffuseColor;

	psout.Diffuse.w = 1;

#			if !defined(DEFERRED) && defined(PHYSICAL_SKY)
	if (SharedData::physSkyData.enabled) {
		const bool inReflection = (Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::InReflection) != 0;
		const float3 physSkyViewDir = normalize(input.WorldPosition.xyz);
		if (inReflection)
			psout.Diffuse.xyz = PhysSky::CompositeAerialPerspectiveReflection(psout.Diffuse.xyz, physSkyViewDir, length(input.WorldPosition.xyz), SampColorSampler);
		else
			psout.Diffuse.xyz = PhysSky::CompositeAerialPerspective(psout.Diffuse.xyz, physSkyViewDir, input.Position.xy, screenUV, length(input.WorldPosition.xyz), SampColorSampler);
	}
#			endif

	psout.MotionVectors = MotionBlur::GetSSMotionVector(float4(input.WorldPosition, 1), float4(input.PreviousWorldPosition, 1));
	psout.Normal.xy = GBuffer::EncodeNormal(normalVS);
	psout.Normal.zw = 0;

	psout.Albedo = float4(albedo, 1);
	psout.Masks = float4(0, 0, Color::RGBToYCoCg(directionalAmbientColor).x, 0);
	psout.Masks2 = float4(1.0 - vertexAO, 0, 0, 0);
#		endif

#		if !defined(RENDER_DEPTH)
	psout.Diffuse.rgb = PreExposure::EncodeScene(psout.Diffuse.rgb);
#			if defined(GRASS_LIGHTING)
	psout.Specular.rgb = PreExposure::EncodeScene(psout.Specular.rgb);
#			endif
#		endif
	return psout;
}
#	endif

#endif  // PSHADER
