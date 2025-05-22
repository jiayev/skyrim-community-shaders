// Raymarching Pass

#include "Common/FrameBuffer.hlsli"
#include "Common/SharedData.hlsli"
#include "LightLimitFix/LightLimitFix.hlsli"

Texture2D<float4> texblurredLinearDepth : register(t0);
Texture2D<float> texDepth : register(t1);

RWTexture2D<float4> outShadow : register(u0);

SamplerState linearSampler : register(s0);

cbuffer blurBuffer : register(b1)
{
	uint MipLevel;
	float Scale;
	uint ResX;
	uint ResY;
};

#define UNIT_TO_M_SCALED (0.01428f / Scale)

#define INV_OCCLUSION_DIST_THRESHOLD 0.5f

float3 UVDepthToView(float2 uv, float depth, uint a_eyeIndex = 0)
{
	float2 ndc = uv * 2.0 - 1.0;
	ndc.y = -ndc.y;

	float4 clipPos = float4(ndc, depth, 1.0);

	float4 viewPos = mul(FrameBuffer::CameraProjInverse[a_eyeIndex], clipPos);

	return viewPos.xyz / viewPos.w;
}

float3 ViewToWorld(float3 x, bool is_position = true, uint a_eyeIndex = 0)
{
	float4 newPosition = float4(x, (float)is_position);
	return mul(FrameBuffer::CameraViewInverse[a_eyeIndex], newPosition).xyz;
}

float3 ViewToScreenCoord(float3 x, bool is_position = true, uint a_eyeIndex = 0)
{
	float4 newPosition = float4(x, (float)is_position);
	float4 uv = mul(FrameBuffer::CameraProj[a_eyeIndex], newPosition);
	return float3((uv.xy / uv.w) * float2(0.5f, -0.5f) + 0.5f, uv.z / uv.w);
}

int GetLevelStartMultipleScale(int mip_level)
{
	int level_mult = 8;
	return int((1 - pow(level_mult, mip_level)) / (1 - level_mult));
}

[numthreads(8, 8, 1)] void main(const uint2 dtid
								: SV_DispatchThreadID) {
	float2 texCoord = (dtid + 0.5) / float2(ResX, ResY);

	float depth = texDepth.SampleLevel(linearSampler, texCoord, MipLevel).x;

	float3 viewPos = UVDepthToView(texCoord, depth);

	uint numClusteredLights = 0;
	uint totalLightCount = LightLimitFix::NumStrictLights;
	uint clusterIndex = 0;
	uint lightOffset = 0;
	if (LightLimitFix::GetClusterIndex(texCoord, viewPos.z, clusterIndex)) {
		numClusteredLights = LightLimitFix::lightGrid[clusterIndex].lightCount;
		totalLightCount += numClusteredLights;
		lightOffset = LightLimitFix::lightGrid[clusterIndex].offset;
	}

	uint lightCount = 4;

	float4 shadow = float4(1, 1, 1, 1);

	for (uint lightIndex = 0; lightIndex < lightCount; lightIndex++) {
		LightLimitFix::Light light;
		if (lightIndex < LightLimitFix::NumStrictLights) {
			light = LightLimitFix::StrictLights[lightIndex];
		} else {
			uint clusteredLightIndex = LightLimitFix::lightList[lightOffset + (lightIndex - LightLimitFix::NumStrictLights)];
			light = LightLimitFix::lights[clusteredLightIndex];

			if (LightLimitFix::IsLightIgnored(light)) {
				continue;
			}
		}

		float opacity = 1.0f;

		float3 lightPos = light.positionWS[0].xyz - FrameBuffer::CameraPosAdjust[0].xyz;
		// float3 lightPos = float3(-384,230,202) - FrameBuffer::CameraPosAdjust[0].xyz;
		float3 lightView = FrameBuffer::WorldToView(lightPos);

		float3 view_ray_start = viewPos;
		float3 view_ray_end = lightView;

		float3 start_screen_coord = float3(texCoord, depth);
		float3 end_screen_coord = ViewToScreenCoord(lightView);

		float total_screen_path = length(end_screen_coord - start_screen_coord);
		float2 screen_dir = (end_screen_coord.xy - start_screen_coord.xy) / total_screen_path;
		float3 view_dir = view_ray_end - view_ray_start;
		float3 view_dir_normalize = view_dir / total_screen_path;

		float screen_step = 0.001f * pow(2, MipLevel - 1);

		float level_path_scale = 0.01;
		float start_offset = max(0.0f, min(total_screen_path, level_path_scale * GetLevelStartMultipleScale(int(max(0, MipLevel)))));
		float end_offset = min(total_screen_path, level_path_scale * GetLevelStartMultipleScale(int(max(0, MipLevel + 1) + screen_step)));

		float3 prev_view_coord = 0.0f;

		for (float screen_offset = start_offset; screen_offset < end_offset; screen_offset += screen_step) {
			float2 curr_screen_coord = start_screen_coord.xy + screen_dir * screen_offset;
			float3 curr_view_coord = view_ray_start + view_dir_normalize * screen_offset;

			float view_step = length(curr_view_coord - prev_view_coord) * UNIT_TO_M_SCALED;
			float3 view_start_delta = curr_view_coord - view_ray_start;
			float3 view_end_delta = curr_view_coord - view_ray_end;

			// Sample linear depth
			float3 linear_depth_sample = texblurredLinearDepth.SampleLevel(linearSampler, curr_screen_coord, 0).xyz;

			// Depth mean variance
			float mean = linear_depth_sample.x;
			float variance = max(linear_depth_sample.y - mean * mean, 1e-7f);

			float ray_depth = length(curr_view_coord.xyz) * UNIT_TO_M_SCALED;

			// Chebyshev
			float delta = (ray_depth - mean);
			float probability = 1 - ((delta < 0.0f) ? 1.0f : (variance / (variance + delta * delta)));
			float thick_delta = delta - 100.0f;
			probability -= ((thick_delta > 0.0f) ? 1.0f : (variance / (variance + thick_delta * thick_delta)));
			probability = max(0.0f, probability);

			float density = probability * view_step;
			density /= (1.0f + 0.05f * length(view_start_delta) * UNIT_TO_M_SCALED);
			density *= pow(saturate(length(view_end_delta) * UNIT_TO_M_SCALED * INV_OCCLUSION_DIST_THRESHOLD - 0.5f), 2.0f);
			opacity *= exp(-density);
			prev_view_coord = curr_view_coord;
		}

		if (lightIndex == 0)
			shadow.x = opacity;
		else if (lightIndex == 1)
			shadow.y = opacity;
		else if (lightIndex == 2)
			shadow.z = opacity;
		else if (lightIndex == 3)
			shadow.w = opacity;

		// if (abs(SharedData::GetScreenDepth(depth) - SharedData::GetScreenDepth(end_screen_coord.z)) < 10) shadow = 1;
		// else shadow = 0;
		// shadow.xyz = end_screen_coord;
		// shadow.w = 1;
	}

	if (MipLevel < 3) {
		float4 lastShadow = outShadow[dtid];
		shadow = shadow * lastShadow;
	}

	outShadow[dtid] = shadow;
}