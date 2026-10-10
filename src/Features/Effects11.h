#pragma once

#include "Buffer.h"
#include "Effects11/SceneHandoff.h"

#include <array>
#include <memory>
#include <winrt/base.h>

// C4324: the aligned PerFrame cache member pads the struct
#pragma warning(push)
#pragma warning(disable: 4324)

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

	/** @brief Persisted Effects11 settings. */
	struct Settings
	{
		std::string ActivePreset;  ///< Empty = Legacy (PresetManager::kLegacyPresetId)
	};

	Settings settings;

	/** @brief Discovers presets and applies settings.ActivePreset, repairing the id if needed. */
	void SyncActivePresetFromSettings();

	/** @brief Copies the manager's active id into ActivePreset and saves it through State::Save(). */
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

		float LightSpriteCurve;
		float pad1[3];

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

		uint EnableCloudsScattering;
		float SkyScatteringIntensity;
		float SkyScatteringShadowAmount;
		float SkyScatteringAmount;

		float3 SkyScatteringColor;
		float SkyScatteringDustDarkening;

		float3 SkyScatteringDustTint;
		float SkyScatteringDustVolume;

		float3 SkyScatteringSunDirection;
		float SkyScatteringSunVisibility;

		float SkyScatteringHorizonRange;
		float SkyScatteringAtmosphereThickness;
		float SkyScatteringAirGlowIntensity;
		float SkyScatteringAirGlowRange;

		float SkyScatteringSunGlowIntensity;
		float SkyScatteringSunGlowRange;
		float SkyScatteringMoonGlowAmount;
		float SkyScatteringMoonGlowRange;

		float SkyScatteringSunIntensity;
		float CloudsLightingSunIntensity;
		float CloudsLightingMoonIntensity;
		uint EnableCloudsLightingFromMoon;

		uint CalculateCloudsEdgeFromScattering;
		float CloudsLightingDesaturation;
		float CloudsLightingForwardScattering;
		float CloudsLightingDensity;

		float3 CloudsColorFilter;
		float CloudsIntensity;

		float CloudsVertexAlphaBoost;
		float CloudsEdgeClamp;
		float CloudsEdgeFadePower;
		float SunBillboardTan;

		float MasserBillboardTan;
		float SecundaBillboardTan;
		float2 SkyScatteringPad0;

		uint EnableWater;
		float WaterWavesAmplitude;
		float WaterMuddiness;
		float WaterSunLightingMultiplier;

		float WaterSunSpecularMultiplier;
		float WaterFresnelMin;
		float WaterFresnelMax;
		float WaterFresnelMultiplier;

		float WaterReflectionAmount;
		float WaterPad0;
		float WaterPad1;
		float WaterPad2;
	};
	static_assert(sizeof(PerFrame) % 16 == 0);
	static_assert(offsetof(PerFrame, EnableCloudsScattering) % 16 == 0);
	static_assert(offsetof(PerFrame, SkyScatteringColor) % 16 == 0);
	static_assert(offsetof(PerFrame, SkyScatteringDustTint) % 16 == 0);
	static_assert(offsetof(PerFrame, SkyScatteringSunDirection) % 16 == 0);
	static_assert(offsetof(PerFrame, SkyScatteringSunIntensity) % 16 == 0);
	static_assert(offsetof(PerFrame, CloudsColorFilter) % 16 == 0);
	static_assert(offsetof(PerFrame, MasserBillboardTan) % 16 == 0);

	bool enableEffect = false;

	/** @brief Whether the active preset hands this feature back to CS through its manifest sceneControl. */
	bool IsHandedOff(E11Handoff::Feature feature) const { return handedOff[static_cast<size_t>(feature)]; }
	/** @brief Sets the runtime handoff flag; UnifiedPresetCatalog owns persisting it to the manifest. */
	void SetHandedOff(E11Handoff::Feature feature, bool value) { handedOff[static_cast<size_t>(feature)] = value; }
	/** @brief Whether Effects 11 drives a feature it can hand to CS. */
	bool OwnsFeature(E11Handoff::Feature feature) const { return loaded && enableEffect && !IsHandedOff(feature); }

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

	/** @brief Sun color after the preset's sun desaturation and filter, normalized to a peak of 1; tints the sky scattering. */
	float3 scatteringSunColor = { 1.0f, 1.0f, 1.0f };
	/** @brief Last sun direction used for sky scattering; held while the sun disc is hidden above the horizon. */
	float3 scatteringSunDirection = { 0.0f, 0.0f, 1.0f };
	bool hasScatteringSunDirection = false;

	PerFrame GetCommonBufferData();
	/** @brief Fills the [SKYSCATTERING] and cloud lighting fields of the per-frame buffer. */
	void UpdateSkyScattering(PerFrame& a_data);

	virtual void DrawSettings() override;
	virtual void SetupResources() override;
	virtual void PostSetupResources() override;
	virtual void Reset() override;
	virtual void Prepass() override;
	virtual void ClearShaderCache() override;

	/** @brief Switches between Effects 11 and Vanilla; bound to the Effects 11 toggle hotkey. */
	void ToggleEnabled();
	/** @brief Writes the "UseEffect" GLOBAL setting; go through PostProcessingMode::Set to keep the pipelines exclusive. */
	void SetUseEffect(bool enabled);
	/** @brief UseEffect is on in enbseries.ini; does not require a compiled preset. */
	bool IsUseEffectEnabled() const;
	/** @brief UseEffect on and a compiled preset is ready (Effects 11 actually drives the frame). */
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
	bool IsRainEnabled();

	/** @brief True when the effect chain replaced ISHDR this frame, leaving an SDR scene for HDR Display to expand. */
	bool ReplacedTonemapperThisFrame() const;

private:
	bool presetActive = false;
	bool resourcesReady = false;
	uint tonemapReplacedFrame = UINT32_MAX;  ///< frameCount when the effect chain last wrote the tonemap output
	std::array<bool, static_cast<size_t>(E11Handoff::Feature::Count)> handedOff{};

	/** @brief Point light settings, resolved once per frame in CheckCommonData since OverridePointLightColor runs per light. */
	struct PointLightingParams
	{
		float curve = 1.0f;
		float desaturation = 0.0f;
		float intensity = 1.0f;
	} pointLighting;

	// The feature buffer is rebuilt several times per frame, so GetCommonBufferData's lookups are replayed from here
	PerFrame perFrameCache{};
	Util::FrameChecker perFrameCacheChecker;
	/** Set when Sync repairs ActivePreset during load; flushed in SetupResources. */
	bool activePresetNeedsPersist = false;
};

#pragma warning(pop)
