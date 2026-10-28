#pragma once
#include <deque>

namespace EnvironmentProbe::Scene
{
	/** Releases borrowed world data at the next frame, including when capture is disabled. */
	void BeginFrame();
	void RequestCapture();
	/** Reuses the current world traversal's leaves and unvisited subtrees. Empty means no compatible traversal. */
	RE::BSTArray<RE::NiPointer<RE::NiAVObject>> Roots(RE::NiAVObject* scene);
	/** Copies world material/light work, using the main view's distance/LOD policy regardless of capture origin. */
	bool CopyPasses(RE::BSLightingShaderProperty* property, RE::BSGeometry* geometry, uint32_t flags, std::deque<RE::BSRenderPass>& output);
	/** Remembers material work performed by the normal world accumulator. */
	void RememberPasses(RE::BSLightingShaderProperty* property, RE::BSGeometry* geometry, uint32_t flags, RE::BSShaderAccumulator* accumulator, const RE::BSShaderProperty::RenderPassArray* passes);
	void InstallHooks();
}
