
#define GROUP_SIZE (32 * 16 * 2)
#define MAX_CLUSTER_LIGHTS 1024

#define CLUSTER_BUILDING_DISPATCH_SIZE_X 32
#define CLUSTER_BUILDING_DISPATCH_SIZE_Y 16
#define CLUSTER_BUILDING_DISPATCH_SIZE_Z 16

struct ClusterAABB
{
	float4 minPoint;
	float4 maxPoint;
};

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

cbuffer PerFrame : register(b0)
{
	row_major float4x4 InvProjMatrix[2];
	float CameraNear;
	float CameraFar;
	float pad[2];
}

float3 GetPositionVS(float2 texcoord, float depth, int eyeIndex = 0)
{
	float4 clipSpaceLocation;
	clipSpaceLocation.xy = texcoord * 2.0f - 1.0f;  // convert from [0,1] to [-1,1]
	clipSpaceLocation.y *= -1;
	clipSpaceLocation.z = depth;
	clipSpaceLocation.w = 1.0f;
	float4 homogenousLocation = mul(clipSpaceLocation, InvProjMatrix[eyeIndex]);
	return homogenousLocation.xyz / homogenousLocation.w;
}
