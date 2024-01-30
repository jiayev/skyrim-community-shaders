#pragma once

struct Feature
{
	bool loaded = false;
	std::string version;

	virtual std::string GetName() = 0;
	virtual std::string GetShortName() = 0;
	virtual std::string_view GetShaderDefineName() { return ""; }

	virtual bool HasShaderDefine(RE::BSShader::Type) { return false; }

	virtual void SetupResources() = 0;
	virtual void Reset() = 0;

	virtual void DrawSettings() = 0;
	virtual void Draw(const RE::BSShader* shader, const uint32_t descriptor) = 0;
	virtual void DrawDeferred() {}

	virtual void DataLoaded() {}
	virtual void PostPostLoad() {}

	virtual void Load(json& o_json);
	virtual void Save(json& o_json) = 0;

	virtual void RestoreDefaultSettings() = 0;

	virtual bool ValidateCache(CSimpleIniA& a_ini);
	virtual void WriteDiskCacheInfo(CSimpleIniA& a_ini);
	virtual void ClearShaderCache() {}

	// Cat: add all the features in here
	static const std::vector<Feature*>& GetFeatureList();
};