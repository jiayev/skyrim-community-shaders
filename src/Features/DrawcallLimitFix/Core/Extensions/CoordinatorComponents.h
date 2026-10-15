#pragma once

// The coordinator component contract's Core side (Api/CoordinatorComponents.h): what a component's hooks are handed - the live tables,
// its state, and the few coordinator services a residency change or a walk's phase needs, never the coordinator itself; the walk's
// scheduling and entry writes; whether an entry class is taken. Implemented by the coordinator (Scene/Coordinator/Components.cpp), which
// calls the hooks, and the registry (EntryClassTaken).

#include "Features/DrawcallLimitFix/Api/CoordinatorComponents.h"
#include "Features/DrawcallLimitFix/Api/MainRanges.h"
#include "Features/DrawcallLimitFix/Core/Engine/LodSegments.h"

#include <ankerl/unordered_dense.h>

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
		 * capture kind (Api::kMirroredKind*; 0 when the mirror holds no record of it). Reads no engine memory.
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
		 * (CoordinatorComponent::Retirement::recycled).
		 */
		void RetireRange(std::uint32_t a_first, std::uint32_t a_count) const;
		/** @brief A seed the next frame's start takes (PublicationDeltas, then FrameView::TakeComponentSeeds with the component's id). */
		void RequestSeed(FadeSeedItem&& a_seed) const;
		/** @brief Whether KeepResidentsAlive counted a_slot (a resident with a pipeline: what the residents keep alive). */
		bool Counted(std::uint32_t a_slot) const;
		/** @brief The coordinator's scene mirror, as the pass's events left it (the parent chains, the nodes' kinds; no engine memory). */
		const SceneMirror& Mirror() const;
		/** @brief a_key's node as the mirror holds it (Api::MirroredNode: a parent chain's walk, the node's kind, RTTI and lists). */
		MirroredNode MirroredNodeOf(const void* a_key) const;

		// The object slots' columns a component reads, and the fixed main ranges' it writes (Api/MainRanges.h; the slots' ordinals in
		// their range: Features/Decals' order), the live tables'. Read per event, never per object per frame.
		/** @brief How many object slots the tables hold. */
		std::uint32_t ObjectSlots() const;
		/** @brief a_slot's geometry (null past the slots, or none). */
		const RE::BSGeometry* ObjectGeometry(std::uint32_t a_slot) const;
		/** @brief The slots a_slot takes in its fixed range, by its skin partitions (RangeSpanOf; one past the slots). */
		std::uint32_t RangeSpan(std::uint32_t a_slot) const;
		/** @brief The slots a range member of a_partitions (a skin partitions value: Api::SlotRewrite's) takes: one per partition up to the last. */
		static std::uint32_t RangeSpanOf(std::uint32_t a_partitions);
		/** @brief Each object slot's ordinal in its fixed range (~0u: none), sized to the object slots first (a new slot's none). */
		std::span<std::uint32_t> SizedRangeOrdinals() const;
		/** @brief Each fixed range's slots. */
		std::span<std::uint32_t, kFixedMainRanges> RangeCounts() const;
		/** @brief a_slot's ordinal moved, or it left its range: its draw's inputs changed (noted with the bindings' cause). */
		void NoteRangeOrdinalChanged(std::uint32_t a_slot) const;
		/** @brief The coordinator's pass serial (the observers' ParityDue). */
		std::uint64_t PassSerial() const;
		/** @brief The pass's stamp (Coordinator::PassStamp: never 0), for a check run once a pass. */
		std::uint32_t PassStamp() const;

		// The drawn index ranges the coordinator keeps by tracked entry (object LOD's sub-index shapes: the attach's capture, then their
		// events), which the walk reads as data (each range a geometry slot of the entry's chain); erased with the entry.
		using EntryRangesMap = ankerl::unordered_dense::map<const RE::BSGeometry*, std::vector<LodSegments::Range>>;
		/** @brief Every entry's kept ranges. */
		const EntryRangesMap& EntryRanges() const;
		/** @brief a_entry's kept ranges; null when it keeps none (not a tracked entry with ranges: only such a key is known alive). */
		const std::vector<LodSegments::Range>* EntryRangesOf(const void* a_entry) const;
		/** @brief The ranges the mirror's record of a_entry holds (SceneCapture::GeometryRecord::segments; null: no record). */
		const std::vector<LodSegments::Range>* MirroredRanges(const void* a_entry) const;
		/** @brief a_entry's kept ranges replaced; a_rewrite: its record (its range slot chain) is written again this walk. */
		void SetEntryRanges(const void* a_entry, std::vector<LodSegments::Range>&& a_ranges, bool a_rewrite) const;

		// The marks on scene roots (Api::Mark), the coordinator's map read as data by the prepares (written only while the mirror's events
		// are applied, before the adds).
		/**
		 * @brief a_key marked the component's (a_token, a_side); a_ownHide: the mirror counts the subtree's hide as the component's (its
		 * entries' verdicts do not take it). Returns the mark it displaced (none: no other).
		 */
		Mark MarkRoot(const void* a_key, std::uint64_t a_token, std::uint8_t a_side, bool a_ownHide) const;
		/** @brief a_key's mark erased when it is the component's with a_token; whether it was. */
		bool UnmarkRoot(const void* a_key, std::uint64_t a_token) const;
		/** @brief a_key's mark (none: unmarked). */
		Mark MarkOf(const void* a_key) const;

		// The commit's vetoes (Api::kVetoHold, kVetoObserve): one byte per entry, read by the commit as data.
		/** @brief a_entry's veto set (0: none); Observe resets its observed slots. Set during settle, it applies in the same commit. */
		void SetEntryVeto(const void* a_entry, std::uint8_t a_veto) const;
		/** @brief What the commit recorded of a_slot while Observe withheld it (none past the slots). */
		ObservedState ObservedSlot(std::uint32_t a_slot) const;
		/** @brief Whether a_slot is a layer's (the tables' IsLayer): a publication claims it with its base geometry. */
		bool LayerSlot(std::uint32_t a_slot) const;
		/** @brief How many entries hold a_veto (Api::kVetoHold or kVetoObserve; every component's), as the coordinator counts them. */
		std::uint32_t VetoCount(std::uint8_t a_veto) const;
		/** @brief Whether the component needs the commit live (settle called with a_live) whatever the vetoes. */
		void SetCommitLive(bool a_live) const;
		/** @brief a_entry requeued (Api::kRequeueSlots, kRequeueReevaluate, kRequeueReevaluateAlways); nothing when it is not tracked. */
		void Requeue(const void* a_entry, std::uint8_t a_flags) const;
		/** @brief a_entry's tracked state (Api::TrackedState; tracked false: not tracked). */
		TrackedState TrackedStateOf(const void* a_entry) const;
		/** @brief A delta the next publication carries for the component (forces one), handed to its adoption (AdoptionContext::deltas). */
		void PublishDelta(std::shared_ptr<const void> a_delta) const;
		/** @brief Whether a_slot's structure changed since the last publication's revocation (its claim is taken back by the next one). */
		bool RestructuredSincePublication(std::uint32_t a_slot) const;
		/**
		 * @brief The tracked entries under a_root as the mirror holds the subtree (the tracking walk's), into a_out (appended); a_lacking set
		 * when the mirror lacks a record of the root or a node under it.
		 */
		void SubtreeEntries(const void* a_root, std::vector<const void*>& a_out, bool& a_lacking) const;

	private:
		Scene::Coordinator& coordinator;  // the services above, not the component's
		std::uint32_t component;          // its registration
	};

	/**
	 * @brief An accumulated patch's values a component lists by (ApplyAccumulatePatch; CoordinatorComponent::Accumulate::patched): the
	 * draw chain the Lighting port derived for an object drawn in a kept order (a decal's group, pass and sub-pass:
	 * AccumulatePatch::decalKey; 0 for none).
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
	 * @brief The delta walk's scheduling (DeltaWalk): whether it is a full evaluation, and what the component's events did, the component's
	 * own counts (for its report lines).
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
		std::uint32_t region = ~0u;  // Api::VertexStreamRow::kNoFirst: none
	};
}

namespace DCLF::Extensions
{
	/**
	 * @brief Any thread, after InstallFeatures (false before): whether a_class's objects are DCLF's (the component that follows it takes
	 * it: Api::CoordinatorComponent::EntryClassHooks::takes). Read as data by the eligibility, the walk and the frame's addresses.
	 */
	bool EntryClassTaken(Api::EntryClass a_class);
	/**
	 * @brief Render thread, the frame's start (after its claims are installed or withdrawn): every component's adoption hook
	 * (Api::CoordinatorComponent::Adoption), in registration order, handed a_frame, a_claims (null: withdrawn) and its deltas
	 * (a_deltas by component: the publications installed since the last call; none past it). Scene/Coordinator/Components.cpp.
	 */
	void ComponentsAdopt(std::uint32_t a_frame, const void* a_claims, std::span<const std::vector<std::shared_ptr<const void>>> a_deltas);
}
