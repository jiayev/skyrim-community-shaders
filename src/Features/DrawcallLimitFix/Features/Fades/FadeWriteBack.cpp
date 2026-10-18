// Fades' write-back (dclf-architecture.md, "The fade write-back"): the events the depth commit harvests (FadeCommit.cpp) judged on DCLF's
// executor by the root states' kick (KickFadeWriteBack), joined by its wait, and the stores made on the main thread at Main::Update's
// entry (WriteFadeStores).

#include "Features/DrawcallLimitFix/Features/Fades/FadeScene.h"

#include "Features/DrawcallLimitFix/Core/Engine/Capture/SceneCapture.h"
#include "Features/DrawcallLimitFix/Core/Engine/Capture/SceneTracker.h"
#include "Features/DrawcallLimitFix/Core/Extensions/RootStates.h"
#include "Features/DrawcallLimitFix/Core/Render/IndirectDraws.h"
#include "Features/DrawcallLimitFix/Core/Runtime/SceneExecutor.h"
#include "Features/DrawcallLimitFix/Core/Runtime/SceneScheduler.h"
#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Tables.h"

#include <chrono>
#include <memory>
#include <vector>

namespace DCLF::Features::Fades
{
	namespace
	{
		/** @brief Fades' scene resources in the scene's tables IndirectDraws keeps (null: none made yet, or no fade program). */
		FadeScene* CurrentScene()
		{
			const auto* scene = IndirectDraws::Get().Scene();
			return scene ? SceneOf(*scene) : nullptr;
		}
	}

	void WriteFadeStores(bool a_live, const std::function<bool(const void*)>& a_written)
	{
		const auto* fades = CurrentScene();
		if (!fades || !fades->fadeWriteBack)
			return;
		auto& writeBack = *fades->fadeWriteBack;
		// The last task's stores, made on the main thread at Main::Update's entry (no list cull, cell or job animation runs; every
		// reader of the node sees them this frame), only on roots DCLF still writes (a_written; the engine steps the rest: one
		// writer). Their snapshot is released here.
		std::unique_ptr<FadeWriteBack::Stores> stores{ writeBack.ready.exchange(nullptr, std::memory_order_acq_rel) };
		if (!stores)
			return;
		// DCLF is not running: the engine culls every root this frame.
		if (!a_live) {
			writeBack.dropped.fetch_add(stores->stores.size(), std::memory_order_relaxed);
			return;
		}
		ZoneScopedN("CS.DCLF.FadeWriteBack.Stores");
		const auto start = std::chrono::steady_clock::now();
		std::uint64_t applied = 0, notWritten = 0;
		for (const auto& store : stores->stores) {
			if (!a_written(store.node)) {
				++notWritten;
				continue;
			}
			// The fade and the flags' fade bits (atomically: the engine owns the others). Stood-in roots' dependents read the fade
			// from FadeStateCS's state, so the fade watch is not told.
			auto* node = static_cast<std::byte*>(store.node);
			std::atomic_ref<std::uint32_t> flags(*reinterpret_cast<std::uint32_t*>(node + 0xF4));
			flags.fetch_and(~kFadeFlagMask, std::memory_order_relaxed);
			flags.fetch_or(store.flags, std::memory_order_relaxed);
			*reinterpret_cast<float*>(node + 0x130) = store.fade;
			// A leaf or tree root's LOD level, transition and blend (written otherwise only by FUN_14147a430 and OnVisible, neither
			// running now).
			std::uint32_t fields = SceneCapture::NodeRecord::kFadeCurrent;
			if (store.lod) {
				*reinterpret_cast<float*>(node + 0x14C) = store.blend;
				*reinterpret_cast<std::uint8_t*>(node + 0x152) = static_cast<std::uint8_t>(store.levels);
				*reinterpret_cast<std::uint8_t*>(node + 0x153) = static_cast<std::uint8_t>(store.levels >> 8);
				fields |= SceneCapture::NodeRecord::kFadeLevel;
			}
			// The mirror's value: an update, not a fade event.
			SceneTracker::Get().PushUpdate(SceneCapture::Update{ fields, SceneCapture::CaptureNodeFields(*static_cast<const RE::NiAVObject*>(store.node), fields) });
			++applied;
		}
		writeBack.applied.fetch_add(applied, std::memory_order_relaxed);
		writeBack.notWritten.fetch_add(notWritten, std::memory_order_relaxed);
		writeBack.storeNs.fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count()),
			std::memory_order_relaxed);
	}

	void KickFadeWriteBack()
	{
		const auto* scene = IndirectDraws::Get().Scene();
		auto fades = scene ? SharedSceneOf(*scene) : nullptr;
		if (!fades || !fades->fadeWriteBack)
			return;
		auto& writeBack = *fades->fadeWriteBack;
		// One task at a time; a running one (or the next) takes the new batches.
		if (bool idle = false; !writeBack.running.compare_exchange_strong(idle, true, std::memory_order_acq_rel)) {
			writeBack.busy.fetch_add(1, std::memory_order_relaxed);
			return;
		}
		// The task judges events against the frame's immutable tables, which the stores hold (and with them the root nodes).
		std::shared_ptr<const SceneTables> tables = Scene::FrameView::Get().AcceptedTables();
		auto find = [fades = std::move(fades), tables] {
			ZoneScopedN("CS.DCLF.FadeWriteBack");
			auto& state = *fades->fadeWriteBack;
			// The roots as those tables list them (the root states' view).
			const auto list = tables ? Extensions::RootListOf(*tables) : Api::RootListView{};
			// The batches in frame order (the stack is newest first, and recordings may finish out of order).
			std::vector<FadeWriteBack::Batch*> batches;
			for (auto* batch = state.batches.exchange(nullptr, std::memory_order_acquire); batch; batch = batch->next)
				batches.push_back(batch);
			std::reverse(batches.begin(), batches.end());
			std::stable_sort(batches.begin(), batches.end(), [](const auto* a_a, const auto* a_b) { return a_a->frame < a_b->frame; });
			auto stores = std::make_unique<FadeWriteBack::Stores>();
			stores->tables = tables;
			for (auto* batch : batches) {
				for (const auto& event : batch->events) {
					if (!tables)
						break;
					if (event.root >= state.appliedFrame.size())
						state.appliedFrame.resize(std::size_t(event.root) + 1, 0u);
					// Only a root still DCLF's to write (same generation, kFadeRootWritten), and never an event older than the last taken.
					if (event.root >= list.rows.size() || list.rows[event.root].generation != event.generation ||
						!(list.rows[event.root].bits & kFadeRootWritten) || !list.nodes[event.root] || batch->frame < state.appliedFrame[event.root]) {
						state.stale.fetch_add(1, std::memory_order_relaxed);
						continue;
					}
					state.appliedFrame[event.root] = batch->frame;
					const std::uint32_t plan = list.rows[event.root].bits & kFadeRootPlanMask;
					stores->stores.push_back({ const_cast<void*>(list.nodes[event.root]), event.flags & kFadeFlagMask, event.currentFade,
						plan == kFadeRootLeaf || plan == kFadeRootTree, event.levels, event.blend });
				}
				delete batch;
			}
			// Stores an earlier task found that no Main::Update entry has made yet go first.
			if (std::unique_ptr<FadeWriteBack::Stores> earlier{ state.ready.exchange(nullptr, std::memory_order_acq_rel) }) {
				earlier->stores.insert(earlier->stores.end(), stores->stores.begin(), stores->stores.end());
				earlier->tables = std::move(stores->tables);
				stores = std::move(earlier);
			}
			if (!stores->stores.empty())
				state.ready.store(stores.release(), std::memory_order_release);
			state.running.store(false, std::memory_order_release);
			state.running.notify_all();
		};
		if (!SceneScheduler::Executor().Dispatch(SceneScheduler::Scope(), SceneExecutor::Preparation, org::async::TaskDispatch::Cpu, "fade write-back",
				[find = std::move(find)](const auto&) { find(); }))
			stl::report_and_fail("Drawcall Limit Fix: the fade write-back was refused by DCLF's executor");
	}

	void WaitFadeWriteBack()
	{
		const auto* fades = CurrentScene();
		if (!fades || !fades->fadeWriteBack)
			return;
		auto& running = fades->fadeWriteBack->running;
		while (running.load(std::memory_order_acquire))
			running.wait(true, std::memory_order_acquire);
		// Teardown, toggle or load: what it found is dropped (the roots may no longer stand in).
		delete fades->fadeWriteBack->ready.exchange(nullptr, std::memory_order_acq_rel);
	}
}
