#pragma once

struct Skin : Feature
{
	static Skin* GetSingleton()
	{
		static Skin singleton;
		return &singleton;
	}

	virtual inline std::string GetName() override { return "Advanced Skin"; }
	virtual inline std::string GetShortName() override { return "Skin"; }
	virtual inline std::string_view GetShaderDefineName() override { return "CS_SKIN"; }
	virtual inline bool HasShaderDefine(RE::BSShader::Type t) override
	{
		return t == RE::BSShader::Type::Lighting;
	};

	virtual inline bool SupportsVR() { return true; }

	virtual void RestoreDefaultSettings() override;
	virtual void DrawSettings() override;

	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;

	virtual void Prepass() override;
	virtual void PostPostLoad() override;

	virtual void SetupResources() override;

	void ReloadSkinDetail();

	struct Settings
	{
		bool EnableSkin = true;
		float SkinMainRoughness = 0.7f;
		float SkinSecondRoughness = 0.35f;
		float SkinSpecularTexMultiplier = 1.0f;
		float SecondarySpecularStrength = 0.15f;
		float F0 = 0.0278f;
		float BaseColorMultiplier = 1.0f;
		float PhysicalMainRoughnessMultiplier = 1.3f;
		float PhysicalSecondRoughnessMultiplier = 0.75f;
		float PhysicalSpecularStrength = 1.0f;
		float ExtraEdgeRoughness = 0.25f;
		bool EnableSkinDetail = true;
		float SkinDetailStrength = 0.5f;
		float SkinDetailTiling = 40.0f;
		float BodyTilingMultiplier = 2.0f;
		float ExtraSkinWetness = 0.0f;
		float WetFadeTime = 10.0f;
		float StartSweat = 0.75f;
		float FullSweat = 0.15f;
		float4 WetParams = { 512.0f, 0.7, 10.0, 4.0f };
		float Translucency = 0.1f;
		float sssWidth = 0.2f;
		bool UseSSS = true;
		float FuzzStrength = 1.0f;
		float FuzzRoughness = 0.35f;
		float FuzzF0 = 0.045f;
	} settings;

	struct alignas(16) SkinData
	{
		float4 skinParams;
		float4 skinParams2;
		float4 skinDetailParams;
		float4 sssParams;
		float4 fuzzParams;
		float4 physicalParams;
		float4 wetParams;
	};

	struct alignas(16) PerGeometryData
	{
		float4 skinPerGeometry;
	};

	ConstantBuffer* PerGeometryCB = nullptr;
	float4 currentWetness = { 0.0f, 0.0f, 0.0f, 0.0f };
	float playerStamina = 0.0f;
	float playerStaminaMax = 0.0f;

	eastl::unique_ptr<Texture2D> texSkinDetail = nullptr;
	std::unordered_map<uint32_t, RE::NiSourceTexturePtr[2]> skinExtraTextures;
	std::unordered_map<uint32_t, float4> actorWetnessMap;

	SkinData GetCommonBufferData();
	float GetWaterHeight(const RE::TESObjectREFR* a_ref, const RE::NiPoint3& a_pos);
	float4 GetWetness(RE::BSGeometry* geometry);

	void SetupExtraTexture(RE::BSLightingShaderMaterialBase const* material, RE::BSTextureSet* inTextureSet);
	void BSLightingShader_SetupMaterial(RE::BSLightingShaderMaterialBase const* material);
	void BSLightingShader_SetupGeometry(RE::BSRenderPass* a_pass);
	void SetShaderResouces(ID3D11DeviceContext* a_context);

	struct Hooks
	{
		struct BSLightingShader_SetupGeometry
		{
			static void thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		static void Install()
		{
			stl::write_vfunc<0x6, BSLightingShader_SetupGeometry>(RE::VTABLE_BSLightingShader[0]);
			logger::info("[Advanced Skin] Installed hooks");
		}
	};
};