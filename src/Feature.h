#pragma once

struct Feature
{
	bool loaded = false;
	std::string version;

	virtual std::string GetName() = 0;
	virtual std::string GetIniFilename() = 0;
	virtual std::string GetIniName() = 0;

	virtual void SetupResources() = 0;
	virtual void Reset() = 0;

	virtual void DrawSettings() = 0;
	virtual void Draw(const RE::BSShader* shader, const uint32_t descriptor) = 0;

	virtual void Load(json& o_json);
	virtual void Save(json& o_json);

	virtual bool ValidateCache(CSimpleIniA& a_ini);
	virtual void WriteDiskCacheInfo(CSimpleIniA& a_ini);

	// Cat: add all the features in here
	static const std::vector<Feature*>& GetFeatureList();
};