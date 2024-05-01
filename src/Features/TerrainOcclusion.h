#pragma once

#include "Buffer.h"
#include "Feature.h"

struct TerrainOcclusion : public Feature
{
	static TerrainOcclusion* GetSingleton()
	{
		static TerrainOcclusion singleton;
		return std::addressof(singleton);
	}

	virtual inline std::string GetName() { return "Terrain Occlusion"; }
	virtual inline std::string GetShortName() { return "TerrainOcclusion"; }
	inline std::string_view GetShaderDefineName() override { return "TERRA_OCC"; }
	inline bool HasShaderDefine(RE::BSShader::Type type) override { return type == RE::BSShader::Type::Lighting; };

	uint shadowUpdateIdx = 0;

	struct Settings
	{
		struct AOGenSettings
		{
			float AoDistance = 12;
			uint SliceCount = 60;
			uint SampleCount = 60;
		} AoGen;

		struct EffectSettings
		{
			uint EnableTerrainShadow = true;
			uint EnableTerrainAO = true;

			float HeightBias = -1000.f;  // in game unit

			float ShadowSofteningRadiusAngle = 1.f * RE::NI_PI / 180.f;
			float2 ShadowFadeDistance = { 1000.f, 2000.f };

			float AOMix = 1.f;
			float AOPower = 1.f;
			float AOFadeOutHeight = 2000;
		} Effect;
	} settings;

	bool needPrecompute = false;

	struct HeightMapMetadata
	{
		std::wstring dir;
		std::string filename;
		std::string worldspace;
		float3 pos0, pos1;  // left-top-z=0 vs right-bottom-z=1
		float2 zRange;
	};
	std::unordered_map<std::string, HeightMapMetadata> heightmaps;
	HeightMapMetadata* cachedHeightmap;

	struct AOGenBuffer
	{
		Settings::AOGenSettings settings;

		float3 pos0;
		float3 pos1;
		float2 zRange;
	};
	std::unique_ptr<Buffer> aoGenBuffer = nullptr;

	struct ShadowUpdateCB
	{
		float2 LightPxDir;   // direction on which light descends, from one pixel to next via dda
		float2 LightDeltaZ;  // per LightUVDir, upper penumbra and lower, should be negative
		uint StartPxCoord;
		float2 PxSize;

		float pad;
	} shadowUpdateCBData;
	static_assert(sizeof(ShadowUpdateCB) % 16 == 0);
	std::unique_ptr<ConstantBuffer> shadowUpdateCB = nullptr;

	struct PerPass
	{
		Settings::EffectSettings effect;

		float3 scale;
		float3 invScale;
		float3 offset;
		float2 zRange;
	};
	std::unique_ptr<Buffer> perPass = nullptr;

	winrt::com_ptr<ID3D11ComputeShader> occlusionProgram = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> shadowUpdateProgram = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> outputProgram = nullptr;

	std::unique_ptr<Texture2D> texHeightMap = nullptr;
	std::unique_ptr<Texture2D> texOcclusion = nullptr;
	std::unique_ptr<Texture2D> texNormalisedHeight = nullptr;
	std::unique_ptr<Texture2D> texShadowHeight = nullptr;

	bool IsHeightMapReady();

	virtual void SetupResources() override;
	void CompileComputeShaders();

	virtual void DrawSettings() override;

	virtual inline void Reset() override{};

	virtual void Draw(const RE::BSShader*, const uint32_t) override;
	void UpdateBuffer();
	void DrawTerrainOcclusion();
	void LoadHeightmap();
	void Precompute();
	void UpdateShadow();

	virtual void Load(json& o_json) override;
	virtual void Save(json&) override;

	virtual inline void RestoreDefaultSettings() override { settings = {}; }
	virtual void ClearShaderCache() override;
};