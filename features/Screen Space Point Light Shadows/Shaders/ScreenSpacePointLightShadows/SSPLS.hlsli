#ifndef SSPLS_COMMON
#define SSPLS_COMMON

#include "Common/SharedData.hlsli"
#include "Common/FrameBuffer.hlsli"

#define UNIT_TO_M_SCALED (0.01428f / SharedData::ssplsSettings.Scale)
#define INV_OCCLUSION_DIST_THRESHOLD 0.5f

namespace ScreenSpacePointLightShadows
{
    Texture2D<float4> SSPLSTexture : register(t56);
    Texture2D<float4> BlurredLinearDepthTexture : register(t57);

    RWTexture2D<float4> SSPLSShadowTexture0 : register(u7);
    RWTexture2D<float4> SSPLSShadowTexture1 : register(u8);
    RWTexture2D<float4> SSPLSShadowTexture2 : register(u9);
    RWTexture2D<float4> SSPLSShadowTexture3 : register(u10);

    // float GetShadow(SamplerState s, float2 uv, int lightIndex)
    // {
    //     float4 shadow = ScreenSpacePointLightShadows::SSPLSTexture.SampleLevel(s, uv, 0);
    //     float result[4] = { shadow.x, shadow.y, shadow.z, shadow.w };
    //     return result[lightIndex];
    // }

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

    float RayMarch(float3 view_ray_start, float3 view_ray_end, float3 start_screen_coord, float3 end_screen_coord, uint mip_level, SamplerState s)
    {
        float opacity = 1.0;

        const float total_screen_path = length(end_screen_coord - start_screen_coord);
        const float2 screen_dir = (end_screen_coord.xy - start_screen_coord.xy) / total_screen_path;
        const float3 view_dir = view_ray_end - view_ray_start;
        const float3 view_dir_normalize = view_dir / total_screen_path;

        const float screen_step = 0.001f * pow(2, mip_level - 1);

        const float level_path_scale = 0.01;
        const float start_offset = max(0.0f, min(total_screen_path, level_path_scale * GetLevelStartMultipleScale(int(max(0, mip_level)))));
        const float end_offset = min(total_screen_path, level_path_scale * GetLevelStartMultipleScale(int(max(0, mip_level + 1) + screen_step)));

        float3 prev_view_coord = 0.0f;

        for(float screen_offset = start_offset; screen_offset < end_offset; screen_offset += screen_step)
        {
            float2 curr_screen_coord = start_screen_coord.xy + screen_dir * screen_offset;
            float3 curr_view_coord = view_ray_start + view_dir_normalize * screen_offset;

            float view_step = length(curr_view_coord - prev_view_coord) * UNIT_TO_M_SCALED;
            float3 view_start_delta = curr_view_coord - view_ray_start;
            float3 view_end_delta = curr_view_coord - view_ray_end;
            
            // Sample linear depth
            float3 linear_depth_sample = BlurredLinearDepthTexture.SampleLevel(s, curr_screen_coord, 0).xyz;

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

        return opacity;
    }

    float GetShadow(SamplerState s, float3 viewPosition, float3 lightDirectionVS, float2 uv)
    {
        const float3 view_ray_start = viewPosition;
        const float3 view_ray_end = lightDirectionVS;
        uint2 sampleCoord = SharedData::ConvertUVToSampleCoord(uv, 0).xy;

        const float3 start_screen_coord = float3(FrameBuffer::ViewToUV(view_ray_start), SharedData::GetDepth(FrameBuffer::ViewToUV(view_ray_start)));
        const float3 end_screen_coord = ViewToScreenCoord(lightDirectionVS);
        
        float shadow = 1.0f;

        for (int mip_level = 0; mip_level < 4; mip_level++)
        {
            if (sampleCoord.x % pow(2, mip_level) == 0 && sampleCoord.y % pow(2, mip_level) == 0)
            {
                const float opacity = RayMarch(view_ray_start, view_ray_end, start_screen_coord, end_screen_coord, mip_level, s);
                shadow *= opacity;
            }
        }

        return shadow;
    }
}
#endif // SSPLS_COMMON