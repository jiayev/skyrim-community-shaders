struct LightGrid
{
    uint offset;
    uint lightCount;
};

struct StructuredLight
{
	float3  color;
    float   radius;
	float3  positionWS;
    float3  positionVS;
    bool    firstPerson;
    uint    pad;
};

struct PerPassLLF
{
    uint EnableGlobalLights;
    float CameraNear;
    float CameraFar;
    float4 CameraData;
    float2 BufferDim;
    uint FrameCount;
    uint EnableShadows;
};

StructuredBuffer<StructuredLight>   lights          : register(t17);
StructuredBuffer<uint>              lightList       : register(t18); //MAX_CLUSTER_LIGHTS * 16^3
StructuredBuffer<LightGrid>         lightGrid       : register(t19); //16^3
Texture2D<float4>                   TexDepthSampler : register(t20);

StructuredBuffer<PerPassLLF>        perPassLLF : register(t32);

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
    // effects like screen space shadows, can get artefacts if a point sampler is used
    return TexDepthSampler.SampleLevel(SampColorSampler, uv, 0).r;
}

bool IsSaturated(float value)  { return value == saturate(value); }
bool IsSaturated(float2 value) { return IsSaturated(value.x) && IsSaturated(value.y); }

// Derived from the interleaved gradient function from Jimenez 2014 http://goo.gl/eomGso
float InterleavedGradientNoise(float2 uv)
{
    // Temporal factor
    float frameStep  = float(perPassLLF[0].FrameCount % 16) * 0.0625f;
    uv.x     += frameStep * 4.7526;
    uv.y     += frameStep * 3.1914;

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

float ContactShadows(float3 rayPos, float2 texcoord, float offset, float3 lightDirectionVS)
{   
    lightDirectionVS *= 1.5;
  
    //Offset starting position with interleaved gradient noise
    rayPos      += lightDirectionVS * offset;

    float3 rayPosStep[4];
    rayPosStep[0] = rayPos + lightDirectionVS;
    rayPosStep[1] = rayPosStep[0] + lightDirectionVS;
    rayPosStep[2] = rayPosStep[1] + lightDirectionVS;
    rayPosStep[3] = rayPosStep[2] + lightDirectionVS;

    float4 rayUVStep[2];
    rayUVStep[0].xy = ViewToUV(rayPosStep[0]);
    rayUVStep[0].zw = ViewToUV(rayPosStep[1]);
    rayUVStep[1].xy = ViewToUV(rayPosStep[2]);
    rayUVStep[1].zw = ViewToUV(rayPosStep[3]);

    float4 depthSteps;
    depthSteps.x = GetDepth(rayUVStep[0].xy);
    depthSteps.y = GetDepth(rayUVStep[0].zw);
    depthSteps.z = GetDepth(rayUVStep[1].xy);
    depthSteps.w = GetDepth(rayUVStep[1].zw);

    float4 depthDelta;
    depthDelta.x = rayPosStep[0].z - GetScreenDepth(depthSteps.x);
    depthDelta.y = rayPosStep[1].z - GetScreenDepth(depthSteps.y);
    depthDelta.z = rayPosStep[2].z - GetScreenDepth(depthSteps.z);
    depthDelta.w = rayPosStep[3].z - GetScreenDepth(depthSteps.w);

    depthDelta.xyzw = (saturate(depthDelta.xyzw) - saturate(depthDelta.xyzw / (depthSteps.xyzw * 10)));
    
    depthDelta.xyzw *= float4(
            IsSaturated(rayUVStep[0].xy), 
            IsSaturated(rayUVStep[0].zw),
            IsSaturated(rayUVStep[1].xy),
            IsSaturated(rayUVStep[1].zw)
        );

    return 1.0 - saturate((depthDelta.x + depthDelta.y + depthDelta.z + depthDelta.w) * 1.5);
}

float ContactShadowsLong(float3 rayPos, float2 texcoord, float offset, float3 lightDirectionVS, float radius)
{     
    lightDirectionVS *= radius / 16;

    // Offset starting position with interleaved gradient noise
    rayPos      += lightDirectionVS * offset;

    // Accumulate samples
    float shadow = 0.0;
    [loop]
    for (uint i = 0; i < 16; i++)
    {
        // Step the ray
        rayPos += lightDirectionVS;
        float2 rayUV  = ViewToUV(rayPos);

        // Ensure the UV coordinates are inside the screen
        if (!IsSaturated(rayUV))
            break;

        // Compute the difference between the ray's and the camera's depth
        float rayDepth =  GetScreenDepth(rayUV);

        // Difference between the current ray distance and the marched light
        float depthDelta = rayPos.z - rayDepth;
        if (rayDepth > 16.5)
            shadow += saturate(depthDelta) - saturate(depthDelta * 0.025);
    }
    
    return 1.0 - saturate(shadow);
}

#if defined(SKINNED) || defined(ENVMAP) || defined(EYE) || defined(MULTI_LAYER_PARALLAX)
#define DRAW_IN_WORLDSPACE
#endif
