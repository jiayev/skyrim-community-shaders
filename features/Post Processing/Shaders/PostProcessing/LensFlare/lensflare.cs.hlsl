// Originally from PotatoFX by Gimle Larpes
/*
MIT License

Copyright (c) 2023 Gimle Larpes

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/


// Constants
static const float PI = 3.14159265359;
static const float EPSILON = 1e-6;

// Textures & Samplers
Texture2D<float4> InputTexture : register(t0);
Texture2D<float4> FlareTexture : register(t1);
SamplerState ColorSampler : register(s0);
SamplerState ResizeSampler : register(s1);
RWTexture2D<float4> OutputTexture : register(u0);

cbuffer LensFlareConstants : register(b1)
{
    float GhostStrength;
    float HaloStrength;
    float HaloRadius;
    float HaloWidth;
    float LensFlareCA;
    float LFStrength;
    bool GLocalMask;
    float ScreenWidth;
    float ScreenHeight;
    int DownsizeScale;
}

static const float4 GHOST_COLORS[8] = 
{
    float4(1.0f, 0.8f, 0.4f, 1.0f),  // Ghost 1
    float4(1.0f, 1.0f, 0.6f, 1.0f),  // Ghost 2
    float4(0.8f, 0.8f, 1.0f, 1.0f),  // Ghost 3
    float4(0.5f, 1.0f, 0.4f, 1.0f),  // Ghost 4
    float4(0.5f, 0.8f, 1.0f, 1.0f),  // Ghost 5
    float4(0.9f, 1.0f, 0.8f, 1.0f),  // Ghost 6
    float4(1.0f, 0.8f, 0.4f, 1.0f),  // Ghost 7
    float4(0.9f, 0.7f, 0.7f, 1.0f)   // Ghost 8
};

static const float GHOST_SCALES[8] = 
{
    -1.5f, 2.5f, -5.0f, 10.0f, 0.7f, -0.4f, -0.2f, -0.1f
};

float4 SampleCA(Texture2D tex, SamplerState samp, float2 texcoord, float strength, uint mipLevel)
{
    float3 influence = float3(0.04, 0.0, 0.03);
    float2 CAr = (texcoord - 0.5) * (1.0 - strength * influence.r) + 0.5;
    float2 CAb = (texcoord - 0.5) * (1.0 + strength * influence.b) + 0.5;

    float4 color;
    color.r = tex.SampleLevel(samp, CAr, mipLevel).r;
    color.ga = tex.SampleLevel(samp, texcoord, mipLevel).ga;
    color.b = tex.SampleLevel(samp, CAb, mipLevel).b;

    return color;
}

float4 KawaseBlurDownSample(Texture2D tex, SamplerState samp, uint2 DTid, int scale)
{
    int DownsizeFrom = max(scale, 1);
    float2 texcoord = (floor((DTid.xy / DownsizeFrom)) * DownsizeFrom + DownsizeFrom * 0.5f) / float2(ScreenWidth, ScreenHeight);
    float2 HALF_TEXEL = float2(1.0f / float2(ScreenWidth, ScreenHeight)) * float(DownsizeFrom);

    float2 DirDiag1 = float2(-HALF_TEXEL.x,  HALF_TEXEL.y); // Top left
    float2 DirDiag2 = float2( HALF_TEXEL.x,  HALF_TEXEL.y); // Top right
    float2 DirDiag3 = float2( HALF_TEXEL.x, -HALF_TEXEL.y); // Bottom right
    float2 DirDiag4 = float2(-HALF_TEXEL.x, -HALF_TEXEL.y); // Bottom left

    float4 color = tex.SampleLevel(samp, texcoord, 0) * 4.0f;
    color += tex.SampleLevel(samp, texcoord + DirDiag1, 0);
    color += tex.SampleLevel(samp, texcoord + DirDiag2, 0);
    color += tex.SampleLevel(samp, texcoord + DirDiag3, 0);
    color += tex.SampleLevel(samp, texcoord + DirDiag4, 0);

    return color * 0.125f;
}

float4 KawaseBlurUpSample(Texture2D tex, SamplerState samp, uint2 DTid, int scale)
{
    int Upscale = max(scale, 1);
    float2 texcoord = (floor((DTid.xy / Upscale)) * Upscale + Upscale * 0.5f) / float2(ScreenWidth, ScreenHeight);
    float2 HALF_TEXEL = float2(1.0f / float2(ScreenWidth, ScreenHeight)) * Upscale;

    float2 DirDiag1 = float2(-HALF_TEXEL.x,  HALF_TEXEL.y); // Top left
    float2 DirDiag2 = float2( HALF_TEXEL.x,  HALF_TEXEL.y); // Top right
    float2 DirDiag3 = float2( HALF_TEXEL.x, -HALF_TEXEL.y); // Bottom right
    float2 DirDiag4 = float2(-HALF_TEXEL.x, -HALF_TEXEL.y); // Bottom left
    float2 DirAxis1 = float2(-HALF_TEXEL.x,  0.0f);          // Left
    float2 DirAxis2 = float2( HALF_TEXEL.x,  0.0f);          // Right
    float2 DirAxis3 = float2(0.0f,  HALF_TEXEL.y);           // Top
    float2 DirAxis4 = float2(0.0f, -HALF_TEXEL.y);           // Bottom

    float4 color = 0.0;
    color += tex.SampleLevel(samp, texcoord + DirDiag1, 2);
    color += tex.SampleLevel(samp, texcoord + DirDiag2, 2);
    color += tex.SampleLevel(samp, texcoord + DirDiag3, 2);
    color += tex.SampleLevel(samp, texcoord + DirDiag4, 2);

    color += tex.SampleLevel(samp, texcoord + DirAxis1, 2) * 2.0f;
    color += tex.SampleLevel(samp, texcoord + DirAxis2, 2) * 2.0f;
    color += tex.SampleLevel(samp, texcoord + DirAxis3, 2) * 2.0f;
    color += tex.SampleLevel(samp, texcoord + DirAxis4, 2) * 2.0f;

    return color / 12.0f;
}

[numthreads(8, 8, 1)]
void CSLensflare(uint3 DTid : SV_DispatchThreadID)
{
    if (DTid.x >= (uint)ScreenWidth || DTid.y >= (uint)ScreenHeight)
        return;
    // float2 texcoord = (DTid.xy + 0.5f) / float2(ScreenWidth, ScreenHeight);
    float2 texcoord = (DTid.xy + 0.5f) / float2(ScreenWidth, ScreenHeight);
    float4 input_color = InputTexture.SampleLevel(ColorSampler, texcoord, 0);
    float weight;
    float4 s = 0.0f;
    float3 color = 0.0f;
    float2 texcoord_clean = texcoord;
    float2 radiant_vector = texcoord - 0.5f;
    float2 halo_vector = texcoord_clean;
    // Ghosts
    [branch]
    if (GhostStrength != 0.0f)
    {
        
        for(int i = 0; i < 8; i++)
        {
            if(abs(GHOST_COLORS[i].a * GHOST_SCALES[i]) > 0.00001f)
            {
                float2 ghost_vector = radiant_vector * GHOST_SCALES[i];

                float distance_mask = 1.0 - length(ghost_vector);
                if (GLocalMask)
				{
            		float mask1 = smoothstep(0.5, 0.9, distance_mask);
            		float mask2 = smoothstep(0.75, 1.0, distance_mask) * 0.95 + 0.05;
					weight = mask1 * mask2;
				}
				else
				{
					weight = distance_mask;
				}
                float4 s1 = 0.0f;
                s1 = InputTexture.SampleLevel(ColorSampler, ghost_vector + 0.5f, 2);
                color += s1.rgb * GHOST_COLORS[i].rgb * GHOST_COLORS[i].a * weight;
            }
        }

        //Screen border mask
        static const float SBMASK_SIZE = 0.9;
        float sb_mask = clamp(length(float2(abs(SBMASK_SIZE * texcoord_clean.x - 0.5), 
                                          abs(SBMASK_SIZE * texcoord_clean.y - 0.5))), 0.0, 1.0);
        color *= sb_mask * (GhostStrength * GhostStrength);
    }

    // Halo
    if (HaloStrength > EPSILON)
    {
        halo_vector -= normalize(radiant_vector) * HaloRadius;
        weight = 1.0 - min(rcp(HaloWidth + EPSILON) * length(0.5 - halo_vector), 1.0);
        weight = pow(abs(weight), 5.0);

        s = SampleCA(InputTexture, ColorSampler, halo_vector, 8.0 * LensFlareCA, 2);
        color += s.rgb * s.a * weight * (HaloStrength * HaloStrength);
    }

    color *= LFStrength * LFStrength;

    OutputTexture[DTid.xy] = float4(color, 1.0f);
}

[numthreads(8, 8, 1)]
void CSFlareDown(uint3 DTid : SV_DispatchThreadID)
{
    OutputTexture[DTid.xy] = KawaseBlurDownSample(FlareTexture, ResizeSampler, DTid.xy, DownsizeScale);
}

[numthreads(8, 8, 1)]
void CSFlareUp(uint3 DTid : SV_DispatchThreadID)
{
    OutputTexture[DTid.xy] = KawaseBlurUpSample(FlareTexture, ResizeSampler, DTid.xy, DownsizeScale);
}

[numthreads(8, 8, 1)]
void CSComposite(uint3 DTid : SV_DispatchThreadID)
{
    float2 texcoord = (DTid.xy + 0.5) / float2(ScreenWidth, ScreenHeight);
    float4 flarecolor = FlareTexture.SampleLevel(ColorSampler, texcoord, 0);
    float4 origincolor = InputTexture.SampleLevel(ColorSampler, texcoord, 0);
    OutputTexture[DTid.xy] = flarecolor + origincolor;
}