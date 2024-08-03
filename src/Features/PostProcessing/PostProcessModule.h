#pragma once

struct PostProcessModuleConstructor;

struct PostProcessModule
{
	virtual ~PostProcessModule() = default;

	virtual std::string GetName() = 0;
	virtual std::string GetDesc() = 0;

	virtual inline void SetupResources() {}
	virtual void ClearShaderCache() = 0;
	virtual void RestoreDefaultSettings() = 0;
	virtual void LoadSettings(json& o_json) = 0;
	virtual void SaveSettings(json& o_json) = 0;
	virtual void DrawSettings() = 0;
	virtual void Draw(ID3D11Texture2D* inputTex, ID3D11Texture2D* inputSrv, ID3D11Texture2D* inputUav = nullptr) = 0;
};

struct PostProcessModuleConstructor
{
	std::function<PostProcessModule*()> fn;
	std::string name;
	std::string desc;

	static const ankerl::unordered_dense::map<std::string, PostProcessModuleConstructor>& GetModuleConstructors();
};

struct TestModule : public PostProcessModule
{
	virtual inline std::string GetName() override { return "Test Module"; }
	virtual inline std::string GetDesc() override { return "Module only for testing"; }

	virtual inline void ClearShaderCache() override{};
	virtual inline void RestoreDefaultSettings() override{};
	virtual inline void LoadSettings(json&) override{};
	virtual inline void SaveSettings(json&) override{};
	virtual inline void DrawSettings() override { ImGui::Text("Hello!"); };
	virtual inline void Draw(ID3D11Texture2D*, ID3D11Texture2D*, ID3D11Texture2D*) override {}
};