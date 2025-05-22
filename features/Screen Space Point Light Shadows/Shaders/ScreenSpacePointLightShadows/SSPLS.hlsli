#ifndef SSPLS_COMMON
#define SSPLS_COMMON

namespace ScreenSpacePointLightShadows
{
	Texture2D<float4> SSPLSTexture : register(t56);

	float GetShadow(SamplerState s, float2 uv, int lightIndex)
	{
		float4 shadow = ScreenSpacePointLightShadows::SSPLSTexture.SampleLevel(s, uv, 0);
		float result[4] = { shadow.x, shadow.y, shadow.z, shadow.w };
		return result[lightIndex];
	}
}
#endif  // SSPLS_COMMON