#pragma once

#include "Buffer.h"
#include "Feature.h"

class MenuOpenCloseEventHandler : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
{
public:
	virtual RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>* a_eventSource);
	static bool Register();
};

struct DynamicCubemaps : Feature
{
public:
	static DynamicCubemaps* GetSingleton()
	{
		static DynamicCubemaps singleton;
		return &singleton;
	}

	bool renderedScreenCamera = false;

	// Specular irradiance

	ID3D11SamplerState* computeSampler = nullptr;

	struct alignas(16) SpecularMapFilterSettingsCB
	{
		float roughness;
		float pad[3];
	};

	ID3D11ComputeShader* specularIrradianceCS = nullptr;
	ConstantBuffer* spmapCB = nullptr;
	Texture2D* envTexture = nullptr;
	winrt::com_ptr<ID3D11UnorderedAccessView> uavArray[9];

	// BRDF 2D LUT

	ID3D11ComputeShader* spBRDFProgram = nullptr;
	Texture2D* spBRDFLUT = nullptr;

	// Reflection capture

	struct alignas(16) UpdateCubemapCB
	{
		float4 CameraData;
		uint Reset;
		float3 CameraPreviousPosAdjust;
	};

	ID3D11ComputeShader* updateCubemapCS = nullptr;
	ConstantBuffer* updateCubemapCB = nullptr;

	ID3D11ComputeShader* inferCubemapCS = nullptr;
	ID3D11ComputeShader* inferCubemapReflectionsCS = nullptr;

	Texture2D* envCaptureTexture = nullptr;
	Texture2D* envCaptureRawTexture = nullptr;
	Texture2D* envCapturePositionTexture = nullptr;
	Texture2D* envInferredTexture = nullptr;

	bool activeReflections = false;
	bool resetCapture = true;

	enum class NextTask
	{
		kCapture,
		kInferrence,
		kIrradiance
	};

	NextTask nextTask = NextTask::kCapture;

	ID3D11UnorderedAccessView* cubemapUAV;

	void UpdateCubemap();

	virtual inline std::string GetName() { return "Dynamic Cubemaps"; }
	virtual inline std::string GetShortName() { return "DynamicCubemaps"; }
	inline std::string_view GetShaderDefineName() override { return "DYNAMIC_CUBEMAPS"; }
	bool HasShaderDefine(RE::BSShader::Type) override { return true; };

	virtual void SetupResources();
	virtual void Reset();

	virtual void DrawSettings();
	virtual void DataLoaded() override;

	virtual void Draw(const RE::BSShader* shader, const uint32_t descriptor);

	virtual void Load(json& o_json);
	virtual void Save(json& o_json);

	virtual void RestoreDefaultSettings();

	std::vector<std::string> iniVRCubeMapSettings{
		{ "bAutoWaterSilhouetteReflections:Water" },  //IniSettings 0x1eaa018
		{ "bForceHighDetailReflections:Water" },      //IniSettings 0x1eaa030
	};

	std::map<std::string, uintptr_t> hiddenVRCubeMapSettings{
		{ "bReflectExplosions:Water", 0x1eaa000 },
		{ "bReflectLODLand:Water", 0x1eaa060 },
		{ "bReflectLODObjects:Water", 0x1eaa078 },
		{ "bReflectLODTrees:Water", 0x1eaa090 },
		{ "bReflectSky:Water", 0x1eaa0a8 },
		{ "bUseWaterRefractions:Water", 0x1eaa0c0 },
	};

	virtual void ClearShaderCache() override;
	ID3D11ComputeShader* GetComputeShaderUpdate();
	ID3D11ComputeShader* GetComputeShaderInferrence();
	ID3D11ComputeShader* GetComputeShaderInferrenceReflections();
	ID3D11ComputeShader* GetComputeShaderSpecularIrradiance();

	void UpdateCubemapCapture();

	virtual void DrawDeferred();
};
