#pragma once

#include "Buffer.h"
#include "Feature.h"
#include "Util.h"

struct VolumetricLighting : Feature
{
public:
	static VolumetricLighting* GetSingleton()
	{
		static VolumetricLighting singleton;
		return &singleton;
	}

	struct Settings
	{
		uint EnabledVL = true;
	};

	Settings settings;

	bool enabledAtBoot = false;

	virtual inline std::string GetName() override { return "Volumetric Lighting"; }
	virtual inline std::string GetShortName() override { return "VolumetricLighting"; }

	virtual void Reset() override;

	virtual void SaveSettings(json&) override;
	virtual void LoadSettings(json&) override;
	virtual void RestoreDefaultSettings() override;
	virtual void DrawSettings() override;
	virtual void DataLoaded() override;
	virtual void PostPostLoad() override;

	std::map<std::string, gameSetting> VLSettings{
		{ "bEnableVolumetricLighting:Display", { "Enable Volumetric Lighting (INI)", "Enables or disables volumetric lighting effects.", 0, true, false, true } },
		{ "bVolumetricLightingDisableInterior:Display", { "Disable Interior Volumetric Lighting", "Disables volumetric lighting effects for interior spaces.", REL::Relocate<uintptr_t>(0, 0, 0x1ed4038), true, false, true } },
		{ "bVolumetricLightingUpdateWeather:Display", { "Update Volumetric Lighting with Weather", "Updates volumetric lighting based on weather conditions.", 0, true, false, true } },
		{ "bVolumetricLightingEnableTemporalAccumulation:Display", { "Enable Volumetric Lighting Temporal Accumulation", "Enables temporal accumulation for volumetric lighting effects.", 0, true, false, true } },
		{ "iVolumetricLightingQuality:Display", { "Lighting Quality", "Adjusts the quality of volumetric lighting. [-1,-2] (off, low, mid, high).", REL::Relocate<uintptr_t>(0, 0, 0x1ed4030), 1, -1, 2 } },
		{ "fVolumetricLightingCustomColorContribution:Display", { "Custom Color Contribution", "Controls the contribution of custom colors to volumetric lighting.", 0, 0.0f, 0.0f, 1.0f } },
		{ "fVolumetricLightingDensityContribution:Display", { "Density Contribution", "Adjusts the density contribution of volumetric lighting.", 0, 0.3f, 0.0f, 1.0f } },
		{ "fVolumetricLightingDensityScale:Display", { "Density Scale", "Scales the density of volumetric lighting effects.", 0, 300.0f, 0.0f, 1000.0f } },
		{ "fVolumetricLightingIntensity:Display", { "Intensity", "Sets the intensity of volumetric lighting effects.", 0, 2.0f, 0.0f, 10.0f } },
		{ "fVolumetricLightingPhaseContribution:Display", { "Phase Contribution", "Controls the contribution of phase to the volumetric lighting effects.", 0, 0.83f, 0.0f, 1.0f } },
		{ "fVolumetricLightingPhaseScattering:Display", { "Phase Scattering", "Sets the phase scattering for volumetric lighting.", 0, 0.85f, 0.0f, 1.0f } },
		{ "fVolumetricLightingRangeFactor:Display", { "Range Factor", "Adjusts the range factor for volumetric lighting effects.", 0, 40.0f, 0.0f, 100.0f } },
		{ "fVolumetricLightingTemporalAccumulationFactor:Display", { "Temporal Accumulation Factor", "Controls the strength of temporal accumulation in volumetric lighting.", 0, 0.75f, 0.0f, 5.0f } },
		{ "fVolumetricLightingWindFallingSpeed:Display", { "Wind Falling Speed", "Sets the speed at which wind affects falling particles in volumetric lighting.", 0, 0.3f, 0.0f, 1.0f } },
		{ "fVolumetricLightingWindSpeedScale:Display", { "Wind Speed Scale", "Scales the wind speed effect on volumetric lighting.", 0, 15.0f, 0.0f, 50.0f } },
		// { "iVolumetricLightingNoiseTextureDepth:Display", { "Noise Texture Depth", "Sets the depth of the noise texture used for volumetric lighting.", 0, 32, 1, 128 } },
		// { "iVolumetricLightingNoiseTextureHeight:Display", { "Noise Texture Height", "Sets the height of the noise texture used for volumetric lighting.", 0, 32, 1, 128 } },
		// { "iVolumetricLightingNoiseTextureWidth:Display", { "Noise Texture Width", "Sets the width of the noise texture used for volumetric lighting.", 0, 32, 1, 128 } },
		// { "iVolumetricLightingTextureDepthHigh:Display", { "High Depth Texture", "Sets the depth for the high-quality volumetric lighting texture.", 0, 90, 1, 128 } },
		// { "iVolumetricLightingTextureDepthMedium:Display", { "Medium Depth Texture", "Sets the depth for the medium-quality volumetric lighting texture.", 0, 70, 1, 128 } },
		// { "iVolumetricLightingTextureDepthLow:Display", { "Low Depth Texture", "Sets the depth for the low-quality volumetric lighting texture.", 0, 50, 1, 128 } },
		// { "iVolumetricLightingTextureFormatHigh:Display", { "High Texture Format", "Sets the format for the high-quality volumetric lighting texture.", 0, 1, 0, 2 } },
		// { "iVolumetricLightingTextureFormatMedium:Display", { "Medium Texture Format", "Sets the format for the medium-quality volumetric lighting texture.", 0, 1, 0, 2 } },
		// { "iVolumetricLightingTextureFormatLow:Display", { "Low Texture Format", "Sets the format for the low-quality volumetric lighting texture.", 0, 0, 0, 2 } },
		// { "iVolumetricLightingTextureHeightHigh:Display", { "High Texture Height", "Sets the height for the high-quality volumetric lighting texture.", 0, 192, 1, 512 } },
		// { "iVolumetricLightingTextureHeightMedium:Display", { "Medium Texture Height", "Sets the height for the medium-quality volumetric lighting texture.", 0, 128, 1, 512 } },
		// { "iVolumetricLightingTextureHeightLow:Display", { "Low Texture Height", "Sets the height for the low-quality volumetric lighting texture.", 0, 96, 1, 512 } },
		// { "iVolumetricLightingTextureWidthHigh:Display", { "High Texture Width", "Sets the width for the high-quality volumetric lighting texture.", 0, 320, 1, 512 } },
		// { "iVolumetricLightingTextureWidthMedium:Display", { "Medium Texture Width", "Sets the width for the medium-quality volumetric lighting texture.", 0, 224, 1, 512 } },
		// { "iVolumetricLightingTextureWidthLow:Display", { "Low Texture Width", "Sets the width for the low-quality volumetric lighting texture.", 0, 160, 1, 512 } },
	};

	std::map<std::string, gameSetting> hiddenVRSettings{
		{ "bEnableVolumetricLighting:Display", { "Enable VL Shaders (INI) ",
												   "Enables volumetric lighting effects by creating shaders. "
												   "Needed at startup. ",
												   0x1ed63d8, true, false, true } },
		{ "bVolumetricLightingEnable:Display", { "Enable VL (INI))", "Enables volumetric lighting. ", 0x3485360, true, false, true } },
		{ "bVolumetricLightingUpdateWeather:Display", { "Enable Volumetric Lighting (Weather) (INI) ",
														  "Enables volumetric lighting for weather. "
														  "Only used during startup and used to set bVLWeatherUpdate.",
														  0x3485361, true, false, true } },
		{ "bVLWeatherUpdate", { "Enable VL (Weather)", "Enables volumetric lighting for weather.", 0x3485363, true, false, true } },
		{ "bVolumetricLightingEnabled_143232EF0", { "Enable VL (Papyrus) ",
													  "Enables volumetric lighting. "
													  "This is the Papyrus command. ",
													  REL::Relocate<uintptr_t>(0x3232ef0, 0, 0x3485362), true, false, true } },
	};

	virtual bool SupportsVR() override { return true; };

	// hooks

	struct CopyResource
	{
		static void thunk(ID3D11DeviceContext* a_this, ID3D11Resource* a_renderTarget, ID3D11Resource* a_renderTargetSource)
		{
			// In VR with dynamic resolution enabled, there's a bug with the depth stencil.
			// The depth stencil passed to IsFullScreenVR is scaled down incorrectly.
			// The fix is to stop a CopyResource from replacing kMAIN_COPY with kMAIN after
			// ISApplyVolumetricLighting because it clobbers a properly scaled kMAIN_COPY.
			// The kMAIN_COPY does not appear to be used in the remaining frame after
			// ISApplyVolumetricLighting except for IsFullScreenVR.
			// But, the copy might have to be done manually later after IsFullScreenVR if
			// used in the next frame.

			auto* singleton = GetSingleton();
			if (singleton && !(Util::IsDynamicResolution() && singleton->settings.EnabledVL)) {
				a_this->CopyResource(a_renderTarget, a_renderTargetSource);
			}
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};
};
