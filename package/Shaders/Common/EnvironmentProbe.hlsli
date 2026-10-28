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
		uint CullBackface;
		float3 LightOriginDelta;  // probe origin minus Light Limit Fix's light-buffer origin
		uint ReceiveSunShadows;
		column_major float4x4 SunProjection[3];
		float4 SunOrigin;
		float4 SunViewDepth;
		float3 SunEndSplits;
		uint SunCascadeCount;
		float4 SunStartSplits;
		column_major float4x4 CoarseSunProjection;
		float CoarseSunBias;
		uint CoarseSunEnabled;
		float2 CoarseSunPadding;
	};

	// Within one octant the octahedral map is projective, not a per-vertex nonlinear warp.
	// w = dot(sign, p) preserves perspective-correct interpolation. SV_ClipDistance clips
	// crossings at x/y/z = 0 before rasterization, including the lower-hemisphere folds.
	float4 Project(float3 p, float3 faceSign)
	{
		float w = dot(faceSign, p);
		float2 q = faceSign.z > 0 ? p.xy : p.xy + faceSign.xy * faceSign.z * p.z;
		// Reversed infinite depth enables hardware early-Z. Along each pixel's ray,
		// w is proportional to radial distance, so this preserves visibility order.
		// Radial distance remains a separate color output; do not write SV_Depth.
		return float4(q.x, -q.y, NearDistance, w);
	}

	float3 ClipDistances(float3 p, float3 faceSign)
	{
		// Expand only the clip planes by one texel. A fragment ownership test below
		// resolves the shared edges, so rendering a single face cannot lose pixels to
		// D3D's top-left rule or overwrite the neighboring, unselected face.
		return faceSign * p + dot(faceSign, p) / max(TextureSize, 4u);
	}

	float4 Project(float3 p) { return Project(p, FaceSign); }
	float3 ClipDistances(float3 p) { return ClipDistances(p, FaceSign); }

	uint PixelFace(float2 position)
	{
		// Integer pixel centers make ownership exact even on axes and diagonals.
		int2 p = int2(position) * 2 + 1 - int(TextureSize);
		return (p.x < 0 ? 1u : 0u) | (p.y < 0 ? 2u : 0u) | (abs(p.x) + abs(p.y) > int(TextureSize) ? 4u : 0u);
	}

	float2 Encode(float3 direction)
	{
		direction /= dot(abs(direction), 1.0.xxx);
		float2 p = direction.xy;
		if (direction.z < 0)
			p = (1.0 - abs(p.yx)) * float2(p.x >= 0 ? 1 : -1, p.y >= 0 ? 1 : -1);
		return p * 0.5 + 0.5;
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
