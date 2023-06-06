#include "DistantTreeLighting.h"

#include "State.h"
#include "Util.h"

void DistantTreeLighting::DrawSettings()
{
	if (ImGui::BeginTabItem("Tree LOD Lighting")) {

		if (ImGui::TreeNodeEx("Complex Tree LOD", ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::Text("Enables advanced lighting simulation on tree LOD.");
			ImGui::Text("Requires DynDOLOD.");
			ImGui::Text("See https://dyndolod.info/ for more information.");
			ImGui::Checkbox("Enable Complex Tree LOD", (bool*)&settings.EnableComplexTreeLOD);

			ImGui::TreePop();
		}

		if (ImGui::TreeNodeEx("Lights", ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::Text("Fix for trees not being affected by sunlight scale.");
			ImGui::Checkbox("Enable Directional Light Fix", (bool*)&settings.EnableDirLightFix);

			ImGui::TreePop();
		}

		if (ImGui::TreeNodeEx("Effects", ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::Text("Soft lighting controls how evenly lit an object is.");
			ImGui::Text("Back lighting illuminates the back face of an object.");
			ImGui::Text("Combined to model the transport of light through the surface.");
			ImGui::SliderFloat("Subsurface Scattering Amount", &settings.SubsurfaceScatteringAmount, 0.0f, 1.0f);

			ImGui::TreePop();
		}

		if (ImGui::TreeNodeEx("Vanilla", ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::Text("Darkens lighting relative fog strength.");
			ImGui::SliderFloat("Fog Dimmer Amount", &settings.FogDimmerAmount, 0.0f, 1.0f);

		}

		ImGui::EndTabItem();
	}
}

class NiDirectionalLight : public RE::NiLight
{
public:
	RE::NiPoint3 m_kWorldDir;
	char _pad[0xC];  // NiColor m_kEffectColor?

	// The model direction of the light is (1,0,0). The world direction is
	// the first column of the world rotation matrix.
	inline const RE::NiPoint3& GetWorldDirection() const
	{
		return m_kWorldDir;
	}
};

enum class DistantTreeShaderTechniques
{
	DistantTreeBlock = 0,
	Depth = 1,
};

void DistantTreeLighting::ModifyDistantTree(const RE::BSShader*, const uint32_t descriptor)
{
	if (auto player = RE::PlayerCharacter::GetSingleton())
	{
		if (auto worldSpace = player->GetWorldspace())
		{
			if (lastWorldSpace != worldSpace)
			{
				lastWorldSpace = worldSpace;
				if (auto name = worldSpace->GetFormEditorID()) {
					CSimpleIniA ini;
					ini.SetUnicode();
					auto path = std::format("Data\\Textures\\Terrain\\{}\\Trees\\{}TreeLOD.ini", name, name);
					ini.LoadFile(path.c_str());
					complexAtlasTexture = ini.GetBoolValue("Information", "ComplexAtlasTexture", false);
				} else {
					complexAtlasTexture = false;
				}
			}
		}
	}

	const auto technique = descriptor & 1;
	if (technique != static_cast<uint32_t>(DistantTreeShaderTechniques::Depth)) {

		PerPass perPassData{};
		ZeroMemory(&perPassData, sizeof(perPassData));

		auto& shaderState = RE::BSShaderManager::State::GetSingleton();
		RE::NiTransform& dalcTransform = shaderState.directionalAmbientTransform;

		Util::StoreTransform3x4NoScale(perPassData.DirectionalAmbient, dalcTransform);

		auto accumulator = BSGraphics::BSShaderAccumulator::GetCurrentAccumulator();
		auto& position = accumulator->GetRuntimeData().m_EyePosition;
		auto state = BSGraphics::RendererShadowState::QInstance();

		RE::NiPoint3 eyePosition{};
		if (REL::Module::IsVR()) {
			// find center of eye position
			eyePosition = state->GetVRRuntimeData2().m_PosAdjust.getEye() + state->GetVRRuntimeData2().m_PosAdjust.getEye(1);
			eyePosition /= 2;
		} else
			eyePosition = state->GetRuntimeData2().m_PosAdjust.getEye();
		perPassData.EyePosition.x = position.x - eyePosition.x;
		perPassData.EyePosition.y = position.y - eyePosition.y;
		perPassData.EyePosition.z = position.z - eyePosition.z;

		if (auto sunLight = (NiDirectionalLight*)accumulator->GetRuntimeData().m_ActiveShadowSceneNode->GetRuntimeData().sunLight->light.get()) {
			auto imageSpaceManager = RE::ImageSpaceManager::GetSingleton();

			perPassData.DirLightScale = imageSpaceManager->data.baseData.hdr.sunlightScale * sunLight->GetLightRuntimeData().fade;

			perPassData.DirLightColor.x = sunLight->GetLightRuntimeData().diffuse.red;
			perPassData.DirLightColor.y = sunLight->GetLightRuntimeData().diffuse.green;
			perPassData.DirLightColor.z = sunLight->GetLightRuntimeData().diffuse.blue;

			auto& direction = sunLight->GetWorldDirection();
			perPassData.DirLightDirection.x = direction.x;
			perPassData.DirLightDirection.y = direction.y;
			perPassData.DirLightDirection.z = direction.z;
		}

		perPassData.ComplexAtlasTexture = complexAtlasTexture;

		perPassData.Settings = settings;

		perPass->Update(perPassData);

		auto context = RE::BSGraphics::Renderer::GetSingleton()->GetRuntimeData().context;

		ID3D11Buffer* buffers[2];
		context->VSGetConstantBuffers(2, 1, buffers);  // buffers[0]
		buffers[1] = perPass->CB();
		context->VSSetConstantBuffers(2, ARRAYSIZE(buffers), buffers);
		context->PSSetConstantBuffers(2, ARRAYSIZE(buffers), buffers);

		auto renderer = RE::BSGraphics::Renderer::GetSingleton();
		ID3D11ShaderResourceView* views[1]{};
		views[0] = renderer->GetRuntimeData().renderTargets[RENDER_TARGET_SHADOW_MASK].SRV;
		context->PSSetShaderResources(17, ARRAYSIZE(views), views);
	}
}

void DistantTreeLighting::Draw(const RE::BSShader* shader, const uint32_t descriptor)
{
	switch (shader->shaderType.get()) {
	case RE::BSShader::Type::DistantTree:
		ModifyDistantTree(shader, descriptor);
		break;
	}
}

void DistantTreeLighting::Load(json& o_json)
{
	if (o_json["Tree LOD Lighting"].is_object()) {
		json& distantTreeLighting = o_json["Distant Tree Lighting"];
		if (distantTreeLighting["EnableComplexTreeLOD"].is_boolean())
			settings.EnableComplexTreeLOD = distantTreeLighting["EnableComplexTreeLOD"];
		if (distantTreeLighting["EnableDirLightFix"].is_boolean())
			settings.EnableDirLightFix = distantTreeLighting["EnableDirLightFix"];
		if (distantTreeLighting["SubsurfaceScatteringAmount"].is_number())
			settings.SubsurfaceScatteringAmount = distantTreeLighting["SubsurfaceScatteringAmount"];
		if (distantTreeLighting["FogDimmerAmount"].is_number())
			settings.FogDimmerAmount = distantTreeLighting["FogDimmerAmount"];
	}
	CSimpleIniA ini;
	ini.SetUnicode();
	ini.LoadFile(L"Data\\Shaders\\Features\\TreeLODLighting.ini");
	if (auto value = ini.GetValue("Info", "Version")) {
		enabled = true;
		version = value;
	} else {
		enabled = false;
	}
}

void DistantTreeLighting::Save(json& o_json)
{
	json distantTreeLighting;
	distantTreeLighting["EnableComplexTreeLOD"] = (bool)settings.EnableComplexTreeLOD;
	distantTreeLighting["EnableDirLightFix"] = (bool)settings.EnableDirLightFix;
	distantTreeLighting["SubsurfaceScatteringAmount"] = settings.SubsurfaceScatteringAmount;
	distantTreeLighting["FogDimmerAmount"] = settings.FogDimmerAmount;
	o_json["Tree LOD Lighting"] = distantTreeLighting;
}

void DistantTreeLighting::SetupResources()
{
	perPass = new ConstantBuffer(ConstantBufferDesc<PerPass>());
}

bool DistantTreeLighting::ValidateCache(CSimpleIniA& a_ini)
{
	logger::info("Validating Tree LOD Lighting");

	auto enabledInCache = a_ini.GetBoolValue("Tree LOD Lighting", "Enabled", false);
	if (enabledInCache && !enabled) {
		logger::info("Feature was uninstalled");
		return false;
	}
	if (!enabledInCache && enabled) {
		logger::info("Feature was installed");
		return false;
	}

	if (enabled) {
		auto versionInCache = a_ini.GetValue("Tree LOD Lighting", "Version");
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

void DistantTreeLighting::WriteDiskCacheInfo(CSimpleIniA& a_ini)
{
	a_ini.SetBoolValue("Tree LOD Lighting", "Enabled", enabled);
	a_ini.SetValue("Tree LOD Lighting", "Version", version.c_str());
}
