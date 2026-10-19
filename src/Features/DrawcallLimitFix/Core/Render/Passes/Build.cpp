// The main view's culling: BuildDraws (both phases) and the sort's scatter.

#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Core/Render/Passes/PassesInternal.h"

namespace DCLF::Draws
{
	struct BuildDrawsBindings
	{
		org::DeclaredViewToken sequences, count, hzb, visibility, frustum;
		org::DeclaredViewToken memberQueries, memberResults, memberClaims;
		org::DeclaredViewToken sortCounts, sortStaging, sortRanks;
		org::DeclaredViewToken fadeRoots;
		org::DeclaredViewToken bucketCounts, poolFirsts;    // the depth segment's buckets (Resources::zBucketCounts) and the pool's firsts
		org::DeclaredViewToken cullPrograms, rootPrograms;  // the main camera's first phase: the cull binding Programs, each root's
		org::DeclaredViewToken classCounts;                 // the colour segment's state class sub-ranges' counts (Resources::classCounts)
	};

	struct BuildDrawsFrame
	{
		std::shared_ptr<const ComputeProgram> program, clearQueries;
		std::uint32_t queryGroups = 0;
		BuildDrawsConstants constants{};
		std::shared_ptr<const org::LatchBlock> latch;
		rhi::CommandSignatureHandle signature{};
	};

	void RecordLatchedDispatch(BuildDrawsConstants a_constants, const org::LatchBlock& a_latch,
		rhi::CommandSignatureHandle a_signature, std::uint32_t a_latchOffset, org::PassRecordContext& a_recording)
	{
		auto& commands = a_recording.Commands();
		const std::uint64_t offset = a_latch.Offset(a_recording.FrameSlot()) + a_latchOffset;
		a_constants.latchOffset = static_cast<std::uint32_t>(offset);
		commands.PushConstants(rhi::ShaderStage::Compute, 0, 0, 0, kBuildDrawsConstantWords, reinterpret_cast<const std::uint32_t*>(&a_constants));
		commands.ExecuteIndirect(a_signature, a_latch.Resource()->GetAPIResource().GetHandle(), offset, {}, 0, 1);
	}

	// Writes this frame's draw sequences and their count from the draw inputs (BuildDrawsCS.hlsl).
	class BuildDrawsPass final : public org::TypedRenderGraphPass<BuildDrawsPass, BuildDrawsFrame, BuildDrawsBindings>
	{
	public:
		BuildDrawsPass(std::shared_ptr<Resources> a_resources, RenderGraphRuntime::Segment a_segment, std::uint32_t a_phase = 0, bool a_queries = false) :
			resources(std::move(a_resources)), segment(a_segment), fixedPhase(a_phase), queries(a_queries) {}

		BuildDrawsBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			BuildDrawsBindings bindings{};
			if (queries) {
				a_builder.PreferQueue(org::QueueKind::Compute);
				const auto bind = [&](Api::CullBindingSlot slot, bool writable) {
					const auto& buffer = Extensions::CullBindingBuffer(slot, *resources->scene);
					return buffer ? (writable ? a_builder.UnorderedAccess(*buffer).View() : a_builder.ShaderResource(*buffer).View()) : org::DeclaredViewToken{};
				};
				bindings.memberQueries = bind(Api::CullBindingSlot::MemberQueries, true);
				bindings.memberClaims = bind(Api::CullBindingSlot::MemberClaims, true);
				bindings.cullPrograms = bind(Api::CullBindingSlot::Programs, false);
				bindings.rootPrograms = bind(Api::CullBindingSlot::RootPrograms, false);
				return bindings;
			}
			if (const auto& results = Extensions::CullBindingBuffer(Api::CullBindingSlot::MemberResults, *resources->scene); results && segment == RenderGraphRuntime::Segment::ZPrepass && fixedPhase != 2)
				bindings.memberResults = a_builder.ShaderResource(*results).View();

			// The scene list, the geometry table and the rows are the installed payload's (the latch names them): no recording binds them.
			bindings.sequences = a_builder.UnorderedAccess(*resources->sequences).View();
			bindings.count = a_builder.UnorderedAccess(resources->count).View();
			bindings.visibility = a_builder.UnorderedAccess(*resources->visibility).View();
			if (resources->frustum)
				bindings.frustum = a_builder.UnorderedAccess(*resources->frustum).View();
			// Every phase reads the fade roots' static rows (the cull binding RootRows: phase 1's fade verdicts, LOD skin partitions). The
			// states FadeStateCS published last frame (RootStates, the latch's) are undeclared: nothing this frame writes them.
			if (const auto& roots = Extensions::CullBindingBuffer(Api::CullBindingSlot::RootRows, *resources->scene))
				bindings.fadeRoots = a_builder.ShaderResource(*roots).View();
			// Phase 1 sees last frame's HZB, phase 2 the one rebuilt this frame: same resource, ordered by their place around the build.
			if (resources->hzb)
				bindings.hzb = a_builder.ShaderResource(resources->hzb).View();
			// The depth phases write into the pipeline slots' buckets, indexed from the pool, for the Z-prepass's plain draws.
			if (segment == RenderGraphRuntime::Segment::ZPrepass && resources->pool) {
				bindings.bucketCounts = a_builder.UnorderedAccess(*resources->zBucketCounts[fixedPhase == 2 ? 1 : 0]).View();
				bindings.poolFirsts = a_builder.ShaderResource(*resources->pool->firsts).View();
			}
			// The colour segment's refraction draws are pooled too; its other draws append through their state class sub-ranges' counts.
			if (segment == RenderGraphRuntime::Segment::MainOpaque && resources->pool)
				bindings.poolFirsts = a_builder.ShaderResource(*resources->pool->firsts).View();
			if (segment == RenderGraphRuntime::Segment::MainOpaque && resources->classCounts)
				bindings.classCounts = a_builder.UnorderedAccess(*resources->classCounts).View();
			if (Sorts()) {
				bindings.sortCounts = a_builder.UnorderedAccess(resources->sort->counts).View();
				bindings.sortStaging = a_builder.UnorderedAccess(*resources->sort->staging).View();
				bindings.sortRanks = a_builder.UnorderedAccess(*resources->sort->ranks).View();
			}
			return bindings;
		}

		void InvocationRevision([[maybe_unused]] const org::PassPrepareContext& a_preparation, std::vector<std::uint64_t>& a_out) const
		{
			const auto now = segment;
			const auto frame = CurrentFrame(a_preparation, now);
			a_out.push_back(frame ? frame->generation : 0);
			a_out.push_back(static_cast<std::uint64_t>(now));
			a_out.push_back(fixedPhase);
			a_out.push_back(queries);
			for (const auto slot : { Api::CullBindingSlot::MemberQueries, Api::CullBindingSlot::MemberResults, Api::CullBindingSlot::MemberClaims })
				a_out.push_back(NamedSerial(a_preparation, Extensions::CullBindingBuffer(slot, *resources->scene)));
			a_out.push_back(resources->pool ? NamedSerial(a_preparation, resources->pool->firsts) : 0);
		}

		BuildDrawsFrame Prepare(const BuildDrawsBindings& a_bindings, [[maybe_unused]] const org::PassPrepareContext& a_preparation) const
		{
			BuildDrawsFrame prepared{};
			const auto now = segment;
			const auto frame = CurrentFrame(a_preparation, now);
			if (!frame || !resources->buildDraws || !frame->latch || !resources->dispatchSignature)
				return prepared;
			const std::uint32_t phase = Phase(now);
			// Phase 2 (retesting phase 1's rejects against the rebuilt HZB) belongs to the depth segment only.
			if (fixedPhase == 2 && now != RenderGraphRuntime::Segment::ZPrepass)
				return prepared;
			prepared.program = resources->buildDraws;
			prepared.latch = frame->latch;
			prepared.signature = resources->dispatchSignature->GetHandle();
			auto& constants = prepared.constants;
			constants.latchIndex = frame->latch->SrvIndex();
			if (queries) {
				if (!a_bindings.memberQueries.layout || !a_bindings.memberClaims.layout)
					return {};
				prepared.program = resources->preparePortalQueries;
				prepared.clearQueries = resources->clearPortalQueries;
				prepared.signature = resources->portalQuerySignature->GetHandle();
				constants.phaseBits = 1u << 4;
				constants.memberQueriesIndex = CaptureViewIndex(a_preparation, a_bindings.memberQueries);
				constants.memberClaimsIndex = CaptureViewIndex(a_preparation, a_bindings.memberClaims);
				const auto capacity = NamedUnitsOf(a_preparation, Extensions::CullBindingBuffer(Api::CullBindingSlot::MemberQueries, *resources->scene));
				prepared.queryGroups = static_cast<std::uint32_t>((capacity + 63) / 64);
				if (RootRowsCurrent(*resources->scene) && a_bindings.cullPrograms.layout && a_bindings.rootPrograms.layout) {
					constants.cullProgramsIndex = CaptureViewIndex(a_preparation, a_bindings.cullPrograms);
					constants.rootProgramsIndex = CaptureViewIndex(a_preparation, a_bindings.rootPrograms);
				}
				return prepared;
			}
			if (phase == 1 && a_bindings.memberResults.layout)
				constants.memberResultsIndex = CaptureViewIndex(a_preparation, a_bindings.memberResults);

			constants.sequencesIndex = CaptureViewIndex(a_preparation, a_bindings.sequences);
			constants.countIndex = CaptureViewIndex(a_preparation, a_bindings.count);
			SetRowStrides(constants);
			constants.phaseBits = (phase & 0xFu) << 4;
			constants.visibilityIndex = CaptureViewIndex(a_preparation, a_bindings.visibility);
			// Frustum stamps: only depth phase 1 tests every candidate's frustum.
			if (resources->frustum && phase == 1)
				constants.frustumIndex = CaptureViewIndex(a_preparation, a_bindings.frustum);
			const bool rootRows = RootRowsCurrent(*resources->scene);
			if (rootRows) {
				constants.fadeRootsIndex = CaptureViewIndex(a_preparation, a_bindings.fadeRoots);
			}
			if (resources->hzb && frame->cullMode >= 2 && frame->width && frame->height) {
				constants.hzbIndex = CaptureViewIndex(a_preparation, a_bindings.hzb);
				constants.hzbSizePacked = (resources->hzbWidth & 0xFFFF) | (resources->hzbHeight << 16);
				constants.hzbMips = resources->hzbMips;
			}
			if (now == RenderGraphRuntime::Segment::ZPrepass && resources->pool) {
				constants.bucketCountsIndex = CaptureViewIndex(a_preparation, a_bindings.bucketCounts);
				constants.poolFirstsIndex = CaptureViewIndex(a_preparation, a_bindings.poolFirsts);
			}
			if (Sorts()) {
				constants.sortCountsIndex = CaptureViewIndex(a_preparation, a_bindings.sortCounts);
				constants.sortStagingIndex = CaptureViewIndex(a_preparation, a_bindings.sortStaging);
				constants.sortRanksIndex = CaptureViewIndex(a_preparation, a_bindings.sortRanks);
			}
			constants.phaseTwoBase = frame->sequenceDraws;
			constants.decalBase = 2 * frame->sequenceDraws;
			constants.decalStride = frame->sequenceDecals;
			// The state class sub-ranges' counts: colour segment only (the latch's class table and map say where each class draws).
			if (now == RenderGraphRuntime::Segment::MainOpaque && resources->classCounts)
				constants.classCountsIndex = CaptureViewIndex(a_preparation, a_bindings.classCounts);
			// The forward range (its classes' halves: the class table's): colour segment only.
			if (now == RenderGraphRuntime::Segment::MainOpaque && frame->forwardCapacity) {
				constants.forwardBase = static_cast<std::uint32_t>(ForwardStagingSlot(*frame));
				constants.forwardCapacity = frame->forwardCapacity;
			}
			// The refraction range follows the forward halves (forwardBase + 2 * forwardCapacity), so forwardBase is set even
			// without forward capacity.
			if (now == RenderGraphRuntime::Segment::MainOpaque && frame->refractionCapacity && resources->pool) {
				constants.forwardBase = static_cast<std::uint32_t>(ForwardStagingSlot(*frame));
				constants.refractionCapacity = frame->refractionCapacity;
				constants.poolFirstsIndex = CaptureViewIndex(a_preparation, a_bindings.poolFirsts);
			}
			return prepared;
		}

		static void Record(const BuildDrawsBindings&, const BuildDrawsFrame& a_frame, org::PassRecordContext& a_recording)
		{
			if (!a_frame.program || !a_frame.latch)
				return;
			auto& commands = a_recording.Commands();
			if (a_frame.clearQueries) {
				auto constants = a_frame.constants;
				constants.latchOffset = static_cast<std::uint32_t>(a_frame.latch->Offset(a_recording.FrameSlot()));
				commands.BindLayout(a_frame.clearQueries->layout->GetHandle());
				commands.BindPipeline(a_frame.clearQueries->pipeline->GetHandle());
				commands.PushConstants(rhi::ShaderStage::Compute, 0, 0, 0, kBuildDrawsConstantWords, reinterpret_cast<const std::uint32_t*>(&constants));
				commands.Dispatch(std::min(a_frame.queryGroups, 65535u), (a_frame.queryGroups + 65534u) / 65535u, 1);
				const auto barrier = rhi::FullMemoryBarrier();
				commands.Barriers(rhi::BarrierBatch{ {}, {}, { &barrier, 1 } });
			}

			commands.BindLayout(a_frame.program->layout->GetHandle());
			commands.BindPipeline(a_frame.program->pipeline->GetHandle());
			RecordLatchedDispatch(a_frame.constants, *a_frame.latch, a_frame.signature, 0, a_recording);
		}

		std::uint32_t Phase(RenderGraphRuntime::Segment a_segment) const
		{
			if (fixedPhase)
				return fixedPhase;
			return a_segment == RenderGraphRuntime::Segment::ZPrepass ? 1u : 3u;
		}

	private:
		// Only the colour segment sorts (by pipeline, for its device-generated draw); depth draws are bucketed.
		bool Sorts() const { return resources->sort && segment != RenderGraphRuntime::Segment::ZPrepass; }

		std::shared_ptr<Resources> resources;
		RenderGraphRuntime::Segment segment;
		std::uint32_t fixedPhase = 0;
		bool queries = false;
	};

	std::shared_ptr<org::RenderPass> MakeBuildDrawsPass(std::shared_ptr<Resources> a_resources, RenderGraphRuntime::Segment a_segment, std::uint32_t a_phase, bool a_queries)
	{
		return std::make_shared<BuildDrawsPass>(std::move(a_resources), a_segment, a_phase, a_queries);
	}

	struct SortSequencesBindings
	{
		org::DeclaredViewToken staging, ranks, offsets, count, sequences;
	};

	struct SortSequencesFrame
	{
		std::shared_ptr<const ComputeProgram> program;
		SortSequencesConstants constants{};
		std::uint32_t groups = 0;
	};

	/** @brief The sort's scatter, after BuildDraws and the scan, over the draws' full range (threads past the count return). */
	class SortSequencesPass final : public org::TypedRenderGraphPass<SortSequencesPass, SortSequencesFrame, SortSequencesBindings>
	{
	public:
		SortSequencesPass(std::shared_ptr<Resources> a_resources, RenderGraphRuntime::Segment a_segment) :
			resources(std::move(a_resources)), segment(a_segment) {}

		SortSequencesBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			SortSequencesBindings bindings{};
			bindings.staging = a_builder.ShaderResource(*resources->sort->staging).View();
			bindings.ranks = a_builder.ShaderResource(*resources->sort->ranks).View();
			bindings.offsets = a_builder.ShaderResource(resources->sort->offsets).View();
			bindings.count = a_builder.ShaderResource(resources->count).View();
			bindings.sequences = a_builder.UnorderedAccess(*resources->sequences).View();
			return bindings;
		}

		void InvocationRevision([[maybe_unused]] const org::PassPrepareContext& a_preparation, std::vector<std::uint64_t>& a_out) const
		{
			const auto now = segment;
			const auto frame = CurrentFrame(a_preparation, now);
			a_out.push_back(frame ? frame->generation : 0);
			a_out.push_back(static_cast<std::uint64_t>(now));
		}

		SortSequencesFrame Prepare(const SortSequencesBindings& a_bindings, [[maybe_unused]] const org::PassPrepareContext& a_preparation) const
		{
			SortSequencesFrame prepared{};
			if (!BuildsDraws(a_preparation, *resources, segment))
				return prepared;
			prepared.program = resources->sort->scatter;
			auto& constants = prepared.constants;
			constants.stagingIndex = CaptureViewIndex(a_preparation, a_bindings.staging);
			constants.ranksIndex = CaptureViewIndex(a_preparation, a_bindings.ranks);
			constants.offsetsIndex = CaptureViewIndex(a_preparation, a_bindings.offsets);
			constants.countIndex = CaptureViewIndex(a_preparation, a_bindings.count);
			constants.sequencesIndex = CaptureViewIndex(a_preparation, a_bindings.sequences);
			constants.sequenceStride = static_cast<std::uint32_t>(sizeof(DrawSequence));
			const auto frame = CurrentFrame(a_preparation, segment);
			constants.drawLimit = frame ? frame->sequenceDraws : 0;
			prepared.groups = (constants.drawLimit + kSortSequencesGroup - 1) / kSortSequencesGroup;
			return prepared;
		}

		static void Record(const SortSequencesBindings&, const SortSequencesFrame& a_frame, org::PassRecordContext& a_recording)
		{
			if (!a_frame.program || !a_frame.groups)
				return;
			auto& commands = a_recording.Commands();
			commands.BindLayout(a_frame.program->layout->GetHandle());
			commands.BindPipeline(a_frame.program->pipeline->GetHandle());
			commands.PushConstants(rhi::ShaderStage::Compute, 0, 0, 0, kSortSequencesConstantWords, reinterpret_cast<const std::uint32_t*>(&a_frame.constants));
			commands.Dispatch(a_frame.groups, 1, 1);
		}

	private:
		std::shared_ptr<Resources> resources;
		RenderGraphRuntime::Segment segment;
	};

	std::shared_ptr<org::RenderPass> MakeSortSequencesPass(std::shared_ptr<Resources> a_resources, RenderGraphRuntime::Segment a_segment)
	{
		return std::make_shared<SortSequencesPass>(std::move(a_resources), a_segment);
	}
}
