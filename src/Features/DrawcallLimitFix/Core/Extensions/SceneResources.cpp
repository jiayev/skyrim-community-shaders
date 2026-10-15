// The registered scene resources' setup, graph registration, requirements and fit, and their states' lookup (see the header).

// Internal.h first: volk must precede every Vulkan header of the translation unit.
#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Core/Extensions/SceneResources.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Registry.h"

namespace DCLF::Api
{
	std::shared_ptr<org::Buffer> SceneResourcesSetup::StructuredBuffer(std::uint32_t a_elements, std::uint32_t a_stride, const char* a_name, bool a_unorderedAccess) const
	{
		auto created = org::Buffer::CreateUnmaterializedStructuredBuffer(a_elements, a_stride, a_unorderedAccess);
		created->SetName(a_name);
		created->Materialize();
		(void)created->GetSRVInfo(0);
		return created;
	}
}

namespace DCLF::Extensions
{
	SceneResourceStates SetUpSceneResources(rhi::Device a_device, bool a_smallStart)
	{
		const auto registered = RegisteredSceneResources();
		SceneResourceStates states(registered.size());
		const Api::SceneResourcesSetup setup{ a_device, a_smallStart };
		for (std::size_t i = 0; i < registered.size(); ++i)
			states[i] = registered[i].setup(setup);
		return states;
	}

	void RegisterSceneResources(org::RenderGraph& a_graph, const SceneResourceStates& a_states)
	{
		const auto registered = RegisteredSceneResources();
		for (std::size_t i = 0; i < registered.size() && i < a_states.size(); ++i)
			if (a_states[i] && registered[i].registerGraph)
				registered[i].registerGraph({ a_graph, a_states[i] });
	}

	void RequireSceneResources(org::services::GrowableBufferSet::Build& a_build, const SceneTables& a_tables, const SceneResourceStates& a_states)
	{
		const auto registered = RegisteredSceneResources();
		for (std::size_t i = 0; i < registered.size() && i < a_states.size(); ++i)
			if (a_states[i] && registered[i].require)
				registered[i].require({ a_build, a_tables, a_states[i] });
	}

	void FitSceneResources(const org::services::NamedVersions& a_sealed, const SceneResourceStates& a_states, Draws::SceneFit& a_fit)
	{
		const auto registered = RegisteredSceneResources();
		for (std::size_t i = 0; i < registered.size() && i < a_states.size(); ++i)
			if (a_states[i] && registered[i].fit) {
				Api::SceneResourcesFit fit{ a_sealed, a_states[i], a_fit };
				registered[i].fit(fit);
			}
	}

	const std::shared_ptr<void>& SceneResourcesStateOf(const Draws::SceneBuffers& a_scene, Api::SceneResourcesId a_id)
	{
		static const std::shared_ptr<void> kNone;
		return a_id.index < a_scene.sceneResources.size() ? a_scene.sceneResources[a_id.index] : kNone;
	}
}
