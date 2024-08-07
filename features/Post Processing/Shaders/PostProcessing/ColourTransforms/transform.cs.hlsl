/// By ProfJack/五脚猫, 2024-2-17 UTC

#include "../../Common/Color.hlsli"

#define PI 3.1415926535

RWTexture2D<float4> RWTexOut : register(u0);

Texture2D<float4> TexColor : register(t0);
StructuredBuffer<float> RWTexAdaptation : register(t1);

cbuffer TonemapCB : register(b1)
{
	// Most: exposure, white point / cutoff
	// AgX: slope, power, offset, saturation
	// Lottes: exposure, contrast, shoulder, hdrMax
	// Day: exposure, black point, white point, crossover point
	// Uchimura: exposure, max brightness, contrast, linear start
	float4 Params0;
	// Lottes: midIn, midOut
	// Day: shoulder, toe
	// Uchimura: linear length, black tightness shape, black tightness offset
	float4 Params1;
	float4 Params2;
	float4 Params3;
};

/////////////////////////////////////////////////////////////////////////////////

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

// https://www.shadertoy.com/view/ss23DD
float3 LiftGammaGain(float3 rgb, float4 lift, float4 gamma, float4 gain)
{
	float4 liftt = 1.0 - pow(1.0 - lift, log2(gain + 1.0));

	float4 gammat = gamma.rgba - float4(0.0, 0.0, 0.0, RGBToLuminance(gamma.rgb));
	float4 gammatTemp = 1.0 + 4.0 * abs(gammat);
	gammat = lerp(gammatTemp, 1.0 / gammatTemp, step(0.0, gammat));

	float3 col = rgb;
	float luma = RGBToLuminance(col);

	col = pow(col, gammat.rgb);
	col *= pow(gain.rgb, gammat.rgb);
	col = max(lerp(2.0 * liftt.rgb, 1.0, col), 0.0);

	luma = pow(luma, gammat.a);
	luma *= pow(gain.a, gammat.a);
	luma = max(lerp(2.0 * liftt.a, 1.0, luma), 0.0);

	col += luma - RGBToLuminance(col);

	return col;
}

/////////////////////////////////////////////////////////////////////////////////

/////////////////////////////////////////////////////////////////////////////////

float3 Clamp(float3 val)
{
	return clamp(val, Params0.xyz, Params1.xyz);
}

float3 LogSpace(float3 val)
{
	return log2(max(0, val));
}

float3 Gamma(float3 val)
{
	return Gamma(val, Params0.rgb, Params1.rgb, Params2.rgb);
}

float3 ASC_CDL(float3 val)
{
	return ASC_CDL(val, Params0.rgb, Params1.rgb, Params2.rgb);
}

float3 LiftGammaGain(float3 val)
{
	return LiftGammaGain(val, Params0.gbar, Params1.gbar, Params2.gbar);
}

float3 SaturationHue(float3 val)
{
	val = Saturation(val, Params0.r);
	val = HueShift(val, Params0.g);
	return val;
}

/////////////////////////////////////////////////////////////////////////////////

float3 MatMul(float3 val)
{
	return mul(float3x3(Params0.rgb, Params1.rgb, Params2.rgb), val);
}

/////////////////////////////////////////////////////////////////////////////////

float3 ExposureContrast(float3 val)
{
	val *= Params0.xyz;
	val = LinearContrast(val, Params1.xyz, Params2.xyz);
	return val;
}

/////////////////////////////////////////////////////////////////////////////////

/*
    tizian/tonemapper
        url:    https://github.com/tizian/tonemapper
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
	val *= Params0.x;
	float luma = RGBToLuminance(val);
	float lumaOut = luma / (1 + luma);
	val = val / (luma + 1e-10) * lumaOut;
	val = saturate(val);
	return val;
}

float3 ReinhardExt(float3 val)
{
	val *= Params0.x;
	float luma = RGBToLuminance(val);
	float lumaOut = luma * (1 + luma / (Params0.y * Params0.y)) / (1 + luma);
	val = val / (luma + 1e-10) * lumaOut;
	val = saturate(val);
	return val;
}

float3 HejlBurgessDawsonFilmic(float3 val)
{
	val *= Params0.x;
	val = max(0, val - 0.004);
	val = (val * (6.2 * val + .5)) / (val * (6.2 * val + 1.7) + 0.06);
	val = pow(saturate(val), 2.2);
	return val;
}

float3 AldridgeFilmic(float3 val)
{
	val *= Params0.x;
	float tmp = 2.0 * Params0.y;
	val = val + (tmp - val) * clamp(tmp - val, 0.0, 1.0) * (0.25 / Params0.y) - Params0.y;
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

	val *= Params0.x;

	val = mul(g_sRGBToACEScg, val);
	float3 a = val * (val + 0.0245786f) - 0.000090537f;
	float3 b = val * (0.983729f * val + 0.4329510f) + 0.238081f;
	val = a / b;
	val = mul(g_ACEScgToSRGB, val);

	val = saturate(val);

	return val;
}

float3 AcesNarkowicz(float3 val)
{
	val *= Params0.x;

	static const float A = 2.51;
	static const float B = 0.03;
	static const float C = 2.43;
	static const float D = 0.59;
	static const float E = 0.14;
	val *= 0.6;
	val = (val * (A * val + B)) / (val * (C * val + D) + E);
	val = saturate(val);
	return val;
}

float3 AcesGuy(float3 val)
{
	val *= Params0.x;
	val = val / (val + 0.155f) * 1.019;

	val = pow(saturate(val), 2.2);
	return val;
}

float3 LottesFilmic(float3 val)
{
	val *= Params0.x;
	float a = Params0.y,
		  d = Params0.z,
		  b = (-pow(Params1.x, a) + pow(Params0.w, a) * Params1.y) /
	          ((pow(Params0.w, a * d) - pow(Params1.x, a * d)) * Params1.y),
		  c = (pow(Params0.w, a * d) * pow(Params1.x, a) - pow(Params0.w, a) * pow(Params1.x, a * d) * Params1.y) /
	          ((pow(Params0.w, a * d) - pow(Params1.x, a * d)) * Params1.y);

	val = pow(val, a) / (pow(val, a * d) * b + c);
	val = saturate(val);
	return val;
}

float DayCurve(float x, float k)
{
	const float b = Params0.y;
	const float w = Params0.z;
	const float c = Params0.w;
	const float s = Params1.x;
	const float t = Params1.y;

	if (x < c) {
		return k * (1.0 - t) * (x - b) / (c - (1.0 - t) * b - t * x);
	} else {
		return (1.0 - k) * (x - c) / (s * x + (1.0 - s) * w - c) + k;
	}
}

float3 DayFilmic(float3 val)
{
	const float b = Params0.y;
	const float w = Params0.z;
	const float c = Params0.w;
	const float s = Params1.x;
	const float t = Params1.y;

	val *= Params0.x;
	float k = (1.0 - t) * (c - b) / ((1.0 - s) * (w - c) + (1.0 - t) * (c - b));
	val = float3(DayCurve(val.r, k), DayCurve(val.g, k), DayCurve(val.b, k));

	val = saturate(val);
	return val;
}

float3 UchimuraFilmic(float3 val)
{
	const float P = Params0.y;
	const float a = Params0.z;
	const float m = Params0.w;
	const float l = Params1.x;
	const float c = Params1.y;
	const float b = Params1.z;

	val *= Params0.x;

	float l0 = ((P - m) * l) / a,
		  S0 = m + l0,
		  S1 = m + a * l0,
		  C2 = (a * P) / (P - S1),
		  CP = -C2 / P;

	float3 w0 = 1.0 - smoothstep(0.0, m, val),
		   w2 = step(m + l0, val),
		   w1 = 1.0 - w0 - w2;

	float3 T = m * pow(val / m, c) + b,           // toe
		L = m + a * (val - m),                    // linear
		S = P - (P - S1) * exp(CP * (val - S0));  // shoulder

	val = T * w0 + L * w1 + S * w2;

	val = saturate(val);
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
	val = ASC_CDL(val, Params0.x, Params0.y, Params0.z);
	val = Saturation(val, Params0.w);
	val = AgxEotf(val);

	return val;
}

[numthreads(32, 32, 1)] void main(uint2 tid
								  : SV_DispatchThreadID) {
	float3 color = TexColor[tid].rgb;

	color = TRANSFORM_FUNC(color);

	RWTexOut[tid] = float4(color, 1);
}