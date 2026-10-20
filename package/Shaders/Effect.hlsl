// SPDX-License-Identifier: MIT AND (LicenseRef-GPL-3.0-with-Modding-Exception WITH GPL-3.0-linking-source-exception)
// Original code Copyright (c) 2022 Ilya Perapechka (MIT), modifications GPL-3.0 with Modding Exception (see EXCEPTIONS.md)

#include "Common/ColorManagement.hlsli"
#include "Common/DCLFObjects.hlsli"
#include "Common/FrameBuffer.hlsli"
#include "Common/GBuffer.hlsli"
#include "Common/Math.hlsli"
#include "Common/MotionBlur.hlsli"
#include "Common/Permutation.hlsli"
#include "Common/Random.hlsli"
#include "Common/SharedData.hlsli"
#include "Common/Skinned.hlsli"
#define EFFECT

#if defined(SOFT) && defined(NORMALS) && defined(TEXTURE) && defined(FALLOFF) && defined(VC) && \
	!defined(LIGHTING) && !defined(PARTICLES) && !defined(STRIP_PARTICLES) &&                   \
	!defined(BLOOD) && !defined(MEMBRANE) && !defined(ADDBLEND) && !defined(MULTBLEND) &&       \
	!defined(MULTBLEND_DECAL) && !defined(ALPHA_TEST) && !defined(DEFERRED) && !defined(SKINNED)
#	define IS_VOLUMETRIC_FOG
#endif

#if !defined(DYNAMIC_CUBEMAPS) && defined(IBL)
#	undef IBL
#endif

#if defined(DCLF_BINDLESS) && defined(PARTICLES)
// Drawcall Limit Fix (T7e; ParticleSnapshots.h): a particle system's quads are pulled from its block of the frame's palettes by the vertex's
// index in the renderer's quad index list (DCLFParticleInput), each particle's subtexture rect resolved: no vertex stream, no IndexedTexCoord.
#	define DCLF_PARTICLE_PULL
#endif

#if defined(VSHADER)
struct VS_INPUT
{
	float4 Position: POSITION0;
#	if defined(TEXCOORD)
#		if defined(STRIP_PARTICLES)
	float3
#		else
	float2
#		endif
		TexCoord0: TEXCOORD0;
#	endif
#	if defined(NORMALS) || defined(MOTIONVECTORS_NORMALS)
	float4 Normal: NORMAL0;
#	endif
#	if defined(BINORMAL_TANGENT)
	float4 Bitangent: BINORMAL0;
#	endif
#	if defined(VC)
	float4 Color: COLOR0;
#	endif
#	if defined(SKINNED)
	float4 BoneWeights: BLENDWEIGHT0;
	float4 BoneIndices: BLENDINDICES0;
#	endif
};
#endif

struct VS_OUTPUT
{
	float4 Position: SV_POSITION0;
	float4 TexCoord0: TEXCOORD0;
	float4 WorldPosition: POSITION1;
#if defined(VC)
	float4 Color: COLOR0;
#endif
#if !defined(MOTIONVECTORS_NORMALS)
	float4 FogParam: COLOR1;
#endif
#if defined(MOTIONVECTORS_NORMALS) && defined(MEMBRANE) && !defined(SKINNED) && defined(NORMALS)
	float3 ScreenSpaceNormal: TEXCOORD1;
#elif (defined(MEMBRANE) && (defined(SKINNED) || defined(NORMALS))) || (defined(PROJECTED_UV) && defined(NORMALS))
	float3 TBN0: TEXCOORD1;
#endif
#if defined(MEMBRANE) && (defined(SKINNED) || defined(NORMALS))
	float FogAlpha: TEXCOORD5;
#endif
#if (defined(MEMBRANE) && defined(SKINNED) && !defined(NORMALS)) || (defined(PROJECTED_UV) && defined(NORMALS) && !defined(MEMBRANE))
	float3 TBN1: TEXCOORD2;
#endif
#if (defined(MEMBRANE) && defined(SKINNED) && !defined(NORMALS))
	float3 TBN2: TEXCOORD3;
#endif
#if defined(MEMBRANE)
	float4 ViewVector: TEXCOORD4;
#endif
#if defined(LIGHTING)
	float3 MSPosition: TEXCOORD6;
#endif
#if !(defined(MEMBRANE) && (defined(SKINNED) || defined(NORMALS)))
	float FogAlpha: TEXCOORD5;
#endif
#if defined(MOTIONVECTORS_NORMALS)
#	if !defined(LIGHTING) && !(defined(MEMBRANE) && defined(SKINNED)) && !(defined(MEMBRANE) && !defined(SKINNED) && defined(NORMALS))
	float3 ScreenSpaceNormal: TEXCOORD7;
#	endif
	float4 PreviousWorldPosition: POSITION2;
#	if (defined(LIGHTING) || (defined(MEMBRANE) && defined(SKINNED))) && !(defined(MEMBRANE) && defined(NORMALS))
	float3 ScreenSpaceNormal: TEXCOORD7;
#	endif
#endif
#if defined(DCLF_PULLED)
	// Drawcall Limit Fix's plain draws (DCLF_PULLED; Lighting.hlsl's): the draw's instance, by which the pixel stage reads its sequence.
	nointerpolation uint DCLFDrawInstance: TEXCOORD15;
#endif
};

#ifdef VSHADER
cbuffer VS_PerFrame : register(b12)
{
	row_major float4x4 ScreenProj : packoffset(c0);
	row_major float4x4 ViewProj : packoffset(c8);
#	if defined(SKINNED) || defined(DCLF_BINDLESS)
	// posAdjust and its previous value: the skinning pivot, and under DCLF_BINDLESS also the eye the absolute placement rows are made
	// relative to (as Lighting.hlsl's).
	float3 BonesPivot : packoffset(c40);
#		if defined(MOTIONVECTORS_NORMALS) || defined(DCLF_BINDLESS)
	float3 PreviousBonesPivot : packoffset(c41);
#		endif  // MOTIONVECTORS_NORMALS || DCLF_BINDLESS
#	endif      // SKINNED || DCLF_BINDLESS
};

#	if defined(DCLF_BINDLESS)
// Drawcall Limit Fix's draws (T7d's Effect port: dclf-architecture.md, "The Effect shader in the forward epoch"): the fog is the
// frame's, the same block every DCLF_BINDLESS draw reads (Lighting.hlsl, DCLFFrameFog: SetupTechnique writes the effect's fog exactly
// as the Lighting technique's), so no pipeline row holds it.
cbuffer DCLFFrameFog : register(b13)
{
	float4 FogParam : packoffset(c0);
	float4 FogNearColor : packoffset(c1);
	float4 FogFarColor : packoffset(c2);
};
#	else
cbuffer PerTechnique : register(b0)
{
	float4 FogParam : packoffset(c0);
	float4 FogNearColor : packoffset(c1);
	float4 FogFarColor : packoffset(c2);
};
#	endif  // DCLF_BINDLESS

cbuffer PerMaterial : register(b1)
{
	float4 TexcoordOffset : packoffset(c0);
	float4 SoftMateralVSParams : packoffset(c1);
	float4 FalloffData : packoffset(c2);
};

cbuffer PerGeometry : register(b2)
{
#	if !defined(DCLF_BINDLESS)
	row_major float3x4 World : packoffset(c0);
	row_major float3x4 PreviousWorld : packoffset(c3);
#	endif  // !DCLF_BINDLESS
	float4 MatProj[3] : packoffset(c6);
	float4 EyePosition : packoffset(c12);
	float4 PosAdjust : packoffset(c13);
	float4 TexcoordOffsetMembrane : packoffset(c14);
}

#	if !defined(DCLF_PARTICLE_PULL)
cbuffer IndexedTexcoordBuffer : register(b11)
{
	float4 IndexedTexCoord[128] : packoffset(c0);
}
#	endif  // !DCLF_PARTICLE_PULL

#	if defined(DCLF_BINDLESS)
// World and PreviousWorld are the object's placement row's (absolute), made relative to this draw's eye and previous eye, the
// subtraction SetupGeometry does on the CPU for the engine's own draws (Lighting.hlsl, DCLF_VS_WORLD).
// CS_DCLF_BILLBOARDS (Billboards.h): a billboard's row is its unturned world and its node's base and pivot, turned here to this
// draw's camera as NiBillboardNode::OnVisible turns the node for the culling camera (DCLFObjects.hlsli, DCLFBillboardWorld): the camera's
// axes are CameraView's rows (c0-c2, as SetCameraData copies the camera's world rotation: right, up, forward) and its eye the posAdjust
// above. Its previous world is the same turn (the row holds no previous base): no motion of its own.
#		if defined(DCLF_PULLED)
// A plain draw's object word is its sequence's, which main reads first: these are assigned there (DCLFVertexObjectStatics).
static bool DCLFBillboard;
#		else
static const bool DCLFBillboard = (DCLFObjects[DCLFObjectIndex].DCLFRecordFlags & kDCLFRecordEffectBillboard) != 0;
#		endif
// The billboard's turned world for this draw's camera (absolute).
float3x4 DCLFEffectBillboardWorld() { return DCLFBillboardWorld(DCLFObjectIndex, ScreenProj[2].xyz, ScreenProj[1].xyz, ScreenProj[0].xyz, BonesPivot); }
float3x4 DCLFEyeRelative(float3x4 a_world, float3 a_eye)
{
	return float3x4(a_world[0] - float4(0, 0, 0, a_eye.x), a_world[1] - float4(0, 0, 0, a_eye.y), a_world[2] - float4(0, 0, 0, a_eye.z));
}
// The placement's World or PreviousWorld (absolute).
float3x4 DCLFPlacementWorld(bool a_previous)
{
	const DCLFPlacement placement = DCLFPlacements[DCLFObjectIndex];
	return a_previous ? float3x4(placement.PreviousWorld[0], placement.PreviousWorld[1], placement.PreviousWorld[2]) :
	                    float3x4(placement.World[0], placement.World[1], placement.World[2]);
}
float3x4 DCLFEffectWorld(bool a_previous)
{
	const float3 eye = a_previous ? PreviousBonesPivot : BonesPivot;
	if (DCLFBillboard)
		return DCLFEyeRelative(DCLFEffectBillboardWorld(), eye);
	return DCLFEyeRelative(DCLFPlacementWorld(a_previous), eye);
}
#		if defined(DCLF_PULLED)
static precise float3x4 World;
static precise float3x4 PreviousWorld;
void DCLFVertexObjectStatics()
{
	DCLFBillboard = (DCLFObjects[DCLFObjectIndex].DCLFRecordFlags & kDCLFRecordEffectBillboard) != 0;
	if (DCLFBillboard) {
		// One turn for both (DCLFEffectWorld's), made relative to each eye.
		const float3x4 world = DCLFEffectBillboardWorld();
		World = DCLFEyeRelative(world, BonesPivot);
		PreviousWorld = DCLFEyeRelative(world, PreviousBonesPivot);
	} else {
		World = DCLFEyeRelative(DCLFPlacementWorld(false), BonesPivot);
		PreviousWorld = DCLFEyeRelative(DCLFPlacementWorld(true), PreviousBonesPivot);
	}
}
#		else
static precise float3x4 World = DCLFEffectWorld(false);
static precise float3x4 PreviousWorld = DCLFEffectWorld(true);
#		endif
#	endif  // DCLF_BINDLESS

#	if defined(PROJECTED_UV)
float GetProjectedU(float3 worldPosition, float4 texCoordOffset)
{
	float projUvTmp = min(abs(worldPosition.x), abs(worldPosition.y)) *
	                  (1 / max(abs(worldPosition.x), abs(worldPosition.y)));
	float projUvTmpSqr = projUvTmp * projUvTmp;
	float projUvTmp2 =
		projUvTmpSqr *
			(projUvTmpSqr *
					(projUvTmpSqr * (projUvTmpSqr * 0.0208350997 + -0.0851330012) + 0.180141002) +
				-0.330299497) +
		0.999866009;
	float projUvTmp5;
	if (abs(worldPosition.x) > abs(worldPosition.y)) {
		projUvTmp5 = projUvTmp * projUvTmp2 * -2 + Math::HALF_PI;
	} else {
		projUvTmp5 = 0;
	}
	float projUvTmp6 = projUvTmp * projUvTmp2 + projUvTmp5;
	float projUvTmp7;
	if (worldPosition.y < -worldPosition.y) {
		projUvTmp7 = -Math::PI;
	} else {
		projUvTmp7 = 0;
	}
	float projUvTmp4 = projUvTmp6 + projUvTmp7;
	float minCoord = min(worldPosition.x, worldPosition.y);
	float maxCoord = max(worldPosition.x, worldPosition.y);
	if (minCoord < -minCoord && maxCoord >= -maxCoord) {
		projUvTmp4 = -projUvTmp4;
	}
	return abs(0.318309158 * projUvTmp4) * texCoordOffset.w + texCoordOffset.y;
}

float GetProjectedV(float3 worldPosition)
{
	return (-PosAdjust.x + (PosAdjust.z + worldPosition.z)) / PosAdjust.y;
}
#	endif

#	if defined(DCLF_PARTICLE_PULL)
struct VS_PARTICLE_INPUT
{
	uint VertexID: SV_VertexID;
};

// The object's block (DCLFBoneOffset; ParticleSnapshots::Expand writes it each frame, five rows a particle, back to front): the particle's
// four corners - the position relative to the model bound's centre (the placement's translation carries it, as SetupGeometry's World does)
// and the colour packed as FUN_140e51320 packs it, red lowest - then its subtexture rect, which the engine's vertex stage applies to the
// corner's texture coordinates from its static stream: (0, 1), (1, 1), (1, 0), (0, 0).
VS_INPUT DCLFParticleInput(uint a_vertex)
{
	const uint quad = a_vertex >> 2;
	const uint corner = a_vertex & 3;
	const uint row = DCLFObjects[DCLFObjectIndex].DCLFBoneOffset + quad * 5;
	const float4 vertex = DCLFPalettes[row + corner];
	VS_INPUT input = (VS_INPUT)0;
	input.Position = float4(vertex.xyz, 0);
#		if defined(TEXCOORD)
	const float4 rect = DCLFPalettes[row + 4];
	const float2 corners = float2(corner == 1 || corner == 2 ? 1.0 : 0.0, corner < 2 ? 1.0 : 0.0);
	input.TexCoord0 = float2(rect.y * corners.x + rect.x, rect.w * corners.y + rect.z);
#		endif
#		if defined(VC)
	// R8G8B8A8_UNORM as the input assembler converts it (DCLFObjects.hlsli's DCLFUnorm4: c / 255 correctly rounded).
	const uint packed = asuint(vertex.w);
	precise float4 c = float4(packed & 0xFF, (packed >> 8) & 0xFF, (packed >> 16) & 0xFF, packed >> 24);
	precise float4 q = c * (1.0 / 255.0);
	precise float4 r = mad(-q, 255.0, c);
	input.Color = mad(r, 1.0 / 255.0, q);
#		endif
	return input;
}
#	endif  // DCLF_PARTICLE_PULL

// Drawcall Limit Fix's builds have no input assembly: main pulls the vertex (DCLFParticleInput; DCLFPulledInput, below) and shades
// it here.
#	if defined(DCLF_PARTICLE_PULL) || defined(DCLF_PULLED_VERTICES) || defined(DCLF_PULLED)
VS_OUTPUT DCLFShade(VS_INPUT input)
#	else
VS_OUTPUT main(VS_INPUT input)
#	endif
{
	VS_OUTPUT vsout;
	precise float4 inputPosition = float4(input.Position.xyz, 1.0);

	precise row_major float4x4 world4x4 = float4x4(World[0], World[1], World[2], float4(0, 0, 0, 1));
	precise float3x3 world3x3 =
		transpose(float3x3(transpose(World)[0], transpose(World)[1], transpose(World)[2]));

#	if defined(SKY_OBJECT)
	float4x4 viewProj = float4x4(ViewProj[0], ViewProj[1], ViewProj[3], ViewProj[3]);
#	else
	row_major float4x4 viewProj = ViewProj;
#	endif

#	if defined(SKINNED)
	precise int4 actualIndices = 765.01.xxxx * input.BoneIndices.xyzw;
	// Drawcall Limit Fix (T7d part 5, skinned effects): the palette rows are the object's in DCLFPalettes, at its record's offsets (as
	// Lighting.hlsl's SKINNED path under DCLF_BINDLESS), not the bone constant buffers.
#		if defined(MOTIONVECTORS_NORMALS)
#			if defined(DCLF_BINDLESS)
	float3x4 previousBoneTransformMatrix =
		Skinned::GetBoneTransformMatrixBindless(DCLFObjects[DCLFObjectIndex].DCLFPreviousBoneOffset, actualIndices, PreviousBonesPivot, input.BoneWeights);
#			else
	float3x4 previousBoneTransformMatrix =
		Skinned::GetBoneTransformMatrix(PreviousBones, actualIndices, PreviousBonesPivot, input.BoneWeights);
#			endif  // DCLF_BINDLESS
	precise float4 previousWorldPosition =
		float4(mul(inputPosition, transpose(previousBoneTransformMatrix)), 1);
#		endif
#		if defined(DCLF_BINDLESS)
	float3x4 boneTransformMatrix =
		Skinned::GetBoneTransformMatrixBindless(DCLFObjects[DCLFObjectIndex].DCLFBoneOffset, actualIndices, BonesPivot, input.BoneWeights);
#		else
	float3x4 boneTransformMatrix =
		Skinned::GetBoneTransformMatrix(Bones, actualIndices, BonesPivot, input.BoneWeights);
#		endif  // DCLF_BINDLESS
	precise float4 worldPosition = float4(mul(inputPosition, transpose(boneTransformMatrix)), 1);
	float4 viewPos = mul(viewProj, worldPosition);
#	else
	precise float4 worldPosition = float4(mul(World, inputPosition), 1);
	precise float4 previousWorldPosition = float4(mul(PreviousWorld, inputPosition), 1);
	precise row_major float4x4 modelView = mul(viewProj, world4x4);
	float4 viewPos = mul(modelView, inputPosition);
#	endif

	vsout.Position = viewPos;
#	if defined(DCLF_BINDLESS)
	// A decal's depth toward the eye (DCLFObjects.hlsli, DCLFDecalNudged): the engine's decal bias, which DCLF's draws cannot carry.
	vsout.Position = DCLFDecalNudged(vsout.Position, DCLFObjectIndex);
#	endif

#	if defined(SKINNED)
#		if defined(DCLF_BINDLESS)
	float3x3 boneRSMatrix = Skinned::GetBoneRSMatrixBindless(DCLFObjects[DCLFObjectIndex].DCLFBoneOffset, actualIndices, input.BoneWeights);
#		else
	float3x3 boneRSMatrix = Skinned::GetBoneRSMatrix(Bones, actualIndices, input.BoneWeights);
#		endif  // DCLF_BINDLESS
	float3x3 boneRSMatrixTr = transpose(boneRSMatrix);
#	endif

#	if defined(NORMALS) || defined(MOTIONVECTORS_NORMALS)
	float3 normal = input.Normal.xyz * 2 - 1;

#		if defined(SKINNED)
	float3 worldNormal = normalize(mul(normal, boneRSMatrixTr));
#		else
	float3 worldNormal = normalize(mul(world3x3, normal));
#		endif
#	endif

#	if defined(VC)
	vsout.Color = input.Color;
#	endif

#	if !defined(MOTIONVECTORS_NORMALS)
	float fogColorParam = min(FogParam.w,
		exp2(FogParam.z * log2(saturate(length(viewPos.xyz) * FogParam.y - FogParam.x))));

	vsout.FogParam.xyz = lerp(ColorManagement::SRGBToWorking(FogNearColor.xyz), ColorManagement::SRGBToWorking(FogFarColor.xyz), fogColorParam);
	vsout.FogParam.w = fogColorParam;
#	endif

	float4 texCoord = float4(0, 0, 1, 0);

#	if defined(MEMBRANE)
	float4 texCoordOffset = TexcoordOffsetMembrane;
#	else
	float4 texCoordOffset = TexcoordOffset;
#	endif

#	if defined(TEXCOORD_INDEX) && !defined(DCLF_PARTICLE_PULL)
#		if defined(NORMALS)
	uint index = input.TexCoord0.z;
#		else
	uint index = input.Position.w;
#		endif
#	endif

#	if defined(PROJECTED_UV)
#		if defined(NORMALS) && !defined(MEMBRANE)
	texCoord.x = dot(MatProj[0].xyz, inputPosition.xyz);
#		else
	texCoord.x = GetProjectedU(worldPosition.xyz, texCoordOffset);
#		endif
#	else
#		if defined(TEXTURE)
	float u = input.TexCoord0.x;
#			if defined(TEXCOORD_INDEX) && !defined(DCLF_PARTICLE_PULL)
	u = IndexedTexCoord[index].y * u + IndexedTexCoord[index].x;
#			endif
	texCoord.x = u * texCoordOffset.z + texCoordOffset.x;
#		endif
#	endif
#	if defined(PROJECTED_UV)
#		if defined(NORMALS) && !defined(MEMBRANE)
	texCoord.y = dot(MatProj[1].xyz, inputPosition.xyz);
#		else
	texCoord.y = GetProjectedV(worldPosition.xyz);
#		endif
#	else
#		if defined(TEXTURE)
	float v = input.TexCoord0.y;
#			if defined(TEXCOORD_INDEX) && !defined(DCLF_PARTICLE_PULL)
	v = IndexedTexCoord[index].w * v + IndexedTexCoord[index].z;
#			endif
	texCoord.y = v * texCoordOffset.w + texCoordOffset.y;
#		endif
#	endif
#	if defined(PROJECTED_UV) && !defined(NORMALS)
	texCoord.w = input.TexCoord0.y;
#	elif defined(SOFT)
	texCoord.w = viewPos.w / SoftMateralVSParams.x;
#	elif defined(MEMBRANE) && (!defined(NORMALS) || defined(ALPHA_TEST))
	texCoord.w = input.TexCoord0.y;
#	endif
#	if defined(PROJECTED_UV) && !defined(NORMALS)
	texCoord.z = input.TexCoord0.x;
#	elif defined(FALLOFF)
	float3 inverseWorldDirection = normalize(-worldPosition.xyz);
	float WdotN = dot(worldNormal, inverseWorldDirection);
	float falloff = saturate((-FalloffData.x + abs(WdotN)) / (FalloffData.y - FalloffData.x));
	float falloffParam = (falloff * falloff) * (3 - falloff * 2);
	texCoord.z = lerp(FalloffData.z, FalloffData.w, falloffParam);
#	elif defined(MEMBRANE) && (!defined(NORMALS) || defined(ALPHA_TEST))
	texCoord.z = input.TexCoord0.x;
#	endif
	vsout.TexCoord0 = texCoord;

	float3 eyePosition = 0.0.xxx;
#	if defined(MEMBRANE) && defined(TEXTURE) && !defined(SKINNED)
	eyePosition = EyePosition.xyz;
#	endif

	float3 viewPosition = inputPosition.xyz;
#	if defined(SKINNED)
	viewPosition = worldPosition.xyz;
#	endif

#	if defined(MEMBRANE)
#		if defined(SKINNED)
#			if defined(NORMALS)
	vsout.TBN0.xyz = worldNormal;
#			else
	float3x3 tbnTr = float3x3(normalize(boneRSMatrixTr[0]), normalize(boneRSMatrixTr[1]),
		normalize(boneRSMatrixTr[2]));
#				if defined(MOTIONVECTORS_NORMALS)
	tbnTr[2] = worldNormal;
#				endif
	float3x3 tbn = transpose(tbnTr);
	vsout.TBN0.xyz = tbn[0];
	vsout.TBN1.xyz = tbn[1];
	vsout.TBN2.xyz = tbn[2];
#			endif
#		endif

	vsout.ViewVector.xyz = normalize(eyePosition - viewPosition);
	vsout.ViewVector.w = 1;
#	endif

#	if !defined(SKINNED) && defined(NORMALS) && !(defined(MOTIONVECTORS_NORMALS) && defined(MEMBRANE) && !defined(SKINNED) && defined(NORMALS))
#		if defined(MEMBRANE)
	vsout.TBN0.xyz = normal;
#		elif defined(PROJECTED_UV)
	vsout.TBN0.xyz = input.Normal.xyz;
#		endif
#	endif

#	if defined(MOTIONVECTORS_NORMALS) && !(defined(MEMBRANE) && defined(SKINNED) && defined(NORMALS))
#		if defined(SKINNED) && !defined(MEMBRANE)
	float3 screenSpaceNormal = normal;
#		elif defined(FALLOFF) || (defined(SKINNED) && defined(MEMBRANE))
	float3 screenSpaceNormal = worldNormal;
#		else
	float4x4 modelScreen = mul(ScreenProj, world4x4);
	float3 screenSpaceNormal = normalize(mul(modelScreen, float4(normal, 0))).xyz;
#		endif

	vsout.ScreenSpaceNormal = screenSpaceNormal;
#	endif

#	if defined(LIGHTING)
	vsout.MSPosition = viewPosition;
#	endif

	vsout.FogAlpha.x = FogNearColor.w;

#	if defined(PROJECTED_UV) && defined(NORMALS) && !defined(MEMBRANE)
	vsout.TBN1.xyz = MatProj[2].xyz;
#	endif

	vsout.WorldPosition = worldPosition;
#	if defined(MOTIONVECTORS_NORMALS)
	vsout.PreviousWorldPosition = previousWorldPosition;
#	endif

	return vsout;
}

#	if defined(DCLF_PARTICLE_PULL) && !defined(DCLF_PULLED)
// A particle system's quads keep priority over the vertex pull below: they have no vertex stream at all.
VS_OUTPUT main(VS_PARTICLE_INPUT particleInput)
{
	return DCLFShade(DCLFParticleInput(particleInput.VertexID));
}
#	elif defined(DCLF_PULLED_VERTICES) || defined(DCLF_PULLED)
// Drawcall Limit Fix's device-generated draws (Lighting.hlsl's DCLF_PULLED_VERTICES main): the vertex's attributes loaded from the
// geometry's buffers in the draw's push data, decoded as the engine's input layout for the draw's vertex layout has them
// (VertexInput.cpp, BuildVertexElements), the input assembler's defaults where it lacks one.
// The attributes read here must match the mask the pipelines check the geometry's layout against (ShaderPrograms.cpp,
// VertexAttributesOf: Effect's).
VS_INPUT DCLFPulledInput(uint a_index, DCLFStream a_first, DCLFStream a_second)
{
	DCLFStreamsAt(a_index, a_first, a_second);
	VS_INPUT input;
	input.Position = DCLFPullFloat4(0, a_first, a_second);
#		if defined(TEXCOORD)
#			if defined(STRIP_PARTICLES)
	// R16G16_FLOAT read as three components: the input assembler fills z with 0.
	input.TexCoord0 = float3(DCLFPullHalf2(1, a_first, a_second), 0);
#			else
	input.TexCoord0 = DCLFPullHalf2(1, a_first, a_second);
#			endif
#		endif
#		if defined(NORMALS) || defined(MOTIONVECTORS_NORMALS)
	input.Normal = DCLFPullUnorm4(3, 0, a_first, a_second);
#		endif
#		if defined(BINORMAL_TANGENT)
	input.Bitangent = DCLFPullUnorm4(4, 0, a_first, a_second);
#		endif
#		if defined(VC)
	input.Color = DCLFPullUnorm4(5, 0, a_first, a_second);
#		endif
#		if defined(SKINNED)
	input.BoneWeights = DCLFPullHalf4(6, a_first, a_second);
	input.BoneIndices = DCLFPullUnorm4(6, 8, a_first, a_second);
#		endif
	return input;
}

#		if defined(DCLF_PULLED)
// The permutation block (b4) as the draw's pipeline row holds it (Permutation.hlsli, DCLF_PULLED_ROWS; Lighting.hlsl's).
void DCLFPermutationStatics()
{
	Permutation::VertexShaderDescriptor = DCLFRowUint(DCLFPipelineRowAddress, kDCLFPermutationBlock);
	Permutation::PixelShaderDescriptor = DCLFRowUint(DCLFPipelineRowAddress, kDCLFPermutationBlock + 4);
	Permutation::ExtraShaderDescriptor = DCLFRowUint(DCLFPipelineRowAddress, kDCLFPermutationBlock + 8);
	Permutation::ExtraFeatureDescriptor = DCLFRowUint(DCLFPipelineRowAddress, kDCLFPermutationBlock + 12);
	Permutation::EffectRadius = DCLFRowFloat(DCLFPipelineRowAddress, kDCLFPermutationBlock + 16);
}

// Drawcall Limit Fix's plain draws (DCLF_PULLED; ForwardDrawPass): one per pipeline, which cannot push data per draw. So this stage
// reads its draw's sequence (BuildDrawsCS: DrawSequence, at DCLFSequencesAddress plus its instance's stride), and through it the draw's
// object word, rows and geometry (Lighting.hlsl's pulled main); a particle system pulls its quads from its object's block. The rows'
// blocks the pulled source reads (ShaderPrograms.cpp, PulledLightingSource) are assigned first; Effect's PerTechnique is the frame's
// (DCLFFrameFog).
VS_OUTPUT DCLFPulledVertex(uint index, uint a_instance)
{
	const uint64_t sequence = DCLFDrawSequence(a_instance);
	const uint4 rows = vk::RawBufferLoad<uint4>(sequence + 4);  // the pipeline row's address, the material row's
	DCLFObjectWord = vk::RawBufferLoad<uint>(sequence + 20);
	DCLFPipelineRowAddress = DCLFAddress(rows.xy);
	DCLFMaterialRowAddress = DCLFAddress(rows.zw);
	DCLFPerMaterialStatics();
	DCLFPerGeometryStatics();
	DCLFPermutationStatics();
	DCLFVertexObjectStatics();
#			if defined(DCLF_PARTICLE_PULL)
	VS_OUTPUT vsout = DCLFShade(DCLFParticleInput(index));
#			else
	DCLFDrawVertexLayout = DCLFPipelineVertexLayout();
	const uint4 vertexBuffer = vk::RawBufferLoad<uint4>(sequence + 24);  // address, size, stride
	const uint4 streamBuffer = vk::RawBufferLoad<uint4>(sequence + 40);
	DCLFStream first, second;
	first.address = DCLFAddress(vertexBuffer.xy);
	first.stride = vertexBuffer.w;
	second.address = DCLFAddress(streamBuffer.xy);
	second.stride = streamBuffer.w;
	VS_OUTPUT vsout = DCLFShade(DCLFPulledInput(index, first, second));
#			endif
	vsout.DCLFDrawInstance = a_instance;
	return vsout;
}

// The draw's lookups keyed on a wave-uniform instance (Lighting.hlsl's pulled main): its rows, statics, object and billboard turn are
// the draw's (scalar), not each lane's.
VS_OUTPUT main(uint index : SV_VertexID, uint a_instance : SV_InstanceID)
{
	VS_OUTPUT vsout;
	[loop] for (;;) {
		const uint drawInstance = WaveReadLaneFirst(a_instance);
		[branch] if (drawInstance == a_instance) {
			vsout = DCLFPulledVertex(index, drawInstance);
			break;
		}
	}
	return vsout;
}
#		else
VS_OUTPUT main(uint index : SV_VertexID)
{
	DCLFDrawVertexLayout = DCLFPushedVertexLayout();
	DCLFStream first, second;
	first.address = DCLFAddress(DCLFVertexBuffer.xy);
	first.stride = DCLFVertexStride;
	second.address = DCLFAddress(DCLFStreamBuffer.xy);
	second.stride = DCLFStreamStride;
	return DCLFShade(DCLFPulledInput(index, first, second));
}
#		endif  // DCLF_PULLED
#	endif  // DCLF_PARTICLE_PULL, DCLF_PULLED_VERTICES, DCLF_PULLED
#endif

typedef VS_OUTPUT PS_INPUT;
SamplerState SampBaseSampler : register(s0);
SamplerState SampNormalSampler : register(s1);
SamplerState SampNoiseSampler : register(s2);
SamplerState SampDepthSampler : register(s3);
SamplerState SampGrayscaleSampler : register(s4);

Texture2D<float4> TexBaseSampler : register(t0);
Texture2D<float4> TexNormalSampler : register(t1);
Texture2D<float4> TexNoiseSampler : register(t2);
Texture2D<float4> TexDepthSamplerEffect : register(t3);
Texture2D<float4> TexGrayscaleSampler : register(t4);

#if defined(DEFERRED)
struct PS_OUTPUT
{
	float4 Diffuse: SV_Target0;
#	if defined(MOTIONVECTORS_NORMALS)
	float4 MotionVectors: SV_Target1;
	float4 NormalGlossiness: SV_Target2;
#	elif defined(NORMALS)
	float4 NormalGlossiness: SV_Target2;
#	endif
	float4 Albedo: SV_Target3;
	float4 Specular: SV_Target4;
	float4 Reflectance: SV_Target5;
	float4 Masks: SV_Target6;
	float4 Masks2: SV_Target7;
};
#else
struct PS_OUTPUT
{
	float4 Diffuse: SV_Target0;
#	if defined(MOTIONVECTORS_NORMALS)
	float2 MotionVectors: SV_Target1;
	float4 ScreenSpaceNormals: SV_Target2;
#	else
	float4 Color2: SV_Target2;
#	endif
};
#endif

#ifdef PSHADER

#	if defined(DCLF_BINDLESS_DRAW) && defined(DCLF_PULLED)
static float AlphaTestRefRS;  // the pulled main's
#	elif defined(DCLF_BINDLESS_DRAW)
// Drawcall Limit Fix: the alpha test reference is the object record's (as Lighting.hlsl's), not a buffer of its own.
static const float AlphaTestRefRS = DCLFObjects[DCLFObjectIndex].AlphaTestRef;
#	else
cbuffer AlphaTestRefCB : register(b11)
{
	float AlphaTestRefRS : packoffset(c0);
}
#	endif  // DCLF_BINDLESS_DRAW

#	if defined(DCLF_BINDLESS)
// Drawcall Limit Fix (T7d's Effect port): the camera's planes are the frame's, in the frame lighting block every DCLF_BINDLESS draw's
// pixel stage reads (Lighting.hlsl, DCLFFrameLighting; LightingConstants.h, SampleEffectCamera): x far, y near, z far - near, as SetupTechnique
// writes CameraDataEffect.yz from the current camera (+0x160, +0x164). VPOSOffset and FilteringParam are not read.
cbuffer DCLFFrameLighting : register(b13)
{
	float4 DCLFEffectCamera : packoffset(c17);
	// The Lighting variant's DLightColor (SetupGeometry: the pass's light 0, the scene's sun light, its NiDirectionalLight +0x14c colour times
	// its fade and the HDR multiplier): the frame's (LightingConstants.h, SampleEffectLight).
	float4 DCLFEffectDLightColor : packoffset(c18);
	// Blood's AlphaTestRef.yz (SetupGeometry: the weapon blood's two globals, 0x1433dccf0 and 0x1433dccf4): the frame's (LightingConstants.h,
	// SampleEffectBlood).
	float4 DCLFEffectBlood : packoffset(c19);
};
static const float4 CameraDataEffect = DCLFEffectCamera;
// The soft depth (Soft: SetupTechnique binds the depth copy, kPOST_ZPREPASS_COPY, to t3): the frame record's, by name (DrawPipelines.h,
// kEffectSoftDepthRegister), so a material row does not hold a render target.
Texture2D<float4> DCLFEffectSoftDepth : register(t118);
#	else
cbuffer PerTechnique : register(b0)
{
	float4 CameraDataEffect : packoffset(c0);
	float2 VPOSOffset : packoffset(c1);
	float2 FilteringParam : packoffset(c1.z);
};
#	endif  // DCLF_BINDLESS

cbuffer PerMaterial : register(b1)
{
	float4 BaseColor : packoffset(c0);
	float4 BaseColorScale : packoffset(c1);
	float4 LightingInfluence : packoffset(c2);
};

cbuffer PerGeometry : register(b2)
{
	float4 PLightPositionX[1] : packoffset(c0);
	float4 PLightPositionY[1] : packoffset(c1);
	float4 PLightPositionZ[1] : packoffset(c2);
	float4 PLightingRadiusInverseSquared : packoffset(c3);
	float4 PLightColorR : packoffset(c4);
	float4 PLightColorG : packoffset(c5);
	float4 PLightColorB : packoffset(c6);
#	if !defined(DCLF_BINDLESS)
	float4 DLightColor : packoffset(c7);
	float4 PropertyColor : packoffset(c8);
#	endif  // !DCLF_BINDLESS
	float4 AlphaTestRef : packoffset(c9);
	float4 MembraneRimColor : packoffset(c10);
	float4 MembraneVars : packoffset(c11);
};

#	if defined(DCLF_BINDLESS)
// Drawcall Limit Fix (T7d's Effect port): PropertyColor is the object's (SetupGeometry: its property's emittance colour, white without
// one, and its alpha, which GetRenderPasses leaves as the material's times the fade node's currentFade): the shading row's (LightingConstants,
// SampleShading) EmitColor and the material's alpha in MaterialData.z, times the object's fade (DCLFAlphaFade, as Lighting's MaterialData.z).
#		if defined(DCLF_PULLED)
static float4 PropertyColor;  // the pulled main's
#		else
static const float4 PropertyColor = float4(DCLFShading[DCLFObjectIndex].EmitColor.xyz,
	DCLFShading[DCLFObjectIndex].MaterialData.z * DCLFAlphaFade(DCLFObjectIndex, (DCLFObjectWord & 0x40000000u) != 0));
#		endif
// The Lighting variant's DLightColor is the frame's (DCLFFrameLighting c18); its point lights are Light Limit Fix's clusters (the PLight
// variables above are read only outside the world, which a DCLF draw never is: a Lighting effect without Light Limit Fix stays the engine's).
static const float4 DLightColor = DCLFEffectDLightColor;
// External Emittance's suppress bit (ExternalEmittance::UpdatePermutation in BSEffectShader::SetupGeometry's hook) is the object's, in its
// record (LightingConstants.h, kRecordEffectSuppressEmittance), not the pipeline row's permutation block.
#		if defined(DCLF_PULLED)
static bool DCLFSuppressExternalEmittance;  // the pulled main's
#		else
static const bool DCLFSuppressExternalEmittance = (DCLFObjects[DCLFObjectIndex].DCLFRecordFlags & 0x8u) != 0;
#		endif
#	endif  // DCLF_BINDLESS

#	if defined(LIGHT_LIMIT_FIX)
#		include "LightLimitFix/LightLimitFix.hlsli"
#	endif

#	define LinearSampler SampBaseSampler

#	if defined(SKYLIGHTING)
#		include "Skylighting/Skylighting.hlsli"
#	endif

#	if defined(IBL)
#		include "IBL/IBL.hlsli"
#	endif

#	if defined(EXP_HEIGHT_FOG)
#		define SampColorSampler SampBaseSampler
#		include "ExponentialHeightFog/ExponentialHeightFog.hlsli"
#	endif

#	include "Common/ShadowSampling.hlsli"

#	if defined(LIGHTING)
float3 GetLightingColor(float3 msPosition, float3 worldPosition, float2 screenPosition, inout float shadowVariance)
{
	float3 color = DLightColor.xyz * Color::EffectLightingScale;
	const float3 sceneAmbientColor = ShadowSampling::GetAmbientLighting();
#		if defined(DCLF_BINDLESS)
	bool suppressExternalEmittance = SharedData::InInterior && DCLFSuppressExternalEmittance;
#		else
	bool suppressExternalEmittance = SharedData::InInterior && (Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::SuppressExternalEmittance);
#		endif  // DCLF_BINDLESS
	if (suppressExternalEmittance) {
		color = sceneAmbientColor + ShadowSampling::GetDirectionalLighting();
	}

#		if defined(SKYLIGHTING)
	float skylightingDiffuse = 1.0;
	if (!SharedData::InInterior) {
		float3 positionMSSkylight = worldPosition;

		sh2 skylightingSH = Skylighting::SampleNoBias(positionMSSkylight);
		skylightingDiffuse = Skylighting::EvaluateDiffuse(skylightingSH, float3(0, 0, 1), Skylighting::GetFadeOutFactor(positionMSSkylight));
	}
#		endif

	float3 dirColor;
	float3 ambientColor;
	ShadowSampling::ExtractLighting(color, sceneAmbientColor, dirColor, ambientColor);

#		if defined(EFFECTS11)
	if (SharedData::enbSettings.Enable) {
		dirColor = ShadowSampling::GetDirectionalLighting();
		ambientColor = sceneAmbientColor;
		dirColor *= SharedData::enbSettings.ParticleLightingInfluence;
		ambientColor *= SharedData::enbSettings.ParticleAmbientInfluence;
	}
#		endif

	float3 viewDirection = normalize(worldPosition.xyz);

	float unusedSurfaceShadow;
	float dirShadow = 1.0;

	const bool inWorld = (Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::InWorld);

	if (inWorld && ShadowSampling::HasDirectionalShadows())
		dirShadow = ShadowSampling::Get3DFilteredShadow(worldPosition.xyz, viewDirection, screenPosition, unusedSurfaceShadow);

	shadowVariance = 1.0 - sqrt(saturate(fwidth(dirShadow)));

	dirColor *= dirShadow;

#		if defined(EXP_HEIGHT_FOG)
	if (SharedData::exponentialHeightFogSettings.enabled) {
		dirColor *= ExponentialHeightFog::GetSunlightFogAttenuation(worldPosition.xyz, FrameBuffer::CameraPosAdjust.xyz);
	}
#		endif

#		if defined(SKYLIGHTING)
#			if defined(IBL)
	if (!SharedData::iblSettings.EnableIBL)
#			endif
	{
		ambientColor = ColorManagement::SceneColor::ScaleByLinear(ambientColor, skylightingDiffuse);
	}
#		endif

	color = dirColor + ambientColor;

#		if defined(LIGHT_LIMIT_FIX)
	if (!(Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::InWorld))
#		endif
	{
		float4 lightDistanceSquared = (PLightPositionX[0] - msPosition.xxxx) * (PLightPositionX[0] - msPosition.xxxx) + (PLightPositionY[0] - msPosition.yyyy) * (PLightPositionY[0] - msPosition.yyyy) + (PLightPositionZ[0] - msPosition.zzzz) * (PLightPositionZ[0] - msPosition.zzzz);
		float4 lightFadeMul = 1.0.xxxx - saturate(PLightingRadiusInverseSquared * lightDistanceSquared);
#		if defined(ENABLE_LL)
		lightFadeMul = pow(lightFadeMul, TransferFunctions::GAME_GAMMA);
#		endif
#		if defined(EFFECTS11)
		float pointScale = SharedData::enbSettings.Enable ? SharedData::enbSettings.ParticlePointLightingInfluence : 1.0;
#		else
		float pointScale = 1.0;
#		endif
		color.x += dot(Color::PointLight(PLightColorR.xxx).x * lightFadeMul * Color::EffectLightingScale, 1.0.xxxx) * pointScale;
		color.y += dot(Color::PointLight(PLightColorG.xxx).x * lightFadeMul * Color::EffectLightingScale, 1.0.xxxx) * pointScale;
		color.z += dot(Color::PointLight(PLightColorB.xxx).x * lightFadeMul * Color::EffectLightingScale, 1.0.xxxx) * pointScale;
	}

	return color;
}
#	else
/** @brief Average sun shadow along the view ray through the particle, 1 outside the world or in interiors. */
float GetViewRayShadow(float3 worldPosition, float2 screenPosition, float depth)
{
	static const uint sampleCount = 8;
	static const float rcpSampleCount = 1.0 / float(sampleCount);

	float noise = Random::InterleavedGradientNoise(screenPosition, SharedData::FrameCount);

	// Enough for sky statics
	float maxDistance = max(0, SharedData::GetScreenDepth(depth));
	float viewRayLength = 2048.0;
	float3 viewDirection = normalize(worldPosition);
	float3 startPosition = worldPosition - viewDirection * viewRayLength;
	float3 endPosition = worldPosition + viewDirection * min(maxDistance, viewRayLength);

	float shadow = 1.0;

	const bool inWorld = (Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::InWorld);

	if (inWorld && !SharedData::InInterior) {
		shadow = 0.0;
		for (uint i = 0; i < sampleCount; i++) {
			float t = (float(i) + noise) * rcpSampleCount;
			float3 samplePositionWS = lerp(startPosition, endPosition, t);
			shadow += ShadowSampling::GetWorldShadow(samplePositionWS, FrameBuffer::CameraPosAdjust.xyz);
		}
		shadow *= rcpSampleCount;
	}

	return shadow;
}

float3 GetLightingShadow(float3 color, float3 worldPosition, float2 screenPosition, float depth, inout float shadowVariance)
{
	float3 dirColor;
	float3 ambientColor;
	ShadowSampling::ExtractLighting(color, dirColor, ambientColor);

	float shadow = GetViewRayShadow(worldPosition, screenPosition, depth);

	shadowVariance = 1.0 - sqrt(saturate(fwidth(shadow)));

	dirColor *= shadow;

#		if defined(EXP_HEIGHT_FOG)
	if (SharedData::exponentialHeightFogSettings.enabled) {
		dirColor *= ExponentialHeightFog::GetSunlightFogAttenuation(worldPosition.xyz, FrameBuffer::CameraPosAdjust.xyz);
	}
#		endif

	return dirColor + ambientColor;
}
#	endif

#	if defined(DCLF_PULLED)
PS_OUTPUT DCLFShadePS(PS_INPUT input)
#	else
PS_OUTPUT main(PS_INPUT input)
#	endif
{
	PS_OUTPUT psout = (PS_OUTPUT)0;

	float4 fogMul = 1;
#	if !defined(MULTBLEND)
	fogMul.xyz = input.FogAlpha;
#	endif

#	if defined(MEMBRANE)
#		if !defined(MOTIONVECTORS_NORMALS) && defined(ALPHA_TEST)
	float noiseAlpha = TexNoiseSampler.Sample(SampNoiseSampler, input.TexCoord0.zw).w;
#			if defined(VC)
	noiseAlpha *= input.Color.w;
#			endif
	if (noiseAlpha - AlphaTestRef.x < 0) {
		discard;
	}
#		endif

#		if defined(MOTIONVECTORS_NORMALS) && defined(MEMBRANE) && !defined(SKINNED) && defined(NORMALS)
	float3 normal = input.ScreenSpaceNormal;
#		elif defined(NORMALS)
	float3 normal = input.TBN0;
#		else
	float3 normal = TexNormalSampler.Sample(SampNormalSampler, input.TexCoord0.zw).xzy * 2 - 1;
#			if defined(SKINNED)
	normal = mul(normal, transpose(float3x3(input.TBN0, input.TBN1, input.TBN2)));
#			endif
#		endif
	float NdotV = dot(normal, input.ViewVector.xyz);
	float membraneColorMul = pow(saturate(1 - NdotV), MembraneVars.x);
	float4 membraneColor = ColorManagement::SRGBToWorking(MembraneRimColor) * membraneColorMul;
#	elif defined(PROJECTED_UV) && defined(NORMALS)
	float2 noiseTexCoord = 0.00333333341 * input.TexCoord0.xy;
	float noise = TexNoiseSampler.Sample(SampNoiseSampler, noiseTexCoord).x * 0.2 + 0.4;
	if (dot(input.TBN0, input.TBN1) - noise < 0) {
		discard;
	}
#	endif

	float softMul = 1;
	float depth = 1;
#	if defined(SOFT)
#		if defined(DCLF_BINDLESS)
	// The material row holds softFalloffDepth itself (MaterialPortEffect.cpp): near * far / it, as SetupMaterial computes it on the CPU
	// from the camera's planes (the same product and quotient).
	depth = DCLFEffectSoftDepth.Load(int3(input.Position.xy, 0)).x;
	const float softInfluence = (DCLFEffectCamera.x * DCLFEffectCamera.y) / LightingInfluence.y;
	softMul = saturate(-input.TexCoord0.w + softInfluence / ((1 - depth) * CameraDataEffect.z + CameraDataEffect.y));
#		else
	depth = TexDepthSamplerEffect.Load(int3(input.Position.xy, 0)).x;
	softMul = saturate(-input.TexCoord0.w + LightingInfluence.y / ((1 - depth) * CameraDataEffect.z + CameraDataEffect.y));
#		endif  // DCLF_BINDLESS
#	endif

	float lightingInfluence = LightingInfluence.x;
	float3 propertyColor = PropertyColor.xyz;
	float shadowVariance = 1.0;

	bool isFire = false;
#	if defined(ADDBLEND)
#		if defined(SOFT)
	if (Permutation::PixelShaderDescriptor & Permutation::EffectFlags::GrayscaleToColor && Permutation::PixelShaderDescriptor & Permutation::EffectFlags::GrayscaleToAlpha)
		isFire = true;
#		elif defined(PARTICLES) && defined(TEXCOORD_INDEX) && defined(INDEXED_TEXTURE)
	isFire = true;
#		endif
#	endif

#	if defined(EFFECTS11) && defined(IS_VOLUMETRIC_FOG)
	const bool isEnbVolumetricFog = SharedData::enbSettings.Enable && !(Permutation::VertexShaderDescriptor & Permutation::EffectFlags::SkyObject) &&
	                                (Permutation::PixelShaderDescriptor & Permutation::EffectFlags::GrayscaleToAlpha) && !(Permutation::PixelShaderDescriptor & Permutation::EffectFlags::GrayscaleToColor);
#	endif

	float4 baseTexColor = float4(1, 1, 1, 1);
	float4 baseColor = float4(1, 1, 1, 1);
#	if !defined(TEXTURE)
	[branch] if (Permutation::PixelShaderDescriptor & Permutation::EffectFlags::GrayscaleToColor || Permutation::PixelShaderDescriptor & Permutation::EffectFlags::GrayscaleToAlpha)
#	endif
	{
		baseTexColor = TexBaseSampler.Sample(SampBaseSampler, input.TexCoord0.xy);
#	if !defined(BLOOD)
		if (!(Permutation::PixelShaderDescriptor & Permutation::EffectFlags::GrayscaleToColor))
			baseTexColor.xyz = ColorManagement::TextureToWorking(baseTexColor.xyz);
#	endif
		baseColor *= baseTexColor;
		if (Permutation::PixelShaderDescriptor & Permutation::EffectFlags::IgnoreTexAlpha || Permutation::PixelShaderDescriptor & Permutation::EffectFlags::GrayscaleToAlpha) {
			baseColor.w = 1;
		}
	}

#	if defined(MEMBRANE)
	float4 baseColorMul = float4(1, 1, 1, 1);
#	else
	float4 baseColorMul = BaseColor;
#		if defined(VC) && !defined(PROJECTED_UV)
#			if defined(BLOOD)
	baseColorMul *= input.Color;
#			else
	baseColorMul *= (Permutation::PixelShaderDescriptor & Permutation::EffectFlags::GrayscaleToColor) ? input.Color : ColorManagement::SRGBToWorking(input.Color);
#			endif
#		endif
#	endif

#	if defined(MEMBRANE)
	baseColor.w *= input.ViewVector.w;
#	else
	baseColor.w *= input.TexCoord0.z;
#	endif

	baseColor = baseColorMul * baseColor;
	baseColor.w *= softMul;

#	if defined(SOFT) && !(defined(FALLOFF) && defined(MULTBLEND))
	if (baseColor.w - 0.003 < 0) {
		discard;
	}
#	endif

	float alpha = baseColor.w;

#	if defined(BLOOD)
#		if defined(DCLF_BINDLESS)
	// Drawcall Limit Fix (T7d's Effect port): SetupGeometry's AlphaTestRef for Blood is the alpha property's threshold / 255 (the object record's
	// reference: LightingConstants.h, AlphaTestRefOf) and the frame's two globals (DCLFEffectBlood); the PerGeometry block is not bound.
	const float4 bloodRef = float4(AlphaTestRefRS, DCLFEffectBlood.yz, 0);
#		else
	const float4 bloodRef = AlphaTestRef;
#		endif
	alpha = baseColor.y;
	float deltaY = saturate(baseColor.y - bloodRef.x);
	float bloodMul = baseColor.z;
#		if defined(VC)
	bloodMul *= input.Color.w;
#		endif
	if (deltaY < bloodRef.y) {
		bloodMul *= (deltaY / bloodRef.y);
	}
	baseColor.xyz = saturate(float3(2, 1, 1) - bloodMul.xxx) * (-bloodMul * bloodRef.z + 1);
	baseColor.xyz = ColorManagement::TextureToWorking(baseColor.xyz);
#	endif

	alpha *= PropertyColor.w;

	float baseColorScale = BaseColorScale.x;

#	if defined(MEMBRANE)
	baseColor.xyz = (PropertyColor.xyz + baseColor.xyz) * alpha + membraneColor.xyz * membraneColor.w;
	alpha += membraneColor.w;
	baseColorScale = MembraneVars.z;
#	endif

	if (Permutation::PixelShaderDescriptor & Permutation::EffectFlags::GrayscaleToAlpha)
		alpha = TexGrayscaleSampler.Sample(SampGrayscaleSampler, float2(baseTexColor.w, alpha)).w;

	[branch] if (Permutation::PixelShaderDescriptor & Permutation::EffectFlags::GrayscaleToColor)
	{
		float2 grayscaleToColorUv = float2(baseTexColor.y, baseColorMul.x);
#	if defined(MEMBRANE)
		grayscaleToColorUv.y = PropertyColor.x;
#	endif
		baseColor.xyz = baseColorScale * ColorManagement::TextureToWorking(TexGrayscaleSampler.Sample(SampGrayscaleSampler, grayscaleToColorUv).xyz);
	}

#	if defined(EFFECTS11) && defined(IS_VOLUMETRIC_FOG)
	[branch] if (isEnbVolumetricFog)
	{
		alpha = saturate(alpha * SharedData::enbSettings.VolumetricFogOpacity);
		float volumetricFogShadow = 1.0;
		[branch] if (SharedData::enbSettings.VolumetricFogEnableShadows)
			volumetricFogShadow = GetViewRayShadow(input.WorldPosition.xyz, input.Position.xy, depth);
		float3 volumetricFogScale = SharedData::enbSettings.VolumetricFogIntensity * SharedData::enbSettings.VolumetricFogColorFilter;
		[branch] if (SharedData::enbSettings.VolumetricFogEnableLighting)
		{
			// The vertex tint only colours the fog here; brightness comes from the scene lighting
			float3 volumetricFogTint = BaseColor.xyz;
			volumetricFogTint /= max(max(max(volumetricFogTint.x, volumetricFogTint.y), volumetricFogTint.z), 1.0);
			float3 volumetricFogLight = ShadowSampling::GetDirectionalLighting() * volumetricFogShadow + ShadowSampling::GetAmbientLighting();
			baseColor.xyz = pow(max(volumetricFogTint * baseTexColor.xyz, 0.0), SharedData::enbSettings.VolumetricFogCurve) * volumetricFogLight * volumetricFogScale * 0.5;
			lightingInfluence = 0.0;
		}
		else
		{
			float volumetricFogShade = lerp(1.0 - SharedData::enbSettings.VolumetricFogShadowAmount, 1.0, volumetricFogShadow);
			baseColor.xyz = pow(max(baseColor.xyz, 0.0), SharedData::enbSettings.VolumetricFogCurve) * volumetricFogShade * volumetricFogScale;
		}
	}
#	endif

#	if !defined(MOTIONVECTORS_NORMALS)
	if (alpha * fogMul.w - AlphaTestRefRS < 0) {
		discard;
	}
#	endif

	// Light only pixels that survive the alpha test.
#	if defined(LIGHTING)
	propertyColor = GetLightingColor(input.MSPosition.xyz, input.WorldPosition.xyz, input.Position.xy, shadowVariance);

#		if defined(LIGHT_LIMIT_FIX)
	uint lightCount = 0;

	float3 viewPosition = mul(FrameBuffer::CameraView, float4(input.WorldPosition.xyz, 1)).xyz;
	float2 screenUV = FrameBuffer::ViewToUV(viewPosition);
	bool inWorld = Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::InWorld;

#			if defined(EFFECTS11)
	float clusteredPointScale = SharedData::enbSettings.Enable ? SharedData::enbSettings.ParticlePointLightingInfluence : 1.0;
#			else
	float clusteredPointScale = 1.0;
#			endif

	uint clusterIndex = 0;
	if (inWorld && LightLimitFix::GetClusterIndex(screenUV, viewPosition.z, clusterIndex)) {
		lightCount = LightLimitFix::lightGrid[clusterIndex].lightCount;
		uint lightOffset = LightLimitFix::lightGrid[clusterIndex].offset;
		[loop] for (uint i = 0; i < lightCount; i++)
		{
			uint clusteredLightIndex = LightLimitFix::lightList[lightOffset + i];
			LightLimitFix::Light light = LightLimitFix::lights[clusteredLightIndex];
			if (LightLimitFix::IsLightIgnored(light) || light.lightFlags & LightLimitFix::LightFlags::ShadowCaster) {
				continue;
			}
			float3 lightDirection = light.positionWS.xyz - input.WorldPosition.xyz;
			float lightDist = length(lightDirection);

			float intensityMultiplier = LightLimitFix::GetAttenuation(lightDist, light);

			float3 lightColor = Color::PointLight(light.color.xyz) * intensityMultiplier * 0.5 * light.fade * Color::EffectLightingScale;
			propertyColor += lightColor * clusteredPointScale;
		}
	}

#		endif
#	elif defined(MEMBRANE)
	propertyColor *= 0;
	lightingInfluence = 0;
#	endif

	// Effects 11 PARTICLE Intensity never reached lit particles, keep that behavior.
#	if !defined(IS_VOLUMETRIC_FOG) && !defined(MULTBLEND) && !defined(MULTBLEND_DECAL) && !(defined(EFFECTS11) && defined(LIGHTING))
	if (!(Permutation::VertexShaderDescriptor & Permutation::EffectFlags::SkyObject) && !isFire)
		propertyColor *= Color::ParticleEffectMult();
#	endif

	float3 lightColor = lerp(baseColor.xyz, propertyColor * baseColor.xyz, lightingInfluence);

#	if defined(ENABLE_LL) && !defined(DEFERRED) && !defined(MEMBRANE) && !defined(BLOOD) && !defined(MULTBLEND) && !defined(MULTBLEND_DECAL)
	if ((Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::SourceAlphaBlend) &&
		(!Permutation::RenderToUI || (Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::InReflection))) {
		// Preserve legacy fades without remapping linear medium transmittance.
		alpha = pow(saturate(alpha), TransferFunctions::GAME_GAMMA);
	}
#	endif

#	if !defined(LIGHTING) && defined(VC) && defined(TEXCOORD) && defined(NORMALS) && defined(TEXTURE) && defined(FALLOFF) && defined(SOFT)
	bool applyViewRayShadow = Permutation::PixelShaderDescriptor & Permutation::EffectFlags::GrayscaleToAlpha && lightingInfluence == 1.0;
#		if defined(EFFECTS11) && defined(IS_VOLUMETRIC_FOG)
	// The fog block above already shadowed it
	applyViewRayShadow = applyViewRayShadow && !isEnbVolumetricFog;
#		endif
	if (applyViewRayShadow)
		lightColor = GetLightingShadow(lightColor, input.WorldPosition.xyz, input.Position.xy, depth, shadowVariance);
#	endif

	lightColor = Color::EffectMult(lightColor);

#	if !defined(MOTIONVECTORS_NORMALS)
	float fogFactor = input.FogParam.w;
	float3 fogColor = input.FogParam.xyz;
#		if defined(IBL)
	if (SharedData::iblSettings.EnableIBL) {
		fogColor = ImageBasedLighting::GetFogIBLColor(fogColor);
	}
#		endif
#		if defined(EXP_HEIGHT_FOG)
	float vanillaFogFactor = fogFactor;
	float3 vanillaFogColor = fogColor;
	float expFogFactor = 0;
	if (SharedData::exponentialHeightFogSettings.enabled) {
		float4 exponentialHeightFog = ExponentialHeightFog::GetExponentialHeightFog(input.WorldPosition.xyz, FrameBuffer::CameraPosAdjust.xyz, fogColor, float4(input.Position.xy * FrameBuffer::DynamicResolutionParams2.xy, input.Position.z, 1));
		expFogFactor = exponentialHeightFog.w;
#			if defined(ADDBLEND) || defined(MULTBLEND) || defined(MULTBLEND_DECAL)
		fogColor = exponentialHeightFog.xyz;
		fogFactor = exponentialHeightFog.w;
#			else
		fogColor = exponentialHeightFog.xyz;
		fogFactor = exponentialHeightFog.w;
		alpha *= 1 - exponentialHeightFog.w;
#			endif
		if (ExponentialHeightFog::ShouldDisableVanillaFog()) {
			vanillaFogColor = lightColor;
			vanillaFogFactor = 0;
		}
	}
#		endif
#		if defined(ADDBLEND)
#			if defined(EXP_HEIGHT_FOG)
	float3 blendedColor = lightColor * (1 - vanillaFogFactor) * (1 - expFogFactor);
#			else
	float3 blendedColor = lightColor * (1 - fogFactor);
#			endif
	blendedColor = Color::AdditiveEffect(blendedColor, isFire);
#		elif defined(MULTBLEND) || defined(MULTBLEND_DECAL)
#			if defined(EXP_HEIGHT_FOG)
	float3 blendedColor = lerp(lightColor, 1.0.xxx, saturate(1.5 * vanillaFogFactor).xxx);
	blendedColor = lerp(blendedColor, 1.0.xxx, saturate(1.5 * expFogFactor).xxx);
#			else
	float3 blendedColor = lerp(lightColor, 1.0.xxx, saturate(1.5 * fogFactor).xxx);
#			endif
#		else
#			if defined(EXP_HEIGHT_FOG)
	float3 blendedColor = lerp(lightColor, vanillaFogColor, vanillaFogFactor.xxx);
	blendedColor = lerp(blendedColor, fogColor, expFogFactor.xxx);
#			else
	float3 blendedColor = lerp(lightColor, fogColor, fogFactor.xxx);
#			endif
#		endif
#	else
	float3 blendedColor = lightColor.xyz;
#	endif

	float4 finalColor = float4(blendedColor, alpha);
#	if defined(MULTBLEND_DECAL)
	finalColor.xyz *= alpha;
#	else
	finalColor *= fogMul;
#	endif
#	if defined(EFFECTS11)
	[branch] if (SharedData::enbSettings.Enable && (Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::IsAurora))
		finalColor.xyz = pow(max(finalColor.xyz, 0.0), SharedData::enbSettings.AuroraCurve) * SharedData::enbSettings.AuroraIntensity;
#	endif
	psout.Diffuse = finalColor;
#	if defined(LIGHTING) && defined(LIGHT_LIMIT_FIX) && defined(LLFDEBUG)
	if (SharedData::lightLimitFixSettings.EnableLightsVisualisation) {
		if (SharedData::lightLimitFixSettings.LightsVisualisationMode == 0) {
			psout.Diffuse.xyz = Color::TurboColormap(0.0);
		} else if (SharedData::lightLimitFixSettings.LightsVisualisationMode == 1) {
			psout.Diffuse.xyz = Color::TurboColormap(0.0);
		} else {
			psout.Diffuse.xyz = Color::TurboColormap((float)lightCount / MAX_CLUSTER_LIGHTS);
		}
	}
#	endif

#	if defined(DEFERRED)

#		if defined(MOTIONVECTORS_NORMALS)
#			if (defined(MEMBRANE) && defined(SKINNED) && defined(NORMALS))
	float3 screenSpaceNormal = normalize(input.TBN0);
#			else
	float3 screenSpaceNormal = normalize(input.ScreenSpaceNormal);
#			endif
	psout.NormalGlossiness = float4(GBuffer::EncodeNormal(screenSpaceNormal), 0.0, psout.Diffuse.w);
	float2 screenMotionVector = MotionBlur::GetSSMotionVector(input.WorldPosition, input.PreviousWorldPosition);
	psout.MotionVectors = float4(screenMotionVector, 0.0, psout.Diffuse.w);
#		endif

#		if defined(MULTBLEND) || defined(MULTBLEND_DECAL)
	psout.Specular = float4(psout.Diffuse.xyz, finalColor.w);
	psout.Albedo = float4(psout.Diffuse.xyz, finalColor.w);
	psout.Reflectance = float4(psout.Diffuse.xyz, finalColor.w);
	psout.Masks = float4(Color::RGBToLuminance(psout.Diffuse.xyz).xxx, finalColor.w);
	psout.Masks2 = float4(0, 0, 0, finalColor.w);
#		else
	psout.Albedo = float4(psout.Diffuse.xyz * !(Permutation::VertexShaderDescriptor & Permutation::EffectFlags::SkyObject), finalColor.w);
	psout.Specular = float4(0, 0, 0, finalColor.w);
	psout.Reflectance = float4(0, 0, 0, finalColor.w);
	psout.Masks = float4(0, 0, 0, finalColor.w);
	psout.Masks2 = float4(0, 0, 0, finalColor.w);
#		endif

#	elif defined(MOTIONVECTORS_NORMALS)
	float2 screenMotionVector = MotionBlur::GetSSMotionVector(input.WorldPosition, input.PreviousWorldPosition);
	psout.MotionVectors = screenMotionVector;

#		if (defined(MEMBRANE) && defined(SKINNED) && defined(NORMALS))
	float3 screenSpaceNormal = normalize(input.TBN0);
#		else
	float3 screenSpaceNormal = normalize(input.ScreenSpaceNormal);
#		endif

	screenSpaceNormal.z = max(0.001, sqrt(8 + -8 * screenSpaceNormal.z));
	screenSpaceNormal.xy /= screenSpaceNormal.zz;
	psout.ScreenSpaceNormals.xy = screenSpaceNormal.xy + 0.5.xx;
	psout.ScreenSpaceNormals.zw = 0.0.xx;
#	else
	psout.Color2 = finalColor;
#	endif

#	if defined(ENABLE_LL)
	if (Permutation::RenderToUI && !(Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::InReflection))
		psout.Diffuse.xyz = ColorManagement::WorkingToUI(psout.Diffuse.xyz);
#	endif
	return psout;
}
#	if defined(DCLF_PULLED)
// The permutation block (b4) as the draw's pipeline row holds it (the vertex stage's DCLFPermutationStatics).
void DCLFPermutationStatics()
{
	Permutation::VertexShaderDescriptor = DCLFRowUint(DCLFPipelineRowAddress, kDCLFPermutationBlock);
	Permutation::PixelShaderDescriptor = DCLFRowUint(DCLFPipelineRowAddress, kDCLFPermutationBlock + 4);
	Permutation::ExtraShaderDescriptor = DCLFRowUint(DCLFPipelineRowAddress, kDCLFPermutationBlock + 8);
	Permutation::ExtraFeatureDescriptor = DCLFRowUint(DCLFPipelineRowAddress, kDCLFPermutationBlock + 12);
	Permutation::EffectRadius = DCLFRowFloat(DCLFPipelineRowAddress, kDCLFPermutationBlock + 16);
}

// Drawcall Limit Fix's plain draws (DCLF_PULLED): the draw's rows and object word are its vertex stage's (Lighting.hlsl's pulled pixel
// main), and so the rows' blocks and the object's values, assigned before anything reads them.
PS_OUTPUT DCLFPulledPixel(PS_INPUT input, uint a_instance)
{
	const uint64_t sequence = DCLFDrawSequence(a_instance);
	const uint4 rows = vk::RawBufferLoad<uint4>(sequence + 4);  // the pipeline row's address, the material row's
	DCLFObjectWord = vk::RawBufferLoad<uint>(sequence + 20);
	DCLFPipelineRowAddress = DCLFAddress(rows.xy);
	DCLFMaterialRowAddress = DCLFAddress(rows.zw);
	DCLFPerMaterialStatics();
	DCLFPerGeometryStatics();
	DCLFPermutationStatics();
#		if defined(DCLF_BINDLESS_DRAW)
	AlphaTestRefRS = DCLFObjects[DCLFObjectIndex].AlphaTestRef;
#		endif
#		if defined(DCLF_BINDLESS)
	PropertyColor = float4(DCLFShading[DCLFObjectIndex].EmitColor.xyz,
		DCLFShading[DCLFObjectIndex].MaterialData.z * DCLFAlphaFade(DCLFObjectIndex, (DCLFObjectWord & 0x40000000u) != 0));
	DCLFSuppressExternalEmittance = (DCLFObjects[DCLFObjectIndex].DCLFRecordFlags & 0x8u) != 0;
#		endif
	return DCLFShadePS(input);
}

// Keyed on a wave-uniform instance (Lighting.hlsl's pulled pixel main): a quad's lanes are one draw's, so they take one pass together.
PS_OUTPUT main(PS_INPUT input)
{
	PS_OUTPUT psout;
	[loop] for (;;) {
		const uint drawInstance = WaveReadLaneFirst(input.DCLFDrawInstance);
		[branch] if (drawInstance == input.DCLFDrawInstance) {
			psout = DCLFPulledPixel(input, drawInstance);
			break;
		}
	}
	return psout;
}
#	endif  // DCLF_PULLED
#endif
