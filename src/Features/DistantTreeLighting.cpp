#include "DistantTreeLighting.h"

#include "State.h"
#include "Util.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	DistantTreeLighting::Settings,
	EnableComplexTreeLOD,
	EnableDirLightFix,
	SubsurfaceScatteringAmount)

void DistantTreeLighting::DrawSettings()
{
	if (ImGui::TreeNodeEx("Complex Tree LOD", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Checkbox("Enable Complex Tree LOD", (bool*)&settings.EnableComplexTreeLOD);
		if (ImGui::IsItemHovered()) {
			ImGui::BeginTooltip();
			ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
			ImGui::Text("Enables advanced lighting simulation on tree LOD.\n");
			ImGui::Text("Requires DynDOLOD.\n");
			ImGui::Text("See https://dyndolod.info/ for more information.");
			ImGui::PopTextWrapPos();
			ImGui::EndTooltip();
		}

		ImGui::Spacing();
		ImGui::Spacing();
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx("Lights", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Checkbox("Enable Directional Light Fix", (bool*)&settings.EnableDirLightFix);
		if (ImGui::IsItemHovered()) {
			ImGui::BeginTooltip();
			ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
			ImGui::Text("Fix for trees not being affected by sunlight scale.");
			ImGui::PopTextWrapPos();
			ImGui::EndTooltip();
		}

		ImGui::Spacing();
		ImGui::Spacing();
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx("Effects", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::SliderFloat("SSS Amount", &settings.SubsurfaceScatteringAmount, 0.0f, 1.0f);
		if (ImGui::IsItemHovered()) {
			ImGui::BeginTooltip();
			ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
			ImGui::Text("Subsurface Scattering (SSS) amount\n");
			ImGui::Text("Soft lighting controls how evenly lit an object is.\n");
			ImGui::Text("Back lighting illuminates the back face of an object.\n");
			ImGui::Text("Combined to model the transport of light through the surface.");
			ImGui::PopTextWrapPos();
			ImGui::EndTooltip();
		}

		ImGui::Spacing();
		ImGui::Spacing();
		ImGui::TreePop();
	}
}

enum class DistantTreeShaderTechniques
{
	DistantTreeBlock = 0,
	Depth = 1,
};

void DistantTreeLighting::ModifyDistantTree(const RE::BSShader*, const uint32_t descriptor)
{
	if (auto player = RE::PlayerCharacter::GetSingleton()) {
		if (auto worldSpace = player->GetWorldspace()) {
			if (lastWorldSpace != worldSpace) {
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

		auto accumulator = RE::BSGraphics::BSShaderAccumulator::GetCurrentAccumulator();

		auto sunLight = skyrim_cast<RE::NiDirectionalLight*>(accumulator->GetRuntimeData().activeShadowSceneNode->GetRuntimeData().sunLight->light.get());
		if (sunLight) {
			auto imageSpaceManager = RE::ImageSpaceManager::GetSingleton();
			auto sunlightScale = !REL::Module::IsVR() ? imageSpaceManager->GetRuntimeData().data.baseData.hdr.sunlightScale :
			                                            imageSpaceManager->GetVRRuntimeData().data.baseData.hdr.sunlightScale;

			perPassData.DirLightScale = sunlightScale * sunLight->GetLightRuntimeData().fade;

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
		views[0] = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kSHADOW_MASK].SRV;
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
	if (o_json[GetName()].is_object())
		settings = o_json[GetName()];

	Feature::Load(o_json);
}

void DistantTreeLighting::Save(json& o_json)
{
	o_json[GetName()] = settings;
}

void DistantTreeLighting::SetupResources()
{
	perPass = new ConstantBuffer(ConstantBufferDesc<PerPass>());
}
