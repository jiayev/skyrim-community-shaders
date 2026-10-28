#ifndef ENVIRONMENT_PROBE_HLSLI
#	include "EnvironmentProbe.hlsli"
#endif

// ProbeVertex and the member aliases are generated from the material VS output
// signature. Preserve every material varying, including feature-specific ones.
[instance(8)]
	[maxvertexcount(3)] void
	main(triangle ProbeVertex input[3], uint face : SV_GSInstanceID, inout TriangleStream<ProbeVertex> output) {
		if ((EnvironmentProbe::FaceMask & (1u << face)) == 0)
			return;
		// Reject before exporting the material varyings. The octant projection has the
		// same winding in all eight faces; positions already include skinning/animation.
		float3 p0 = input[0].PROBE_WORLD.xyz;
		float3 p1 = input[1].PROBE_WORLD.xyz;
		float3 p2 = input[2].PROBE_WORLD.xyz;
		float winding = dot(cross(p1 - p0, p2 - p0), p0);
		if ((EnvironmentProbe::CullBackface == 1 && winding <= 0) || (EnvironmentProbe::CullBackface == 2 && winding >= 0))
			return;
		float3 sign = float3(face & 1 ? -1 : 1, face & 2 ? -1 : 1, face & 4 ? -1 : 1);
		float3 clip0 = EnvironmentProbe::ClipDistances(input[0].PROBE_WORLD.xyz, sign);
		float3 clip1 = EnvironmentProbe::ClipDistances(input[1].PROBE_WORLD.xyz, sign);
		float3 clip2 = EnvironmentProbe::ClipDistances(input[2].PROBE_WORLD.xyz, sign);
		if (any(max(clip0, max(clip1, clip2)) < 0))
			return;
		[unroll] for (uint i = 0; i < 3; ++i)
		{
			ProbeVertex vertex = input[i];
			vertex.PROBE_POSITION = EnvironmentProbe::Project(vertex.PROBE_WORLD.xyz, sign);
			vertex.PROBE_CLIP = EnvironmentProbe::ClipDistances(vertex.PROBE_WORLD.xyz, sign);
			vertex.PROBE_FACE = face;
			output.Append(vertex);
		}
	}
