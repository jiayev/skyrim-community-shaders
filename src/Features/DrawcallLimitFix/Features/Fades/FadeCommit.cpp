// Fades' depth commit hook (Api::CommitHook "fade roots"): the root rows and, once a frame, FadeStateCS's inputs (the list jobs' camera,
// the engine's fade globals, the animation job's batch, the log window); the write-back's readback harvested and armed; each root's
// program from the main cull input. Core's visibility readback is armed after the hooks (CommitMainPayload).

#include <Tracy/Tracy.hpp>

#include "Features/DrawcallLimitFix/Core/Extensions/Commits.h"
#include "Features/DrawcallLimitFix/Core/Extensions/CullInputs.h"
#include "Features/DrawcallLimitFix/Core/Extensions/RootStates.h"
#include "Features/DrawcallLimitFix/Core/Extensions/TableFamilies.h"
#include "Features/DrawcallLimitFix/Core/Render/Payload/Uploads.h"
#include "Features/DrawcallLimitFix/Core/Render/Payload/Versioned.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/SceneBuffers.h"
#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Tables.h"
#include "Features/DrawcallLimitFix/Diagnostics/Draws/Events.h"
#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"
#include "Features/DrawcallLimitFix/Features/Fades/FadeScene.h"
#include "Features/DrawcallLimitFix/Features/Fades/FadeState.h"
#include "Features/DrawcallLimitFix/Features/Fades/FadeWriters.h"
#include "RenderGraph/RenderGraphRuntime.h"

#include <algorithm>
#include <numeric>

namespace DCLF::Features::Fades
{
	namespace
	{
		// Once a frame, FadeStateCS's inputs: the list jobs' camera (FadeState::FadeEye), the engine's fade globals and the
		// animation job's batch (its roots' words sent), and the log window.
		void SampleFadeInputs(const Draws::NamedVersions& a_adopted, Draws::CommitUploads& a_uploads, FadeScene& a_buffers, std::uint32_t a_rootSlots,
			FadeFrame& a_inputs, std::uint32_t& a_logBase)
		{
			a_inputs = FadeState::SampleFrame(nullptr);
			const auto eye = FadeState::FadeEye();
			std::copy_n(eye.data(), 3, a_inputs.eye);
			a_inputs.lodAdjust = eye[3];
			// The animation job's updates since the last cull: roots stamped with this frame, inputs in the frame row.
			std::vector<AnimatedFadeRoot> roots;
			AnimatedFadeInputs anim{};
			TakeAnimatedBatch(roots, anim);
			std::copy_n(anim.eye, 3, a_inputs.animEye);
			a_inputs.animLodAdjust = anim.lodAdjust;
			a_inputs.animCounter = anim.counter;
			a_inputs.animDeltaTime = anim.deltaTime;
			// Each root's updates counted in a dense array kept across frames (only the roots this batch names are touched, and
			// cleared again). The batch names root slots resolved by the animation job against the writer tables; the frame's tables
			// may be newer, so a slot counts only where they still list that node at that generation (no lookup, no hashing).
			const auto batchRoots = Extensions::RootListOf(Scene::FrameView::Get().GetTables());
			auto& counts = a_buffers.fadeAnimatedCounts;
			if (counts.size() < a_rootSlots)
				counts.resize(a_rootSlots, 0u);
			std::vector<std::uint32_t> stamped;
			if (a_buffers.fadeAnimated && anim.valid) {
				for (const auto& root : roots) {
					if (root.slot >= a_rootSlots || root.slot >= batchRoots.rows.size() || root.slot >= batchRoots.nodes.size() ||
						batchRoots.nodes[root.slot] != root.node || batchRoots.rows[root.slot].generation != root.generation)
						continue;
					if (counts[root.slot]++ == 0)
						stamped.push_back(root.slot);
				}
			}
			auto& animatedWords = a_buffers.fadeAnimatedMirror;
			if (animatedWords.size() < a_rootSlots)
				animatedWords.resize(a_rootSlots, 0u);
			for (const std::uint32_t root : stamped) {
				animatedWords[root] = FadeAnimatedWord(Scene::FrameView::Get().GetFrame(), counts[root]);
				counts[root] = 0;
			}
			std::sort(stamped.begin(), stamped.end());
			// A version not filled is sent the whole mirror.
			if (const auto& animated = a_adopted.Of(a_buffers.fadeAnimated); !a_buffers.fadeAnimatedFilled.Holds(animated)) {
				stamped.resize(std::min<std::size_t>(animatedWords.size(), animated.units));
				std::iota(stamped.begin(), stamped.end(), 0u);
				a_buffers.fadeAnimatedFilled.Filled(animated);
			}
			Draws::SendWordRuns(stamped, [&](std::uint32_t a_first, std::uint32_t a_count) {
				a_uploads(a_buffers.fadeAnimated, animatedWords.data() + a_first, std::size_t(a_count) * sizeof(std::uint32_t), std::uint64_t(a_first) * sizeof(std::uint32_t));
			});
			if (anim.varied)
				Diagnostics::Notify(Diagnostics::FadeInputsVaried{ Scene::FrameView::Get().GetFrame() });
			// FadeStateCS's log window, when an observer asks.
			a_logBase = ~0u;
			Diagnostics::Notify(Diagnostics::FadeLogWindow{ Scene::FrameView::Get().GetFrame(), Scene::FrameView::Get().GetTables(), a_inputs, a_logBase });
		}

		/**
		 * @brief The depth commit's fade uploads into the versions a_sealed (the adopted set) names: missing static rows, and once a frame
		 * the inputs (the main cull input's programs are its scene resources', sent by its commit hook).
		 */
		void UploadFadeRoots(const Draws::NamedVersions& a_sealed, const SceneTables& a_tables, std::uint32_t a_frame, const FadeFrame& a_inputs,
			std::uint32_t a_logBase, FadeScene& a_scene, Draws::CommitUploads& a_uploads)
		{
			if (!a_scene.fadeState || !a_scene.fadeRoots)
				return;
			// The roots as the root states' family lists them (its view), and its journal's runs.
			const auto list = Extensions::RootListOf(a_tables);
			const auto* family = Draws::RootFamilyOf(a_tables);
			const std::uint64_t current = list.contentEpoch;
			const auto& roots = a_sealed.Of(a_scene.fadeRoots);
			const std::uint64_t contentEpoch = a_scene.fadeRootsFilled.Since(roots).value_or(~0ull);
			if (contentEpoch != current && list.rows.size() <= roots.units) {
				const auto* rows = list.rows.data();
				if (family)
					family->Journal().Take().ForEachRun(contentEpoch, list.rows.size(), [&](std::uint64_t a_first, std::uint64_t a_count) {
						a_uploads(a_scene.fadeRoots, rows + a_first, static_cast<std::size_t>(a_count * sizeof(FadeRootStatic)), a_first * sizeof(FadeRootStatic));
					});
				a_scene.fadeRootsFilled.Filled(roots, current);
			}
			a_scene.fadeRootCount = a_scene.FadeRootsContentEpoch(a_sealed) == current ? static_cast<std::uint32_t>(list.rows.size()) : 0u;
			if (a_scene.fadeFrameNumber != a_frame) {
				a_scene.fadeFrameNumber = a_frame;
				a_scene.fadeFrame = a_inputs;
				a_scene.fadeLogBase = a_logBase;
			}
			a_scene.fadeFrame.rootCount = a_scene.fadeRootCount;
			a_scene.fadeFrame.sceneFrame = a_scene.fadeFrameNumber;
			a_scene.fadeFrame.logBase = a_scene.fadeLogBase;
			a_uploads(a_scene.fadeFrameBuffer, &a_scene.fadeFrame, sizeof(FadeFrame), 0);
			// The write-back's header is ArmFadeReadback's.
		}

		// The roots UploadFadeRootPrograms judges again, in its order: incrementally, those written since the mirror's content epoch
		// (the root states' family's journal, below the mirror's size) and the new ones; else every root.
		void CollectRootsToJudge(const Extensions::TableFamilyColumns* a_family, bool a_incremental, std::uint64_t a_judgedEpoch, std::size_t a_before,
			std::size_t a_rootCount, std::vector<std::uint32_t>& a_roots)
		{
			a_roots.clear();
			if (a_incremental) {
				if (a_family)
					a_family->Journal().Take().ForEachRun(a_judgedEpoch, a_before, [&](std::uint64_t a_first, std::uint64_t a_count) {
						for (std::uint64_t r = a_first; r < a_first + a_count && r < a_before; ++r)
							a_roots.push_back(static_cast<std::uint32_t>(r));
					});
				for (std::size_t r = a_before; r < a_rootCount; ++r)
					a_roots.push_back(static_cast<std::uint32_t>(r));
			} else {
				for (std::size_t r = 0; r < a_rootCount; ++r)
					a_roots.push_back(static_cast<std::uint32_t>(r));
			}
		}

		// The batch's programs from the main cull input's judge, called once: a root without a node takes noProgram unasked.
		void JudgeRootPrograms(const Extensions::MainCullInputEntry& a_input, std::span<const void* const> a_nodes, FadeScene::RootProgramsBatch& a_batch)
		{
			a_batch.asked.clear();
			a_batch.programs.assign(a_batch.roots.size(), a_input.contribution.noProgram);
			for (std::size_t k = 0; k < a_batch.roots.size(); ++k)
				if (a_nodes[a_batch.roots[k]])
					a_batch.asked.push_back(static_cast<std::uint32_t>(k));
			if (a_batch.asked.empty())
				return;
			Api::RootProgramsJudge judge{ a_nodes, a_batch.roots, a_batch.asked, a_batch.programs };
			a_input.contribution.judge(judge);
		}

		// Each root's program (the main cull input's: Features/Portals' room programs), when the roots or the input's answers
		// (programsVersion) changed. With the input unchanged and the version holding the mirror, only the roots written since the
		// mirror's content epoch (the roots' journal) and the new ones are judged again; an input change, or a version the mirror is not
		// of, judges every root. Without an input FadeStateCS and the builds bind no programs, so the roots' are not read.
		void UploadFadeRootPrograms(const Draws::NamedVersions& a_adopted, Draws::CommitUploads& a_uploads, FadeScene& a_buffers)
		{
			const auto* input = Extensions::MainCullInput();
			if (!input)
				return;
			const auto& rootTables = Scene::FrameView::Get().GetTables();
			const auto rootList = Extensions::RootListOf(rootTables);
			const std::uint64_t rootsEpoch = rootList.contentEpoch, inputVersion = input->contribution.programsVersion();
			const std::uint64_t programsKey = rootsEpoch * 0x9E3779B97F4A7C15ull ^ inputVersion;
			const auto& programsVersion = a_adopted.Of(a_buffers.fadeRootPrograms);
			const auto programsFilledKey = a_buffers.fadeRootProgramsFilled.Since(programsVersion);
			const std::size_t rootCount = rootList.nodes.size();
			if (programsFilledKey != programsKey && rootCount <= programsVersion.units) {
				auto& sent = a_buffers.fadeRootProgramsMirror;
				const auto judged = a_buffers.fadeRootProgramsMirrorOf;
				const bool incremental = programsFilledKey && *programsFilledKey == judged.key && judged.programs == inputVersion && sent.size() <= rootCount;
				if (!programsFilledKey)
					sent.clear();
				if (!incremental)
					sent.resize(std::min(sent.size(), rootCount));
				auto& batch = a_buffers.fadeRootProgramsBatch;
				CollectRootsToJudge(Draws::RootFamilyOf(rootTables), incremental, judged.rootsEpoch, sent.size(), rootCount, batch.roots);
				JudgeRootPrograms(*input, rootList.nodes, batch);
				// Only words that differ (all of them for a version not filled); the mirror takes the new words.
				std::vector<std::uint32_t> differing;
				for (std::size_t k = 0; k < batch.roots.size(); ++k) {
					const std::size_t root = batch.roots[k];
					const std::uint32_t program = batch.programs[k];
					if (root < sent.size() && sent[root] == program)
						continue;
					if (root >= sent.size())
						sent.resize(root + 1, ~program);
					sent[root] = program;
					differing.push_back(static_cast<std::uint32_t>(root));
				}
				if (incremental) {
					std::sort(differing.begin(), differing.end());
					differing.erase(std::unique(differing.begin(), differing.end()), differing.end());
				}
				Draws::SendWordRuns(differing, [&](std::uint32_t a_first, std::uint32_t a_count) {
					a_uploads(a_buffers.fadeRootPrograms, sent.data() + a_first, std::size_t(a_count) * sizeof(std::uint32_t), std::uint64_t(a_first) * sizeof(std::uint32_t));
				});
				a_buffers.fadeRootProgramsFilled.Filled(programsVersion, programsKey);
				a_buffers.fadeRootProgramsMirrorOf = { programsKey, rootsEpoch, inputVersion };
			}
		}

		/** @brief The depth commit: harvests the frame slot's last fade events for the write-back task, resets the list, arms the slot. */
		void ArmFadeReadback(FadeScene& a_scene, Draws::CommitUploads& a_uploads)
		{
			if (!a_scene.fadeWriteBack || !a_scene.fadeEvents)
				return;
			auto& writeBack = *a_scene.fadeWriteBack;
			auto* host = RenderGraphRuntime::Get().Host();
			if (!host)
				return;
			const std::uint32_t frameSlot = host->CurrentFrameSlot();
			if (frameSlot >= writeBack.armed.size())
				return;
			// Harvests the slot's last execution (the host waited for it) if its buffer holds the frame that armed it. If the epoch did
			// not run for that frame, FadeStateCS did not report either: the next execution reports those changes. Roots past the
			// list's end report again next frame while the list grows (RequireSceneTables).
			if (auto& armed = writeBack.armed[frameSlot]; armed.armed) {
				armed.armed = false;
				writeBack.Harvest(frameSlot, armed.frame, [&](const std::uint32_t* a_words, std::uint32_t a_count, std::uint32_t a_capacity) {
					if (const std::uint32_t events = std::min(a_count, a_capacity)) {
						auto* batch = new FadeWriteBack::Batch();
						batch->frame = armed.frame;
						const auto* list = reinterpret_cast<const FadeEvent*>(a_words + kFadeEventHeaderWords);
						batch->events.assign(list, list + events);
						writeBack.Push(batch);
						writeBack.harvestedWithEvents.fetch_add(1, std::memory_order_relaxed);
						writeBack.events.fetch_add(events, std::memory_order_relaxed);
					}
					writeBack.harvested.fetch_add(1, std::memory_order_relaxed);
				});
			}
			// The header FadeStateCS appends after: the count and the frame the harvest matches.
			const std::uint32_t header[kFadeEventHeaderWords]{ 0u, Scene::FrameView::Get().GetFrame(), 0u, 0u };
			a_uploads(a_scene.fadeEvents, header, sizeof(header), 0);
			auto& armed = writeBack.armed[frameSlot];
			armed.armed = true;
			armed.frame = Scene::FrameView::Get().GetFrame();
		}

		// The depth commit only: fade roots and, once a frame, their inputs for FadeStateCS; the write-back armed; the roots' cull
		// programs. Core's visibility readback is armed after the hooks (CommitMainPayload).
		void CommitFadeRoots(const Api::CommitContext& a_commit)
		{
			if (a_commit.kind != Api::CommitKind::Depth)
				return;
			auto* fades = SceneOf(a_commit.scene);
			if (!fades || !fades->fadeState)
				return;
			const auto& adopted = a_commit.adopted;
			auto& uploads = a_commit.uploads;
			auto& buffers = *fades;
			const auto rootSlots = static_cast<std::uint32_t>(adopted.Of(buffers.fadeRoots).units);
			std::uint32_t logBase = buffers.fadeLogBase;
			FadeFrame inputs = buffers.fadeFrame;
			if (buffers.fadeFrameNumber != a_commit.frame)
				SampleFadeInputs(adopted, uploads, buffers, rootSlots, inputs, logBase);
			UploadFadeRoots(adopted, a_commit.tables, a_commit.frame, inputs, logBase, buffers, uploads);
			// Query programs and scratch must describe the same admitted publication. Diagnose a growth race explicitly;
			// the shader emits an invalid verdict rather than indexing past the bucket buffers or taking a frustum fallback.
			if (buffers.portalBatch)
				Portals::CheckMainBatchCapacity(*buffers.portalBatch, a_commit.frame,
					std::min(adopted.Of(buffers.portalBatch->counts).units, adopted.Of(buffers.portalBatch->offsets).units) / 2);

			Extensions::TableFamilySent(Extensions::RootStatesFamily(), buffers.FadeRootsContentEpoch(adopted));
			// The fade events' header and readback slot (its last execution harvested first).
			ArmFadeReadback(buffers, uploads);
			UploadFadeRootPrograms(adopted, uploads, buffers);
		}
	}

	void RegisterCommit(Api::InstallContext& a_context)
	{
		// The depth commit's rows, inputs, write-back and root programs, after every feature's hook (Portals' programs among them).
		a_context.RegisterCommitHook({ .name = "fade roots", .commit = &CommitFadeRoots });
	}
}
