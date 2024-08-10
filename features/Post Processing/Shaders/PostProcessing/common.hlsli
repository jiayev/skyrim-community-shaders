#include "../../Common/Color.hlsli"

float4 KarisAverage(float4 a, float4 b, float4 c, float4 d)
{
	float wa = rcp(1 + RGBToLuminance(a.rgb));
	float wb = rcp(1 + RGBToLuminance(b.rgb));
	float wc = rcp(1 + RGBToLuminance(c.rgb));
	float wd = rcp(1 + RGBToLuminance(d.rgb));
	float wsum = wa + wb + wc + wd;
	return (a * wa + b * wb + c * wc + d * wd) / wsum;
}

// Maybe rewrite as fetch
float4 DownsampleCOD(Texture2D tex, SamplerState samp, float2 uv, float2 out_px_size)
{
	int x, y;

	float4 retval = 0;

	[unroll] for (x = 0; x < 2; ++x)
		[unroll] for (y = 0; y < 2; ++y)
			retval += 0.125 * tex.SampleLevel(samp, uv + (int2(x, y) - .5) * out_px_size, 0);

	// const static float weights[9] = { 0.03125, 0.625, 0.03125, 0.625, 0.125, 0.625, 0.03125, 0.625, 0.03125 };
	// corresponds to (1 << (!x + !y)) * 0.03125 when $x,y \in [-1, 1] \cap \mathbb N$
	[unroll] for (x = -1; x <= 1; ++x)
		[unroll] for (y = -1; y <= 1; ++y)
			retval += (1u << (!x + !y)) * 0.03125 * tex.SampleLevel(samp, uv + int2(x, y) * out_px_size, 0);

	return retval;
}

float4 DownsampleCODFirstMip(Texture2D tex, SamplerState samp, float2 uv, float2 out_px_size)
{
	int x, y;

	float4 retval = 0;
	float4 fetches2x2[4];
	float4 fetches3x3[9];

	[unroll] for (x = 0; x < 2; ++x)
		[unroll] for (y = 0; y < 2; ++y)
			fetches2x2[x * 2 + y] = tex.SampleLevel(samp, uv + (int2(x, y) * 2 - 1) * out_px_size, 0);
	[unroll] for (x = 0; x < 3; ++x)
		[unroll] for (y = 0; y < 3; ++y)
			fetches3x3[x * 3 + y] = tex.SampleLevel(samp, uv + (int2(x, y) - 1) * 2 * out_px_size, 0);

	retval += 0.5 * KarisAverage(fetches2x2[0], fetches2x2[1], fetches2x2[2], fetches2x2[3]);

	[unroll] for (x = 0; x < 2; ++x)
		[unroll] for (y = 0; y < 2; ++y)
			retval += 0.125 * KarisAverage(fetches3x3[x * 3 + y], fetches3x3[(x + 1) * 3 + y], fetches3x3[x * 3 + y + 1], fetches3x3[(x + 1) * 3 + y + 1]);

	return retval;
}

/*
    OpenColorIO
        url:    https://github.com/AcademySoftwareFoundation/OpenColorIO/
        license:
			Copyright Contributors to the OpenColorIO Project.

			Redistribution and use in source and binary forms, with or without
			modification, are permitted provided that the following conditions are
			met:

			* Redistributions of source code must retain the above copyright
			notice, this list of conditions and the following disclaimer.
			* Redistributions in binary form must reproduce the above copyright
			notice, this list of conditions and the following disclaimer in the
			documentation and/or other materials provided with the distribution.
			* Neither the name of the copyright holder nor the names of its
			contributors may be used to endorse or promote products derived from
			this software without specific prior written permission.

			THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
			"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
			LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
			A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
			HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
			SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
			LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
			DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
			THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
			(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
			OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

float3 LogContrast(float3 col, float3 contrast, float3 pivot)
{
	return lerp(pivot, col, contrast);
}

float3 LinearContrast(float3 col, float3 contrast, float3 pivot)
{
	col = col / pivot;
	float3 sgn = sign(col);
	col = pow(abs(col), contrast) * pivot;
	col *= sgn;
	return col;
}

float3 Gamma(float3 col, float3 gamma, float3 black_pivot, float3 white_pivot)
{
	col = col - black_pivot;
	float3 sgn = sign(col);
	float3 range = white_pivot - black_pivot;
	col = col / range;
	col = pow(max(0, col), gamma);
	col = col * sgn * range + black_pivot;
	return col;
}

float3 Saturation(float3 col, float sat)
{
	float luma = RGBToLuminance(col);
	return lerp(luma, col, sat);
}

// https://www.shadertoy.com/view/MdjBRy
float3 HueShift(float3 col, float shift)
{
	float3 P = 0.55735 * dot(0.55735, col);
	float3 U = col - P;
	float3 V = cross(0.55735, U);
	col = U * cos(shift * 6.2832) + V * sin(shift * 6.2832) + P;
	return col;
}

float3 ASC_CDL(float3 col, float3 slope, float3 power, float3 offset)
{
	return Gamma(col * slope + offset, power, 0, 1);
}