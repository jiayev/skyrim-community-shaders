#include "GrassLighting.h"

#include "State.h"
#include "Util.h"

#include "Features/Clustered.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	GrassLighting::Settings,
	Glossiness,
	SpecularStrength,
	SubsurfaceScatteringAmount,
	EnableDirLightFix,
	EnablePointLights)

enum class GrassShaderTechniques
{
	RenderDepth = 8,
};

void GrassLighting::DrawSettings()
{
	if (ImGui::BeginTabItem("Grass Lighting")) {
		if (ImGui::TreeNodeEx("Complex Grass", ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::Text("Specular highlights for complex grass.\nFunctions the same as on other objects.");
			ImGui::SliderFloat("Glossiness", &settings.Glossiness, 1.0f, 100.0f);
			ImGui::SliderFloat("Specular Strength", &settings.SpecularStrength, 0.0f, 1.0f);

			ImGui::TreePop();
		}

		if (ImGui::TreeNodeEx("Effects", ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::Text("Soft lighting controls how evenly lit an object is.");
			ImGui::Text("Back lighting illuminates the back face of an object.");
			ImGui::Text("Combined to model the transport of light through the surface.");
			ImGui::SliderFloat("Subsurface Scattering Amount", &settings.SubsurfaceScatteringAmount, 0.0f, 1.0f);

			ImGui::TreePop();
		}

		if (ImGui::TreeNodeEx("Lights", ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::Text("Fix for grass not being affected by sunlight scale.");
			ImGui::Checkbox("Enable Directional Light Fix", (bool*)&settings.EnableDirLightFix);

			ImGui::Text("Enables point lights on grass like other objects. Slightly impacts performance if there are a lot of lights.");
			ImGui::Checkbox("Enable Point Lights", (bool*)&settings.EnablePointLights);

			ImGui::TreePop();
		}

		ImGui::EndTabItem();
	}
}

void GrassLighting::ModifyGrass(const RE::BSShader*, const uint32_t descriptor)
{
	const auto technique = descriptor & 0b1111;
	if (technique != static_cast<uint32_t>(GrassShaderTechniques::RenderDepth)) {
		if (updatePerFrame) {
			PerFrame perFrameData{};
			ZeroMemory(&perFrameData, sizeof(perFrameData));

			auto& shaderState = RE::BSShaderManager::State::GetSingleton();
			RE::NiTransform& dalcTransform = shaderState.directionalAmbientTransform;

			Util::StoreTransform3x4NoScale(perFrameData.DirectionalAmbient, dalcTransform);

			auto accumulator = RE::BSGraphics::BSShaderAccumulator::GetCurrentAccumulator();
			auto& position = accumulator->GetRuntimeData().eyePosition;
			auto state = RE::BSGraphics::RendererShadowState::GetSingleton();

			RE::NiPoint3 eyePosition{};
			if (REL::Module::IsVR()) {
				// find center of eye position
				eyePosition = state->GetVRRuntimeData().posAdjust.getEye() + state->GetVRRuntimeData().posAdjust.getEye(1);
				eyePosition /= 2;
			} else
				eyePosition = state->GetRuntimeData().posAdjust.getEye();
			perFrameData.EyePosition.x = position.x - eyePosition.x;
			perFrameData.EyePosition.y = position.y - eyePosition.y;
			perFrameData.EyePosition.z = position.z - eyePosition.z;

			auto manager = RE::ImageSpaceManager::GetSingleton();
			perFrameData.SunlightScale = manager->data.baseData.hdr.sunlightScale;

			perFrameData.Settings = settings;

			perFrame->Update(perFrameData);

			updatePerFrame = false;
		}

		Clustered::GetSingleton()->Bind(true);
		auto context = RE::BSGraphics::Renderer::GetSingleton()->GetRuntimeData().context;

		ID3D11Buffer* buffers[2];
		context->VSGetConstantBuffers(2, 1, buffers);  // buffers[0]
		buffers[1] = perFrame->CB();
		context->VSSetConstantBuffers(2, ARRAYSIZE(buffers), buffers);
		context->PSSetConstantBuffers(2, ARRAYSIZE(buffers), buffers);
	}
}

void GrassLighting::Draw(const RE::BSShader* shader, const uint32_t descriptor)
{
	switch (shader->shaderType.get()) {
	case RE::BSShader::Type::Grass:
		ModifyGrass(shader, descriptor);
		break;
	}
}

void GrassLighting::Load(json& o_json)
{
	settings = o_json["Grass Lighting"];

	CSimpleIniA ini;
	ini.SetUnicode();
	ini.LoadFile(L"Data\\Shaders\\Features\\GrassLighting.ini");
	if (auto value = ini.GetValue("Info", "Version")) {
		enabled = true;
		version = value;
		logger::info("GrassLighting.ini successfully loaded");
	} else {
		enabled = false;
		logger::warn("GrassLighting.ini not successfully loaded");
	}
}

void GrassLighting::Save(json& o_json)
{
	o_json["Grass Lighting"] = settings;
}

void GrassLighting::SetupResources()
{
	perFrame = new ConstantBuffer(ConstantBufferDesc<PerFrame>());
}

void GrassLighting::Reset()
{
	updatePerFrame = true;
}

bool GrassLighting::ValidateCache(CSimpleIniA& a_ini)
{
	logger::info("Validating Grass Lighting");

	auto enabledInCache = a_ini.GetBoolValue("Grass Lighting", "Enabled", false);
	if (enabledInCache && !enabled) {
		logger::info("Feature was uninstalled");
		return false;
	}
	if (!enabledInCache && enabled) {
		logger::info("Feature was installed");
		return false;
	}

	if (enabled) {
		auto versionInCache = a_ini.GetValue("Grass Lighting", "Version");
		if (strcmp(versionInCache, version.c_str()) != 0) {
			logger::info("Change in version detected. Installed {} but {} in Disk Cache", version, versionInCache);
			return false;
		} else {
			logger::info("Installed version and cached version match.");
		}
	}

	logger::info("Cached feature is valid");
	return true;
}

void GrassLighting::WriteDiskCacheInfo(CSimpleIniA& a_ini)
{
	a_ini.SetBoolValue("Grass Lighting", "Enabled", enabled);
	a_ini.SetValue("Grass Lighting", "Version", version.c_str());
}