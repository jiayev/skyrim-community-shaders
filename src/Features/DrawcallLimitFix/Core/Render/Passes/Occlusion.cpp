// The two-phase occlusion tail's compute: the HZB rebuild and the visibility events once both phases' verdicts are final.

#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Core/Render/Passes/PassesInternal.h"

namespace DCLF::Draws
{
	struct HzbBindings
	{
		org::DeclaredViewToken depth;
		org::DeclaredViewToken counter;
		std::vector<org::DeclaredViewToken> hzbMips;
	};

	struct HzbFrame
	{
		std::shared_ptr<const ComputeProgram> program;
		// One single-pass downsample per kHzbDispatchMips levels, the first from scene depth, each next from the previous one's
		// last level. Up to 4096 texels a side takes one.
		struct Dispatch
		{
			HzbConstants constants{};
			std::uint32_t groupsX = 0, groupsY = 0;
		};
		std::vector<Dispatch> dispatches;
	};

	/**
	 * @brief Builds the HZB in the ZPrepass segment after DCLF's depth draws, so it holds the native occluders plus DCLF's. On
	 * AE that is before the first-person model's depth and the rooms' stencil draws: fewer occluders, never more. The chain
	 * is one single-pass downsample (HzbCS.hlsl, FidelityFX SPD): per-level dispatches would need a full barrier each.
	 */
	class HzbPass final : public org::TypedRenderGraphPass<HzbPass, HzbFrame, HzbBindings>
	{
	public:
		HzbPass(std::shared_ptr<Resources> a_resources, RenderGraphRuntime::Segment a_segment) :
			resources(std::move(a_resources)), segment(a_segment) {}

		HzbBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			HzbBindings bindings{};
			bindings.depth = a_builder.ShaderResource(resources->nativeDepth).View();
			bindings.counter = a_builder.UnorderedAccess(resources->hzbCounter).View();
			bindings.hzbMips.reserve(resources->hzbMips);
			for (std::uint32_t mip = 0; mip < resources->hzbMips; ++mip)
				bindings.hzbMips.push_back(a_builder.UnorderedAccess(resources->hzb, org::UavView{ UINT32_MAX, mip }).View());
			return bindings;
		}

		void InvocationRevision([[maybe_unused]] const org::PassPrepareContext& a_preparation, std::vector<std::uint64_t>& a_out) const
		{
			const auto now = segment;
			const auto frame = CurrentFrame(a_preparation, now);
			a_out.push_back(frame ? frame->generation : 0);
			a_out.push_back(static_cast<std::uint64_t>(now));
		}

		HzbFrame Prepare(const HzbBindings& a_bindings, [[maybe_unused]] const org::PassPrepareContext& a_preparation) const
		{
			HzbFrame prepared{};
			// Only in the Z-prepass segment, where the depth is the world's final depth.
			const auto now = segment;
			if (now != RenderGraphRuntime::Segment::ZPrepass)
				return prepared;
			if (!resources->hzb || !resources->hzbProgram || !resources->hzbCounter)
				return prepared;
			// The depth commit zeroed the group counter (hzbCounterZeroed).
			const auto frame = CurrentFrame(a_preparation, now);
			if (!frame)
				return prepared;
			const std::uint32_t renderWidth = frame->width ? frame->width : resources->width;
			const std::uint32_t renderHeight = frame->height ? frame->height : resources->height;
			prepared.program = resources->hzbProgram;
			const std::uint32_t depthIndex = CaptureViewIndex(a_preparation, a_bindings.depth);
			const std::uint32_t counterIndex = CaptureViewIndex(a_preparation, a_bindings.counter);
			std::vector<std::uint32_t> levelIndices(resources->hzbMips);
			for (std::uint32_t mip = 0; mip < resources->hzbMips; ++mip)
				levelIndices[mip] = CaptureViewIndex(a_preparation, a_bindings.hzbMips[mip]);
			const auto levelSize = [&](std::uint32_t a_mip, std::uint32_t a_axis) {
				return std::max(1u, (a_axis == 0 ? resources->hzbWidth : resources->hzbHeight) >> a_mip);
			};
			for (std::uint32_t level = 0; level < resources->hzbMips;) {
				HzbFrame::Dispatch dispatch{};
				auto& constants = dispatch.constants;
				constants.counterIndex = counterIndex;
				if (level == 0) {
					// Mip 0 is half the padded domain; only the rendered area holds depth (the image can exceed the viewport).
					constants.fromDepth = 1;
					constants.sourceIndex = depthIndex;
					constants.domainSize[0] = resources->hzbWidth * 2;
					constants.domainSize[1] = resources->hzbHeight * 2;
					constants.validSize[0] = std::min(renderWidth, constants.domainSize[0]);
					constants.validSize[1] = std::min(renderHeight, constants.domainSize[1]);
				} else {
					constants.fromDepth = 0;
					constants.sourceIndex = levelIndices[level - 1];
					constants.domainSize[0] = constants.validSize[0] = levelSize(level - 1, 0);
					constants.domainSize[1] = constants.validSize[1] = levelSize(level - 1, 1);
				}
				constants.targetSize[0] = levelSize(level, 0);
				constants.targetSize[1] = levelSize(level, 1);
				// A group reduces its tile through six levels; the last group continues past the sixth only when that level fits
				// one tile (a domain of at most kHzbTile tiles a side).
				const std::uint32_t domain = std::max(constants.domainSize[0], constants.domainSize[1]);
				const std::uint32_t reach = domain / kHzbTile <= kHzbTile ? kHzbDispatchMips : 6u;
				constants.mips = std::min(resources->hzbMips - level, reach);
				for (std::uint32_t i = 0; i < constants.mips; ++i)
					constants.targetIndices[i] = levelIndices[level + i];
				dispatch.groupsX = (constants.domainSize[0] + kHzbTile - 1) / kHzbTile;
				dispatch.groupsY = (constants.domainSize[1] + kHzbTile - 1) / kHzbTile;
				constants.workGroups = dispatch.groupsX * dispatch.groupsY;
				level += constants.mips;
				prepared.dispatches.push_back(dispatch);
			}
			return prepared;
		}

		static void Record(const HzbBindings&, const HzbFrame& a_frame, org::PassRecordContext& a_recording)
		{
			if (!a_frame.program || a_frame.dispatches.empty())
				return;
			auto& commands = a_recording.Commands();
			commands.BindLayout(a_frame.program->layout->GetHandle());
			commands.BindPipeline(a_frame.program->pipeline->GetHandle());
			for (std::size_t i = 0; i < a_frame.dispatches.size(); ++i) {
				const auto& dispatch = a_frame.dispatches[i];
				if (i != 0) {
					// The previous dispatch's last level must be complete before this one reads it.
					const rhi::GlobalBarrier global = rhi::FullMemoryBarrier();
					const rhi::BarrierBatch batch{ {}, {}, { &global, 1 } };
					commands.Barriers(batch);
				}
				commands.PushConstants(rhi::ShaderStage::Compute, 0, 0, 0, kHzbConstantWords, reinterpret_cast<const std::uint32_t*>(&dispatch.constants));
				commands.Dispatch(dispatch.groupsX, dispatch.groupsY, 1);
			}
		}

	private:
		std::shared_ptr<Resources> resources;
		RenderGraphRuntime::Segment segment;
	};

	std::shared_ptr<org::RenderPass> MakeHzbPass(std::shared_ptr<Resources> a_resources, RenderGraphRuntime::Segment a_segment)
	{
		return std::make_shared<HzbPass>(std::move(a_resources), a_segment);
	}

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
			bindings.roots = a_builder.ShaderResource(*scene.fadeRoots).View();
			bindings.states = a_builder.ShaderResource(*scene.fadeStates).View();
			bindings.frame = a_builder.ShaderResource(scene.fadeFrameBuffer).View();
			bindings.frustum = a_builder.ShaderResource(*resources->frustum).View();
			bindings.rootReported = a_builder.UnorderedAccess(*scene.visibilityRootReported).View();
			bindings.objectReported = a_builder.UnorderedAccess(*scene.visibilityObjectReported).View();
			bindings.events = a_builder.UnorderedAccess(*scene.visibilityEvents).View();
			return bindings;
		}

		// The buffers' versions and the depth shape; frame, counts and camera are in the uploaded fade frame row.
		void InvocationRevision([[maybe_unused]] const org::PassPrepareContext& a_preparation, std::vector<std::uint64_t>& a_out) const
		{
			const auto frame = CurrentFrame(a_preparation, RenderGraphRuntime::Segment::ZPrepass);
			const auto& main = *resources;
			const auto& scene = *main.scene;
			for (const auto* buffer : { &scene.fadeRoots, &scene.fadeStates, &main.frustum, &scene.visibilityRootReported, &scene.visibilityObjectReported,
					 &scene.visibilityEvents })
				a_out.push_back(NamedSerial(a_preparation, *buffer));
			a_out.push_back(frame ? frame->generation : 0);
		}

		VisibilityEventsPrepared Prepare(const VisibilityEventsBindings& a_bindings, [[maybe_unused]] const org::PassPrepareContext& a_preparation) const
		{
			VisibilityEventsPrepared prepared{};
			const auto& scene = *resources->scene;
			const auto events = static_cast<std::uint32_t>(NamedUnitsOf(a_preparation, scene.visibilityEvents));
			// Only with a staging set that holds the whole list, so the copy misses nothing.
			if (!scene.visibilityEventsProgram || !scene.visibilityReadback || !events || !scene.visibilityReadback->SetFor(events))
				return prepared;
			prepared.program = scene.visibilityEventsProgram;
			auto& constants = prepared.constants;
			constants.rootsIndex = CaptureViewIndex(a_preparation, a_bindings.roots);
			constants.statesIndex = CaptureViewIndex(a_preparation, a_bindings.states);
			constants.frameIndex = CaptureViewIndex(a_preparation, a_bindings.frame);
			constants.frustumIndex = CaptureViewIndex(a_preparation, a_bindings.frustum);
			constants.rootReportedIndex = CaptureViewIndex(a_preparation, a_bindings.rootReported);
			constants.objectReportedIndex = CaptureViewIndex(a_preparation, a_bindings.objectReported);
			constants.eventsIndex = CaptureViewIndex(a_preparation, a_bindings.events);
			constants.eventCapacity = events;
			// Every root slot, then every object slot while the segment builds (without depth phase 1 no stamp is this frame's): what the
			// revision's versions of the buffers read and written by slot hold.
			constants.rootSlots = static_cast<std::uint32_t>(std::min({ NamedUnitsOf(a_preparation, scene.fadeRoots), NamedUnitsOf(a_preparation, scene.fadeStates),
				NamedUnitsOf(a_preparation, scene.visibilityRootReported) }));
			constants.objectSlots = BuildsDraws(a_preparation, *resources, RenderGraphRuntime::Segment::ZPrepass) ?
			                            static_cast<std::uint32_t>(std::min(NamedUnitsOf(a_preparation, resources->frustum), NamedUnitsOf(a_preparation, scene.visibilityObjectReported))) :
			                            0u;
			prepared.groups = (constants.rootSlots + constants.objectSlots + kVisibilityEventsGroup - 1) / kVisibilityEventsGroup;
			return prepared;
		}

		static void Record(const VisibilityEventsBindings&, const VisibilityEventsPrepared& a_frame, org::PassRecordContext& a_recording)
		{
			if (!a_frame.program || !a_frame.groups)
				return;
			auto& commands = a_recording.Commands();
			commands.BindLayout(a_frame.program->layout->GetHandle());
			commands.BindPipeline(a_frame.program->pipeline->GetHandle());
			commands.PushConstants(rhi::ShaderStage::Compute, 0, 0, 0, kVisibilityEventsConstantWords, reinterpret_cast<const std::uint32_t*>(&a_frame.constants));
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
