/**
 * Copyright (C) 2012 Jorge Jimenez (jorge@iryoku.com)
 * Copyright (C) 2012 Diego Gutierrez (diegog@unizar.es)
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 
 *    1. Redistributions of source code must retain the above copyright notice,
 *       this list of conditions and the following disclaimer.
 *
 *    2. Redistributions in binary form must reproduce the following disclaimer
 *       in the documentation and/or other materials provided with the 
 *       distribution:
 *
 *       "Uses Separable SSS. Copyright (C) 2012 by Jorge Jimenez and Diego
 *        Gutierrez."
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS ``AS 
 * IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, 
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR 
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL COPYRIGHT HOLDERS OR CONTRIBUTORS 
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR 
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF 
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS 
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN 
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) 
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE 
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * The views and conclusions contained in the software and documentation are 
 * those of the authors and should not be interpreted as representing official
 * policies, either expressed or implied, of the copyright holders.
 */

/**
 *                  _______      _______      _______      _______
 *                 /       |    /       |    /       |    /       |
 *                |   (----    |   (----    |   (----    |   (----
 *                 \   \        \   \        \   \        \   \
 *              ----)   |    ----)   |    ----)   |    ----)   |
 *             |_______/    |_______/    |_______/    |_______/
 *
 *        S E P A R A B L E   S U B S U R F A C E   S C A T T E R I N G
 *
 *                           http://www.iryoku.com/
 *
 * Hi, thanks for your interest in Separable SSS!
 *
 * It's a simple shader composed of two components:
 *
 * 1) A transmittance function, 'SSSSTransmittance', which allows to calculate
 *    light transmission in thin slabs, useful for ears and nostrils. It should
 *    be applied during the main rendering pass as follows:
 *
 *        float3 t = albedo.rgb * lights[i].color * attenuation * spot;
 *        color.rgb += t * SSSSTransmittance(...)
 *
 *    (See 'Main.fx' for more details).
 *
 * 2) A simple two-pass reflectance post-processing shader, 'SSSSBlur*', which
 *    softens the skin appearance. It should be applied as a regular
 *    post-processing effect like bloom (the usual framebuffer ping-ponging):
 *
 *    a) The first pass (horizontal) must be invoked by taking the final color
 *       framebuffer as input, and storing the results into a temporal
 *       framebuffer.
 *    b) The second pass (vertical) must be invoked by taking the temporal
 *       framebuffer as input, and storing the results into the original final
 *       color framebuffer.
 *
 *    Note that This SSS filter should be applied *before* tonemapping.
 *
 * Before including SeparableSSS.h you'll have to setup the target. The
 * following targets are available:
 *         SMAA_HLSL_3
 *         SMAA_HLSL_4
 *         SMAA_GLSL_3
 *
 * For more information of what's under the hood, you can check the following
 * URLs (but take into account that the shader has evolved a little bit since
 * these publications):
 *
 * 1) Reflectance: http://www.iryoku.com/sssss/
 * 2) Transmittance: http://www.iryoku.com/translucency/
 *
 * If you've got any doubts, just contact us!
 */

//-----------------------------------------------------------------------------

float3 sRGB2Lin(float3 color)
{
	if (UseLinear)
		return color > 0.04045 ? pow(color / 1.055 + 0.055 / 1.055, 2.4) : color / 12.92;
	return color;
}

float3 Lin2sRGB(float3 color)
{
	if (UseLinear)
		return color > 0.0031308 ? 1.055 * pow(color, 1.0 / 2.4) - 0.055 : 12.92 * color;
	return color;
}

float InterleavedGradientNoise(float2 uv)
{
	// Temporal factor
	float frameStep = float(FrameCount % 16) * 0.0625f;
	uv.x += frameStep * 4.7526;
	uv.y += frameStep * 3.1914;

	float3 magic = float3(0.06711056f, 0.00583715f, 52.9829189f);
	return frac(magic.z * frac(dot(uv, magic.xy)));
}

#define SSSS_N_SAMPLES 33

float4 SSSSBlurCS(
	uint2 DTid,
	float2 texcoord,
	float2 dir,
	float4 normals)
{
	float sssAmount = normals.z;

	// Fetch color of current pixel:
	float4 colorM = ColorTexture[DTid.xy];

#if defined(HORIZONTAL)
	colorM.rgb = sRGB2Lin(colorM.rgb);
#endif

	if (sssAmount == 0)
		return colorM;

	// Fetch linear depth of current pixel:
	float depthM = DepthTexture[DTid.xy].r;
	depthM = GetScreenDepth(depthM);

	bool firstPerson = depthM < 16.5;

	// Accumulate center sample, multiplying it with its gaussian weight:
	float4 colorBlurred = colorM;
	colorBlurred.rgb *= kernel[0].rgb;

	// World-space width
	float distanceToProjectionWindow = 1.0 / tan(0.5 * radians(SSSS_FOVY));
	float scale = distanceToProjectionWindow / depthM;

	// Calculate the final step to fetch the surrounding pixels:
	float2 finalStep = scale * BufferDim;
	finalStep *= sssAmount;
	finalStep *= (1.0 / 3.0);
	finalStep *= BlurRadius;

	[flatten] if (firstPerson)
	{
		finalStep *= 0.1;
		distanceToProjectionWindow *= 100.0;
	}

	float jitter = InterleavedGradientNoise(DTid.xy);
	float2x2 rotationMatrix = float2x2(cos(jitter), sin(jitter), -sin(jitter), cos(jitter));
	float2x2 identityMatrix = float2x2(1.0, 0.0, 0.0, 1.0);

	// Accumulate the other samples:
	for (uint i = 1; i < SSSS_N_SAMPLES; i++) {
		float2 offset = kernel[i].a * finalStep;

		// Apply randomized rotation
		offset = mul(offset, rotationMatrix);

		uint2 coords = DTid.xy + uint2(offset + 0.5);
		coords = clamp(coords, uint2(0, 0), uint2(BufferDim));  // Dynamic resolution

		float3 color = ColorTexture[coords].rgb;

#if defined(HORIZONTAL)
		color.rgb = sRGB2Lin(color.rgb);
#endif

		float depth = DepthTexture[coords].r;
		depth = GetScreenDepth(depth);

		// If the difference in depth is huge, we lerp color back to "colorM":
		float s = min(saturate((1.0 - DepthFalloff) * distanceToProjectionWindow * abs(depthM - depth)), 1.0 - Backlighting);  // Backlighting;
		color = lerp(color, colorM.rgb, s);

		// Accumulate:
		colorBlurred.rgb += kernel[i].rgb * color.rgb;
	}

	return colorBlurred;
}

//-----------------------------------------------------------------------------
