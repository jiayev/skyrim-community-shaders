#pragma once

#include "Buffer.h"

#include <memory>
#include <winrt/base.h>

struct Effects11 : Feature
{
public:
	virtual inline std::string GetName() override { return "Effects11"; }
	virtual inline std::string GetShortName() override { return "Effects11"; }
	virtual inline std::string GetDisplayName() override { return "Effects 11"; }
	virtual std::string_view GetCategory() const override { return "Post-Processing"; }
	virtual inline std::string_view GetShaderDefineName() override { return "EFFECTS11"; }
	bool HasShaderDefine(RE::BSShader::Type) override;

	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return {
			T("feature.effects11.description", "Effects 11 loads ENBSeries-compatible FX post-processing. Install multiple presets under Data/SKSE/Plugins/CommunityShaders/Effects11/Presets/<Name>/ (each with enbseries.ini + enbseries/), use a classic root/Data enbseries install as Legacy, or ship effects11/ inside a unified Presets pack. Hotswap does not modify root files."),
			{ T("feature.effects11.key_feature_1", "ENBSeries-compatible FX support"),
				T("feature.effects11.key_feature_2", "Multi-preset library with in-game hotswap"),
				T("feature.effects11.key_feature_3", "Legacy root/Data enbseries support"),
				T("feature.effects11.key_feature_4", "Advanced post-processing pipeline"),
				T("feature.effects11.key_feature_5", "Dynamic UI variable system") }
		};
	}

	struct Settings
	{
		std::string ActivePreset;  ///< Empty = Legacy (PresetManager::kLegacyPresetId)
	};

	Settings settings;

	/** Discover presets and apply settings.ActivePreset, repairing the id if needed. */
	void SyncActivePresetFromSettings();

	/** Sync ActivePreset from the manager and write SettingsUser.json via State::Save(). */
	void PersistActivePreset();

	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;

	struct alignas(16) PerFrame
	{
		uint Enable;
		float ColorPow;
		float LightSpriteIntensity;
		float FireIntensity;

		float FireCurve;
		uint EnableRain;
		float RainMotionStretch;
		float RainMotionTransparency;

		float CloudsCurve;
		float CloudsDesaturation;
		float CloudsEdgeIntensity;
		float CloudsEdgeMoonMultiplier;

		uint EnableProceduralSun;
		float ProceduralSunDiskRadiusSq;
		float ProceduralSunDiskEdgeScale;
		float ProceduralSunGlowIntensity;

		float ProceduralSunCoronaFalloff;
		float ProceduralSunCoronaScale;
		uint UseProceduralGradientWeights;
		float ProceduralGradientWeightCurve;

		float ParticleIntensity;
		float ParticleLightingInfluence;
		float ParticleAmbientInfluence;
		float ParticlePointLightingInfluence;

		uint EnableVolumetricRays;
		float VolumetricRaysIntensity;
		float VolumetricRaysExtinction;
		float VolumetricRaysSkyColorAmount;

		float VolumetricRaysDesaturation;
		float3 VolumetricRaysColorFilter;
	};

	bool enableEffect = false;

	ID3D11PixelShader* raymarchVolumetricRaysPS = nullptr;
	ID3D11PixelShader* applyVolumetricRaysPS = nullptr;
	ID3D11ComputeShader* blurHCS = nullptr;
	ID3D11ComputeShader* blurVCS = nullptr;
	winrt::com_ptr<ID3D11BlendState> additiveBlendState;
	winrt::com_ptr<ID3D11BlendState> alphaBlendState;

	std::unique_ptr<Texture2D> vlTexA;
	std::unique_ptr<Texture2D> vlTexB;
	std::unique_ptr<Texture2D> vlDepthHalf;
	std::unique_ptr<ConstantBuffer> vlBlurCB;

	winrt::com_ptr<ID3D11Texture2D> raindropTexture;
	winrt::com_ptr<ID3D11ShaderResourceView> raindropSRV;
	std::string raindropStatus;
	void LoadRaindropTexture();

	PerFrame GetCommonBufferData();

	virtual void DrawSettings() override;
	virtual void SetupResources() override;
	virtual void PostSetupResources() override;
	virtual void Reset() override;
	virtual void Prepass() override;
	virtual void ClearShaderCache() override;

	/** @brief Flips the "UseEffect" GLOBAL setting; bound to the Effects 11 toggle hotkey. */
	void ToggleEnabled();
	bool IsPresetEnabled() const;
	bool IsActive() const { return presetActive; }

	void DrawVolumetricRays();

	void OnSkyUpdateColors(RE::Sky* a_sky);
	void OverrideWeather(RE::Sky* a_sky);
	void CheckCommonData();
	void OverridePointLightColor(float3& a_color);

	struct DirectionalAmbientColors
	{
		RE::NiColor directionalAmbientColors[3][2];
	};
	void OverrideAmbientLighting(DirectionalAmbientColors& DirectionalAmbientColors);

	void ModifySky(RE::BSRenderPass* Pass);
	__declspec(noinline) void ModifyParticle(RE::BSRenderPass* Pass);
	void ParticleShaderHacks();

	/**
	 * @brief Whether Effects11 wants to replace the vanilla tonemap this frame.
	 *
	 * Queried by State::GetTonemapOwner() to arbitrate against Post Processing. Does not
	 * render anything; refreshes per-frame common data as a side effect.
	 */
	bool WantsTonemapOwnership();

	/**
	 * @brief Runs the ENB effect chain in place of the vanilla tonemap pass.
	 * @param a_input Render target holding the scene color to tonemap.
	 * @param a_output Render target receiving the tonemapped result.
	 * @return True only if the chain wrote the output; false means the caller must fall
	 *         back to the vanilla pass.
	 */
	bool RenderTonemap(RE::RENDER_TARGET a_input, RE::RENDER_TARGET a_output);

	/** @brief True when the effect chain replaced ISHDR this frame, leaving an SDR scene for HDR Display to expand. */
	bool ReplacedTonemapperThisFrame() const;

private:
	bool presetActive = false;
	bool resourcesReady = false;
	uint tonemapReplacedFrame = UINT32_MAX;  ///< frameCount when the effect chain last wrote the tonemap output
	/** Set when Sync repairs ActivePreset during load; flushed in SetupResources. */
	bool activePresetNeedsPersist = false;
};
