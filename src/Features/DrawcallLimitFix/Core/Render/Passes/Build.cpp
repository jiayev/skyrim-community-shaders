// The main view's culling: BuildDraws (both phases) and the sort's scatter.

#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Core/Render/Passes/PassesInternal.h"
#include "Features/DrawcallLimitFix/Core/Render/Epochs/GraphConditions.h"

#include <array>
#include <cstddef>
#include <cstring>
#include <type_traits>
#include <vector>

namespace DCLF::Draws
{
	struct BuildDrawsBindings
	{
		org::DeclaredViewToken sequences, count, hzb, visibility, frustum;
		org::DeclaredViewToken writers;  // TEMP (CS_DCLF_SET_PARITY): Resources::writers
		org::DeclaredViewToken memberQueries, memberResults, memberClaims;
		org::DeclaredViewToken sortCounts, sortStaging, sortRanks;
		org::DeclaredViewToken fadeRoots;
		org::DeclaredViewToken bucketCounts, poolFirsts;    // the depth segment's buckets (Resources::zBucketCounts) and the pool's firsts
		org::DeclaredViewToken rootPrograms;  // the main member queries: each root's program (the programs are the latch's)
		org::DeclaredViewToken classCounts;                 // the colour segment's state class sub-ranges' counts (Resources::classCounts)
		org::DeclaredViewToken objectGates;                 // the colour segment: the objects' strict-light gates (the cull binding ObjectGates)
		org::DeclaredViewToken retests, retestDispatch;     // the depth phases: the retest list (phase 1 appends, phase 2 reads), phase 2's dispatch
		org::ResourceBindingToken retestArguments;          // phase 2: that dispatch, as its ExecuteIndirect's arguments
	};

	namespace
	{
		static_assert(std::is_trivially_copyable_v<BuildDrawsConstants> && sizeof(BuildDrawsConstants) == kBuildDrawsConstantWords * sizeof(std::uint32_t));

		/**
		 * @brief The words of a BuildDrawsConstants its recording fills with declared views' bindless indices (PassRecordContext::Index):
		 * Prepare chooses the view each names, the recording resolves them against the frame it records.
		 */
		class ViewIndexWords
		{
		public:
			/** @brief Has the recording fill a_field (a word of a_constants) with a_view's index; a later Set of the same word replaces it. */
			void Set(const BuildDrawsConstants& a_constants, const std::uint32_t& a_field, const org::DeclaredViewToken& a_view)
			{
				const auto word = static_cast<std::uint32_t>(
					(reinterpret_cast<const std::byte*>(&a_field) - reinterpret_cast<const std::byte*>(&a_constants)) / sizeof(std::uint32_t));
				for (auto& entry : entries)
					if (entry.word == word) {
						entry.view = a_view;
						return;
					}
				entries.push_back({ word, a_view });
			}

			/** @brief a_constants with every word Set filled from a_recording's views. */
			BuildDrawsConstants Apply(BuildDrawsConstants a_constants, const org::PassRecordContext& a_recording) const
			{
				std::array<std::uint32_t, kBuildDrawsConstantWords> words{};
				std::memcpy(words.data(), &a_constants, sizeof(a_constants));
				for (const auto& entry : entries)
					words[entry.word] = a_recording.Index(entry.view);
				std::memcpy(&a_constants, words.data(), sizeof(a_constants));
				return a_constants;
			}

		private:
			struct Entry
			{
				std::uint32_t word = 0;
				org::DeclaredViewToken view;
			};
			std::vector<Entry> entries;
		};
	}

	struct BuildDrawsFrame
	{
		std::shared_ptr<const ComputeProgram> program, clearQueries;
		std::uint32_t queryGroups = 0;
		BuildDrawsConstants constants{};
		ViewIndexWords views;  // the constants' view index words
		std::shared_ptr<const org::LatchBlock> latch;
		rhi::CommandSignatureHandle signature{};
		// Phase 2 over the retest list: dispatched from the bindings' retestArguments (phase 1's groups), not the latch's.
		bool retestIndirect = false;
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
				const auto bind = [&](Api::CullBindingSlot slot, bool writable) {
					const auto& buffer = Extensions::CullBindingBuffer(slot, *resources->scene);
					return buffer ? (writable ? a_builder.UnorderedAccess(*buffer).View() : a_builder.ShaderResource(*buffer).View()) : org::DeclaredViewToken{};
				};
				bindings.memberQueries = bind(Api::CullBindingSlot::MemberQueries, true);
				bindings.memberClaims = bind(Api::CullBindingSlot::MemberClaims, true);
				bindings.rootPrograms = bind(Api::CullBindingSlot::RootPrograms, false);
				return bindings;
			}
			if (const auto& results = Extensions::CullBindingBuffer(Api::CullBindingSlot::MemberResults, *resources->scene); results && segment == RenderGraphRuntime::Segment::ZPrepass && fixedPhase != 2)
				bindings.memberResults = a_builder.ShaderResource(*results).View();

			// The scene list, the geometry table and the rows are the installed payload's (the latch names them): no recording binds them.
			bindings.sequences = a_builder.UnorderedAccess(*resources->sequences).View();
			bindings.count = a_builder.UnorderedAccess(resources->count).View();
			bindings.visibility = a_builder.UnorderedAccess(*resources->visibility).View();
			if (resources->writers)
				bindings.writers = a_builder.UnorderedAccess(*resources->writers).View();
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
			// Phase 1 appends its OccludedRetest inputs to the retest list and grows phase 2's dispatch; phase 2 runs over the list, dispatched
			// from it.
			if (segment == RenderGraphRuntime::Segment::ZPrepass && resources->retests && resources->retestDispatch) {
				if (fixedPhase == 2) {
					bindings.retests = a_builder.ShaderResource(*resources->retests).View();
					bindings.retestArguments = a_builder.IndirectArguments(resources->retestDispatch);
				} else {
					bindings.retests = a_builder.UnorderedAccess(*resources->retests).View();
					bindings.retestDispatch = a_builder.UnorderedAccess(resources->retestDispatch).View();
				}
			}
			// The colour segment's refraction draws are pooled too; its other draws append through their state class sub-ranges' counts.
			if (segment == RenderGraphRuntime::Segment::MainOpaque && resources->pool)
				bindings.poolFirsts = a_builder.ShaderResource(*resources->pool->firsts).View();
			if (segment == RenderGraphRuntime::Segment::MainOpaque && resources->classCounts)
				bindings.classCounts = a_builder.UnorderedAccess(*resources->classCounts).View();
			// The colour segment's receivers of portal-strict lights pass their gates (LocalShadowMask).
			if (segment == RenderGraphRuntime::Segment::MainOpaque)
				if (const auto& gates = Extensions::CullBindingBuffer(Api::CullBindingSlot::ObjectGates, *resources->scene))
					bindings.objectGates = a_builder.ShaderResource(*gates).View();
			if (Sorts()) {
				bindings.sortCounts = a_builder.UnorderedAccess(resources->sort->counts).View();
				bindings.sortStaging = a_builder.UnorderedAccess(*resources->sort->staging).View();
				bindings.sortRanks = a_builder.UnorderedAccess(*resources->sort->ranks).View();
			}
			return bindings;
		}

		// The shape and the buffer versions the recording binds are the revision's (the ticket's host data).
		void Revision(std::vector<std::uint64_t>& a_out) const
		{
			a_out.push_back(static_cast<std::uint64_t>(segment));
			a_out.push_back(fixedPhase);
			a_out.push_back(queries);
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
			auto& views = prepared.views;
			constants.latchIndex = frame->latch->SrvIndex();
			if (queries) {
				if (!a_bindings.memberQueries.layout || !a_bindings.memberClaims.layout)
					return {};
				prepared.program = resources->preparePortalQueries;
				prepared.clearQueries = resources->clearPortalQueries;
				prepared.signature = resources->portalQuerySignature->GetHandle();
				constants.phaseBits = 1u << 4;
				views.Set(constants, constants.memberQueriesIndex, a_bindings.memberQueries);
				views.Set(constants, constants.memberClaimsIndex, a_bindings.memberClaims);
				const auto capacity = NamedUnitsOf(a_preparation, Extensions::CullBindingBuffer(Api::CullBindingSlot::MemberQueries, *resources->scene));
				prepared.queryGroups = static_cast<std::uint32_t>((capacity + 63) / 64);
				// The programs are the depth latch's portalProgramsIndex (the frame's publication).
				if (RootRowsCurrent(*resources->scene) && a_bindings.rootPrograms.layout)
					views.Set(constants, constants.rootProgramsIndex, a_bindings.rootPrograms);
				return prepared;
			}
			if (phase == 1 && a_bindings.memberResults.layout)
				views.Set(constants, constants.memberResultsIndex, a_bindings.memberResults);

			views.Set(constants, constants.sequencesIndex, a_bindings.sequences);
			views.Set(constants, constants.countIndex, a_bindings.count);
			SetRowStrides(constants);
			constants.phaseBits = (phase & 0xFu) << 4;
			views.Set(constants, constants.visibilityIndex, a_bindings.visibility);
			// TEMP (CS_DCLF_SET_PARITY): the writer records, through the claims word only the query preparation uses.
			if (a_bindings.writers.layout)
				views.Set(constants, constants.memberClaimsIndex, a_bindings.writers);
			// Frustum stamps: only depth phase 1 tests every candidate's frustum.
			if (resources->frustum && phase == 1)
				views.Set(constants, constants.frustumIndex, a_bindings.frustum);
			const bool rootRows = RootRowsCurrent(*resources->scene);
			if (rootRows) {
				views.Set(constants, constants.fadeRootsIndex, a_bindings.fadeRoots);
			}
			const bool occlusion = Draws::OcclusionTested(*resources, *frame);
			if (occlusion) {
				views.Set(constants, constants.hzbIndex, a_bindings.hzb);
				constants.hzbSizePacked = (resources->hzbWidth & 0xFFFF) | (resources->hzbHeight << 16);
				constants.hzbMips = resources->hzbMips;
			}
			// The retest list, with the HZB test (only it leaves an input OccludedRetest): phase 1 appends, phase 2 runs over the list,
			// dispatched indirect from phase 1's groups. Both phases decide this from the same shape, so phase 2 never waits on a list
			// phase 1 did not fill; without it phase 2 runs over every input from the latch's dispatch.
			if (occlusion && now == RenderGraphRuntime::Segment::ZPrepass && a_bindings.retests.layout) {
				if (phase == 2) {
					views.Set(constants, constants.retestsIndex, a_bindings.retests);
					prepared.retestIndirect = true;
				} else if (a_bindings.retestDispatch.layout) {
					views.Set(constants, constants.retestsIndex, a_bindings.retests);
					views.Set(constants, constants.retestDispatchIndex, a_bindings.retestDispatch);
				}
			}
			if (now == RenderGraphRuntime::Segment::ZPrepass && resources->pool) {
				views.Set(constants, constants.bucketCountsIndex, a_bindings.bucketCounts);
				views.Set(constants, constants.poolFirstsIndex, a_bindings.poolFirsts);
			}
			if (Sorts()) {
				views.Set(constants, constants.sortCountsIndex, a_bindings.sortCounts);
				views.Set(constants, constants.sortStagingIndex, a_bindings.sortStaging);
				views.Set(constants, constants.sortRanksIndex, a_bindings.sortRanks);
			}
			constants.phaseTwoBase = frame->sequenceDraws;
			constants.decalBase = 2 * frame->sequenceDraws;
			constants.decalStride = frame->sequenceDecals;
			// The state class sub-ranges' counts: colour segment only (the latch's class table and map say where each class draws).
			if (now == RenderGraphRuntime::Segment::MainOpaque && resources->classCounts)
				views.Set(constants, constants.classCountsIndex, a_bindings.classCounts);
			if (now == RenderGraphRuntime::Segment::MainOpaque && a_bindings.objectGates.layout)
				views.Set(constants, constants.objectGatesIndex, a_bindings.objectGates);
			// The forward range (its classes' halves: the class table's): colour segment only.
			if (now == RenderGraphRuntime::Segment::MainOpaque && frame->forwardCapacity) {
				constants.forwardBase = static_cast<std::uint32_t>(ForwardStagingSlot(*frame));
				constants.forwardCapacity = frame->forwardCapacity;
			}
			// The forward buckets' draws are pooled (CS_DCLF_FORWARD_PLAIN), as are refraction's.
			if (now == RenderGraphRuntime::Segment::MainOpaque && resources->pool)
				views.Set(constants, constants.poolFirstsIndex, a_bindings.poolFirsts);
			// The refraction range follows the forward halves (forwardBase + 2 * forwardCapacity), so forwardBase is set even
			// without forward capacity.
			if (now == RenderGraphRuntime::Segment::MainOpaque && frame->refractionCapacity && resources->pool) {
				constants.forwardBase = static_cast<std::uint32_t>(ForwardStagingSlot(*frame));
				constants.refractionCapacity = frame->refractionCapacity;
				views.Set(constants, constants.poolFirstsIndex, a_bindings.poolFirsts);
			}
			return prepared;
		}

		static void Record(const BuildDrawsBindings& a_bindings, const BuildDrawsFrame& a_frame, org::PassRecordContext& a_recording)
		{
			if (!a_frame.program || !a_frame.latch)
				return;
			auto& commands = a_recording.Commands();
			const auto viewed = a_frame.views.Apply(a_frame.constants, a_recording);
			if (a_frame.clearQueries) {
				auto constants = viewed;
				constants.latchOffset = static_cast<std::uint32_t>(a_frame.latch->Offset(a_recording.FrameSlot()));
				commands.BindLayout(a_frame.clearQueries->layout->GetHandle());
				commands.BindPipeline(a_frame.clearQueries->Pipeline());
				commands.PushConstants(rhi::ShaderStage::Compute, 0, 0, 0, kBuildDrawsConstantWords, reinterpret_cast<const std::uint32_t*>(&constants));
				commands.Dispatch(std::min(a_frame.queryGroups, 65535u), (a_frame.queryGroups + 65534u) / 65535u, 1);
				const auto barrier = rhi::FullMemoryBarrier();
				commands.Barriers(rhi::BarrierBatch{ {}, {}, { &barrier, 1 } });
			}

			commands.BindLayout(a_frame.program->layout->GetHandle());
			commands.BindPipeline(a_frame.program->Pipeline());
			if (a_frame.retestIndirect) {
				// Phase 2: the latch's values, the retest list's groups.
				auto constants = viewed;
				constants.latchOffset = static_cast<std::uint32_t>(a_frame.latch->Offset(a_recording.FrameSlot()));
				commands.PushConstants(rhi::ShaderStage::Compute, 0, 0, 0, kBuildDrawsConstantWords, reinterpret_cast<const std::uint32_t*>(&constants));
				commands.ExecuteIndirect(a_frame.signature, a_recording.Resolve(a_bindings.retestArguments).GetHandle(), 0, {}, 0, 1);
				return;
			}
			RecordLatchedDispatch(viewed, *a_frame.latch, a_frame.signature, 0, a_recording);
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

		// The shape is the revision's (the ticket's host data).
		void Revision(std::vector<std::uint64_t>& a_out) const { a_out.push_back(static_cast<std::uint64_t>(segment)); }

		SortSequencesFrame Prepare(const SortSequencesBindings&, const org::PassPrepareContext& a_preparation) const
		{
			SortSequencesFrame prepared{};
			if (!BuildsDraws(a_preparation, *resources, segment))
				return prepared;
			// The main buckets' plain draws (CS_DCLF_MAIN_PLAIN) stage nothing: BuildDraws writes them in place, and its draw count is no
			// staged count, so a scatter would copy stale staging over the buckets.
			if (const auto shape = CurrentFrame(a_preparation, segment); shape && shape->classPlan && !shape->classPlan->main.empty())
				return prepared;
			prepared.program = resources->sort->scatter;
			auto& constants = prepared.constants;
			constants.sequenceStride = static_cast<std::uint32_t>(sizeof(DrawSequence));
			const auto frame = CurrentFrame(a_preparation, segment);
			constants.drawLimit = frame ? frame->sequenceDraws : 0;
			prepared.groups = (constants.drawLimit + kSortSequencesGroup - 1) / kSortSequencesGroup;
			return prepared;
		}

		static void Record(const SortSequencesBindings& a_bindings, const SortSequencesFrame& a_frame, org::PassRecordContext& a_recording)
		{
			if (!a_frame.program || !a_frame.groups)
				return;
			auto constants = a_frame.constants;
			constants.stagingIndex = a_recording.Index(a_bindings.staging);
			constants.ranksIndex = a_recording.Index(a_bindings.ranks);
			constants.offsetsIndex = a_recording.Index(a_bindings.offsets);
			constants.countIndex = a_recording.Index(a_bindings.count);
			constants.sequencesIndex = a_recording.Index(a_bindings.sequences);
			auto& commands = a_recording.Commands();
			commands.BindLayout(a_frame.program->layout->GetHandle());
			commands.BindPipeline(a_frame.program->Pipeline());
			commands.PushConstants(rhi::ShaderStage::Compute, 0, 0, 0, kSortSequencesConstantWords, reinterpret_cast<const std::uint32_t*>(&constants));
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
