// Tree LOD's passes: its cull (TreeLodCullCS) in the depth segment's two phases, right before each phase's depth draw, and its list
// counts' readback once both phases are done, from the tables (the scene resources) and the depth segment's latch.

// Main/Resources.h first: volk must precede every Vulkan header of the translation unit.
#include "Features/DrawcallLimitFix/Core/Render/Main/Resources.h"
#include "Features/DrawcallLimitFix/Features/TreeLod/TreeLodTables.h"
#include "Features/DrawcallLimitFix/Core/Extensions/PassContext.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/Helpers.h"
#include "Features/DrawcallLimitFix/Core/Render/Revision/Shapes.h"

#include "RenderGraph/ComputeProgram.h"
#include "RenderGraph/RenderGraphRuntime.h"
#include <Render/LatchBlock.h>
#include <RenderPasses/Base/TypedRenderGraphPass.h>
#include <rhi.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

namespace DCLF::Draws
{
	// In DCLF::Draws, where they were defined before the feature moved: ORG's recipe telemetry names a pass by its type (typeid).
	struct TreeLodCullBindings
	{
		org::DeclaredViewToken shapes, instances, draw, visible, hzb;
	};

	struct TreeLodCullPrepared
	{
		std::shared_ptr<const ComputeProgram> program;
		Features::TreeLod::TreeLodCullConstants constants{};
		std::shared_ptr<const org::LatchBlock> latch;
		std::uint32_t groups = 0;
	};

	/**
	 * @brief Tree LOD's cull (TreeLodCullCS.hlsl) in the depth segment's two phases, like BuildDraws: phase 1 tests every instance
	 * against the frustum and last frame's HZB; phase 2 retests phase 1's occluded ones against the HZB rebuilt this frame. Both
	 * append to the list the passes draw.
	 */
	class TreeLodCullPass final : public org::TypedRenderGraphPass<TreeLodCullPass, TreeLodCullPrepared, TreeLodCullBindings>
	{
	public:
		// a_segment: its epoch's (the depth epoch's: the PassContext it was made with).
		TreeLodCullPass(std::shared_ptr<Resources> a_resources, std::shared_ptr<const Features::TreeLod::TreeLodTables> a_tables, std::uint32_t a_phase,
			RenderGraphRuntime::Segment a_segment) :
			resources(std::move(a_resources)), tables(std::move(a_tables)), phase(a_phase), segment(a_segment) {}

		TreeLodCullBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			TreeLodCullBindings bindings{};
			bindings.shapes = a_builder.ShaderResource(*tables->shapes).View();
			bindings.instances = a_builder.ShaderResource(*tables->instances).View();
			bindings.draw = a_builder.ShaderResource(tables->draw).View();
			bindings.visible = a_builder.UnorderedAccess(*tables->visible).View();
			// Which HZB a phase sees depends on its place around the HZB build (GatherStructuralPasses).
			if (resources->hzb)
				bindings.hzb = a_builder.ShaderResource(resources->hzb).View();
			return bindings;
		}

		// The recording depends on the tables' versions (a growth changes views and dispatch) and the depth segment's shape.
		void InvocationRevision([[maybe_unused]] const org::PassPrepareContext& a_preparation, std::vector<std::uint64_t>& a_out) const
		{
			const auto frame = CurrentFrame(a_preparation, segment);
			for (const auto* table : { &tables->shapes, &tables->instances, &tables->visible })
				a_out.push_back(NamedSerial(a_preparation, *table));
			a_out.push_back(frame ? frame->generation : 0);
		}

		TreeLodCullPrepared Prepare(const TreeLodCullBindings& a_bindings, [[maybe_unused]] const org::PassPrepareContext& a_preparation) const
		{
			TreeLodCullPrepared prepared{};
			const auto frame = CurrentFrame(a_preparation, segment);
			// The shape slots every table the cull reads holds (the revision's versions).
			const std::uint64_t shapes = std::min({ NamedUnitsOf(a_preparation, tables->shapes), NamedUnitsOf(a_preparation, tables->instances),
				NamedUnitsOf(a_preparation, tables->visible) });
			if (!tables->cull || !shapes || !frame || !frame->latch)
				return prepared;
			prepared.program = tables->cull;
			prepared.latch = frame->latch;
			auto& constants = prepared.constants;
			constants.shapesIndex = CaptureViewIndex(a_preparation, a_bindings.shapes);
			constants.instancesIndex = CaptureViewIndex(a_preparation, a_bindings.instances);
			constants.drawIndex = CaptureViewIndex(a_preparation, a_bindings.draw);
			constants.visibleIndex = CaptureViewIndex(a_preparation, a_bindings.visible);
			constants.latchIndex = frame->latch->SrvIndex();
			constants.phase = phase;
			const std::uint64_t records = shapes * TreeInstances::kMaxGroupInstances;
			constants.retestOffset = static_cast<std::uint32_t>(sizeof(TreeInstances::VisibleHeader) + records * sizeof(std::uint32_t));
			if (resources->hzb && frame->cullMode >= 2 && frame->width && frame->height) {
				constants.hzbIndex = CaptureViewIndex(a_preparation, a_bindings.hzb);
				constants.hzbSizePacked = (resources->hzbWidth & 0xFFFF) | (resources->hzbHeight << 16);
				constants.hzbMips = resources->hzbMips;
			}
			// Dispatched for full capacity; the shader stops at the real counts.
			prepared.groups = static_cast<std::uint32_t>((records + Features::TreeLod::kTreeLodCullGroup - 1) / Features::TreeLod::kTreeLodCullGroup);
			return prepared;
		}

		static void Record(const TreeLodCullBindings&, const TreeLodCullPrepared& a_frame, org::PassRecordContext& a_recording)
		{
			if (!a_frame.program || !a_frame.groups || !a_frame.latch)
				return;
			auto constants = a_frame.constants;
			// The depth segment's BuildDrawsLatch in this frame slot.
			constants.latchOffset = static_cast<std::uint32_t>(a_frame.latch->Offset(a_recording.FrameSlot()));
			auto& commands = a_recording.Commands();
			commands.BindLayout(a_frame.program->layout->GetHandle());
			commands.BindPipeline(a_frame.program->Pipeline());
			commands.PushConstants(rhi::ShaderStage::Compute, 0, 0, 0, Features::TreeLod::kTreeLodCullConstantWords, reinterpret_cast<const std::uint32_t*>(&constants));
			commands.Dispatch(a_frame.groups, 1, 1);
		}

	private:
		std::shared_ptr<Resources> resources;
		std::shared_ptr<const Features::TreeLod::TreeLodTables> tables;
		std::uint32_t phase = 1;
		RenderGraphRuntime::Segment segment;
	};

	struct TreeLodReadbackBindings
	{
		org::ResourceBindingToken visible;
	};

	struct TreeLodReadbackPrepared
	{
		std::shared_ptr<Features::TreeLod::TreeLodCounts> counts;
	};

	/** @brief Copies tree LOD's list header to the frame slot's host buffer after phase 2, first summing what the slot held. */
	class TreeLodReadbackPass final : public org::TypedRenderGraphPass<TreeLodReadbackPass, TreeLodReadbackPrepared, TreeLodReadbackBindings>
	{
	public:
		explicit TreeLodReadbackPass(std::shared_ptr<const Features::TreeLod::TreeLodTables> a_tables) :
			tables(std::move(a_tables)) {}

		TreeLodReadbackBindings Declare(org::PassBuilder& a_builder)
		{
			return { a_builder.CopySource(*tables->visible) };
		}

		void InvocationRevision([[maybe_unused]] const org::PassPrepareContext& a_preparation, std::vector<std::uint64_t>& a_out) const
		{
			a_out.push_back(NamedSerial(a_preparation, tables->visible));
		}

		TreeLodReadbackPrepared Prepare(const TreeLodReadbackBindings&, [[maybe_unused]] const org::PassPrepareContext& a_preparation) const
		{
			return { tables->counts };
		}

		static void Record(const TreeLodReadbackBindings& a_bindings, const TreeLodReadbackPrepared& a_frame, org::PassRecordContext& a_recording)
		{
			if (!a_frame.counts)
				return;
			auto& counts = *a_frame.counts;
			const std::uint32_t slot = a_recording.FrameSlot();
			if (slot >= counts.readback.size())
				return;
			if (counts.filled[slot]) {
				auto resource = counts.readback[slot]->GetAPIResource();
				void* mapped = nullptr;
				resource.Map(&mapped);
				if (mapped) {
					TreeInstances::VisibleHeader header;
					std::memcpy(&header, mapped, sizeof(header));
					counts.frames.fetch_add(1, std::memory_order_relaxed);
					counts.phaseOne.fetch_add(header.phaseOne[1], std::memory_order_relaxed);
					counts.retests.fetch_add(header.retests, std::memory_order_relaxed);
					counts.phaseTwo.fetch_add(header.phaseTwo[1], std::memory_order_relaxed);
					resource.Unmap(0, 0);
				}
			}
			a_recording.Commands().CopyBufferRegion(counts.readback[slot]->GetAPIResource().GetHandle(), 0, a_recording.Resolve(a_bindings.visible).GetHandle(), 0,
				sizeof(TreeInstances::VisibleHeader));
			counts.filled[slot] = 1;
		}

	private:
		std::shared_ptr<const Features::TreeLod::TreeLodTables> tables;
	};
}

namespace DCLF::Features::TreeLod
{
	namespace
	{
		// The passes' states: the cull's program and the counts' readbacks, as the tables' setup made them (fixed with the scene's tables).
		std::shared_ptr<const void> CullSetup(const Api::PassSetup& a_setup)
		{
			return SharedTablesOf(a_setup.scene);
		}

		std::shared_ptr<const void> CountsSetup(const Api::PassSetup& a_setup)
		{
			auto tables = SharedTablesOf(a_setup.scene);
			if (!tables || !tables->counts)
				return nullptr;
			return tables;
		}

		std::shared_ptr<org::RenderPass> MakeCullPhaseOne(const Api::PassContext& a_context)
		{
			return std::make_shared<Draws::TreeLodCullPass>(a_context.Main(), a_context.State<TreeLodTables>(), 1, a_context.segment);
		}

		// Phase 2's retest only with the HZB rebuilt this frame (the depth epoch's phase 2 block).
		std::shared_ptr<org::RenderPass> MakeCullPhaseTwo(const Api::PassContext& a_context)
		{
			if (!a_context.PhaseTwo())
				return nullptr;
			return std::make_shared<Draws::TreeLodCullPass>(a_context.Main(), a_context.State<TreeLodTables>(), 2, a_context.segment);
		}

		std::shared_ptr<org::RenderPass> MakeCounts(const Api::PassContext& a_context)
		{
			return std::make_shared<Draws::TreeLodReadbackPass>(a_context.State<TreeLodTables>());
		}
	}

	void RegisterPasses(Api::InstallContext& a_context)
	{
		// Phase 1's list before the depth draw; phase 2's retest against the HZB rebuilt this frame; the list's counts read back once both
		// phases are done.
		a_context.RegisterPass({ .key = "tree-lod.cull", .passName = "cs.dclf.z.tree-lod-cull", .make = &MakeCullPhaseOne, .setup = &CullSetup });
		a_context.RegisterPass({ .key = "tree-lod.cull-phase2", .passName = "cs.dclf.tree-lod-cull-phase2", .make = &MakeCullPhaseTwo, .setup = &CullSetup });
		a_context.RegisterPass({ .key = "tree-lod.counts", .passName = "cs.dclf.z.tree-lod-counts", .kind = Api::PassKind::Copy,
			.make = &MakeCounts, .setup = &CountsSetup });
	}
}
