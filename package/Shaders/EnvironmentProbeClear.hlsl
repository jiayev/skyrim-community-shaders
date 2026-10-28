#include "Common/EnvironmentProbe.hlsli"

#ifdef VSHADER
float4 main(uint id : SV_VertexID) : SV_Position
{
	float2 uv = float2((id << 1) & 2, id & 2);
	return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
#else
struct Output
{
	float4 Radiance: SV_Target0;
	float Depth: SV_Target1;
	float2 Normal: SV_Target2;
};

Output main(float4 position : SV_Position)
{
	uint face = EnvironmentProbe::PixelFace(position.xy);
	if ((EnvironmentProbe::FaceMask & (1u << face)) == 0)
		discard;
	Output output;
	output.Radiance = 0;
	output.Depth = EnvironmentProbe::FarDistance;
	output.Normal = 0;
	return output;
}
#endif
