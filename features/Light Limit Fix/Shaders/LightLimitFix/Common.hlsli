
#define GROUP_SIZE (16 * 8 * 8)
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

float3 GetPositionVS(float2 texcoord, float depth)
{
	float4 clipSpaceLocation;
#ifdef VR
	uint eyeIndex = (texcoord.x > .5);
	// next code should convert between eyes in VR (may not be necessary)
//	texcoord.x = (texcoord.x * 2 - eyeIndex);       // [0, 0.5] -> [0, 1], [0.5, 1] -> [0, 1]
#else
	uint eyeIndex = 0;
#endif                                              //VR
	clipSpaceLocation.xy = texcoord * 2.0f - 1.0f;  // convert from [0,1] to [-1,1]
	clipSpaceLocation.y *= -1;
	clipSpaceLocation.z = depth;
	clipSpaceLocation.w = 1.0f;
	float4 homogenousLocation = mul(clipSpaceLocation, InvProjMatrix[0]);
	return homogenousLocation.xyz / homogenousLocation.w;
}
