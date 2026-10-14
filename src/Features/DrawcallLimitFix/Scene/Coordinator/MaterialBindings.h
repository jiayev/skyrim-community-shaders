#pragma once

// The coordinator's material texture bindings: the requests in flight, the answers kept and the slots holding a previous view. Scene
// lane only, except the replies queue (the import thread answers into it).

#include <cstdint>
#include <memory>
#include <vector>

#include <ankerl/unordered_dense.h>

#include "Features/DrawcallLimitFix/Runtime/KeptState.h"
#include "Features/DrawcallLimitFix/Runtime/SceneWake.h"
#include "Features/DrawcallLimitFix/Draws/Gpu/GpuTextures.h"
#include "Features/DrawcallLimitFix/Scene/Model/Lookups.h"

struct ID3D11ShaderResourceView;

namespace DCLF
{
	/**
	 * @brief The material slots' texture bindings, made by the scene work (Coordinator::UpdateMaterialBindings).
	 *
	 * Where the scene work makes or changes a material slot's record (the tables' material log), or a slot becomes used, it asks
	 * GpuTextures for the bindings of the record's views (GpuTextures::Request) and writes the slot's entry: the descriptor
	 * indices, the owners, the binding block. Every answer is an event in `replies`, drained by the next scene work pass, which
	 * writes the entries of the slots waiting on it. A view the scene work has had answered before is served from `cache` (the
	 * owner held weakly: an import nobody holds goes), so a record whose views are known resolves in the pass that makes it.
	 *
	 * The entries are the scene lane's lookups' (Lookups::materials, written straight in); the slots changed since the last commit or
	 * publication take their versions and their log entries before it (Coordinator::VersionMaterialBindings). This holds what is not
	 * published: the requests, the answers kept, the slots holding a previous view.
	 */
	struct MaterialBindings
	{
		struct Cached
		{
			std::weak_ptr<const void> owner;
			std::uint32_t index = Lookups::kNone;
		};
		struct Stats
		{
			std::uint64_t requested = 0;  // views asked of the import thread
			std::uint64_t answered = 0;   // their answers drained
			std::uint64_t rejected = 0;   // of which the view was rejected
			std::uint64_t stale = 0;      // answers to a request made before a reset
			std::uint64_t cached = 0;     // views served from the cache
			std::uint64_t resolved = 0;   // entries that turned resolved
			std::uint64_t retired = 0;    // entries dropped with their slot
			std::uint64_t versioned = 0;  // entry changes versioned into the lookups (VersionMaterialBindings)
		};
		/** @brief The work the bindings saved, for the material records' line (Coordinator::RefreshMaterialRecords, which resets it). */
		struct Work
		{
			std::uint64_t sealed = 0;          // owner blocks sealed (GpuTextures::Seal)
			std::uint64_t sealShared = 0;      // owner blocks shared with another slot of the material instead
			std::uint64_t waitersDeduped = 0;  // a slot already waiting on a view, not listed again
		};

		// Parallel to Tables::materials: a slot whose view changed and whose new one is still asked for: it keeps the previous view's
		// binding meanwhile.
		std::vector<std::uint8_t> held;
		// The slots whose entry changed since they were last versioned (VersionMaterialBindings), once each.
		std::vector<std::uint32_t> changed;
		std::vector<std::uint8_t> changedMark;
		// The views asked for and not answered yet, with the slots waiting on each.
		ankerl::unordered_dense::map<ID3D11ShaderResourceView*, std::vector<std::uint32_t>> inFlight;
		// (view, slot) for every slot listed in inFlight: a slot resolved again while it waits is listed once.
		ankerl::unordered_dense::set<std::pair<ID3D11ShaderResourceView*, std::uint32_t>> waiting;
		// Each material's newest owner block (weak: the entries hold it): a slot of the material with the same owners takes it instead of
		// sealing its own (a material's passes name the same textures). Swept as it grows.
		ankerl::unordered_dense::map<const void*, std::weak_ptr<const Lookups::MaterialOwners>> sealed;
		std::size_t sealedSweep = 0;
		ankerl::unordered_dense::map<ID3D11ShaderResourceView*, Cached> cache;
		std::size_t cacheSweep = 0;
		// The answers of the drain being applied (rejections included), what the waiting slots take.
		ankerl::unordered_dense::map<ID3D11ShaderResourceView*, GpuTextures::Binding> arrived;
		// The used set as the last pass saw it: a slot set since is resolved (a member joined on a slot made earlier).
		std::vector<std::uint64_t> usedSeen;
		LogCursor cursor;  // the tables' material log
		std::uint64_t cookie = 1;  // the requests' since the last reset: an answer with another is stale
		GpuTextures::Replies replies{ &WakeSceneTextureReply };  // an answer wakes the scene pump
		Stats stats;
		Work work;

		void MarkChanged(std::uint32_t a_slot)
		{
			if (changedMark.size() <= a_slot)
				changedMark.resize(std::size_t(a_slot) + 1, 0);
			if (!changedMark[a_slot]) {
				changedMark[a_slot] = 1;
				changed.push_back(a_slot);
			}
		}
	};
}
