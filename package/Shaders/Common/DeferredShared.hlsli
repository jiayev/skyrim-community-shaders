#ifndef __DEFERRED_SHARED_DEPENDENCY_HLSL__
#define __DEFERRED_SHARED_DEPENDENCY_HLSL__

cbuffer PerFrameDeferredShared : register(b11)
{
	float4 BufferDim;
	float4 CameraData;
	row_major float3x4 DirectionalAmbient;
	uint FrameCount;
	uint FrameCountAlwaysActive;
};

namespace DeferredShared
{
	float GetScreenDepth(float depth)
	{
		return (CameraData.w / (-depth * CameraData.z + CameraData.x));
	}
}

#endif  // __DEFERRED_SHARED_DEPENDENCY_HLSL__