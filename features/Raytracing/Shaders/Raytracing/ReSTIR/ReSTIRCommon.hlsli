#ifndef RESTIR_COMMON_HLSLI
#define RESTIR_COMMON_HLSLI

#include "Common/Game.hlsli"
#include "Raytracing/Includes/RT/CommonRT.hlsli"

struct LightDX11
{
	float3 Vector;
	float Range;
	float3 Color;
	uint TypeISL;
    
    void GetTypeISL(out uint type, out uint isl)
    {
        type =  TypeISL & 0xFFFF;          // low 16 bits
        isl  = (TypeISL >> 16) & 0xFFFF;   // high 16 bits      
    }
};

Texture2D<float> DepthTexture : register(t1);
Texture2D<float4> NormalGlossinessTexture : register(t2);
StructuredBuffer<LightDX11> Lights : register(t3);

cbuffer ReSTIRCB : register(b1)
{
    uint SpatialReuse;
    uint TemporalReuse;
    uint InitialCandidateCount;
    uint LightCount;
    uint MaxCandidateCount;
    uint3 Padding;
};

#define Reservoir float4
// Reservoir.x = W (sum of weights)
// Reservoir.y = light index
// Reservoir.z = M (number of samples considered)
// Reservoir.w = the final adjusted weight for the current pixel following the formula in algorithm 3 (r.W)

#define DEPTH_THRESHOLD 0.1f
#define NORMAL_THRESHOLD 0.5f

#define NEIGHBOURS_COUNT 15
#define NEIGHBOURS_RANGE 5

Reservoir UpdateReservoir(Reservoir reservoir, int lightIndex, float weight, inout uint randSeed)
{
    reservoir.x += weight; // Update W
    reservoir.z += 1.0;   // Update M

    if (Random(randSeed) < weight / reservoir.x)
    {
        reservoir.y = lightIndex; // Update selected light index
    }
    return reservoir;
}

float GetLightWeight(LightDX11 light, float3 normalWS, float3 positionWS)
{
    float3 L = light.Vector - positionWS;
    float distance = length(L);
    L = L / distance;
    distance *= GAME_UNIT_TO_M;
    float NdotL = saturate(dot(normalWS, L));
    float p_hat = NdotL * light.Color / (distance * distance);
    return p_hat;
}

bool IsValidNeighbor(float3 neighborNormal, float neighborDepth, float3 normal, float depth)
{
    float checkNormal = dot(normal, neighborNormal);
    float checkDepth = abs(depth - neighborDepth);
    return checkNormal > NORMAL_THRESHOLD && checkDepth < DEPTH_THRESHOLD * depth;
}

#if defined(DX11)
#include "Common/FrameBuffer.hlsli"

void ReprojectHit(Texture2D<half2> MotionTexture, SamplerState s, float3 uvz, uint eyeIndex, out float2 outPrevUV)
{
	// Camera motion for pixel (in ScreenPos space).
	float2 thisScreen = (uvz.xy - 0.5f) * float2(2.0f, -2.0f);
	float4 thisClip = float4(thisScreen, uvz.z, 1);
    float4 thisView = mul(FrameBuffer::CameraProjUnjitteredInverse[eyeIndex], thisClip);
    thisView.xyz = thisView.xyz / thisView.w;
    float4 thisWorld = mul(FrameBuffer::CameraViewInverse[eyeIndex], float4(thisView.xyz, 1.0f));
    thisWorld.xyz = thisWorld.xyz / thisWorld.w;
	float4 prevClip = mul(FrameBuffer::CameraPreviousViewProjUnjittered[eyeIndex], float4(thisWorld.xyz, 1.0f));
	float2 prevScreen = prevClip.xy / prevClip.w;

	float2 velocity = MotionTexture.SampleLevel(s, uvz.xy, 0).xy;

	prevScreen = thisClip.xy + velocity * float2(2.f, -2.f);

	float2 prevUV = prevScreen.xy * float2(0.5f, -0.5f) + 0.5f;
	
	outPrevUV = prevUV;
}
#endif

#endif