#include "LodGates.h"

#include "ImportTimings.h"
#include "SceneCapture.h"
#include "Features/DrawcallLimitFix/Common/SceneWake.h"
#include "Features/DrawcallLimitFix/Common/Switches.h"

#include <Tracy/Tracy.hpp>
#include <ankerl/unordered_dense.h>

#include <intrin.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace DCLF::Scene
{
	// HiddenStores.cpp: LodGates' own app-cull stores as hidden events (the row past the patched stores), and whether those stores are
	// patched (the vetoes live in their stubs: no gate opens without them).
	void PushGateHidden(const RE::NiAVObject* a_object);
	bool HiddenStoresLive();
}

namespace DCLF::LodGates
{
	namespace
	{
		/**
		 * AE 1.6.1170, from the image base (skyrim-engine-notes.md, "LOD swaps and their gates"); every call site's bytes compared before
		 * any is patched. The quadtree node (0x50 bytes): +0x08 terrain block handle, +0x10 object block handle, +0x18 tree handle,
		 * +0x20/+0x28 the map's fixed-level terrain/object handles, +0x30 children (4 contiguous nodes), +0x40 flags. A handle: +0x0C state
		 * (bits 0x70000000) and references, +0x28 the block. An object block: +0x08 its NiNode (attached to the LOD root), +0x11 attached.
		 * A terrain block: +0x08 the land node and +0x10 the water node (the two its show and hide store to), +0x18 the container
		 * attached to a level node, +0x21 attached, +0x22 water registered.
		 */
		enum Offset : std::uintptr_t
		{
			// The Func2s' call sites (the gates' swap): their attaches (pre-hide) and the retirement that follows (the steal, the gate).
			kObjectUpgradeAttach = 0x51339a,     // BGSQueuedObjectUpgrade::Func2: FUN_1404fda90(child block), once per ready child
			kObjectUpgradeRetire = 0x51340c,     // ...: FUN_140510a10(parent, 2)
			kTerrainUpgradeAttach = 0x512ceb,    // FUN_140512c90 (terrain upgrade Func2): FUN_140509550(child block)
			kTerrainUpgradeRetire = 0x512d57,    // ...: FUN_140510a10(parent, 1)
			kObjectDowngradeAttach = 0x513615,   // BGSQueuedObjectDowngrade::Func2: FUN_1404fda90(parent block)
			kObjectDowngradeRetire = 0x513671,   // ...: FUN_140510c20(child, 2), once per child
			kTerrainDowngradeAttach = 0x512f55,  // BGSQueuedTerrainDowngrade::Func2: FUN_140509550(parent block)
			kTerrainDowngradeRetire = 0x512fb1,  // ...: FUN_140510c20(child, 1), once per child
			// Forced releases at call sites: FUN_14050e430's early return with the LOD roots hidden (a tail `jmp` to FUN_140509950),
			// and its full rebuild FUN_14050f790(manager, root, .., 1).
			kRootsHiddenReturn = 0x50e4e4,
			kFullRebuild = 0x50e7ae,
			// Detoured.
			kLodDrain = 0x513840,           // FUN_140513840: the swaps' drain (Func2s), from FUN_14050e430 under the manager's lock
			kRetire = 0x510a10,             // FUN_140510a10(node, bits): a node's blocks detached and released (1 terrain, 2 objects)
			kDeactivate = 0x50ed60,         // FUN_14050ed60: the manager deactivated (a worldspace's unload, FUN_14050e250, calls it first)
			kMapHideAll = 0x511b40,         // FUN_140511b40(node, show): the map's show or hide of every block
			kMapEnter = 0x511680,           // FUN_140511680: to the map's fixed levels
			kMapExit = 0x511860,            // FUN_140511860: back from them (also FUN_14050e430's LOD-off branch)
			kTerrainCacheFlush = 0x5099a0,  // FUN_1405099a0(0): the terrain block cache flushed (LOD off, the map)
			kObjectCacheFlush = 0x4fdca0,   // FUN_1404fdca0(0): the object block cache flushed
			// The engine's own retirement, as FUN_140510a10 runs it.
			kDetachObject = 0x4fd970,   // FUN_1404fd970(block)
			kDetachTerrain = 0x509770,  // FUN_140509770(block): unregisters its LOD water too
			kReleaseObject = 0x501d00,  // FUN_140501d00(handle): one reference
			kReleaseTerrain = 0x50d170,  // FUN_14050d170(handle)
			kLodVisibleTest = 0x512160,  // FUN_140512160(node, cell, objects): the update's show test (the coverage observer's)
			// Data.
			kManagerLock = 0x315b8c8,  // DAT_14315b8c8: the terrain manager's recursive BSSpinLock, held around the drain
			kTlsIndex = 0x35f0878,     // the module's TLS index: +0x768 in its block is the memory context id FUN_140510a10 sets to 0x55
		};

		struct CallSite
		{
			std::uintptr_t offset;
			std::array<std::uint8_t, 5> bytes;
		};
		// Read from the binary (Ghidra, AE 1.6.1170): each a rel32 `call` (`jmp` for the early return) to the function named above.
		constexpr std::array<CallSite, 10> kCallSites{ {
			{ kObjectUpgradeAttach, { 0xE8, 0xF1, 0xA6, 0xFE, 0xFF } },
			{ kObjectUpgradeRetire, { 0xE8, 0xFF, 0xD5, 0xFF, 0xFF } },
			{ kTerrainUpgradeAttach, { 0xE8, 0x60, 0x68, 0xFF, 0xFF } },
			{ kTerrainUpgradeRetire, { 0xE8, 0xB4, 0xDC, 0xFF, 0xFF } },
			{ kObjectDowngradeAttach, { 0xE8, 0x76, 0xA4, 0xFE, 0xFF } },
			{ kObjectDowngradeRetire, { 0xE8, 0xAA, 0xD5, 0xFF, 0xFF } },
			{ kTerrainDowngradeAttach, { 0xE8, 0xF6, 0x65, 0xFF, 0xFF } },
			{ kTerrainDowngradeRetire, { 0xE8, 0x6A, 0xDC, 0xFF, 0xFF } },
			{ kRootsHiddenReturn, { 0xE9, 0x67, 0xB4, 0xFF, 0xFF } },
			{ kFullRebuild, { 0xE8, 0xDD, 0x0F, 0x00, 0x00 } },
		} };

		std::uintptr_t Base()
		{
			static const std::uintptr_t base = REL::Module::get().base();
			return base;
		}

		using EngineFn = void (*)(std::uintptr_t);
		EngineFn EngineCall(std::uintptr_t a_offset)
		{
			return reinterpret_cast<EngineFn>(Base() + a_offset);
		}

		/**
		 * @brief The terrain manager's lock (recursive, the engine's own): every engine-side structure here (the open gates, the pending and
		 * held maps, the veto table's writes) is touched under it, as the quadtree it mirrors is. The swaps and the drain already hold it;
		 * the forced paths outside it (a worldspace's unload, the map, LOD off) take it, as the engine's own update would. LodGates adds
		 * no lock of its own.
		 */
		class EngineLock
		{
		public:
			EngineLock() :
				lock(*reinterpret_cast<RE::BSSpinLock*>(Base() + kManagerLock))
			{
				lock.Lock();
			}
			~EngineLock() { lock.Unlock(); }
			EngineLock(const EngineLock&) = delete;
			EngineLock& operator=(const EngineLock&) = delete;

		private:
			RE::BSSpinLock& lock;
		};

		/** @brief FUN_140510a10's memory context (its first act): the engine's allocation accounting sees our releases as its own. */
		class MemoryContext
		{
		public:
			explicit MemoryContext(std::uint32_t a_id)
			{
				const auto index = *reinterpret_cast<const std::uint32_t*>(Base() + kTlsIndex);
				const auto* blocks = reinterpret_cast<const std::uintptr_t*>(__readgsqword(0x58));
				slot = reinterpret_cast<std::uint32_t*>(blocks[index] + 0x768);
				saved = *slot;
				*slot = a_id;
			}
			~MemoryContext() { *slot = saved; }
			MemoryContext(const MemoryContext&) = delete;
			MemoryContext& operator=(const MemoryContext&) = delete;

		private:
			std::uint32_t* slot;
			std::uint32_t saved;
		};

		enum class Kind : std::uint8_t
		{
			Object,
			Terrain
		};

		template <class T>
		T& At(std::uintptr_t a_address, std::uintptr_t a_offset)
		{
			return *reinterpret_cast<T*>(a_address + a_offset);
		}

		std::uintptr_t HandleSlot(Kind a_kind) { return a_kind == Kind::Object ? 0x10 : 0x08; }
		std::uintptr_t MapHandleSlot(Kind a_kind) { return a_kind == Kind::Object ? 0x28 : 0x20; }

		// FUN_140510a10's test before a detach: the handle's state 3 or 4 (loaded).
		bool HandleReady(std::uintptr_t a_handle)
		{
			const std::uint32_t state = *reinterpret_cast<const volatile std::uint32_t*>(a_handle + 0x0C);
			return (((state & 0x70000000u) + 0xD0000000u) & 0xEFFFFFFFu) == 0;
		}

		bool Attached(std::uintptr_t a_block, Kind a_kind)
		{
			return At<const volatile std::uint8_t>(a_block, a_kind == Kind::Object ? 0x11 : 0x21) != 0;
		}

		// FUN_140510a10's terrain detach also wants the node's level field at most 8 (every LOD level, 4 to 32).
		bool TerrainLevelDetaches(std::uintptr_t a_node)
		{
			return (At<const volatile std::uint32_t>(a_node, 0x40) & 0x7F800000u) <= 0x4000000u;
		}

		std::array<RE::NiAVObject*, 2> BlockNodes(std::uintptr_t a_block, Kind a_kind)
		{
			if (a_kind == Kind::Object)
				return { At<RE::NiAVObject*>(a_block, 0x08), nullptr };
			return { At<RE::NiAVObject*>(a_block, 0x08), At<RE::NiAVObject*>(a_block, 0x10) };
		}

		// NiAVObject::flags (+0xF4) bit 0, app-cull: the bit every LOD show and hide stores. Atomic, as other bits of the dword may be
		// written by the engine at the same time. Returns whether the bit changed.
		bool SetHidden(RE::NiAVObject* a_object, bool a_hidden)
		{
			std::atomic_ref<std::uint32_t> flags(At<std::uint32_t>(reinterpret_cast<std::uintptr_t>(a_object), 0xF4));
			const auto before = a_hidden ? flags.fetch_or(1u, std::memory_order_relaxed) : flags.fetch_and(~1u, std::memory_order_relaxed);
			return ((before & 1u) != 0) != a_hidden;
		}

		bool IsHidden(const RE::NiAVObject* a_object)
		{
			return (At<const volatile std::uint32_t>(reinterpret_cast<std::uintptr_t>(a_object), 0xF4) & 1u) != 0;
		}

		std::int64_t Ticks()
		{
			LARGE_INTEGER now;
			QueryPerformanceCounter(&now);
			return now.QuadPart;
		}

		double TicksToMs(std::int64_t a_ticks)
		{
			static const double msPerTick = [] {
				LARGE_INTEGER frequency;
				QueryPerformanceFrequency(&frequency);
				return 1000.0 / static_cast<double>(frequency.QuadPart);
			}();
			return static_cast<double>(a_ticks) * msPerTick;
		}

		/**
		 * @brief A gate's state, with its token: one word, `token << 3 | state`, so that a compare-exchange from Open names the gate too (a
		 * record is reused for later gates; a stale token's exchange fails). Open -> Flipping -> Flipped -> Released (the render thread's
		 * flip, then the drain's release), or Open -> Forced (a safety path, or superseded by a later swap of the same nodes).
		 */
		enum State : std::uint64_t
		{
			kFree = 0,
			kOpen = 1,
			kFlipping = 2,  // the render thread is storing the flip's app-cull bits (flipActive)
			kFlipped = 3,
			kForced = 4,
			kReleased = 5,
		};
		constexpr std::uint64_t Word(std::uint64_t a_token, State a_state) { return (a_token << 3) | a_state; }

		enum Handoff : std::uint8_t
		{
			kOwned = 0,
			kReattached = 1,  // attached again by a swap while its gate was open (the engine reused it): the flip leaves it shown
			kHandedOff = 2,   // stolen again by a later gate (from its node, after a reattach): that gate owns its visibility
		};

		struct Block
		{
			std::uintptr_t block = 0;   // the handle's +0x28
			std::uintptr_t handle = 0;  // held: the stolen handle, whose reference DCLF owns; incoming: 0 (the attach sees the block only)
			std::uintptr_t node = 0;    // held: the quadtree node it was taken from (the reuse check)
			Kind kind = Kind::Object;
			std::array<RE::NiAVObject*, 2> nodes{};  // the app-cull nodes
			std::atomic<std::uint8_t> handoff{ kOwned };  // held, while its gate lives: Handoff

			Block() = default;
			Block(const Block& a_other) :
				block(a_other.block), handle(a_other.handle), node(a_other.node), kind(a_other.kind), nodes(a_other.nodes), handoff(a_other.handoff.load(std::memory_order_relaxed)) {}
			Block& operator=(const Block& a_other)
			{
				block = a_other.block;
				handle = a_other.handle;
				node = a_other.node;
				kind = a_other.kind;
				nodes = a_other.nodes;
				handoff.store(a_other.handoff.load(std::memory_order_relaxed), std::memory_order_relaxed);
				return *this;
			}
		};

		/**
		 * @brief One gate. Type-stable: records are reused, never freed, so a stale reference (the veto table's, the render thread's lookup,
		 * a pending block's) reads a live word and finds its token gone. The vectors are written by the engine side before the gate is
		 * published (Open) and after it leaves Open by the engine's own exchange; the render thread reads them only between its exchange to
		 * Flipping and its store of Flipped.
		 */
		struct Gate
		{
			std::atomic<std::uint64_t> word{ Word(0, kFree) };
			std::uint64_t token = 0;
			std::vector<Block> incoming, outgoing;
			std::vector<RE::NiAVObject*> incomingNodes, outgoingNodes;
			std::int64_t openedTicks = 0, flippedTicks = 0;
			std::uint32_t openedFrame = 0, flippedFrame = 0;
		};

		bool Live(const Gate* a_gate, std::uint64_t a_token)
		{
			const auto word = a_gate->word.load(std::memory_order_acquire);
			return word == Word(a_token, kOpen) || word == Word(a_token, kFlipping);
		}

		// Live, or flipped and not yet released: the veto table keeps the entry (the coverage observer's re-hide marks).
		bool Unreleased(const Gate* a_gate, std::uint64_t a_token)
		{
			return Live(a_gate, a_token) || a_gate->word.load(std::memory_order_acquire) == Word(a_token, kFlipped);
		}

		// ---------------------------------------------------------------------------------------------------------------------------
		// The veto table: the pending incoming nodes, read lock-free by HiddenStores' veto stubs on any thread. Written by the engine side
		// alone (under the manager's lock); a slot is filled once and never reused (a removal is a tombstone), so a reader's entry never
		// changes under it; whether it vetoes is its gate's word (Open or Flipping). Growth (and the compaction of resolved entries)
		// builds a new table and publishes it; an old table is freed once no reader is inside one (vetoReaders seen at 0 after the
		// publication: a reader counts itself before loading the table). No cap: the table grows with the open gates' nodes.
		// ---------------------------------------------------------------------------------------------------------------------------
		struct VetoEntry
		{
			std::atomic<std::uintptr_t> key{ 0 };  // 0 empty, kTomb removed, else the NiAVObject
			const Gate* gate = nullptr;
			std::uint64_t token = 0;
			// Parity observer (NoteEngineHide): the engine's LOD hide store that hid the node again after its gate's flip (its offset), else 0.
			std::atomic<std::uint32_t> rehidSite{ 0 };
		};
		constexpr std::uintptr_t kTomb = 1;

		struct VetoTable
		{
			explicit VetoTable(std::size_t a_capacity) :
				capacity(a_capacity), entries(std::make_unique<VetoEntry[]>(a_capacity)) {}
			std::size_t capacity;  // a power of two, at least twice the slots filled (a probe always meets an empty slot)
			std::size_t filled = 0;
			std::unique_ptr<VetoEntry[]> entries;
		};

		std::size_t Hash(std::uintptr_t a_key, std::size_t a_capacity)
		{
			std::uint64_t h = static_cast<std::uint64_t>(a_key) * 0x9E3779B97F4A7C15ull;
			h ^= h >> 32;
			return static_cast<std::size_t>(h) & (a_capacity - 1);
		}

		std::atomic<VetoTable*> vetoTable{ nullptr };
		std::atomic<std::uint32_t> vetoReaders{ 0 };
		std::atomic<std::uint32_t> openGates{ 0 };  // gates in Open: the stubs' fast test
		std::atomic<std::uint32_t> flipActive{ 0 };  // the render thread between a flip's exchange and its Flipped
		std::atomic<std::uint32_t> engineWork{ 0 };  // open + pending + deferred: the hooks' fast test (no lock taken at 0)
		std::atomic<std::uint64_t> forcedGeneration{ 0 };

		// ---------------------------------------------------------------------------------------------------------------------------
		// Counters, since the last report.
		// ---------------------------------------------------------------------------------------------------------------------------
		enum class Flush : std::uint8_t
		{
			Deactivate,
			MapHideAll,
			MapEnter,
			MapExit,
			TerrainCache,
			ObjectCache,
			RootsHidden,
			Rebuild,
			Stopped,
			Count
		};
		constexpr std::array<const char*, static_cast<std::size_t>(Flush::Count)> kFlushNames{ "deactivate", "map hide-all", "map enter", "map exit",
			"terrain cache", "object cache", "roots hidden", "rebuild", "DCLF stopped" };

		struct Counters
		{
			std::atomic<std::uint64_t> opened{ 0 }, flipped{ 0 }, flipMisses{ 0 }, forced{ 0 }, superseded{ 0 }, held{ 0 }, released{ 0 }, reused{ 0 },
				handedOff{ 0 }, deferred{ 0 }, vetoes{ 0 }, orphansShown{ 0 }, coverageBoth{ 0 }, coverageNeither{ 0 }, coverageEngineHid{ 0 }, engineRehides{ 0 };
			std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(Flush::Count)> flushes{};
			std::atomic<std::int64_t> flipTicks{ 0 }, flipTicksMax{ 0 }, releaseTicks{ 0 }, releaseTicksMax{ 0 };
			std::atomic<std::uint64_t> flipFrames{ 0 }, releases{ 0 };
			// Set at each drain (the engine side's view): the open gates and the oldest one's age.
			std::atomic<std::uint32_t> open{ 0 }, oldestFrames{ 0 };
			std::atomic<std::int64_t> oldestTicks{ 0 };
		};
		Counters counters;

		void NoteMax(std::atomic<std::int64_t>& a_max, std::int64_t a_value)
		{
			auto current = a_max.load(std::memory_order_relaxed);
			while (a_value > current && !a_max.compare_exchange_weak(current, a_value, std::memory_order_relaxed)) {
			}
		}

		// ---------------------------------------------------------------------------------------------------------------------------
		// Outcomes (to the coordinator) and opened gates (to the render thread's lookup): lock-free stacks.
		// ---------------------------------------------------------------------------------------------------------------------------
		struct OutcomeNode
		{
			Outcome outcome;
			OutcomeNode* next = nullptr;
		};
		std::atomic<OutcomeNode*> outcomes{ nullptr };

		void PostOutcome(const Outcome& a_outcome)
		{
			auto* node = new OutcomeNode{ a_outcome, outcomes.load(std::memory_order_relaxed) };
			while (!outcomes.compare_exchange_weak(node->next, node, std::memory_order_release, std::memory_order_relaxed)) {
			}
		}

		struct OpenedNode
		{
			Gate* gate;
			std::uint64_t token;
			OpenedNode* next = nullptr;
		};
		std::atomic<OpenedNode*> opened{ nullptr };

		// The render thread's: token -> gate, from the opened stack (Flip's lookup; nothing else reads it).
		ankerl::unordered_dense::map<std::uint64_t, Gate*> renderGates;
		std::size_t renderPruneMark = 256;

		bool installed = false;
		bool detoursInstalled = false;
		bool (*running)() = nullptr;  // DrawcallLimitFix::Running (Install's): the gates hold only while DCLF runs
		bool coverageObserver = false;  // the parity switches: the coverage invariant checked at each release

		// ---------------------------------------------------------------------------------------------------------------------------
		// The engine side: under the manager's lock (EngineLock), on whichever engine thread holds it.
		// ---------------------------------------------------------------------------------------------------------------------------
		struct GateRef
		{
			Gate* gate = nullptr;
			std::uint64_t token = 0;
		};
		struct Pending
		{
			GateRef ref;
			Kind kind = Kind::Object;
			std::array<RE::NiAVObject*, 2> nodes{};
		};

		struct EngineSide
		{
			std::vector<Gate*> open;  // Open, Flipping or Flipped (awaiting its release), oldest first
			std::vector<Gate*> spare;  // released records, reused (type-stable)
			ankerl::unordered_dense::map<std::uintptr_t, Pending> pending;  // an incoming block (hidden, vetoed) -> its gate
			ankerl::unordered_dense::map<std::uintptr_t, GateRef> held;     // a held block -> the gate that holds it (the latest)
			std::vector<Block> deferred;  // held blocks to release once no flip is in progress (their nodes are the flip's)
			std::unique_ptr<VetoTable> table;  // the published one
			std::vector<std::unique_ptr<VetoTable>> retiredTables;
			std::uint64_t nextToken = 0;
			bool orphans = false;  // a gate was superseded with pending blocks the swap did not retire: SweepOrphans shows them
		};
		EngineSide& Side()
		{
			static EngineSide side;
			return side;
		}

		void Publish(EngineSide& a_side)
		{
			engineWork.store(static_cast<std::uint32_t>(a_side.open.size() + a_side.pending.size() + a_side.deferred.size()), std::memory_order_release);
		}

		void FreeRetiredTables(EngineSide& a_side)
		{
			// The tables were unpublished before this load (seq_cst both): a reader that counts itself after it loads the new table.
			if (!a_side.retiredTables.empty() && vetoReaders.load(std::memory_order_seq_cst) == 0)
				a_side.retiredTables.clear();
		}

		VetoTable& RebuildVeto(EngineSide& a_side, std::size_t a_extra)
		{
			std::size_t live = 0;
			if (a_side.table)
				for (std::size_t i = 0; i < a_side.table->capacity; ++i) {
					const auto& entry = a_side.table->entries[i];
					const auto key = entry.key.load(std::memory_order_relaxed);
					live += key > kTomb && Unreleased(entry.gate, entry.token);
				}
			std::size_t capacity = 64;
			while (capacity < (live + a_extra) * 4)
				capacity <<= 1;
			auto fresh = std::make_unique<VetoTable>(capacity);
			if (a_side.table)
				for (std::size_t i = 0; i < a_side.table->capacity; ++i) {
					const auto& entry = a_side.table->entries[i];
					const auto key = entry.key.load(std::memory_order_relaxed);
					if (key <= kTomb || !Unreleased(entry.gate, entry.token))
						continue;
					auto slot = Hash(key, capacity);
					while (fresh->entries[slot].key.load(std::memory_order_relaxed))
						slot = (slot + 1) & (capacity - 1);
					fresh->entries[slot].gate = entry.gate;
					fresh->entries[slot].token = entry.token;
					fresh->entries[slot].rehidSite.store(entry.rehidSite.load(std::memory_order_relaxed), std::memory_order_relaxed);
					fresh->entries[slot].key.store(key, std::memory_order_relaxed);
					++fresh->filled;
				}
			vetoTable.store(fresh.get(), std::memory_order_seq_cst);
			if (a_side.table)
				a_side.retiredTables.push_back(std::move(a_side.table));
			a_side.table = std::move(fresh);
			FreeRetiredTables(a_side);
			return *a_side.table;
		}

		/** @brief The engine side's (the table's writer): the node's entry for this gate, or null. */
		const VetoEntry* FindVeto(const EngineSide& a_side, const RE::NiAVObject* a_object, std::uint64_t a_token)
		{
			const auto key = reinterpret_cast<std::uintptr_t>(a_object);
			const VetoTable* table = a_side.table.get();
			if (!table || key <= kTomb)
				return nullptr;
			for (auto slot = Hash(key, table->capacity);; slot = (slot + 1) & (table->capacity - 1)) {
				const auto& entry = table->entries[slot];
				const auto at = entry.key.load(std::memory_order_relaxed);
				if (!at)
					return nullptr;
				if (at == key && entry.token == a_token)
					return &entry;
			}
		}

		void VetoInsert(EngineSide& a_side, const RE::NiAVObject* a_object, const Gate* a_gate, std::uint64_t a_token)
		{
			const auto key = reinterpret_cast<std::uintptr_t>(a_object);
			VetoTable* table = a_side.table.get();
			if (!table || (table->filled + 1) * 2 > table->capacity)
				table = &RebuildVeto(a_side, 1);
			const auto mask = table->capacity - 1;
			auto slot = Hash(key, table->capacity);
			// One live entry per key: an earlier one (a resolved gate's, the node gated again) is removed on the way to an empty slot.
			for (std::uintptr_t at; (at = table->entries[slot].key.load(std::memory_order_relaxed)) != 0; slot = (slot + 1) & mask)
				if (at == key)
					table->entries[slot].key.store(kTomb, std::memory_order_release);
			table->entries[slot].gate = a_gate;
			table->entries[slot].token = a_token;
			table->entries[slot].key.store(key, std::memory_order_release);
			++table->filled;
		}

		thread_local struct Build
		{
			std::uint32_t expected = 0;  // the swap's retirement calls still to come (upgrade 1, downgrade 4: one per child)
			bool retiring = false;       // inside the swap's retirement with pending incoming: the retire detour steals
			std::vector<Block> incoming, outgoing;
			void Reset()
			{
				expected = 0;
				retiring = false;
				incoming.clear();
				outgoing.clear();
			}
		} build;

		bool Active()
		{
			return installed && Scene::HiddenStoresLive() && (!running || running());
		}

		Gate* AllocateGate(EngineSide& a_side)
		{
			if (a_side.spare.empty())
				return new Gate();  // type-stable: never freed (see Gate)
			auto* gate = a_side.spare.back();
			a_side.spare.pop_back();
			return gate;
		}

		void Recycle(EngineSide& a_side, Gate* a_gate)
		{
			a_gate->incoming.clear();
			a_gate->outgoing.clear();
			a_gate->incomingNodes.clear();
			a_gate->outgoingNodes.clear();
			std::erase(a_side.open, a_gate);
			a_side.spare.push_back(a_gate);
		}

		void Unhide(const std::array<RE::NiAVObject*, 2>& a_nodes)
		{
			for (auto* node : a_nodes)
				if (node && SetHidden(node, false))
					Scene::PushGateHidden(node);
		}

		void EraseIfHeldBy(EngineSide& a_side, std::uintptr_t a_block, std::uint64_t a_token)
		{
			if (const auto it = a_side.held.find(a_block); it != a_side.held.end() && it->second.token == a_token)
				a_side.held.erase(it);
		}

		void ErasePendingOf(EngineSide& a_side, const Gate& a_gate)
		{
			for (const auto& block : a_gate.incoming)
				if (const auto it = a_side.pending.find(block.block); it != a_side.pending.end() && it->second.ref.token == a_gate.token)
					a_side.pending.erase(it);
		}

		/**
		 * @brief A held block's release, as FUN_140510a10 would have run it at the swap: detached when loaded and attached (a terrain block
		 * when its node's level allows, as the engine's test), then the reference released. Reuse: a quadtree node references the
		 * handle again (the engine's swap back found the block DCLF still held; a block is cached per node, so its own node's slot or that
		 * node's map slot is the only place) - only DCLF's reference goes, and the block stays attached and shown. While a flip is in
		 * progress the release waits for a later drain (its nodes may be the flip's), unless a forced flush already waited the flip out.
		 */
		void ReleaseHeld(EngineSide& a_side, const Block& a_block, bool a_force)
		{
			if (!a_force && flipActive.load(std::memory_order_acquire)) {
				a_side.deferred.push_back(a_block);
				counters.deferred.fetch_add(1, std::memory_order_relaxed);
				return;
			}
			MemoryContext context(0x55);
			const auto handoff = a_block.handoff.load(std::memory_order_relaxed);
			const bool reused = At<std::uintptr_t>(a_block.node, HandleSlot(a_block.kind)) == a_block.handle ||
			                    At<std::uintptr_t>(a_block.node, MapHandleSlot(a_block.kind)) == a_block.handle;
			if (handoff == kHandedOff) {
				counters.handedOff.fetch_add(1, std::memory_order_relaxed);
			} else if (reused) {
				counters.reused.fetch_add(1, std::memory_order_relaxed);
				// Attached again while its gate was open: the engine shows it; a flip that raced the reattach may have hidden it.
				if (handoff == kReattached)
					Unhide(a_block.nodes);
			} else if (HandleReady(a_block.handle) && At<std::uintptr_t>(a_block.handle, 0x28) == a_block.block && Attached(a_block.block, a_block.kind)) {
				if (a_block.kind == Kind::Object)
					EngineCall(kDetachObject)(a_block.block);
				else if (TerrainLevelDetaches(a_block.node))
					EngineCall(kDetachTerrain)(a_block.block);
			}
			EngineCall(a_block.kind == Kind::Object ? kReleaseObject : kReleaseTerrain)(a_block.handle);
			counters.released.fetch_add(1, std::memory_order_relaxed);
		}

		void ReleaseDeferred(EngineSide& a_side, bool a_force)
		{
			if (a_side.deferred.empty() || (!a_force && flipActive.load(std::memory_order_acquire)))
				return;
			auto blocks = std::exchange(a_side.deferred, {});
			for (const auto& block : blocks)
				ReleaseHeld(a_side, block, a_force);
		}

		void NoteForced(Gate& a_gate)
		{
			forcedGeneration.fetch_add(1, std::memory_order_acq_rel);
			PostOutcome({ a_gate.token, true });
			counters.forced.fetch_add(1, std::memory_order_relaxed);
		}

		/**
		 * @brief Pending blocks whose gate was resolved without them (superseded by a swap that retired other blocks of it): still attached,
		 * so shown, ungated; the rest are gone from the map already (their retirement erased them).
		 */
		void SweepOrphans(EngineSide& a_side)
		{
			if (!a_side.orphans)
				return;
			a_side.orphans = false;
			for (auto it = a_side.pending.begin(); it != a_side.pending.end();) {
				const auto word = it->second.ref.gate->word.load(std::memory_order_acquire);
				if ((word >> 3) == it->second.ref.token && (word & 7) != kForced) {
					++it;
					continue;
				}
				if (Attached(it->first, it->second.kind)) {
					Unhide(it->second.nodes);
					counters.orphansShown.fetch_add(1, std::memory_order_relaxed);
				}
				it = a_side.pending.erase(it);
			}
		}

		/**
		 * @brief A gate a swap superseded (the caller's exchange took it from Open to Forced): one of its pending blocks is being retired.
		 * Its other pending blocks become orphans (SweepOrphans, at the swap's end or the next drain: most are retired by the same swap).
		 * Its held blocks: reused ones lose DCLF's reference; inside a swap that opens a gate they move to it (still shown, held until
		 * that gate's flip); outside one the area is being retired, and they are released now.
		 */
		void Supersede(EngineSide& a_side, Gate& a_gate, Build* a_into)
		{
			openGates.fetch_sub(1, std::memory_order_acq_rel);
			a_side.orphans = true;
			for (const auto& block : a_gate.outgoing) {
				EraseIfHeldBy(a_side, block.block, a_gate.token);
				const bool reused = At<std::uintptr_t>(block.node, HandleSlot(block.kind)) == block.handle ||
				                    At<std::uintptr_t>(block.node, MapHandleSlot(block.kind)) == block.handle;
				if (a_into && !reused && block.handoff.load(std::memory_order_relaxed) == kOwned) {
					auto& moved = a_into->outgoing.emplace_back(block);
					moved.handoff.store(kOwned, std::memory_order_relaxed);
				} else
					ReleaseHeld(a_side, block, false);
			}
			counters.superseded.fetch_add(1, std::memory_order_relaxed);
			NoteForced(a_gate);
			Recycle(a_side, &a_gate);
			WakeSceneGate();
		}

		/** @brief A held block stolen again by a later swap (its node referenced it again): the earlier gate's copy only drops its reference. */
		void HandOff(EngineSide& a_side, std::uintptr_t a_block)
		{
			const auto it = a_side.held.find(a_block);
			if (it == a_side.held.end())
				return;
			const auto ref = it->second;
			a_side.held.erase(it);
			if ((ref.gate->word.load(std::memory_order_acquire) >> 3) != ref.token)
				return;
			for (auto& block : ref.gate->outgoing)
				if (block.block == a_block)
					block.handoff.store(kHandedOff, std::memory_order_relaxed);
		}

		/**
		 * @brief Parity runs: at a flipped gate's release, exactly one level shown (the incoming, still attached, against the outgoing),
		 * unless the engine itself hid the incoming again after the flip. The terrain manager's own update hides LOD under the loaded
		 * cells (terrain: FUN_140510110's param_3, FUN_140512160(node, player cell, 0), 0 when every cell around the node is loaded, ->
		 * FUN_140509b20; objects: FUN_1405103e0's param_3, FUN_140512160(node, cell, 1), the cell's LOD bitset, -> the store at
		 * 0x1405106a4), as it does after an ungated swap's attach: neither level shown there is the engine's, the loaded cells cover it.
		 * Those re-hides are marked on the gate's veto entries (NoteEngineHide, from HiddenStores' stubs) and counted apart; an incoming
		 * block hidden without one is a hole. Each flagged gate names, per hidden incoming block, the store that hid it, the engine's test
		 * now and whether the node is still attached at that level.
		 */
		void CheckCoverage(EngineSide& a_side, const Gate& a_gate)
		{
			std::uint32_t incoming = 0, incomingShown = 0, engineHid = 0, outgoingShown = 0;
			std::string detail;
			auto* player = RE::PlayerCharacter::GetSingleton();
			// An exterior cell only: FUN_140512160's loaded-cells walk reads the cell's worldspace (null for an interior), as the update
			// that calls it runs only for an exterior.
			auto* cell = player ? player->GetParentCell() : nullptr;
			if (cell && (At<const volatile std::uint8_t>(reinterpret_cast<std::uintptr_t>(cell), 0x40) & 1) != 0)
				cell = nullptr;
			using Test = std::uint64_t (*)(std::uintptr_t, RE::TESObjectCELL*, std::uint8_t);
			const auto test = reinterpret_cast<Test>(Base() + kLodVisibleTest);
			for (const auto& block : a_gate.incoming) {
				const auto it = a_side.pending.find(block.block);
				if (it == a_side.pending.end() || it->second.ref.token != a_gate.token || !Attached(block.block, block.kind) || !block.nodes[0])
					continue;
				++incoming;
				if (!IsHidden(block.nodes[0])) {
					++incomingShown;
					continue;
				}
				const auto* entry = FindVeto(a_side, block.nodes[0], a_gate.token);
				const std::uint32_t site = entry ? entry->rehidSite.load(std::memory_order_relaxed) : 0u;
				engineHid += site != 0;
				const auto node = At<std::uintptr_t>(block.block, 0x00);
				const std::uint32_t flags = At<const volatile std::uint32_t>(node, 0x40);
				const bool levelAttached = (flags & (block.kind == Kind::Object ? 0x80u : 0x4u)) != 0;
				const int wanted = cell ? static_cast<int>(test(node, cell, block.kind == Kind::Object ? std::uint8_t(1) : std::uint8_t(0)) & 0xFF) : -1;
				if (detail.size() < 400)
					detail += fmt::format("{}{} level {} (attached at it {}): hidden by {}, the engine's test {}", detail.empty() ? "" : "; ",
						block.kind == Kind::Object ? "object" : "terrain", (flags >> 21) & 0x3FC, levelAttached,
						site ? fmt::format("{:#x}", 0x140000000ull + site) : std::string("none seen"),
						wanted < 0 ? "-" : wanted ? "show" : "hide (the loaded cells)");
			}
			for (const auto& block : a_gate.outgoing)
				if (block.handoff.load(std::memory_order_relaxed) == kOwned && block.nodes[0] && Attached(block.block, block.kind))
					outgoingShown += !IsHidden(block.nodes[0]);
			const bool both = incomingShown && outgoingShown;
			const bool neither = incoming && !incomingShown && !outgoingShown;
			if (!both && !neither)
				return;
			if (neither && engineHid == incoming) {
				counters.coverageEngineHid.fetch_add(1, std::memory_order_relaxed);
				return;
			}
			const auto count = (both ? counters.coverageBoth : counters.coverageNeither).fetch_add(1, std::memory_order_relaxed);
			if (count < 8)
				logger::warn("[DCLF] LOD gate {}: at its release {} of {} incoming blocks shown ({} hidden again by the engine) and {} of {} outgoing{}{} <- GATE COVERAGE",
					a_gate.token, incomingShown, incoming, engineHid, outgoingShown, a_gate.outgoing.size(), detail.empty() ? "" : ": ", detail);
		}

		void ReleaseFlipped(EngineSide& a_side, Gate& a_gate)
		{
			if (coverageObserver)
				CheckCoverage(a_side, a_gate);
			for (const auto& block : a_gate.outgoing) {
				EraseIfHeldBy(a_side, block.block, a_gate.token);
				ReleaseHeld(a_side, block, false);
			}
			ErasePendingOf(a_side, a_gate);
			const auto ticks = Ticks() - a_gate.flippedTicks;
			counters.releaseTicks.fetch_add(ticks, std::memory_order_relaxed);
			NoteMax(counters.releaseTicksMax, ticks);
			counters.releases.fetch_add(1, std::memory_order_relaxed);
			a_gate.word.store(Word(a_gate.token, kReleased), std::memory_order_release);
			Recycle(a_side, &a_gate);
		}

		/**
		 * @brief Every gate resolved now (unload, the map, LOD off, a rebuild, DCLF stopped): open ones forced (their pending blocks shown,
		 * their held ones released), flipped ones released. A flip already past its exchange finishes first: the one wait in LodGates,
		 * bounded by that flip's few stores (after the exchanges no other flip can start).
		 */
		void FlushAll(Flush a_reason)
		{
			if (!installed || engineWork.load(std::memory_order_acquire) == 0)
				return;
			EngineLock lock;
			auto& side = Side();
			std::vector<Gate*> forced;
			for (auto* gate : side.open) {
				auto expected = Word(gate->token, kOpen);
				if (gate->word.compare_exchange_strong(expected, Word(gate->token, kForced), std::memory_order_acq_rel)) {
					openGates.fetch_sub(1, std::memory_order_acq_rel);
					forced.push_back(gate);
				}
			}
			while (flipActive.load(std::memory_order_acquire))
				std::this_thread::yield();
			for (auto* gate : std::vector<Gate*>(side.open)) {
				if (std::ranges::find(forced, gate) != forced.end()) {
					for (const auto& block : gate->incoming)
						if (const auto it = side.pending.find(block.block); it != side.pending.end() && it->second.ref.token == gate->token) {
							if (Attached(block.block, block.kind))
								Unhide(block.nodes);
							side.pending.erase(it);
						}
					for (const auto& block : gate->outgoing) {
						EraseIfHeldBy(side, block.block, gate->token);
						ReleaseHeld(side, block, true);
					}
					NoteForced(*gate);
					Recycle(side, gate);
				} else if ((gate->word.load(std::memory_order_acquire) & 7) == kFlipped)
					ReleaseFlipped(side, *gate);
			}
			ReleaseDeferred(side, true);
			side.orphans = true;
			SweepOrphans(side);
			counters.flushes[static_cast<std::size_t>(a_reason)].fetch_add(1, std::memory_order_relaxed);
			Publish(side);
			if (!forced.empty())
				WakeSceneGate();
		}

		// ---------------------------------------------------------------------------------------------------------------------------
		// The swap: attaches (pre-hide), the retirement (steal), the gate (Emit).
		// ---------------------------------------------------------------------------------------------------------------------------

		/**
		 * @brief An incoming block's attach in a Func2: hidden before it when it was not attached (a block already attached and shown, a
		 * reused one, must not go dark), the hide undone when the attach did nothing (FUN_140509550 attaches only a loaded terrain block).
		 * No hidden event: the block is not in the world until the attach, whose capture reads the bit. A block attached already that a
		 * gate holds: its gate open - reused while shown (the flip leaves it); its gate flipped - hidden by the flip, so incoming again.
		 */
		template <class F>
		void OnAttach(std::uintptr_t a_block, Kind a_kind, std::uint32_t a_retireCalls, F&& a_original)
		{
			if (!Active()) {
				a_original(a_block);
				return;
			}
			EngineLock lock;
			auto& b = build;
			if (!b.expected)
				b.Reset();
			b.expected = a_retireCalls;
			const auto nodes = BlockNodes(a_block, a_kind);
			const bool wasAttached = Attached(a_block, a_kind);
			std::array<bool, 2> preHidden{};
			if (!wasAttached)
				for (std::size_t i = 0; i < nodes.size(); ++i)
					preHidden[i] = nodes[i] && SetHidden(nodes[i], true);
			a_original(a_block);
			Block incoming;
			incoming.block = a_block;
			incoming.kind = a_kind;
			incoming.nodes = nodes;
			if (!wasAttached) {
				if (Attached(a_block, a_kind))
					b.incoming.push_back(incoming);
				else
					for (std::size_t i = 0; i < nodes.size(); ++i)
						if (preHidden[i])
							SetHidden(nodes[i], false);
				return;
			}
			auto& side = Side();
			const auto it = side.held.find(a_block);
			if (it == side.held.end())
				return;
			const auto [gate, token] = it->second;
			const auto word = gate->word.load(std::memory_order_acquire);
			if (word == Word(token, kOpen)) {
				for (auto& block : gate->outgoing)
					if (block.block == a_block)
						block.handoff.store(kReattached, std::memory_order_relaxed);
			} else if (word == Word(token, kFlipping) || word == Word(token, kFlipped))
				b.incoming.push_back(incoming);
		}

		/**
		 * @brief FUN_140510a10's body around the engine's: the steal inside a gated swap (a loaded, attached block taken from its node, so
		 * the engine skips its detach and release and DCLF owns the reference), and every retirement of a pending block (its gate
		 * superseded; the engine retires the block, hidden and never shown). A pending block whose gate a flip is showing right now is
		 * held instead (stolen, released at a later drain): its nodes are the flip's until it ends. Trees (4) and the map's handles (0x10)
		 * pass through.
		 */
		template <class F>
		void Retire(std::uintptr_t a_node, std::uint32_t a_bits, F&& a_original)
		{
			auto& b = build;
			if (!installed || (!b.retiring && engineWork.load(std::memory_order_acquire) == 0)) {
				a_original(a_node, a_bits);
				return;
			}
			EngineLock lock;
			auto& side = Side();
			constexpr std::array<std::pair<std::uint32_t, Kind>, 2> kSlots{ { { 1u, Kind::Terrain }, { 2u, Kind::Object } } };
			for (const auto& [bit, kind] : kSlots) {
				if (!(a_bits & bit))
					continue;
				auto& slot = At<std::uintptr_t>(a_node, HandleSlot(kind));
				const auto handle = slot;
				if (!handle || !HandleReady(handle))
					continue;
				const auto block = At<std::uintptr_t>(handle, 0x28);
				if (!block)
					continue;
				bool hiddenRetired = false, flipping = false;
				if (const auto it = side.pending.find(block); it != side.pending.end()) {
					const auto ref = it->second.ref;
					side.pending.erase(it);
					auto expected = Word(ref.token, kOpen);
					if (ref.gate->word.compare_exchange_strong(expected, Word(ref.token, kForced), std::memory_order_acq_rel)) {
						Supersede(side, *ref.gate, b.retiring ? &b : nullptr);
						hiddenRetired = true;
					} else if (expected == Word(ref.token, kFlipping))
						flipping = true;
					else if ((expected >> 3) != ref.token || (expected & 7) == kForced)
						hiddenRetired = true;  // an orphan: still hidden, its gate resolved without it
				}
				if (hiddenRetired)
					continue;  // the engine detaches and releases it
				const bool shown = Attached(block, kind) && (kind == Kind::Object || TerrainLevelDetaches(a_node));
				if (!shown)
					continue;
				Block held;
				held.block = block;
				held.handle = handle;
				held.node = a_node;
				held.kind = kind;
				held.nodes = BlockNodes(block, kind);
				if (b.retiring) {
					HandOff(side, block);
					slot = 0;  // the steal: FUN_140510a10 skips the detach and the release; the reference is DCLF's
					b.outgoing.push_back(held);
					counters.held.fetch_add(1, std::memory_order_relaxed);
				} else if (flipping) {
					slot = 0;
					side.deferred.push_back(held);
					counters.deferred.fetch_add(1, std::memory_order_relaxed);
				}
			}
			a_original(a_node, a_bits);
			Publish(side);
		}

		/**
		 * @brief The swap's end (its last retirement call): orphans shown, then the gate opened when an incoming block is pending - published
		 * to the render thread's lookup, then captured as the tracker's Gate event, in order after the attaches.
		 */
		void Emit(Build& a_build)
		{
			EngineLock lock;
			auto& side = Side();
			SweepOrphans(side);
			if (a_build.incoming.empty()) {
				// No steal happens without incoming (retiring): nothing can be held here.
				a_build.Reset();
				Publish(side);
				return;
			}
			auto* gate = AllocateGate(side);
			gate->token = ++side.nextToken;
			gate->incoming = std::move(a_build.incoming);
			gate->outgoing = std::move(a_build.outgoing);
			for (const auto& block : gate->incoming)
				for (auto* node : block.nodes)
					if (node)
						gate->incomingNodes.push_back(node);
			for (const auto& block : gate->outgoing)
				for (auto* node : block.nodes)
					if (node)
						gate->outgoingNodes.push_back(node);
			gate->openedTicks = Ticks();
			gate->openedFrame = SceneCapture::Frame();
			gate->word.store(Word(gate->token, kOpen), std::memory_order_release);
			openGates.fetch_add(1, std::memory_order_acq_rel);
			for (const auto& block : gate->incoming) {
				side.pending[block.block] = Pending{ { gate, gate->token }, block.kind, block.nodes };
				for (auto* node : block.nodes)
					if (node)
						VetoInsert(side, node, gate, gate->token);
			}
			for (const auto& block : gate->outgoing)
				side.held[block.block] = { gate, gate->token };
			side.open.push_back(gate);
			auto* node = new OpenedNode{ gate, gate->token, opened.load(std::memory_order_relaxed) };
			while (!opened.compare_exchange_weak(node->next, node, std::memory_order_release, std::memory_order_relaxed)) {
			}
			CaptureGate(gate->token, gate->incomingNodes, gate->outgoingNodes);
			counters.opened.fetch_add(1, std::memory_order_relaxed);
			a_build.Reset();
			Publish(side);
		}

		template <class F>
		void OnSwapRetire(std::uintptr_t a_node, std::uint32_t a_bits, F&& a_original)
		{
			auto& b = build;
			if (!b.expected) {
				a_original(a_node, a_bits);
				return;
			}
			b.retiring = !b.incoming.empty();
			a_original(a_node, a_bits);  // FUN_140510a10's detour, directly or through FUN_140510c20's recursion
			b.retiring = false;
			if (--b.expected == 0)
				Emit(b);
		}

		/** @brief The drain's head, under the manager's lock: flipped gates' held blocks released, ages noted; all flushed if DCLF stopped. */
		void DrainHead()
		{
			if (!installed || engineWork.load(std::memory_order_acquire) == 0)
				return;
			if (!Active()) {
				FlushAll(Flush::Stopped);
				return;
			}
			EngineLock lock;
			auto& side = Side();
			ReleaseDeferred(side, false);
			for (auto* gate : std::vector<Gate*>(side.open))
				if ((gate->word.load(std::memory_order_acquire) & 7) == kFlipped)
					ReleaseFlipped(side, *gate);
			SweepOrphans(side);
			FreeRetiredTables(side);
			std::uint32_t openNow = 0, oldestFrames = 0;
			std::int64_t oldestTicks = 0;
			const auto now = Ticks();
			const auto frame = SceneCapture::Frame();
			for (const auto* gate : side.open)
				if ((gate->word.load(std::memory_order_acquire) & 7) == kOpen) {
					++openNow;
					oldestTicks = std::max(oldestTicks, now - gate->openedTicks);
					oldestFrames = std::max(oldestFrames, frame - gate->openedFrame);
				}
			counters.open.store(openNow, std::memory_order_relaxed);
			counters.oldestTicks.store(oldestTicks, std::memory_order_relaxed);
			counters.oldestFrames.store(oldestFrames, std::memory_order_relaxed);
			Publish(side);
		}

		// ---------------------------------------------------------------------------------------------------------------------------
		// The hooks.
		// ---------------------------------------------------------------------------------------------------------------------------
		template <Kind K, std::uint32_t RetireCalls>
		struct Attach
		{
			static void thunk(std::uintptr_t a_block)
			{
				OnAttach(a_block, K, RetireCalls, [](std::uintptr_t a_at) { func(a_at); });
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
		using ObjectUpgradeAttach = Attach<Kind::Object, 1>;
		using TerrainUpgradeAttach = Attach<Kind::Terrain, 1>;
		using ObjectDowngradeAttach = Attach<Kind::Object, 4>;
		using TerrainDowngradeAttach = Attach<Kind::Terrain, 4>;

		// The Func2s' retirement calls: FUN_140510a10(parent, bits) in an upgrade, FUN_140510c20(child, bits) per child in a downgrade.
		template <int Site>
		struct SwapRetire
		{
			static void thunk(std::uintptr_t a_node, std::uint32_t a_bits)
			{
				OnSwapRetire(a_node, a_bits, [](std::uintptr_t a_at, std::uint32_t a_which) { func(a_at, a_which); });
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct LodDrain
		{
			static void thunk(std::uintptr_t a_queue)
			{
				ZoneNamedN(lodDrainZone, "CS.DCLF.Import.LodDrain", ImportTimings::Installed());
				ImportTimings::LodScope scope(ImportTimings::LodEntry::Drain);
				DrainHead();
				func(a_queue);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct LodRetire
		{
			static void thunk(std::uintptr_t a_node, std::uint32_t a_bits)
			{
				ZoneNamedN(lodRetireZone, "CS.DCLF.Import.LodRetire", ImportTimings::Installed());
				ImportTimings::LodScope scope(ImportTimings::LodEntry::Retire);
				Retire(a_node, a_bits, [](std::uintptr_t a_at, std::uint32_t a_which) { func(a_at, a_which); });
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		// The forced paths: everything flushed before the engine's own.
		struct Deactivate
		{
			static void thunk(std::uintptr_t a_manager, std::uint64_t a_unload, std::uint64_t a_bits)
			{
				FlushAll(Flush::Deactivate);
				func(a_manager, a_unload, a_bits);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
		struct MapHideAll
		{
			static void thunk(std::uintptr_t a_node, std::uint64_t a_show)
			{
				FlushAll(Flush::MapHideAll);
				func(a_node, a_show);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
		struct MapEnter
		{
			static void thunk(std::uintptr_t a_node)
			{
				FlushAll(Flush::MapEnter);
				func(a_node);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
		struct MapExit
		{
			static void thunk(std::uintptr_t a_node)
			{
				FlushAll(Flush::MapExit);
				func(a_node);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
		struct TerrainCacheFlush
		{
			static void thunk(std::uint64_t a_all)
			{
				FlushAll(Flush::TerrainCache);
				func(a_all);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
		struct ObjectCacheFlush
		{
			static void thunk(std::uint64_t a_all)
			{
				FlushAll(Flush::ObjectCache);
				func(a_all);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
		// FUN_14050e430's early return (an interior, or the roots hidden): the drain does not run, so the flush is here.
		struct RootsHidden
		{
			static std::uint64_t thunk(std::uint64_t a_arg)
			{
				FlushAll(Flush::RootsHidden);
				return func(a_arg);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
		// FUN_14050f790(manager, root, .., .., 1) at 0x14050e7ae: the full rebuild (the manager's first update, or one asked for). Not
		// the function: the round-robin node update passes 1 for every node near the player (flags bit 0x8000), every update.
		struct FullRebuild
		{
			static void thunk(std::uintptr_t a_manager, std::uintptr_t a_root, std::uintptr_t a_cell, std::uintptr_t a_player, std::uint8_t a_full)
			{
				FlushAll(Flush::Rebuild);
				func(a_manager, a_root, a_cell, a_player, a_full);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		void AbsorbOpened()
		{
			auto* node = opened.exchange(nullptr, std::memory_order_acquire);
			while (node) {
				auto* next = node->next;
				renderGates[node->token] = node->gate;
				delete node;
				node = next;
			}
			if (renderGates.size() < renderPruneMark)
				return;
			// Gates resolved without a flip (forced) leave their entries: dropped when the map doubles.
			for (auto it = renderGates.begin(); it != renderGates.end();)
				it = Live(it->second, it->first) ? std::next(it) : renderGates.erase(it);
			renderPruneMark = std::max<std::size_t>(256, renderGates.size() * 2);
		}
	}

	void InstallDetours()
	{
		if (detoursInstalled)
			return;
		stl::detour_thunk<LodDrain>(Base() + kLodDrain);
		stl::detour_thunk<LodRetire>(Base() + kRetire);
		detoursInstalled = true;
	}

	void Install(bool (*a_running)())
	{
		if (installed)
			return;
		running = a_running;
		if (SwitchValue(Switch::LodGates) == "0") {
			logger::info("[DCLF] LOD gates (T6b5) off (CS_DCLF_LOD_GATES=0): the engine's LOD swaps ungated");
			return;
		}
		for (const auto& site : kCallSites)
			if (std::memcmp(reinterpret_cast<const void*>(Base() + site.offset), site.bytes.data(), site.bytes.size()) != 0) {
				logger::warn("[DCLF] LOD gates (T6b5) not installed: the instruction at {:#x} is not the expected one", 0x140000000 + site.offset);
				return;
			}
		InstallDetours();
		stl::write_thunk_call<ObjectUpgradeAttach>(Base() + kObjectUpgradeAttach);
		stl::write_thunk_call<TerrainUpgradeAttach>(Base() + kTerrainUpgradeAttach);
		stl::write_thunk_call<ObjectDowngradeAttach>(Base() + kObjectDowngradeAttach);
		stl::write_thunk_call<TerrainDowngradeAttach>(Base() + kTerrainDowngradeAttach);
		stl::write_thunk_call<SwapRetire<0>>(Base() + kObjectUpgradeRetire);
		stl::write_thunk_call<SwapRetire<1>>(Base() + kTerrainUpgradeRetire);
		stl::write_thunk_call<SwapRetire<2>>(Base() + kObjectDowngradeRetire);
		stl::write_thunk_call<SwapRetire<3>>(Base() + kTerrainDowngradeRetire);
		stl::write_thunk_jmp<RootsHidden>(Base() + kRootsHiddenReturn);
		stl::write_thunk_call<FullRebuild>(Base() + kFullRebuild);
		stl::detour_thunk<Deactivate>(Base() + kDeactivate);
		stl::detour_thunk<MapHideAll>(Base() + kMapHideAll);
		stl::detour_thunk<MapEnter>(Base() + kMapEnter);
		stl::detour_thunk<MapExit>(Base() + kMapExit);
		stl::detour_thunk<TerrainCacheFlush>(Base() + kTerrainCacheFlush);
		stl::detour_thunk<ObjectCacheFlush>(Base() + kObjectCacheFlush);
		coverageObserver = SwitchEnabled(Switch::SetParity) || SwitchEnabled(Switch::PersistentParity);
		installed = true;
		logger::info("[DCLF] LOD gates (T6b5): {} call sites (the swaps', the early return's, the full rebuild's), the drain, the retirement and 6 forced paths' functions hooked{}", kCallSites.size(),
			coverageObserver ? "; the coverage invariant checked at each release (parity)" : "");
	}

	bool Enabled()
	{
		return installed;
	}

	const std::atomic<std::uint32_t>& OpenGates()
	{
		return openGates;
	}

	bool Vetoes(const void* a_object)
	{
		const auto key = reinterpret_cast<std::uintptr_t>(a_object);
		if (key <= kTomb)
			return false;
		vetoReaders.fetch_add(1, std::memory_order_seq_cst);
		bool vetoed = false;
		if (const auto* table = vetoTable.load(std::memory_order_seq_cst)) {
			const auto mask = table->capacity - 1;
			for (auto slot = Hash(key, table->capacity);; slot = (slot + 1) & mask) {
				const auto& entry = table->entries[slot];
				const auto at = entry.key.load(std::memory_order_acquire);
				if (!at)
					break;
				if (at == key) {
					vetoed = Live(entry.gate, entry.token);
					break;
				}
			}
		}
		vetoReaders.fetch_sub(1, std::memory_order_release);
		if (vetoed)
			counters.vetoes.fetch_add(1, std::memory_order_relaxed);
		return vetoed;
	}

	void NoteEngineHide(const void* a_object, std::uint32_t a_siteOffset)
	{
		const auto key = reinterpret_cast<std::uintptr_t>(a_object);
		if (!coverageObserver || key <= kTomb)
			return;
		vetoReaders.fetch_add(1, std::memory_order_seq_cst);
		if (auto* table = vetoTable.load(std::memory_order_seq_cst)) {
			for (auto slot = Hash(key, table->capacity);; slot = (slot + 1) & (table->capacity - 1)) {
				auto& entry = table->entries[slot];
				const auto at = entry.key.load(std::memory_order_acquire);
				if (!at)
					break;
				if (at == key) {
					if (entry.gate->word.load(std::memory_order_acquire) == Word(entry.token, kFlipped)) {
						entry.rehidSite.store(a_siteOffset, std::memory_order_relaxed);
						counters.engineRehides.fetch_add(1, std::memory_order_relaxed);
					}
					break;
				}
			}
		}
		vetoReaders.fetch_sub(1, std::memory_order_release);
	}

	bool Flip(std::uint64_t a_token)
	{
		if (!installed)
			return false;
		AbsorbOpened();
		const auto it = renderGates.find(a_token);
		if (it == renderGates.end()) {
			counters.flipMisses.fetch_add(1, std::memory_order_relaxed);
			return false;
		}
		auto* gate = it->second;
		renderGates.erase(it);
		// Announced before the exchange: a forced flush that wins its own exchange and then sees this clear knows no flip is storing.
		flipActive.store(1, std::memory_order_seq_cst);
		auto expected = Word(a_token, kOpen);
		if (!gate->word.compare_exchange_strong(expected, Word(a_token, kFlipping), std::memory_order_seq_cst)) {
			flipActive.store(0, std::memory_order_release);
			counters.flipMisses.fetch_add(1, std::memory_order_relaxed);
			return false;
		}
		openGates.fetch_sub(1, std::memory_order_acq_rel);
		for (const auto& block : gate->incoming)
			Unhide(block.nodes);
		for (const auto& block : gate->outgoing)
			if (block.handoff.load(std::memory_order_relaxed) == kOwned)
				for (auto* node : block.nodes)
					if (node && SetHidden(node, true))
						Scene::PushGateHidden(node);
		gate->flippedTicks = Ticks();
		gate->flippedFrame = SceneCapture::Frame();
		const auto ticks = gate->flippedTicks - gate->openedTicks;
		counters.flipTicks.fetch_add(ticks, std::memory_order_relaxed);
		NoteMax(counters.flipTicksMax, ticks);
		counters.flipFrames.fetch_add(gate->flippedFrame - gate->openedFrame, std::memory_order_relaxed);
		gate->word.store(Word(a_token, kFlipped), std::memory_order_release);
		flipActive.store(0, std::memory_order_release);
		PostOutcome({ a_token, false });
		counters.flipped.fetch_add(1, std::memory_order_relaxed);
		return true;
	}

	void DrainOutcomes(void (*a_visit)(void*, const Outcome&), void* a_context)
	{
		OutcomeNode* node = outcomes.exchange(nullptr, std::memory_order_acquire);
		OutcomeNode* oldest = nullptr;
		while (node) {
			auto* next = node->next;
			node->next = oldest;
			oldest = node;
			node = next;
		}
		while (oldest) {
			auto* next = oldest->next;
			a_visit(a_context, oldest->outcome);
			delete oldest;
			oldest = next;
		}
	}

	std::uint64_t ForcedGeneration()
	{
		return forcedGeneration.load(std::memory_order_acquire);
	}

	std::string TakeReport(std::uint32_t a_frames)
	{
		if (!installed)
			return {};
		auto take = [](auto& a_counter) { return a_counter.exchange(0, std::memory_order_relaxed); };
		const auto flipped = take(counters.flipped);
		const auto releases = take(counters.releases);
		const auto flipTicks = take(counters.flipTicks), flipMax = take(counters.flipTicksMax);
		const auto flipFrames = take(counters.flipFrames);
		const auto releaseTicks = take(counters.releaseTicks), releaseMax = take(counters.releaseTicksMax);
		std::string flushes;
		for (std::size_t i = 0; i < counters.flushes.size(); ++i)
			if (const auto count = take(counters.flushes[i]))
				flushes += fmt::format("{}{} {}", flushes.empty() ? "" : ", ", kFlushNames[i], count);
		return fmt::format(
			"[DCLF] LOD gates (T6b5, over {} frames): {} opened, {} flipped ({} flips missed), {} forced ({} superseded; flushes: {}); {} blocks held, {} released "
			"({} reused, {} handed off, {} deferred past a flip), {} orphans shown, {} shows vetoed; {} open, the oldest {} frames {:.1f} ms; request to flip "
			"{:.1f} frames {:.2f} ms average, {:.2f} ms max; flip to release {:.2f} ms average, {:.2f} ms max{}",
			a_frames, take(counters.opened), flipped, take(counters.flipMisses), take(counters.forced), take(counters.superseded), flushes.empty() ? "none" : flushes,
			take(counters.held), take(counters.released), take(counters.reused), take(counters.handedOff), take(counters.deferred), take(counters.orphansShown),
			take(counters.vetoes), counters.open.load(std::memory_order_relaxed), counters.oldestFrames.load(std::memory_order_relaxed),
			TicksToMs(counters.oldestTicks.load(std::memory_order_relaxed)), flipped ? static_cast<double>(flipFrames) / static_cast<double>(flipped) : 0.0,
			flipped ? TicksToMs(flipTicks) / static_cast<double>(flipped) : 0.0, TicksToMs(flipMax), releases ? TicksToMs(releaseTicks) / static_cast<double>(releases) : 0.0,
			TicksToMs(releaseMax),
			coverageObserver ? fmt::format("; coverage: both levels shown {}, neither {}, neither with every incoming hidden again by the engine (LOD under the "
										   "loaded cells) {}; incoming hidden by the engine between flip and release {}",
								   take(counters.coverageBoth), take(counters.coverageNeither), take(counters.coverageEngineHid), take(counters.engineRehides)) :
							   std::string());
	}
}
