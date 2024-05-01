#include "TerrainBlending.h"

#include "Deferred.h"
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

void TerrainBlending::SetupGeometry(RE::BSRenderPass*)
{
	//if (!enableBlending) {
	//	Deferred::GetSingleton()->SetOverwriteTerrainMode(false);
	//	Deferred::GetSingleton()->SetOverwriteTerrainMaskingMode(Deferred::TerrainMaskMode::kNone);
	//	return;
	//}

	//bool validPass = TerrainBlending::ValidBlendingPass(a_pass);

	//if (a_pass->shaderProperty->flags.all(RE::BSShaderProperty::EShaderPropertyFlag::kMultiTextureLandscape)) {
	//	if (validPass) {
	//		Deferred::GetSingleton()->SetOverwriteTerrainMode(true);
	//		Deferred::GetSingleton()->SetOverwriteTerrainMaskingMode(Deferred::TerrainMaskMode::kRead);

	//		auto context = RE::BSGraphics::Renderer::GetSingleton()->GetRuntimeData().context;
	//		auto view = Deferred::GetSingleton()->terrainBlendingMask ? Deferred::GetSingleton()->terrainBlendingMask->srv.get() : nullptr;
	//		if (view)
	//			context->PSSetShaderResources(35, 1, &view);
	//	} else {
	//		Deferred::GetSingleton()->SetOverwriteTerrainMode(false);
	//		Deferred::GetSingleton()->SetOverwriteTerrainMaskingMode(Deferred::TerrainMaskMode::kNone);
	//	}
	//} else {
	//	Deferred::GetSingleton()->SetOverwriteTerrainMode(false);
	//	bool staticReference = false;
	//	if (validPass) {
	//		if (auto ref = a_pass->geometry->GetUserData()) {
	//			if (auto base = ref->GetBaseObject()) {
	//				if (base->As<RE::TESObjectSTAT>()) {
	//					staticReference = true;
	//				}
	//			}
	//		}
	//	}
	//	Deferred::GetSingleton()->SetOverwriteTerrainMaskingMode(validPass && !staticReference ?
	//																 (!REL::Module::IsVR() ?
	//																		 Deferred::TerrainMaskMode::kWrite :
	//																		 Deferred::TerrainMaskMode::kRead) :  // Fix VR artifact where static objects would appear shifted in each eye
	//																 Deferred::TerrainMaskMode::kNone);
	//}
}