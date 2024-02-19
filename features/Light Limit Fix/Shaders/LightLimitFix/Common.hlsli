
#define GROUP_SIZE (16 * 16 * 4)
#define MAX_CLUSTER_LIGHTS 128

#define CLUSTER_BUILDING_DISPATCH_SIZE_X 16
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
	uint firstPersonShadow;
};