// Grass's passes: its cull (GrassCullCS) and its draws' arguments (GrassArgsCS) in the depth segment's two phases, right before each
// phase's depth draw; the colour draws' arguments once both phases are done (which zero the counts for the next frame); and the draws
// header's readback, from the tables (the scene resources) and the depth segment's latch.

// Main/Resources.h first: volk must precede every Vulkan header of the translation unit.
#include "Features/DrawcallLimitFix/Core/Render/Main/Resources.h"
#include "Features/DrawcallLimitFix/Features/Grass/GrassTables.h"
#include "Features/DrawcallLimitFix/Core/Extensions/PassContext.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/Helpers.h"
#include "Features/DrawcallLimitFix/Core/Render/Revision/Shapes.h"
#include "Features/DrawcallLimitFix/Core/Render/Epochs/GraphConditions.h"

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
	// In DCLF::Draws, as tree LOD's: ORG's recipe telemetry names a pass by its type (typeid).
	struct GrassCullBindings
	{
		org::DeclaredViewToken draw, instances, owners, shapes, buckets, frame, visible, draws, hzb;
	};

	struct GrassCullPrepared
	{
		std::shared_ptr<const ComputeProgram> program;
		Features::Grass::GrassCullConstants constants{};
		std::shared_ptr<const org::LatchBlock> latch;
		std::uint32_t groups = 0;
	};

	/**
	 * @brief Grass's cull (GrassCullCS.hlsl) in the depth segment's two phases, like BuildDraws: phase 1 tests every live instance slot
	 * (Grass Optimizations' distance, frustum, size, thinning and fade tests, then last frame's HZB), writes its frame values and
	 * appends it to its bucket tier's visible range, or to the retest list; phase 2 retests phase 1's occluded ones against the HZB
	 * rebuilt this frame and appends the ones it brings back after phase 1's.
	 */
	class GrassCullPass final : public org::TypedRenderGraphPass<GrassCullPass, GrassCullPrepared, GrassCullBindings>
	{
	public:
		// a_segment: its epoch's (the depth epoch's: the PassContext it was made with).
		GrassCullPass(std::shared_ptr<Resources> a_resources, std::shared_ptr<const Features::Grass::GrassTables> a_tables, std::uint32_t a_phase,
			RenderGraphRuntime::Segment a_segment) :
			resources(std::move(a_resources)), tables(std::move(a_tables)), phase(a_phase), segment(a_segment) {}

		GrassCullBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			GrassCullBindings bindings{};
			bindings.draw = a_builder.ShaderResource(tables->draw).View();
			bindings.instances = a_builder.ShaderResource(*tables->instances).View();
			bindings.owners = a_builder.ShaderResource(*tables->owners).View();
			bindings.shapes = a_builder.ShaderResource(*tables->shapes).View();
			bindings.buckets = a_builder.ShaderResource(*tables->buckets).View();
			bindings.frame = a_builder.UnorderedAccess(*tables->frame).View();
			bindings.visible = a_builder.UnorderedAccess(*tables->visible).View();
			bindings.draws = a_builder.UnorderedAccess(tables->draws).View();
			// Which HZB a phase sees depends on its place around the HZB build (GatherStructuralPasses).
			if (resources->hzb)
				bindings.hzb = a_builder.ShaderResource(resources->hzb).View();
			return bindings;
		}

		// The recording depends on the tables' versions (a growth changes views and dispatch) and the depth segment's shape.
		void InvocationRevision([[maybe_unused]] const org::PassPrepareContext& a_preparation, std::vector<std::uint64_t>& a_out) const
		{
			const auto frame = CurrentFrame(a_preparation, segment);
			for (const auto* table : { &tables->instances, &tables->owners, &tables->frame, &tables->visible, &tables->shapes, &tables->buckets })
				a_out.push_back(NamedSerial(a_preparation, *table));
			a_out.push_back(frame ? frame->generation : 0);
		}

		GrassCullPrepared Prepare(const GrassCullBindings& a_bindings, [[maybe_unused]] const org::PassPrepareContext& a_preparation) const
		{
			GrassCullPrepared prepared{};
			const auto frame = CurrentFrame(a_preparation, segment);
			// The instance slots every instance-slot table holds (the revision's versions).
			const std::uint64_t slots = std::min({ NamedUnitsOf(a_preparation, tables->instances), NamedUnitsOf(a_preparation, tables->owners),
				NamedUnitsOf(a_preparation, tables->frame), NamedUnitsOf(a_preparation, tables->visible) });
			if (!tables->cull || !slots || !frame || !frame->latch)
				return prepared;
			prepared.program = tables->cull;
			prepared.latch = frame->latch;
			auto& constants = prepared.constants;
			constants.drawIndex = CaptureViewIndex(a_preparation, a_bindings.draw);
			constants.instancesIndex = CaptureViewIndex(a_preparation, a_bindings.instances);
			constants.ownersIndex = CaptureViewIndex(a_preparation, a_bindings.owners);
			constants.shapesIndex = CaptureViewIndex(a_preparation, a_bindings.shapes);
			constants.bucketsIndex = CaptureViewIndex(a_preparation, a_bindings.buckets);
			constants.frameIndex = CaptureViewIndex(a_preparation, a_bindings.frame);
			constants.visibleIndex = CaptureViewIndex(a_preparation, a_bindings.visible);
			constants.drawsIndex = CaptureViewIndex(a_preparation, a_bindings.draws);
			constants.latchIndex = frame->latch->SrvIndex();
			constants.phase = phase;
			// The retest list past the visible entries (GrassLayout.h): 12 bytes per unit the visible version holds.
			constants.retestOffset = static_cast<std::uint32_t>(NamedUnitsOf(a_preparation, tables->visible) * GrassInstances::kRetestOffsetPerUnit);
			if (Draws::OcclusionTested(*resources, *frame)) {
				constants.hzbIndex = CaptureViewIndex(a_preparation, a_bindings.hzb);
				constants.hzbSizePacked = (resources->hzbWidth & 0xFFFF) | (resources->hzbHeight << 16);
				constants.hzbMips = resources->hzbMips;
			}
			// Dispatched for full capacity; the shader stops at the draw row's slots (phase 1) or the retest count (phase 2).
			prepared.groups = static_cast<std::uint32_t>((slots + Features::Grass::kGrassCullGroup - 1) / Features::Grass::kGrassCullGroup);
			return prepared;
		}

		static void Record(const GrassCullBindings&, const GrassCullPrepared& a_frame, org::PassRecordContext& a_recording)
		{
			if (!a_frame.program || !a_frame.groups || !a_frame.latch)
				return;
			auto constants = a_frame.constants;
			// The depth segment's BuildDrawsLatch in this frame slot.
			constants.latchOffset = static_cast<std::uint32_t>(a_frame.latch->Offset(a_recording.FrameSlot()));
			auto& commands = a_recording.Commands();
			commands.BindLayout(a_frame.program->layout->GetHandle());
			commands.BindPipeline(a_frame.program->Pipeline());
			commands.PushConstants(rhi::ShaderStage::Compute, 0, 0, 0, Features::Grass::kGrassCullConstantWords, reinterpret_cast<const std::uint32_t*>(&constants));
			commands.Dispatch(a_frame.groups, 1, 1);
		}

	private:
		std::shared_ptr<Resources> resources;
		std::shared_ptr<const Features::Grass::GrassTables> tables;
		std::uint32_t phase = 1;
		RenderGraphRuntime::Segment segment;
	};

	struct GrassArgsBindings
	{
		org::DeclaredViewToken draw, buckets, meshes, draws;
	};

	struct GrassArgsPrepared
	{
		std::shared_ptr<const ComputeProgram> program;
		Features::Grass::GrassArgsConstants constants{};
		std::uint32_t groups = 0;
	};

	/**
	 * @brief Grass's draws' arguments (GrassArgsCS.hlsl): a thread per draw slot (a bucket's tier) packs the slot's draw, if it has
	 * instances, into its array - phase 1's depth draws (mode 1), phase 2's (2: the instances phase 2 appended after phase 1's), or the
	 * colour draws of the bucket's colour program (3: every instance, then the slot's counts zeroed for the next frame) - and counts it
	 * in the header, which the draws' ExecuteIndirect reads as its count.
	 */
	class GrassArgsPass final : public org::TypedRenderGraphPass<GrassArgsPass, GrassArgsPrepared, GrassArgsBindings>
	{
	public:
		GrassArgsPass(std::shared_ptr<const Features::Grass::GrassTables> a_tables, std::uint32_t a_mode) :
			tables(std::move(a_tables)), mode(a_mode) {}

		GrassArgsBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			GrassArgsBindings bindings{};
			bindings.draw = a_builder.ShaderResource(tables->draw).View();
			bindings.buckets = a_builder.ShaderResource(*tables->buckets).View();
			bindings.meshes = a_builder.ShaderResource(*tables->meshes).View();
			bindings.draws = a_builder.UnorderedAccess(tables->draws).View();
			return bindings;
		}

		void InvocationRevision([[maybe_unused]] const org::PassPrepareContext& a_preparation, std::vector<std::uint64_t>& a_out) const
		{
			a_out.push_back(NamedSerial(a_preparation, tables->buckets));
			a_out.push_back(NamedSerial(a_preparation, tables->meshes));
		}

		GrassArgsPrepared Prepare(const GrassArgsBindings& a_bindings, [[maybe_unused]] const org::PassPrepareContext& a_preparation) const
		{
			GrassArgsPrepared prepared{};
			const auto buckets = std::min<std::uint64_t>(NamedUnitsOf(a_preparation, tables->buckets), GrassInstances::kMaxBuckets);
			if (!tables->args || !buckets)
				return prepared;
			prepared.program = tables->args;
			auto& constants = prepared.constants;
			constants.drawIndex = CaptureViewIndex(a_preparation, a_bindings.draw);
			constants.bucketsIndex = CaptureViewIndex(a_preparation, a_bindings.buckets);
			constants.meshesIndex = CaptureViewIndex(a_preparation, a_bindings.meshes);
			constants.drawsIndex = CaptureViewIndex(a_preparation, a_bindings.draws);
			constants.mode = mode;
			constants.drawSlots = static_cast<std::uint32_t>(buckets * GrassInstances::kTiers);
			// Dispatched for the buckets the version holds; the shader stops at the draw row's bucket slots.
			prepared.groups = (constants.drawSlots + Features::Grass::kGrassArgsGroup - 1) / Features::Grass::kGrassArgsGroup;
			return prepared;
		}

		static void Record(const GrassArgsBindings&, const GrassArgsPrepared& a_frame, org::PassRecordContext& a_recording)
		{
			if (!a_frame.program || !a_frame.groups)
				return;
			auto& commands = a_recording.Commands();
			commands.BindLayout(a_frame.program->layout->GetHandle());
			commands.BindPipeline(a_frame.program->Pipeline());
			commands.PushConstants(rhi::ShaderStage::Compute, 0, 0, 0, Features::Grass::kGrassArgsConstantWords, reinterpret_cast<const std::uint32_t*>(&a_frame.constants));
			commands.Dispatch(a_frame.groups, 1, 1);
		}

	private:
		std::shared_ptr<const Features::Grass::GrassTables> tables;
		std::uint32_t mode = 1;
	};

	struct GrassReadbackBindings
	{
		org::ResourceBindingToken draws;
	};

	struct GrassReadbackPrepared
	{
		std::shared_ptr<Features::Grass::GrassCounts> counts;
	};

	/** @brief Copies grass's draws header to the frame slot's host buffer once both phases are done, first summing what the slot held. */
	class GrassReadbackPass final : public org::TypedRenderGraphPass<GrassReadbackPass, GrassReadbackPrepared, GrassReadbackBindings>
	{
	public:
		explicit GrassReadbackPass(std::shared_ptr<const Features::Grass::GrassTables> a_tables) :
			tables(std::move(a_tables)) {}

		GrassReadbackBindings Declare(org::PassBuilder& a_builder)
		{
			return { a_builder.CopySource(tables->draws) };
		}

		void InvocationRevision([[maybe_unused]] const org::PassPrepareContext& a_preparation, std::vector<std::uint64_t>& a_out) const
		{
			a_out.push_back(0);
		}

		GrassReadbackPrepared Prepare(const GrassReadbackBindings&, [[maybe_unused]] const org::PassPrepareContext& a_preparation) const
		{
			return { tables->counts };
		}

		static void Sum(Features::Grass::GrassCounts& a_counts, const GrassInstances::DrawsHeader& a_header)
		{
			auto add = [](std::atomic<std::uint64_t>& a_sum, std::uint32_t a_value) { a_sum.fetch_add(a_value, std::memory_order_relaxed); };
			add(a_counts.frames, 1);
			add(a_counts.live, a_header.stats[GrassInstances::kStatLive]);
			add(a_counts.outside, a_header.stats[GrassInstances::kStatOutside]);
			add(a_counts.thinned, a_header.stats[GrassInstances::kStatThinned]);
			add(a_counts.faded, a_header.stats[GrassInstances::kStatFaded]);
			add(a_counts.tested, a_header.tested);
			add(a_counts.phaseOne, a_header.phaseOneInstances);
			add(a_counts.retests, a_header.retests);
			add(a_counts.phaseTwo, a_header.phaseTwoInstances);
			add(a_counts.phaseOneDraws, a_header.phaseOneDraws);
			add(a_counts.phaseTwoDraws, a_header.phaseTwoDraws);
			add(a_counts.colourDraws, a_header.colourDraws[0]);
			add(a_counts.pbrDraws, a_header.colourDraws[1]);
		}

		static void Record(const GrassReadbackBindings& a_bindings, const GrassReadbackPrepared& a_frame, org::PassRecordContext& a_recording)
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
					GrassInstances::DrawsHeader header;
					std::memcpy(&header, mapped, sizeof(header));
					Sum(counts, header);
					resource.Unmap(0, 0);
				}
			}
			a_recording.Commands().CopyBufferRegion(counts.readback[slot]->GetAPIResource().GetHandle(), 0, a_recording.Resolve(a_bindings.draws).GetHandle(), 0,
				sizeof(GrassInstances::DrawsHeader));
			counts.filled[slot] = 1;
		}

	private:
		std::shared_ptr<const Features::Grass::GrassTables> tables;
	};
}

namespace DCLF::Features::Grass
{
	namespace
	{
		// The passes' states: the programs and the counts' readbacks, as the tables' setup made them (fixed with the scene's tables).
		std::shared_ptr<const void> TablesSetup(const Api::PassSetup& a_setup)
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
			if (a_context.Interior())  // no grass in an interior: its passes are left out
				return nullptr;
			return std::make_shared<Draws::GrassCullPass>(a_context.Main(), a_context.State<GrassTables>(), 1, a_context.segment);
		}

		std::shared_ptr<org::RenderPass> MakeArgsPhaseOne(const Api::PassContext& a_context)
		{
			if (a_context.Interior())
				return nullptr;
			return std::make_shared<Draws::GrassArgsPass>(a_context.State<GrassTables>(), kArgsPhaseOne);
		}

		// Phase 2's retest and its draws' arguments only with the HZB rebuilt this frame (the depth epoch's phase 2 block).
		std::shared_ptr<org::RenderPass> MakeCullPhaseTwo(const Api::PassContext& a_context)
		{
			if (a_context.Interior())
				return nullptr;
			if (!a_context.PhaseTwo())
				return nullptr;
			return std::make_shared<Draws::GrassCullPass>(a_context.Main(), a_context.State<GrassTables>(), 2, a_context.segment);
		}

		std::shared_ptr<org::RenderPass> MakeArgsPhaseTwo(const Api::PassContext& a_context)
		{
			if (a_context.Interior())
				return nullptr;
			if (!a_context.PhaseTwo())
				return nullptr;
			return std::make_shared<Draws::GrassArgsPass>(a_context.State<GrassTables>(), kArgsPhaseTwo);
		}

		// The colour draws' arguments, always (they zero the counts the next frame's cull appends with).
		std::shared_ptr<org::RenderPass> MakeArgsColour(const Api::PassContext& a_context)
		{
			if (a_context.Interior())
				return nullptr;
			return std::make_shared<Draws::GrassArgsPass>(a_context.State<GrassTables>(), kArgsColour);
		}

		std::shared_ptr<org::RenderPass> MakeCounts(const Api::PassContext& a_context)
		{
			if (a_context.Interior())
				return nullptr;
			return std::make_shared<Draws::GrassReadbackPass>(a_context.State<GrassTables>());
		}
	}

	void RegisterPasses(Api::InstallContext& a_context)
	{
		// Phase 1's lists and arguments before the depth draw; phase 2's retest against the HZB rebuilt this frame and its arguments; the
		// colour draws' arguments once both phases are done; the header's counts read back.
		a_context.RegisterPass({ .key = "grass.cull", .passName = "cs.dclf.z.grass-cull", .make = &MakeCullPhaseOne, .setup = &TablesSetup });
		a_context.RegisterPass({ .key = "grass.args", .passName = "cs.dclf.z.grass-args", .make = &MakeArgsPhaseOne, .setup = &TablesSetup });
		a_context.RegisterPass({ .key = "grass.cull-phase2", .passName = "cs.dclf.grass-cull-phase2", .make = &MakeCullPhaseTwo, .setup = &TablesSetup });
		a_context.RegisterPass({ .key = "grass.args-phase2", .passName = "cs.dclf.grass-args-phase2", .make = &MakeArgsPhaseTwo, .setup = &TablesSetup });
		a_context.RegisterPass({ .key = "grass.args-colour", .passName = "cs.dclf.z.grass-args-colour", .make = &MakeArgsColour, .setup = &TablesSetup });
		a_context.RegisterPass({ .key = "grass.counts", .passName = "cs.dclf.z.grass-counts", .kind = Api::PassKind::Copy, .make = &MakeCounts, .setup = &CountsSetup });
	}
}
