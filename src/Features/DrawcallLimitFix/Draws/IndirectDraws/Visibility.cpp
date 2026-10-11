#if defined(CS_HAS_RENDER_GRAPH) && defined(CS_HAS_ORG_MODULE_SERVICES)
#	include "Internal.h"
#	include "Features/DrawcallLimitFix/Engine/TreeList.h"

// T7b1: the main camera's visibility as GPU change events, read back without a stall and decoded into DCLF's verdicts, with the
// <- ACCUMULATED observer against the engine's kAccumulated (drawcall-limit-fix.md, "T7b1: visibility events"; Internal.h,
// VisibilityReadback; VisibilityEventsCS.hlsl). T7b2: from those verdicts, kAccumulated written on the roots the cut stands in for
// and their geometry, at Main::Update's entry ("T7b2: kAccumulated written by DCLF"). The readback is the normal path; the
// observers are CS_DCLF_ACCUMULATED_PARITY's alone.

namespace DCLF
{
	using namespace Draws;

	namespace
	{
		using Readback = VisibilityReadback;

		/** @brief The decoder's verdicts made new: a reset frame (a new backing of the reported words, which then hold nothing). */
		void StartAgain(Readback& a_readback)
		{
			a_readback.roots.assign(a_readback.roots.size(), {});
			a_readback.objects.assign(a_readback.objects.size(), {});
			a_readback.rootApplied.assign(a_readback.rootApplied.size(), 0u);
			a_readback.objectApplied.assign(a_readback.objectApplied.size(), 0u);
			a_readback.decodedFrame = a_readback.previousDecodedFrame = 0;
		}

		/** @brief One batch onto the verdicts (the task's), against the tables of its frame. */
		void Apply(Readback& a_readback, const Readback::Batch& a_batch)
		{
			const auto& tables = *a_batch.tables;
			if (a_batch.reset)
				StartAgain(a_readback);
			if (a_readback.roots.size() < tables.fadeRoots.size()) {
				a_readback.roots.resize(tables.fadeRoots.size());
				a_readback.rootApplied.resize(tables.fadeRoots.size(), 0u);
			}
			if (a_readback.objects.size() < tables.objects.size()) {
				a_readback.objects.resize(tables.objects.size());
				a_readback.objectApplied.resize(tables.objects.size(), 0u);
			}
			const std::uint32_t frame = a_batch.frame;
			std::uint64_t stale = 0;
			for (const VisibilityEvent& event : a_batch.events) {
				const bool visible = (event.bits & kVisibilityBitVisible) != 0;
				if (event.kind == kVisibilityKindRoot) {
					// The listing the event is of, in its frame's tables, and no older than the one taken last.
					if (event.slot >= tables.fadeRoots.size() || tables.fadeRoots[event.slot].generation != event.generation || !tables.fadeRootNode[event.slot] ||
						frame < a_readback.rootApplied[event.slot]) {
						++stale;
						continue;
					}
					a_readback.rootApplied[event.slot] = frame;
					auto& root = a_readback.roots[event.slot];
					root.generation = event.generation;
					root.known = (event.bits & kVisibilityBitVerdict) != 0;
					if (root.visible && !visible)
						root.outFrame = frame;
					root.visible = visible;
					if (visible)
						root.lastFrame = frame;
				} else {
					if (event.slot >= tables.objects.size() || !tables.objectGeometry[event.slot] || frame < a_readback.objectApplied[event.slot]) {
						++stale;
						continue;
					}
					a_readback.objectApplied[event.slot] = frame;
					auto& object = a_readback.objects[event.slot];
					if (object.visible && !visible)
						object.outFrame = frame;
					object.visible = visible;
					if (visible)
						object.lastFrame = frame;
				}
			}
			if (frame > a_readback.decodedFrame) {
				a_readback.previousDecodedFrame = a_readback.decodedFrame;
				a_readback.decodedFrame = frame;
			}
			a_readback.batches.fetch_add(1, std::memory_order_relaxed);
			a_readback.events.fetch_add(a_batch.events.size(), std::memory_order_relaxed);
			a_readback.stale.fetch_add(stale, std::memory_order_relaxed);
		}

		/**
		 * @brief T7b2, the task's, after a decode's batches: the kAccumulated writes of the stood-in roots (kFadeRootStoodIn in the newest
		 * batch's tables) and their geometry, by the conservative rule, where they differ from what was last asked for (all of them on a
		 * full write). Reads no engine memory: the slots, the verdicts and the tables' pointers alone. Null when there is nothing to write.
		 *
		 * The rule (drawcall-limit-fix.md, "T7b2"): visible when in view in either of the last two decoded frames (in view now, or gone
		 * out after the frame before the newest), when the root's listing has no verdict yet (a new listing, one listed again, no seed:
		 * visible until FadeStateCS's first), or while a camera cut has no batch of its frame or later decoded.
		 */
		Readback::Writes* PrepareWrites(Readback& a_readback, const Readback::Batch& a_newest, bool a_reset)
		{
			const auto& tables = *a_newest.tables;
			const std::uint32_t cut = a_readback.cutFrame.load(std::memory_order_acquire);
			const bool cutPending = cut && a_readback.decodedFrame < cut;
			// The first batch of a cut's frame or later: everything again (the main thread set every stood-in root at the cut and left
			// the older batches' clears).
			const bool cutAnswered = cut && !cutPending && a_readback.cutAnswered != cut;
			const bool full = a_reset || cutAnswered || a_readback.refreshWanted.exchange(false, std::memory_order_acq_rel);
			if (cutAnswered)
				a_readback.cutAnswered = cut;
			const std::size_t roots = tables.fadeRootNode.size(), objects = tables.objectGeometry.size();
			if (a_readback.rootWritten.size() < roots) {
				a_readback.rootWritten.resize(roots, Readback::kWrittenNone);
				a_readback.rootWrittenGeneration.resize(roots, 0u);
			}
			if (a_readback.objectWritten.size() < objects) {
				a_readback.objectWritten.resize(objects, Readback::kWrittenNone);
				a_readback.objectWrittenGeometry.resize(objects, nullptr);
			}
			const std::uint32_t window = a_readback.previousDecodedFrame;
			auto* list = new Readback::Writes();
			list->frame = a_newest.frame;
			list->full = full;
			// Per root slot: stood in for in this batch's tables (its geometry is written with it).
			std::vector<std::uint8_t> stoodIn(roots, 0u);
			for (std::size_t r = 0; r < roots; ++r) {
				const void* node = tables.fadeRootNode[r];
				if (!node || r >= tables.fadeRoots.size() || !(tables.fadeRoots[r].bits & kFadeRootStoodIn)) {
					// Not stood in (the engine culls it, or no root here): what it was last asked is forgotten, so a return writes again.
					a_readback.rootWritten[r] = Readback::kWrittenNone;
					continue;
				}
				const auto& row = tables.fadeRoots[r];
				stoodIn[r] = 1;
				bool visible = cutPending;
				if (!visible) {
					const bool listed = r < a_readback.roots.size() && a_readback.roots[r].generation == row.generation && a_readback.roots[r].known;
					visible = !listed || a_readback.roots[r].visible || a_readback.roots[r].outFrame > window;
				}
				const std::uint8_t wanted = visible ? Readback::kWrittenSet : Readback::kWrittenClear;
				if (!full && a_readback.rootWritten[r] == wanted && a_readback.rootWrittenGeneration[r] == row.generation)
					continue;
				a_readback.rootWritten[r] = wanted;
				a_readback.rootWrittenGeneration[r] = row.generation;
				list->writes.push_back({ const_cast<void*>(node), node, visible });
			}
			for (std::size_t o = 0; o < objects; ++o) {
				auto* geometry = tables.objectGeometry[o];
				const std::uint32_t root = o < tables.objectFadeRoot.size() ? tables.objectFadeRoot[o] : kNoFadeRoot;
				if (!geometry || tables.IsLayer(static_cast<std::uint32_t>(o)) || root == kNoFadeRoot || root >= roots || !stoodIn[root]) {
					a_readback.objectWrittenGeometry[o] = nullptr;
					continue;
				}
				bool visible = cutPending;
				if (!visible && o < a_readback.objects.size())
					visible = a_readback.objects[o].visible || a_readback.objects[o].outFrame > window;
				const std::uint8_t wanted = visible ? Readback::kWrittenSet : Readback::kWrittenClear;
				if (!full && a_readback.objectWritten[o] == wanted && a_readback.objectWrittenGeometry[o] == geometry)
					continue;
				a_readback.objectWritten[o] = wanted;
				a_readback.objectWrittenGeometry[o] = geometry;
				list->writes.push_back({ geometry, tables.fadeRootNode[root], visible });
			}
			if (list->writes.empty()) {
				delete list;
				return nullptr;
			}
			a_readback.fullWrites.fetch_add(full ? 1 : 0, std::memory_order_relaxed);
			list->tables = a_newest.tables;
			return list;
		}

		/** @brief Main thread: one node's kAccumulated as wanted (an atomic read-modify-write; none when it already is). */
		void Store(Readback& a_readback, void* a_node, bool a_set)
		{
			constexpr std::size_t kObjectFlags = 0xF4;            // NiAVObject::flags
			constexpr std::uint32_t kFlagAccumulated = 1u << 26;  // kAccumulated
			std::atomic_ref<std::uint32_t> flags(*reinterpret_cast<std::uint32_t*>(static_cast<std::byte*>(a_node) + kObjectFlags));
			if (((flags.load(std::memory_order_relaxed) & kFlagAccumulated) != 0) == a_set) {
				a_readback.writesUnchanged.fetch_add(1, std::memory_order_relaxed);
				return;
			}
			if (a_set) {
				flags.fetch_or(kFlagAccumulated, std::memory_order_relaxed);
				a_readback.writesSet.fetch_add(1, std::memory_order_relaxed);
			} else {
				flags.fetch_and(~kFlagAccumulated, std::memory_order_relaxed);
				a_readback.writesCleared.fetch_add(1, std::memory_order_relaxed);
			}
		}

		void Count(Readback::Counts& a_counts, bool a_dclf, bool a_engine)
		{
			auto& counter = a_dclf ? (a_engine ? a_counts.both : a_counts.dclfOnly) : (a_engine ? a_counts.engineOnly : a_counts.neither);
			counter.fetch_add(1, std::memory_order_relaxed);
		}

		/**
		 * @brief The observer (<- ACCUMULATED): the decoder's verdicts just after the batch of a_sample's frame against the engine's bit
		 * as that frame's cull left it, exactly. A slot whose node is not the batch tables' (the tables moved between the sample and the
		 * commit) is counted aside, as are a root DCLF has no verdict for and DCLF's alone on an app-culled root. The first differences
		 * are kept for the render thread to name.
		 */
		void Compare(Readback& a_readback, Readback::Batch& a_batch, const SideEffects::AccumulatedSample& a_sample)
		{
			using namespace SideEffects;
			const auto& tables = *a_batch.tables;
			std::array<std::uint32_t, Readback::kCategories> kept{};
			const auto keep = [&](Readback::Category a_category, const void* a_node) {
				if (kept[a_category]++ < 2)
					a_batch.firsts.emplace_back(static_cast<std::uint8_t>(a_category), a_node);
			};
			std::uint64_t moved = 0, staleGeometry = 0, noVerdictEngine = 0, noVerdictNeither = 0, hiddenDclfOnly = 0;
			for (std::size_t r = 0; r < a_sample.rootNodes.size(); ++r) {
				const void* node = a_sample.rootNodes[r];
				if (!node)
					continue;
				if (r >= tables.fadeRootNode.size() || tables.fadeRootNode[r] != node) {
					++moved;
					continue;
				}
				const std::uint8_t bits = a_sample.rootBits[r];
				const bool engine = (bits & kSampleAccumulated) != 0;
				const bool listed = r < a_readback.roots.size() && a_readback.roots[r].generation == tables.fadeRoots[r].generation;
				if (!listed || !a_readback.roots[r].known) {
					++(engine ? noVerdictEngine : noVerdictNeither);
					continue;
				}
				const bool dclf = a_readback.roots[r].visible;
				if (dclf && !engine && (bits & kSampleHidden)) {
					++hiddenDclfOnly;
					continue;
				}
				const bool stoodIn = (bits & kSampleStoodIn) != 0;
				Count(stoodIn ? a_readback.stoodInRootCounts : a_readback.rootCounts, dclf, engine);
				if (!stoodIn && dclf != engine)
					keep(dclf ? Readback::kRootDclfOnly : Readback::kRootEngineOnly, node);
			}
			for (std::size_t o = 0; o < a_sample.objectGeometry.size(); ++o) {
				const void* geometry = a_sample.objectGeometry[o];
				if (!geometry)
					continue;
				if (o >= tables.objectGeometry.size() || tables.objectGeometry[o] != geometry) {
					++moved;
					continue;
				}
				const std::uint8_t bits = a_sample.objectBits[o];
				const bool engine = (bits & kSampleAccumulated) != 0;
				const bool dclf = o < a_readback.objects.size() && a_readback.objects[o].visible;
				if (dclf && !engine && (bits & kSampleHidden)) {
					++hiddenDclfOnly;
					continue;
				}
				const bool stoodIn = (bits & kSampleStoodIn) != 0;
				Count(stoodIn ? a_readback.stoodInGeometryCounts : a_readback.geometryCounts, dclf, engine);
				if (stoodIn || dclf == engine)
					continue;
				// The engine's alone under a root its cull failed: the bit a cull that reached it left, not this frame's.
				if (engine && !(bits & kSampleRootAccumulated))
					++staleGeometry;
				keep(dclf ? Readback::kGeometryDclfOnly : Readback::kGeometryEngineOnly, geometry);
			}
			a_readback.moved.fetch_add(moved, std::memory_order_relaxed);
			a_readback.staleGeometry.fetch_add(staleGeometry, std::memory_order_relaxed);
			a_readback.noVerdictEngine.fetch_add(noVerdictEngine, std::memory_order_relaxed);
			a_readback.noVerdictNeither.fetch_add(noVerdictNeither, std::memory_order_relaxed);
			a_readback.hiddenDclfOnly.fetch_add(hiddenDclfOnly, std::memory_order_relaxed);
			a_readback.framesCompared.fetch_add(1, std::memory_order_relaxed);
		}

		/** @brief Render thread: a decoded batch's finds named (its tables hold the nodes), then the batch and its tables let go. */
		void Release(Readback& a_readback, Readback::Batch* a_batch)
		{
			for (const auto& [category, node] : a_batch->firsts) {
				auto& names = a_readback.firstNames[category];
				if (names.size() > 400)
					continue;
				const auto* object = static_cast<const RE::NiAVObject*>(node);
				names += fmt::format("{}'{}' (frame {})", names.empty() ? "" : ", ", object->name.c_str() ? object->name.c_str() : "?", a_batch->frame);
			}
			delete a_batch;
		}

		/**
		 * @brief Render thread, the depth commit of a frame slot: the slot's last execution's events, if its buffer holds the frame that
		 * armed it, copied into a batch. The host has waited for that execution, and the next one is not submitted yet.
		 */
		void Harvest(Readback& a_readback, std::uint32_t a_frameSlot)
		{
			auto& armed = a_readback.armed[a_frameSlot];
			if (!armed.armed)
				return;
			armed.armed = false;
			// The staging set the execution's revision copied into: the one whose buffer names the armed frame (a growth's revisions copy
			// into the next set; a buffer of another set holds an older frame).
			for (const auto* set = a_readback.sets.load(std::memory_order_acquire); set; set = set->next) {
				auto resource = set->buffers[a_frameSlot]->GetAPIResource();
				void* mapped = nullptr;
				resource.Map(&mapped);
				if (!mapped)
					continue;
				const auto* words = static_cast<const std::uint32_t*>(mapped);
				if (words[1] != armed.frame) {
					resource.Unmap(0, 0);
					continue;
				}
				const std::uint32_t count = words[0];
				const std::uint32_t events = std::min(count, set->events);
				auto* batch = new Readback::Batch();
				batch->frame = armed.frame;
				batch->reset = armed.reset || std::exchange(a_readback.carryReset, false);
				batch->tables = std::move(armed.tables);
				// The slots past the list's end append again next frame; the list grows to them (ReserveSceneTables).
				batch->complete = count <= set->events;
				if (!batch->complete) {
					std::uint32_t wanted = a_readback.wanted.load(std::memory_order_relaxed);
					while (wanted < count && !a_readback.wanted.compare_exchange_weak(wanted, count, std::memory_order_release, std::memory_order_relaxed)) {}
					a_readback.overflowed.fetch_add(1, std::memory_order_relaxed);
				}
				const auto* list = reinterpret_cast<const VisibilityEvent*>(words + kVisibilityEventHeaderWords);
				batch->events.assign(list, list + events);
				resource.Unmap(0, 0);
				a_readback.pending.push_back(batch);
				return;
			}
			// The epoch did not run for that frame, or ran a revision without the readback: its changes are the next frame's (the GPU
			// appends again what it did not report). A reset it carried goes with the next batch harvested.
			a_readback.missed.fetch_add(1, std::memory_order_relaxed);
			a_readback.carryReset = a_readback.carryReset || armed.reset;
			armed.tables.reset();
		}
	}

	namespace
	{
		/** @brief Render thread: the batches the last task decoded, their finds named and their tables let go (engine reads and the
		 * publication's retirement are the render thread's). */
		void ReleaseDecoded(Readback& a_readback)
		{
			auto* batch = a_readback.decoded.exchange(nullptr, std::memory_order_acquire);
			// T7b5's differences named first: their nodes are held by these batches' tables, or by a decode still running.
			TreeList::NameFirsts();
			while (batch) {
				auto* following = batch->next;
				Release(a_readback, batch);
				batch = following;
			}
		}

		/**
		 * @brief Render thread: the harvested batches handed to one decode task on DCLF's executor, never joined (one at a time; the
		 * batches a running one did not take wait for the next call). The task applies them in frame order, runs the observer when its
		 * samples are there, and prepares the kAccumulated writes from the newest (T7b2) for Main::Update's entry.
		 */
		void Dispatch(const std::shared_ptr<Readback>& a_readback)
		{
			auto& readback = *a_readback;
			// The samples no batch will reach: older than every armed frame and every batch waiting (their frames ran no readback).
			std::uint32_t oldestPending = SceneStore::Get().GetFrame();
			for (const auto& armed : readback.armed)
				if (armed.armed)
					oldestPending = std::min(oldestPending, armed.frame);
			for (const auto* batch : readback.pending)
				oldestPending = std::min(oldestPending, batch->frame);
			if (bool idle = false; !readback.running.compare_exchange_strong(idle, true, std::memory_order_acq_rel)) {
				readback.busy.fetch_add(1, std::memory_order_relaxed);
				return;
			}
			if (readback.pending.empty()) {
				readback.samplesUnmatched.fetch_add(SideEffects::DropAccumulatedSamples(oldestPending), std::memory_order_relaxed);
				TreeList::DropSamples(oldestPending);
				readback.running.store(false, std::memory_order_release);
				readback.running.notify_all();
				return;
			}
			auto batches = std::exchange(readback.pending, {});
			std::stable_sort(batches.begin(), batches.end(), [](const auto* a_a, const auto* a_b) { return a_a->frame < a_b->frame; });
			// The engine's samples those batches reach (CS_DCLF_ACCUMULATED_PARITY: none are taken without it).
			auto samples = std::make_shared<std::vector<std::unique_ptr<SideEffects::AccumulatedSample>>>(SideEffects::TakeAccumulatedSamples(batches.back()->frame));
			readback.samplesUnmatched.fetch_add(SideEffects::DropAccumulatedSamples(oldestPending), std::memory_order_relaxed);
			// T7b5's tree manager samples likewise (CS_DCLF_TREE_PARITY: none without it).
			auto treeSamples = std::make_shared<std::vector<TreeList::SamplePtr>>(TreeList::TakeSamples(batches.back()->frame));
			TreeList::DropSamples(oldestPending);
			auto decode = [readback = a_readback, batches = std::move(batches), holder = std::move(samples), treeSamples = std::move(treeSamples)] {
				ZoneScopedN("CS.DCLF.VisibilityDecode");
				const auto& samples = *holder;
				const auto start = std::chrono::steady_clock::now();
				std::size_t next = 0, nextTree = 0;
				bool reset = false;
				for (auto* batch : batches) {
					reset = reset || batch->reset;
					Apply(*readback, *batch);
					// The samples before this batch's frame had none; its own is compared, unless the batch left changes for the next frame.
					for (; next < samples.size() && samples[next]->frame < batch->frame; ++next)
						readback->samplesUnmatched.fetch_add(1, std::memory_order_relaxed);
					if (next < samples.size() && samples[next]->frame == batch->frame) {
						if (batch->complete)
							Compare(*readback, *batch, *samples[next]);
						else
							readback->framesIncomplete.fetch_add(1, std::memory_order_relaxed);
						++next;
					}
					if (!treeSamples->empty()) {
						const auto& tables = *batch->tables;
						TreeList::OnBatch(*treeSamples, nextTree, batch->frame, batch->complete,
							[&](std::uint32_t a_slot) -> const void* { return a_slot < tables.fadeRootNode.size() ? tables.fadeRootNode[a_slot] : nullptr; },
							[&](std::uint32_t a_slot) {
								TreeList::RootVerdict verdict;
								if (a_slot < readback->roots.size() && a_slot < tables.fadeRoots.size() && readback->roots[a_slot].generation == tables.fadeRoots[a_slot].generation) {
									verdict.known = readback->roots[a_slot].known;
									verdict.visible = readback->roots[a_slot].visible;
								}
								return verdict;
							});
					}
				}
				readback->samplesUnmatched.fetch_add(samples.size() - next, std::memory_order_relaxed);
				TreeList::OnDecodeEnd(*treeSamples, nextTree);
				// T7b2: the writes, from the newest batch's tables (held by the writes until the main thread made them).
				if (auto* writes = PrepareWrites(*readback, *batches.back(), reset)) {
					writes->next = readback->writes.load(std::memory_order_relaxed);
					while (!readback->writes.compare_exchange_weak(writes->next, writes, std::memory_order_release, std::memory_order_relaxed)) {}
				}
				// Back to the render thread (their tables are its to let go).
				for (auto* batch : batches) {
					batch->next = readback->decoded.load(std::memory_order_relaxed);
					while (!readback->decoded.compare_exchange_weak(batch->next, batch, std::memory_order_release, std::memory_order_relaxed)) {}
				}
				readback->decodeNs.fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count()),
					std::memory_order_relaxed);
				readback->running.store(false, std::memory_order_release);
				readback->running.notify_all();
			};
			if (!SceneScheduler::Executor().Dispatch(SceneScheduler::Scope(), PublishedSceneExecutor::Preparation, org::async::TaskDispatch::Cpu, "visibility decode",
					[decode = std::move(decode)](const auto&) { decode(); }))
				stl::report_and_fail("Drawcall Limit Fix: the visibility decode was refused by DCLF's executor");
		}
	}

	void IndirectDraws::Impl::ArmVisibilityReadback(SceneStore& a_store, SceneBuffers& a_scene, CommitUploads& a_uploads)
	{
		if (!a_scene.visibilityReadback || !a_scene.visibilityEvents)
			return;
		auto& readback = *a_scene.visibilityReadback;
		auto* host = RenderGraphRuntime::Get().Host();
		if (!host)
			return;
		const std::uint32_t frameSlot = host->CurrentFrameSlot();
		if (frameSlot >= readback.armed.size())
			return;
		const std::size_t harvested = readback.pending.size();
		Harvest(readback, frameSlot);
		// The count and the frame, for this execution's VisibilityEventsCS to append from.
		static constexpr std::uint32_t kZeroHeader[kVisibilityEventHeaderWords]{};
		a_uploads(a_scene.visibilityEvents, kZeroHeader, sizeof(kZeroHeader), 0);
		// A new backing of the reported words holds nothing: zeroed (nothing reported, no verdict), and the decoder's verdicts start
		// again with this frame's batch.
		bool reset = false;
		if (!a_scene.visibilityReportedZeroed) {
			const std::vector<std::byte> roots(std::size_t(a_scene.fadeRootCapacity) * 2 * sizeof(std::uint32_t));
			a_uploads(a_scene.visibilityRootReported, roots.data(), roots.size(), 0);
			const std::vector<std::byte> objects(std::size_t(a_scene.objectCapacity) * sizeof(std::uint32_t));
			a_uploads(a_scene.visibilityObjectReported, objects.data(), objects.size(), 0);
			a_scene.visibilityReportedZeroed = true;
			reset = true;
		}
		auto& armed = readback.armed[frameSlot];
		armed.armed = true;
		armed.frame = a_store.GetFrame();
		armed.reset = reset;
		armed.tables = a_store.AcceptedTables();
		// T7b2: a batch harvested goes to the decode now, not at the next frame's start, so its writes reach the next Main::Update's
		// entry (a frame less of latency).
		if (readback.pending.size() > harvested)
			Dispatch(a_scene.visibilityReadback);
	}

	void IndirectDraws::KickVisibilityReadback()
	{
		if (!impl->scene || !impl->scene->visibilityReadback)
			return;
		ReleaseDecoded(*impl->scene->visibilityReadback);
		Dispatch(impl->scene->visibilityReadback);
	}

	void IndirectDraws::NoteCameraCut(std::uint32_t a_frame)
	{
		if (!impl->scene || !impl->scene->visibilityReadback)
			return;
		impl->scene->visibilityReadback->cutFrame.store(a_frame, std::memory_order_release);
		impl->scene->visibilityReadback->cuts.fetch_add(1, std::memory_order_relaxed);
	}

	void IndirectDraws::RequestAccumulatedRefresh()
	{
		if (impl->scene && impl->scene->visibilityReadback)
			impl->scene->visibilityReadback->refreshWanted.store(true, std::memory_order_release);
	}

	void IndirectDraws::WriteAccumulated(bool a_live, const std::function<bool(const void*)>& a_stoodIn)
	{
		if (!impl->scene || !impl->scene->visibilityReadback)
			return;
		auto& r = *impl->scene->visibilityReadback;
		const auto start = std::chrono::steady_clock::now();
		// The decoder's writes, newest first on the stack: in frame order.
		Readback::Writes* ordered = nullptr;
		for (auto* list = r.writes.exchange(nullptr, std::memory_order_acquire); list;) {
			auto* following = list->next;
			list->next = ordered;
			ordered = list;
			list = following;
		}
		// DCLF does not run this frame: nothing is written, and the decoder writes everything again once it does (what it last asked
		// for was not made, and the engine's cull wrote the roots meanwhile).
		if (!a_live) {
			for (auto* list = ordered; list;) {
				auto* following = list->next;
				delete list;
				list = following;
			}
			r.writtenTables.reset();
			r.refreshWanted.store(true, std::memory_order_release);
			return;
		}
		const std::uint32_t cut = r.cutFrame.load(std::memory_order_acquire);
		for (auto* list = ordered; list;) {
			// A batch older than the last cut: its sets only (every stood-in root stays visible until a batch of the cut's frame or later
			// arrives; the decoder's full write then puts every clear back).
			const bool beforeCut = cut && list->frame < cut;
			for (const auto& write : list->writes) {
				if (!a_stoodIn(write.root)) {
					r.writesNotStoodIn.fetch_add(1, std::memory_order_relaxed);
					continue;
				}
				if (beforeCut && !write.set) {
					r.writesHeldByCut.fetch_add(1, std::memory_order_relaxed);
					continue;
				}
				Store(r, write.node, write.set);
			}
			r.writeBatches.fetch_add(1, std::memory_order_relaxed);
			// The newest's tables kept (a cut's writes take the stood-in roots from them); the older let go here, on the main thread.
			r.writtenTables = std::move(list->tables);
			auto* following = list->next;
			delete list;
			list = following;
		}
		// A camera cut since the last: every stood-in root, and its geometry, visible now (the newest tables' slots).
		if (cut && r.cutWritten != cut && r.writtenTables) {
			r.cutWritten = cut;
			const auto& tables = *r.writtenTables;
			std::vector<std::uint8_t> stoodIn(tables.fadeRootNode.size(), 0u);
			for (std::size_t root = 0; root < tables.fadeRootNode.size(); ++root) {
				const void* node = tables.fadeRootNode[root];
				if (!node || root >= tables.fadeRoots.size() || !(tables.fadeRoots[root].bits & kFadeRootStoodIn) || !a_stoodIn(node))
					continue;
				stoodIn[root] = 1;
				Store(r, const_cast<void*>(node), true);
				r.cutWrites.fetch_add(1, std::memory_order_relaxed);
			}
			for (std::size_t o = 0; o < tables.objectGeometry.size(); ++o) {
				const std::uint32_t root = o < tables.objectFadeRoot.size() ? tables.objectFadeRoot[o] : kNoFadeRoot;
				if (tables.objectGeometry[o] && !tables.IsLayer(static_cast<std::uint32_t>(o)) && root < stoodIn.size() && stoodIn[root])
					Store(r, tables.objectGeometry[o], true);
			}
		}
		r.writeNs.fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count()),
			std::memory_order_relaxed);
	}

	void IndirectDraws::Impl::WaitVisibilityReadback()
	{
		if (!scene || !scene->visibilityReadback)
			return;
		auto& running = scene->visibilityReadback->running;
		while (running.load(std::memory_order_acquire))
			running.wait(true, std::memory_order_acquire);
	}

	std::string IndirectDraws::VisibilityReport()
	{
		if (!impl->scene || !impl->scene->visibilityReadback)
			return {};
		auto& r = *impl->scene->visibilityReadback;
		const auto take = [](std::atomic<std::uint64_t>& a_counter) { return a_counter.exchange(0, std::memory_order_relaxed); };
		// T7b2's writes (the normal path).
		std::string text = fmt::format(
			"[DCLF] accumulated writes (T7b2): {} write lists made at Main::Update's entry ({} full), {} set, {} cleared, {} already so, {} left (no longer stood in), "
			"{} clears held by a camera cut ({} cuts, {} roots set by them); {:.1f} us on the main thread\n",
			take(r.writeBatches), take(r.fullWrites), take(r.writesSet), take(r.writesCleared), take(r.writesUnchanged), take(r.writesNotStoodIn), take(r.writesHeldByCut),
			take(r.cuts), take(r.cutWrites), double(take(r.writeNs)) / 1000.0);
		// T7b5, CS_DCLF_TREE_PARITY (<- TREE MANAGER).
		text += TreeList::Report();
		if (!SideEffects::AccumulatedParity())
			return text;
		text += SideEffects::AnimRateReport();
		struct Taken
		{
			std::uint64_t both, dclfOnly, engineOnly, neither;
		};
		const auto takeCounts = [&](Readback::Counts& a_counts) { return Taken{ take(a_counts.both), take(a_counts.dclfOnly), take(a_counts.engineOnly), take(a_counts.neither) }; };
		const auto roots = takeCounts(r.rootCounts), geometry = takeCounts(r.geometryCounts);
		const auto stoodRoots = takeCounts(r.stoodInRootCounts), stoodGeometry = takeCounts(r.stoodInGeometryCounts);
		const auto frames = take(r.framesCompared), incomplete = take(r.framesIncomplete), unmatched = take(r.samplesUnmatched), moved = take(r.moved);
		const auto staleGeometry = take(r.staleGeometry), noVerdictEngine = take(r.noVerdictEngine), noVerdictNeither = take(r.noVerdictNeither);
		const auto hidden = take(r.hiddenDclfOnly);
		const auto batches = take(r.batches), events = take(r.events), stale = take(r.stale), overflowed = take(r.overflowed), missed = take(r.missed);
		const auto setsAdded = take(r.setsAdded), busy = take(r.busy), decodeNs = take(r.decodeNs);
		std::uint32_t sets = 0, listEvents = 0;
		for (const auto* set = r.sets.load(std::memory_order_acquire); set; set = set->next) {
			++sets;
			listEvents = std::max(listEvents, set->events);
		}
		std::string firsts;
		static constexpr std::array<const char*, Readback::kCategories> kCategoryNames{ "roots DCLF's alone", "roots the engine's alone", "geometry DCLF's alone",
			"geometry the engine's alone" };
		for (std::size_t c = 0; c < Readback::kCategories; ++c) {
			if (!r.firstNames[c].empty())
				firsts += fmt::format("; first {}: {}", kCategoryNames[c], r.firstNames[c]);
			r.firstNames[c].clear();
		}
		const bool differ = roots.dclfOnly || roots.engineOnly || geometry.dclfOnly || geometry.engineOnly;
		return text + fmt::format(
			"[DCLF] accumulated (T7b1): {} frames compared ({} left out: a batch past its list; {} samples no batch reached; {} slots moved), engine-culled roots "
			"{} both / {} DCLF's alone / {} the engine's alone / {} neither, geometry {} / {} / {} / {} ({} of the engine's alone stale under a failed root); stood-in roots "
			"{} / {} / {} / {}, geometry {} / {} / {} / {}; aside: roots with no DCLF verdict {} set by the engine / {} not, {} DCLF's alone on an app-culled root; "
			"readback {} batches, {} events ({} stale), {} past their list, {} arms missed, {} staging sets (+{}), event list {}, {} wakes busy, decode {:.1f} us{}{}\n",
			frames, incomplete, unmatched, moved, roots.both, roots.dclfOnly, roots.engineOnly, roots.neither, geometry.both, geometry.dclfOnly, geometry.engineOnly, geometry.neither,
			staleGeometry, stoodRoots.both, stoodRoots.dclfOnly, stoodRoots.engineOnly, stoodRoots.neither, stoodGeometry.both, stoodGeometry.dclfOnly, stoodGeometry.engineOnly,
			stoodGeometry.neither, noVerdictEngine, noVerdictNeither, hidden, batches, events, stale, overflowed, missed, sets, setsAdded, listEvents, busy,
			double(decodeNs) / 1000.0, !frames ? " <- NOT COMPARED" : differ ? " <- ACCUMULATED DIFFER" : " <- OK", firsts);
	}
}

#endif
