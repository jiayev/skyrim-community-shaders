#include "../package/Shaders/Common/EnvironmentProbe.hlsli"

struct Vertex
{
	float4 position: SV_Position;
	float3 world: TEXCOORD0;
	float3 clipPlanes: SV_ClipDistance0;
};

Vertex VS(float3 position : POSITION)
{
	Vertex result;
	result.position = EnvironmentProbe::Project(position);
	result.clipPlanes = EnvironmentProbe::ClipDistances(position);
	result.world = position;
	return result;
}

struct Output
{
	float4 radiance: SV_Target0;
	float distance: SV_Target1;
	float depth: SV_Depth;
};

Output PS(Vertex input)
{
	if (EnvironmentProbe::PixelFace(input.position.xy) != EnvironmentProbe::GetFace(EnvironmentProbe::FaceSign))
		discard;
	float radius = length(input.world);
	clip(radius - EnvironmentProbe::NearDistance);
	clip(EnvironmentProbe::FarDistance - radius);
	Output result;
	result.radiance = float4(input.world, 1);
	result.distance = radius;
	result.depth = radius / EnvironmentProbe::FarDistance;
	return result;
}
