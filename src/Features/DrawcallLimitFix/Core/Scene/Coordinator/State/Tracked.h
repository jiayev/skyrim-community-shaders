#pragma once

// The coordinator's tracked set (CoordinatorTracked): each tracked geometry's entry (Tracked, its caches and list keys), the
// tracked map, the scene identity and the walk's and the joins' orders. Every phase reads it: attach and detach (ApplyEvents.cpp,
// Classification.cpp), the walk (Walk.cpp), the joins (AccumulatePhase.cpp) and the components' services (Components.cpp).
//
// A private base of Scene::Coordinator, included by Coordinator.h: names, types and initialisers are the coordinator's own,
// and the functions over them are declared there.

namespace DCLF::Scene
{
	struct CoordinatorTracked
	{
		/** @brief How a tracked geometry came to be tracked (diagnostics). */
		enum class TrackSource : std::uint8_t
		{
			AttachEvent,       // an attach event's subtree walk
			CategoryAppeared,  // the walk of a category node RefreshCategoryNodes found new
		};
		struct Tracked
		{
			RE::NiPointer<RE::BSGeometry> geometry;
			// Its shader and layer properties, held from the naming event's pins (HoldProperties): the scene work's references are copies
			// of these.
			RE::NiPointer<RE::BSShaderProperty> property, layerProperty;
			RE::BSShaderProperty* HeldProperty(bool a_layer) const { return (a_layer ? layerProperty : property).get(); }
			// Written while the mirror held no record of it (WriteObject): evaluated again when a capture names it.
			bool leafWaiting = false;
			std::uint64_t identity = 0;
			std::uint64_t groupIdentity = 0;
			const RE::TESObjectREFR* ownerReference = nullptr;  // its owner class's reference; lookup only, never published
			const void* ancestorKey = nullptr;  // the ancestor value's lookup key (AncestorKeyOf), refreshed on structural attachment
			std::uint64_t ancestorMapGeneration = 0;
			std::int32_t ancestorValue = -1;
			RE::NiNode* categoryNode = nullptr;
			// UnsupportedParent, Billboard or Switch (decided per frame) for what lies between the leaf and its category node; else None.
			Ineligible parentReason = Ineligible::None;
			// When and how it was added (GetTrackInfo).
			std::uint32_t trackedFrame = 0;
			TrackSource trackedBy = TrackSource::AttachEvent;

			/**
			 * @brief A cached negative verdict (positives are Derived's), checked in full against its witnesses each evaluation
			 * (rendererData too: a stale negative would leave an object native forever); ValidateSlice covers what has no cheap witness.
			 */
			struct StaticVerdict
			{
				bool cached = false;
				Ineligible reason = Ineligible::None;
				const RE::BSGraphics::TriShape* rendererData = nullptr;
				const RE::BSShaderProperty* property = nullptr;
				const RE::BSShaderMaterial* material = nullptr;
				std::uint8_t fadeState = 0;
			};
			StaticVerdict verdict;

			/**
			 * @brief The cached positive derivation (descriptors, static flags, pipeline key, slots), valid while its witnesses hold; slots
			 * are checked against their keys before use, so a reused slot is never handed back.
			 */
			struct Derived
			{
				bool valid = false;
				std::uint32_t generation = 0;  // the slot tables' generation the slots belong to
				const RE::BSGraphics::TriShape* triShape = nullptr;
				const RE::BSShaderProperty* property = nullptr;
				const RE::BSShaderMaterial* material = nullptr;
				std::uint8_t fadeState = 0;
				bool interior = false;
				bool alphaBelowOne = false;
				std::uint32_t technique = 0;
				std::uint32_t subPass = 0;
				std::uint32_t hint = 0;
				std::uint32_t biasWitness = 0;
				LightingDescriptors descriptors;
				std::uint32_t staticFlags = 0;
				PipelineKey key{};
				std::uint32_t geometrySlot = ~0u;
				std::uint32_t pipelineSlot = ~0u;
				std::uint32_t materialSlot = ~0u;
			};
			Derived derived;

			/**
			 * @brief The cull-only verdict (a culling candidate when the engine did not keep it) and the pass that took it; stands until a
			 * delta walk event retakes it.
			 */
			std::uint32_t candidateFrame = 0;  // the pass that classified it (PassStamp); 0: never classified
			Ineligible candidateReason = Ineligible::None;
			// Its entry class (its geometry type's rule, Api::GeometryTypeContribution::entry: Features/Faces' face shape, a BSDynamicTriShape
			// under a BSFaceGenNiNode, positions from its head's snapshots, followed by the Faces component; None: none); resolved once by the
			// walk.
			Api::EntryClass entryClass = Api::EntryClass::None;
			bool entryClassResolved = false;
			// Whether its last record write found no second vertex stream (its component's next event, a face shape's head's publication,
			// rewrites it).
			bool streamWaiting = false;
			// Its entry class's source node, the parent its rule holds (EntryRule::holdsParent: a face shape's head), held from the batch's pins
			// at its attach (the component's record's reference is a copy).
			RE::NiPointer<RE::NiNode> sourceRef;
			// Its property's fade node, held from the naming event's pins: fade-root owners (and the components': Features/TreeWind's tree
			// owners) are copied from it, never made from a key.
			RE::NiPointer<RE::NiAVObject> fadeNodeRef;
			const void* fadeNodeRequested = nullptr;  // the fade node key asked of the render thread (FadeNodeRequest), until answered
			// Its commit veto (Api::kVetoHold, kVetoObserve; 0: none), a component's (CoordinatorComponentContext::SetEntryVeto): read by the
			// commit as data (VetoedPhases), counted in vetoHeld and vetoObserved.
			std::uint8_t veto = 0;
			// Its owner class and rules (Api::EntryClassContribution, by its leaf's reference's form type: Features/Actors' actor-owned, whose
			// draws Advanced Skin gives the actor's wetness, SceneTables::classMembers: Integrations/AdvancedSkin's), apart from
			// entryClass (a face shape is both). Resolved once by the walk (Extensions::OwnerClassOf).
			Extensions::OwnerClassRule owner;
			bool ownerResolved = false;
			// The joins' verdict when they left it unbound (Diagnostics::CensusVerdictOf reads this frame's).
			Ineligible accumulateReason = Ineligible::None;
			std::uint32_t accumulateReasonFrame = 0;  // the frame it was taken
			std::uint64_t accumulateReasonPass = 0;   // the pass (a join failed for a verdict of this pass)
			// Its index in this walk's tables, valid while objectStamp equals the coordinator's.
			std::uint32_t objectStamp = 0;
			std::uint32_t objectId = 0;
			// Its persistent object slot (objectId's value while it keeps a record), kNoObjectSlot while it has none.
			std::uint32_t slot = kNoObjectSlot;
			// Its entry's root: the node whose bound decides sun caster candidacy, resolved once (the graph above a tracked geometry is fixed;
			// ResolveSunEntry), the tables' listedRoot column. Kept apart from listedRoot below (the rootDependents key, this one while
			// listed): that one is cleared when the entry is unlisted, and ResolveSunEntry (also called by the walk's write and the
			// placements) sets this one without listing it.
			const RE::NiAVObject* entryRoot = nullptr;
			bool sunEntryResolved = false;
			// Its light entry (LightEntryOf), listed in lightDependents while tracked; null: none.
			const RE::NiAVObject* lightRoot = nullptr;
			// Its main-pass layer's slot (LayerPropertyOf: a multi-index shape's additional property, second index list, decal group 3), or
			// kNoObjectSlot; it lives and dies with the base (WriteLayer).
			std::uint32_t layerSlot = kNoObjectSlot;
			Derived layerDerived;
			const void* listedLayerProperty = nullptr;  // the layer's property in propertyDependents
			// Its membership passes' derived descriptors, base and layer (BindByMembership stores them): they go with the entry.
			MembershipDerived membershipDerived, membershipLayerDerived;
			// The delta walk's. perFrame: inputs change every frame (PerFrameTraits), set by a classifying evaluation and kept.
			// scheduledWalk: the walk that last scheduled it. bucket: its histogram bucket (kNoBucket: none).
			bool perFrame = false;
			bool perFrameListed = false;
			// lightTraits: per frame only because it moves, is skinned or is an actor's: while its classification stands the light path
			// retakes only an actor's verdict and a skin's partitions, and only when an event names it (MoverDue); its placement is the
			// plan's (moverSet). 0 otherwise. movedWalk: the walk that took this path.
			std::uint32_t lightTraits = 0;
			std::uint32_t movedWalk = 0;
			// Its move events' keys (MoveKeysOf): the one reference on its chain (MoveKeyOf); with several, the innermost here and the
			// outermost in moveKeyOuter; with none, its category node. The light path takes it when one of them moved (MoverDue).
			const void* moveKey = nullptr;
			const void* moveKeyOuter = nullptr;
			// The witness walk that evaluated it though no event named it (parity: the gate's check, MergeEntry).
			std::uint32_t gateWitnessWalk = 0;
			// An actor's light-path entry: the nodes up to its category node (in hiddenDependents), and whether one had a hidden event
			// since its last evaluation. Owed to the next per-frame walk, not the draining event walk, which skips per-frame entries (else
			// the first-person switch's hide of the third-person skeleton is lost).
			std::vector<const RE::NiAVObject*> hiddenChain;
			bool hiddenEventOwed = false;
			// The keys it is listed under: its shader and alpha properties in propertyDependents (the last evaluation's), its entry root
			// (entryRoot) in rootDependents (while tracked).
			const void* listedProperty = nullptr;
			const void* listedAlpha = nullptr;
			const void* listedEmittance = nullptr;  // external emittance's shared colour (emittanceEvents)
			const RE::NiAVObject* listedRoot = nullptr;
			std::uint32_t fullWalk = 0;  // the walk that must write it in full (an event, not the per-frame set)
			std::uint8_t bucket = kNoBucket;
			std::uint32_t scheduledWalk = 0;
			static constexpr std::uint8_t kNoBucket = 0xFF;
		};
		ankerl::unordered_dense::map<RE::BSGeometry*, Tracked> tracked;
		SceneIdentity sceneIdentity;
		/** @brief The walk's iteration order, a member to keep its capacity; a join's entry carries its membership pass. */
		struct OrderEntry
		{
			RE::BSGeometry* geometry;
			Tracked* tracked;  // mutable: BuildFrame updates the cached verdict through it
			const AccumulatedPass* accumulated;
			bool layer = false;  // the joins': the entry's layer (Tracked::layerSlot) rather than its base
		};
		std::vector<OrderEntry> order;
		// The joins' iteration (the pass's membership joins); a member for its capacity.
		std::vector<OrderEntry> accumulateOrder;
	};
}
