#include "BSGraphicsTypes.h"

BSGraphics::RendererShadowState* BSGraphics::RendererShadowState::QInstance()
{
	static RendererShadowState* g_RendererShadowState = (RendererShadowState*)RELOCATION_ID(524773, 388819).address();
	return g_RendererShadowState;
}

BSGraphics::Renderer* BSGraphics::Renderer::QInstance()
{
	static BSGraphics::Renderer* g_Renderer = (Renderer*)RELOCATION_ID(524907, 411393).address();  // no added ae address
	return g_Renderer;
}

BSGraphics::CameraStateData* BSGraphics::State::FindCameraDataCache(const RE::NiCamera* Camera, bool UseJitter)
{
	for (uint32_t i = 0; i < GetRuntimeData().kCameraDataCacheA.size(); i++) {
		if (GetRuntimeData().kCameraDataCacheA[i].pReferenceCamera == Camera && GetRuntimeData().kCameraDataCacheA[i].UseJitter == UseJitter)
			return &GetRuntimeData().kCameraDataCacheA[i];
	}

	return nullptr;
}

BSGraphics::SceneState* BSGraphics::SceneState::QInstance()
{
	static SceneState* g_SceneState = (SceneState*)RELOCATION_ID(524907, 411393).address();  // no added ae address

	return nullptr;
}

BSGraphics::ShaderState* BSGraphics::ShaderState::QInstance()
{
	static ShaderState* g_ShaderState = (ShaderState*)RELOCATION_ID(513211, 390951).address();
	return g_ShaderState;
}

BSGraphics::BSShaderAccumulator* BSGraphics::BSShaderAccumulator::GetCurrentAccumulator()
{
	using func_t = decltype(&GetCurrentAccumulator);
	REL::Relocation<func_t> func{ REL::RelocationID(98997, 105651) };
	return func();
}