
#define GROUP_SIZE (16*8*8)
#define MAX_CLUSTER_LIGHTS 128

#define CLUSTER_BUILDING_DISPATCH_SIZE_X 16
#define CLUSTER_BUILDING_DISPATCH_SIZE_Y 8
#define CLUSTER_BUILDING_DISPATCH_SIZE_Z 24

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
	float3  color;
    float   radius;
	float3  positionWS;
    float3  positionVS;
    uint    shadowMode;
    uint    pad;
};

cbuffer PerFrame : register(b0)
{
    row_major float4x4 InvProjMatrix; 
    float CameraNear;
    float CameraFar;
    float pad[2];
}

float3 GetPositionVS(float2 texcoord, float depth)
{
    float4 clipSpaceLocation;
    clipSpaceLocation.xy = texcoord * 2.0f - 1.0f;
    clipSpaceLocation.y *= -1;
    clipSpaceLocation.z = depth;
    clipSpaceLocation.w = 1.0f;
    float4 homogenousLocation = mul(clipSpaceLocation, InvProjMatrix);
    return homogenousLocation.xyz / homogenousLocation.w;
}
