// The visibility events once both phases' verdicts are final, from the roots' InView and phase 1's frustum stamps: what the stood-in
// roots' write-back reads (Handoff's, not the occlusion's).

#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Core/Render/Passes/PassesInternal.h"

namespace DCLF::Draws
{
	struct VisibilityEventsBindings
	{
		org::DeclaredViewToken roots, states, frame, frustum, rootReported, objectReported, events;
	};

	struct VisibilityEventsPrepared
	{
		std::shared_ptr<const ComputeProgram> program;
		VisibilityEventsConstants constants{};
		std::uint32_t groups = 0;
	};

	/**
	 * @brief The main camera's visibility as change events (VisibilityEventsCS.hlsl) at the depth segment's end, from the roots'
	 * InView and the objects' frustum stamps. The normal path writes the stood-in roots' kAccumulated from them.
	 */
	class VisibilityEventsPass final : public org::TypedRenderGraphPass<VisibilityEventsPass, VisibilityEventsPrepared, VisibilityEventsBindings>
	{
	public:
		explicit VisibilityEventsPass(std::shared_ptr<Resources> a_resources) :
			resources(std::move(a_resources)) {}

		VisibilityEventsBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			const auto& scene = *resources->scene;
			VisibilityEventsBindings bindings{};
			// The root states' rows, the states their step wrote this frame and its frame row (the cull bindings RootRows, RootStepStates,
			// RootFrame: the pass is added only with the rows).
			bindings.roots = a_builder.ShaderResource(*Extensions::CullBindingBuffer(Api::CullBindingSlot::RootRows, scene)).View();
			bindings.states = a_builder.ShaderResource(*Extensions::CullBindingBuffer(Api::CullBindingSlot::RootStepStates, scene)).View();
			bindings.frame = a_builder.ShaderResource(*Extensions::SelectCullBinding(Api::CullBindingSlot::RootFrame, scene).buffer).View();
			bindings.frustum = a_builder.ShaderResource(*resources->frustum).View();
			bindings.rootReported = a_builder.UnorderedAccess(*scene.visibilityRootReported).View();
			bindings.objectReported = a_builder.UnorderedAccess(*scene.visibilityObjectReported).View();
			bindings.events = a_builder.UnorderedAccess(*scene.visibilityEvents).View();
			return bindings;
		}

		// The buffers' versions and the depth shape are the revision's (the ticket's host data); frame, counts and camera are in the
		// uploaded fade frame row.
		void Revision(std::vector<std::uint64_t>&) const {}

		VisibilityEventsPrepared Prepare(const VisibilityEventsBindings&, const org::PassPrepareContext& a_preparation) const
		{
			VisibilityEventsPrepared prepared{};
			const auto& scene = *resources->scene;
			const auto events = static_cast<std::uint32_t>(NamedUnitsOf(a_preparation, scene.visibilityEvents));
			// Only with a staging set that holds the whole list, so the copy misses nothing.
			if (!scene.visibilityEventsProgram || !scene.visibilityReadback || !events || !scene.visibilityReadback->SetFor(events))
				return prepared;
			prepared.program = scene.visibilityEventsProgram;
			auto& constants = prepared.constants;
			constants.eventCapacity = events;
			// Every root slot, then every object slot while the segment builds (without depth phase 1 no stamp is this frame's): what the
			// revision's versions of the buffers read and written by slot hold.
			constants.rootSlots = static_cast<std::uint32_t>(std::min({ NamedUnitsOf(a_preparation, Extensions::CullBindingBuffer(Api::CullBindingSlot::RootRows, scene)),
				NamedUnitsOf(a_preparation, Extensions::CullBindingBuffer(Api::CullBindingSlot::RootStepStates, scene)), NamedUnitsOf(a_preparation, scene.visibilityRootReported) }));
			constants.objectSlots = BuildsDraws(a_preparation, *resources, RenderGraphRuntime::Segment::ZPrepass) ?
			                            static_cast<std::uint32_t>(std::min(NamedUnitsOf(a_preparation, resources->frustum), NamedUnitsOf(a_preparation, scene.visibilityObjectReported))) :
			                            0u;
			prepared.groups = (constants.rootSlots + constants.objectSlots + kVisibilityEventsGroup - 1) / kVisibilityEventsGroup;
			return prepared;
		}

		static void Record(const VisibilityEventsBindings& a_bindings, const VisibilityEventsPrepared& a_frame, org::PassRecordContext& a_recording)
		{
			if (!a_frame.program || !a_frame.groups)
				return;
			auto constants = a_frame.constants;
			constants.rootsIndex = a_recording.Index(a_bindings.roots);
			constants.statesIndex = a_recording.Index(a_bindings.states);
			constants.frameIndex = a_recording.Index(a_bindings.frame);
			constants.frustumIndex = a_recording.Index(a_bindings.frustum);
			constants.rootReportedIndex = a_recording.Index(a_bindings.rootReported);
			constants.objectReportedIndex = a_recording.Index(a_bindings.objectReported);
			constants.eventsIndex = a_recording.Index(a_bindings.events);
			auto& commands = a_recording.Commands();
			commands.BindLayout(a_frame.program->layout->GetHandle());
			commands.BindPipeline(a_frame.program->Pipeline());
			commands.PushConstants(rhi::ShaderStage::Compute, 0, 0, 0, kVisibilityEventsConstantWords, reinterpret_cast<const std::uint32_t*>(&constants));
			commands.Dispatch(a_frame.groups, 1, 1);
		}

	private:
		std::shared_ptr<Resources> resources;
	};

	std::shared_ptr<org::RenderPass> MakeVisibilityEventsPass(std::shared_ptr<Resources> a_resources)
	{
		return std::make_shared<VisibilityEventsPass>(std::move(a_resources));
	}
}
