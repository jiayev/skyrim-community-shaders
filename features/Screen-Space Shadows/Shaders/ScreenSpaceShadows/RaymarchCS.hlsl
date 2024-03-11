#include "Common.hlsl"

Texture2D<float> ShadowTexture : register(t1);

// Needed to fix a bug in VR that caused the arm
// to have the "outline" of the VR headset canvas
// rendered into it and to not cast rays outside the eyes
#ifdef VR
Texture2D<uint2> StencilTexture : register(t89);

float GetStencil(float2 uv)
{
	return StencilTexture.Load(int3(uv * BufferDim * DynamicRes.xy, 0)).g;
}

float GetStencil(float2 uv, uint a_eyeIndex)
{
	uv = ConvertToStereoUV(uv, a_eyeIndex);
	return GetStencil(uv);
}
#endif  // VR

bool IsSaturated(float value)
{
	return value == saturate(value);
}
bool IsSaturated(float2 value) { return IsSaturated(value.x) && IsSaturated(value.y); }

// https://www.shadertoy.com/view/Xt23zV
float smoothbumpstep(float edge0, float edge1, float x)
{
	x = 1.0 - abs(clamp((x - edge0) / (edge1 - edge0), 0.0, 1.0) - .5) * 2.0;
	return x * x * (3.0 - x - x);
}

// Derived from the interleaved gradient function from Jimenez 2014 http://goo.gl/eomGso
float InterleavedGradientNoise(float2 uv)
{
	float3 magic = float3(0.06711056f, 0.00583715f, 52.9829189f);
	return frac(magic.z * frac(dot(uv, magic.xy)));
}

float GetScreenDepth(float depth)
{
	return (CameraData.w / (-depth * CameraData.z + CameraData.x));
}

float GetScreenDepth(float2 uv, uint a_eyeIndex)
{
	uv = ConvertToStereoUV(uv, a_eyeIndex);
	float depth = GetDepth(uv);
	return GetScreenDepth(depth);
}

float ScreenSpaceShadowsUV(float2 texcoord, float3 lightDirectionVS, uint eyeIndex)
{
#ifdef VR
	if (GetStencil(texcoord) != 0)
		return 1;
#endif  // VR

	// Ignore the sky
	float startDepth = GetDepth(texcoord);
	if (startDepth >= 1)
		return 1;

	// Compute ray position in view-space
	float3 rayPos = InverseProjectUVZ(ConvertFromStereoUV(texcoord, eyeIndex), startDepth, eyeIndex);

	// Blends effect variables between near, mid and far field
	float blendFactorFar = smoothstep(ShadowDistance / 3, ShadowDistance / 2, rayPos.z);
	float blendFactorMid = smoothbumpstep(0, ShadowDistance / 2, rayPos.z);

	// Max shadow length, longer shadows are less accurate
	float maxDistance = lerp(NearDistance, rayPos.z * FarDistanceScale, blendFactorFar);

	// Max ray steps, affects quality and performance
	uint maxSteps = max(1, (uint)((float)MaxSamples * (1 - blendFactorMid)));

	// How far to move each sample each step
	float stepLength = maxDistance / (float)maxSteps;

	// Compute ray step
	float3 rayStep = lightDirectionVS * stepLength;

	// Offset starting position with interleaved gradient noise
	float offset = InterleavedGradientNoise(texcoord * BufferDim);
	rayPos += rayStep * offset;

	float thickness = lerp(NearThickness, rayPos.z * FarThicknessScale, blendFactorFar);

	// Accumulate samples
	float shadow = 0.0f;
	uint samples = 0;

	float2 rayUV = 0.0f;
	for (uint i = 0; i < maxSteps; i++) {
		samples++;

		// Step the ray
		rayPos += rayStep;
		rayUV = ViewToUV(rayPos, true, eyeIndex);

		// Ensure the UV coordinates are inside the screen
		if (!IsSaturated(rayUV))
			break;

#ifdef VR
		if (GetStencil(rayUV, eyeIndex) != 0)
			break;
#endif  // VR

		// Compute the difference between the ray's and the camera's depth
		float rayDepth = GetScreenDepth(rayUV, eyeIndex);

		// Difference between the current ray distance and the marched light
		float depthDelta = rayPos.z - rayDepth;

		// Distant shadows simulate real shadows whereas near shadows are only intended for small objects
		float rayShadow = depthDelta / thickness;

		// Check if the depth difference is considered a shadow
		if (rayShadow > 0.0f && rayShadow <= 1.0f)
			shadow += rayShadow;
	}

	// Average samples
	shadow /= samples;

	// Intensity and sharpness of shadows
	shadow *= lerp(NearHardness, FarHardness, blendFactorFar);

	// Convert to visibility
	return 1 - saturate(shadow);
}

[numthreads(32, 32, 1)] void main(uint3 DTid
								  : SV_DispatchThreadID) {
	float2 texCoord = (DTid.xy + 0.5) * RcpBufferDim * DynamicRes.zw;
	uint eyeIndex = GetEyeIndexFromTexCoord(texCoord);
	OcclusionRW[DTid.xy] = ScreenSpaceShadowsUV(texCoord, InvDirLightDirectionVS.xyz, eyeIndex);
}