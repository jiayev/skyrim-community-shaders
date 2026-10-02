#include "Common/Color.hlsli"
#include "Common/FrameBuffer.hlsli"
#include "Common/LodLandscape.hlsli"
#include "Common/Math.hlsli"
#include "Common/Random.hlsli"
#include "Common/DCLFObjects.hlsli"
#include "Common/SharedData.hlsli"
#include "Common/Skinned.hlsli"
#if defined(RENDER_SHADOWMASK) || defined(RENDER_SHADOWMASKSPOT) || defined(RENDER_SHADOWMASKPB) || defined(RENDER_SHADOWMASKDPB)
#	define RENDER_SHADOWMASK_ANY
#endif

#if defined(VSHADER)
struct VS_INPUT
{
	float4 PositionMS: POSITION0;

#	if defined(TEXTURE)
	float2 TexCoord: TEXCOORD0;
#	endif

#	if defined(NORMALS)
	float4 Normal: NORMAL0;
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
	float4 PositionCS: SV_POSITION0;

#if !(defined(RENDER_DEPTH) && defined(RENDER_SHADOWMASK_ANY)) && SHADOWFILTER != 2
#	if (defined(ALPHA_TEST) && ((!defined(RENDER_DEPTH) && !defined(RENDER_SHADOWMAP)) || defined(RENDER_SHADOWMAP_PB))) || defined(RENDER_NORMAL) || defined(DEBUG_SHADOWSPLIT) || defined(RENDER_BASE_TEXTURE)
	float4 TexCoord0: TEXCOORD0;
#	endif

#	if defined(RENDER_NORMAL)
	float4 Normal: TEXCOORD1;
#	endif

#	if defined(RENDER_SHADOWMAP_PB)
	float3 TexCoord1: TEXCOORD2;
#	elif defined(ALPHA_TEST) && (defined(RENDER_DEPTH) || defined(RENDER_SHADOWMAP))
	float4 TexCoord1: TEXCOORD2;
#	elif defined(ADDITIONAL_ALPHA_MASK)
	float2 TexCoord1: TEXCOORD2;
#	endif

#	if defined(LOCALMAP_FOGOFWAR)
	float Alpha: TEXCOORD3;
#	endif

#	if defined(RENDER_SHADOWMASK_ANY)
	float4 PositionMS: TEXCOORD5;
#	endif

#	if defined(ALPHA_TEST) && defined(VC) && defined(RENDER_SHADOWMASK_ANY)
	float2 Alpha: TEXCOORD4;
#	elif (defined(ALPHA_TEST) && defined(VC) && !defined(TREE_ANIM)) || defined(RENDER_SHADOWMASK_ANY)
	float Alpha: TEXCOORD4;
#	endif

#	if defined(DEBUG_SHADOWSPLIT)
	float Depth: TEXCOORD2;
#	endif
#endif
#if defined(DCLF_PULLED)
	// The draw's diffuse descriptor and object word, for the pixel stage of a pulled draw (DCLF_PULLED, below).
	nointerpolation uint2 DCLFDraw: TEXCOORD7;
#endif
};

#ifdef VSHADER
cbuffer PerTechnique : register(b0)
{
	float4 HighDetailRange : packoffset(c0);  // loaded cells center in xy, size in zw
	float2 ParabolaParam : packoffset(c1);    // inverse radius in x, y is 1 for forward hemisphere or -1 for backward hemisphere
#	if defined(DCLF_BINDLESS)
	// Drawcall Limit Fix, per shadow view: the main camera's eye minus this view's, which turns the
	// records' eye-relative transforms into this camera's (engine notes: shadow maps).
	float4 DCLFEyeDelta : packoffset(c2);
#	endif
};

#	if defined(DCLF_PULLED)
static float4 TexcoordOffset;  // the draw's material row's (DCLF_PULLED, below)
#	else
cbuffer PerMaterial : register(b1)
{
	float4 TexcoordOffset : packoffset(c0);
};
#	endif

cbuffer PerGeometry : register(b2)
{
	float4 ShadowFadeParam : packoffset(c0);
#	if !defined(DCLF_BINDLESS)
	row_major float4x4 World : packoffset(c1);
#	endif
	float4 EyePos : packoffset(c5);
	float4 WaterParams : packoffset(c6);
#	if !defined(DCLF_BINDLESS)
	float4 TreeParams : packoffset(c7);
#	endif
};

#if defined(DCLF_PULLED)
// Set by main from the draw's object (DCLF_PULLED, below), as the DCLF_BINDLESS build's statics.
static precise row_major float4x4 World;
static float4 TreeParams;
#elif defined(DCLF_BINDLESS)
// Drawcall Limit Fix draws the shadow views indirectly: every object of a view shares one pipeline and
// one PerGeometry block, so the values that differ between them come from the per-object record instead
// (Common/DCLFObjects.hlsli). World is absolute in the record and made relative here to the drawing
// view's own eye, the view's VS_PerFrame CameraPosAdjust - the engine's own Utility World is relative to
// whichever camera is drawing (engine notes: shadow maps).
static precise row_major float4x4 World = float4x4(
	DCLFObjects[DCLFObjectIndex].World[0] - float4(0, 0, 0, FrameBuffer::CameraPosAdjust.x),
	DCLFObjects[DCLFObjectIndex].World[1] - float4(0, 0, 0, FrameBuffer::CameraPosAdjust.y),
	DCLFObjects[DCLFObjectIndex].World[2] - float4(0, 0, 0, FrameBuffer::CameraPosAdjust.z),
	float4(0, 0, 0, 1));
static float4 TreeParams = DCLFTreeParamsOf(DCLFObjectIndex);
#endif

float2 SmoothSaturate(float2 value)
{
	return value * value * (3 - 2 * value);
}

#	if defined(DCLF_PULLED)
VS_OUTPUT DCLFShade(VS_INPUT input)
#	else
VS_OUTPUT main(VS_INPUT input)
#	endif
{
	VS_OUTPUT vsout;

#	if (defined(RENDER_DEPTH) && defined(RENDER_SHADOWMASK_ANY)) || SHADOWFILTER == 2
	vsout.PositionCS.xy = input.PositionMS.xy;
#		if defined(RENDER_SHADOWMASKDPB) || defined(RENDER_SHADOWMASKSPOT) || defined(RENDER_SHADOWMASKPB)
	vsout.PositionCS.z = ShadowFadeParam.z;
#		else
	vsout.PositionCS.z = HighDetailRange.x;
#		endif
	vsout.PositionCS.w = 1;
#	elif defined(STENCIL_ABOVE_WATER)
	vsout.PositionCS.y = WaterParams.x * 2 + input.PositionMS.y;
	vsout.PositionCS.xzw = input.PositionMS.xzw;
#	else

	precise float4 positionMS = float4(input.PositionMS.xyz, 1.0);
	float4 positionCS = float4(0, 0, 0, 0);

	float3 normalMS = float3(1, 1, 1);
#		if defined(NORMALS)
	normalMS = input.Normal.xyz * 2 - 1;
#		endif

#		if defined(VC) && defined(NORMALS) && defined(TREE_ANIM)
	float2 treeTmp1 = SmoothSaturate(abs(2 * frac(float2(0.1, 0.25) * (TreeParams.w * TreeParams.y * TreeParams.x) + dot(input.PositionMS.xyz, 1.0.xxx) + 0.5) - 1));
	float normalMult = (treeTmp1.x + 0.1 * treeTmp1.y) * (input.Color.w * TreeParams.z);
	positionMS.xyz += normalMS.xyz * normalMult;
#		endif

#		if defined(LOD_LANDSCAPE)
	positionMS = LodLandscape::AdjustLodLandscapeVertexPositionMS(positionMS, World, HighDetailRange);
#		endif

#		if defined(SKINNED)
	precise int4 boneIndices = 765.01.xxxx * input.BoneIndices.xyzw;

#			if defined(DCLF_BINDLESS)
	// The rows are absolute: the pivot is this view's eye.
	float3x4 worldMatrix = Skinned::GetBoneTransformMatrixBindless(DCLFObjects[DCLFObjectIndex].DCLFBoneOffset, boneIndices, FrameBuffer::CameraPosAdjust.xyz, input.BoneWeights);
#			else
	float3x4 worldMatrix = Skinned::GetBoneTransformMatrix(Bones, boneIndices, FrameBuffer::CameraPosAdjust.xyz, input.BoneWeights);
#			endif
	precise float4 positionWS = float4(mul(positionMS, transpose(worldMatrix)), 1);

	positionCS = mul(FrameBuffer::CameraViewProj, positionWS);
#		else
	precise float4x4 modelViewProj = mul(FrameBuffer::CameraViewProj, World);
	positionCS = mul(modelViewProj, positionMS);
#		endif

#		if defined(RENDER_SHADOWMAP) && defined(RENDER_SHADOWMAP_CLAMPED)
	positionCS.z = max(0, positionCS.z);
#		endif

#		if defined(LOD_LANDSCAPE)
	vsout.PositionCS = LodLandscape::AdjustLodLandscapeVertexPositionCS(positionCS);
#		elif defined(RENDER_SHADOWMAP_PB)
	float3 positionCSPerspective = positionCS.xyz / positionCS.w;
	float3 shadowDirection = normalize(normalize(positionCSPerspective) + float3(0, 0, ParabolaParam.y));
	vsout.PositionCS.xy = shadowDirection.xy / shadowDirection.z;
	vsout.PositionCS.z = ParabolaParam.x * length(positionCSPerspective);
	vsout.PositionCS.w = positionCS.w;
#		else
	vsout.PositionCS = positionCS;
#		endif

#		if defined(RENDER_NORMAL)
	float3 normalVS = float3(1, 1, 1);
#			if defined(SKINNED)
#				if defined(DCLF_BINDLESS)
	float3x3 boneRSMatrix = Skinned::GetBoneRSMatrixBindless(DCLFObjects[DCLFObjectIndex].DCLFBoneOffset, boneIndices, input.BoneWeights);
#				else
	float3x3 boneRSMatrix = Skinned::GetBoneRSMatrix(Bones, boneIndices, input.BoneWeights);
#				endif
	normalMS = normalize(mul(normalMS, transpose(boneRSMatrix)));
	normalVS = mul(FrameBuffer::CameraView, float4(normalMS, 0)).xyz;
#			else
	normalVS = mul(mul(FrameBuffer::CameraView, World), float4(normalMS, 0)).xyz;
#			endif
#			if defined(RENDER_NORMAL_CLAMP)
	normalVS = max(min(normalVS, 0.1), -0.1);
#			endif
	vsout.Normal.xyz = normalVS;

#			if defined(VC)
	vsout.Normal.w = input.Color.w;
#			else
	vsout.Normal.w = 1;
#			endif

#		endif

#		if (defined(ALPHA_TEST) && ((!defined(RENDER_DEPTH) && !defined(RENDER_SHADOWMAP)) || defined(RENDER_SHADOWMAP_PB))) || defined(RENDER_NORMAL) || defined(DEBUG_SHADOWSPLIT) || defined(RENDER_BASE_TEXTURE)
	float4 texCoord = float4(0, 0, 1, 1);
	texCoord.xy = input.TexCoord * TexcoordOffset.zw + TexcoordOffset.xy;

#			if defined(RENDER_NORMAL)
	texCoord.z = max(1, 0.0013333333 * positionCS.z + 0.8);

	float falloff = 1;
#				if defined(RENDER_NORMAL_FALLOFF)
#					if defined(SKINNED)
	falloff = dot(normalMS, normalize(EyePos.xyz - positionWS.xyz));
#					else
	falloff = dot(normalMS, normalize(EyePos.xyz - positionMS.xyz));
#					endif
#				endif
	texCoord.w = EyePos.w * falloff;
#			endif

	vsout.TexCoord0 = texCoord;
#		endif

#		if defined(RENDER_SHADOWMAP_PB)
	vsout.TexCoord1.x = ParabolaParam.x * length(positionCSPerspective);
	vsout.TexCoord1.y = positionCS.w;
	precise float parabolaParam = ParabolaParam.y * positionCS.z;
	vsout.TexCoord1.z = parabolaParam * 0.5 + 0.5;
#		elif defined(ALPHA_TEST) && (defined(RENDER_DEPTH) || defined(RENDER_SHADOWMAP))
	float4 texCoord1 = float4(0, 0, 0, 0);
	texCoord1.xy = positionCS.zw;
	texCoord1.zw = input.TexCoord * TexcoordOffset.zw + TexcoordOffset.xy;

	vsout.TexCoord1 = texCoord1;
#		elif defined(ADDITIONAL_ALPHA_MASK)
	vsout.TexCoord1 = positionCS.zw;
#		elif defined(DEBUG_SHADOWSPLIT)
	vsout.Depth = positionCS.z;
#		endif

#		if defined(RENDER_SHADOWMASK_ANY)
	vsout.Alpha.x = 1 - pow(saturate(dot(positionCS.xyz, positionCS.xyz) / ShadowFadeParam.x), 8);

#			if defined(SKINNED)
	vsout.PositionMS.xyz = positionWS.xyz;
#			else
	vsout.PositionMS.xyz = positionMS.xyz;
#			endif
	vsout.PositionMS.w = positionCS.z;
#		endif

#		if (defined(ALPHA_TEST) && defined(VC)) || defined(LOCALMAP_FOGOFWAR)
#			if defined(RENDER_SHADOWMASK_ANY)
	vsout.Alpha.y = input.Color.w;
#			elif !defined(TREE_ANIM)
	vsout.Alpha.x = input.Color.w;
#			endif
#		endif

#	endif

#	if defined(OFFSET_DEPTH)
	vsout.PositionCS.z += 10.0;
#	endif

	return vsout;
}

#	if defined(DCLF_PULLED)
// Drawcall Limit Fix's shadow views draw with plain indirect draws (ShadowViewPass): one per pipeline, which cannot bind a
// vertex buffer, or push data, per draw. So this stage reads its draw's sequence (BuildDrawsCS: DrawSequence, at
// DCLFSequencesAddress + its instance's stride), and through it the draw's object word, material row and geometry. The draw
// is indexed from the index pool (ShadowIndexPool), with no vertex offset, so the stage's vertex index is the geometry's own,
// and the stage loads the attributes at it, decoded as the engine's input layout for the pipeline's vertex layout has them
// (VertexInput.cpp, BuildVertexElements).
static const uint kDCLFSequenceStride = 92;

uint64_t DCLFAddress(uint2 a_words) { return (uint64_t(a_words.y) << 32) | uint64_t(a_words.x); }

// A stream of the geometry: the vertex buffer (stream 0) or a dynamic shape's positions (stream 1).
struct DCLFStream
{
	uint64_t address;
	uint stride;
};

// The address of attribute a_attribute (BSGraphics::Vertex::Attribute) of vertex a_index: its stream as the layout flags it
// (bit 44 + a in stream 0, else bit 54 + a), and its offset the layout's nibble times four, except the position's (0).
uint64_t DCLFAttribute(uint a_attribute, uint a_index, DCLFStream a_first, DCLFStream a_second)
{
	const bool first = ((DCLFVertexLayout.y >> (12 + a_attribute)) & 1) != 0;
	const uint offset = a_attribute == 0 ? 0 : ((DCLFVertexLayout.x >> (4 * a_attribute + 4)) & 0xF) * 4;
	const uint64_t address = first ? a_first.address : a_second.address;
	const uint stride = first ? a_first.stride : a_second.stride;
	return address + uint64_t(a_index) * stride + offset;
}

float4 DCLFUnorm4(uint a_word) { return float4(a_word & 0xFF, (a_word >> 8) & 0xFF, (a_word >> 16) & 0xFF, a_word >> 24) / 255.0; }
float2 DCLFHalf2(uint a_word) { return float2(f16tof32(a_word), f16tof32(a_word >> 16)); }

VS_OUTPUT main(uint index : SV_VertexID, uint a_instance : SV_InstanceID)
{
	const uint64_t sequence = DCLFAddress(DCLFSequencesAddress) + uint64_t(a_instance) * kDCLFSequenceStride;
	const uint4 rows = vk::RawBufferLoad<uint4>(sequence + 4);  // the pipeline row's address, the material row's
	DCLFObjectWord = vk::RawBufferLoad<uint>(sequence + 20);
	const uint4 vertexBuffer = vk::RawBufferLoad<uint4>(sequence + 24);  // address, size, stride
	const uint4 streamBuffer = vk::RawBufferLoad<uint4>(sequence + 40);
	const uint64_t materialRow = DCLFAddress(rows.zw);

	DCLFStream first, second;
	first.address = DCLFAddress(vertexBuffer.xy);
	first.stride = vertexBuffer.w;
	second.address = DCLFAddress(streamBuffer.xy);
	second.stride = streamBuffer.w;

	VS_INPUT input;
	input.PositionMS = asfloat(vk::RawBufferLoad<uint4>(DCLFAttribute(0, index, first, second)));
#		if defined(TEXTURE)
	input.TexCoord = DCLFHalf2(vk::RawBufferLoad<uint>(DCLFAttribute(1, index, first, second)));
#		endif
#		if defined(NORMALS)
	input.Normal = DCLFUnorm4(vk::RawBufferLoad<uint>(DCLFAttribute(3, index, first, second)));
	input.Bitangent = DCLFUnorm4(vk::RawBufferLoad<uint>(DCLFAttribute(4, index, first, second)));
#		endif
#		if defined(VC)
	input.Color = DCLFUnorm4(vk::RawBufferLoad<uint>(DCLFAttribute(5, index, first, second)));
#		endif
#		if defined(SKINNED)
	const uint64_t skinning = DCLFAttribute(6, index, first, second);
	const uint2 weights = vk::RawBufferLoad<uint2>(skinning);
	input.BoneWeights = float4(DCLFHalf2(weights.x), DCLFHalf2(weights.y));
	input.BoneIndices = DCLFUnorm4(vk::RawBufferLoad<uint>(skinning + 8));
#		endif

	// What the DCLF_BINDLESS build's statics hold, from this draw's object and material row.
	World = float4x4(
		DCLFObjects[DCLFObjectIndex].World[0] - float4(0, 0, 0, FrameBuffer::CameraPosAdjust.x),
		DCLFObjects[DCLFObjectIndex].World[1] - float4(0, 0, 0, FrameBuffer::CameraPosAdjust.y),
		DCLFObjects[DCLFObjectIndex].World[2] - float4(0, 0, 0, FrameBuffer::CameraPosAdjust.z),
		float4(0, 0, 0, 1));
	TreeParams = DCLFTreeParamsOf(DCLFObjectIndex);
	TexcoordOffset = asfloat(vk::RawBufferLoad<uint4>(materialRow));

	VS_OUTPUT vsout = DCLFShade(input);
	// The pixel stage's: the diffuse's descriptor (the shadow material row's, kShadowRowDiffuseOffset) and the object word.
	vsout.DCLFDraw = uint2(vk::RawBufferLoad<uint>(materialRow + 16), DCLFObjectWord);
	return vsout;
}
#	endif
#endif

typedef VS_OUTPUT PS_INPUT;

struct PS_OUTPUT
{
	float4 Color: SV_Target0;
};

#ifdef PSHADER

SamplerState SampBaseSampler : register(s0);
SamplerState SampNormalSampler : register(s1);
SamplerState SampDepthSampler : register(s2);
SamplerState SampShadowMapSampler : register(s3);
SamplerComparisonState SampShadowMapSamplerComp : register(s4);
SamplerState SampStencilSampler : register(s5);
SamplerComparisonState SampFocusShadowMapSamplerComp : register(s6);
SamplerState SampGrayscaleSampler : register(s7);

#	if defined(DCLF_PULLED)
// A pulled draw's diffuse, by the descriptor its vertex stage passes (DCLFDraw.x): the draw has no binding record of its own.
Texture2D<float4> DCLFDiffuse(uint a_index)
{
	return ResourceDescriptorHeap[NonUniformResourceIndex(a_index)];
}
#		define TexBaseSampler DCLFDiffuse(input.DCLFDraw.x)
#	else
Texture2D<float4> TexBaseSampler : register(t0);
#	endif
Texture2D<float4> TexNormalSampler : register(t1);
Texture2D<float4> TexDepthUtilitySampler : register(t2);
Texture2DArray<float4> TexShadowMapSampler : register(t3);
Texture2DArray<float4> TexShadowMapSamplerComp : register(t4);
Texture2D<uint4> TexStencilSampler : register(t5);
Texture2DArray<float4> TexFocusShadowMapSamplerComp : register(t6);
Texture2D<float4> TexGrayscaleSampler : register(t7);

cbuffer PerTechnique : register(b0)
{
	float4 VPOSOffset : packoffset(c0);
	float4 ShadowSampleParam : packoffset(c1);    // fPoissonRadiusScale / iShadowMapResolution in z and w
	float4 EndSplitDistances : packoffset(c2);    // cascade end distances int xyz, cascade count int z
	float4 StartSplitDistances : packoffset(c3);  // cascade start ditances int xyz, 4 int z
	float4 FocusShadowFadeParam : packoffset(c4);
}

cbuffer PerMaterial : register(b1)
{
	float RefractionPower : packoffset(c0);
	float4 BaseColor : packoffset(c1);
}

cbuffer PerGeometry : register(b2)
{
	float4 DebugColor : packoffset(c0);
	float4 PropertyColor : packoffset(c1);
	float4 AlphaTestRef : packoffset(c2);
	float4 ShadowLightParam : packoffset(c3);  // Falloff in x, ShadowDistance squared in z
	float4x3 FocusShadowMapProj[4] : packoffset(c4);
#	if defined(RENDER_SHADOWMASK)
	float4x3 ShadowMapProj[3] : packoffset(c16);  // 16, 19, 22
#	elif defined(RENDER_SHADOWMASKSPOT) || defined(RENDER_SHADOWMASKPB) || defined(RENDER_SHADOWMASKDPB)
	float4x4 ShadowMapProj[3] : packoffset(c16);
#	endif
}

cbuffer AlphaTestRefCB : register(b11)
{
	float AlphaTestRefRS : packoffset(c0);
}

#	if SHADOWFILTER == 8
static const uint ShadowPCFSampleCount = 16;
#	else
static const uint ShadowPCFSampleCount = 8;
#	endif

float2 GetShadowPCFOffset(uint index)
{
#	if SHADOWFILTER == 8
	return Random::PoissonSampleOffsets16[index];
#	else
	return Random::SpiralSampleOffsets8[index];
#	endif
}

#	if defined(DCLF_PULLED)
// One pipeline draws every threshold, so the reference is the object's own (its word set by main, below).
#		define AlphaTestRefRS (DCLFObjects[DCLFObjectIndex].AlphaTestRef)
#	elif defined(DCLF_BINDLESS)
// One pipeline draws every threshold, so the reference is the object's own.
static float DCLFAlphaTestRef = DCLFObjects[DCLFObjectIndex].AlphaTestRef;
#		define AlphaTestRefRS DCLFAlphaTestRef
#	endif

float SampleShadowPCF(Texture2DArray<float4> tex, SamplerComparisonState samp, float2 baseUV, float layerIndex, float compareValue, float2x2 rotationMatrix, float radius)
{
	float visibility = 0.0;
	[unroll] for (uint i = 0; i < ShadowPCFSampleCount; i++)
	{
		float2 offset = mul(GetShadowPCFOffset(i), rotationMatrix) * radius;
		visibility += tex.SampleCmpLevelZero(samp, float3(baseUV + offset, layerIndex), compareValue).x;
	}
	return visibility / ShadowPCFSampleCount;
}

float SampleShadow3x3(Texture2DArray<float4> tex, SamplerComparisonState samp, float2 baseUV, float layerIndex, float compareValue, float2 texelSize)
{
	float visibility = 0.0;
	[unroll] for (int y = -1; y <= 1; y++)
	{
		[unroll] for (int x = -1; x <= 1; x++)
		{
			const float2 uv = baseUV + float2(x, y) * texelSize;
			visibility += tex.SampleCmpLevelZero(samp, float3(uv, layerIndex), compareValue).x;
		}
	}
	return visibility / 9.0;
}

float SampleDualParaboloidShadowPCF(Texture2DArray<float4> tex, SamplerComparisonState samp, float2 baseUV, float layerIndex, float compareValue, float2x2 rotationMatrix, float radius, bool lowerHalf)
{
	float visibility = 0.0;
	[unroll] for (uint i = 0; i < ShadowPCFSampleCount; i++)
	{
		float2 offset = mul(GetShadowPCFOffset(i), rotationMatrix) * radius;
		float2 uv = baseUV + offset;
		uv.y = lowerHalf ? max(uv.y, 0.5) : min(uv.y, 0.5);
		visibility += tex.SampleCmpLevelZero(samp, float3(uv, layerIndex), compareValue).x;
	}
	return visibility / ShadowPCFSampleCount;
}

// Drawcall Limit Fix's depth-only draws of a technique without an alpha test: the stage has no output and no effect, so it is
// compiled empty, so its pipeline tests depth before it shades and the empty stage is skipped (DrawPipelines::ShadowDiscards).
// (The engine's build writes a constant colour to no target.) A TEXTURE technique's test below cannot discard here: its alpha
// is 1 and the reference at most 1. Left in, it put every textured opaque caster on late depth testing.
#	if defined(DCLF_BINDLESS) && (defined(RENDER_SHADOWMAP) || defined(RENDER_DEPTH)) && !defined(ALPHA_TEST) && \
		!defined(ADDITIONAL_ALPHA_MASK) && !defined(RENDER_SHADOWMAP_PB) && !defined(RENDER_SHADOWMASK_ANY) && \
		!defined(DEBUG_SHADOWSPLIT) && !defined(DEBUG_COLOR) && !defined(DEPTH_WRITE_DECALS) && !defined(STENCIL_ABOVE_WATER)
void main(PS_INPUT input)
{
}
#	else
PS_OUTPUT main(PS_INPUT input)
{
	PS_OUTPUT psout;
#	if defined(DCLF_PULLED)
	DCLFObjectWord = input.DCLFDraw.y;  // what AlphaTestRefRS reads
#	endif

#	if defined(ADDITIONAL_ALPHA_MASK)
	uint2 alphaMask = input.PositionCS.xy;
	alphaMask.x = ((alphaMask.x << 2) & 12);
	alphaMask.x = (alphaMask.y & 3) | (alphaMask.x & ~3);
	const float maskValues[16] = {
		0.003922,
		0.533333,
		0.133333,
		0.666667,
		0.800000,
		0.266667,
		0.933333,
		0.400000,
		0.200000,
		0.733333,
		0.066667,
		0.600000,
		0.996078,
		0.466667,
		0.866667,
		0.333333,
	};

	if (AlphaTestRef.w - maskValues[alphaMask.x] < 0) {
		discard;
	}
#	endif

	float2 baseTexCoord = 0;
#	if !(defined(RENDER_DEPTH) && defined(RENDER_SHADOWMASK_ANY)) && SHADOWFILTER != 2
#		if (defined(RENDER_DEPTH) || defined(RENDER_SHADOWMAP)) && defined(ALPHA_TEST) && !defined(RENDER_SHADOWMAP_PB)
	baseTexCoord = input.TexCoord1.zw;
#		elif (defined(ALPHA_TEST) && ((!defined(RENDER_DEPTH) && !defined(RENDER_SHADOWMAP)) || defined(RENDER_SHADOWMAP_PB))) || defined(RENDER_NORMAL) || defined(DEBUG_SHADOWSPLIT) || defined(RENDER_BASE_TEXTURE)
	baseTexCoord = input.TexCoord0.xy;
#		endif
#	endif
	float4 baseColor = TexBaseSampler.SampleBias(SampBaseSampler, baseTexCoord, SharedData::MipBias);

#	if defined(RENDER_SHADOWMAP_PB)
	if (input.TexCoord1.z < 0) {
		discard;
	}
#	endif

	float alpha = 1;
#	if defined(ALPHA_TEST) && !(defined(RENDER_SHADOWMASK_ANY) && defined(RENDER_DEPTH))
	alpha = baseColor.w;
#		if defined(DEPTH_WRITE_DECALS) && !defined(GRAYSCALE_MASK)
	alpha = saturate(1.05 * alpha);
#		endif
#		if defined(OPAQUE_EFFECT)
	alpha *= BaseColor.w * PropertyColor.w;
#		endif
#		if (defined(RENDER_DEPTH) || defined(RENDER_SHADOWMAP) || ((defined(RENDER_NORMAL) || defined(RENDER_NORMAL_CLEAR)) && defined(DEBUG_COLOR))) && defined(VC) && !defined(TREE_ANIM) && !defined(LOD_OBJECT)
	alpha *= input.Alpha;
#		elif defined(RENDER_SHADOWMASK_ANY) && defined(VC)
	alpha *= input.Alpha.y;
#		endif
#		if defined(GRAYSCALE_TO_ALPHA)
	float grayScaleColor = TexGrayscaleSampler.SampleBias(SampGrayscaleSampler, float2(baseColor.w, alpha), SharedData::MipBias).w;
	if (grayScaleColor - AlphaTestRef.x < 0) {
		discard;
	}
#		endif
#		if defined(GRAYSCALE_MASK)
	if (baseColor.y - AlphaTestRef.x < 0) {
		discard;
	}
#		elif !defined(RENDER_SHADOWMAP)
	if (alpha - AlphaTestRef.x < 0) {
		discard;
	}
#		endif
#	endif

#	if defined(RENDER_SHADOWMASK_ANY)
	float4 shadowColor = 1;

	uint stencilValue = 0;
	float shadowMapDepth = 0;
#		if defined(RENDER_DEPTH)
	float2 depthUv = input.PositionCS.xy * VPOSOffset.xy + VPOSOffset.zw;
	float depth = TexDepthUtilitySampler.Sample(SampDepthSampler, depthUv).x;

	shadowMapDepth = depth;

#			if defined(FOCUS_SHADOW)
	uint3 stencilDimensions;
	TexStencilSampler.GetDimensions(0, stencilDimensions.x, stencilDimensions.y, stencilDimensions.z);
	stencilValue = TexStencilSampler.Load(float3(stencilDimensions.xy * depthUv, 0)).x;
#			endif
	depthUv = depthUv * FrameBuffer::DynamicResolutionParams2.xy;
	float4 positionCS = float4(2 * float2(depthUv.x, -depthUv.y + 1) - 1, depth, 1);
	float4 positionMS = mul(FrameBuffer::CameraViewProjInverse, positionCS);
	positionMS.xyz = positionMS.xyz / positionMS.w;

	float fadeFactor = 1 - pow(saturate(dot(positionMS.xyz, positionMS.xyz) / ShadowLightParam.z), 8);
#		else
	float4 positionMS = input.PositionMS.xyzw;

	shadowMapDepth = positionMS.w;

	float fadeFactor = input.Alpha.x;
#		endif

	float noise = Random::InterleavedGradientNoise(input.PositionCS.xy, SharedData::FrameCount);

	float2 rotation;
	sincos(Math::TAU * noise, rotation.y, rotation.x);
	float2x2 rotationMatrix = float2x2(rotation.x, rotation.y, -rotation.y, rotation.x);

#		if defined(RENDER_SHADOWMASK)
	if (SharedData::InInterior)
		shadowColor = float4(0, 0, 0, 0);

	if (EndSplitDistances.z >= shadowMapDepth) {
		float4x3 lightProjectionMatrix = ShadowMapProj[0];
		float shadowMapThreshold = AlphaTestRef.y;
		float cascadeIndex = 0;
		if (2.5 < EndSplitDistances.w && EndSplitDistances.y < shadowMapDepth) {
			lightProjectionMatrix = ShadowMapProj[2];
			shadowMapThreshold = AlphaTestRef.z;
			cascadeIndex = 2;
		} else if (EndSplitDistances.x < shadowMapDepth) {
			lightProjectionMatrix = ShadowMapProj[1];
			shadowMapThreshold = AlphaTestRef.z;
			cascadeIndex = 1;
		}

		float shadowVisibility = 0;

		float3 positionLS = mul(transpose(lightProjectionMatrix), float4(positionMS.xyz, 1)).xyz;
		float shadowMapCompareValue = positionLS.z - shadowMapThreshold;

#			if SHADOWFILTER == 0
		float shadowMapValue = TexShadowMapSampler.Sample(SampShadowMapSampler, float3(positionLS.xy, cascadeIndex)).x;
		if (shadowMapValue >= shadowMapCompareValue) {
			shadowVisibility = 1;
		}
#			elif SHADOWFILTER == 1
		shadowVisibility = TexShadowMapSamplerComp.SampleCmpLevelZero(SampShadowMapSamplerComp, float3(positionLS.xy, cascadeIndex), shadowMapCompareValue).x;
#			elif SHADOWFILTER == 2
		shadowVisibility = SampleShadow3x3(TexShadowMapSamplerComp, SampShadowMapSamplerComp, positionLS.xy, cascadeIndex, shadowMapCompareValue, ShadowSampleParam.zw);
#			elif SHADOWFILTER == 4 || SHADOWFILTER == 8
		shadowVisibility = SampleShadowPCF(TexShadowMapSamplerComp, SampShadowMapSamplerComp, positionLS.xy, cascadeIndex, shadowMapCompareValue, rotationMatrix, ShadowSampleParam.z);
#			endif

		if (cascadeIndex < 1 && StartSplitDistances.y < shadowMapDepth) {
			float cascade1ShadowVisibility = 0;

			float3 cascade1PositionLS = mul(transpose(ShadowMapProj[1]), float4(positionMS.xyz, 1)).xyz;
			float cascade1ShadowMapCompareValue = cascade1PositionLS.z - AlphaTestRef.z;

#			if SHADOWFILTER == 0
			float cascade1ShadowMapValue = TexShadowMapSampler.Sample(SampShadowMapSampler, float3(cascade1PositionLS.xy, 1)).x;
			if (cascade1ShadowMapValue >= cascade1ShadowMapCompareValue) {
				cascade1ShadowVisibility = 1;
			}
#			elif SHADOWFILTER == 1
			cascade1ShadowVisibility = TexShadowMapSamplerComp.SampleCmpLevelZero(SampShadowMapSamplerComp, float3(cascade1PositionLS.xy, 1), cascade1ShadowMapCompareValue).x;
#			elif SHADOWFILTER == 2
			cascade1ShadowVisibility = SampleShadow3x3(TexShadowMapSamplerComp, SampShadowMapSamplerComp, cascade1PositionLS.xy, 1, cascade1ShadowMapCompareValue, ShadowSampleParam.zw);
#			elif SHADOWFILTER == 4 || SHADOWFILTER == 8
			cascade1ShadowVisibility = SampleShadowPCF(TexShadowMapSamplerComp, SampShadowMapSamplerComp, cascade1PositionLS.xy, 1, cascade1ShadowMapCompareValue, rotationMatrix, ShadowSampleParam.z);
#			endif

			float cascade1BlendFactor = smoothstep(0, 1, (shadowMapDepth - StartSplitDistances.y) / (EndSplitDistances.x - StartSplitDistances.y));
			shadowVisibility = lerp(shadowVisibility, cascade1ShadowVisibility, cascade1BlendFactor);

			shadowMapThreshold = AlphaTestRef.z;
		}

		if (stencilValue != 0) {
			uint focusShadowIndex = stencilValue - 1;
			float3 focusShadowMapPosition = mul(transpose(FocusShadowMapProj[focusShadowIndex]), float4(positionMS.xyz, 1));
			float3 focusShadowMapUv = float3(focusShadowMapPosition.xy, StartSplitDistances.w + focusShadowIndex);
			float focusShadowMapCompareValue = focusShadowMapPosition.z - 3 * shadowMapThreshold;
#			if SHADOWFILTER == 4 || SHADOWFILTER == 8
			float focusShadowVisibility = SampleShadowPCF(TexFocusShadowMapSamplerComp, SampFocusShadowMapSamplerComp, focusShadowMapUv.xy, focusShadowMapUv.z, focusShadowMapCompareValue, rotationMatrix, ShadowSampleParam.z);
#			elif SHADOWFILTER == 2
			float focusShadowVisibility = SampleShadow3x3(TexFocusShadowMapSamplerComp, SampFocusShadowMapSamplerComp, focusShadowMapUv.xy, focusShadowMapUv.z, focusShadowMapCompareValue, ShadowSampleParam.zw);
#			else
			float focusShadowVisibility = TexFocusShadowMapSamplerComp.SampleCmpLevelZero(SampFocusShadowMapSamplerComp, focusShadowMapUv, focusShadowMapCompareValue).x;
#			endif
			float focusShadowFade = FocusShadowFadeParam[focusShadowIndex];
			shadowVisibility = min(shadowVisibility, lerp(1, focusShadowVisibility, focusShadowFade));
		}

		shadowColor.xyzw = lerp(1.0 * !SharedData::InInterior, shadowVisibility, fadeFactor);
	}
#		elif defined(RENDER_SHADOWMASKSPOT)
	float4 positionLS = mul(transpose(ShadowMapProj[0]), float4(positionMS.xyz, 1));
	positionLS.xyz /= positionLS.w;
	float2 shadowMapUv = positionLS.xy * 0.5 + 0.5;
	float shadowBaseVisibility = 0;
#			if SHADOWFILTER == 0
	float shadowMapValue = TexShadowMapSampler.Sample(SampShadowMapSampler, float3(shadowMapUv, EndSplitDistances.x)).x;
	if (shadowMapValue >= positionLS.z - AlphaTestRef.y) {
		shadowBaseVisibility = 1;
	}
#			elif SHADOWFILTER == 1
	shadowBaseVisibility = TexShadowMapSamplerComp.SampleCmpLevelZero(SampShadowMapSamplerComp, float3(shadowMapUv, EndSplitDistances.x), positionLS.z - AlphaTestRef.y).x;
#			elif SHADOWFILTER == 2
	shadowBaseVisibility = SampleShadow3x3(TexShadowMapSamplerComp, SampShadowMapSamplerComp, shadowMapUv, EndSplitDistances.x, positionLS.z - AlphaTestRef.y, ShadowSampleParam.zw);
#			elif SHADOWFILTER == 4 || SHADOWFILTER == 8
	shadowBaseVisibility = SampleShadowPCF(TexShadowMapSamplerComp, SampShadowMapSamplerComp, shadowMapUv, EndSplitDistances.x, positionLS.z - AlphaTestRef.y, rotationMatrix, ShadowSampleParam.z);
#			endif
	float shadowVisibilityFactor = pow(2 * length(0.5 * positionLS.xy), ShadowLightParam.x);
	float shadowVisibility = shadowBaseVisibility - shadowVisibilityFactor * shadowBaseVisibility;

	if (stencilValue != 0) {
		uint focusShadowIndex = stencilValue - 1;
		float3 focusShadowMapPosition = mul(transpose(FocusShadowMapProj[focusShadowIndex]), float4(positionMS.xyz, 1));
		float3 focusShadowMapUv = float3(focusShadowMapPosition.xy, StartSplitDistances.w + focusShadowIndex);
		float focusShadowMapCompareValue = focusShadowMapPosition.z - 3 * AlphaTestRef.y;
		float focusShadowVisibility = 0;
#			if SHADOWFILTER == 0
		float shadowMapValue = TexShadowMapSampler.Sample(SampShadowMapSampler, focusShadowMapUv).x;
		if (shadowMapValue >= focusShadowMapCompareValue) {
			focusShadowVisibility = 1;
		}
#			elif SHADOWFILTER == 1
		focusShadowVisibility = TexShadowMapSamplerComp.SampleCmpLevelZero(SampShadowMapSamplerComp, focusShadowMapUv, focusShadowMapCompareValue).x;
#			elif SHADOWFILTER == 2
		focusShadowVisibility = SampleShadow3x3(TexShadowMapSamplerComp, SampShadowMapSamplerComp, focusShadowMapUv.xy, focusShadowMapUv.z, focusShadowMapCompareValue, ShadowSampleParam.zw);
#			elif SHADOWFILTER == 4 || SHADOWFILTER == 8
		focusShadowVisibility = SampleShadowPCF(TexShadowMapSamplerComp, SampShadowMapSamplerComp, focusShadowMapUv.xy, focusShadowMapUv.z, focusShadowMapCompareValue, rotationMatrix, ShadowSampleParam.z);
#			endif
		shadowVisibility = min(shadowVisibility, lerp(1, focusShadowVisibility, FocusShadowFadeParam[focusShadowIndex]));
	}

	shadowColor.xyzw = fadeFactor * shadowVisibility;
#		elif defined(RENDER_SHADOWMASKPB)
	float4 unadjustedPositionLS = mul(transpose(ShadowMapProj[0]), float4(positionMS.xyz, 1));

	float shadowVisibility = 0;

	if (unadjustedPositionLS.z * 0.5 + 0.5 >= 0) {
		float3 positionLS = unadjustedPositionLS.xyz / unadjustedPositionLS.w;
		float3 lightDirection = normalize(normalize(positionLS) + float3(0, 0, 1));
		float2 shadowMapUv = lightDirection.xy / lightDirection.z * 0.5 + 0.5;
		float shadowMapCompareValue = saturate(length(positionLS) / ShadowLightParam.x) - AlphaTestRef.y;
#			if SHADOWFILTER == 0
		float shadowMapValue = TexShadowMapSampler.Sample(SampShadowMapSampler, float3(shadowMapUv, EndSplitDistances.x)).x;
		if (shadowMapValue >= shadowMapCompareValue) {
			shadowVisibility = 1;
		}
#			elif SHADOWFILTER == 1
		shadowVisibility = TexShadowMapSamplerComp.SampleCmpLevelZero(SampShadowMapSamplerComp, float3(shadowMapUv, EndSplitDistances.x), shadowMapCompareValue).x;
#			elif SHADOWFILTER == 2
		shadowVisibility = SampleShadow3x3(TexShadowMapSamplerComp, SampShadowMapSamplerComp, shadowMapUv, EndSplitDistances.x, shadowMapCompareValue, ShadowSampleParam.zw);
#			elif SHADOWFILTER == 4 || SHADOWFILTER == 8
		shadowVisibility = SampleDualParaboloidShadowPCF(TexShadowMapSamplerComp, SampShadowMapSamplerComp, shadowMapUv, EndSplitDistances.x, shadowMapCompareValue, rotationMatrix, ShadowSampleParam.z, false);
#			endif
	} else {
		shadowVisibility = 1;
	}

	shadowColor.xyzw = fadeFactor * shadowVisibility;
#		elif defined(RENDER_SHADOWMASKDPB)
	float3 positionLS = mul(transpose(ShadowMapProj[0]), float4(positionMS.xyz, 1)).xyz;

	bool lowerHalf = positionLS.z * 0.5 + 0.5 < 0;
	float3 normalizedPositionLS = normalize(positionLS);

	float shadowMapCompareValue = saturate(length(positionLS) / ShadowLightParam.x) - AlphaTestRef.y;

	float3 positionOffset = lowerHalf ? float3(0, 0, -1) : float3(0, 0, 1);
	float3 lightDirection = normalize(normalizedPositionLS + positionOffset);
	float2 shadowMapUv = lightDirection.xy / lightDirection.z * 0.5 + 0.5;
	shadowMapUv.y = lowerHalf ? 1.0 - 0.5 * shadowMapUv.y : 0.5 * shadowMapUv.y;

	float shadowVisibility = 0;
#			if SHADOWFILTER == 0
	float shadowMapValue = TexShadowMapSampler.Sample(SampShadowMapSampler, float3(shadowMapUv, EndSplitDistances.x)).x;
	if (shadowMapValue >= shadowMapCompareValue) {
		shadowVisibility = 1;
	}
#			elif SHADOWFILTER == 1
	shadowVisibility = TexShadowMapSamplerComp.SampleCmpLevelZero(SampShadowMapSamplerComp, float3(shadowMapUv, EndSplitDistances.x), shadowMapCompareValue).x;
#			elif SHADOWFILTER == 2
	shadowVisibility = SampleShadow3x3(TexShadowMapSamplerComp, SampShadowMapSamplerComp, shadowMapUv, EndSplitDistances.x, shadowMapCompareValue, ShadowSampleParam.zw);
#			elif SHADOWFILTER == 4 || SHADOWFILTER == 8
	shadowVisibility = SampleDualParaboloidShadowPCF(TexShadowMapSamplerComp, SampShadowMapSamplerComp, shadowMapUv, EndSplitDistances.x, shadowMapCompareValue, rotationMatrix, ShadowSampleParam.z, lowerHalf);
#			endif

	shadowColor.xyzw = fadeFactor * shadowVisibility;
#		endif
#	endif

#	if defined(TEXTURE)
	float testAlpha = 1;
#		if defined(RENDER_SHADOWMASK_ANY)
	testAlpha = shadowColor.w;
#		elif defined(DEBUG_SHADOWSPLIT) || defined(RENDER_BASE_TEXTURE)
	testAlpha = baseColor.w;
#		elif defined(LOCALMAP_FOGOFWAR)
	testAlpha = input.Alpha;
#		elif (defined(RENDER_DEPTH) || defined(RENDER_SHADOWMAP)) && defined(ALPHA_TEST)
	testAlpha = alpha;
#		elif defined(STENCIL_ABOVE_WATER)
	testAlpha = 0.5;
#		elif defined(RENDER_NORMAL_CLEAR) && !defined(DEBUG_COLOR)
	testAlpha = 0;
#		endif
	if (-AlphaTestRefRS + testAlpha < 0) {
		discard;
	}
#	endif

#	if defined(RENDER_SHADOWMASK_ANY)
	psout.Color.xyzw = shadowColor;
#	elif defined(DEBUG_SHADOWSPLIT)

	float3 splitFactor = 0;

	if (input.Depth < EndSplitDistances.x) {
		splitFactor.y += 1;
	}
	if (input.Depth < EndSplitDistances.w && input.Depth > EndSplitDistances.z) {
		splitFactor.y += 1;
		splitFactor.z += 1;
	}
	if (input.Depth < EndSplitDistances.y && input.Depth > EndSplitDistances.x) {
		splitFactor.z += 1;
	}
	if (input.Depth < EndSplitDistances.z && input.Depth > EndSplitDistances.y) {
		splitFactor.x += 1;
		splitFactor.y += 1;
	}

#		if defined(DEBUG_COLOR)
	psout.Color.xyz = (-splitFactor.xyz + DebugColor.xyz) * 0.9 + splitFactor.xyz;
#		else
	psout.Color.xyz = (-splitFactor.xyz + baseColor.xyz) * 0.9 + splitFactor.xyz;
#		endif

	psout.Color.w = baseColor.w;
#	elif defined(DEBUG_COLOR)
	psout.Color = float4(DebugColor.xyz, 1);
#	elif defined(RENDER_BASE_TEXTURE)
	psout.Color.xyzw = baseColor;
#		if defined(TRUE_PBR)
	psout.Color.xyz = TransferFunctions::LinearToGamma22(baseColor.xyz);
#		endif
#	elif defined(LOCALMAP_FOGOFWAR)
	psout.Color = float4(0, 0, 0, input.Alpha);
#	elif defined(GRAYSCALE_MASK)
	psout.Color = float4(input.TexCoord1.x / input.TexCoord1.y, baseColor.yz, alpha);
#	elif (defined(RENDER_SHADOWMAP) && defined(ALPHA_TEST)) || defined(RENDER_SHADOWMAP_PB)
	psout.Color = float4(input.TexCoord1.xxx / input.TexCoord1.yyy, alpha);
#	elif defined(RENDER_DEPTH) && (defined(ALPHA_TEST) || defined(ADDITIONAL_ALPHA_MASK))
	psout.Color = float4(input.TexCoord1.x / input.TexCoord1.y, 1, 1, alpha);
#	elif defined(STENCIL_ABOVE_WATER)
	psout.Color = float4(1, 0, 0, 0.5);
#	elif defined(RENDER_NORMAL_CLEAR)
	psout.Color = float4(0.5, 0.5, 0, 0);
#	elif defined(RENDER_NORMAL)
	float2 normal = 2 * (-0.5 + TexNormalSampler.Sample(SampNormalSampler, input.TexCoord0.xy).xy);
#		if defined(RENDER_NORMAL_CLAMP)
	normal = clamp(normal, -0.1, 0.1);
#		endif
	psout.Color.xy = ((normal * 0.9 + input.Normal.xy) / input.TexCoord0.z) * 0.5 + 0.5;
	psout.Color.z = input.Normal.w * (RefractionPower * input.TexCoord0.w);
	psout.Color.w = 1;
#	else
	psout.Color = float4(1, 1, 1, 1);
#	endif

	return psout;
}
#	endif

#endif
