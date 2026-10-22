// A local shadow light's map lookup as the vanilla mask shaders make it (Utility.hlsl: RENDER_SHADOWMASKSPOT, RENDER_SHADOWMASKPB,
// RENDER_SHADOWMASKDPB): where a position falls in the map and the depth it is compared against. Shared by Light Limit Fix's local
// shadow cache (LightLimitFix.hlsli, GetLocalShadow) and Drawcall Limit Fix's mask resolve (LocalShadowMaskCS.hlsl), which sample it
// their own ways. Included inside the includer's namespace.

struct LocalShadowData
{
	column_major float4x4 ShadowProj;
	float4 Params;   // x: 0 spot / 1 hemisphere / 2 omni, y: radius (<= 0 = no data), z: depth bias, w: fade-in
	float4 Params2;  // x: spot cone falloff exponent, y: skinned receiver offset toward the light
	float4 Origin;
};

static const uint LOCAL_SHADOW_TYPE_SPOT = 0;
static const uint LOCAL_SHADOW_TYPE_HEMISPHERE = 1;
static const uint LOCAL_SHADOW_TYPE_OMNI = 2;

struct LocalShadowLookup
{
	bool sampled;         // false: unsampled is the visibility
	float unsampled;
	float2 uv;
	float receiverDepth;  // compared against the map: lit where the map's depth is not less
	float2 clampMin;      // the taps' window (an omni map's half)
	float2 clampMax;
	float scale;          // the sampled visibility's factor (a spot light's cone falloff)
};

// shadowPosition is in the space the data's ShadowProj takes (relative to its Origin); texel is the map's texel size in UV.
LocalShadowLookup GetLocalShadowLookup(LocalShadowData data, float3 shadowPosition, float texel)
{
	LocalShadowLookup lookup;
	lookup.sampled = false;
	lookup.unsampled = 1.0;
	lookup.uv = 0.0;
	lookup.receiverDepth = 0.0;
	lookup.clampMin = texel;
	lookup.clampMax = 1.0 - texel;
	lookup.scale = 1.0;

	float4 positionLS = mul(data.ShadowProj, float4(shadowPosition, 1.0));
	const uint shadowType = (uint)data.Params.x;
	[branch] if (shadowType == LOCAL_SHADOW_TYPE_SPOT)
	{
		// The vanilla mask only draws the frustum volume: behind the light and outside the cone is dark.
		lookup.unsampled = 0.0;
		[branch] if (positionLS.w > 1e-4)
		{
			positionLS.xyz /= positionLS.w;
			[branch] if (all(abs(positionLS.xy) < 1.0) && positionLS.z >= 0.0)
			{
				lookup.sampled = true;
				lookup.uv = positionLS.xy * 0.5 + 0.5;
				lookup.receiverDepth = positionLS.z - data.Params.z;
				lookup.scale = saturate(1.0 - pow(length(positionLS.xy), data.Params2.x));
			}
		}
	}
	else
	{
		const bool omni = shadowType == LOCAL_SHADOW_TYPE_OMNI;
		const bool lowerHalf = positionLS.z < -1.0;
		[branch] if (omni || !lowerHalf)
		{
			float3 lightDirection = normalize(normalize(positionLS.xyz) + float3(0.0, 0.0, lowerHalf ? -1.0 : 1.0));
			// Must keep the sign: the lower paraboloid half has a negative axis term, and the
			// flip it produces is what maps that hemisphere. Clamping to a positive epsilon
			// collapses the whole lower half onto one edge texel. Matches vanilla Utility.hlsl.
			float axialTerm = lightDirection.z;
			axialTerm = abs(axialTerm) < 1e-4 ? (axialTerm < 0.0 ? -1e-4 : 1e-4) : axialTerm;
			lookup.uv = lightDirection.xy / axialTerm * 0.5 + 0.5;
			if (omni) {
				lookup.uv.y = lowerHalf ? 1.0 - 0.5 * lookup.uv.y : 0.5 * lookup.uv.y;
				lookup.clampMin.y = lowerHalf ? 0.5 + texel : texel;
				lookup.clampMax.y = lowerHalf ? 1.0 - texel : 0.5 - texel;
			}
			lookup.sampled = true;
			lookup.receiverDepth = saturate(length(positionLS.xyz) / data.Params.y) - data.Params.z;
		}
	}
	return lookup;
}
