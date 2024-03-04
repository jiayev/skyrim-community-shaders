#include "TerrainBlending.h"

#include "Bindings.h"
#include "State.h"

void TerrainBlending::DrawSettings()
{
	ImGui::Checkbox("Enable", &enableBlending);
}

bool TerrainBlending::ValidBlendingPass(RE::BSRenderPass* a_pass)
{
	auto eyePosition = !REL::Module::IsVR() ? RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData().posAdjust.getEye(0) : RE::BSGraphics::RendererShadowState::GetSingleton()->GetVRRuntimeData().posAdjust.getEye(0);
	auto objectPosition = a_pass->geometry->world.translate;
	objectPosition -= eyePosition;
	auto objectDistance = objectPosition.Length();

	if (objectDistance < optimisationDistance) {
		auto accumulator = RE::BSGraphics::BSShaderAccumulator::GetCurrentAccumulator();

		auto reflections = (!REL::Module::IsVR() ?
								   RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData().cubeMapRenderTarget :
								   RE::BSGraphics::RendererShadowState::GetSingleton()->GetVRRuntimeData().cubeMapRenderTarget) == RE::RENDER_TARGETS_CUBEMAP::kREFLECTIONS;

		return !reflections && accumulator->GetRuntimeData().activeShadowSceneNode == RE::BSShaderManager::State::GetSingleton().shadowSceneNode[0];
	}
	return false;
}

void TerrainBlending::Draw(const RE::BSShader*, const uint32_t)
{
}

void TerrainBlending::SetupResources()
{
}

void TerrainBlending::Reset()
{
}

void TerrainBlending::Load(json& o_json)
{
	Feature::Load(o_json);
}

void TerrainBlending::Save(json&)
{
}

void TerrainBlending::RestoreDefaultSettings()
{
}

void TerrainBlending::PostPostLoad()
{
	Hooks::Install();
}

void TerrainBlending::SetupGeometry(RE::BSRenderPass* a_pass)
{
	if (!enableBlending) {
		Bindings::GetSingleton()->SetOverwriteTerrainMode(false);
		Bindings::GetSingleton()->SetOverwriteTerrainMaskingMode(Bindings::TerrainMaskMode::kNone);
		return;
	}

	bool validPass = TerrainBlending::ValidBlendingPass(a_pass);

	if (a_pass->shaderProperty->flags.all(RE::BSShaderProperty::EShaderPropertyFlag::kMultiTextureLandscape)) {
		if (validPass) {
			Bindings::GetSingleton()->SetOverwriteTerrainMode(true);
			Bindings::GetSingleton()->SetOverwriteTerrainMaskingMode(Bindings::TerrainMaskMode::kRead);

			auto context = RE::BSGraphics::Renderer::GetSingleton()->GetRuntimeData().context;
			auto view = Bindings::GetSingleton()->terrainBlendingMask ? Bindings::GetSingleton()->terrainBlendingMask->srv.get() : nullptr;
			if (view)
				context->PSSetShaderResources(35, 1, &view);
		} else {
			Bindings::GetSingleton()->SetOverwriteTerrainMode(false);
			Bindings::GetSingleton()->SetOverwriteTerrainMaskingMode(Bindings::TerrainMaskMode::kNone);
		}
	} else {
		Bindings::GetSingleton()->SetOverwriteTerrainMode(false);
		bool staticReference = false;
		if (validPass) {
			if (auto ref = a_pass->geometry->GetUserData()) {
				if (auto base = ref->GetBaseObject()) {
					if (base->As<RE::TESObjectSTAT>()) {
						staticReference = true;
					}
				}
			}
		}
		Bindings::GetSingleton()->SetOverwriteTerrainMaskingMode(validPass && !staticReference ?
																	 (!REL::Module::IsVR() ?
																			 Bindings::TerrainMaskMode::kWrite :
																			 Bindings::TerrainMaskMode::kRead) :  // Fix VR artifact where static objects would appear shifted in each eye
																	 Bindings::TerrainMaskMode::kNone);
	}
}