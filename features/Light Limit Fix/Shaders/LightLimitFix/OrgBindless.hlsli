#ifndef __LLF_ORG_BINDLESS_DEPENDENCY_HLSL__
#define __LLF_ORG_BINDLESS_DEPENDENCY_HLSL__

// Resource and constant bindings for the OpenRenderGraph (Vulkan, bindless) build of
// the cluster shaders. Buffers are fetched from the descriptor heap by index. The push
// data holds only what is fixed across executions; the camera, the depth range and the
// light count are the execution's, read from its region of the latch block
// (org::LatchBlock), which the host writes just before submission - so the recorded
// dispatch never changes with the frame. Layouts must match OrgClusterConstants and
// LLFLatch in ORGLightCulling.cpp.

cbuffer LLFOrgConstants : register(b0)
{
	uint4 ClusterSize;
	uint ClustersIndex;
	uint LightsIndex;
	uint LightIndexCounterIndex;
	uint LightIndexListIndex;
	uint LightGridIndex;
	uint LatchIndex;    // ByteAddressBuffer: the latch block
	uint LatchOffset;   // this execution's LLFLatch, in bytes
	uint MatrixOffset;  // within it: 0 for CameraProjInverse (building), 64 for CameraView (culling)
}

// The execution's values (LLFLatch), read at the top of main by LLF_DECLARE_RESOURCES.
static float4x4 LLFCameraMatrix;  // building: CameraProjInverse, culling: CameraView (rows)
static float LightsNear;
static float LightsFar;
static uint LightCount;

void LLFLoadLatch()
{
	ByteAddressBuffer latch = ResourceDescriptorHeap[LatchIndex];
	const uint matrix = LatchOffset + MatrixOffset;
	LLFCameraMatrix = float4x4(asfloat(latch.Load4(matrix)), asfloat(latch.Load4(matrix + 16)),
		asfloat(latch.Load4(matrix + 32)), asfloat(latch.Load4(matrix + 48)));
	const uint4 tail = latch.Load4(LatchOffset + 128);
	LightsNear = asfloat(tail.x);
	LightsFar = asfloat(tail.y);
	LightCount = tail.z;
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
