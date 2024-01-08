#define EFFECT

struct VS_INPUT
{
	float4 Position : POSITION0;
#if defined(TEXCOORD)
#	if defined(STRIP_PARTICLES)
	float3
#	else
	float2
#	endif
		TexCoord0 : TEXCOORD0;
#endif
#if defined(NORMALS) || defined(MOTIONVECTORS_NORMALS)
	float4 Normal : NORMAL0;
#endif
#if defined(BINORMAL_TANGENT)
	float4 Bitangent : BINORMAL0;
#endif
#if defined(VC)
	float4 Color : COLOR0;
#endif
#if defined(SKINNED)
	float4 BoneWeights : BLENDWEIGHT0;
	float4 BoneIndices : BLENDINDICES0;
#endif
};

struct VS_OUTPUT
{
	float4 Position : SV_POSITION0;
	float4 TexCoord0 : TEXCOORD0;
	float4 WorldPosition : POSITION1;
#if defined(VC)
	float4 Color : COLOR0;
#endif
#if !defined(MOTIONVECTORS_NORMALS)
	float4 FogParam : COLOR1;
#endif
#if defined(MOTIONVECTORS_NORMALS) && defined(MEMBRANE) && !defined(SKINNED) && defined(NORMALS)
	float3 ScreenSpaceNormal : TEXCOORD1;
#elif (defined(MEMBRANE) && (defined(SKINNED) || defined(NORMALS))) || (defined(PROJECTED_UV) && defined(NORMALS))
	float3 TBN0 : TEXCOORD1;
#endif
#if defined(MEMBRANE) && (defined(SKINNED) || defined(NORMALS))
	float FogAlpha : TEXCOORD5;
#endif
#if (defined(MEMBRANE) && defined(SKINNED) && !defined(NORMALS)) || (defined(PROJECTED_UV) && defined(NORMALS) && !defined(MEMBRANE))
	float3 TBN1 : TEXCOORD2;
#endif
#if (defined(MEMBRANE) && defined(SKINNED) && !defined(NORMALS))
	float3 TBN2 : TEXCOORD3;
#endif
#if defined(MEMBRANE)
	float4 ViewVector : TEXCOORD4;
#endif
#if defined(LIGHTING)
	float3 MSPosition : TEXCOORD6;
#endif
#if !(defined(MEMBRANE) && (defined(SKINNED) || defined(NORMALS)))
	float FogAlpha : TEXCOORD5;
#endif
#if defined(MOTIONVECTORS_NORMALS)
#	if !(defined(MEMBRANE) && (defined(SKINNED) || defined(NORMALS)))
	float3 ScreenSpaceNormal : TEXCOORD7;
#	endif
	float4 PreviousWorldPosition : POSITION2;
#	if (defined(MEMBRANE) && defined(SKINNED) && !defined(NORMALS))
	float3 ScreenSpaceNormal : TEXCOORD7;
#	endif
#endif
};

#ifdef VSHADER
cbuffer PerFrame : register(b12)
{
	row_major float4x3 ScreenProj : packoffset(c0);
	row_major float4x4 ViewProj : packoffset(c8);
#	if defined(SKINNED)
	float3 BonesPivot : packoffset(c40);
#		if defined(MOTIONVECTORS_NORMALS)
	float3 PreviousBonesPivot : packoffset(c41);
#		endif
#	endif
};

cbuffer PerTechnique : register(b0)
{
	float4 FogParam : packoffset(c0);
	float4 FogNearColor : packoffset(c1);
	float4 FogFarColor : packoffset(c2);
};

cbuffer PerMaterial : register(b1)
{
	float4 TexcoordOffset : packoffset(c0);
	float4 SoftMateralVSParams : packoffset(c1);
	float4 FalloffData : packoffset(c2);
};

cbuffer PerGeometry : register(b2)
{
	row_major float3x4 World : packoffset(c0);
	row_major float3x4 PreviousWorld : packoffset(c3);
	float4 MatProj[3] : packoffset(c6);
	float4 EyePosition : packoffset(c12);
	float4 PosAdjust : packoffset(c13);
	float4 TexcoordOffsetMembrane : packoffset(c14);
}

cbuffer IndexedTexcoordBuffer : register(b11)
{
	float4 IndexedTexCoord[128] : packoffset(c0);
}

#	if defined(SKINNED)
#		if defined(MOTIONVECTORS_NORMALS)
cbuffer PreviousBonesBuffer : register(b9)
{
	float4 PreviousBones[240] : packoffset(c0);
}
#		endif

cbuffer BonesBuffer : register(b10)
{
	float4 Bones[240] : packoffset(c0);
}
#	endif

#	if defined(SKINNED)
float3x4 GetBoneTransformMatrix(float4 bones[240], int4 actualIndices, float3 pivot, float4 weights)
{
	float3x4 pivotMatrix = transpose(float4x3(0.0.xxx, 0.0.xxx, 0.0.xxx, pivot));

	float3x4 boneMatrix1 =
		float3x4(bones[actualIndices.x], bones[actualIndices.x + 1], bones[actualIndices.x + 2]);
	float3x4 boneMatrix2 =
		float3x4(bones[actualIndices.y], bones[actualIndices.y + 1], bones[actualIndices.y + 2]);
	float3x4 boneMatrix3 =
		float3x4(bones[actualIndices.z], bones[actualIndices.z + 1], bones[actualIndices.z + 2]);
	float3x4 boneMatrix4 =
		float3x4(bones[actualIndices.w], bones[actualIndices.w + 1], bones[actualIndices.w + 2]);

	float3x4 unitMatrix = float3x4(1.0.xxxx, 1.0.xxxx, 1.0.xxxx);
	float3x4 weightMatrix1 = unitMatrix * weights.x;
	float3x4 weightMatrix2 = unitMatrix * weights.y;
	float3x4 weightMatrix3 = unitMatrix * weights.z;
	float3x4 weightMatrix4 = unitMatrix * weights.w;

	return (boneMatrix1 - pivotMatrix) * weightMatrix1 +
	       (boneMatrix2 - pivotMatrix) * weightMatrix2 +
	       (boneMatrix3 - pivotMatrix) * weightMatrix3 +
	       (boneMatrix4 - pivotMatrix) * weightMatrix4;
}

float3x3 GetBoneRSMatrix(float4 bones[240], int4 actualIndices, float4 weights)
{
	float3x3 result;
	for (int rowIndex = 0; rowIndex < 3; ++rowIndex) {
		result[rowIndex] = weights.xxx * bones[actualIndices.x + rowIndex].xyz +
		                   weights.yyy * bones[actualIndices.y + rowIndex].xyz +
		                   weights.zzz * bones[actualIndices.z + rowIndex].xyz +
		                   weights.www * bones[actualIndices.w + rowIndex].xyz;
	}
	return result;
}
#	endif

#	define M_HALFPI 1.57079637;
#	define M_PI 3.141593

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
		projUvTmp5 = projUvTmp * projUvTmp2 * -2 + M_HALFPI;
	} else {
		projUvTmp5 = 0;
	}
	float projUvTmp6 = projUvTmp * projUvTmp2 + projUvTmp5;
	float projUvTmp7;
	if (worldPosition.y < -worldPosition.y) {
		projUvTmp7 = -M_PI;
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

VS_OUTPUT main(VS_INPUT input)
{
	VS_OUTPUT vsout;

	precise float4 inputPosition = float4(input.Position.xyz, 1.0);

	precise float4x4 world4x4 = float4x4(World[0], World[1], World[2], float4(0, 0, 0, 1));
	precise float3x3 world3x3 =
		transpose(float3x3(transpose(World)[0], transpose(World)[1], transpose(World)[2]));

#	if defined(SKY_OBJECT)
	float4x4 viewProj = float4x4(ViewProj[0], ViewProj[1], ViewProj[3], ViewProj[3]);
#	else
	float4x4 viewProj = ViewProj;
#	endif

#	if defined(SKINNED)
	precise int4 actualIndices = 765.01.xxxx * input.BoneIndices.xyzw;
#		if defined(MOTIONVECTORS_NORMALS)
	float3x4 previousBoneTransformMatrix =
		GetBoneTransformMatrix(PreviousBones, actualIndices, PreviousBonesPivot, input.BoneWeights);
	precise float4 previousWorldPosition =
		float4(mul(inputPosition, transpose(previousBoneTransformMatrix)), 1);
#		endif
	float3x4 boneTransformMatrix =
		GetBoneTransformMatrix(Bones, actualIndices, BonesPivot, input.BoneWeights);
	precise float4 worldPosition = float4(mul(inputPosition, transpose(boneTransformMatrix)), 1);
	float4 viewPos = mul(viewProj, worldPosition);
#	else
	precise float4 worldPosition = float4(mul(World, inputPosition), 1);
	precise float4 previousWorldPosition = float4(mul(PreviousWorld, inputPosition), 1);
	precise float4x4 modelView = mul(viewProj, world4x4);
	float4 viewPos = mul(modelView, inputPosition);
#	endif

	vsout.Position = viewPos;

#	if defined(SKINNED)
	float3x3 boneRSMatrix = GetBoneRSMatrix(Bones, actualIndices, input.BoneWeights);
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

	vsout.FogParam.xyz = lerp(FogNearColor.xyz, FogFarColor.xyz, fogColorParam);
	vsout.FogParam.w = fogColorParam;
#	endif

	float4 texCoord = float4(0, 0, 1, 0);

#	if defined(MEMBRANE)
	float4 texCoordOffset = TexcoordOffsetMembrane;
#	else
	float4 texCoordOffset = TexcoordOffset;
#	endif

#	if defined(TEXCOORD_INDEX)
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
#			if defined(TEXCOORD_INDEX)
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
#			if defined(TEXCOORD_INDEX)
	v = IndexedTexCoord[index].w * v + IndexedTexCoord[index].z;
#			endif
	texCoord.y = v * texCoordOffset.w + texCoordOffset.y;
#		endif
#	endif
#	if defined(PROJECTED_UV) && !defined(NORMALS)
	texCoord.w = input.TexCoord0.y;
#	elif defined(SOFT)
	texCoord.w = viewPos.w / SoftMateralVSParams.x;
#	elif defined(MEMBRANE) && !defined(NORMALS)
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
#	elif defined(MEMBRANE) && !defined(NORMALS)
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
	float4x3 modelScreen = mul(ScreenProj, world3x3);
	float3 screenSpaceNormal = normalize(mul(modelScreen, normal)).xyz;
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
Texture2D<float4> TexDepthSampler : register(t3);
Texture2D<float4> TexGrayscaleSampler : register(t4);

struct PS_OUTPUT
{
	float4 Color : SV_Target0;
#if defined(MOTIONVECTORS_NORMALS)
	float2 MotionVectors : SV_Target1;
	float4 ScreenSpaceNormals : SV_Target2;
#else
	float4 Normal : SV_Target1;
	float4 Color2 : SV_Target2;
#endif
};

#ifdef PSHADER

#	include "Common/Color.hlsl"
#	include "Common/FrameBuffer.hlsl"
#	include "Common/MotionBlur.hlsl"
#	include "Common/Permutation.hlsl"

cbuffer AlphaTestRefBuffer : register(b11)
{
	float AlphaTestRef1 : packoffset(c0);
}

cbuffer PerTechnique : register(b0)
{
	float4 CameraData : packoffset(c0);
	float2 VPOSOffset : packoffset(c1);
	float2 FilteringParam : packoffset(c1.z);
};

cbuffer PerMaterial : register(b1)
{
	float4 BaseColor : packoffset(c0);
	float4 BaseColorScale : packoffset(c1);
	float4 LightingInfluence : packoffset(c2);
};

cbuffer PerGeometry : register(b2)
{
	float4 PLightPositionX : packoffset(c0);
	float4 PLightPositionY : packoffset(c1);
	float4 PLightPositionZ : packoffset(c2);
	float4 PLightingRadiusInverseSquared : packoffset(c3);
	float4 PLightColorR : packoffset(c4);
	float4 PLightColorG : packoffset(c5);
	float4 PLightColorB : packoffset(c6);
	float4 DLightColor : packoffset(c7);
	float4 PropertyColor : packoffset(c8);
	float4 AlphaTestRef : packoffset(c9);
	float4 MembraneRimColor : packoffset(c10);
	float4 MembraneVars : packoffset(c11);
};

#	if defined(MEMBRANE) || !defined(LIGHTING)
#		undef LIGHT_LIMIT_FIX
#	endif

#	if defined(LIGHT_LIMIT_FIX)
#		include "LightLimitFix/LightLimitFix.hlsli"
#	endif

#	if defined(LIGHTING)
float3 GetLightingColor(float3 msPosition)
{
	float4 lightDistanceSquared = (PLightPositionY - msPosition.yyyy) * (PLightPositionY - msPosition.yyyy) + (PLightPositionX - msPosition.xxxx) * (PLightPositionX - msPosition.xxxx) + (PLightPositionZ - msPosition.zzzz) * (PLightPositionZ - msPosition.zzzz);
	float4 lightFadeMul = 1.0.xxxx - saturate(PLightingRadiusInverseSquared * lightDistanceSquared);

	float3 color = DLightColor.xyz;
	color.x += dot(PLightColorR * lightFadeMul, 1.0.xxxx);
	color.z += dot(PLightColorB * lightFadeMul, 1.0.xxxx);
	color.y += dot(PLightColorG * lightFadeMul, 1.0.xxxx);

	return color;
}
#	endif

PS_OUTPUT main(PS_INPUT input)
{
	PS_OUTPUT psout;

	float4 fogMul = float4(input.FogAlpha.xxx, 1);

#	if defined(MEMBRANE)
	float noiseAlpha = TexNoiseSampler.Sample(SampNoiseSampler, input.TexCoord0.zw).w;
#		if defined(VC)
	noiseAlpha *= input.Color.w;
#		endif
	if (noiseAlpha - AlphaTestRef.x < 0) {
		discard;
	}
#		if defined(ALPHA_TEST)
#		endif

#		if defined(NORMALS)
	float3 normal = input.TBN0;
#		else
	float3 normal = TexNormalSampler.Sample(SampNormalSampler, input.TexCoord0.zw).xzy * 2 - 1;
#			if defined(SKINNED)
	normal = mul(normal, transpose(float3x3(input.TBN0, input.TBN1, input.TBN2)));
#			endif
#		endif
	float NdotV = dot(normal, input.ViewVector.xyz);
	float membraneColorMul = pow(saturate(1 - NdotV), MembraneVars.x);
	float4 membraneColor = MembraneRimColor * membraneColorMul;
#	endif

	float softMul = 1;
#	if defined(SOFT)
	float depth = TexDepthSampler.Load(int3(input.Position.xy, 0)).x;
	softMul = saturate(-input.TexCoord0.w + LightingInfluence.y / ((1 - depth) * CameraData.z + CameraData.y));
#	endif

#	if defined(MEMBRANE)
	float4 baseColorMul = float4(1, 1, 1, 1);
#	else
	float4 baseColorMul = BaseColor;
#		if defined(VC)
	baseColorMul *= input.Color;
#		endif
#	endif

	float4 baseTexColor = float4(1, 1, 1, 1);
	float4 baseColor = float4(1, 1, 1, 1);
#	if defined(TEXTURE)
	baseTexColor = TexBaseSampler.Sample(SampBaseSampler, input.TexCoord0.xy);
	baseColor *= baseTexColor;
#		if defined(IGNORE_TEX_ALPHA) || defined(GRAYSCALE_TO_ALPHA)
	baseColor.w = 1;
#		endif
#	endif
#	if defined(MEMBRANE)
	baseColor.w *= input.ViewVector.w;
#	else
	baseColor.w *= input.TexCoord0.z;
#	endif

	baseColor *= baseColorMul;
	baseColor.w *= softMul;

#	if defined(SOFT)
	if (baseColor.w - 0.003 < 0) {
		discard;
	}
#	endif

	float lightingInfluence = LightingInfluence.x;
	float3 propertyColor = PropertyColor.xyz;
#	if defined(LIGHTING)
	propertyColor = GetLightingColor(input.MSPosition);
#	elif defined(MEMBRANE)
	propertyColor *= 0;
	lightingInfluence = 0;
#	endif

#	if defined(LIGHT_LIMIT_FIX)
	uint eyeIndex = 0;
	uint lightCount = 0;
	if (LightingInfluence.x > 0.0) {
		float3 viewPosition = mul(CameraView[eyeIndex], float4(input.WorldPosition.xyz, 1)).xyz;
		float2 screenUV = ViewToUV(viewPosition, true, eyeIndex);

		uint clusterIndex = 0;
		if (GetClusterIndex(screenUV, viewPosition.z, clusterIndex)) {
			lightCount = lightGrid[clusterIndex].lightCount;
			uint lightOffset = lightGrid[clusterIndex].offset;
			[loop] for (uint i = 0; i < lightCount; i++)
			{
				uint light_index = lightList[lightOffset + i];
				StructuredLight light = lights[light_index];
				float3 lightDirection = light.positionWS[eyeIndex].xyz - input.WorldPosition.xyz;
				float lightDist = length(lightDirection);
				float intensityFactor = saturate(lightDist / light.radius);
				float intensityMultiplier = 1 - intensityFactor * intensityFactor;
				float3 lightColor = light.color.xyz * intensityMultiplier;
				propertyColor += lightColor;
			}
		}
	}
#	endif

#	if defined(GRAYSCALE_TO_COLOR)
	baseColor.xyz = BaseColorScale.x * TexGrayscaleSampler.Sample(SampGrayscaleSampler, float2(baseTexColor.y, baseColorMul.x)).xyz;
#	endif

	float alpha = PropertyColor.w * baseColor.w;

#	if defined(GRAYSCALE_TO_ALPHA)
	alpha = TexGrayscaleSampler.Sample(SampGrayscaleSampler, float2(baseTexColor.w, alpha)).w;
#	endif

#	if defined(BLOOD)
	baseColor.w = baseColor.y;
	float deltaY = saturate(baseColor.y - AlphaTestRef.x);
	float bloodMul = baseColor.z;
	if (deltaY < AlphaTestRef.y) {
		bloodMul *= (deltaY / AlphaTestRef.y);
	}
	baseColor.xyz = saturate(float3(2, 1, 1) - bloodMul.xxx) * (-bloodMul * AlphaTestRef.z + 1);
#	endif

#	if defined(MEMBRANE)
	baseColor.xyz = (PropertyColor.xyz + baseColor.xyz) * alpha + membraneColor.xyz * membraneColor.w;
	alpha += membraneColor.w;
#	endif

	float3 lightColor = lerp(baseColor.xyz, propertyColor * baseColor.xyz, lightingInfluence.xxx);

#	if !defined(MOTIONVECTORS_NORMALS)
	if (alpha * fogMul.w - AlphaTestRef1 < 0) {
		discard;
	}
#	endif

#	if !defined(MOTIONVECTORS_NORMALS)
#		if defined(ADDBLEND)
	float3 blendedColor = lightColor * (1 - input.FogParam.www);
#		elif defined(MULTBLEND) || defined(MULTBLEND_DECAL)
	float3 blendedColor = lerp(lightColor, 1.0.xxx, saturate(1.5 * input.FogParam.w).xxx);
#		else
	float3 blendedColor = lerp(lightColor, input.FogParam.xyz, input.FogParam.www);
#		endif
#	else
	float3 blendedColor = lightColor.xyz;
#	endif

	float4 finalColor = float4(blendedColor, alpha);
#	if defined(MULTBLEND_DECAL)
	finalColor.xyz *= alpha;
#	elif !defined(MULTBLEND)
	finalColor *= fogMul;
#	endif
	psout.Color = finalColor;
#	if defined(LIGHT_LIMIT_FIX) && defined(LLFDEBUG)
	if (perPassLLF[0].EnableLightsVisualisation) {
		if (perPassLLF[0].LightsVisualisationMode == 0) {
			psout.Color.xyz = TurboColormap(0.0);
		} else if (perPassLLF[0].LightsVisualisationMode == 1) {
			psout.Color.xyz = TurboColormap(0.0);
		} else {
			psout.Color.xyz = TurboColormap((float)lightCount / 128.0);
		}
	}
#	endif

#	if defined(MOTIONVECTORS_NORMALS)
	float2 screenMotionVector = GetSSMotionVector(input.WorldPosition, input.PreviousWorldPosition, 0);

	psout.MotionVectors = screenMotionVector;

#		if (defined(MEMBRANE) && (defined(SKINNED) || defined(NORMALS)))
	float3 screenSpaceNormal = normalize(input.TBN0);
#		else
	float3 screenSpaceNormal = normalize(input.ScreenSpaceNormal);
#		endif

	screenSpaceNormal.z = max(0.001, sqrt(8 + -8 * screenSpaceNormal.z));
	screenSpaceNormal.xy /= screenSpaceNormal.zz;
	psout.ScreenSpaceNormals.xy = screenSpaceNormal.xy + 0.5.xx;
	psout.ScreenSpaceNormals.zw = 0.0.xx;
#	else
	psout.Normal.xyz = float3(1, 0, 0);
	psout.Normal.w = finalColor.w;

	psout.Color2 = finalColor;
#	endif

	return psout;
}
#endif
