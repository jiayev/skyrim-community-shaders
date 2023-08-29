struct LightGrid
{
	uint offset;
	uint lightCount;
};

struct StructuredLight
{
	float3 color;
	float radius;
	float3 positionWS[2];
	float3 positionVS[2];
	uint shadowMode;
	uint pad;
};

struct PerPassLLF
{
	uint EnableGlobalLights;
	float CameraNear;
	float CameraFar;
	float4 CameraData;
	float2 BufferDim;
	uint FrameCount;
};

StructuredBuffer<StructuredLight> lights : register(t17);
StructuredBuffer<uint> lightList : register(t18);       //MAX_CLUSTER_LIGHTS * 16^3
StructuredBuffer<LightGrid> lightGrid : register(t19);  //16^3

#if !defined(SCREEN_SPACE_SHADOWS)
Texture2D<float4> TexDepthSampler : register(t20);
#endif  // SCREEN_SPACE_SHADOWS

StructuredBuffer<PerPassLLF> perPassLLF : register(t32);

float GetNearPlane()
{
	return perPassLLF[0].CameraNear;
}

float GetFarPlane()
{
	return perPassLLF[0].CameraFar;
}

// Get a raw depth from the depth buffer.
float GetDepth(float2 uv)
{
	return TexDepthSampler.Load(int3(uv * perPassLLF[0].BufferDim, 0));
}

bool IsSaturated(float value) { return value == saturate(value); }
bool IsSaturated(float2 value) { return IsSaturated(value.x) && IsSaturated(value.y); }

// Derived from the interleaved gradient function from Jimenez 2014 http://goo.gl/eomGso
float InterleavedGradientNoise(float2 uv)
{
	// Temporal factor
	float frameStep = float(perPassLLF[0].FrameCount % 16) * 0.0625f;
	uv.x += frameStep * 4.7526;
	uv.y += frameStep * 3.1914;

	float3 magic = float3(0.06711056f, 0.00583715f, 52.9829189f);
	return frac(magic.z * frac(dot(uv, magic.xy)));
}

float GetScreenDepth(float depth)
{
	return (perPassLLF[0].CameraData.w / (-depth * perPassLLF[0].CameraData.z + perPassLLF[0].CameraData.x));
}

float GetScreenDepth(float2 uv)
{
	float depth = GetDepth(uv);
	return GetScreenDepth(depth);
}

float ContactShadows(float3 rayPos, float2 texcoord, float offset, float3 lightDirectionVS, float radius = 0.0, uint a_eyeIndex = 0)
{
	float lightDirectionMult = 1.5;
	float2 depthDeltaMult = float2(0.20, 0.1);
	uint loopMax = 4;

	if (radius > 0) {  // long
		lightDirectionMult = radius / 32;
		depthDeltaMult = float2(1.0, 0.05);
		loopMax = 32;
	}

	lightDirectionVS *= lightDirectionMult;

	// Offset starting position with interleaved gradient noise
	rayPos += lightDirectionVS * offset;

	// Accumulate samples
	float shadow = 0.0;
	[loop] for (uint i = 0; i < loopMax; i++)
	{
		// Step the ray
		rayPos += lightDirectionVS;
		float2 rayUV = ViewToUV(rayPos, true, a_eyeIndex);

		// Ensure the UV coordinates are inside the screen
		if (!IsSaturated(rayUV))
			break;

		// Compute the difference between the ray's and the camera's depth
		float rayDepth = GetScreenDepth(rayUV);

		// Difference between the current ray distance and the marched light
		float depthDelta = rayPos.z - rayDepth;
		if (rayDepth > 16.5)  // First person
			shadow += saturate(depthDelta * depthDeltaMult.x) - saturate(depthDelta * depthDeltaMult.y);
	}

	return 1.0 - saturate(shadow);
}

#if defined(SKINNED) || defined(ENVMAP) || defined(EYE) || defined(MULTI_LAYER_PARALLAX)
#	define DRAW_IN_WORLDSPACE
#endif
