#include "../package/Shaders/Common/EnvironmentProbe.hlsli"

struct Vertex
{
	float4 position: SV_Position;
	float3 world: POSITION1;
	nointerpolation uint octant: TEXCOORD13;
	float3 clipPlanes: SV_ClipDistance0;
};

Vertex VS(float3 position : POSITION)
{
	Vertex result;
	result.position = EnvironmentProbe::Project(position);
	result.clipPlanes = EnvironmentProbe::ClipDistances(position);
	result.world = position;
	result.octant = EnvironmentProbe::GetFace(EnvironmentProbe::FaceSign);
	return result;
}

struct Output
{
	float4 radiance: SV_Target0;
	float distance: SV_Target1;
};

Output PS(Vertex input)
{
	if (EnvironmentProbe::PixelFace(input.position.xy) != input.octant)
		discard;
	float radius = length(input.world);
	clip(radius - EnvironmentProbe::NearDistance);
	clip(EnvironmentProbe::FarDistance - radius);
	Output result;
	result.radiance = float4(input.world, 1);
	result.distance = radius;
	return result;
}

#define ProbeVertex Vertex
#define PROBE_POSITION position
#define PROBE_WORLD world
#define PROBE_CLIP clipPlanes
#define PROBE_FACE octant
#include "../package/Shaders/Common/EnvironmentProbeGeometry.hlsli"
