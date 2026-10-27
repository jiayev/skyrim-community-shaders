#ifndef ENVIRONMENT_PROBE_HLSLI
#define ENVIRONMENT_PROBE_HLSLI

namespace EnvironmentProbe
{
	cbuffer Capture : register(b8)
	{
		float3 FaceSign;
		uint FaceMask;
		float NearDistance;
		float FarDistance;
		uint LightCount;
		uint TextureSize;
		float3 WorldOriginDelta;  // probe origin minus the origin of cached world features
		float CapturePadding2;
		float3 LightOriginDelta;  // probe origin minus Light Limit Fix's light-buffer origin
		float CapturePadding3;
	};

	// Within one octant the octahedral map is projective, not a per-vertex nonlinear warp.
	// w = dot(sign, p) preserves perspective-correct interpolation. SV_ClipDistance clips
	// crossings at x/y/z = 0 before rasterization, including the lower-hemisphere folds.
	float4 Project(float3 p)
	{
		float w = dot(FaceSign, p);
		float2 q = FaceSign.z > 0 ? p.xy : p.xy + FaceSign.xy * FaceSign.z * p.z;
		return float4(q.x, -q.y, 0.5 * w, w);
	}

	float3 ClipDistances(float3 p)
	{
		// Expand only the clip planes by one texel. A fragment ownership test below
		// resolves the shared edges, so rendering a single face cannot lose pixels to
		// D3D's top-left rule or overwrite the neighboring, unselected face.
		return FaceSign * p + dot(FaceSign, p) / max(TextureSize, 4u);
	}

	uint PixelFace(float2 position)
	{
		// Integer pixel centers make ownership exact even on axes and diagonals.
		int2 p = int2(position) * 2 + 1 - int(TextureSize);
		return (p.x < 0 ? 1u : 0u) | (p.y < 0 ? 2u : 0u) | (abs(p.x) + abs(p.y) > int(TextureSize) ? 4u : 0u);
	}

	float3 Decode(float2 uv)
	{
		float2 p = uv * 2.0 - 1.0;
		float3 n = float3(p, 1.0 - abs(p.x) - abs(p.y));
		float t = saturate(-n.z);
		n.xy += float2(n.x >= 0 ? -t : t, n.y >= 0 ? -t : t);
		return normalize(n);
	}

	uint GetFace(float3 direction)
	{
		return (direction.x < 0 ? 1u : 0u) | (direction.y < 0 ? 2u : 0u) | (direction.z < 0 ? 4u : 0u);
	}
}
#endif
