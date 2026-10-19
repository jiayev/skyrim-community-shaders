// The registered scene resources' setup, graph registration, requirements, fit, posted inputs, latched buffers and sealed sets, and
// their states' lookup (see the header).

// Internal.h first: volk must precede every Vulkan header of the translation unit.
#include "Features/DrawcallLimitFix/Core/Render/Internal.h"

#include "Features/DrawcallLimitFix/Core/Extensions/Registry.h"
#include "Features/DrawcallLimitFix/Core/Extensions/SceneResources.h"
#include "Features/DrawcallLimitFix/Core/Extensions/VertexStreams.h"

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
		// The vertex stream's first (SceneResourcesOrder): without its buffer the scene's tables are not made, and nothing else is set up.
		for (const std::uint32_t i : SceneResourcesOrder()) {
			states[i] = registered[i].setup(setup);
			if (!states[i] && VertexStreamResources(i))
				break;
		}
		return states;
	}

	void RegisterSceneResources(org::RenderGraph& a_graph, const SceneResourceStates& a_states)
	{
		const auto registered = RegisteredSceneResources();
		for (const std::uint32_t i : SceneResourcesOrder())
			if (i < a_states.size() && a_states[i] && registered[i].registerGraph)
				registered[i].registerGraph({ a_graph, a_states[i] });
	}

	void RequireSceneResources(org::services::GrowableBufferSet::Build& a_build, const SceneTables& a_tables, const SceneResourceStates& a_states,
		const SceneResourceInputs& a_inputs)
	{
		const auto registered = RegisteredSceneResources();
		for (const std::uint32_t i : SceneResourcesOrder())
			if (i < a_states.size() && a_states[i] && registered[i].require)
				registered[i].require({ a_build, a_tables, a_states[i], std::span(a_inputs).subspan(registered[i].firstInput, registered[i].inputWords), static_cast<std::uint32_t>(a_tables.objects.size()) });
	}

	void PostSceneResourceInputs(SceneResourceInputs& a_out)
	{
		a_out.fill(0);
		for (const auto& registered : RegisteredSceneResources())
			if (registered.postInputs)
				registered.postInputs(std::span(a_out).subspan(registered.firstInput, registered.inputWords));
	}

	std::span<const std::uint32_t> SceneResourceInputsOf(const SceneResourceInputs& a_inputs, Api::SceneResourcesId a_id)
	{
		const auto registered = RegisteredSceneResources();
		if (a_id.index >= registered.size())
			return {};
		return std::span(a_inputs).subspan(registered[a_id.index].firstInput, registered[a_id.index].inputWords);
	}

	void LatchSceneResources(const SceneResourceStates& a_states,
		void (*a_sink)(void*, const std::shared_ptr<org::Buffer>&, const std::shared_ptr<org::VersionedBuffer>&, std::size_t), void* a_context)
	{
		const auto registered = RegisteredSceneResources();
		for (const std::uint32_t i : SceneResourcesOrder())
			if (i < a_states.size() && a_states[i] && registered[i].latched) {
				Api::SceneResourcesLatch latch{ a_states[i], a_sink, a_context };
				registered[i].latched(latch);
			}
	}

	void FitSceneResources(const org::services::NamedVersions& a_sealed, const SceneResourceStates& a_states, Draws::SceneFit& a_fit)
	{
		const auto registered = RegisteredSceneResources();
		for (const std::uint32_t i : SceneResourcesOrder())
			if (i < a_states.size() && a_states[i] && registered[i].fit) {
				Api::SceneResourcesFit fit{ a_sealed, a_states[i], a_fit };
				registered[i].fit(fit);
			}
	}

	void SealSceneResources(const org::services::NamedVersions& a_sealed, const SceneResourceStates& a_states)
	{
		const auto registered = RegisteredSceneResources();
		for (const std::uint32_t i : SceneResourcesOrder())
			if (i < a_states.size() && a_states[i] && registered[i].sealed)
				registered[i].sealed({ a_sealed, a_states[i] });
	}

	const std::shared_ptr<void>& SceneResourcesStateOf(const Draws::SceneBuffers& a_scene, Api::SceneResourcesId a_id)
	{
		static const std::shared_ptr<void> kNone;
		return a_id.index < a_scene.sceneResources.size() ? a_scene.sceneResources[a_id.index] : kNone;
	}
}
