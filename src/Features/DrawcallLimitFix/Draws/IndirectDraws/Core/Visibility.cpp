// The visibility readback (Readbacks.h, VisibilityReadback; drawcall-limit-fix.md): the main camera's visibility as GPU change
// events (VisibilityEventsCS), armed and harvested by the depth commit (render thread), decoded into verdicts by a task on DCLF's
// executor, and written as kAccumulated by the main thread at Main::Update's entry on the roots DCLF lists and their geometry.
// The <- ACCUMULATED observer against the engine's bit runs only under CS_DCLF_ACCUMULATED_PARITY.

#if defined(CS_HAS_RENDER_GRAPH) && defined(CS_HAS_ORG_MODULE_SERVICES)
#	include "Features/DrawcallLimitFix/Draws/IndirectDraws/Internal.h"
#include "Features/DrawcallLimitFix/Scene/FrameView/FrameView.h"
#	include "Features/DrawcallLimitFix/Diagnostics/Engine/Accumulated.h"

namespace DCLF
{
	using namespace Draws;

	namespace
	{
		using Readback = VisibilityReadback;

		/** @brief Clears the decoder's verdicts (a reset frame: the reported words have a new, empty backing). */
		void StartAgain(Readback& a_readback)
		{
			a_readback.roots.assign(a_readback.roots.size(), {});
			a_readback.objects.assign(a_readback.objects.size(), {});
			a_readback.rootApplied.assign(a_readback.rootApplied.size(), 0u);
			a_readback.objectApplied.assign(a_readback.objectApplied.size(), 0u);
			a_readback.decodedFrame = a_readback.previousDecodedFrame = 0;
		}

		/** @brief Decode task: applies one batch to the verdicts, against its frame's tables. */
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
					// Must match its listing in its frame's tables and be no older than the last applied.
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
		 * @brief Decode task: the kAccumulated writes that differ from the last asked (all on a full write). Reads no engine
		 * memory. Null when there is nothing to write.
		 *
		 * The conservative rule (drawcall-limit-fix.md): visible when in view in either of the last two decoded frames, when the
		 * listing has no verdict yet (visible until FadeStateCS's first), or while a camera cut has no decoded batch of its frame or
		 * later.
		 *
		 * Stood-in roots and their geometry, plus wide writes: every other listed fade root and non-layer geometry under one, and
		 * each plain entry root (visible when any of its geometries is). A slot changing class is rewritten.
		 */
		Readback::Writes* PrepareWrites(Readback& a_readback, const Readback::Batch& a_newest, bool a_reset)
		{
			const auto& tables = *a_newest.tables;
			const std::uint32_t cut = a_readback.cutFrame.load(std::memory_order_acquire);
			const bool cutPending = cut && a_readback.decodedFrame < cut;
			// The first batch at or after a cut's frame rewrites everything (the main thread set every root at the cut and held back
			// older batches' clears).
			const bool cutAnswered = cut && !cutPending && a_readback.cutAnswered != cut;
			const bool full = a_reset || cutAnswered || a_readback.refreshWanted.exchange(false, std::memory_order_acq_rel);
			if (cutAnswered)
				a_readback.cutAnswered = cut;
			const std::size_t roots = tables.fadeRootNode.size(), objects = tables.objectGeometry.size();
			if (a_readback.rootWritten.size() < roots) {
				a_readback.rootWritten.resize(roots, Readback::kWrittenNone);
				a_readback.rootWrittenGeneration.resize(roots, 0u);
				a_readback.rootWrittenWide.resize(roots, 0u);
			}
			if (a_readback.objectWritten.size() < objects) {
				a_readback.objectWritten.resize(objects, Readback::kWrittenNone);
				a_readback.objectWrittenGeometry.resize(objects, nullptr);
				a_readback.objectWrittenWide.resize(objects, 0u);
			}
			const std::uint32_t window = a_readback.previousDecodedFrame;
			auto* list = new Readback::Writes();
			list->frame = a_newest.frame;
			list->full = full;
			// Per root slot: 1 stood in, 2 wide (its geometry takes the same class).
			std::vector<std::uint8_t> rootClass(roots, 0u);
			for (std::size_t r = 0; r < roots; ++r) {
				const void* node = tables.fadeRootNode[r];
				const bool stoodIn = node && r < tables.fadeRoots.size() && (tables.fadeRoots[r].bits & kFadeRootStoodIn);
				if (!node || r >= tables.fadeRoots.size()) {
					// Not written: forget the last write so a return writes again.
					a_readback.rootWritten[r] = Readback::kWrittenNone;
					continue;
				}
				const auto& row = tables.fadeRoots[r];
				rootClass[r] = stoodIn ? 1 : 2;
				bool visible = cutPending;
				const bool listed = r < a_readback.roots.size() && a_readback.roots[r].generation == row.generation && a_readback.roots[r].known;
				if (!visible)
					visible = !listed || a_readback.roots[r].visible || a_readback.roots[r].outFrame > window;
				const std::uint8_t wanted = visible ? Readback::kWrittenSet : Readback::kWrittenClear;
				const std::uint8_t wideBit = stoodIn ? 0 : 1;
				if (!full && a_readback.rootWritten[r] == wanted && a_readback.rootWrittenGeneration[r] == row.generation && a_readback.rootWrittenWide[r] == wideBit)
					continue;
				a_readback.rootWritten[r] = wanted;
				a_readback.rootWrittenGeneration[r] = row.generation;
				a_readback.rootWrittenWide[r] = wideBit;
				list->writes.push_back({ const_cast<void*>(node), node, visible, Readback::kWriteRoot, !stoodIn });
			}
			// Plain entry roots (wide): each object slot's entry, cached per occupant for this candidates snapshot.
			const SunCandidates* candidates = a_newest.candidates.get();
			if (candidates != a_readback.entryCandidates) {
				a_readback.entryCandidates = candidates;
				a_readback.objectEntry.assign(a_readback.objectEntry.size(), SunCandidates::kNone);
				a_readback.objectEntryGeometry.assign(a_readback.objectEntryGeometry.size(), nullptr);
			}
			if (a_readback.objectEntry.size() < objects) {
				a_readback.objectEntry.resize(objects, SunCandidates::kNone);
				a_readback.objectEntryGeometry.resize(objects, nullptr);
			}
			const std::uint32_t entries = candidates ? candidates->Capacity() : 0u;
			// Per entry: 1 listed, 2 visible; an entry with an engine-residue geometry keeps the engine's bit.
			std::vector<std::uint8_t> entryState(entries, 0u), entryResidue(entries, 0u);
			for (std::size_t o = 0; o < objects; ++o) {
				auto* geometry = tables.objectGeometry[o];
				const std::uint32_t root = o < tables.objectFadeRoot.size() ? tables.objectFadeRoot[o] : kNoFadeRoot;
				std::uint8_t objectClass = 0;
				const void* rootNode = nullptr;
				std::uint32_t entry = SunCandidates::kNone;
				if (geometry && !tables.IsLayer(static_cast<std::uint32_t>(o))) {
					if (root != kNoFadeRoot && root < roots && rootClass[root]) {
						objectClass = rootClass[root];
						rootNode = tables.fadeRootNode[root];
					} else if (root == kNoFadeRoot && candidates) {
						if (a_readback.objectEntryGeometry[o] != geometry) {
							a_readback.objectEntryGeometry[o] = geometry;
							a_readback.objectEntry[o] = SunCandidates::kNone;
							if (const auto it = candidates->geometries.find(geometry); it != candidates->geometries.end() && it->second < candidates->geometryEntry.size())
								a_readback.objectEntry[o] = candidates->geometryEntry[it->second];
						}
						entry = a_readback.objectEntry[o];
						const bool entryIsRoot = entry < entries && candidates->entryNodes[entry] && tables.fadeRootIndex.contains(candidates->entryNodes[entry]);
						if (entry < entries && candidates->entryNodes[entry] && !entryIsRoot) {
							objectClass = 2;
							rootNode = candidates->entryNodes[entry];
						} else {
							entry = SunCandidates::kNone;
						}
					}
				}
				// A wide geometry DCLF does not draw in the main view (engine residue, shadow-only) has no verdict: its bit stays
				// the engine cull's.
				if (objectClass == 2 && o < tables.objects.size() &&
					(!(tables.objects[o].flags & kObjectMember) || (tables.objects[o].flags & (kObjectNoBindings | kObjectShadowOnly | kObjectFree)))) {
					if (entry != SunCandidates::kNone)
						entryResidue[entry] = 1;
					objectClass = 0;
				}
				if (!objectClass) {
					a_readback.objectWrittenGeometry[o] = nullptr;
					continue;
				}
				bool visible = cutPending;
				if (!visible && o < a_readback.objects.size())
					visible = a_readback.objects[o].visible || a_readback.objects[o].outFrame > window;
				if (entry != SunCandidates::kNone)
					entryState[entry] = std::max<std::uint8_t>(entryState[entry], visible ? 2 : 1);
				const std::uint8_t wideBit = objectClass == 2 ? 1 : 0;
				const std::uint8_t wanted = visible ? Readback::kWrittenSet : Readback::kWrittenClear;
				if (!full && a_readback.objectWritten[o] == wanted && a_readback.objectWrittenGeometry[o] == geometry && a_readback.objectWrittenWide[o] == wideBit)
					continue;
				a_readback.objectWritten[o] = wanted;
				a_readback.objectWrittenGeometry[o] = geometry;
				a_readback.objectWrittenWide[o] = wideBit;
				list->writes.push_back({ geometry, rootNode, visible, Readback::kWriteGeometry, wideBit != 0 });
			}
			if (a_readback.entryWritten.size() < entries) {
				a_readback.entryWritten.resize(entries, Readback::kWrittenNone);
				a_readback.entryWrittenVersion.resize(entries, 0u);
			}
			for (std::uint32_t e = 0; e < entries; ++e) {
				if (!entryState[e] || entryResidue[e]) {
					a_readback.entryWritten[e] = Readback::kWrittenNone;
					continue;
				}
				const bool visible = cutPending || entryState[e] == 2;
				const std::uint8_t wanted = visible ? Readback::kWrittenSet : Readback::kWrittenClear;
				const std::uint32_t version = e < candidates->entryVersion.size() ? candidates->entryVersion[e] : 0u;
				if (!full && a_readback.entryWritten[e] == wanted && a_readback.entryWrittenVersion[e] == version)
					continue;
				a_readback.entryWritten[e] = wanted;
				a_readback.entryWrittenVersion[e] = version;
				const void* node = candidates->entryNodes[e];
				list->writes.push_back({ const_cast<void*>(node), node, visible, Readback::kWriteEntry, true });
			}
			if (list->writes.empty()) {
				delete list;
				return nullptr;
			}
			a_readback.fullWrites.fetch_add(full ? 1 : 0, std::memory_order_relaxed);
			list->tables = a_newest.tables;
			if (candidates)
				list->candidates = a_newest.candidates;
			return list;
		}

		using Engine::kFlagAccumulated;
		using Engine::kFlagHidden;
		using Engine::kObjectFlags;
		using Engine::FlagsOf;

		/** @brief Main thread: sets or clears one node's kAccumulated atomically, skipping the RMW when already as wanted. */
		void Store(Readback& a_readback, void* a_node, bool a_set, bool a_wide = false)
		{
			std::atomic_ref<std::uint32_t> flags(*reinterpret_cast<std::uint32_t*>(static_cast<std::byte*>(a_node) + kObjectFlags));
			if (((flags.load(std::memory_order_relaxed) & kFlagAccumulated) != 0) == a_set) {
				(a_wide ? a_readback.wideUnchanged : a_readback.writesUnchanged).fetch_add(1, std::memory_order_relaxed);
				return;
			}
			if (a_set) {
				flags.fetch_or(kFlagAccumulated, std::memory_order_relaxed);
				(a_wide ? a_readback.wideSet : a_readback.writesSet).fetch_add(1, std::memory_order_relaxed);
			} else {
				flags.fetch_and(~kFlagAccumulated, std::memory_order_relaxed);
				(a_wide ? a_readback.wideCleared : a_readback.writesCleared).fetch_add(1, std::memory_order_relaxed);
			}
		}

		/**
		 * @brief Main thread: sets kAccumulated on the nodes between a geometry written visible and its root (receivers test every
		 * level; skyrim-engine-notes.md). Set only: a stale bit above an out-of-view geometry is harmless (receivers stop at the
		 * geometry's clear bit; the animation gate and tree clock read the root alone). The chain is written only if it reaches the
		 * root: the write's tables hold the geometry, a live node's parent is live (NiNode's destructor clears its children's parent
		 * pointers), and nothing attaches or detaches at Main::Update's entry.
		 */
		void SetChain(Readback& a_readback, RE::NiAVObject* a_geometry, const void* a_root)
		{
			auto& chain = a_readback.chain;
			chain.clear();
			RE::NiAVObject* node = a_geometry->parent;
			for (; node && node != a_root; node = node->parent)
				chain.push_back(node);
			if (!node) {
				a_readback.wideNoChain.fetch_add(1, std::memory_order_relaxed);
				if (a_readback.wideNoChainFirst.size() < 300) {
					const auto* root = static_cast<const RE::NiAVObject*>(a_root);
					const auto* top = chain.empty() ? a_geometry : chain.back();
					a_readback.wideNoChainFirst += fmt::format("{}'{}' under '{}' (top '{}')", a_readback.wideNoChainFirst.empty() ? "" : ", ",
						a_geometry->name.c_str() ? a_geometry->name.c_str() : "?", root && root->name.c_str() ? root->name.c_str() : "?",
						top->name.c_str() ? top->name.c_str() : "?");
				}
				return;
			}
			for (auto* between : chain) {
				std::atomic_ref<std::uint32_t> flags(*reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::byte*>(between) + kObjectFlags));
				if (!(flags.load(std::memory_order_relaxed) & kFlagAccumulated)) {
					flags.fetch_or(kFlagAccumulated, std::memory_order_relaxed);
					a_readback.wideNodesSet.fetch_add(1, std::memory_order_relaxed);
				}
			}
		}

		void Count(Readback::Counts& a_counts, bool a_dclf, bool a_engine)
		{
			auto& counter = a_dclf ? (a_engine ? a_counts.both : a_counts.dclfOnly) : (a_engine ? a_counts.engineOnly : a_counts.neither);
			counter.fetch_add(1, std::memory_order_relaxed);
		}


		/**
		 * @brief Main thread at Main::Update's entry: sets kAccumulated (set only) on the object root's non-hidden children, as
		 * DrawWorld_BuildSceneLists does every frame, for frames where DCLF builds the lists instead.
		 */
		void CellRoots(Readback& a_readback)
		{
			const auto* sceneNode = globals::game::smState ? globals::game::smState->shadowSceneNode[0] : nullptr;
			if (!sceneNode)
				return;
			auto* root = Engine::ObjectRoot(*sceneNode);
			auto* objectRoot = root ? root->AsNode() : nullptr;
			if (!objectRoot || Engine::IsHidden(objectRoot))
				return;
			// The engine's build (DrawWorld_BuildSceneLists, AE 0x14064bc20; drawcall-limit-fix.md): in an exterior it sets every
			// non-hidden child from the third on; children 0 and 1 are list entries (appended to list 0) whose bit the main camera's
			// list cull sets or clears. In an interior only the cell's 3D is a cell root. Containers are set too (receivers then test
			// the levels below; nothing else reads them).
			const auto* tes = RE::TES::GetSingleton();
			auto* interior = tes ? tes->interiorCell : nullptr;
			const auto* loaded = interior ? interior->GetRuntimeData().loadedData : nullptr;
			const RE::NiAVObject* interior3D = loaded ? loaded->cell3D.get() : nullptr;
			std::uint32_t index = 0;
			for (const auto& child : objectRoot->GetChildren()) {
				const bool container = interior ? child.get() != interior3D : index < 2;
				++index;
				auto* node = child ? child->AsNode() : nullptr;
				if (!node || node->GetFlags().any(RE::NiAVObject::Flag::kHidden))
					continue;
				if (!container)
					a_readback.cellRootsSeen.fetch_add(1, std::memory_order_relaxed);
				std::atomic_ref<std::uint32_t> flags(*reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::byte*>(node) + kObjectFlags));
				if (flags.load(std::memory_order_relaxed) & kFlagAccumulated)
					continue;
				flags.fetch_or(kFlagAccumulated, std::memory_order_relaxed);
				a_readback.cellRootsSet.fetch_add(1, std::memory_order_relaxed);
			}
		}

		/**
		 * @brief The <- ACCUMULATED observer: verdicts against the engine's bit as a_sample's frame left it. Moved slots, roots
		 * without a verdict and DCLF-only on app-culled roots are counted aside.
		 */
		void Compare(Readback& a_readback, Readback::Batch& a_batch, const Diagnostics::Accumulated::Sample& a_sample)
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
				const bool engine = (bits & Diagnostics::Accumulated::kSampleAccumulated) != 0;
				const bool listed = r < a_readback.roots.size() && a_readback.roots[r].generation == tables.fadeRoots[r].generation;
				if (!listed || !a_readback.roots[r].known) {
					++(engine ? noVerdictEngine : noVerdictNeither);
					continue;
				}
				const bool dclf = a_readback.roots[r].visible;
				if (dclf && !engine && (bits & Diagnostics::Accumulated::kSampleHidden)) {
					++hiddenDclfOnly;
					continue;
				}
				const bool stoodIn = (bits & Diagnostics::Accumulated::kSampleStoodIn) != 0;
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
				const bool engine = (bits & Diagnostics::Accumulated::kSampleAccumulated) != 0;
				const bool dclf = o < a_readback.objects.size() && a_readback.objects[o].visible;
				if (dclf && !engine && (bits & Diagnostics::Accumulated::kSampleHidden)) {
					++hiddenDclfOnly;
					continue;
				}
				const bool stoodIn = (bits & Diagnostics::Accumulated::kSampleStoodIn) != 0;
				Count(stoodIn ? a_readback.stoodInGeometryCounts : a_readback.geometryCounts, dclf, engine);
				if (stoodIn || dclf == engine)
					continue;
				// Engine-only under a root its cull failed: a stale bit from an earlier cull.
				if (engine && !(bits & Diagnostics::Accumulated::kSampleRootAccumulated))
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

		/** @brief Render thread: names a decoded batch's finds (its tables hold the nodes), then frees the batch and its tables. */
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
		 * @brief Render thread, at a frame slot's depth commit: copies the slot's last execution's events into a batch if the buffer
		 * holds the arming frame. The host has waited for that execution; the next is not submitted yet.
		 */
		void Harvest(Readback& a_readback, std::uint32_t a_frameSlot)
		{
			auto& armed = a_readback.armed[a_frameSlot];
			if (!armed.armed)
				return;
			armed.armed = false;
			// Events past the list's end are appended again next frame; the list grows (ReserveSceneTables).
			const bool found = a_readback.Harvest(a_frameSlot, armed.frame, [&](const std::uint32_t* a_words, std::uint32_t a_count, std::uint32_t a_capacity) {
				auto* batch = new Readback::Batch();
				batch->frame = armed.frame;
				batch->reset = armed.reset || std::exchange(a_readback.carryReset, false);
				batch->tables = std::move(armed.tables);
				batch->candidates = std::move(armed.candidates);
				batch->complete = a_count <= a_capacity;
				const auto* list = reinterpret_cast<const VisibilityEvent*>(a_words + kVisibilityEventHeaderWords);
				batch->events.assign(list, list + std::min(a_count, a_capacity));
				a_readback.pending.push_back(batch);
			});
			if (found)
				return;
			// No readback ran for that frame: the GPU reports its changes next frame; a carried reset goes with the next batch.
			a_readback.carryReset = a_readback.carryReset || armed.reset;
			armed.tables.reset();
			armed.candidates.reset();
		}
	}

	namespace
	{
		/** @brief Render thread: releases the last task's decoded batches (engine reads and publication retirement are its own). */
		void ReleaseDecoded(Readback& a_readback)
		{
			auto* batch = a_readback.decoded.exchange(nullptr, std::memory_order_acquire);
			while (batch) {
				auto* following = batch->next;
				Release(a_readback, batch);
				batch = following;
			}
		}

		/**
		 * @brief Render thread: hands the harvested batches to one decode task on DCLF's executor, never joined (one at a time).
		 */
		void Dispatch(const std::shared_ptr<Readback>& a_readback)
		{
			auto& readback = *a_readback;
			// Samples older than every armed frame and waiting batch will never be matched.
			std::uint32_t oldestPending = Scene::FrameView::Get().GetFrame();
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
				readback.samplesUnmatched.fetch_add(Diagnostics::Accumulated::DropSamples(oldestPending), std::memory_order_relaxed);
				readback.running.store(false, std::memory_order_release);
				readback.running.notify_all();
				return;
			}
			auto batches = std::exchange(readback.pending, {});
			std::stable_sort(batches.begin(), batches.end(), [](const auto* a_a, const auto* a_b) { return a_a->frame < a_b->frame; });
			// Engine samples (only taken under CS_DCLF_ACCUMULATED_PARITY).
			auto samples = std::make_shared<std::vector<std::unique_ptr<Diagnostics::Accumulated::Sample>>>(Diagnostics::Accumulated::TakeSamples(batches.back()->frame));
			readback.samplesUnmatched.fetch_add(Diagnostics::Accumulated::DropSamples(oldestPending), std::memory_order_relaxed);
			auto decode = [readback = a_readback, batches = std::move(batches), holder = std::move(samples)] {
				ZoneScopedN("CS.DCLF.VisibilityDecode");
				const auto& samples = *holder;
				const auto start = std::chrono::steady_clock::now();
				std::size_t next = 0;
				bool reset = false;
				for (auto* batch : batches) {
					reset = reset || batch->reset;
					Apply(*readback, *batch);
					// Earlier samples had no batch; this frame's is compared only if the batch is complete.
					for (; next < samples.size() && samples[next]->frame < batch->frame; ++next)
						readback->samplesUnmatched.fetch_add(1, std::memory_order_relaxed);
					if (next < samples.size() && samples[next]->frame == batch->frame) {
						if (batch->complete)
							Compare(*readback, *batch, *samples[next]);
						else
							readback->framesIncomplete.fetch_add(1, std::memory_order_relaxed);
						++next;
					}
				}
				readback->samplesUnmatched.fetch_add(samples.size() - next, std::memory_order_relaxed);
				if (auto* writes = PrepareWrites(*readback, *batches.back(), reset)) {
					writes->next = readback->writes.load(std::memory_order_relaxed);
					while (!readback->writes.compare_exchange_weak(writes->next, writes, std::memory_order_release, std::memory_order_relaxed)) {}
				}
				// Back to the render thread, which releases their tables.
				for (auto* batch : batches) {
					batch->next = readback->decoded.load(std::memory_order_relaxed);
					while (!readback->decoded.compare_exchange_weak(batch->next, batch, std::memory_order_release, std::memory_order_relaxed)) {}
				}
				readback->decodeNs.fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count()),
					std::memory_order_relaxed);
				readback->running.store(false, std::memory_order_release);
				readback->running.notify_all();
			};
			if (!SceneScheduler::Executor().Dispatch(SceneScheduler::Scope(), SceneExecutor::Preparation, org::async::TaskDispatch::Cpu, "visibility decode",
					[decode = std::move(decode)](const auto&) { decode(); }))
				stl::report_and_fail("Drawcall Limit Fix: the visibility decode was refused by DCLF's executor");
		}
	}

	void IndirectDraws::Impl::ArmVisibilityReadback(SceneBuffers& a_scene, CommitUploads& a_uploads)
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
		// Reset the header for this execution's VisibilityEventsCS.
		static constexpr std::uint32_t kZeroHeader[kVisibilityEventHeaderWords]{};
		a_uploads(a_scene.visibilityEvents, kZeroHeader, sizeof(kZeroHeader), 0);
		// A new backing of the reported words is zeroed, and the decoder restarts with this frame's batch.
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
		armed.frame = Scene::FrameView::Get().GetFrame();
		armed.reset = reset;
		armed.tables = Scene::FrameView::Get().AcceptedTables();
		// For the wide writes' plain entry roots.
		armed.candidates = Scene::FrameView::Get().GetSunCandidates();
		// Decode a harvested batch now, so its writes reach the next Main::Update (one frame less latency).
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
		// The stack is newest first: reverse into frame order.
		Readback::Writes* ordered = nullptr;
		for (auto* list = r.writes.exchange(nullptr, std::memory_order_acquire); list;) {
			auto* following = list->next;
			list->next = ordered;
			ordered = list;
			list = following;
		}
		// DCLF not live: write nothing and request a full rewrite once it is (the engine's cull writes the roots meanwhile).
		if (!a_live) {
			for (auto* list = ordered; list;) {
				auto* following = list->next;
				delete list;
				list = following;
			}
			r.writtenTables.reset();
			r.writtenCandidates.reset();
			r.rootVerdicts.clear();
			r.refreshWanted.store(true, std::memory_order_release);
			return;
		}
		const std::uint32_t cut = r.cutFrame.load(std::memory_order_acquire);
		const bool wrote = ordered != nullptr;
		for (auto* list = ordered; list;) {
			// A batch older than the last cut applies sets only; the post-cut full write restores the clears.
			const bool beforeCut = cut && list->frame < cut;
			for (const auto& write : list->writes) {
				const bool held = beforeCut && !write.set;
				if (held) {
					r.writesHeldByCut.fetch_add(1, std::memory_order_relaxed);
					continue;
				}
				Store(r, write.node, write.set, write.wide);
				if (write.kind != Readback::kWriteGeometry)
					r.rootVerdicts.insert_or_assign(write.node, write.set);
				else if (write.set)
					SetChain(r, static_cast<RE::NiAVObject*>(write.node), write.root);
			}
			r.writeBatches.fetch_add(1, std::memory_order_relaxed);
			// Keep the newest tables (a cut's writes read them); older ones are released here, on the main thread.
			r.writtenTables = std::move(list->tables);
			r.writtenCandidates = std::move(list->candidates);
			auto* following = list->next;
			delete list;
			list = following;
		}
		// A new camera cut: every listed root, its geometry, and the plain entry roots set visible now.
		if (cut && r.cutWritten != cut && r.writtenTables) {
			r.cutWritten = cut;
			const auto& tables = *r.writtenTables;
			std::vector<std::uint8_t> written(tables.fadeRootNode.size(), 0u);
			for (std::size_t root = 0; root < tables.fadeRootNode.size(); ++root) {
				const void* node = tables.fadeRootNode[root];
				if (!node || root >= tables.fadeRoots.size())
					continue;
				const bool stoodIn = (tables.fadeRoots[root].bits & kFadeRootStoodIn) && a_stoodIn(node);
				written[root] = 1;
				Store(r, const_cast<void*>(node), true, !stoodIn);
				r.rootVerdicts.insert_or_assign(node, true);
				r.cutWrites.fetch_add(1, std::memory_order_relaxed);
			}
			const auto* candidates = r.writtenCandidates.get();
			for (std::size_t o = 0; o < tables.objectGeometry.size(); ++o) {
				auto* geometry = tables.objectGeometry[o];
				if (!geometry || tables.IsLayer(static_cast<std::uint32_t>(o)))
					continue;
				const std::uint32_t root = o < tables.objectFadeRoot.size() ? tables.objectFadeRoot[o] : kNoFadeRoot;
				if (root < written.size() && written[root]) {
					Store(r, geometry, true, !(tables.fadeRoots[root].bits & kFadeRootStoodIn));
					SetChain(r, geometry, tables.fadeRootNode[root]);
					continue;
				}
				// A plain entry root's geometry (no fade root).
				if (root != kNoFadeRoot || !candidates)
					continue;
				const auto it = candidates->geometries.find(geometry);
				const std::uint32_t entry = it != candidates->geometries.end() && it->second < candidates->geometryEntry.size() ? candidates->geometryEntry[it->second] : SunCandidates::kNone;
				const RE::NiAVObject* entryNode = entry < candidates->entryNodes.size() ? candidates->entryNodes[entry] : nullptr;
				if (!entryNode || tables.fadeRootIndex.contains(entryNode))
					continue;
				r.rootVerdicts.insert_or_assign(entryNode, true);
				Store(r, const_cast<RE::NiAVObject*>(entryNode), true, true);
				Store(r, geometry, true, true);
				SetChain(r, geometry, entryNode);
			}
		}
		// Every 60th write pass, forget verdicts of roots no longer listed (lookups check the listing themselves).
		if (wrote && ++r.verdictPrunes >= 60) {
			r.verdictPrunes = 0;
			for (auto it = r.rootVerdicts.begin(); it != r.rootVerdicts.end();)
				it = (r.writtenTables && r.writtenTables->fadeRootIndex.contains(it->first)) ||
				             (r.writtenCandidates && r.writtenCandidates->entries.contains(static_cast<const RE::NiAVObject*>(it->first))) ?
				         std::next(it) :
				         r.rootVerdicts.erase(it);
		}
		CellRoots(r);
		r.writeNs.fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count()),
			std::memory_order_relaxed);
	}

	bool IndirectDraws::AccumulatedVerdict(const void* a_root) const
	{
		if (!impl->scene || !impl->scene->visibilityReadback)
			return true;
		const auto& verdicts = impl->scene->visibilityReadback->rootVerdicts;
		const auto it = verdicts.find(a_root);
		return it == verdicts.end() || it->second;
	}

	bool IndirectDraws::AccumulatedVerdictOf(const void* a_root, bool& a_visible) const
	{
		if (!impl->scene || !impl->scene->visibilityReadback)
			return false;
		const auto& r = *impl->scene->visibilityReadback;
		const auto it = r.rootVerdicts.find(a_root);
		if (it == r.rootVerdicts.end())
			return false;
		// Must still be listed: a kept verdict for a freed node must not answer for another at its address.
		if (!(r.writtenTables && r.writtenTables->fadeRootIndex.contains(a_root)) &&
			!(r.writtenCandidates && r.writtenCandidates->entries.contains(static_cast<const RE::NiAVObject*>(a_root))))
			return false;
		a_visible = it->second;
		return true;
	}

	void IndirectDraws::Impl::WaitVisibilityReadback()
	{
		if (!scene || !scene->visibilityReadback)
			return;
		auto& running = scene->visibilityReadback->running;
		while (running.load(std::memory_order_acquire))
			running.wait(true, std::memory_order_acquire);
	}

}

#endif
