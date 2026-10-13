#pragma once

// Event lists read back without a stall: the fade write-back's and the visibility events' host sides.
// Part of IndirectDraws' implementation: included by Internal.h, in its order.

namespace DCLF::Draws
{
	/**
	 * @brief The scene's tables on the GPU, one set for every epoch - the shadow views', Skylighting's, the Z-prepass's and
	 * the colour segment's: the object records (t127), the bone rows (t126), the geometry table (BuildDraws' draws) and the
	 * NPC face shapes' positions (the face draws' second stream). Kept by one store each (Impl::objectStore, extrasStore,
	 * geometryStore), whose builds run in frame order - the shadow build, the Z-prepass's, the colour build's, each joined
	 * before the next is kicked - so each commit sends what changed since the version the buffers hold, which the commit
	 * before it wrote. Created with the first epoch's resources and kept across their recreation.
	 */
	/**
	 * @brief An event list the depth epoch's GPU work appends to, read back without a stall (the fade write-back's, the visibility
	 * events'). The depth epoch is revision-driven, so its readback pass (EventReadbackPass) records a fixed copy of the whole list
	 * into the host buffer of its frame slot, in the staging set of the revision's list capacity; nothing is chosen in its Record. The
	 * depth commit arms its frame slot with the frame and harvests the slot's last execution first, when the slot comes round again:
	 * the host has waited for that execution and not submitted the next, so DCLF waits for nothing. A list's header holds its count,
	 * then the frame it was armed for.
	 */
	struct HostEventRing
	{
		/**
		 * @brief The host buffers of one list capacity, one per frame slot (the revisions sized for it copy into them). Append-only (a
		 * growth adds one: Impl::ReserveSceneTables, before any revision names the capacity), kept until the ring goes: a kept
		 * recording may name any of them.
		 */
		struct StagingSet
		{
			std::uint32_t events = 0;
			std::vector<std::shared_ptr<org::Buffer>> buffers;  // by frame slot
			StagingSet* next = nullptr;
		};
		explicit HostEventRing(std::uint64_t (*a_bytesFor)(std::uint32_t)) :
			bytesFor(a_bytesFor) {}
		~HostEventRing()
		{
			for (StagingSet* set = sets.exchange(nullptr); set;) {
				StagingSet* following = set->next;
				delete set;
				set = following;
			}
		}
		std::uint64_t (*bytesFor)(std::uint32_t);  // a list of so many events, its header included
		std::uint32_t frameSlots = 0;
		std::atomic<StagingSet*> sets{ nullptr };
		// The largest count a harvest read past its list: the list grows to it (what it left out appends again).
		std::atomic<std::uint32_t> wanted{ 0 };
		// Since the last report (any thread): harvests past their list, arms whose slot's buffer did not hold their frame (the epoch did
		// not run, or ran without the readback), staging sets added.
		std::atomic<std::uint64_t> overflowed{ 0 }, missed{ 0 }, setsAdded{ 0 };

		/** @brief The staging set of a_events (the revision's list capacity), null if none was added for it. */
		const StagingSet* SetFor(std::uint32_t a_events) const
		{
			for (const StagingSet* set = sets.load(std::memory_order_acquire); set; set = set->next)
				if (set->events == a_events)
					return set;
			return nullptr;
		}
		/** @brief A staging set for a list of a_events (one thread at a time: the setup, then the growths' revision code). */
		void AddSet(std::uint32_t a_events)
		{
			if (SetFor(a_events))
				return;
			auto* set = new StagingSet();
			set->events = a_events;
			for (std::uint32_t i = 0; i < frameSlots; ++i)
				set->buffers.push_back(org::Buffer::CreateShared(rhi::HeapType::Readback, bytesFor(a_events)));
			set->next = sets.load(std::memory_order_relaxed);
			while (!sets.compare_exchange_weak(set->next, set, std::memory_order_release, std::memory_order_relaxed)) {}
			setsAdded.fetch_add(1, std::memory_order_relaxed);
		}
		/**
		 * @brief The frame slot's last execution armed for a_frame: a_take(header words, count, capacity) on the buffer that holds that
		 * frame, in the staging set its revision copied into (a growth's revisions copy into the next set; a buffer of another set holds
		 * an older frame), its list past its capacity counted and asked for. False, counted missed, when none holds it.
		 */
		template <class Take>
		bool Harvest(std::uint32_t a_frameSlot, std::uint32_t a_frame, Take&& a_take)
		{
			for (const auto* set = sets.load(std::memory_order_acquire); set; set = set->next) {
				auto resource = set->buffers[a_frameSlot]->GetAPIResource();
				void* mapped = nullptr;
				resource.Map(&mapped);
				if (!mapped)
					continue;
				const auto* words = static_cast<const std::uint32_t*>(mapped);
				if (words[1] != a_frame) {
					resource.Unmap(0, 0);
					continue;
				}
				const std::uint32_t count = words[0];
				if (count > set->events) {
					std::uint32_t asked = wanted.load(std::memory_order_relaxed);
					while (asked < count && !wanted.compare_exchange_weak(asked, count, std::memory_order_release, std::memory_order_relaxed)) {}
					overflowed.fetch_add(1, std::memory_order_relaxed);
				}
				a_take(words, count, set->events);
				resource.Unmap(0, 0);
				return true;
			}
			missed.fetch_add(1, std::memory_order_relaxed);
			return false;
		}
	};

	/**
	 * @brief The fade write-back's host side (drawcall-limit-fix.md, "The fade write-back"): the event list read back without a stall
	 * (HostEventRing), and its events handed to the write-back task in batches through a lock-free stack. A frame whose epoch did not
	 * run is counted missed: FadeStateCS did not run either, so nothing was reported and nothing is lost.
	 */
	struct FadeWriteBack : HostEventRing
	{
		FadeWriteBack() :
			HostEventRing(&BytesFor) {}
		struct Batch
		{
			std::vector<FadeEvent> events;
			std::uint32_t frame = 0;  // the scene frame whose FadeStateCS appended them (the list's header)
			Batch* next = nullptr;
		};
		// The render thread's: each frame slot's last arm (whether, and the scene frame it was for).
		struct Armed
		{
			bool armed = false;
			std::uint32_t frame = 0;
		};
		std::vector<Armed> armed;
		std::atomic<Batch*> batches{ nullptr };
		// The write-back task's (IndirectDraws::KickFadeWriteBack, step 6e S4: one at a time on DCLF's executor, never joined): per
		// root slot the scene frame of the event last taken - an older one (a list read late) is not written over it. Its alone.
		std::vector<std::uint32_t> appliedFrame;
		std::atomic<bool> running{ false };
		/**
		 * @brief What a task found to write (step 6e F1): the stores, in event order, and the tables snapshot they were judged
		 * against - held until the render thread makes them, so no node they name is let go first (its publication's
		 * retirement node). The task touches no engine memory; the render thread makes the stores at the next frame's start.
		 */
		struct Stores
		{
			struct Store
			{
				void* node = nullptr;
				std::uint32_t flags = 0;  // the fade bits (kFadeFlagMask) only
				float fade = 0.0f;
				// T7b4: a leaf or tree root's LOD state (+0x152, +0x153, +0x14C), written with the fade (lod false: a plain fade root's
				// levels are the node's own, not written).
				bool lod = false;
				std::uint32_t levels = 0;
				float blend = 0.0f;
			};
			std::shared_ptr<const SceneTables> tables;
			std::vector<Store> stores;
		};
		std::atomic<Stores*> ready{ nullptr };
		// Since the last report: milestones taken and written, stale, frames whose start found the last task still running, and
		// the main thread's time making the stores; T7b4: stores left because DCLF no longer writes the root (the engine's cull
		// steps it again: one writer), and stores dropped because DCLF did not run.
		std::atomic<std::uint64_t> applied{ 0 }, stale{ 0 }, busy{ 0 }, storeNs{ 0 }, notWritten{ 0 }, dropped{ 0 };
		// Since the last report: frames harvested (and those with events), events.
		std::atomic<std::uint64_t> harvested{ 0 }, harvestedWithEvents{ 0 }, events{ 0 };

		void Push(Batch* a_batch)
		{
			a_batch->next = batches.load(std::memory_order_relaxed);
			while (!batches.compare_exchange_weak(a_batch->next, a_batch, std::memory_order_release, std::memory_order_relaxed)) {}
		}
		~FadeWriteBack()
		{
			delete ready.exchange(nullptr);
			for (Batch* b = batches.exchange(nullptr); b;) {
				Batch* following = b->next;
				delete b;
				b = following;
			}
		}
		static std::uint64_t BytesFor(std::uint32_t a_events) { return (kFadeEventHeaderWords + std::uint64_t(a_events) * kFadeEventWords) * sizeof(std::uint32_t); }
	};

	/**
	 * @brief T7b1's and T7b2's host side (drawcall-limit-fix.md, "T7b1: visibility events", "T7b2: kAccumulated written by DCLF"):
	 * the event list (VisibilityEventsCS) read back without a stall, its decoder's state, and the kAccumulated writes it prepares for
	 * the roots the cut stands in for. Part of the normal path; CS_DCLF_ACCUMULATED_PARITY adds the observers alone.
	 *
	 * The depth epoch is revision-driven (persistent-epochs.md, "Revision-driven epochs"): its passes are recorded once per ring slot
	 * for a revision and the recordings are submitted again frame after frame, so nothing in a recording's Record runs per frame. The
	 * readback is therefore recorded as a fixed copy: VisibilityReadbackPass copies the list into the host buffer of its frame slot
	 * (RecordingContext::FrameSlot, the one per-frame input a recording has) in the staging set of the revision's list capacity. The
	 * render thread arms each frame (ArmVisibilityReadback: the frame, its tables, by frame slot) and harvests the slot's last
	 * execution when the slot comes round again: the depth commit runs after the host waited for the slot's previous execution, and
	 * before the new one is submitted, so that buffer is complete and not yet written again (no wait of DCLF's own). The buffer whose
	 * header names the armed frame is copied into a batch (the header's count of events, a few hundred at most in motion), and the
	 * batches go to the decode task in frame order. Batches are heap allocations, as many as wait: none is dropped.
	 *
	 * The decoder (one task at a time, never joined) applies the batches in frame order to the verdicts it keeps by DCLF's slots, the
	 * roots' by their listing's generation, against the tables of each batch's frame (held by its batch). It writes no engine memory
	 * and reads none: from the newest batch's tables it prepares the kAccumulated writes of the stood-in roots (kFadeRootStoodIn) and
	 * their geometry by the conservative rule (T7b2: in view in either of the last two decoded frames, no verdict yet, or a camera cut
	 * not yet followed by a batch), only where they differ from what it last asked for (Writes, holding those tables, so no node is
	 * let go before the write). The main thread makes them at Main::Update's entry (IndirectDraws::WriteAccumulated), the one point
	 * every reader sees in the same frame and no cull or cell animation runs. With the engine's samples of the same frame
	 * (SideEffects::SampleAccumulated) it is the <- ACCUMULATED observer. Decoded batches go back to the render thread, which names
	 * their finds and lets go of their tables.
	 *
	 * T7f checklist item 1 ("T7f preparation, part 2"): the decoder also prepares every other listed root's writes (fade roots, their
	 * geometry, the plain entry roots of the frame's SunCandidates), which the main thread makes with the nodes between and the cell roots.
	 */
	struct VisibilityReadback : HostEventRing
	{
		VisibilityReadback() :
			HostEventRing(&VisibilityEventBytes) {}
		/** @brief A root slot's verdict, for the listing of its generation: any other generation's is none. */
		struct RootVisibility
		{
			std::uint32_t generation = 0;
			bool known = false;    // FadeStateCS updated the listing this frame (kVisibilityBitVerdict): it has a verdict
			bool visible = false;
			std::uint32_t lastFrame = 0;  // the last scene frame it was in view
			std::uint32_t outFrame = 0;   // the scene frame it went out of view (T7b2: still in view in every decoded frame before it)
		};
		/** @brief An object slot's verdict: its occupant's in the tables of the frame it was decoded with. */
		struct ObjectVisibility
		{
			bool visible = false;
			std::uint32_t lastFrame = 0;
			std::uint32_t outFrame = 0;
		};
		/** @brief A write's node: a fade root slot's, a plain entry root (SunCandidates' entry with no fade root), a geometry. */
		enum WriteKind : std::uint8_t
		{
			kWriteRoot,
			kWriteEntry,
			kWriteGeometry,
		};
		/**
		 * @brief T7b2: kAccumulated writes the decoder prepared from one decode's newest batch, in its tables' slots, for the main
		 * thread (Main::Update's entry). The tables hold every node named (their publication's retirement), so none is let go
		 * before the write; only the main thread lets go of them.
		 */
		struct Writes
		{
			struct Write
			{
				void* node = nullptr;        // a fade root, a plain entry root, or a geometry under one
				const void* root = nullptr;  // its root (a stood-in one's is written only while the cut still stands in for it); the
				                             // end of the walk that sets the nodes between
				bool set = false;
				std::uint8_t kind = kWriteGeometry;
				// T7f checklist item 1: a root the cut does not stand in for (or a geometry under one, or a plain entry root), written
				// as well.
				bool wide = false;
			};
			std::uint32_t frame = 0;  // the scene frame of the batch the writes follow
			bool full = false;        // every stood-in root and geometry, not only the changes (a refresh, a reset, a cut's first batch after)
			std::shared_ptr<const SceneTables> tables;
			std::shared_ptr<const SunCandidates> candidates;  // the plain entry roots' (held: their nodes live until the write)
			std::vector<Write> writes;
			Writes* next = nullptr;
		};
		/** @brief What the decoder last asked the main thread to write for a root slot (by listing) or an object slot (by occupant). */
		enum Written : std::uint8_t
		{
			kWrittenNone,
			kWrittenClear,
			kWrittenSet,
		};
		// The observer's finds the render thread names (Batch::firsts' categories).
		enum Category : std::uint8_t
		{
			kRootDclfOnly,
			kRootEngineOnly,
			kGeometryDclfOnly,
			kGeometryEngineOnly,
			kCategories,
		};
		/** @brief A frame's events, copied out of its frame slot's host buffer, and what the decoder found for the render thread. */
		struct Batch
		{
			std::uint32_t frame = 0;
			bool reset = false;     // the frame's commit zeroed the reported words: the decoder's verdicts start again
			bool complete = true;   // every change fit (none of the frame's verdicts arrives a frame late)
			std::shared_ptr<const SceneTables> tables;  // the frame's (the occupants); let go by the render thread alone
			std::shared_ptr<const SunCandidates> candidates;   // the frame's entries (the plain entry roots of T7f's wide writes)
			std::vector<VisibilityEvent> events;
			std::vector<std::pair<std::uint8_t, const void*>> firsts;  // (category, node), named while `tables` holds the node
			Batch* next = nullptr;
		};
		/** @brief What a frame slot's last arm was for (render thread). */
		struct Armed
		{
			bool armed = false;
			std::uint32_t frame = 0;
			bool reset = false;
			std::shared_ptr<const SceneTables> tables;
			std::shared_ptr<const SunCandidates> candidates;
		};

		// The render thread's: each frame slot's arm, and the batches harvested and not yet handed to a task, in frame order.
		std::vector<Armed> armed;
		std::vector<Batch*> pending;
		bool carryReset = false;  // a reset arm whose frame was missed: the next batch harvested starts the decoder again
		// Decoded batches, back to the render thread (a lock-free stack: the task pushes, the wake takes them all).
		std::atomic<Batch*> decoded{ nullptr };
		std::atomic<bool> running{ false };
		// The decoder's alone (one task at a time; `running` orders them).
		std::vector<RootVisibility> roots;
		std::vector<ObjectVisibility> objects;
		std::vector<std::uint32_t> rootApplied, objectApplied;  // per slot: the scene frame of the event last taken
		// T7b2, the decoder's too: the last two scene frames decoded (the conservative rule's window), the cut its full write
		// answered, and per slot what it last asked to write (a root's for its listing's generation, an object's for its occupant).
		std::uint32_t decodedFrame = 0, previousDecodedFrame = 0, cutAnswered = 0;
		std::vector<std::uint32_t> rootWrittenGeneration;
		std::vector<std::uint8_t> rootWritten, objectWritten;
		std::vector<const void*> objectWrittenGeometry;
		// T7f preparation, the decoder's: the class each slot was last asked in (1: wide), the plain entry roots' last asks (by entry
		// index and version), and each object slot's plain entry (cached by occupant, for one candidates snapshot).
		std::vector<std::uint8_t> rootWrittenWide, objectWrittenWide, entryWritten;
		std::vector<std::uint32_t> entryWrittenVersion;
		const SunCandidates* entryCandidates = nullptr;
		std::vector<std::uint32_t> objectEntry;
		std::vector<const void*> objectEntryGeometry;
		// T7b2, any thread: a full write asked for (PrimaryCull: a frame the engine culled the stood-in roots itself, the main thread:
		// writes dropped), and the scene frame of the last camera cut (the render thread's, PrimaryCull::AfterFullFrustum: from there
		// every stood-in root is visible until a batch of that frame or later is decoded).
		std::atomic<bool> refreshWanted{ true };
		std::atomic<std::uint32_t> cutFrame{ 0 };
		// The prepared writes, to the main thread (a lock-free stack: the task pushes, Main::Update's entry takes them all).
		std::atomic<Writes*> writes{ nullptr };
		// The main thread's: the newest writes' tables (a cut's writes take the stood-in roots from them), and the cut it wrote.
		std::shared_ptr<const SceneTables> writtenTables;
		std::shared_ptr<const SunCandidates> writtenCandidates;  // the newest writes' (T7f preparation: the plain entry roots)
		std::uint32_t cutWritten = 0;
		std::uint32_t verdictPrunes = 0;           // entries since rootVerdicts was last pruned to the newest tables (wide verdicts)
		std::vector<RE::NiAVObject*> chain;        // scratch: a geometry's nodes up to its root
		std::string wideNoChainFirst;              // T7f: the first geometries whose parent walk did not reach their root, and the root
		// T7b5 fix, the main thread's: each stood-in root's verdict as last written (set: visible), forgotten once it is no longer stood
		// in (IndirectDraws::AccumulatedVerdict: the tree manager's multibound results of the stood-in trees). T7f preparation: with the
		// wide writes, every listed root's too, forgotten once the newest tables no longer list it.
		ankerl::unordered_dense::map<const void*, bool> rootVerdicts;
		// T7b2, since the last report: writes made (set, cleared), found as wanted, left (the cut no longer stands in for the root;
		// a clear older than a cut), the cut's own writes, full writes prepared, cuts, and the main thread's time.
		std::atomic<std::uint64_t> writesSet{ 0 }, writesCleared{ 0 }, writesUnchanged{ 0 }, writesHeldByCut{ 0 };
		std::atomic<std::uint64_t> cutWrites{ 0 }, fullWrites{ 0 }, cuts{ 0 }, writeNs{ 0 }, writeBatches{ 0 };
		// T7f preparation (the main thread's, since the last report): the wide writes made (set, cleared, already so), the nodes
		// between set (and geometries whose walk did not reach their root), and the cell roots (ListUpkeep's replacement: seen, set).
		std::atomic<std::uint64_t> wideSet{ 0 }, wideCleared{ 0 }, wideUnchanged{ 0 }, wideNodesSet{ 0 }, wideNoChain{ 0 };
		std::atomic<std::uint64_t> cellRootsSeen{ 0 }, cellRootsSet{ 0 };
		// Since the last report (any thread): the batches decoded and their events, events stale (another listing's, a slot left
		// empty, older than one taken), batches past their list, arms whose slot's buffer did not hold their frame (the epoch did not
		// run, or ran without the readback), staging sets added, wakes that found the last task running, and the decode's time ...
		std::atomic<std::uint64_t> batches{ 0 }, events{ 0 }, stale{ 0 }, busy{ 0 }, decodeNs{ 0 };
		// ... and the observer's: frames compared, frames left out (a batch past its list: some verdicts a frame late), samples whose
		// frame no batch had, and per class (both in view, DCLF's alone, the engine's alone, neither) the roots and geometries the
		// engine culled, and those it did not (stood in: the cut's admitted, unmixed entries). Aside from the gate: roots with no DCLF
		// verdict this frame (not updated by FadeStateCS: no seed, no member drawn), and DCLF's alone on an app-culled (hidden) root,
		// which the engine's cull does not reach. staleGeometry: the engine's alone whose root's bit is clear (the bit the last cull
		// that reached it left).
		struct Counts
		{
			std::atomic<std::uint64_t> both{ 0 }, dclfOnly{ 0 }, engineOnly{ 0 }, neither{ 0 };
		};
		Counts rootCounts, geometryCounts, stoodInRootCounts, stoodInGeometryCounts;
		std::atomic<std::uint64_t> framesCompared{ 0 }, framesIncomplete{ 0 }, samplesUnmatched{ 0 }, moved{ 0 }, staleGeometry{ 0 };
		std::atomic<std::uint64_t> noVerdictEngine{ 0 }, noVerdictNeither{ 0 }, hiddenDclfOnly{ 0 };
		// The render thread's: the first finds' names since the last report, per category.
		std::array<std::string, kCategories> firstNames;

		~VisibilityReadback()
		{
			for (Batch* batch = decoded.exchange(nullptr); batch;) {
				Batch* following = batch->next;
				delete batch;
				batch = following;
			}
			for (Batch* batch : pending)
				delete batch;
			for (Writes* list = writes.exchange(nullptr); list;) {
				Writes* following = list->next;
				delete list;
				list = following;
			}
		}
	};
}
