#define PI 3.1415926535
#define HALF_PI 1.570796327

struct AOGenBuffer
{
	float aoDistance;
	uint sliceCount;
	uint sampleCount;

	float3 pos0;
	float3 pos1;
	float2 zRange;
};

RWTexture2D<float> RWTexOcclusion : register(u0);
RWTexture2D<float> RWTexNormalisedHeight : register(u1);

StructuredBuffer<AOGenBuffer> aoGen : register(t0);
Texture2D<float> TexHeightmap : register(t1);

SamplerState HeightmapSampler
{
	Filter = MIN_MAG_MIP_LINEAR;
	AddressU = Clamp;
	AddressV = Clamp;
	AddressW = Clamp;
};

float3 getPos(float2 uv)
{
	float3 pos = float3(uv, TexHeightmap.SampleLevel(HeightmapSampler, uv, 0).x);
	pos = lerp(aoGen[0].pos0.xyz, aoGen[0].pos1.xyz, pos);
	return pos;
}

// https://gist.github.com/bgolus/a07ed65602c009d5e2f753826e8078a0
float3 ReconstructNormal(float2 uv, float2 texelSize)
{
	// get current pixel's view space position
	float3 viewSpacePos_c = getPos(uv + float2(0.0, 0.0) * texelSize);

	// get view space position at 1 pixel offsets in each major direction
	float3 viewSpacePos_l = getPos(uv + float2(-1.0, 0.0) * texelSize);
	float3 viewSpacePos_r = getPos(uv + float2(1.0, 0.0) * texelSize);
	float3 viewSpacePos_d = getPos(uv + float2(0.0, -1.0) * texelSize);
	float3 viewSpacePos_u = getPos(uv + float2(0.0, 1.0) * texelSize);

	// get the difference between the current and each offset position
	float3 l = viewSpacePos_c - viewSpacePos_l;
	float3 r = viewSpacePos_r - viewSpacePos_c;
	float3 d = viewSpacePos_c - viewSpacePos_d;
	float3 u = viewSpacePos_u - viewSpacePos_c;

	// pick horizontal and vertical diff with the smallest z difference
	float3 hDeriv = abs(l.z) < abs(r.z) ? l : r;
	float3 vDeriv = abs(d.z) < abs(u.z) ? d : u;

	// get view space normal from the cross product of the two smallest offsets
	float3 viewNormal = normalize(cross(hDeriv, vDeriv));

	return viewNormal;
}

[numthreads(32, 32, 1)] void main(const uint2 tid
								  : SV_DispatchThreadID) {
	uint2 dims;
	TexHeightmap.GetDimensions(dims.x, dims.y);
	float2 texelSize = rcp(dims);

	uint2 px_coord = tid.xy;
	float2 uv = (px_coord + 0.5) * texelSize;

	float3 normal = -ReconstructNormal(uv, texelSize);
	float3 pos = getPos(uv);
	float3 view = float3(0, 0, 1);

	// helpful constants
	float rcp_sample_count = rcp(aoGen[0].sampleCount);
	float2 world_uv_scale = rcp(aoGen[0].pos1.xy - aoGen[0].pos0.xy);  // delta world pos * world_uv_scale = delta uv;

	float cos_cone = 0;
	float visibility = 0;
	for (uint slice = 0; slice < aoGen[0].sliceCount; slice++) {
		float theta = (PI / aoGen[0].sliceCount) * slice;
		float3 slice_dir = 0;
		sincos(theta, slice_dir.y, slice_dir.x);

		float3 axis_dir = cross(slice_dir, view);
		float3 proj_normal = normal - axis_dir * dot(normal, axis_dir);
		float proj_normal_len = length(proj_normal);

		float sgn_n = sign(dot(slice_dir, proj_normal));
		float cos_n = saturate(dot(proj_normal, view) / proj_normal_len);
		float n = sgn_n * acos(cos_n);

		for (int side = 0; side <= 1; side++) {
			float horizon_cos = -1;
			for (uint samp = 0; samp < aoGen[0].sampleCount; samp++) {
				float dist_ratio = (samp + 1) * rcp_sample_count;
				float2 curr_uv = uv + (2 * side - 1) * dist_ratio * aoGen[0].aoDistance * slice_dir.xy * world_uv_scale;
				float3 curr_pos = getPos(curr_uv);
				float3 horizon_dir = normalize(curr_pos - pos);
				horizon_cos = max(horizon_cos, dot(horizon_dir, view));
			}
			float h = n + clamp((-1 + 2 * side) * acos(horizon_cos) - n, -HALF_PI, HALF_PI);
			visibility += saturate(proj_normal_len * (cos_n + 2 * h * sin(n) - cos(2 * h - n)) * .25);
		}
	}
	visibility /= aoGen[0].sliceCount;

	float norm_z = (pos.z - aoGen[0].zRange.x) / (aoGen[0].zRange.y - aoGen[0].zRange.x);
	RWTexOcclusion[tid] = visibility;
	RWTexNormalisedHeight[tid] = norm_z;
}