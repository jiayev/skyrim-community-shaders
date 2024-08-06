/// By ProfJack/五脚猫, 2024-2-17 UTC

#include "../../Common/Color.hlsli"

RWTexture2D<float4> RWTexOut : register(u0);

Texture2D<float4> TexColor : register(t0);
StructuredBuffer<float> RWTexAdaptation : register(t1);

cbuffer TonemapCB : register(b1)
{
	// Most: key value / exposure, white point, cutoff
	// AgX: slope, power, offset, saturation
	float4 Params1;
};

float3 ASC_CDL(float3 col, float3 slope, float3 power, float3 offset)
{
	return pow(col * slope + offset, power);
}

float3 Saturation(float3 col, float3 sat)
{
	float luma = RGBToLuminance(col);
	return lerp(luma, col, sat);
}

/////////////////////////////////////////////////////////////////////////////////

/*
    tizian/tonemapper
        url:    https://github.com/tizian/tonemapper
        credit: plenty o' mapping functions
        license:
            The MIT License (MIT)

            Copyright (c) 2022 Tizian Zeltner

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

float3 Reinhard(float3 val)
{
	val *= Params1.x;
	float luma = RGBToLuminance(val);
	float lumaOut = luma / (1 + luma);
	val = val / (luma + 1e-10) * lumaOut;
	return val;
}

float3 ReinhardExt(float3 val)
{
	val *= Params1.x;
	float luma = RGBToLuminance(val);
	float lumaOut = luma * (1 + luma / (Params1.y * Params1.y)) / (1 + luma);
	val = val / (luma + 1e-10) * lumaOut;
	return val;
}

float3 HejlBurgessDawsonFilmic(float3 val)
{
	val *= Params1.x;
	val = max(0, val - 0.004);
	val = (val * (6.2 * val + .5)) / (val * (6.2 * val + 1.7) + 0.06);
	val = pow(saturate(val), 2.2);
	return val;
}

float3 AldridgeFilmic(float3 val)
{
	val *= Params1.x;
	float tmp = 2.0 * Params1.z;
	val = val + (tmp - val) * clamp(tmp - val, 0.0, 1.0) * (0.25 / Params1.z) - Params1.z;
	val = (val * (6.2 * val + 0.5)) / (val * (6.2 * val + 1.7) + 0.06);
	val = pow(saturate(val), 2.2);
	return val;
}

float3 AcesHill(float3 val)
{
	static const float3x3 g_sRGBToACEScg = float3x3(
		0.613117812906440, 0.341181995855625, 0.045787344282337,
		0.069934082307513, 0.918103037508582, 0.011932775530201,
		0.020462992637737, 0.106768663382511, 0.872715910619442);
	static const float3x3 g_ACEScgToSRGB = float3x3(
		1.704887331049502, -0.624157274479025, -0.080886773895704,
		-0.129520935348888, 1.138399326040076, -0.008779241755018,
		-0.024127059936902, -0.124620612286390, 1.148822109913262);

	val *= Params1.x;

	val = mul(g_sRGBToACEScg, val);
	float3 a = val * (val + 0.0245786f) - 0.000090537f;
	float3 b = val * (0.983729f * val + 0.4329510f) + 0.238081f;
	val = a / b;
	val = mul(g_ACEScgToSRGB, val);

	return val;
}

float3 AcesNarkowicz(float3 val)
{
	val *= Params1.x;

	static const float A = 2.51;
	static const float B = 0.03;
	static const float C = 2.43;
	static const float D = 0.59;
	static const float E = 0.14;
	val *= 0.6;
	val = (val * (A * val + B)) / (val * (C * val + D) + E);

	return val;
}

float3 AcesGuy(float3 val)
{
	val *= Params1.x;
	val = val / (val + 0.155f) * 1.019;

	val = pow(saturate(val), 2.2);
	return val;
}

/*  AgX Reference:
 *  AgX by longbool https://www.shadertoy.com/view/dtSGD1
 *  AgX Minimal by bwrensch https://www.shadertoy.com/view/cd3XWr
 *  Fork AgX Minima troy_s 342 by troy_s https://www.shadertoy.com/view/mdcSDH
 */

// Mean error^2: 3.6705141e-06
float3 AgxDefaultContrastApprox5(float3 x)
{
	float3 x2 = x * x;
	float3 x4 = x2 * x2;

	return +15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4 - 6.868 * x2 * x +
	       0.4298 * x2 + 0.1191 * x - 0.00232;
}

// Mean error^2: 1.85907662e-06
float3 AgxDefaultContrastApprox6(float3 x)
{
	float3 x2 = x * x;
	float3 x4 = x2 * x2;

	return -17.86 * x4 * x2 * x + 78.01 * x4 * x2 - 126.7 * x4 * x + 92.06 * x4 -
	       28.72 * x2 * x + 4.361 * x2 - 0.1718 * x + 0.002857;
}

float3 Agx(float3 val)
{
	const float3x3 agx_mat = transpose(
		float3x3(0.842479062253094, 0.0423282422610123, 0.0423756549057051,
			0.0784335999999992, 0.878468636469772, 0.0784336,
			0.0792237451477643, 0.0791661274605434, 0.879142973793104));

	const float min_ev = -12.47393f;
	const float max_ev = 4.026069f;

	// Input transform
	val = mul(agx_mat, val);

	// Log2 space encoding
	val = clamp(log2(val), min_ev, max_ev);
	val = (val - min_ev) / (max_ev - min_ev);

	// Apply sigmoid function approximation
	val = AgxDefaultContrastApprox6(val);

	return val;
}

float3 AgxEotf(float3 val)
{
	const float3x3 agx_mat_inv = transpose(
		float3x3(1.19687900512017, -0.0528968517574562, -0.0529716355144438,
			-0.0980208811401368, 1.15190312990417, -0.0980434501171241,
			-0.0990297440797205, -0.0989611768448433, 1.15107367264116));

	// Undo input transform
	val = mul(agx_mat_inv, val);

	// sRGB IEC 61966-2-1 2.2 Exponent Reference EOTF Display
	// NOTE: We're linearizing the output here. Comment/adjust when
	// *not* using a sRGB render target
	val = pow(saturate(val), 2.2);

	return val;
}

float3 AgxMinimal(float3 val)
{
	val = Agx(val);
	val = ASC_CDL(val, Params1.x, Params1.y, Params1.z);
	val = Saturation(val, Params1.w);
	val = AgxEotf(val);

	return val;
}

[numthreads(32, 32, 1)] void main(uint2 tid
								  : SV_DispatchThreadID) {
	float3 color = TexColor[tid].rgb;

	color = TONEMAP_OPERATOR(color);

	RWTexOut[tid] = float4(color, 1);
}