#ifndef __LLF_ORG_BINDLESS_DEPENDENCY_HLSL__
#define __LLF_ORG_BINDLESS_DEPENDENCY_HLSL__

// Resource and constant bindings for the OpenRenderGraph (Vulkan, bindless) build of
// the cluster shaders. Constants arrive as push data; buffers are fetched from the
// descriptor heap by index. Layout must match OrgClusterConstants in ORGLightCulling.h.

cbuffer LLFOrgConstants : register(b0)
{
	row_major float4x4 LLFCameraMatrix;  // building: CameraProjInverse, culling: CameraView
	float LightsNear;
	float LightsFar;
	uint LightCount;
	uint pad0;
	uint4 ClusterSize;
	uint ClustersIndex;
	uint LightsIndex;
	uint LightIndexCounterIndex;
	uint LightIndexListIndex;
	uint LightGridIndex;
	uint3 pad1;
}

float3 LLFGetPositionVS(float2 texcoord, float depth)
{
	float4 clipSpaceLocation;
	clipSpaceLocation.xy = texcoord * 2.0f - 1.0f;
	clipSpaceLocation.y *= -1;
	clipSpaceLocation.z = depth;
	clipSpaceLocation.w = 1.0f;
	float4 homogenousLocation = mul(LLFCameraMatrix, clipSpaceLocation);
	return homogenousLocation.xyz / homogenousLocation.w;
}

float3 LLFWorldToView(float3 positionWS)
{
	return mul(LLFCameraMatrix, float4(positionWS, 1.0)).xyz;
}

#endif  //__LLF_ORG_BINDLESS_DEPENDENCY_HLSL__
