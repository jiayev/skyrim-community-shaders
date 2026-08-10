#pragma once

#include <array>

#include <d3d11_4.h>
#include <directx/d3d12.h>

#define STATIC_ASSERT_ENUM_COUNT(EnumType, Array) \
	static_assert(_countof(Array) == magic_enum::enum_count<EnumType>(), "Array size must match enum count");

#define LOAD_FN(name)                                                 \
	name = reinterpret_cast<name##Fn>(GetProcAddress(handle, #name)); \
	if (!name)                                                        \
		logger::error("[Raytracing] 'CreationEngineRaytracing.dll' " #name " is nullptr (older version?)");

struct CreationEngineRaytracing
{
	enum class Mode
	{
		None,
		GlobalIllumination,
		PathTracing,
		Debug
	};

	enum class Denoiser
	{
		None,
		NRD_Reblur,
		NRD_Relax,
		DLSS_RR,
		Accumulation
	};

	struct GeneralSettings
	{
		Denoiser Denoiser = Denoiser::None;
		Mode Mode = Mode::GlobalIllumination;
		bool RaytracedShadows = false;

		bool operator==(const GeneralSettings&) const = default;

		NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(GeneralSettings, Denoiser, Mode, RaytracedShadows)
	};

	enum class RussianRoulette
	{
		Disabled,
		Standard,
		Enhanced
	};

	struct RaytracingSettings
	{
		int Bounces = 2;
		int SamplesPerPixel = 1;
		RussianRoulette RussianRoulette = RussianRoulette::Standard;
		float ResolutionScale = 1.0f;

		bool operator==(const struct RaytracingSettings&) const = default;

		NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(RaytracingSettings, Bounces, SamplesPerPixel, RussianRoulette, ResolutionScale)
	};

	struct NRDSettings
	{
		// Parameters shared by both NVIDIA NRD denoisers (REBLUR and RELAX).

		// [0; maxFastAccumulatedFrameNum) - number of reconstructed frames after history reset
		uint32_t historyFixFrameNum = 3;

		// (> 0) - base stride between pixels in 5x5 history reconstruction kernel
		uint32_t historyFixBasePixelStride = 14;
		uint32_t historyFixAlternatePixelStride = 14;  // see "historyFixAlternatePixelStrideMaterialID"

		// [1; 3] - standard deviation scale of the color box for clamping slow "main" history to responsive "fast" history
		float fastHistoryClampingSigmaScale = 2.0f;  // 2 is old default, 1.5 works well even for dirty signals, 1.1 is a safe value for occlusion denoising

		// (pixels) - pre-accumulation spatial reuse pass blur radius (0 = disabled, must be used in case of badly defined signals and probabilistic sampling)
		float diffusePrepassBlurRadius = 30.0f;
		float specularPrepassBlurRadius = 50.0f;

		// (0; 0.2] - bigger values reduce sensitivity to shadows in spatial passes, smaller values are recommended for signals with relatively clean hit distance (like RTXDI/RESTIR)
		float minHitDistanceWeight = 0.1f;

		// (normalized %) - base fraction of diffuse or specular lobe angle used to drive normal based rejection
		float lobeAngleFraction = 0.15f;

		// (normalized %) - base fraction of center roughness used to drive roughness based rejection
		float roughnessFraction = 0.15f;

		// Helps to mitigate fireflies emphasized by DLSS. Very cheap and unbiased in most of the cases, better keep in enabled to maximize quality
		bool enableAntiFirefly = true;

		bool operator==(const NRDSettings&) const = default;

		NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(
			NRDSettings,
			historyFixFrameNum,
			historyFixBasePixelStride,
			historyFixAlternatePixelStride,
			fastHistoryClampingSigmaScale,
			diffusePrepassBlurRadius,
			specularPrepassBlurRadius,
			minHitDistanceWeight,
			lobeAngleFraction,
			roughnessFraction,
			enableAntiFirefly)
	};

	struct NRDReblurSettings
	{
		// [0; REBLUR_MAX_HISTORY_FRAME_NUM] - maximum number of linearly accumulated frames
		// Always accumulate in "seconds" not in "frames", use "GetMaxAccumulatedFrameNum" for conversion
		uint32_t maxAccumulatedFrameNum = 30;

		// [0; maxAccumulatedFrameNum] - maximum number of linearly accumulated frames for fast history
		// Values ">= maxAccumulatedFrameNum" disable fast history
		// Usually 5x-7x times shorter than the main history (casting more rays, using SHARC or other signal improving techniques help to accumulate less)
		uint32_t maxFastAccumulatedFrameNum = 6;

		// [0; maxAccumulatedFrameNum] - maximum number of linearly accumulated frames for stabilized radiance
		// "0" disables the stabilization pass
		// Values ">= maxAccumulatedFrameNum" get clamped to "maxAccumulatedFrameNum"
		uint32_t maxStabilizedFrameNum = 63;

		// (pixels) - min denoising radius (for converged state)
		float minBlurRadius = 1.0f;

		// (pixels) - base (max) denoising radius (gets reduced over time)
		float maxBlurRadius = 30.0f;

		// (normalized %) - represents maximum allowed deviation from the local tangent plane
		float planeDistanceSensitivity = 0.02f;

		// "IN_MV = lerp(IN_MV, specularMotion, smoothstep(this[0], this[1], specularProbability))"
		std::array<float, 2> specularProbabilityThresholdsForMvModification = { 0.5f, 0.9f };

		// [1; 3] - undesired sporadic outliers suppression to keep output stable (smaller values maximize suppression in exchange of bias)
		float fireflySuppressorMinRelativeScale = 2.0f;

		// In rare cases, when bright samples are so sparse that any other bright neighbor can't
		// be reached, pre-pass transforms a standalone bright pixel into a standalone bright blob,
		// worsening the situation. Despite that it's a problem of sampling, the denoiser needs to
		// handle it somehow on its side too. Diffuse pre-pass can be just disabled, but for specular
		// it's still needed to find optimal hit distance for tracking. This boolean allow to use
		// specular pre-pass for tracking purposes only (use with care)
		bool usePrepassOnlyForSpecularMotionEstimation = false;

		// Allows to get diffuse or specular history length in ".w" channel of the output instead of denoised ambient/specular occlusion (normalized hit distance).
		// Diffuse history length shows disocclusions, specular history length is more complex and includes accelerations of various kinds caused by specular tracking.
		// History length is measured in frames, it can be in "[0; maxAccumulatedFrameNum]" range
		bool returnHistoryLengthInsteadOfOcclusion = false;

		bool operator==(const NRDReblurSettings&) const = default;

		NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(
			NRDReblurSettings,
			maxAccumulatedFrameNum,
			maxFastAccumulatedFrameNum,
			maxStabilizedFrameNum,
			minBlurRadius,
			maxBlurRadius,
			planeDistanceSensitivity,
			specularProbabilityThresholdsForMvModification,
			fireflySuppressorMinRelativeScale,
			usePrepassOnlyForSpecularMotionEstimation,
			returnHistoryLengthInsteadOfOcclusion)
	};

	struct NRDRelaxSettings
	{
		// [0; RELAX_MAX_HISTORY_FRAME_NUM] - maximum number of linearly accumulated frames
		uint32_t diffuseMaxAccumulatedFrameNum = 30;
		uint32_t specularMaxAccumulatedFrameNum = 30;

		// [0; maxAccumulatedFrameNum) - maximum number of linearly accumulated frames for fast history
		// Values ">= maxAccumulatedFrameNum" disable fast history
		uint32_t diffuseMaxFastAccumulatedFrameNum = 6;
		uint32_t specularMaxFastAccumulatedFrameNum = 6;

		// A-trous edge stopping luminance sensitivity
		float diffusePhiLuminance = 2.0f;
		float specularPhiLuminance = 1.0f;

		// [2; 8] - number of iterations for A-Trous wavelet transform
		uint32_t atrousIterationNum = 3;

		// (>= 0) - how much variance we inject to specular if reprojection confidence is low
		float specularVarianceBoost = 0.0f;

		// (degrees) - slack for the specular lobe angle used in normal based rejection during A-Trous passes
		float specularLobeAngleSlack = 0.15f;

		// (normalized %) - depth threshold for spatial passes
		float depthThreshold = 0.003f;

		// Roughness based rejection
		bool enableRoughnessEdgeStopping = true;

		bool operator==(const NRDRelaxSettings&) const = default;

		NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(
			NRDRelaxSettings,
			diffuseMaxAccumulatedFrameNum,
			specularMaxAccumulatedFrameNum,
			diffuseMaxFastAccumulatedFrameNum,
			specularMaxFastAccumulatedFrameNum,
			diffusePhiLuminance,
			specularPhiLuminance,
			atrousIterationNum,
			specularVarianceBoost,
			specularLobeAngleSlack,
			depthThreshold,
			enableRoughnessEdgeStopping)
	};

	struct MaterialSettings
	{
		float2 Roughness = { 0.0f, 1.0f };
		float2 Metalness = { 0.0f, 1.0f };

		bool operator==(const MaterialSettings&) const = default;

		NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(MaterialSettings, Roughness, Metalness)
	};

	struct LightingSettings
	{
		float Directional = 5.0f;
		float Point = 5.0f;
		bool LodDimmer = false;
		float Emissive = 5.0f;
		float Effect = 5.0f;
		float Sky = 5.0f;

		bool operator==(const LightingSettings&) const = default;

		NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(LightingSettings, Directional, Point, LodDimmer, Emissive, Effect, Sky)
	};

	struct SHaRCSettings
	{
		bool Enabled = false;
		float SceneScale = 1.0f;
		int AccumFrameNum = 10;
		int StaleFrameNum = 64;
		float RadianceScale = 1e3f;

		bool operator==(const SHaRCSettings&) const = default;

		NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(SHaRCSettings, Enabled, SceneScale, AccumFrameNum, StaleFrameNum)
	};

	// Resampled Importance Sampling
	struct RISSettings
	{
		bool Enabled = false;
		int MaxCandidates = 4;

		bool operator==(const RISSettings&) const = default;

		NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(RISSettings, Enabled, MaxCandidates)
	};

	// TODO: Rename to ReflectanceModel?
	enum struct DiffuseBRDF : int32_t
	{
		Lambert,
		Burley,
		OrenNayar,
		Gotanda,
		Chan
	};

	enum struct HairBSDF : int32_t
	{
		None,
		ChiangBSDF,
		FarFieldBCSDF
	};

	struct SSSSettings
	{
		bool Enabled = false;
		int SampleCount = 1;
		float MaxSampleRadius = 1.0f;
		bool EnableTransmission = true;

		bool MaterialOverride = false;
		float3 OverrideTransmissionColor = float3(1.0f, 0.735f, 0.612f);
		float3 OverrideScatteringColor = float3(1.0f, 1.0f, 1.0f);
		float OverrideScale = 40.0f;
		float OverrideAnisotropy = -0.5f;

		bool operator==(const SSSSettings&) const = default;

		NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(
			SSSSettings,
			Enabled,
			SampleCount,
			MaxSampleRadius,
			EnableTransmission,
			MaterialOverride,
			OverrideTransmissionColor,
			OverrideScatteringColor,
			OverrideScale,
			OverrideAnisotropy)
	};

	struct AdvancedSettings
	{
		uint NumWorkerThreads = 8;
		float TexLODBias = -1.0f;
		bool VariableUpdateRate = true;
		bool GGXEnergyConservation = true;
		bool PerLightTLAS = false;
		RISSettings RIS;
		HairBSDF HairBSDF = HairBSDF::FarFieldBCSDF;
		DiffuseBRDF DiffuseBRDF = DiffuseBRDF::Burley;
		SSSSettings SSSSettings;
		bool StablePlanes = false;

		bool operator==(const AdvancedSettings&) const = default;

		NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(
			AdvancedSettings,
			NumWorkerThreads,
			TexLODBias,
			VariableUpdateRate,
			GGXEnergyConservation,
			PerLightTLAS,
			RIS,
			HairBSDF,
			DiffuseBRDF,
			SSSSettings,
			StablePlanes)
	};

	struct WaterSettings
	{
		float AbsorptionScale = 1.0f;

		bool operator==(const WaterSettings&) const = default;

		NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(WaterSettings, AbsorptionScale)
	};

	enum struct ReSTIRGIResamplingMode : int32_t
	{
		None = 0,
		Temporal = 1,
		Spatial = 2,
		TemporalAndSpatial = 3,
		FusedSpatiotemporal = 4,
	};

	enum struct ReSTIRGIBiasCorrection : int32_t
	{
		Off = 0,
		Basic = 1,
		Raytraced = 3
	};

	struct ReSTIRGISettings
	{
		bool Enabled = false;
		ReSTIRGIResamplingMode ResamplingMode = ReSTIRGIResamplingMode::TemporalAndSpatial;

		float TemporalDepthThreshold = 0.1f;
		float TemporalNormalThreshold = 0.5f;
		int MaxHistoryLength = 20;
		int MaxReservoirAge = 100;
		bool EnablePermutationSampling = true;
		bool EnableFallbackSampling = true;
		ReSTIRGIBiasCorrection TemporalBiasCorrection = ReSTIRGIBiasCorrection::Basic;

		float SpatialDepthThreshold = 0.1f;
		float SpatialNormalThreshold = 0.5f;
		int SpatialNumSamples = 2;
		float SpatialSamplingRadius = 32.0f;
		ReSTIRGIBiasCorrection SpatialBiasCorrection = ReSTIRGIBiasCorrection::Basic;

		bool EnableBoilingFilter = true;
		float BoilingFilterStrength = 0.4f;

		bool EnableFinalVisibility = true;
		bool EnableFinalMIS = false;

		bool operator==(const ReSTIRGISettings&) const = default;

		NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(ReSTIRGISettings, Enabled, ResamplingMode,
			TemporalDepthThreshold, TemporalNormalThreshold, MaxHistoryLength, MaxReservoirAge,
			EnablePermutationSampling, EnableFallbackSampling, TemporalBiasCorrection,
			SpatialDepthThreshold, SpatialNormalThreshold, SpatialNumSamples, SpatialSamplingRadius,
			SpatialBiasCorrection, EnableBoilingFilter, BoilingFilterStrength,
			EnableFinalVisibility, EnableFinalMIS)
	};

	enum struct PTCullMode : uint32_t
	{
		Disabled = 0,
		Enabled = 1,
		Full = 2
	};

	enum struct TextureMode : uint32_t
	{
		Share = 0,
		Exclusive = 1
	};

	struct ExperimentalSettings
	{
		PTCullMode PathTracingCull = PTCullMode::Enabled;
		TextureMode TextureMode = TextureMode::Share;
		uint32_t TextureCutOff = 0;
		bool GlobalLights = false;

		bool operator==(const ExperimentalSettings&) const = default;

		NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(ExperimentalSettings, PathTracingCull, TextureMode, TextureCutOff, GlobalLights)
	};

	enum struct TimingMode
	{
		Disabled,
		Standard,
		Extended
	};

	struct DebugSettings
	{
		bool Markers = false;
		TimingMode Timings = TimingMode::Disabled;

		bool operator==(const DebugSettings&) const = default;

		NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(DebugSettings, Markers, Timings)
	};

	struct PassTiming
	{
		eastl::string name;
		float gpuTiming;
		float cpuTiming;
	};

	struct Settings
	{
		bool Enabled = true;
		GeneralSettings GeneralSettings;
		LightingSettings LightingSettings;
		RaytracingSettings RaytracingSettings;
		NRDSettings NRDSettings;
		NRDReblurSettings NRDReblurSettings;
		NRDRelaxSettings NRDRelaxSettings;
		MaterialSettings MaterialSettings;
		SHaRCSettings SHaRCSettings;
		AdvancedSettings AdvancedSettings;
		WaterSettings WaterSettings;
		ExperimentalSettings ExperimentalSettings;
		ReSTIRGISettings ReSTIRGI;
		DebugSettings DebugSettings;

		bool operator==(const Settings&) const = default;

		NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(
			Settings,
			Enabled,
			GeneralSettings,
			LightingSettings,
			RaytracingSettings,
			NRDSettings,
			NRDReblurSettings,
			NRDRelaxSettings,
			MaterialSettings,
			SHaRCSettings,
			AdvancedSettings,
			WaterSettings,
			ExperimentalSettings,
			ReSTIRGI,
			DebugSettings)
	};

	struct SharedTexture
	{
		ID3D12Resource* native = nullptr;
		ID3D11Texture2D* shared = nullptr;
	};
	
	static constexpr uint32_t MAX_FRAMES_IN_FLIGHT = 2;

	HMODULE handle = nullptr;

	using InitializeRendererFn = bool (*)(ID3D11Device5*, ID3D12Device5*, ID3D12CommandQueue*, ID3D12CommandQueue*, ID3D12CommandQueue*);
	using InitializeVulkanRendererFn = bool (*)(void* instance, void* physicalDevice, void* device, void* graphicsQueue, int graphicsQueueIndex, void* transferQueue, int transferQueueIndex, void* computeQueue, int computeQueueIndex);
	using InitializeFn = void (*)(Settings);
	using UpdateCameraFn = void (*)();
	using ExecuteFn = void (*)();
	using PostExecutionFn = uint32_t (*)();
	using GetResolutionFn = void (*)(uint32_t&, uint32_t&);
	using SetResolutionFn = void (*)(uint32_t, uint32_t);
	using UpdateFeatureDataFn = void (*)(void*, uint32_t);
	using SetSkyHemisphereFn = void (*)(ID3D12Resource*);
	using SetWaterFlowMapFn = void (*)(ID3D12Resource*);
	using GetPassTimingsFn = void (*)(eastl::vector<PassTiming>&);
	using UpdateSettingsFn = void (*)(Settings);
	using GetRRInputFn = void (*)(ID3D12Resource*&, ID3D12Resource*&);
	using SetSharedTexturesFn = void (*)(ID3D12Resource*, ID3D12Resource*, ID3D12Resource*);
	using GetSharedTexturesFn = void (*)(SharedTexture*, SharedTexture*, SharedTexture*, SharedTexture*);
	using UpdateJitterFn = void (*)(float2);
	using SetSkinDetailNormalFn = void (*)(ID3D12Resource*);
	using GetAccumulatedFrameCountFn = uint32_t (*)();
	using GetFakeDoubledVRAMUsageFn = uint64_t (*)();
	using GetSceneGraphCountersFn = void (*)(uint32_t& textures, uint32_t& models, uint32_t& instances);

	InitializeRendererFn InitializeRenderer = nullptr;
	InitializeVulkanRendererFn InitializeVulkanRenderer = nullptr;	
	InitializeFn Initialize = nullptr;
	UpdateCameraFn UpdateCamera = nullptr;
	ExecuteFn Execute = nullptr;
	PostExecutionFn PostExecution = nullptr;
	SetResolutionFn SetResolution = nullptr;
	UpdateFeatureDataFn UpdateFeatureData = nullptr;
	SetSkyHemisphereFn SetSkyHemisphere = nullptr;
	SetWaterFlowMapFn SetWaterFlowMap = nullptr;
	GetPassTimingsFn GetPassTimings = nullptr;
	GetSceneGraphCountersFn GetSceneGraphCounters = nullptr;
	UpdateSettingsFn UpdateSettings = nullptr;
	GetRRInputFn GetRRInput = nullptr;
	SetSharedTexturesFn SetSharedTextures = nullptr;
	GetSharedTexturesFn GetSharedTextures = nullptr;
	UpdateJitterFn UpdateJitter = nullptr;
	SetSkinDetailNormalFn SetSkinDetailNormal = nullptr;
	GetAccumulatedFrameCountFn GetAccumulatedFrameCount = nullptr;
	GetFakeDoubledVRAMUsageFn GetFakeDoubledVRAMUsage = nullptr;

	CreationEngineRaytracing()
	{
		GetModuleHandleEx(
			GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			L"CreationEngineRaytracing.dll",
			&handle);

		if (!handle) {
			logger::critical("[Raytracing] 'CreationEngineRaytracing.dll' not found, make sure Creation Engine Raytracing is enabled in your mod manager.");
			return;
		}

		LOAD_FN(InitializeRenderer);
		LOAD_FN(InitializeVulkanRenderer);		
		LOAD_FN(Initialize);
		LOAD_FN(UpdateCamera);
		LOAD_FN(Execute);
		LOAD_FN(PostExecution);
		LOAD_FN(SetResolution);
		LOAD_FN(UpdateFeatureData);
		LOAD_FN(SetSkyHemisphere);
		LOAD_FN(SetWaterFlowMap);
		LOAD_FN(GetPassTimings);
		LOAD_FN(GetSceneGraphCounters);
		LOAD_FN(UpdateSettings);
		LOAD_FN(GetRRInput);
		LOAD_FN(SetSharedTextures);
		LOAD_FN(GetSharedTextures);
		LOAD_FN(UpdateJitter);
		LOAD_FN(SetSkinDetailNormal);
		LOAD_FN(GetAccumulatedFrameCount);
		LOAD_FN(GetFakeDoubledVRAMUsage);
	}
};
