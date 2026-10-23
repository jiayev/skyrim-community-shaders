// LodGates: the swap thunks and detours (on the engine's LOD threads, under the terrain manager's lock), the veto table HiddenStores'
// stubs read lock-free, the render thread's Flip, and the outcomes posted to the coordinator.
#include "Features/DrawcallLimitFix/Features/Lod/LodGates.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"

#include "Features/DrawcallLimitFix/Core/Engine/FrameClock.h"
#include "Features/DrawcallLimitFix/Core/Extensions/EngineEvents.h"
#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"

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
#include "Features/DrawcallLimitFix/Diagnostics/Observation.h"

namespace DCLF::Features::Lod::LodGates
{
	namespace
	{
		/**
		 * AE 1.6.1170 and SE 1.5.97, from the image base (skyrim-engine-notes.md, "LOD swaps and their gates"); call sites are checked
		 * before patching. Quadtree node (0x50 bytes): +0x08 terrain handle, +0x10 object handle, +0x18 tree handle, +0x20/+0x28 the map's
		 * fixed-level terrain/object handles, +0x30 children (4 contiguous), +0x40 flags. Handle: +0x0C state (bits 0x70000000) and
		 * references, +0x28 the block. Object block: +0x08 its NiNode (under the LOD root), +0x11 attached. Terrain block: +0x08 land
		 * and +0x10 water node (what show/hide store to), +0x18 the container attached to a level node, +0x21 attached, +0x22 water
		 * registered. The layouts are the same on SE.
		 */
		using Engine::Offset;
		// The Func2s' call sites: attaches (pre-hide) and the retirement that follows (steal, then the gate).
		inline constexpr Offset kObjectUpgradeAttach{ 0x51339a, 0x4b6fc9 };     // BGSQueuedObjectUpgrade::Func2: FUN_1404fda90(child block), once per ready child
		inline constexpr Offset kObjectUpgradeRetire{ 0x51340c, 0x4b701f };     // ...: FUN_140510a10(parent, 2)
		inline constexpr Offset kTerrainUpgradeAttach{ 0x512ceb, 0x4b6972 };    // FUN_140512c90 (terrain upgrade Func2): FUN_140509550(child block)
		inline constexpr Offset kTerrainUpgradeRetire{ 0x512d57, 0x4b69be };    // ...: FUN_140510a10(parent, 1)
		inline constexpr Offset kObjectDowngradeAttach{ 0x513615, 0x4b71d5 };   // BGSQueuedObjectDowngrade::Func2: FUN_1404fda90(parent block)
		inline constexpr Offset kObjectDowngradeRetire{ 0x513671, 0x4b7244 };   // ...: FUN_140510c20(child, 2), once per child
		inline constexpr Offset kTerrainDowngradeAttach{ 0x512f55, 0x4b6b65 };  // BGSQueuedTerrainDowngrade::Func2: FUN_140509550(parent block)
		inline constexpr Offset kTerrainDowngradeRetire{ 0x512fb1, 0x4b6bd1 };  // ...: FUN_140510c20(child, 1), once per child
		// The InitialLoad Func2s (vtables 0x1418690d8 objects, 0x141869030 terrain; queued by FUN_140513ea0 / FUN_1405139b0 for an
		// unloaded node with no loaded block above it): attach only, nothing retired.
		inline constexpr Offset kObjectInitialAttach{ 0x513789, 0x4b739c };   // BGSQueuedObjectInitialLoad::Func2 (0x140513740): FUN_1404fda90(node's block)
		inline constexpr Offset kTerrainInitialAttach{ 0x513091, 0x4b6cd1 };  // BGSQueuedTerrainInitialLoad::Func2 (0x140513080): FUN_140509550(item's block)
		// Forced releases: FUN_14050e430's early return with the LOD roots hidden (tail `jmp` to FUN_140509950) and its full rebuild.
		inline constexpr Offset kRootsHiddenReturn{ 0x50e4e4, 0x4b2a14 };
		// AE: the call of FUN_14050f790(manager, root, .., .., 1). SE inlines that function into its caller: the site is the inlined
		// body's first call, FUN_1404b3f60(root, pos, player cell, 0x27) (AE's FUN_14050fe10), the caches' updates after it (FullRebuildSE).
		inline constexpr Offset kFullRebuild{ 0x50e7ae, 0x4b2ce1 };
		// The call sites' callees.
		inline constexpr Offset kObjectAttach{ 0x4fda90, 0x4a18d0 };       // FUN_1404fda90(block)
		inline constexpr Offset kTerrainAttach{ 0x509550, 0x4ad940 };      // FUN_140509550(block)
		inline constexpr Offset kRetireRecurse{ 0x510c20, 0x4b4c20 };      // FUN_140510c20(node, bits): FUN_140510a10 on the node and its subtree
		inline constexpr Offset kRootsHiddenTail{ 0x509950, 0x4add70 };    // FUN_140509950(0)
		inline constexpr Offset kFullRebuildCallee{ 0x50f790, 0x4b3f60 };  // FUN_14050f790 (SE: FUN_1404b3f60, see kFullRebuild)
		// Detoured.
		inline constexpr Offset kLodDrain{ 0x513840, 0x4b7450 };           // FUN_140513840: the swaps' drain (Func2s), from FUN_14050e430 under the manager's lock
		inline constexpr Offset kRetire{ 0x510a10, 0x4b49b0 };             // FUN_140510a10(node, bits): a node's blocks detached and released (1 terrain, 2 objects)
		inline constexpr Offset kDeactivate{ 0x50ed60, 0x4b3190 };         // FUN_14050ed60: manager deactivated (FUN_14050e250, a worldspace unload, calls it first)
		inline constexpr Offset kMapHideAll{ 0x511b40, 0x4b5a30 };         // FUN_140511b40(node, show): the map's show or hide of every block
		inline constexpr Offset kMapEnter{ 0x511680, 0x4b5560 };           // FUN_140511680: to the map's fixed levels
		inline constexpr Offset kMapExit{ 0x511860, 0x4b5750 };            // FUN_140511860: back from them (also FUN_14050e430's LOD-off branch)
		inline constexpr Offset kTerrainCacheFlush{ 0x5099a0, 0x4addf0 };  // FUN_1405099a0(0): the terrain block cache flushed (LOD off, the map)
		inline constexpr Offset kObjectCacheFlush{ 0x4fdca0, 0x4a1b10 };   // FUN_1404fdca0(0): the object block cache flushed
		// The engine's own retirement, as FUN_140510a10 runs it.
		inline constexpr Offset kDetachObject{ 0x4fd970, 0x4a17c0 };    // FUN_1404fd970(block)
		inline constexpr Offset kDetachTerrain{ 0x509770, 0x4adb60 };   // FUN_140509770(block): unregisters its LOD water too
		inline constexpr Offset kReleaseObject{ 0x501d00, 0x4a5f10 };   // FUN_140501d00(handle): one reference (SE: (cache, handle), Release)
		inline constexpr Offset kReleaseTerrain{ 0x50d170, 0x4b1730 };  // FUN_14050d170(handle) (SE: (cache, handle))
		inline constexpr Offset kLodVisibleTest{ 0x512160, 0x4b60b0 };  // FUN_140512160(node, cell, objects): the update's show test
		// Data.
		inline constexpr Offset kManagerLock{ 0x315b8c8, 0x2f23ed8 };  // DAT_14315b8c8: the terrain manager's recursive BSSpinLock, held around the drain
		inline constexpr Offset kLoadScreen{ 0x315b6a0, 0x2f23cb0 };   // DAT_14315b6a0: load screen up (FUN_1401a1150 sets it; its close runs the LOD full update, then clears it)
		inline constexpr Offset kTlsIndex{ 0x35f0878, 0x3497408 };     // TLS index: +0x768 in its block is the memory context id FUN_140510a10 sets to 0x55
		// The object and terrain block caches (pointers): SE's release is the cache's, (cache + 0xD0, handle) (Release). AE's reads
		// its own (DAT_143146ee0, DAT_143155448); only SE reads these.
		inline constexpr Offset kObjectCache{ 0x3146ee0, 0x2f0f508 };
		inline constexpr Offset kTerrainCache{ 0x3155448, 0x2f1da58 };

		struct CallSite
		{
			Offset offset;
			std::uint8_t opcode;  // 0xE8 call, 0xE9 jmp
			Offset target;
		};
		// Each a rel32 `call` (`jmp` for the early return) to the function named above: the bytes checked are computed from the resolved
		// addresses.
		constexpr std::array<CallSite, 12> kCallSites{ {
			{ kObjectUpgradeAttach, 0xE8, kObjectAttach },
			{ kObjectUpgradeRetire, 0xE8, kRetire },
			{ kTerrainUpgradeAttach, 0xE8, kTerrainAttach },
			{ kTerrainUpgradeRetire, 0xE8, kRetire },
			{ kObjectDowngradeAttach, 0xE8, kObjectAttach },
			{ kObjectDowngradeRetire, 0xE8, kRetireRecurse },
			{ kTerrainDowngradeAttach, 0xE8, kTerrainAttach },
			{ kTerrainDowngradeRetire, 0xE8, kRetireRecurse },
			{ kObjectInitialAttach, 0xE8, kObjectAttach },
			{ kTerrainInitialAttach, 0xE8, kTerrainAttach },
			{ kRootsHiddenReturn, 0xE9, kRootsHiddenTail },
			{ kFullRebuild, 0xE8, kFullRebuildCallee },
		} };
		// The detoured functions and the engine state and calls the gates use: each must be known on the running runtime.
		constexpr std::array<Offset, 18> kFunctions{ { kLodDrain, kRetire, kDeactivate, kMapHideAll, kMapEnter, kMapExit, kTerrainCacheFlush,
			kObjectCacheFlush, kDetachObject, kDetachTerrain, kReleaseObject, kReleaseTerrain, kLodVisibleTest, kManagerLock, kLoadScreen, kTlsIndex,
			kObjectCache, kTerrainCache } };

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
		 * @brief One reference of a block handle released, as FUN_140510a10 does. AE's release takes the handle alone; SE's has no such
		 * form (every caller passes the block cache's +0xD0 with the handle), so it is called that way there.
		 */
		void Release(bool a_object, std::uintptr_t a_handle)
		{
			const Offset release = a_object ? kReleaseObject : kReleaseTerrain;
			if (!REL::Module::IsSE()) {
				EngineCall(release)(a_handle);
				return;
			}
			const auto cache = *reinterpret_cast<const std::uintptr_t*>(Base() + (a_object ? kObjectCache : kTerrainCache)) + 0xD0;
			reinterpret_cast<void (*)(std::uintptr_t, std::uintptr_t)>(Base() + release)(cache, a_handle);
		}

		/**
		 * @brief The terrain manager's recursive lock: every engine-side structure here (open gates, pending and held maps, veto table
		 * writes) is touched under it, like the quadtree. Swaps and the drain already hold it; the forced paths take it. LodGates adds no
		 * lock of its own.
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

		/** @brief FUN_140510a10's memory context (its first act), so the engine's allocation accounting sees our releases as its own. */
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

		// NiAVObject::flags (+0xF4) bit 0, app-cull: what every LOD show/hide stores. Atomic: the engine may write other bits at once.
		// Returns whether the bit changed.
		bool SetHidden(RE::NiAVObject* a_object, bool a_hidden)
		{
			std::atomic_ref<std::uint32_t> flags(At<std::uint32_t>(reinterpret_cast<std::uintptr_t>(a_object), 0xF4));
			const auto before = a_hidden ? flags.fetch_or(1u, std::memory_order_relaxed) : flags.fetch_and(~1u, std::memory_order_relaxed);
			return ((before & 1u) != 0) != a_hidden;
		}

		bool IsHidden(const RE::NiAVObject* a_object) { return Engine::IsHidden(a_object); }

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
		 * @brief A gate's state with its token in one word (`token << 3 | state`), so an exchange on a reused record with a stale token
		 * fails. Open -> Flipping -> Flipped (render thread) -> Released (drain), or Open -> Forced (safety path, or superseded).
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

		/**
		 * @brief A held block's fate while its gate lives. The swap's reattach and the render thread's flip race by seq_cst exchange:
		 * the flip never hides a block the engine just made current again, and a reattach after the hide makes it a later gate's incoming.
		 */
		enum Handoff : std::uint8_t
		{
			kOwned = 0,
			kReattached = 1,  // reattached by a swap before the flip reached it: the flip leaves it shown
			kHandedOff = 2,   // stolen again by a later gate, which owns its visibility
			kFlipHidden = 3,  // hidden by its gate's flip
		};

		struct Block
		{
			std::uintptr_t block = 0;   // the handle's +0x28
			std::uintptr_t handle = 0;  // held: the stolen handle, whose reference DCLF owns; incoming: 0
			std::uintptr_t node = 0;    // held: the quadtree node it was taken from (the reuse check)
			std::uint64_t token = 0;    // held: its gate (0 for a pending block held past a flip)
			Kind kind = Kind::Object;
			std::array<RE::NiAVObject*, 2> nodes{};  // the app-cull nodes
			std::atomic<std::uint8_t> handoff{ kOwned };  // held, while its gate lives: Handoff

			Block() = default;
			Block(const Block& a_other) :
				block(a_other.block), handle(a_other.handle), node(a_other.node), token(a_other.token), kind(a_other.kind), nodes(a_other.nodes), handoff(a_other.handoff.load(std::memory_order_relaxed)) {}
			Block& operator=(const Block& a_other)
			{
				block = a_other.block;
				handle = a_other.handle;
				node = a_other.node;
				token = a_other.token;
				kind = a_other.kind;
				nodes = a_other.nodes;
				handoff.store(a_other.handoff.load(std::memory_order_relaxed), std::memory_order_relaxed);
				return *this;
			}
		};

		/**
		 * @brief One gate. Type-stable (reused, never freed), so a stale reference reads a live word and finds its token gone. The engine
		 * side writes the vectors before publishing (Open) and after its own exchange out of Open; the render thread reads them only
		 * between its exchange to Flipping and its store of Flipped.
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

		// Live, or flipped and unreleased (the veto entry stays for the coverage observer).
		bool Unreleased(const Gate* a_gate, std::uint64_t a_token)
		{
			return Live(a_gate, a_token) || a_gate->word.load(std::memory_order_acquire) == Word(a_token, kFlipped);
		}

		// The veto table: pending incoming nodes, read lock-free by HiddenStores' veto stubs on any thread, written by the engine side
		// alone. A slot is filled once (removal is a tombstone), so a reader's entry never changes; its gate's word decides the veto.
		// Growth and compaction publish a new table; an old one is freed once vetoReaders is seen at 0 after the publication (readers
		// count themselves before loading). No cap.
		struct VetoEntry
		{
			std::atomic<std::uintptr_t> key{ 0 };  // 0 empty, kTomb removed, else the NiAVObject
			const Gate* gate = nullptr;
			std::uint64_t token = 0;
			// NoteEngineHide: offset of the engine store that hid the node again after its gate's flip, else 0.
			std::atomic<std::uint32_t> rehidSite{ 0 };
		};
		constexpr std::uintptr_t kTomb = 1;

		struct VetoTable
		{
			explicit VetoTable(std::size_t a_capacity) :
				capacity(a_capacity), entries(std::make_unique<VetoEntry[]>(a_capacity)) {}
			std::size_t capacity;  // a power of two, at least twice the filled slots (a probe always meets an empty one)
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
		// The feature's registration (NoteRegistered, its install: before any gate opens): the gates' SceneStack channel and their stores'
		// hidden site.
		Api::EngineChannelId gatesChannel;
		std::uint32_t gatesHiddenSite = 0;

		// Counters, since the last report.
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
				handedOff{ 0 }, deferred{ 0 }, vetoes{ 0 }, orphansShown{ 0 }, coverageBoth{ 0 }, coverageNeither{ 0 }, coverageEngineHid{ 0 }, engineRehides{ 0 },
				reattached{ 0 }, reusedHidden{ 0 }, kept{ 0 }, initialOpened{ 0 }, initialLoadScreen{ 0 };
			std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(Flush::Count)> flushes{};
			std::atomic<std::int64_t> flipTicks{ 0 }, flipTicksMax{ 0 }, releaseTicks{ 0 }, releaseTicksMax{ 0 };
			std::atomic<std::uint64_t> flipFrames{ 0 }, releases{ 0 };
			// Set at each drain: open gates and the oldest one's age.
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

		// Outcomes (to the coordinator) and opened gates (to the render thread's lookup): lock-free stacks.
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

		/**
		 * @brief A held block the engine reused: the scene drops its gate's hold on the block's geometries and commits them again. Posted
		 * after the gate's own event, so the coordinator meets it in that batch or later. Idempotent.
		 */
		void PostKept(std::uint64_t a_token, const std::array<RE::NiAVObject*, 2>& a_nodes)
		{
			if (!a_token || !a_nodes[0])
				return;
			PostOutcome({ a_token, false, { a_nodes[0], a_nodes[1] } });
			counters.kept.fetch_add(1, std::memory_order_relaxed);
			Extensions::WakeChannel(gatesChannel);
		}

		struct OpenedNode
		{
			Gate* gate;
			std::uint64_t token;
			OpenedNode* next = nullptr;
		};
		std::atomic<OpenedNode*> opened{ nullptr };

		// Render thread only: token -> gate, from the opened stack (Flip's lookup).
		ankerl::unordered_dense::map<std::uint64_t, Gate*> renderGates;
		std::size_t renderPruneMark = 256;

		bool installed = false;
		bool detoursInstalled = false;
		bool (*running)() = nullptr;  // the gates hold only while DCLF runs
		bool coverageObserver = false;  // parity: the coverage invariant checked at each release

		// The engine side: under the manager's lock, on whichever engine thread holds it.
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
			ankerl::unordered_dense::map<std::uintptr_t, GateRef> held;     // a held block -> the latest gate holding it
			std::vector<Block> deferred;  // held blocks to release once no flip is in progress
			std::unique_ptr<VetoTable> table;  // the published one
			std::vector<std::unique_ptr<VetoTable>> retiredTables;
			std::uint64_t nextToken = 0;
			bool orphans = false;  // a superseded gate left pending blocks: SweepOrphans shows them
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
			// Unpublished before this load (both seq_cst): a reader counting itself later loads the new table.
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
			// One live entry per key: an earlier one is tombstoned on the way to an empty slot.
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
			std::uint32_t expected = 0;  // retirement calls still to come (upgrade 1, downgrade 4: one per child)
			bool retiring = false;       // in the swap's retirement with pending incoming: the retire detour steals
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
			return installed && Extensions::HiddenEventsLive() && (!running || running());
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
					Extensions::PushReservedHidden(node, gatesHiddenSite);
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
		 * @brief A held block's release as FUN_140510a10 would have run it: detached when loaded and attached (terrain when the node's
		 * level allows), then the reference released. Reused (its node's slot or map slot references the handle again: blocks are cached
		 * per node): only DCLF's reference goes and the scene is told (PostKept) unless a later gate holds it pending. During a flip it
		 * waits for a later drain, unless a forced flush already waited the flip out.
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
				if (!a_side.pending.contains(a_block.block)) {
					// Hidden by the flip and current again with no gate pending: a reattach no thunk saw. Visibility stays the engine's.
					if (handoff == kFlipHidden && a_block.nodes[0] && IsHidden(a_block.nodes[0]))
						counters.reusedHidden.fetch_add(1, std::memory_order_relaxed);
					PostKept(a_block.token, a_block.nodes);
				}
			} else if (HandleReady(a_block.handle) && At<std::uintptr_t>(a_block.handle, 0x28) == a_block.block && Attached(a_block.block, a_block.kind)) {
				if (a_block.kind == Kind::Object)
					EngineCall(kDetachObject)(a_block.block);
				else if (TerrainLevelDetaches(a_block.node))
					EngineCall(kDetachTerrain)(a_block.block);
			}
			Release(a_block.kind == Kind::Object, a_block.handle);
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
			PostOutcome({ a_gate.token, true });
			counters.forced.fetch_add(1, std::memory_order_relaxed);
		}

		/** @brief Pending blocks whose gate was resolved without them: shown if still attached, and ungated. */
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
		 * @brief A gate a swap superseded (caller exchanged Open -> Forced) because one of its pending blocks is being retired. Other
		 * pending blocks become orphans. Held blocks: reused ones lose DCLF's reference; inside a gating swap they move to the new gate;
		 * otherwise they are released now.
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
			Extensions::WakeChannel(gatesChannel);
		}

		/** @brief A held block stolen again by a later swap: the earlier gate's copy only drops its reference. */
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
		 * @brief The coverage observer (parity): at a flipped gate's release exactly one level is shown, unless the engine hid the incoming
		 * again. The manager's update hides LOD under loaded cells (terrain: FUN_140510110's param_3, FUN_140512160(node, player cell, 0)
		 * -> FUN_140509b20; objects: FUN_1405103e0's param_3, FUN_140512160(node, cell, 1) -> the store at 0x1405106a4); those re-hides are
		 * marked by NoteEngineHide and counted apart. Any other hidden incoming block is a hole, logged with its store, the engine's test
		 * now, and whether it is attached at that level.
		 */
		void CheckCoverage(EngineSide& a_side, const Gate& a_gate)
		{
			std::uint32_t incoming = 0, incomingShown = 0, engineHid = 0, outgoingShown = 0;
			std::string detail;
			auto* player = RE::PlayerCharacter::GetSingleton();
			// Exterior only: FUN_140512160 reads the cell's worldspace (null for an interior).
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
			// The outgoing the flip owned (not reattached or handed off).
			for (const auto& block : a_gate.outgoing)
				if (const auto handoff = block.handoff.load(std::memory_order_relaxed); (handoff == kFlipHidden || handoff == kOwned) && block.nodes[0] &&
																						  Attached(block.block, block.kind))
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
		 * @brief Resolves every gate now (unload, map, LOD off, rebuild, DCLF stopped): open ones forced (pending shown, held released),
		 * flipped ones released. A flip past its exchange finishes first: LodGates' one wait, bounded by its few stores.
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
				Extensions::WakeChannel(gatesChannel);
		}

		// The swap: attaches (pre-hide), the retirement (steal), the gate (Emit).

		/**
		 * @brief An incoming block's attach in a Func2: pre-hidden unless already attached (a reused, shown block must not go dark), the
		 * hide undone if the attach did nothing (FUN_140509550 attaches only a loaded terrain block). No hidden event: the attach's capture
		 * reads the bit. An attached block a gate holds is taken from the flip by exchange: still shown -> reused (PostKept); already
		 * flip-hidden -> incoming again.
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
			if (word != Word(token, kOpen) && word != Word(token, kFlipping) && word != Word(token, kFlipped))
				return;
			for (auto& block : gate->outgoing) {
				if (block.block != a_block)
					continue;
				auto handoff = static_cast<std::uint8_t>(kOwned);
				if (block.handoff.compare_exchange_strong(handoff, kReattached, std::memory_order_seq_cst)) {
					counters.reattached.fetch_add(1, std::memory_order_relaxed);
					PostKept(token, block.nodes);
				} else if (handoff == kFlipHidden)
					b.incoming.push_back(incoming);
				break;
			}
		}

		/**
		 * @brief Around FUN_140510a10: inside a gated swap, steals a loaded, attached block (the engine skips detach and release; DCLF owns
		 * the reference); retiring a pending block supersedes its gate (the engine retires it, never shown). A pending block whose gate is
		 * flipping right now is held until a later drain. Trees (4) and the map's handles (0x10) pass through.
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
		 * @brief The swap's end (last retirement call): orphans shown, then a gate opened if any incoming block is pending, published to the
		 * render thread's lookup and captured as the tracker's Gate event after the attaches.
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
			for (auto& block : gate->outgoing) {
				block.token = gate->token;
				for (auto* node : block.nodes)
					if (node)
						gate->outgoingNodes.push_back(node);
			}
			gate->openedTicks = Ticks();
			gate->openedFrame = FrameClock::Frame();
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

		/** @brief The drain's head, under the manager's lock: releases flipped gates' held blocks, notes ages; flushes if DCLF stopped. */
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
			const auto frame = FrameClock::Frame();
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

		// The hooks.
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

		/**
		 * @brief The InitialLoad Func2s' attach (an area entering LOD range): nothing retired, so the gate is emitted right after. Under a
		 * load screen (DAT_14315b6a0; the queuers retire the node's subtree and ancestors first) it passes through: the load screen ends
		 * in a full rebuild, which flushes every gate.
		 */
		template <Kind K>
		struct InitialAttach
		{
			static void thunk(std::uintptr_t a_block)
			{
				if (!Active() || *reinterpret_cast<const volatile std::uint8_t*>(Base() + kLoadScreen) != 0) {
					if (Active())
						counters.initialLoadScreen.fetch_add(1, std::memory_order_relaxed);
					func(a_block);
					return;
				}
				// No retirement call follows (expected 0): the build is this attach alone.
				OnAttach(a_block, K, 0, [](std::uintptr_t a_at) { func(a_at); });
				auto& b = build;
				if (b.expected || b.incoming.empty())
					return;
				counters.initialOpened.fetch_add(1, std::memory_order_relaxed);
				Emit(b);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
		using ObjectInitialAttach = InitialAttach<Kind::Object>;
		using TerrainInitialAttach = InitialAttach<Kind::Terrain>;

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
				DrainHead();
				func(a_queue);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct LodRetire
		{
			static void thunk(std::uintptr_t a_node, std::uint32_t a_bits)
			{
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
		// The full rebuild call at 0x14050e7ae only: the round-robin node update also passes 1 to FUN_14050f790 for nodes near the player.
		struct FullRebuild
		{
			static void thunk(std::uintptr_t a_manager, std::uintptr_t a_root, std::uintptr_t a_cell, std::uintptr_t a_player, std::uint8_t a_full)
			{
				FlushAll(Flush::Rebuild);
				func(a_manager, a_root, a_cell, a_player, a_full);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
		// SE: FUN_14050f790 is inlined at the full rebuild; its first call (FUN_1404b3f60(root, pos, player cell, 0x27), AE's
		// FUN_14050fe10) is the site, so the flush still comes before the rebuild's work, as FullRebuild's does on AE.
		struct FullRebuildSE
		{
			static void thunk(std::uintptr_t a_root, std::uintptr_t a_position, std::uintptr_t a_player, std::uint32_t a_flags)
			{
				FlushAll(Flush::Rebuild);
				func(a_root, a_position, a_player, a_flags);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		// The instruction at a_site is the rel32 call or jmp (a_opcode) to a_target.
		bool BranchesTo(std::uintptr_t a_site, std::uint8_t a_opcode, std::uintptr_t a_target)
		{
			const auto* bytes = reinterpret_cast<const std::uint8_t*>(a_site);
			std::int32_t displacement = 0;
			std::memcpy(&displacement, bytes + 1, sizeof(displacement));
			return bytes[0] == a_opcode && a_site + 5 + static_cast<std::intptr_t>(displacement) == a_target;
		}

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
		if (detoursInstalled || !kLodDrain.Known() || !kRetire.Known())
			return;
		stl::detour_thunk<LodDrain>(Base() + kLodDrain);
		stl::detour_thunk<LodRetire>(Base() + kRetire);
		detoursInstalled = true;
	}

	void Install(Api::SwitchId a_switch, bool (*a_running)())
	{
		if (installed)
			return;
		running = a_running;
		if (SwitchValue(a_switch) == "0") {
			logger::info("[DCLF] LOD gates (T6b5) off (CS_DCLF_LOD_GATES=0): the engine's LOD swaps ungated");
			return;
		}
		for (const auto& function : kFunctions)
			if (!function.Known()) {
				logger::warn("[DCLF] LOD gates (T6b5) not installed: an engine address is unknown on this runtime");
				return;
			}
		for (const auto& site : kCallSites)
			if (!site.offset.Known() || !site.target.Known() || !BranchesTo(Base() + site.offset, site.opcode, Base() + site.target)) {
				logger::warn("[DCLF] LOD gates (T6b5) not installed: the instruction at {:#x} is not the expected one", 0x140000000 + site.offset.Get());
				return;
			}
		InstallDetours();
		stl::write_thunk_call<ObjectUpgradeAttach>(Base() + kObjectUpgradeAttach);
		stl::write_thunk_call<TerrainUpgradeAttach>(Base() + kTerrainUpgradeAttach);
		stl::write_thunk_call<ObjectDowngradeAttach>(Base() + kObjectDowngradeAttach);
		stl::write_thunk_call<TerrainDowngradeAttach>(Base() + kTerrainDowngradeAttach);
		stl::write_thunk_call<ObjectInitialAttach>(Base() + kObjectInitialAttach);
		stl::write_thunk_call<TerrainInitialAttach>(Base() + kTerrainInitialAttach);
		stl::write_thunk_call<SwapRetire<0>>(Base() + kObjectUpgradeRetire);
		stl::write_thunk_call<SwapRetire<1>>(Base() + kTerrainUpgradeRetire);
		stl::write_thunk_call<SwapRetire<2>>(Base() + kObjectDowngradeRetire);
		stl::write_thunk_call<SwapRetire<3>>(Base() + kTerrainDowngradeRetire);
		stl::write_thunk_jmp<RootsHidden>(Base() + kRootsHiddenReturn);
		if (REL::Module::IsSE())
			stl::write_thunk_call<FullRebuildSE>(Base() + kFullRebuild);
		else
			stl::write_thunk_call<FullRebuild>(Base() + kFullRebuild);
		stl::detour_thunk<Deactivate>(Base() + kDeactivate);
		stl::detour_thunk<MapHideAll>(Base() + kMapHideAll);
		stl::detour_thunk<MapEnter>(Base() + kMapEnter);
		stl::detour_thunk<MapExit>(Base() + kMapExit);
		stl::detour_thunk<TerrainCacheFlush>(Base() + kTerrainCacheFlush);
		stl::detour_thunk<ObjectCacheFlush>(Base() + kObjectCacheFlush);
		coverageObserver = Diagnostics::Observe().claimsOrScene;
		installed = true;
		logger::info("[DCLF] LOD gates (T6b5): {} call sites (the swaps', the initial loads', the early return's, the full rebuild's), the drain, the retirement and 6 forced paths' functions hooked{}", kCallSites.size(),
			coverageObserver ? "; the coverage invariant checked at each release (parity)" : "");
	}

	bool Enabled()
	{
		return installed;
	}

	void CaptureGate(std::uint64_t a_token, std::span<RE::NiAVObject* const> a_incoming, std::span<RE::NiAVObject* const> a_outgoing)
	{
		// The scene side of the gate: an event on the swapping thread's stack, ordered after the attaches it follows; the coordinator
		// opens the gate when it applies it (the gates' component's stacked hook).
		Extensions::PushStacked(gatesChannel, a_token, a_incoming, a_outgoing);
	}

	void NoteRegistered(Api::EngineChannelId a_channel, std::uint32_t a_hiddenSite)
	{
		gatesChannel = a_channel;
		gatesHiddenSite = a_hiddenSite;
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
		// Before the exchange: a forced flush that wins its exchange and then sees this clear knows no flip is storing.
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
		// Each owned outgoing block is claimed by exchange before its hide (a reattached or handed-off one is left alone).
		for (auto& block : gate->outgoing) {
			auto handoff = static_cast<std::uint8_t>(kOwned);
			if (!block.handoff.compare_exchange_strong(handoff, kFlipHidden, std::memory_order_seq_cst))
				continue;
			for (auto* node : block.nodes)
				if (node && SetHidden(node, true))
					Extensions::PushReservedHidden(node, gatesHiddenSite);
		}
		gate->flippedTicks = Ticks();
		gate->flippedFrame = FrameClock::Frame();
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

	Totals Take()
	{
		Totals t;
		t.installed = installed;
		if (!installed)
			return t;
		auto take = [](auto& a_counter) { return a_counter.exchange(0, std::memory_order_relaxed); };
		t.flipped = take(counters.flipped);
		t.releases = take(counters.releases);
		t.flipTicks = take(counters.flipTicks), t.flipTicksMax = take(counters.flipTicksMax);
		t.flipFrames = take(counters.flipFrames);
		t.releaseTicks = take(counters.releaseTicks), t.releaseTicksMax = take(counters.releaseTicksMax);
		for (std::size_t i = 0; i < counters.flushes.size(); ++i)
			t.flushes[i] = take(counters.flushes[i]);
		t.opened = take(counters.opened), t.initialOpened = take(counters.initialOpened), t.initialLoadScreen = take(counters.initialLoadScreen);
		t.flipMisses = take(counters.flipMisses), t.forced = take(counters.forced), t.superseded = take(counters.superseded);
		t.held = take(counters.held), t.released = take(counters.released), t.reused = take(counters.reused), t.handedOff = take(counters.handedOff);
		t.deferred = take(counters.deferred), t.reattached = take(counters.reattached), t.kept = take(counters.kept), t.reusedHidden = take(counters.reusedHidden);
		t.orphansShown = take(counters.orphansShown), t.vetoes = take(counters.vetoes);
		t.open = counters.open.load(std::memory_order_relaxed), t.oldestFrames = counters.oldestFrames.load(std::memory_order_relaxed);
		t.oldestTicks = counters.oldestTicks.load(std::memory_order_relaxed);
		t.coverage = coverageObserver;
		if (coverageObserver) {
			t.coverageBoth = take(counters.coverageBoth), t.coverageNeither = take(counters.coverageNeither);
			t.coverageEngineHid = take(counters.coverageEngineHid), t.engineRehides = take(counters.engineRehides);
		}
		return t;
	}

	const char* FlushName(std::size_t a_index)
	{
		return kFlushNames[a_index];
	}

	double TicksToMilliseconds(std::int64_t a_ticks)
	{
		return TicksToMs(a_ticks);
	}
}
