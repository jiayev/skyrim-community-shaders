/**
 * @file HDROutputCS.hlsl
 * @brief HDR: gamma decode, paper-white × (nits/203), BT.2020, PQ. SDR: passthrough + UI.
 */

#include "Common/Color.hlsli"
#include "Common/DisplayMapping.hlsli"
#include "Common/SharedData.hlsli"

Texture2D<float4> SceneTex : register(t0);
Texture2D<float4> UITex : register(t1);
RWTexture2D<float4> HDROutput : register(u0);

cbuffer PerFrame : register(b0)
{
	float enableHDR : packoffset(c0.x);
	float paperWhite : packoffset(c0.y);
	float peakNits : packoffset(c0.z);
	float hdrPad0 : packoffset(c0.w);
	float uiBrightness : packoffset(c1.x);
	float isSceneLinear : packoffset(c1.y);
	float isMainOrLoadingMenu : packoffset(c1.z);
	float fgTweenMenuMidAlphaBoost : packoffset(c1.w);  ///< TweenMenu: soften AA band when compositing here (UIBrightnessCS skips while paused)
	float previewSDR : packoffset(c2.x);                ///< 1.0 = emit gamma 2.2 SDR (crop preview) instead of PQ HDR10
	float applyAutoHDR : packoffset(c2.y);              ///< 1.0 = Effects11 replaced ISHDR, so expand its SDR result into HDR
}

[numthreads(8, 8, 1)] void main(uint3 dispatchID : SV_DispatchThreadID) {
	uint width, height;
	HDROutput.GetDimensions(width, height);
	if (dispatchID.x >= width || dispatchID.y >= height)
		return;

	float4 scene = SceneTex[dispatchID.xy];
	float4 ui = UITex[dispatchID.xy];

	bool hdrEnabled = enableHDR > 0.5;
	bool postProcessOutput = SharedData::postProcessingSettings.DisableVanillaTonemapping != 0 && !(isMainOrLoadingMenu > 0.5);
	float3 finalColor;

	if (hdrEnabled) {
		bool sceneIsLinear = isSceneLinear > 0.5 || postProcessOutput;

		if (applyAutoHDR > 0.5) {
			float3 outputColor = sceneIsLinear ? scene.xyz : TransferFunctions::SignedGamma22ToLinear(scene.xyz);
			outputColor = DisplayMapping::PumboAutoHDR(outputColor, SharedData::HDRData.z, SharedData::HDRData.y, 2.75, 1.0);
			scene.xyz = sceneIsLinear ? outputColor : TransferFunctions::LinearToSignedGamma22(outputColor);
		}

		float3 compositedColorLinear;

		if (sceneIsLinear) {
			float3 sceneLinear = max(0.0, scene.rgb);
			float3 uiLinear = TransferFunctions::Gamma22ToLinear(max(0.0, ui.rgb));
			if (!(isMainOrLoadingMenu > 0.5)) {  // UI and scene can't be separated in main menu or loading screen
				// scale UI brightness (multiplier based on paperWhite)
				uiLinear *= uiBrightness;
			}
			if (postProcessOutput)
				uiLinear = Color::BT709ToBT2020(uiLinear);
			compositedColorLinear = uiLinear + sceneLinear * (1.0 - ui.a);
		} else {
			float3 sceneGamma = scene.rgb;
			float3 uiGamma = ui.rgb;
			if (!(isMainOrLoadingMenu > 0.5)) {  // UI and scene can't be separated in main menu or loading screen
				// scale UI brightness (multiplier based on paperWhite)
				float3 uiLinear = TransferFunctions::Gamma22ToLinear(max(0, uiGamma));
				uiLinear *= uiBrightness;
				uiGamma = TransferFunctions::LinearToGamma22(uiLinear);
			}
#if 0
            if (fgTweenMenuMidAlphaBoost > 0.5 && ui.a > 1e-3) {
                float midBand = smoothstep(0.3, 0.35, ui.a) * (1.0 - smoothstep(0.55, 0.6, ui.a));
                const float fgMidAlphaBoost = 0.12;
                ui.a = saturate(ui.a + midBand * fgMidAlphaBoost);
            }
#endif

			float3 compositedColorGamma = uiGamma + sceneGamma * (1.0 - ui.a);

			// Non-LL path: ISHDR output is gamma-encoded at this stage.
			compositedColorLinear = TransferFunctions::SignedGamma22ToLinear(compositedColorGamma);
		}

		if (previewSDR > 0.5) {
			if (postProcessOutput)
				compositedColorLinear = Color::BT2020ToBT709(compositedColorLinear);
			// Crop preview lives in the SDR menu buffer: emit gamma 2.2 instead of PQ.
			finalColor = saturate(TransferFunctions::LinearToGamma22(max(0.0, compositedColorLinear)));
		} else {
			if (!postProcessOutput)
				compositedColorLinear = Color::BT709ToBT2020(compositedColorLinear);
			finalColor = Color::pq::Encode(max(0.0, compositedColorLinear), paperWhite);

			finalColor = saturate(finalColor);
		}
	} else {
		float3 sceneGamma = scene.rgb;
		finalColor = ui.rgb + sceneGamma * (1.0 - ui.a);
		finalColor = saturate(finalColor);
	}

	HDROutput[dispatchID.xy] = float4(finalColor, 1.0);
}
