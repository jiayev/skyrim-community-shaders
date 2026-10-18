#pragma once

// Host sides of the event lists the depth epoch's GPU work appends to (the visibility events; Features/Fades' write-back is another),
// read back without a stall. The render thread arms and harvests, a task on DCLF's executor decodes, the main thread makes the engine
// writes at Main::Update's entry. Self-contained (features include it); Internal.h includes it in its order.

#include "Features/DrawcallLimitFix/Core/Extensions/RootStates.h"
#include "Features/DrawcallLimitFix/Core/Render/Payload/Versioned.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/GpuLayouts.h"

#include <OpenRenderGraph/PersistentGraphHost.h>
#include <Render/LatchBlock.h>
#include <RenderPasses/Base/TypedRenderGraphPass.h>
#include <Resources/Buffers/Buffer.h>
#include <rhi.h>

#include <ankerl/unordered_dense.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace DCLF
{
	struct EntrySpace;
	struct SceneTables;
}

namespace DCLF::Draws
{
	struct SceneBuffers;

	/**
	 * @brief An event list the depth epoch appends to, read back without a stall. Its revision-driven readback pass records a fixed
	 * copy of the list into its frame slot's host buffer. The depth commit first harvests the slot's last execution (the host has
	 * waited for it, not yet submitted the next: DCLF waits for nothing), then arms it. Header: count, then the armed frame.
	 */
	struct HostEventRing
	{
		/**
		 * @brief The host buffers of one list capacity, one per frame slot. Append-only (one per capacity a sealed set names, added
		 * from its named units before any shape of that set: Impl::MakeRevision), kept until the ring goes: a kept recording may name
		 * any of them.
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
		std::uint64_t (*bytesFor)(std::uint32_t);  // a list of so many events, header included
		std::uint32_t frameSlots = 0;
		std::atomic<StagingSet*> sets{ nullptr };
		// The largest count a harvest read past its list: the list grows to it (what was left out appends again).
		std::atomic<std::uint32_t> wanted{ 0 };
		// Since the last report (any thread): harvests past their list, arms whose buffer did not hold their frame, sets added.
		std::atomic<std::uint64_t> overflowed{ 0 }, missed{ 0 }, setsAdded{ 0 };

		/** @brief The staging set of a_events (the revision's list capacity), null if none was added for it. */
		const StagingSet* SetFor(std::uint32_t a_events) const
		{
			for (const StagingSet* set = sets.load(std::memory_order_acquire); set; set = set->next)
				if (set->events == a_events)
					return set;
			return nullptr;
		}
		/** @brief Adds a staging set for a list of a_events (one thread at a time: the setup, then the revision producer's seals). */
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
		 * @brief a_take(header words, count, capacity) on whichever set's buffer holds the slot's execution armed for a_frame (a
		 * growth's revisions copy into the new set); an overflow is asked for. False, counted missed, when none holds it.
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

	/** @brief An event ring's copy to the host (Passes/Readbacks.cpp): a_events into a_ring's frame slot's staging buffer. */
	std::shared_ptr<org::RenderPass> MakeEventReadbackPass(std::shared_ptr<SceneBuffers> a_scene, Versioned a_events, std::shared_ptr<HostEventRing> a_ring);

	/**
	 * @brief The visibility readback (dclf-architecture.md, "The visibility events", "kAccumulated written by DCLF"): the
	 * VisibilityEventsCS list, its decoder's state, and the kAccumulated writes it prepares. Normal path; CS_DCLF_ACCUMULATED_PARITY
	 * adds only the observers. Harvested batches (heap-allocated, none dropped) go to the decode task in frame order.
	 *
	 * The decoder (one task at a time, never joined) applies batches to its verdicts by DCLF slot (roots by listing generation),
	 * against each batch's own tables, touching no engine memory. From the newest tables it prepares kAccumulated writes for the
	 * stood-in roots (kFadeRootStoodIn) and their geometry by the conservative rule (in view in either of the last two decoded frames,
	 * no verdict yet, or a camera cut not yet followed by a batch), plus the wide writes for every other listed root, only where they
	 * differ from its last ask. Writes hold those tables so no node is let go first. The main thread makes them at Main::Update's
	 * entry, the one point every reader sees in the same frame and no cull or cell animation runs. Decoded batches return to the
	 * render thread, which lets go of their tables.
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
			std::uint32_t outFrame = 0;   // the scene frame it went out of view (in view in every decoded frame before it)
		};
		/** @brief An object slot's verdict: its occupant's in the tables of the frame it was decoded with. */
		struct ObjectVisibility
		{
			bool visible = false;
			std::uint32_t lastFrame = 0;
			std::uint32_t outFrame = 0;
		};
		/** @brief A write's node: a fade root slot's, a plain entry root (EntrySpace' entry with no fade root), a geometry. */
		enum WriteKind : std::uint8_t
		{
			kWriteRoot,
			kWriteEntry,
			kWriteGeometry,
		};
		/**
		 * @brief The kAccumulated writes prepared from one decode's newest batch, for the main thread. The tables hold every node
		 * named until the write; only the main thread lets go of them.
		 */
		struct Writes
		{
			struct Write
			{
				void* node = nullptr;        // a fade root, a plain entry root, or a geometry under one
				const void* root = nullptr;  // its root (a stood-in one's written only while the cut still stands in for it); the
				                             // end of the walk that sets the nodes between
				bool set = false;
				std::uint8_t kind = kWriteGeometry;
				// A wide write: a root the cut does not stand in for (or a geometry under one, or a plain entry root).
				bool wide = false;
			};
			std::uint32_t frame = 0;  // the scene frame of the batch the writes follow
			bool full = false;        // every stood-in root and geometry, not only the changes (a refresh, a reset, a cut's first batch)
			std::shared_ptr<const SceneTables> tables;
			std::shared_ptr<const EntrySpace> candidates;  // the plain entry roots' (held: their nodes live until the write)
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
			bool complete = true;   // every change fit (no verdict arrives a frame late)
			std::shared_ptr<const SceneTables> tables;  // the frame's (the occupants); let go by the render thread alone
			std::shared_ptr<const EntrySpace> candidates;   // the frame's entries (the wide writes' plain entry roots)
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
			std::shared_ptr<const EntrySpace> candidates;
		};

		// Render thread: each frame slot's arm, and harvested batches not yet handed to a task, in frame order.
		std::vector<Armed> armed;
		std::vector<Batch*> pending;
		bool carryReset = false;  // a reset arm whose frame was missed: the next harvested batch restarts the decoder
		// Decoded batches back to the render thread (lock-free stack: the task pushes, the wake takes all).
		std::atomic<Batch*> decoded{ nullptr };
		std::atomic<bool> running{ false };
		// The decoder's alone (one task at a time; `running` orders them).
		std::vector<RootVisibility> roots;
		std::vector<ObjectVisibility> objects;
		std::vector<std::uint32_t> rootApplied, objectApplied;  // per slot: the scene frame of the event last taken
		// The decoder's: the last two decoded frames (the conservative rule's window), the cut it answered, per slot its last ask.
		std::uint32_t decodedFrame = 0, previousDecodedFrame = 0, cutAnswered = 0;
		std::vector<std::uint32_t> rootWrittenGeneration;
		std::vector<std::uint8_t> rootWritten, objectWritten;
		std::vector<const void*> objectWrittenGeometry;
		// The decoder's: each slot's last ask class (1: wide), the plain entry roots' last asks, each object slot's cached plain entry.
		std::vector<std::uint8_t> rootWrittenWide, objectWrittenWide, entryWritten;
		std::vector<std::uint32_t> entryWrittenVersion;
		const EntrySpace* entryCandidates = nullptr;
		std::vector<std::uint32_t> objectEntry;
		std::vector<const void*> objectEntryGeometry;
		// The decoder's alone: PrepareWrites' incremental state (Visibility.cpp's VisibilityDecode).
		std::shared_ptr<void> decodeState;
		// Any thread: a full write asked for (the engine culled the stood-in roots itself, or writes were dropped), and the scene
		// frame of the last camera cut (every stood-in root visible until a batch of that frame or later is decoded).
		std::atomic<bool> refreshWanted{ true };
		std::atomic<std::uint32_t> cutFrame{ 0 };
		// Prepared writes to the main thread (lock-free stack: the task pushes, Main::Update's entry takes all).
		std::atomic<Writes*> writes{ nullptr };
		// The main thread's: the newest writes' tables (a cut's writes take the stood-in roots from them), and the cut it wrote.
		std::shared_ptr<const SceneTables> writtenTables;
		Api::RootListView writtenRoots;  // writtenTables' listed roots (the root states' view: valid while they are held)
		std::shared_ptr<const EntrySpace> writtenCandidates;  // the newest writes' (the plain entry roots)
		std::uint32_t cutWritten = 0;
		std::uint32_t verdictPrunes = 0;           // entries since rootVerdicts was last pruned to the newest tables
		std::vector<RE::NiAVObject*> chain;        // scratch: a geometry's nodes up to its root
		std::string wideNoChainFirst;              // the first geometries whose parent walk did not reach their root, and the root
		// The main thread's: each listed root's verdict as last written (set: visible; AccumulatedVerdict, the tree manager's
		// multibound results), forgotten once no longer stood in or listed by the newest tables.
		ankerl::unordered_dense::map<const void*, bool> rootVerdicts;
		// Since the last report: writes made, unchanged, and left (no longer stood in; a clear older than a cut); cuts; time.
		std::atomic<std::uint64_t> writesSet{ 0 }, writesCleared{ 0 }, writesUnchanged{ 0 }, writesHeldByCut{ 0 };
		std::atomic<std::uint64_t> cutWrites{ 0 }, fullWrites{ 0 }, cuts{ 0 }, writeNs{ 0 }, writeBatches{ 0 };
		// The main thread's, since the last report: wide writes, nodes between set (and walks short of their root), and the cell
		// roots (PrimaryCull::ListUpkeep's kAccumulated).
		std::atomic<std::uint64_t> wideSet{ 0 }, wideCleared{ 0 }, wideUnchanged{ 0 }, wideNodesSet{ 0 }, wideNoChain{ 0 };
		std::atomic<std::uint64_t> cellRootsSeen{ 0 }, cellRootsSet{ 0 };
		// Since the last report (any thread): batches decoded, events, stale events (another listing's, an empty slot, older than one
		// taken), wakes that found the task running, decode time ...
		std::atomic<std::uint64_t> batches{ 0 }, events{ 0 }, stale{ 0 }, busy{ 0 }, decodeNs{ 0 };
		// ... and the observer's (<- ACCUMULATED): frames compared and incomplete, unmatched samples, per class the roots and
		// geometries (engine-culled, stood in); roots with no DCLF verdict, DCLF-only on an app-culled root, and engine-only geometry
		// whose root's bit is clear.
		struct Counts
		{
			std::atomic<std::uint64_t> both{ 0 }, dclfOnly{ 0 }, engineOnly{ 0 }, neither{ 0 };
		};
		Counts rootCounts, geometryCounts, stoodInRootCounts, stoodInGeometryCounts;
		std::atomic<std::uint64_t> framesCompared{ 0 }, framesIncomplete{ 0 }, samplesUnmatched{ 0 }, moved{ 0 }, staleGeometry{ 0 };
		std::atomic<std::uint64_t> noVerdictEngine{ 0 }, noVerdictNeither{ 0 }, hiddenDclfOnly{ 0 };
		// Render thread: the first finds' names since the last report, per category.
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

	/**
	 * @brief The culling's counters read back without a stall: each execution of its epoch, a fixed copy (CounterReadbackPass) of
	 * each drawn source's count words and of the stamp its culling latch carried (BuildDrawsLatch::visibilityStamp, the frame) into
	 * the frame slot's host buffer. The render thread arms one source after an execution and reads it once that slot's copy is the
	 * armed execution's and done (the host has waited, or the execution's GPU point is reached and no later one took the slot);
	 * the stamp names the frame the words are of. Made by the pass at its graph build, held by the pass; the epochs find it by
	 * CullCounterReadback.
	 */
	struct CounterReadback
	{
		static constexpr std::uint32_t kSourceWords = kCountWords + 1;  // the count words, then the latch's stamp
		CounterReadback(std::uint32_t a_frameSlots, std::uint32_t a_sources) :
			sources(a_sources)
		{
			for (std::uint32_t i = 0; i < a_frameSlots; ++i)
				buffers.push_back(org::Buffer::CreateShared(rhi::HeapType::Readback, std::uint64_t(std::max(a_sources, 1u)) * kSourceWords * sizeof(std::uint32_t)));
		}
		static constexpr std::uint32_t StampOf(std::uint32_t a_frame) { return a_frame & 0x0FFFFFFFu; }  // as the commits write it
		std::uint32_t sources = 0;
		std::vector<std::shared_ptr<org::Buffer>> buffers;  // by frame slot: sources x kSourceWords words
		/** @brief Render thread: the armed sample (the slot and source its words land in, the stamp they must carry). */
		struct Armed
		{
			bool armed = false;
			std::uint32_t frameSlot = 0, source = 0, stamp = 0;
			std::uint32_t view = 0, mode = 0;              // a shadow view's labels, for the report
			org::PersistentGraphHost::GpuPoint done;       // when the slot is not read inside its next commit: the execution's end
		} armed;
		std::uint64_t harvested = 0, missed = 0;  // render thread, since the readback was made

		/**
		 * @brief a_take(the source's kCountWords words) when the slot's host buffer holds the source as of a_stamp; false otherwise.
		 * The caller knows the slot's copy is done and no later execution writes it.
		 */
		template <class Take>
		bool Read(std::uint32_t a_frameSlot, std::uint32_t a_source, std::uint32_t a_stamp, Take&& a_take) const
		{
			if (a_frameSlot >= buffers.size() || a_source >= sources)
				return false;
			auto resource = buffers[a_frameSlot]->GetAPIResource();
			void* mapped = nullptr;
			resource.Map(&mapped);
			if (!mapped)
				return false;
			const auto* words = static_cast<const std::uint32_t*>(mapped) + std::size_t(a_source) * kSourceWords;
			const bool held = words[kCountWords] == a_stamp;
			if (held)
				a_take(words);
			resource.Unmap(0, 0);
			return held;
		}
	};

	/** @brief What a counter readback copies in an execution: per drawn source (its count buffer), its culling latch's offset in the slot's region. */
	struct CounterSource
	{
		std::uint64_t generation = 0;  // the shape's (the pass's invocation revision)
		std::shared_ptr<const org::LatchBlock> latch;
		struct Copy
		{
			std::uint32_t source = 0, latchOffset = 0;
		};
		std::vector<Copy> copies;
	};

	/** @brief The epochs whose culling counters are read back. */
	enum class CullCounterView : std::uint8_t
	{
		kColour,
		kShadow,
		kCount,
	};
	/** @brief The view's counter readback as its graph's pass last declared it; null before, or once that pass is gone. */
	std::shared_ptr<CounterReadback> CullCounterReadback(CullCounterView a_view);
	/**
	 * @brief The fixed copy of a_counts' words (by source index) and their latches' stamps into a new CounterReadback, published for
	 * a_view at its declaration (Passes/Readbacks.cpp).
	 */
	std::shared_ptr<org::RenderPass> MakeCounterReadbackPass(CullCounterView a_view, std::vector<std::shared_ptr<org::Buffer>> a_counts,
		std::function<CounterSource(const org::PassPrepareContext&)> a_source);
	struct ShadowResources;
	/** @brief The shadow epoch's counter readback pass: every drawn view's counters, by view slot (Passes/Readbacks.cpp). */
	std::shared_ptr<org::RenderPass> MakeShadowCullCounterPass(std::shared_ptr<ShadowResources> a_resources);
}
