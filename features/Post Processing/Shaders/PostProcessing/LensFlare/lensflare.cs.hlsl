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
SamplerState ColorSampler : register(s0);
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
}

float4 GhostColors[8] = 
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

float GhostScales[8] = 
{
    -1.5f, 2.5f, -5.0f, 10.0f, 0.7f, -0.4f, -0.2f, -0.1f
};

float4 SampleCA(Texture2D tex, SamplerState samp, float2 texcoord, float strength)
{
    float3 influence = float3(0.04, 0.0, 0.03);
    float2 CAr = (texcoord - 0.5) * (1.0 - strength * influence.r) + 0.5;
    float2 CAb = (texcoord - 0.5) * (1.0 + strength * influence.b) + 0.5;

    float4 color;
    color.r = tex.SampleLevel(samp, CAr, 0).r;
    color.ga = tex.SampleLevel(samp, texcoord, 0).ga;
    color.b = tex.SampleLevel(samp, CAb, 0).b;

    return color;
}

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    // if (DTid.x >= (uint)ScreenSize.x || DTid.y >= (uint)ScreenSize.y)
    //     return;
    float3 dtidTestColor = float3(ScreenWidth / 2560, ScreenHeight / 1440, 0.0);
    float2 texcoord = (DTid.xy + 0.5) / float2(ScreenWidth, ScreenHeight);
    float4 debug_color = InputTexture.SampleLevel(ColorSampler, texcoord, 0);
    float weight;
    float4 s = 0.0;
    float3 color = 0.0;
    color = debug_color.rgb;
    float2 texcoord_clean = texcoord;
    float2 radiant_vector = texcoord - 0.5;

    // Ghosts
    [branch]
    if (GhostStrength > EPSILON)
    {
        for(int i = 0; i < 8; i++)
        {
            if(abs(GhostColors[i].a * GhostScales[i]) > EPSILON)
            {
                float2 ghost_vector = radiant_vector * GhostScales[i];
                float distance_mask = 1.0 - length(ghost_vector);
                weight = distance_mask;

                float4 s = InputTexture.SampleLevel(ColorSampler, ghost_vector + 0.5, 0);
                color += s.rgb * s.a * GhostColors[i].rgb * GhostColors[i].a * weight;
            }
        }

        static const float SBMASK_SIZE = 0.9;
        float sb_mask = clamp(length(float2(abs(SBMASK_SIZE * texcoord_clean.x - 0.5), 
                                          abs(SBMASK_SIZE * texcoord_clean.y - 0.5))), 0.0, 1.0);
        float3 color2 = color;
        color *= sb_mask * (GhostStrength * GhostStrength);
        color += color2;
    }

    // Halo
    if (HaloStrength > EPSILON)
    {
        float2 halo_vector = texcoord;
        halo_vector -= normalize(radiant_vector) * HaloRadius;
        weight = 1.0 - min(rcp(HaloWidth + EPSILON) * length(0.5 - halo_vector), 1.0);
        weight = pow(abs(weight), 5.0);

        s = SampleCA(InputTexture, ColorSampler, halo_vector, 8.0 * LensFlareCA);
        color += s.rgb * s.a * weight * (HaloStrength * HaloStrength);
    }

    // Final output
    OutputTexture[DTid.xy] = float4(color, 1.0f);
}
