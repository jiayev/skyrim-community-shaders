#pragma once

// The coordinator component contract's Core side (Api/CoordinatorComponents.h): what a component's hooks are handed - the live tables,
// its state, and the few coordinator services a residency change or a walk's phase needs, never the coordinator itself; the walk's
// scheduling and entry writes; whether an entry class is taken. Implemented by the coordinator (Scene/Coordinator/Components.cpp), which
// calls the hooks, and the registry (EntryClassTaken).

#include "Features/DrawcallLimitFix/Api/CoordinatorComponents.h"

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace DCLF
{
	struct SceneTables;
	struct FadeSeedItem;
	class SceneMirror;
	namespace Scene
	{
		class Coordinator;
	}
}

namespace DCLF::Api
{
	/** @brief What a component's hook is handed (scene lane, inside a pass): the live tables, its state, and the coordinator's services. */
	class CoordinatorComponentContext
	{
	public:
		CoordinatorComponentContext(SceneTables& a_tables, const std::shared_ptr<void>& a_state, Scene::Coordinator& a_coordinator, std::uint32_t a_component) :
			tables(a_tables), state(a_state), coordinator(a_coordinator), component(a_component) {}

		SceneTables& tables;  // the coordinator's live tables (its table families: Extensions::TableFamilies::Mutable)
		const std::shared_ptr<void>& state;

		template <class T>
		T& State() const
		{
			return *static_cast<T*>(state.get());
		}

		/** @brief Whether the walk parity's dense walk runs: it restores the tables, not what the components keep beside them. */
		bool DenseWalk() const;
		/**
		 * @brief The fade node of a_slot's property as the mirror holds it (null: no geometry, no property record or none), and the node's
		 * capture kind (SceneCapture::kKind*; 0 when the mirror holds no record of it). Reads no engine memory.
		 */
		const void* MirroredFadeNode(std::uint32_t a_slot, std::uint32_t& a_kind) const;
		/**
		 * @brief A reference to a_node from a_slot's entry (its fade node, held from the naming event's pins), for a newly listed owner;
		 * null when the entry holds another (asked of the render thread: the component is called again with the answer, resident).
		 */
		RE::NiPointer<RE::NiAVObject> HoldFadeNode(std::uint32_t a_slot, const void* a_node) const;
		/** @brief a_slot is recounted at the next KeepResidentsAlive (its keys may have changed). */
		void NoteTouched(std::uint32_t a_slot) const;
		/** @brief An engine reference let go of: released at Present through the retirement chain. */
		void HandBack(RE::NiPointer<RE::NiAVObject>&& a_reference) const;
		void HandBack(std::vector<RE::NiPointer<RE::NiAVObject>>& a_references) const;
		void HandBack(RE::NiPointer<RE::NiRefObject>&& a_reference) const;
		/** @brief A tracked entry's write state (none: a_geometry is not tracked). */
		struct TrackedWrite
		{
			bool tracked = false;
			std::uint32_t slot = ~0u;  // its object slot (kNoObjectSlot: none)
			bool streamWaiting = false;  // its last record write waited for its second vertex stream (EntryWrite)
		};
		TrackedWrite TrackedWriteOf(const RE::BSGeometry* a_geometry) const;
		/** @brief a_geometry's tracked entry written in full this walk (Coordinator::Schedule); false when it is not tracked. */
		bool ScheduleWrite(RE::BSGeometry* a_geometry) const;
		/** @brief The tracked entry's own reference of the geometry keyed a_key (held from its attach's pins), or null when not tracked. */
		RE::BSGeometry* TrackedGeometry(const void* a_key) const;
		/**
		 * @brief A range of the component's (a_count units from a_first) freed: retiring until no publication names it, then handed back
		 * (CoordinatorComponent::recycled).
		 */
		void RetireRange(std::uint32_t a_first, std::uint32_t a_count) const;
		/** @brief A seed the next frame's start takes (PublicationDeltas, then FrameView::TakeComponentSeeds with the component's id). */
		void RequestSeed(FadeSeedItem&& a_seed) const;
		/** @brief Whether KeepResidentsAlive counted a_slot (a resident with a pipeline: what the residents keep alive). */
		bool Counted(std::uint32_t a_slot) const;
		/** @brief The coordinator's scene mirror, as the pass's events left it (the parent chains, the nodes' kinds; no engine memory). */
		const SceneMirror& Mirror() const;
		/** @brief The coordinator's pass serial (the observers' ParityDue). */
		std::uint64_t PassSerial() const;

	private:
		Scene::Coordinator& coordinator;  // the services above, not the component's
		std::uint32_t component;          // its registration
	};

	/**
	 * @brief An accumulated patch's values a component lists by (ApplyAccumulatePatch): the draw chain the Lighting port derived for an
	 * object drawn in a kept order (a decal's group, pass and sub-pass: AccumulatePatch::decalKey; 0 for none).
	 */
	class AccumulatedPatch
	{
	public:
		std::uint64_t chain;
	};

	/**
	 * @brief A walk's rewrite of a slot (Coordinator::WriteObject, WriteLayer, or a skin's refresh in place): whether its scene half was
	 * rewritten (it may have moved in the scene graph), and its skin partitions before and now (equal when they did not change).
	 */
	class SlotRewrite
	{
	public:
		bool written;
		std::uint32_t partitionsBefore, partitionsNow;
	};

	/**
	 * @brief A recount (KeepResidentsAlive): rebuilt, every resident is counted again from nothing; otherwise the slots touched since the
	 * last recount (sorted, each once) are uncounted, all of them first, then counted as they are now.
	 */
	class ResidentRecount
	{
	public:
		bool rebuild;
		std::span<const std::uint32_t> touched;    // without rebuild
		std::span<const std::uint32_t> residents;  // with rebuild, in the residents' order
	};

	/**
	 * @brief The delta walk's scheduling (DeltaWalk): whether it is a full evaluation, and what the component's events did, counted into
	 * the walk's statistics (the scene delta line).
	 */
	class WalkScheduling
	{
	public:
		bool full;
		std::uint64_t events = 0;   // the component's events this walk
		std::uint64_t updated = 0;  // the slots it updated in place
		std::uint64_t written = 0;  // the writes it scheduled (ScheduleWrite)
	};

	/**
	 * @brief A walk's write of an entry of the class a component follows (Coordinator::MergeWrite): the entry, and the second vertex stream
	 * resolve gives its record (held on the merge's stack: what it leases lives as long as the write).
	 */
	class EntryWrite
	{
	public:
		RE::BSGeometry& geometry;
		RE::NiNode* source = nullptr;  // the entry's held source node (a face shape's BSFaceGenNiNode, from its attach's pins), or null
		// The stream: float4 positions, published by generation, leased by owner, and its region of the scene's positions buffer.
		const float* positions = nullptr;
		std::uint32_t vertexCount = 0;
		std::uint64_t generation = 0;
		std::shared_ptr<const std::vector<float>> owner;
		std::uint32_t region = ~0u;  // kNoFaceRegion: none
	};
}

namespace DCLF::Extensions
{
	/**
	 * @brief Any thread, after InstallFeatures (false before): whether a_class's objects are DCLF's (the component that follows it takes
	 * it: Api::CoordinatorComponent::takes). Read as data by the eligibility, the walk and the frame's addresses.
	 */
	bool EntryClassTaken(Api::EntryClass a_class);
}
