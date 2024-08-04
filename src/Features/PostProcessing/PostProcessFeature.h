#pragma once

#include "Feature.h"

struct PostProcessFeatureConstructor;

struct PostProcessFeature
{
	virtual ~PostProcessFeature() = default;

	bool enabled = true;

	virtual std::string GetType() const = 0;
	std::string name;
	virtual std::string GetDesc() const = 0;
	virtual bool SupportsVR() const { return false; }

	virtual inline void SetupResources() = 0;
	virtual void ClearShaderCache() = 0;
	virtual void RestoreDefaultSettings() = 0;

	virtual void LoadSettings(json& o_json) = 0;
	virtual void SaveSettings(json& o_json) = 0;
	virtual void DrawSettings() = 0;
	virtual void Draw(ID3D11Texture2D* inputTex, ID3D11Texture2D* inputSrv, ID3D11Texture2D* inputUav = nullptr) = 0;

	virtual inline void Reset(){};
};

struct PostProcessFeatureConstructor
{
	std::function<PostProcessFeature*()> fn;
	std::string name;
	std::string desc;

	template <class T>
	static inline std::pair<std::string, PostProcessFeatureConstructor> GetFeatureConstructorPair()
	{
		return { T().GetType(), { []() { return new T(); }, T().GetType(), T().GetDesc() } };
	};
	static const ankerl::unordered_dense::map<std::string, PostProcessFeatureConstructor>& GetFeatureConstructors();
};

struct TestFeature : public PostProcessFeature
{
	virtual inline std::string GetType() const override { return "Test Feature"; }
	virtual inline std::string GetDesc() const override { return "Feature only for testing"; }

	virtual inline void SetupResources() override{};
	virtual inline void ClearShaderCache() override{};
	virtual inline void RestoreDefaultSettings() override{};
	virtual inline void LoadSettings(json&) override{};
	virtual inline void SaveSettings(json&) override{};
	virtual inline void DrawSettings() override { ImGui::Text("Hello!"); };
	virtual inline void Draw(ID3D11Texture2D*, ID3D11Texture2D*, ID3D11Texture2D*) override {}
};