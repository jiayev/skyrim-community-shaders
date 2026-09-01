#pragma once

#include "PhysicalSky/Ndf.h"

struct PhysicalSky final : public Feature
{
	////////////////////////////////////////////////// Boilerplate
	static PhysicalSky* GetSingleton()
	{
		static PhysicalSky singleton;
		return &singleton;
	}

	// Metadata
	inline std::string GetName() override { return "Physical Sky"; }
	std::string GetDisplayName() override { return T("feature.physical_sky.name", "Physical Sky"); }
	inline std::string GetShortName() override { return "PhysicalSky"; }
	inline std::string_view GetCategory() const override { return "Sky"; }
	inline std::string GetFeatureModLink() override { return MakeNexusModURL("999999"); }
	inline std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return {
			T("feature.physical_sky.description", "Physically-based sky model for realistic sky gradients and other astronomical effects."),
			{
				T("feature.physical_sky.key_feature_1", "Physically-based atmosphere and aerial perspective."),
				T("feature.physical_sky.key_feature_2", "Procedural sun disk and celestial lighting controls."),
				T("feature.physical_sky.key_feature_3", "Worldspace whitelist and interior override support."),
				T("feature.physical_sky.key_feature_4", "Cloud relighting and silver lining controls."),
			}
		};
	}

	// Functionality
	inline std::string_view GetShaderDefineName() override { return "PHYSICAL_SKY"; }
	inline bool HasShaderDefine(RE::BSShader::Type) override { return true; };

	// Settings & UI
	void DataLoaded() override;
	void RestoreDefaultSettings() override;
	void LoadSettings(json& o_json) override;
	void SaveSettings(json& o_json) override;

	void DrawSettings() override;
	void SettingsGeneral();
	void SettingsCelestials();
	void SettingsAtmosphere();
	void SettingsClouds();
	void SettingsVolumetricClouds();
	void SettingsDebug();

	// Resources
	void SetupResources() override;
	void ClearShaderCache() override;
	void CompileShaders();
	bool ShadersOK();

	// Draw
	void Reset() override;
	void EarlyPrepass() override;
	void ReflectionsPrepass() override;
	void Prepass() override;
	void GenerateLuts();
	void AccumShadow();
	inline void PostPostLoad() override { Hooks::Install(); }

	////////////////////////////////////////////////// Feature Specific Data
	constexpr static uint16_t kTrLutW = 256;
	constexpr static uint16_t kTrLutH = 64;
	constexpr static uint16_t kMsLutW = 32;
	constexpr static uint16_t kMsLutH = 32;
	constexpr static uint16_t kSvLutW = 200;
	constexpr static uint16_t kSvLutH = 150;
	constexpr static uint16_t kApLutW = 32;
	constexpr static uint16_t kApLutH = 32;
	constexpr static uint16_t kApLutD = 32;

	struct WorldspaceInfo
	{
		float zBottom = -14500.f;
	};

	struct Settings
	{
		bool enabled = true;
		bool enableAllExteriorCells = false;
		bool forceEnableAllInteriorCells = false;
		bool overrideDirLight = true;
		bool lightSkyStatics = true;
		float skyStaticsBrightness = 1.0f;
		bool halfResApShadow = false;
		int tonemapper = 2;
		float vanillaMix = 0;
		float trMix = 1;
		float apLumMix = 1;
		float apTrMix = 1;

		float2 cloudShadowRemapRange = float2{ 0, 1.f };

		float3 sunlightColor = float3{ 1.0f, 0.97f, 0.95f } * 10.f;
		float3 masserColor = float3{ 1.0f, 0.6f, 0.6f } * 0.1f;
		float3 secundaColor = float3{ 0.8f, 1.0f, 1.0f } * 0.05f;

		bool proceduralSun = true;
		float sunDiskRad = DirectX::XMConvertToRadians(0.53f);

		std::map<std::string, WorldspaceInfo> worldspaceWhitelist = {
			{ "Tamriel", { -14500.f } },
			{ "WindhelmWorld", { -14500.f } },
			{ "RiftenWorld", { -14500.f } },
			{ "MarkarthWorld", { -14500.f } },
			{ "WhiterunWorld", { -14500.f } },
			{ "SolitudeWorld", { -14500.f } },
			{ "WhiterunDragonsreachWorld", { -14500.f } },
			{ "DLC01FalmerValley", { 3000.f } },
			{ "DLC2SolstheimWorld", { 256.f } }
		};
		float fallbackZBottom = 0.f;
		float3 groundAlbedo = { .2f, .2f, .2f };

		float planetRadius = 6.36e3f;      // in km
		float atmosphereRadius = 6.42e3f;  // in km

		float rayleighFalloff = 1 / 8.69645f;                    // in km^-1
		float3 rayleighScatter = { 6.6049f, 12.345f, 29.413f };  // in megameter^-1
		float aerosolFalloff = 1 / 1.2f;
		float aerosolPhaseG = 0.8f;
		float3 aerosolScatter = { 39.96f, 39.96f, 39.96f };
		float3 aerosolAbsorption = { 4.44f, 4.44f, 4.44f };
		float ozoneAltitude = 22.3499f + 35.66071f * .5f;  // in km
		float ozoneThickness = 35.66071f;
		float3 ozoneAbsorption = { 2.2911f, 1.5404f, 0 };

		// VANILLA CLOUDS
		bool enableVanillaClouds = true;
		float cloudRelightMix = 1.f;
		float cloudOriginalMix = 0.5f;
		float silverLiningMix = 1.f;
		float silverLiningSpread = 0.f;

		// VOLUMETRIC CLOUDS
		bool enableVolumetricClouds = false;
		float rayMarchRange = 32.f;     // km
		float shadowVolumeRange = 8.f;  // km
		uint32_t cloudMaxStep = 97;
		float temporalAccumulationFactor = 0.95f;
		bool ghostingReduction = true;
		NdfSettings cloudMap = {};
		CloudLayer cloudLayer = {};
	} settings;

	struct CbData
	{
		// DYNAMIC
		float2 texDim;
		float2 rcpTexDim;  //
		float2 frameDim;
		float2 rcpFrameDim;  //

		float zCameraPlanet;
		float3 sunDir;  //
		float3 sunlightColor;
		float trMix;  //
		float3 masserDir;
		float apLumMix;  //
		float3 masserColor;
		float apTrMix;  //
		float3 secundaDir;
		float sunDiskCos;  //
		float3 secundaColor;

		// GENERAL
		uint enabled;  //
		int tonemapper;
		float vanillaMix;

		// WORLD
		float zBottom;
		float rPlanet;  //
		float rAtmosphere;
		float3 groundAlbedo;  //

		// ATMOSPHERE
		float2 cloudShadowRemapRange;

		float aerosolFalloff;
		float aerosolPhaseG;  //
		float3 aerosolScatter;
		uint halfResApShadow;  //
		float3 aerosolAbsorption;

		float rayleighFalloff;
		float3 rayleighScatter;  //

		float ozoneAltitude;  //
		float ozoneThickness;
		float3 ozoneAbsorption;  //

		// CLOUDS (VANILLA)
		uint enableVanillaClouds;
		float cloudRelightMix;
		float cloudOriginalMix;
		float silverLiningMix;  //
		float silverLiningSpread;

		// VOLUMETRIC CLOUDS (toggle + shadow-volume parameters for GetDirlightTransmittance)
		uint enableVolumetricClouds;
		float shadowVolumeRange;
		float lowestCloudAltitude;  //
		float highestCloudAltitude;
		float3 volCloudScatter;  //
		float _padVolCloudScatter;
		float3 volCloudAbsorption;  //
		float volCloudLowBottom;
		float volCloudLowThickness;

		// SETTINGS
		uint lightSkyStatics;
		float skyStaticsBrightness;  //
	} cbData;
	STATIC_ASSERT_ALIGNAS_16(CbData);

	eastl::unique_ptr<Texture2D> texTrLut = nullptr;     // transmittance
	eastl::unique_ptr<Texture2D> texMsLut = nullptr;     // multiscattering
	eastl::unique_ptr<Texture2D> texSvLut = nullptr;     // sky view
	eastl::unique_ptr<Texture3D> texApLut = nullptr;     // aerial perspective
	eastl::unique_ptr<Texture3D> texApSunLut = nullptr;  // direct solar single-scattering aerial perspective
	eastl::unique_ptr<Texture2D> texApShadow = nullptr;

	// Volumetric cloud resources
	constexpr static uint16_t kShadowVolW = 256;
	constexpr static uint16_t kShadowVolH = 256;
	constexpr static uint16_t kShadowVolD = 64;
	constexpr static uint16_t kVolCubeSize = 64;
	constexpr static uint16_t kVolCloudDownsample = 4;

	eastl::unique_ptr<Texture2D> texVolTr = nullptr;      // blurred full-resolution volumetric transmittance result
	eastl::unique_ptr<Texture2D> texVolLum = nullptr;     // blurred full-resolution volumetric luminance result
	eastl::unique_ptr<Texture2D> texVolAux = nullptr;     // full-resolution cloud depth/metadata
	eastl::unique_ptr<Texture2D> texVolLowTr = nullptr;   // quarter-resolution trace transmittance
	eastl::unique_ptr<Texture2D> texVolLowLum = nullptr;  // quarter-resolution trace luminance
	eastl::unique_ptr<Texture2D> texVolLowAux = nullptr;  // quarter-resolution trace depth/metadata
	eastl::unique_ptr<Texture2D> texVolUpscaleTr = nullptr;
	eastl::unique_ptr<Texture2D> texVolUpscaleLum = nullptr;
	eastl::unique_ptr<Texture2D> texVolUpscaleAux = nullptr;
	eastl::unique_ptr<Texture2D> texVolHistoryTr = nullptr;
	eastl::unique_ptr<Texture2D> texVolHistoryLum = nullptr;
	eastl::unique_ptr<Texture2D> texVolHistoryAux = nullptr;
	eastl::unique_ptr<Texture2D> texVolCubeTr = nullptr;     // low-resolution cubemap transmittance result
	eastl::unique_ptr<Texture2D> texVolCubeLum = nullptr;    // low-resolution cubemap luminance result
	eastl::unique_ptr<Texture3D> texShadowVolume = nullptr;  // cloud shadow volume 3D

	winrt::com_ptr<ID3D11ShaderResourceView> baseShapeNoiseSrv = nullptr;
	winrt::com_ptr<ID3D11ShaderResourceView> nubisWarpSrv = nullptr;
	winrt::com_ptr<ID3D11ShaderResourceView> cloudTopLutSrv = nullptr;
	winrt::com_ptr<ID3D11ShaderResourceView> cloudBottomLutSrv = nullptr;

	TextureManager ndfTexManager{ "Cloud Map" };
	NdfManager ndfManager;
	HighCloudMapManager highCloudMapManager;

	// Volumetric cloud StructuredBuffer (compute-only)
	struct VolumetricCloudSB
	{
		float rayMarchRange;
		float shadowVolumeRange;
		uint cloudMaxStep;
		uint fullResolution;

		float2 frameDim;
		float2 rcpFrameDim;
		float3 dirlightDir;
		float _pad1;
		float bottomZ;
		float planetRadius;
		float2 activeFrameDim;

		float lowestCloudAltitude;
		float highestCloudAltitude;
		float lowCloudBaseAltitude;
		float lowCloudTopAltitude;
		float lowCloudTraceTopAltitude;

		float2 weatherCenter;
		float weatherWorldSize;
		float highCloudEnabled;
		float2 lowNdfFrequency;
		float2 noiseWindOffset;
		float noiseFrequency;
		float3 noiseOffset;
		float extinctionCoefficient;
		float2 noiseHeightShear;
		float warpFrequency;
		float2 highCellScale;
		float highCellWindSpeed;
		float2 highCellWarpScale;
		float highCellWarpStrength;
		float highCellThickStrength;
		float highAsCellThickStrength;
		float highCellThickPow;
		float highCloudBottom;
		float highCloudTop;
		float highBottomCoverageScale;
		float highHeightCurvePow;
		float highDensityThreshold;
		float highDensitySoftness;
		float highCloudSoftness;
		float2 highWispScale;
		float highWispStrength;
		float highDensityMultiplier;
		float highDensitySoftAIntensity;
		float highDensitySoftAContrast;
		float highDensityModAIntensity;
		float highDensityModAContrast;
		float3 scatterTint;
		float forwardEccentricity;
		float backwardEccentricity;
		float ambientTopMultiplier;
		float ambientBottomMultiplier;
		float aoUpwardScale;
		float msAttenuation;
		float msContribution;
		float msEccentricity;
		float scatterSourceODScale;
		float scatterSourceCurvePow;
		float powderIntensity;
		uint lightSteps;
		uint cloudPhaseModel;
		float phiFwdIntensity;
		float phiFwdDepthPow;
		float phiFwdDepthBias;
		float phiFwdBoundaryConfidence;
		float phiFwdMSBuildScale;
		float phiFwdCompress;
		float highForwardEccentricity;
		float highBackwardEccentricity;
		float highAmbientTopMultiplier;
		float highAmbientBottomMultiplier;
		float highSkyBlendStrength;
		float highMSAttenuation;
		float highMSContribution;
		float highMSEccentricity;
		float highLightAbsorption;
		float highViewAbsorption;
		float highCoverAbsorptionStrength;

		float2 lowFrameDim;
		float2 rcpLowFrameDim;
		uint historyValid;
		float temporalAccumulationFactor;
		float cloudHistoryInvalidation;
		uint ghostingReduction;
		uint scatterIntegration;
		float lightStepDistanceLod;
		float shadowVolumeBottom;
		float shadowVolumeTop;
	};
	eastl::unique_ptr<StructuredBuffer> volCloudSb = nullptr;

	eastl::unique_ptr<Texture2D> texVolCloudAmbientSH = nullptr;
	bool volMainHistoryValid = false;
	uint32_t volHistoryWidth = 0;
	uint32_t volHistoryHeight = 0;
	float3 volHistorySunDir = { 0.0f, 0.0f, 1.0f };

	winrt::com_ptr<ID3D11ComputeShader> csVolMainView = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> csVolReproject = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> csVolUpscale = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> csVolShadowVolume = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> csVolCubemap = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> csVolAmbientSH = nullptr;

	winrt::com_ptr<ID3D11SamplerState> sampTileable = nullptr;

	// Volumetric cloud methods
	enum class VolumetricCloudPass
	{
		kShadowVolume,
		kMainViewAndCubemap
	};
	void SetupVolumetricResources();
	void CompileVolumetricShaders();
	void LoadCloudTextures();
	void CreateNubisWarpTexture();
	void RenderVolumetricClouds(VolumetricCloudPass a_pass);

	winrt::com_ptr<ID3D11SamplerState> sampTr = nullptr;
	winrt::com_ptr<ID3D11SamplerState> sampSv = nullptr;
	winrt::com_ptr<ID3D11SamplerState> sampNoise = nullptr;

	winrt::com_ptr<ID3D11ComputeShader> csTrLutGen = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> csMsLutGen = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> csSvLutGen = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> csApLutGen = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> csShadowAccum = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> csShadowAccumHalfRes = nullptr;

	ID3D11SamplerState* originalPSSamplers[2] = { nullptr, nullptr };
	winrt::com_ptr<ID3D11SamplerState> originalPSGrassSampler = nullptr;

	void ModifySky();
	void RestoreSamplers();
	void ModifyGrass();
	void RestoreGrassSampler();
	struct Hooks
	{
		struct BSSkyShader_SetupGeometry
		{
			static void thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct BSSkyShader_RestoreGeometry
		{
			static void thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct BSGrassShader_SetupGeometry
		{
			static void thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct BSGrassShader_RestoreGeometry
		{
			static void thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		static void Install()
		{
			stl::write_vfunc<0x6, BSSkyShader_SetupGeometry>(RE::VTABLE_BSSkyShader[0]);
			stl::write_vfunc<0x7, BSSkyShader_RestoreGeometry>(RE::VTABLE_BSSkyShader[0]);
			stl::write_vfunc<0x6, BSGrassShader_SetupGeometry>(RE::VTABLE_BSGrassShader[0]);
			stl::write_vfunc<0x7, BSGrassShader_RestoreGeometry>(RE::VTABLE_BSGrassShader[0]);
		}
	};
};
