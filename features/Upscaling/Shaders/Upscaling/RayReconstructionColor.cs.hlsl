#include "Common/ColorManagement.hlsli"

RWTexture2D<float4> ColorRW : register(u0);

[numthreads(8, 8, 1)] void main(uint3 tid : SV_DispatchThreadID) {
	uint2 size = (uint2)SharedData::BufferDim.xy;
	if (any(tid.xy >= size))
		return;
	float4 color = ColorRW[tid.xy];
#ifdef TO_SCENE
	color.rgb = ColorManagement::LinearToScene(color.rgb);
#else
	color.rgb = ColorManagement::SceneToLinear(color.rgb);
#endif
	ColorRW[tid.xy] = color;
}
