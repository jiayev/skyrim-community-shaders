// The visibility readback (Readbacks.h, VisibilityReadback; dclf-architecture.md, "The visibility events"): the main camera's visibility as GPU change
// events (VisibilityEventsCS), armed and harvested by the depth commit (render thread), decoded into verdicts by a task on DCLF's
// executor, and written as kAccumulated by the main thread at Main::Update's entry on the roots DCLF lists and their geometry.
// The <- ACCUMULATED observer against the engine's bit runs only under CS_DCLF_ACCUMULATED_PARITY.

#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Diagnostics/Engine/Accumulated.h"
#include "Features/DrawcallLimitFix/Core/Scene/Coordinator/EntrySpaceTable.h"

namespace DCLF
{
	using namespace Draws;

	namespace
	{
		using Readback = VisibilityReadback;

		inline constexpr std::uint32_t kUnlisted = ~0u;
		// An object's share in its plain entry (VisibilityDecode::objectShare): counted (listed), in view, an engine residue.
		inline constexpr std::uint8_t kShareListed = 1, kShareVisible = 2, kShareResidue = 4;

		/**
		 * @brief PrepareWrites' own state (Readback::decodeState). A pass visits only what can have changed: the slots the batches'
		 * events name, the verdicts whose out-of-view window has passed (expiries, by frame), the object slots the tables' change log
		 * names, the root rows the fade-root journal marked since the rows the classes were judged from, and the plain entries the
		 * candidates moved; plus the objects a root's or an entry's change reaches, through the indexes kept here. Only a reset, a
		 * camera cut and a refresh (and a log or journal it cannot read on) visit everything.
		 */
		struct VisibilityDecode
		{
			struct Expiry
			{
				std::uint32_t frame = 0;  // the frame the slot went out of view: judged again once the window reaches it
				std::uint32_t slot = 0;
				bool root = false;
				bool operator>(const Expiry& a_other) const { return frame > a_other.frame; }
			};
			MarkedList roots, objects, entries;  // this pass's visits
			std::vector<Expiry> expiries;        // a min-heap by frame
			LogCursor changes;                   // tables.changeLog
			std::uint64_t rootEpoch = 0;         // the root states' family's journal content epoch the root classes were judged at
			bool rootsHeld = false;
			std::uint32_t cutVisited = 0;        // the cut whose pending pass visited everything
			std::shared_ptr<const EntrySpace> candidates;  // the snapshot whose indices entryMembers are in
			// Per root slot: its class as last judged (0 not written, 1 stood in, 2 wide), its node, the object slots under it.
			std::vector<std::uint8_t> rootClass;
			std::vector<const void*> rootNode;
			std::vector<std::vector<std::uint32_t>> rootMembers;
			// Per object slot: the root and the plain entry it is listed under (kUnlisted) with its place in their lists, its share
			// in the entry, and its occupant as last judged (the occupants' slots find an entry's new geometries).
			std::vector<std::uint32_t> objectRoot, objectRootAt, objectEntry, objectEntryAt;
			std::vector<std::uint8_t> objectShare;
			std::vector<const void*> objectGeometry;
			ankerl::unordered_dense::map<const void*, std::uint32_t> geometrySlot;
			// Per plain entry: its object slots, and how many of them count listed, in view, as an engine residue.
			std::vector<std::vector<std::uint32_t>> entryMembers;
			std::vector<std::uint32_t> entryListed, entryVisible, entryResidue;
			// Since the last line (CS_DCLF_STATS): passes, their time, slots visited and whole passes; the parity observer's.
			std::uint64_t serial = 0, passes = 0, prepareNs = 0, visits = 0, wholePasses = 0, parityFrames = 0, parityDiffs = 0;
		};

		/** @brief The decode task's state (Readback::decodeState), made on its first use. Only the decode task touches it. */
		VisibilityDecode& DecodeOf(Readback& a_readback)
		{
			if (!a_readback.decodeState)
				a_readback.decodeState = std::make_shared<VisibilityDecode>();
			return *static_cast<VisibilityDecode*>(a_readback.decodeState.get());
		}

		/** @brief Clears the decoder's verdicts (a reset frame: the reported words have a new, empty backing). */
		void StartAgain(Readback& a_readback, VisibilityDecode& a_decode)
		{
			a_readback.roots.assign(a_readback.roots.size(), {});
			a_readback.objects.assign(a_readback.objects.size(), {});
			a_readback.rootApplied.assign(a_readback.rootApplied.size(), 0u);
			a_readback.objectApplied.assign(a_readback.objectApplied.size(), 0u);
			a_readback.decodedFrame = a_readback.previousDecodedFrame = 0;
			// The reset's pass visits everything.
			a_decode.expiries.clear();
		}

		/** @brief A slot to judge again once the window reaches a_frame (it went out of view then). */
		void Expire(VisibilityDecode& a_decode, std::uint32_t a_frame, std::uint32_t a_slot, bool a_root)
		{
			a_decode.expiries.push_back({ a_frame, a_slot, a_root });
			std::push_heap(a_decode.expiries.begin(), a_decode.expiries.end(), std::greater<>{});
		}

		/** @brief Decode task: applies one batch to the verdicts, against its frame's tables. Each slot it takes is visited. */
		void Apply(Readback& a_readback, VisibilityDecode& a_decode, const Readback::Batch& a_batch)
		{
			const auto& tables = *a_batch.tables;
			const auto rootList = Extensions::RootListOf(tables);
			if (a_batch.reset)
				StartAgain(a_readback, a_decode);
			if (a_readback.roots.size() < rootList.rows.size()) {
				a_readback.roots.resize(rootList.rows.size());
				a_readback.rootApplied.resize(rootList.rows.size(), 0u);
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
					if (event.slot >= rootList.rows.size() || rootList.rows[event.slot].generation != event.generation || !rootList.nodes[event.slot] ||
						frame < a_readback.rootApplied[event.slot]) {
						++stale;
						continue;
					}
					a_readback.rootApplied[event.slot] = frame;
					auto& root = a_readback.roots[event.slot];
					root.generation = event.generation;
					root.known = (event.bits & kVisibilityBitVerdict) != 0;
					if (root.visible && !visible) {
						root.outFrame = frame;
						Expire(a_decode, frame, event.slot, true);
					}
					root.visible = visible;
					if (visible)
						root.lastFrame = frame;
					a_decode.roots.Add(event.slot);
				} else {
					if (event.slot >= tables.objects.size() || !tables.objectGeometry[event.slot] || frame < a_readback.objectApplied[event.slot]) {
						++stale;
						continue;
					}
					a_readback.objectApplied[event.slot] = frame;
					auto& object = a_readback.objects[event.slot];
					if (object.visible && !visible) {
						object.outFrame = frame;
						Expire(a_decode, frame, event.slot, false);
					}
					object.visible = visible;
					if (visible)
						object.lastFrame = frame;
					a_decode.objects.Add(event.slot);
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

		/** @brief The asks PrepareWrites compares with, copied for the parity observer's reference pass. */
		struct Asked
		{
			std::vector<std::uint32_t> rootWrittenGeneration;
			std::vector<std::uint8_t> rootWritten, objectWritten;
			std::vector<const void*> objectWrittenGeometry;
			std::vector<std::uint8_t> rootWrittenWide, objectWrittenWide, entryWritten;
			std::vector<std::uint32_t> entryWrittenVersion;
			const EntrySpace* entryCandidates = nullptr;
			std::vector<std::uint32_t> objectEntry;
			std::vector<const void*> objectEntryGeometry;

			explicit Asked(const Readback& a_readback) :
				rootWrittenGeneration(a_readback.rootWrittenGeneration), rootWritten(a_readback.rootWritten), objectWritten(a_readback.objectWritten),
				objectWrittenGeometry(a_readback.objectWrittenGeometry), rootWrittenWide(a_readback.rootWrittenWide), objectWrittenWide(a_readback.objectWrittenWide),
				entryWritten(a_readback.entryWritten), entryWrittenVersion(a_readback.entryWrittenVersion), entryCandidates(a_readback.entryCandidates),
				objectEntry(a_readback.objectEntry), objectEntryGeometry(a_readback.objectEntryGeometry)
			{}
		};

		/** @brief ReferenceWrites' shared locals: the reference's inputs, its asks and its writes. */
		struct ReferencePass
		{
			const Readback& readback;
			Asked& asked;
			const SceneTables& tables;
			const Api::RootListView& rootList;
			std::size_t roots, objects;  // the tables'
			std::uint32_t window;
			bool full, cutPending;
			std::vector<Readback::Writes::Write>& writes;
		};

		/** @brief The reference's roots: every listed root's write, and per root slot its class (1 stood in, 2 wide) into a_rootClass. */
		void ReferenceRoots(const ReferencePass& a_ref, std::vector<std::uint8_t>& a_rootClass)
		{
			const auto& rootList = a_ref.rootList;
			auto& rootClass = a_rootClass;
			for (std::size_t r = 0; r < a_ref.roots; ++r) {
				const void* node = rootList.nodes[r];
				const bool stoodIn = node && r < rootList.rows.size() && (rootList.rows[r].bits & kFadeRootStoodIn);
				if (!node || r >= rootList.rows.size()) {
					a_ref.asked.rootWritten[r] = Readback::kWrittenNone;
					continue;
				}
				const auto& row = rootList.rows[r];
				rootClass[r] = stoodIn ? 1 : 2;
				bool visible = a_ref.cutPending;
				const bool listed = r < a_ref.readback.roots.size() && a_ref.readback.roots[r].generation == row.generation && a_ref.readback.roots[r].known;
				if (!visible)
					visible = !listed || a_ref.readback.roots[r].visible || a_ref.readback.roots[r].outFrame > a_ref.window;
				const std::uint8_t wanted = visible ? Readback::kWrittenSet : Readback::kWrittenClear;
				const std::uint8_t wideBit = stoodIn ? 0 : 1;
				if (!a_ref.full && a_ref.asked.rootWritten[r] == wanted && a_ref.asked.rootWrittenGeneration[r] == row.generation && a_ref.asked.rootWrittenWide[r] == wideBit)
					continue;
				a_ref.asked.rootWritten[r] = wanted;
				a_ref.asked.rootWrittenGeneration[r] = row.generation;
				a_ref.asked.rootWrittenWide[r] = wideBit;
				a_ref.writes.push_back({ const_cast<void*>(node), node, visible, Readback::kWriteRoot, !stoodIn });
			}
		}

		/**
		 * @brief The reference's objects: every drawn geometry's write by its root's class or its plain entry's, and per entry its
		 * state (1 listed, 2 visible) and engine residue into a_entryState and a_entryResidue.
		 */
		void ReferenceObjects(const ReferencePass& a_ref, const EntrySpace* a_candidates, std::uint32_t a_entries, const std::vector<std::uint8_t>& a_rootClass,
			std::vector<std::uint8_t>& a_entryState, std::vector<std::uint8_t>& a_entryResidue)
		{
			const auto& tables = a_ref.tables;
			const auto& rootList = a_ref.rootList;
			const auto* candidates = a_candidates;
			const std::uint32_t entries = a_entries;
			const auto& rootClass = a_rootClass;
			auto& asked = a_ref.asked;
			auto& entryState = a_entryState;
			auto& entryResidue = a_entryResidue;
			for (std::size_t o = 0; o < a_ref.objects; ++o) {
				auto* geometry = tables.objectGeometry[o];
				const std::uint32_t root = rootList.RootOf(static_cast<std::uint32_t>(o));
				std::uint8_t objectClass = 0;
				const void* rootNode = nullptr;
				std::uint32_t entry = EntrySpace::kNone;
				if (geometry && !tables.IsLayer(static_cast<std::uint32_t>(o))) {
					if (root != kNoFadeRoot && root < a_ref.roots && rootClass[root]) {
						objectClass = rootClass[root];
						rootNode = rootList.nodes[root];
					} else if (root == kNoFadeRoot && candidates) {
						if (asked.objectEntryGeometry[o] != geometry) {
							asked.objectEntryGeometry[o] = geometry;
							asked.objectEntry[o] = EntrySpace::kNone;
							if (const auto it = candidates->geometries.find(geometry); it != candidates->geometries.end() && it->second < candidates->geometryEntry.size())
								asked.objectEntry[o] = candidates->geometryEntry[it->second];
						}
						entry = asked.objectEntry[o];
						const bool entryIsRoot = entry < entries && candidates->entryNodes[entry] && rootList.Contains(candidates->entryNodes[entry]);
						if (entry < entries && candidates->entryNodes[entry] && !entryIsRoot) {
							objectClass = 2;
							rootNode = candidates->entryNodes[entry];
						} else {
							entry = EntrySpace::kNone;
						}
					}
				}
				if (objectClass == 2 && o < tables.objects.size() &&
					(!(tables.objects[o].flags & kObjectMember) || (tables.objects[o].flags & (kObjectNoBindings | kObjectShadowOnly | kObjectFree)))) {
					if (entry != EntrySpace::kNone)
						entryResidue[entry] = 1;
					objectClass = 0;
				}
				if (!objectClass) {
					asked.objectWrittenGeometry[o] = nullptr;
					continue;
				}
				bool visible = a_ref.cutPending;
				if (!visible && o < a_ref.readback.objects.size())
					visible = a_ref.readback.objects[o].visible || a_ref.readback.objects[o].outFrame > a_ref.window;
				if (entry != EntrySpace::kNone)
					entryState[entry] = std::max<std::uint8_t>(entryState[entry], visible ? 2 : 1);
				const std::uint8_t wideBit = objectClass == 2 ? 1 : 0;
				const std::uint8_t wanted = visible ? Readback::kWrittenSet : Readback::kWrittenClear;
				if (!a_ref.full && asked.objectWritten[o] == wanted && asked.objectWrittenGeometry[o] == geometry && asked.objectWrittenWide[o] == wideBit)
					continue;
				asked.objectWritten[o] = wanted;
				asked.objectWrittenGeometry[o] = geometry;
				asked.objectWrittenWide[o] = wideBit;
				a_ref.writes.push_back({ geometry, rootNode, visible, Readback::kWriteGeometry, wideBit != 0 });
			}
		}

		/** @brief The reference's plain entry roots: each counted entry's write, none while one of its geometries is an engine residue. */
		void ReferenceEntries(const ReferencePass& a_ref, const EntrySpace* a_candidates, std::uint32_t a_entries, const std::vector<std::uint8_t>& a_entryState,
			const std::vector<std::uint8_t>& a_entryResidue)
		{
			const auto* candidates = a_candidates;
			const std::uint32_t entries = a_entries;
			const auto& entryState = a_entryState;
			const auto& entryResidue = a_entryResidue;
			auto& asked = a_ref.asked;
			if (asked.entryWritten.size() < entries) {
				asked.entryWritten.resize(entries, Readback::kWrittenNone);
				asked.entryWrittenVersion.resize(entries, 0u);
			}
			for (std::uint32_t e = 0; e < entries; ++e) {
				if (!entryState[e] || entryResidue[e]) {
					asked.entryWritten[e] = Readback::kWrittenNone;
					continue;
				}
				const bool visible = a_ref.cutPending || entryState[e] == 2;
				const std::uint8_t wanted = visible ? Readback::kWrittenSet : Readback::kWrittenClear;
				const std::uint32_t version = e < candidates->entryVersion.size() ? candidates->entryVersion[e] : 0u;
				if (!a_ref.full && asked.entryWritten[e] == wanted && asked.entryWrittenVersion[e] == version)
					continue;
				asked.entryWritten[e] = wanted;
				asked.entryWrittenVersion[e] = version;
				const void* node = candidates->entryNodes[e];
				a_ref.writes.push_back({ const_cast<void*>(node), node, visible, Readback::kWriteEntry, true });
			}
		}

		/**
		 * @brief The parity observer's reference: every listed root, object and plain entry judged against a_asked (the asks before
		 * the pass), as PrepareWrites did before it visited only changes. Reads no engine memory.
		 */
		std::vector<Readback::Writes::Write> ReferenceWrites(const Readback& a_readback, Asked& a_asked, const Readback::Batch& a_newest, bool a_full, bool a_cutPending)
		{
			const auto& tables = *a_newest.tables;
			const auto rootList = Extensions::RootListOf(tables);
			const bool full = a_full, cutPending = a_cutPending;
			const std::size_t roots = rootList.nodes.size(), objects = tables.objectGeometry.size();
			if (a_asked.rootWritten.size() < roots) {
				a_asked.rootWritten.resize(roots, Readback::kWrittenNone);
				a_asked.rootWrittenGeneration.resize(roots, 0u);
				a_asked.rootWrittenWide.resize(roots, 0u);
			}
			if (a_asked.objectWritten.size() < objects) {
				a_asked.objectWritten.resize(objects, Readback::kWrittenNone);
				a_asked.objectWrittenGeometry.resize(objects, nullptr);
				a_asked.objectWrittenWide.resize(objects, 0u);
			}
			const std::uint32_t window = a_readback.previousDecodedFrame;
			std::vector<Readback::Writes::Write> writes;
			const ReferencePass ref{ a_readback, a_asked, tables, rootList, roots, objects, window, full, cutPending, writes };
			// Per root slot: 1 stood in, 2 wide (its geometry takes the same class).
			std::vector<std::uint8_t> rootClass(roots, 0u);
			ReferenceRoots(ref, rootClass);
			const EntrySpace* candidates = a_newest.candidates.get();
			if (candidates != a_asked.entryCandidates) {
				a_asked.entryCandidates = candidates;
				a_asked.objectEntry.assign(a_asked.objectEntry.size(), EntrySpace::kNone);
				a_asked.objectEntryGeometry.assign(a_asked.objectEntryGeometry.size(), nullptr);
			}
			if (a_asked.objectEntry.size() < objects) {
				a_asked.objectEntry.resize(objects, EntrySpace::kNone);
				a_asked.objectEntryGeometry.resize(objects, nullptr);
			}
			const std::uint32_t entries = candidates ? candidates->Capacity() : 0u;
			// Per entry: 1 listed, 2 visible; an entry with an engine-residue geometry keeps the engine's bit.
			std::vector<std::uint8_t> entryState(entries, 0u), entryResidue(entries, 0u);
			ReferenceObjects(ref, candidates, entries, rootClass, entryState, entryResidue);
			ReferenceEntries(ref, candidates, entries, entryState, entryResidue);
			return writes;
		}

		/** @brief One PrepareWrites pass's inputs and output. */
		struct Pass
		{
			Readback& readback;
			VisibilityDecode& decode;
			const SceneTables& tables;
			const Api::RootListView& rootList;  // the tables' listed roots (the root states' view)
			const EntrySpace* candidates;
			std::uint32_t entries;      // the candidates' index space
			std::size_t roots, objects;  // the tables'
			std::uint32_t window;        // the conservative rule's: the older of the last two decoded frames
			bool cutPending, full;
			Readback::Writes& list;
		};

		/** @brief Takes a_slot out of its owner's list (a_of: per slot its owner, a_at: its place there). */
		void Unlist(std::vector<std::vector<std::uint32_t>>& a_lists, std::vector<std::uint32_t>& a_of, std::vector<std::uint32_t>& a_at, std::uint32_t a_slot)
		{
			const std::uint32_t owner = a_of[a_slot];
			if (owner == kUnlisted)
				return;
			auto& list = a_lists[owner];
			const std::uint32_t at = a_at[a_slot];
			list[at] = list.back();
			a_at[list[at]] = at;
			list.pop_back();
			a_of[a_slot] = kUnlisted;
		}

		void List(std::vector<std::vector<std::uint32_t>>& a_lists, std::vector<std::uint32_t>& a_of, std::vector<std::uint32_t>& a_at, std::uint32_t a_slot,
			std::uint32_t a_owner)
		{
			if (a_lists.size() <= a_owner)
				a_lists.resize(std::size_t(a_owner) + 1);
			a_of[a_slot] = a_owner;
			a_at[a_slot] = static_cast<std::uint32_t>(a_lists[a_owner].size());
			a_lists[a_owner].push_back(a_slot);
		}

		void SizeRoots(VisibilityDecode& a_decode, std::size_t a_count)
		{
			if (a_decode.rootClass.size() >= a_count)
				return;
			a_decode.rootClass.resize(a_count, 0u);
			a_decode.rootNode.resize(a_count, nullptr);
			if (a_decode.rootMembers.size() < a_count)
				a_decode.rootMembers.resize(a_count);
		}

		void SizeObjects(VisibilityDecode& a_decode, std::size_t a_count)
		{
			if (a_decode.objectRoot.size() >= a_count)
				return;
			a_decode.objectRoot.resize(a_count, kUnlisted);
			a_decode.objectRootAt.resize(a_count, 0u);
			a_decode.objectEntry.resize(a_count, kUnlisted);
			a_decode.objectEntryAt.resize(a_count, 0u);
			a_decode.objectShare.resize(a_count, 0u);
			a_decode.objectGeometry.resize(a_count, nullptr);
		}

		void SizeEntries(VisibilityDecode& a_decode, std::size_t a_count)
		{
			if (a_decode.entryListed.size() >= a_count)
				return;
			a_decode.entryListed.resize(a_count, 0u);
			a_decode.entryVisible.resize(a_count, 0u);
			a_decode.entryResidue.resize(a_count, 0u);
			if (a_decode.entryMembers.size() < a_count)
				a_decode.entryMembers.resize(a_count);
		}

		/** @brief An object's share in its plain entry; the entry is visited when its counts move. */
		void Share(VisibilityDecode& a_decode, std::uint32_t a_object, std::uint8_t a_share)
		{
			const std::uint8_t previous = a_decode.objectShare[a_object];
			if (previous == a_share)
				return;
			a_decode.objectShare[a_object] = a_share;
			const std::uint32_t entry = a_decode.objectEntry[a_object];
			if (entry == kUnlisted)
				return;
			const auto count = [&](std::uint8_t a_bits, bool a_add) {
				const auto step = [&](std::uint32_t& a_counter) { a_counter = a_add ? a_counter + 1 : a_counter - 1; };
				if (a_bits & kShareListed)
					step(a_decode.entryListed[entry]);
				if (a_bits & kShareVisible)
					step(a_decode.entryVisible[entry]);
				if (a_bits & kShareResidue)
					step(a_decode.entryResidue[entry]);
			};
			count(previous, false);
			count(a_share, true);
			a_decode.entries.Add(entry);
		}

		/** @brief A root slot: its write when it differs from the last asked; a changed class or node visits the objects under it. */
		void JudgeRoot(Pass& a_pass, std::uint32_t a_root)
		{
			auto& readback = a_pass.readback;
			auto& decode = a_pass.decode;
			SizeRoots(decode, std::size_t(a_root) + 1);
			const auto& rootList = a_pass.rootList;
			const void* node = a_root < a_pass.roots ? rootList.nodes[a_root] : nullptr;
			std::uint8_t rootClass = 0;
			if (!node || a_root >= rootList.rows.size()) {
				// Not written: forget the last write so a return writes again.
				node = nullptr;
				if (a_root < readback.rootWritten.size())
					readback.rootWritten[a_root] = Readback::kWrittenNone;
			} else {
				const auto& row = rootList.rows[a_root];
				const bool stoodIn = (row.bits & kFadeRootStoodIn) != 0;
				rootClass = stoodIn ? 1 : 2;
				bool visible = a_pass.cutPending;
				const bool listed = a_root < readback.roots.size() && readback.roots[a_root].generation == row.generation && readback.roots[a_root].known;
				if (!visible)
					visible = !listed || readback.roots[a_root].visible || readback.roots[a_root].outFrame > a_pass.window;
				const std::uint8_t wanted = visible ? Readback::kWrittenSet : Readback::kWrittenClear;
				const std::uint8_t wideBit = stoodIn ? 0 : 1;
				if (a_pass.full || readback.rootWritten[a_root] != wanted || readback.rootWrittenGeneration[a_root] != row.generation ||
					readback.rootWrittenWide[a_root] != wideBit) {
					readback.rootWritten[a_root] = wanted;
					readback.rootWrittenGeneration[a_root] = row.generation;
					readback.rootWrittenWide[a_root] = wideBit;
					a_pass.list.writes.push_back({ const_cast<void*>(node), node, visible, Readback::kWriteRoot, !stoodIn });
				}
			}
			if (rootClass == decode.rootClass[a_root] && node == decode.rootNode[a_root])
				return;
			// Its geometry takes its class; a node listed or unlisted as a fade root also turns its plain entry's geometry off or on.
			for (const std::uint32_t object : decode.rootMembers[a_root])
				decode.objects.Add(object);
			if (a_pass.candidates)
				for (const void* changed : { decode.rootNode[a_root], node })
					if (const auto it = changed ? a_pass.candidates->entries.find(static_cast<const RE::NiAVObject*>(changed)) : a_pass.candidates->entries.end();
						it != a_pass.candidates->entries.end() && it->second < decode.entryMembers.size())
						for (const std::uint32_t object : decode.entryMembers[it->second])
							decode.objects.Add(object);
			decode.rootClass[a_root] = rootClass;
			decode.rootNode[a_root] = node;
		}

		/** @brief An object slot's geometry indexed by its node (geometrySlot) and its place under its root (rootMembers). */
		inline void IndexObject(VisibilityDecode& a_decode, std::uint32_t a_object, RE::BSGeometry* a_geometry, bool a_drawn, std::uint32_t a_root)
		{
			auto& decode = a_decode;
			auto* geometry = a_geometry;
			const std::uint32_t root = a_root;
			if (decode.objectGeometry[a_object] != geometry) {
				if (const auto it = decode.geometrySlot.find(decode.objectGeometry[a_object]); it != decode.geometrySlot.end() && it->second == a_object)
					decode.geometrySlot.erase(it);
				if (geometry)
					decode.geometrySlot.insert_or_assign(geometry, a_object);
				decode.objectGeometry[a_object] = geometry;
			}
			if (const std::uint32_t memberOf = a_drawn && root != kNoFadeRoot ? root : kUnlisted; memberOf != decode.objectRoot[a_object]) {
				Unlist(decode.rootMembers, decode.objectRoot, decode.objectRootAt, a_object);
				if (memberOf != kUnlisted) {
					SizeRoots(decode, std::size_t(memberOf) + 1);
					List(decode.rootMembers, decode.objectRoot, decode.objectRootAt, a_object, memberOf);
				}
			}
		}

		/** @brief A drawn object's class (1 stood in, 2 wide) and write root, by its root's class or its plain entry's. */
		struct ObjectClass
		{
			std::uint8_t objectClass = 0;
			const void* rootNode = nullptr;
			std::uint32_t entry = EntrySpace::kNone, entryOf = kUnlisted;
		};

		inline ObjectClass ClassifyObject(Pass& a_pass, std::uint32_t a_object, RE::BSGeometry* a_geometry, bool a_drawn, std::uint32_t a_root)
		{
			auto& readback = a_pass.readback;
			const auto& decode = a_pass.decode;
			const auto& rootList = a_pass.rootList;
			auto* geometry = a_geometry;
			const std::uint32_t root = a_root;
			ObjectClass out;
			auto& objectClass = out.objectClass;
			auto& rootNode = out.rootNode;
			auto& entry = out.entry;
			auto& entryOf = out.entryOf;
			const auto* candidates = a_pass.candidates;
			if (a_drawn) {
				if (root != kNoFadeRoot && root < a_pass.roots && root < decode.rootClass.size() && decode.rootClass[root]) {
					objectClass = decode.rootClass[root];
					rootNode = rootList.nodes[root];
				} else if (root == kNoFadeRoot && candidates) {
					if (readback.objectEntryGeometry[a_object] != geometry) {
						readback.objectEntryGeometry[a_object] = geometry;
						readback.objectEntry[a_object] = EntrySpace::kNone;
						if (const auto it = candidates->geometries.find(geometry); it != candidates->geometries.end() && it->second < candidates->geometryEntry.size())
							readback.objectEntry[a_object] = candidates->geometryEntry[it->second];
					}
					entry = readback.objectEntry[a_object];
					if (entry < a_pass.entries && candidates->entryNodes[entry]) {
						entryOf = entry;
						if (!rootList.Contains(candidates->entryNodes[entry])) {
							objectClass = 2;
							rootNode = candidates->entryNodes[entry];
						} else {
							entry = EntrySpace::kNone;
						}
					} else {
						entry = EntrySpace::kNone;
					}
				}
			}
			return out;
		}

		/** @brief An object slot's place in its plain entry's list (a_entryOf: kUnlisted for none); a move takes its share out first. */
		inline void ListUnderEntry(Pass& a_pass, std::uint32_t a_object, std::uint32_t a_entryOf)
		{
			auto& readback = a_pass.readback;
			auto& decode = a_pass.decode;
			const std::uint32_t entryOf = a_entryOf;
			// The cached entry holds only while the object is listed under it (an entry's move visits its members alone).
			if (entryOf == kUnlisted && a_object < readback.objectEntryGeometry.size())
				readback.objectEntryGeometry[a_object] = nullptr;
			if (entryOf != decode.objectEntry[a_object]) {
				Share(decode, a_object, 0);
				Unlist(decode.entryMembers, decode.objectEntry, decode.objectEntryAt, a_object);
				if (entryOf != kUnlisted) {
					SizeEntries(decode, std::size_t(entryOf) + 1);
					List(decode.entryMembers, decode.objectEntry, decode.objectEntryAt, a_object, entryOf);
				}
			}
		}

		/** @brief An object slot: its write when it differs from the last asked, its place under its root and its plain entry. */
		void JudgeObject(Pass& a_pass, std::uint32_t a_object)
		{
			auto& readback = a_pass.readback;
			auto& decode = a_pass.decode;
			const auto& tables = a_pass.tables;
			SizeObjects(decode, std::size_t(a_object) + 1);
			const bool held = a_object < a_pass.objects;
			auto* geometry = held ? tables.objectGeometry[a_object] : nullptr;
			const bool drawn = geometry && !tables.IsLayer(a_object);
			const auto& rootList = a_pass.rootList;
			const std::uint32_t root = held ? rootList.RootOf(a_object) : kNoFadeRoot;
			IndexObject(decode, a_object, geometry, drawn, root);
			auto [objectClass, rootNode, entry, entryOf] = ClassifyObject(a_pass, a_object, geometry, drawn, root);
			ListUnderEntry(a_pass, a_object, entryOf);
			// A wide geometry DCLF does not draw in the main view (engine residue, shadow-only) has no verdict: its bit stays the
			// engine cull's (and its plain entry's too).
			std::uint8_t share = 0;
			if (objectClass == 2 && a_object < tables.objects.size() &&
				(!(tables.objects[a_object].flags & kObjectMember) || (tables.objects[a_object].flags & (kObjectNoBindings | kObjectShadowOnly | kObjectFree)))) {
				if (entry != EntrySpace::kNone)
					share = kShareResidue;
				objectClass = 0;
			}
			if (!objectClass) {
				Share(decode, a_object, share);
				if (a_object < readback.objectWrittenGeometry.size())
					readback.objectWrittenGeometry[a_object] = nullptr;
				return;
			}
			bool visible = a_pass.cutPending;
			if (!visible && a_object < readback.objects.size())
				visible = readback.objects[a_object].visible || readback.objects[a_object].outFrame > a_pass.window;
			if (entry != EntrySpace::kNone)
				share = visible ? kShareListed | kShareVisible : kShareListed;
			Share(decode, a_object, share);
			const std::uint8_t wideBit = objectClass == 2 ? 1 : 0;
			const std::uint8_t wanted = visible ? Readback::kWrittenSet : Readback::kWrittenClear;
			if (!a_pass.full && readback.objectWritten[a_object] == wanted && readback.objectWrittenGeometry[a_object] == geometry &&
				readback.objectWrittenWide[a_object] == wideBit)
				return;
			readback.objectWritten[a_object] = wanted;
			readback.objectWrittenGeometry[a_object] = geometry;
			readback.objectWrittenWide[a_object] = wideBit;
			a_pass.list.writes.push_back({ geometry, rootNode, visible, Readback::kWriteGeometry, wideBit != 0 });
		}

		/** @brief A plain entry root: visible when any of its counted geometries is, unwritten while one is an engine residue. */
		void JudgeEntry(Pass& a_pass, std::uint32_t a_entry)
		{
			if (a_entry >= a_pass.entries)
				return;
			auto& readback = a_pass.readback;
			const auto& decode = a_pass.decode;
			const bool counted = a_entry < decode.entryListed.size();
			const std::uint32_t listed = counted ? decode.entryListed[a_entry] : 0u, inView = counted ? decode.entryVisible[a_entry] : 0u;
			if (!listed || (counted && decode.entryResidue[a_entry])) {
				readback.entryWritten[a_entry] = Readback::kWrittenNone;
				return;
			}
			const bool visible = a_pass.cutPending || inView;
			const std::uint8_t wanted = visible ? Readback::kWrittenSet : Readback::kWrittenClear;
			const auto* candidates = a_pass.candidates;
			const std::uint32_t version = a_entry < candidates->entryVersion.size() ? candidates->entryVersion[a_entry] : 0u;
			if (!a_pass.full && readback.entryWritten[a_entry] == wanted && readback.entryWrittenVersion[a_entry] == version)
				return;
			readback.entryWritten[a_entry] = wanted;
			readback.entryWrittenVersion[a_entry] = version;
			const void* node = candidates->entryNodes[a_entry];
			a_pass.list.writes.push_back({ const_cast<void*>(node), node, visible, Readback::kWriteEntry, true });
		}

		/**
		 * @brief The parity observer (CS_DCLF_PERSISTENT_PARITY): a pass's writes against the reference's from the same asks, as
		 * sets. A write only one side makes is a difference (<- WRITES DIFFER, with the first ones by slot).
		 */
		void CompareWrites(VisibilityDecode& a_decode, const Pass& a_pass, std::uint32_t a_frame, std::vector<Readback::Writes::Write> a_reference)
		{
			using Write = Readback::Writes::Write;
			const auto key = [](const Write& a_write) { return std::tuple(a_write.kind, a_write.node, a_write.set, a_write.wide, a_write.root); };
			const auto less = [&](const Write& a_left, const Write& a_right) { return key(a_left) < key(a_right); };
			auto made = a_pass.list.writes;
			std::sort(made.begin(), made.end(), less);
			std::sort(a_reference.begin(), a_reference.end(), less);
			std::uint64_t diffs = 0;
			std::string first;
			const auto note = [&](const Write& a_write, bool a_referenceOnly) {
				if (++diffs > 8)
					return;
				static constexpr std::array<const char*, 3> kKinds{ "root", "entry", "geometry" };
				std::uint32_t slot = ~0u;
				if (a_write.kind == Readback::kWriteRoot) {
					if (const std::uint32_t root = a_pass.rootList.Find(a_write.node); root != kNoFadeRoot)
						slot = root;
				} else if (a_write.kind == Readback::kWriteEntry) {
					if (a_pass.candidates)
						if (const auto it = a_pass.candidates->entries.find(static_cast<const RE::NiAVObject*>(a_write.node)); it != a_pass.candidates->entries.end())
							slot = it->second;
				} else if (const auto it = a_decode.geometrySlot.find(a_write.node); it != a_decode.geometrySlot.end()) {
					slot = it->second;
				}
				first += fmt::format("{}{} {} {}{} ({})", first.empty() ? "" : ", ", kKinds[std::min<std::size_t>(a_write.kind, 2)], static_cast<std::int64_t>(slot == ~0u ? -1 : slot),
					a_write.set ? "set" : "clear", a_write.wide ? " wide" : "", a_referenceOnly ? "reference's alone" : "pass's alone");
			};
			std::size_t i = 0, j = 0;
			while (i < made.size() || j < a_reference.size()) {
				if (j == a_reference.size() || (i < made.size() && less(made[i], a_reference[j])))
					note(made[i++], false);
				else if (i == made.size() || less(a_reference[j], made[i]))
					note(a_reference[j++], true);
				else
					++i, ++j;
			}
			++a_decode.parityFrames;
			a_decode.parityDiffs += diffs;
			if (diffs)
				logger::info("[DCLF] accumulated writes parity (A5): frame {}, {} writes, the reference's {}, {} differ <- WRITES DIFFER; first: {}", a_frame, made.size(),
					a_reference.size(), diffs, first);
		}

		/**
		 * @brief The object slots the tables' change log names since the last pass; whether the pass visits everything: on a forced
		 * write, on tables or a log the cursor cannot read on, and when a cut starts (every slot visible until it is answered).
		 */
		bool VisitChangedObjects(VisibilityDecode& a_decode, const SceneTables& a_tables, bool a_full, std::uint32_t a_cut, bool a_cutPending)
		{
			const auto& tables = a_tables;
			const std::uint32_t cut = a_cut;
			bool everything = a_full;
			// The log is invalidated whenever the slots are cleared (SceneTables::Clear); a generation InvalidateVerdicts alone moves
			// leaves the columns, whose rewrites the log carries: the cursor follows the log alone.
			if (!a_decode.changes.Continues(tables.changeLog, 0u)) {
				everything = true;
				a_decode.changes.Restart(0u);
			} else {
				for (const auto& change : a_decode.changes.Unread(tables.changeLog))
					a_decode.objects.Add(change.slot);
			}
			a_decode.changes.Advance(tables.changeLog);
			if (a_cutPending && a_decode.cutVisited != cut) {
				a_decode.cutVisited = cut;
				everything = true;
			}
			return everything;
		}

		/** @brief The plain entries the candidates moved: their members before and after (the cached entry of each is looked up again). */
		void VisitMovedEntries(Readback& a_readback, VisibilityDecode& a_decode, const Readback::Batch& a_newest, std::uint32_t a_entries, bool& a_everything)
		{
			const EntrySpace* candidates = a_newest.candidates.get();
			const std::uint32_t entries = a_entries;
			bool& everything = a_everything;
			if (candidates != a_readback.entryCandidates) {
				a_readback.entryCandidates = candidates;
				if (!candidates || !a_decode.candidates) {
					everything = true;
				} else if (!everything) {
					std::vector<std::uint32_t> moved;
					ChangedEntriesByLog(a_decode.candidates.get(), *candidates, moved);
					const auto forget = [&](std::uint32_t a_object) {
						a_decode.objects.Add(a_object);
						if (a_object < a_readback.objectEntryGeometry.size())
							a_readback.objectEntryGeometry[a_object] = nullptr;
					};
					for (const std::uint32_t e : moved) {
						a_decode.entries.Add(e);
						if (e < a_decode.entryMembers.size())
							for (const std::uint32_t object : a_decode.entryMembers[e])
								forget(object);
						if (e < entries)
							for (const std::uint32_t g : candidates->entryGeometries[e]) {
								if (g < candidates->geometrySlot.size() && candidates->geometrySlot[g] >= 0)
									forget(static_cast<std::uint32_t>(candidates->geometrySlot[g]));
								if (const auto it = g < candidates->geometryNodes.size() ? a_decode.geometrySlot.find(candidates->geometryNodes[g]) : a_decode.geometrySlot.end();
									it != a_decode.geometrySlot.end())
									forget(it->second);
							}
					}
				}
				if (everything) {
					a_readback.objectEntry.assign(a_readback.objectEntry.size(), EntrySpace::kNone);
					a_readback.objectEntryGeometry.assign(a_readback.objectEntryGeometry.size(), nullptr);
				}
				a_decode.candidates = a_newest.candidates;
			}
		}

		/** @brief The root rows the journal marked since the classes were judged (all of them for a holder behind its floor). */
		void VisitMarkedRoots(VisibilityDecode& a_decode, const SceneTables& a_tables, const Api::RootListView& a_rootList, std::size_t a_roots, bool& a_everything)
		{
			bool& everything = a_everything;
			const std::size_t roots = a_roots;
			if (everything || !a_decode.rootsHeld) {
				everything = true;
			} else if (const auto* family = RootFamilyOf(a_tables)) {
				family->Journal().Take().ForEachRun(a_decode.rootEpoch, roots, [&](std::uint64_t a_first, std::uint64_t a_count) {
					for (std::uint64_t r = a_first; r < a_first + a_count; ++r)
						a_decode.roots.Add(static_cast<std::uint32_t>(r));
				});
			}
			a_decode.rootEpoch = a_rootList.contentEpoch;
			a_decode.rootsHeld = true;
		}

		/** @brief The verdicts whose out-of-view frame the window has reached. */
		void VisitExpired(VisibilityDecode& a_decode, std::uint32_t a_window)
		{
			const std::uint32_t window = a_window;
			while (!a_decode.expiries.empty() && a_decode.expiries.front().frame <= window) {
				std::pop_heap(a_decode.expiries.begin(), a_decode.expiries.end(), std::greater<>{});
				const auto expiry = a_decode.expiries.back();
				a_decode.expiries.pop_back();
				(expiry.root ? a_decode.roots : a_decode.objects).Add(expiry.slot);
			}
		}

		/** @brief Every root, object and plain entry slot, those the tables hold and those the decoder still has state for. */
		void VisitEverything(VisibilityDecode& a_decode, std::size_t a_roots, std::size_t a_objects, std::uint32_t a_entries)
		{
			for (std::size_t r = 0, end = std::max(a_roots, a_decode.rootClass.size()); r < end; ++r)
				a_decode.roots.Add(static_cast<std::uint32_t>(r));
			for (std::size_t o = 0, end = std::max(a_objects, a_decode.objectRoot.size()); o < end; ++o)
				a_decode.objects.Add(static_cast<std::uint32_t>(o));
			for (std::size_t e = 0, end = std::max<std::size_t>(a_entries, a_decode.entryListed.size()); e < end; ++e)
				a_decode.entries.Add(static_cast<std::uint32_t>(e));
		}

		/** @brief The decode's line every 300 passes (CS_DCLF_STATS), and its counters reset. */
		void ReportDecode(VisibilityDecode& a_decode)
		{
			if (++a_decode.passes >= 300) {
				if (Diagnostics::Observe().stats)
					logger::info("[DCLF] accumulated writes decode (A5): {} passes, {:.1f} us preparing ({:.2f} us a pass), {} slots visited ({:.1f} a pass), {} whole{}",
						a_decode.passes, double(a_decode.prepareNs) / 1000.0, double(a_decode.prepareNs) / 1000.0 / double(a_decode.passes), a_decode.visits,
						double(a_decode.visits) / double(a_decode.passes), a_decode.wholePasses,
						Diagnostics::Observe().scene ? fmt::format("; parity {} frames, {} write differences{}", a_decode.parityFrames, a_decode.parityDiffs,
														   !a_decode.parityFrames ? " <- NOT COMPARED" : a_decode.parityDiffs ? " <- WRITES DIFFER" : " <- OK") :
													   std::string());
				a_decode.passes = a_decode.prepareNs = a_decode.visits = a_decode.wholePasses = a_decode.parityFrames = a_decode.parityDiffs = 0;
			}
		}

		/**
		 * @brief Decode task: the kAccumulated writes that differ from the last asked (all on a full write). Reads no engine
		 * memory. Null when there is nothing to write.
		 *
		 * The conservative rule (dclf-architecture.md, "kAccumulated written by DCLF"): visible when in view in either of the last two decoded frames, when the
		 * listing has no verdict yet (visible until FadeStateCS's first), or while a camera cut has no decoded batch of its frame or
		 * later.
		 *
		 * Stood-in roots and their geometry, plus wide writes: every other listed fade root and non-layer geometry under one, and
		 * each plain entry root (visible when any of its geometries is). A slot changing class is rewritten. Only the slots that
		 * can have changed are visited (VisibilityDecode).
		 */
		Readback::Writes* PrepareWrites(Readback& a_readback, VisibilityDecode& a_decode, const Readback::Batch& a_newest, bool a_reset)
		{
			ZoneScopedN("CS.DCLF.VisibilityPrepare");
			const auto start = std::chrono::steady_clock::now();
			const auto& tables = *a_newest.tables;
			const std::uint32_t cut = a_readback.cutFrame.load(std::memory_order_acquire);
			const bool cutPending = cut && a_readback.decodedFrame < cut;
			// The first batch at or after a cut's frame rewrites everything (the main thread set every root at the cut and held back
			// older batches' clears).
			const bool cutAnswered = cut && !cutPending && a_readback.cutAnswered != cut;
			const bool full = a_reset || cutAnswered || a_readback.refreshWanted.exchange(false, std::memory_order_acq_rel);
			if (cutAnswered)
				a_readback.cutAnswered = cut;
			const auto rootList = Extensions::RootListOf(tables);
			const std::size_t roots = rootList.nodes.size(), objects = tables.objectGeometry.size();
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
			const EntrySpace* candidates = a_newest.candidates.get();
			const std::uint32_t entries = candidates ? candidates->Capacity() : 0u;
			// The parity observer's reference starts from the asks before this pass.
			std::optional<Asked> asked;
			if (Diagnostics::Observe().scene && Diagnostics::ParityDue(a_decode.serial, 23))
				asked.emplace(a_readback);
			++a_decode.serial;
			bool everything = VisitChangedObjects(a_decode, tables, full, cut, cutPending);
			VisitMovedEntries(a_readback, a_decode, a_newest, entries, everything);
			if (a_readback.objectEntry.size() < objects) {
				a_readback.objectEntry.resize(objects, EntrySpace::kNone);
				a_readback.objectEntryGeometry.resize(objects, nullptr);
			}
			if (a_readback.entryWritten.size() < entries) {
				a_readback.entryWritten.resize(entries, Readback::kWrittenNone);
				a_readback.entryWrittenVersion.resize(entries, 0u);
			}
			VisitMarkedRoots(a_decode, tables, rootList, roots, everything);
			// The verdicts whose out-of-view frame the window has reached.
			const std::uint32_t window = a_readback.previousDecodedFrame;
			VisitExpired(a_decode, window);
			if (everything)
				VisitEverything(a_decode, roots, objects, entries);
			auto* list = new Readback::Writes();
			list->frame = a_newest.frame;
			list->full = full;
			Pass pass{ a_readback, a_decode, tables, rootList, candidates, entries, roots, objects, window, cutPending, full, *list };
			// Roots first (their classes are their objects'), objects next (their shares are their entries' counts).
			for (const std::uint32_t r : a_decode.roots.list)
				JudgeRoot(pass, r);
			for (std::size_t k = 0; k < a_decode.objects.list.size(); ++k)
				JudgeObject(pass, a_decode.objects.list[k]);
			for (const std::uint32_t e : a_decode.entries.list)
				JudgeEntry(pass, e);
			a_decode.visits += a_decode.roots.Size() + a_decode.objects.Size() + a_decode.entries.Size();
			a_decode.wholePasses += everything ? 1 : 0;
			a_decode.roots.Clear();
			a_decode.objects.Clear();
			a_decode.entries.Clear();
			a_decode.prepareNs += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
			if (asked)
				CompareWrites(a_decode, pass, a_newest.frame, ReferenceWrites(a_readback, *asked, a_newest, full, cutPending));
			ReportDecode(a_decode);
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
			// The engine's build (DrawWorld_BuildSceneLists, AE 0x14064bc20; dclf-architecture.md, "kAccumulated written by DCLF"): in an exterior it sets every
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
			const auto& tables = *a_batch.tables;
			const auto rootList = Extensions::RootListOf(tables);
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
				if (r >= rootList.nodes.size() || rootList.nodes[r] != node) {
					++moved;
					continue;
				}
				const std::uint8_t bits = a_sample.rootBits[r];
				const bool engine = (bits & Diagnostics::Accumulated::kSampleAccumulated) != 0;
				const bool listed = r < a_readback.roots.size() && a_readback.roots[r].generation == rootList.rows[r].generation;
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
			// Events past the list's end are appended again next frame; the list grows (RequireSceneTables).
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
				auto& state = DecodeOf(*readback);
				const auto start = std::chrono::steady_clock::now();
				std::size_t next = 0;
				bool reset = false;
				for (auto* batch : batches) {
					reset = reset || batch->reset;
					Apply(*readback, state, *batch);
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
				if (auto* writes = PrepareWrites(*readback, state, *batches.back(), reset)) {
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
		// A reported-words version the decoder has not followed holds zeros (zero-filled at growth): it restarts with this frame's
		// batch.
		bool reset = false;
		auto& followed = a_scene.visibilityReportedFollowed;
		const auto &roots = Adopted().Of(a_scene.visibilityRootReported), &objects = Adopted().Of(a_scene.visibilityObjectReported);
		if (!followed[0].Holds(roots) || !followed[1].Holds(objects)) {
			followed[0].Filled(roots);
			followed[1].Filled(objects);
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
			r.writtenRoots = {};
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
			r.writtenRoots = r.writtenTables ? Extensions::RootListOf(*r.writtenTables) : Api::RootListView{};
			r.writtenCandidates = std::move(list->candidates);
			auto* following = list->next;
			delete list;
			list = following;
		}
		// A new camera cut: every listed root, its geometry, and the plain entry roots set visible now.
		if (cut && r.cutWritten != cut && r.writtenTables) {
			r.cutWritten = cut;
			const auto& tables = *r.writtenTables;
			const auto& rootList = r.writtenRoots;
			std::vector<std::uint8_t> written(rootList.nodes.size(), 0u);
			for (std::size_t root = 0; root < rootList.nodes.size(); ++root) {
				const void* node = rootList.nodes[root];
				if (!node || root >= rootList.rows.size())
					continue;
				const bool stoodIn = (rootList.rows[root].bits & kFadeRootStoodIn) && a_stoodIn(node);
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
				const std::uint32_t root = rootList.RootOf(static_cast<std::uint32_t>(o));
				if (root < written.size() && written[root]) {
					Store(r, geometry, true, !(rootList.rows[root].bits & kFadeRootStoodIn));
					SetChain(r, geometry, rootList.nodes[root]);
					continue;
				}
				// A plain entry root's geometry (no fade root).
				if (root != kNoFadeRoot || !candidates)
					continue;
				const auto it = candidates->geometries.find(geometry);
				const std::uint32_t entry = it != candidates->geometries.end() && it->second < candidates->geometryEntry.size() ? candidates->geometryEntry[it->second] : EntrySpace::kNone;
				const RE::NiAVObject* entryNode = entry < candidates->entryNodes.size() ? candidates->entryNodes[entry] : nullptr;
				if (!entryNode || rootList.Contains(entryNode))
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
				it = (r.writtenTables && r.writtenRoots.Contains(it->first)) ||
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
		if (!(r.writtenTables && r.writtenRoots.Contains(a_root)) &&
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
