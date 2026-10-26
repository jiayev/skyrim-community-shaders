// Fades' CPU stage (FadeScene.h, RegisterCpuStage; dclf-architecture.md, "The CPU-driven pipeline"): FadeStateCS on the CPU, as a frame-value
// pool ("fade-states-cpu", after the seeds' pool, so its sample sees this producer's seeds). Its kick (render thread, in FrameValues::Kick)
// takes Core's arm (Direct::TakeCpuFrameArm) and the frame's inputs: the camera's planes and fade globals, the animation job's batch, each
// root's portal program (the main cull input's judge, kept in a mirror judged again only where the roots or the input moved) and the
// programs' words. Its sample (the producer, right after the placements' sampling: Direct::SampledRows) steps every listed root as
// FadeStateCS's main does (seed merge, reseed, the animated updates, the cull test through the root's program, OnVisible: FadeState's
// port), reports the write-back's milestones (ReportMilestone), and publishes the frame's Direct::CullFrame. Its send puts the states
// into the pool's backing: the fade states the frame's draws (t120) and culls (RootStates) bind while the frame is the CPU's.
//
// States are this frame's, not the frame before's (FadeStateCS publishes a frame ahead): the stage runs before the frame's culls.
// The camera test without a program is the engine's (Process1's sphere against the camera's culling planes, ShadowCameras::FrustumPlanes),
// not FadeStateCS's view-projection planes: the two agree but for rounding at the planes.

#include "Features/DrawcallLimitFix/Features/Fades/FadeScene.h"
#include "Features/DrawcallLimitFix/Features/Fades/FadeState.h"
#include "Features/DrawcallLimitFix/Features/Fades/FadeWriters.h"
#include "Features/DrawcallLimitFix/Features/Portals/PortalPrograms.h"
#include "Features/DrawcallLimitFix/Features/Portals/PortalProgramsCpu.h"

#include "Features/DrawcallLimitFix/Core/Extensions/CullInputs.h"
#include "Features/DrawcallLimitFix/Core/Extensions/FrameValuePools.h"
#include "Features/DrawcallLimitFix/Core/Extensions/RootStates.h"
#include "Features/DrawcallLimitFix/Core/Extensions/TableFamilies.h"
#include "Features/DrawcallLimitFix/Core/Gpu/FrameValues.h"
#include "Features/DrawcallLimitFix/Core/Render/Direct/Cull.h"
#include "Features/DrawcallLimitFix/Core/Render/IndirectDraws.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/SceneBuffers.h"
#include "Features/DrawcallLimitFix/Core/Render/Shadow/ShadowCameras.h"
#include "Features/DrawcallLimitFix/Core/Runtime/KeptState.h"
#include "Features/DrawcallLimitFix/Core/Runtime/SceneScheduler.h"
#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Tables.h"
#include "Features/DrawcallLimitFix/Core/Shading/LightingConstants.h"

#include <Tracy/Tracy.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

namespace DCLF::Features::Fades
{
	namespace
	{
		Api::FrameValuePoolId cpuStatesPool;  // install
		constexpr std::uint32_t kNoObject = ~0u;  // a free root slot's object (FadeStateCS's kNoObject)

		/** @brief What a CPU frame's kick hands its producer (held by the frame's job; no engine reference in it). */
		struct CpuStageInput
		{
			std::shared_ptr<const Direct::ViewBatchSet> set;
			std::uint32_t sceneFrame = 0;
			bool visibilityReset = false;
			std::shared_ptr<const SceneTables> tables;
			std::shared_ptr<const EntrySpace> candidates;
			std::shared_ptr<FadeScene> scene;  // the write-back's ring
			FadeFrame frame{};
			ShadowCameras::Planes camera;  // the main camera's culling planes (none active: no camera)
			std::array<float, 4> fadeEye{};
			std::array<float, 2> treeHeight{};
			std::vector<std::pair<std::uint32_t, std::uint32_t>> animated;  // (root slot, updates) of the animation job's batch
			std::shared_ptr<const std::vector<std::uint32_t>> rootPrograms;  // by root slot
			std::vector<std::uint32_t> programs;  // the main cull input's words, this frame's walk
		};

		// Render thread: the roots' programs as last judged (FadeCommit.cpp's UploadFadeRootPrograms, with a mirror of its own), shared with
		// the frames that read it (a new vector only when an answer changed), and the rows the next producer may leave.
		struct ProgramsMirror
		{
			std::shared_ptr<const std::vector<std::uint32_t>> programs = std::make_shared<std::vector<std::uint32_t>>();
			std::uint64_t key = 0, rootsEpoch = 0, inputVersion = 0;
			bool judged = false;
			std::vector<std::uint32_t> roots, asked, answers;  // the judge's batch (kept warm)
		};
		ProgramsMirror programsMirror;
		std::uint64_t stateRows = 0;
		std::shared_ptr<const CpuStageInput> idleInput = std::make_shared<CpuStageInput>();

		// The producers' (one at a time): the states by root slot (the pool's rows), each root's last reported generation and milestone
		// (FadeStateCS's fade-reported), and the published frames' storage, recycled once no cull holds it.
		KeptWholeArray<FadeNodeState> cpuStates;
		std::vector<std::pair<std::uint32_t, std::uint32_t>> reported;

		/** @brief The spans a CullFrame names (Direct::CullFrame::storage), reused frame after frame. */
		struct FrameStorage
		{
			std::vector<std::array<float, 4>> sunEntry, fadeNode;
			std::vector<std::uint32_t> rootWords, rootGenerations, rootPrograms, programs;
			std::vector<FadeNodeState> rootStates;
			FrameStorage* next = nullptr;
		};
		std::atomic<FrameStorage*> freeStorage{ nullptr };

		FrameStorage* TakeStorage()
		{
			FrameStorage* storage = freeStorage.load(std::memory_order_acquire);
			while (storage && !freeStorage.compare_exchange_weak(storage, storage->next, std::memory_order_acq_rel, std::memory_order_acquire)) {}
			return storage ? storage : new FrameStorage();
		}
		/** @brief The last cull's release of a frame's storage (any thread): back to the free stack, for a later frame. */
		void GiveBackStorage(FrameStorage* a_storage)
		{
			a_storage->next = freeStorage.load(std::memory_order_relaxed);
			while (!freeStorage.compare_exchange_weak(a_storage->next, a_storage, std::memory_order_release, std::memory_order_relaxed)) {}
		}

		// ---- Kick (render thread) ----

		/** @brief The roots the judge answers again: those the roots' journal wrote since the last judging and the new ones; else every root. */
		void CollectRoots(const SceneTables& a_tables, bool a_incremental, std::size_t a_before, std::size_t a_count, std::vector<std::uint32_t>& a_roots)
		{
			a_roots.clear();
			if (a_incremental) {
				if (const auto* family = Draws::RootFamilyOf(a_tables))
					family->Journal().Take().ForEachRun(programsMirror.rootsEpoch, a_before, [&](std::uint64_t a_first, std::uint64_t a_runs) {
						for (std::uint64_t r = a_first; r < a_first + a_runs && r < a_before; ++r)
							a_roots.push_back(static_cast<std::uint32_t>(r));
					});
				for (std::size_t r = a_before; r < a_count; ++r)
					a_roots.push_back(static_cast<std::uint32_t>(r));
				return;
			}
			for (std::size_t r = 0; r < a_count; ++r)
				a_roots.push_back(static_cast<std::uint32_t>(r));
		}

		/** @brief Each root's program, judged again when the roots or the input's answers moved (only the roots that can have changed). */
		std::shared_ptr<const std::vector<std::uint32_t>> RootPrograms(const SceneTables& a_tables)
		{
			auto& m = programsMirror;
			const auto* input = Extensions::MainCullInput();
			if (!input || !input->contribution.judge || !input->contribution.programsVersion)
				return m.programs;
			const auto list = Extensions::RootListOf(a_tables);
			const std::uint64_t inputVersion = input->contribution.programsVersion();
			const std::uint64_t key = list.contentEpoch * 0x9E3779B97F4A7C15ull ^ inputVersion;
			if (m.judged && m.key == key)
				return m.programs;
			const std::size_t count = list.nodes.size();
			auto programs = std::make_shared<std::vector<std::uint32_t>>(*m.programs);
			const bool incremental = m.judged && m.inputVersion == inputVersion && programs->size() <= count;
			if (!incremental)
				programs->clear();
			CollectRoots(a_tables, incremental, programs->size(), count, m.roots);
			m.asked.clear();
			m.answers.assign(m.roots.size(), input->contribution.noProgram);
			for (std::size_t k = 0; k < m.roots.size(); ++k)
				if (list.nodes[m.roots[k]])
					m.asked.push_back(static_cast<std::uint32_t>(k));
			if (!m.asked.empty()) {
				Api::RootProgramsJudge judge{ list.nodes, m.roots, m.asked, m.answers };
				input->contribution.judge(judge);
			}
			programs->resize(count, input->contribution.noProgram);
			for (std::size_t k = 0; k < m.roots.size(); ++k)
				(*programs)[m.roots[k]] = m.answers[k];
			m.programs = std::move(programs);
			m.key = key;
			m.rootsEpoch = list.contentEpoch;
			m.inputVersion = inputVersion;
			m.judged = true;
			return m.programs;
		}

		/** @brief The animation job's batch, as FadeCommit.cpp's SampleFadeInputs counts it: a root only where the tables still list that node. */
		void TakeAnimated(const Api::RootListView& a_list, CpuStageInput& a_input)
		{
			std::vector<AnimatedFadeRoot> roots;
			AnimatedFadeInputs anim{};
			DrainAnimatedFades();
			TakeAnimatedBatch(roots, anim);
			std::copy_n(anim.eye, 3, a_input.frame.animEye);
			a_input.frame.animLodAdjust = anim.lodAdjust;
			a_input.frame.animCounter = anim.counter;
			a_input.frame.animDeltaTime = anim.deltaTime;
			if (!anim.valid)
				return;
			std::sort(roots.begin(), roots.end(), [](const auto& a_a, const auto& a_b) { return a_a.slot < a_b.slot; });
			for (const auto& root : roots) {
				if (root.slot >= a_list.rows.size() || root.slot >= a_list.nodes.size() || a_list.nodes[root.slot] != root.node ||
					a_list.rows[root.slot].generation != root.generation)
					continue;
				if (!a_input.animated.empty() && a_input.animated.back().first == root.slot)
					a_input.animated.back().second = std::min(a_input.animated.back().second + 1, kFadeAnimatedCountMask);
				else
					a_input.animated.push_back({ root.slot, 1u });
			}
		}

		Api::FrameValueInput CpuStatesKick(const Api::FrameValuePoolKick&)
		{
			auto arm = Direct::TakeCpuFrameArm();
			const auto& view = Scene::FrameView::Get();
			auto tables = view.AcceptedTables();
			const auto* scene = IndirectDraws::Get().Scene();
			if (!arm.set || !tables || !scene)
				return { idleInput, stateRows };
			auto input = std::make_shared<CpuStageInput>();
			input->set = std::move(arm.set);
			input->sceneFrame = arm.sceneFrame;
			input->visibilityReset = arm.visibilityReset;
			input->candidates = view.GetSunCandidates();
			input->scene = SharedSceneOf(*scene);
			// The frame's inputs as the depth commit samples them for FadeStateCS (SampleFadeInputs), and the camera's culling planes.
			input->frame = FadeState::SampleFrame(nullptr);
			input->fadeEye = FadeState::FadeEye();
			input->treeHeight = FadeState::TreeHeightTest();
			std::copy_n(input->fadeEye.data(), 3, input->frame.eye);
			input->frame.lodAdjust = input->fadeEye[3];
			if (const auto* camera = FadeState::MainCamera()) {
				ShadowCameras::Camera matrices;
				ShadowCameras::CameraMatrices(*camera, matrices);
				input->camera = matrices.frustum;
			}
			const auto list = Extensions::RootListOf(*tables);
			TakeAnimated(list, *input);
			input->rootPrograms = RootPrograms(*tables);
			if (const auto* cull = Extensions::MainCullInput(); cull && cull->contribution.programWords) {
				const auto words = cull->contribution.programWords();
				input->programs.assign(words.begin(), words.end());
			}
			input->tables = std::move(tables);
			stateRows = std::max<std::uint64_t>(stateRows, list.rows.size());
			return { std::move(input), stateRows };
		}

		// ---- Sample (the producer) ----

		/** @brief FadeStateCS's FadeMilestone: faded out, fading or faded in, with the leaf LOD level and transition. */
		std::uint32_t Milestone(const FadeNodeState& a_state)
		{
			const std::uint32_t fade = a_state.currentFade <= 0.0f ? 0u : (a_state.currentFade >= 1.0f ? 2u : 1u);
			return fade | ((a_state.levels & 0xFu) << 2) | (((a_state.levels >> 12) & 0x7u) << 6);
		}

		/** @brief The cull test without a program (FrustumInView): always-draw passes untested, else the camera's planes. */
		bool CameraInView(const CpuStageInput& a_input, std::uint32_t a_bits, const float a_centre[3], float a_radius)
		{
			if (a_bits & kFadeRootAlwaysDraw)
				return true;
			const auto& planes = a_input.camera;
			for (std::uint32_t p = 0; p < 6; ++p) {
				if (!(planes.mask & (1u << p)))
					continue;
				const auto& n = planes.plane[p];
				if (n[0] * a_centre[0] + n[1] * a_centre[1] + n[2] * a_centre[2] - n[3] < -a_radius)
					return false;
			}
			return true;
		}

		/** @brief The root's cull test (the fade query: PreparePortalQueries' choice, then PortalTestAt through its program). */
		bool RootInView(const CpuStageInput& a_input, std::uint32_t a_program, const FadeRootStatic& a_root, const float a_centre[3], float a_radius)
		{
			const std::uint32_t count = a_input.programs.size() > kPortalSetBytes / 4 ? a_input.programs[kPortalSetBytes / 4] : 0u;
			if (a_program >= count || (a_root.bits & kFadeRootPreprocessed))
				return CameraInView(a_input, a_root.bits, a_centre, a_radius);
			const std::uint32_t flags = ((a_root.bits & kFadeRootAlwaysDraw) ? 1u << 11 : 0u) | ((a_root.bits & kFadeRootPreprocessed) ? 1u << 12 : 0u) |
			                            ((a_root.bits & kFadeRootPreprocessHidden) ? 1u << 20 : 0u);
			return ::DCLF::Portals::PortalTestCpu(a_input.programs, a_program, a_centre, a_radius, flags) == 1;
		}

		/** @brief The verdicts a root's members' culls read (Direct::RootWord), from the state they take. */
		std::uint32_t RootWordOf(const FadeNodeState& a_effective, const FadeNodeState& a_state, const FadeRootStatic& a_row, std::uint32_t a_sceneFrame)
		{
			std::uint32_t word = Direct::kRootListed;
			if (a_effective.currentFade < 1.0f)
				word |= Direct::kRootFading;
			// FadedOutOfOcclusion: an unsettled root recurses only while its fade is above 0 and its fadeAmount is not 0.
			const bool settledIn = (a_effective.flags & kFadeFlagSettled) && a_row.fadeAmount == 1.0f && a_effective.currentFade == 1.0f;
			if (!settledIn && (!(a_effective.currentFade > 0.0f) || a_row.fadeAmount == 0.0f))
				word |= Direct::kRootFadedOutOfOcclusion;
			if (a_state.generation == a_row.generation) {
				word |= (a_state.verdict & kFadeVerdictServiced) ? Direct::kRootServiced : 0u;
				word |= (a_state.verdict & kFadeVerdictDrawn) ? Direct::kRootDrawn : 0u;
				word |= (a_state.verdict & kFadeVerdictInView) ? Direct::kRootInView : 0u;
				word |= a_state.frame == a_sceneFrame ? Direct::kRootUpdated : 0u;
			}
			const std::uint32_t level = std::min(a_effective.levels & 0xFu, 3u);
			word |= level << Direct::kRootLodShift;
			if (((a_effective.levels >> 8) & 0x70u) != 0x20u && level < 3 && static_cast<int>(a_effective.blend * 31.0f) > 0)
				word |= Direct::kRootLodCopy;
			return word;
		}

		/** @brief The frame's producer state a root's step reads; events and marked are its chunk's own (StepRoots). */
		struct StepContext
		{
			const CpuStageInput& input;
			const Api::RootListView& list;
			const std::vector<FadeRootStatic>& seeds;
			FrameStorage& storage;
			std::vector<FadeNodeState>& states;
			std::vector<FadeEvent>& events;
			std::vector<std::uint32_t>& marked;  // the slots whose state changed, for cpuStates.Mark (not thread-safe)
			std::size_t animatedAt = 0;
		};

		/** @brief ReportMilestone: a root DCLF writes whose milestone moved appends an event (the list never overflows on the CPU). */
		void Report(StepContext& a_context, std::uint32_t a_root, const FadeRootStatic& a_row, const FadeNodeState& a_state)
		{
			auto& last = reported[a_root];
			if (last.first != a_row.generation)
				last = { a_row.generation, Milestone(a_row.initial) };
			const std::uint32_t milestone = Milestone(a_state);
			if ((a_row.bits & kFadeRootWritten) && last.second != milestone) {
				FadeEvent event;
				event.root = a_root;
				event.generation = a_row.generation;
				event.flags = a_state.flags;
				event.currentFade = a_state.currentFade;
				event.levels = a_state.levels;
				event.blend = a_state.blend;
				a_context.events.push_back(event);
			}
			last = { a_row.generation, milestone };
		}

		/** @brief FadeStateCS's main for one root slot: its update (once a frame, with a camera), its event, and the words its members read. */
		void StepRoot(StepContext& a_context, std::uint32_t a_slot)
		{
			const auto& input = a_context.input;
			const FadeRootStatic& row = a_context.list.rows[a_slot];
			FadeNodeState& state = a_context.states[a_slot];
			auto& storage = a_context.storage;
			storage.rootGenerations[a_slot] = row.generation;
			const bool listed = row.object != kNoObject && row.generation != 0;
			FadeRootStatic root = row;
			const bool merged = listed && MergeFadeSeed(row, a_context.seeds.data(), a_context.seeds.size(), a_slot, root);
			if (merged) {
				if (state.generation != root.generation) {
					state = root.initial;
					state.generation = root.generation;
					state.verdict = 0;
					state.frame = 0;
				}
				if (state.frame != input.sceneFrame && input.frame.lodAdjust != 0.0f && root.object < storage.fadeNode.size()) {
					const float* centre = storage.fadeNode[root.object].data();
					// The bound's radius as the engine reads it now: the sun entry row's, else the listed radius.
					const float entryRadius = storage.sunEntry[root.object][3];
					const float radius = entryRadius >= 0.0f && entryRadius < 1e30f ? entryRadius : root.radius;
					state.verdict = 0;
					std::uint32_t updates = 0;
					auto& at = a_context.animatedAt;
					while (at < input.animated.size() && input.animated[at].first < a_slot)
						++at;
					if (at < input.animated.size() && input.animated[at].first == a_slot)
						updates = input.animated[at].second;
					if (updates)
						FadeState::AnimatedUpdate(state, root, centre, input.frame, updates);
					const std::uint32_t program = a_slot < input.rootPrograms->size() ? (*input.rootPrograms)[a_slot] : kPortalNoProgram;
					if (RootInView(input, program, root, centre, radius))
						state.verdict = FadeState::OnVisible(state, root, centre, input.frame);
					if (updates)
						state.verdict |= kFadeVerdictAnimated | (updates << 5);
					state.frame = input.sceneFrame;
					a_context.marked.push_back(a_slot);
					Report(a_context, a_slot, root, state);
				}
			}
			// The state the members' readers take (BuildDrawsCS's SeedState rule): the root's own, else its seed's, else its row's.
			const FadeNodeState& effective = state.generation == row.generation ? state : (merged ? root.initial : row.initial);
			storage.rootStates[a_slot] = effective;
			storage.rootWords[a_slot] = listed ? RootWordOf(effective, state, row, input.sceneFrame) : 0u;
		}

		/** @brief The placements' rows the culls read (sun entry, fade node), copied out of the producer's rows (the next producer rewrites them). */
		void CopyRows(std::span<const BindlessPlacement> a_rows, FrameStorage& a_storage)
		{
			a_storage.sunEntry.resize(a_rows.size());
			a_storage.fadeNode.resize(a_rows.size());
			for (std::size_t o = 0; o < a_rows.size(); ++o) {
				std::memcpy(a_storage.sunEntry[o].data(), a_rows[o].sunEntry, sizeof(float) * 4);
				std::memcpy(a_storage.fadeNode[o].data(), a_rows[o].lodFadeNode, sizeof(float) * 4);
			}
		}

		/** @brief The frame's CullFrame over a_storage (its owner: given back to the free stack when the last cull lets go). */
		std::shared_ptr<Direct::CullFrame> MakeCullFrame(const CpuStageInput& a_input, std::uint64_t a_seq, FrameStorage* a_storage)
		{
			auto frame = std::make_shared<Direct::CullFrame>();
			auto& s = *a_storage;
			frame->sceneFrame = a_input.sceneFrame;
			frame->stamp = a_input.sceneFrame & 0x0FFFFFFFu;
			frame->frameValuesSeq = a_seq;
			frame->set = a_input.set;
			frame->sunEntry = s.sunEntry;
			frame->fadeNode = s.fadeNode;
			frame->rootWords = s.rootWords;
			frame->rootStates = s.rootStates;
			frame->rootGenerations = s.rootGenerations;
			frame->rootPrograms = s.rootPrograms;
			frame->programs = s.programs;
			frame->portalTest = &::DCLF::Portals::PortalTestCpu;
			frame->portalTestBatch = &::DCLF::Portals::PortalTestCpuBatch;
			frame->fadeEye = a_input.fadeEye;
			frame->treeHeight = a_input.treeHeight;
			frame->tables = a_input.tables;
			frame->candidates = a_input.candidates;
			frame->visibilityReset = a_input.visibilityReset;
			frame->storage = std::shared_ptr<const void>(a_storage, [](const void* a_owned) { GiveBackStorage(static_cast<FrameStorage*>(const_cast<void*>(a_owned))); });
			return frame;
		}

		void CpuStatesSample(Api::FrameValuePoolProducer& a_producer)
		{
			const auto* input = a_producer.Input<CpuStageInput>();
			if (!input || !input->set || !input->tables)
				return;
			ZoneScopedN("CS.DCLF.FadeCpuStage");
			a_producer.BeginBuild(cpuStates);
			const auto list = Extensions::RootListOf(*input->tables);
			const std::size_t roots = list.rows.size();
			auto& states = cpuStates.Mutable();
			if (states.size() < roots) {
				const std::size_t first = states.size();
				states.resize(roots, FadeNodeState{});
				cpuStates.MarkRange(first, roots - first);
			}
			if (reported.size() < roots)
				reported.resize(roots, { 0u, 0u });
			auto* storage = TakeStorage();
			CopyRows(Direct::SampledRows(a_producer.seq), *storage);
			storage->rootWords.assign(roots, 0u);
			storage->rootGenerations.assign(roots, 0u);
			storage->rootStates.assign(roots, FadeNodeState{});
			// The members' programs: a root's past this frame's programs takes the frustum alone.
			const std::uint32_t count = input->programs.size() > kPortalSetBytes / 4 ? input->programs[kPortalSetBytes / 4] : 0u;
			storage->rootPrograms.assign(roots, kPortalNoProgram);
			for (std::size_t r = 0; r < roots && r < input->rootPrograms->size(); ++r)
				if ((*input->rootPrograms)[r] < count)
					storage->rootPrograms[r] = (*input->rootPrograms)[r];
			storage->programs = input->programs;
			// The roots in chunks on the pool (a root's step touches its own slot alone; the culls wait for this stage): each chunk's
			// events and marks kept apart, then taken in root order, as one pass would make them.
			constexpr std::size_t kRootGrain = 1024;
			const std::size_t chunks = (roots + kRootGrain - 1) / kRootGrain;
			std::vector<std::vector<FadeEvent>> chunkEvents(chunks);
			std::vector<std::vector<std::uint32_t>> chunkMarked(chunks);
			const auto& seeds = ProducerSeeds();
			SceneScheduler::Executor().ParallelFor("CS.DCLF.FadeCpuStage.Roots", roots, kRootGrain, [&](std::size_t a_begin, std::size_t a_end) {
				const std::size_t chunk = a_begin / kRootGrain;
				StepContext context{ *input, list, seeds, *storage, states, chunkEvents[chunk], chunkMarked[chunk] };
				context.animatedAt = static_cast<std::size_t>(std::lower_bound(input->animated.begin(), input->animated.end(), a_begin,
																	[](const auto& a_entry, std::size_t a_slot) { return a_entry.first < a_slot; }) -
															  input->animated.begin());
				for (std::size_t r = a_begin; r < a_end; ++r)
					StepRoot(context, static_cast<std::uint32_t>(r));
			});
			std::vector<FadeEvent> events;
			for (std::size_t c = 0; c < chunks; ++c) {
				events.insert(events.end(), chunkEvents[c].begin(), chunkEvents[c].end());
				for (const std::uint32_t slot : chunkMarked[c])
					cpuStates.Mark(slot);
			}
			if (input->scene && input->scene->fadeWriteBack)
				PostCpuFadeEvents(*input->scene->fadeWriteBack, input->sceneFrame, std::move(events));
			Direct::PublishCullFrame(MakeCullFrame(*input, a_producer.seq, storage));
		}

		void CpuStatesSend(Api::FrameValuePoolProducer& a_producer)
		{
			a_producer.Send(cpuStates);
		}
	}

	void RegisterCpuStage(Api::InstallContext& a_context)
	{
		// After the seeds' pool: its sample reads the seeds this producer made (ProducerSeeds).
		cpuStatesPool = a_context.RegisterFrameValuePool({ .name = "fade-states-cpu", .stride = sizeof(FadeNodeState), .kick = &CpuStatesKick,
			.sample = &CpuStatesSample, .send = &CpuStatesSend });
	}

	std::uint32_t CpuStatesIndex()
	{
		return FrameValues::Get().PoolIndex(cpuStatesPool);
	}
}
