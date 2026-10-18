#pragma once

// What crosses from the coordinator to the render thread with a publication: the immutable ScenePublication, the publication
// log's per-publication deltas (claims, placement plans, shading names, seed requests, gate flips) and their item types. Made on
// the scene-work thread, read on the render thread once installed.

#include <array>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "Features/DrawcallLimitFix/Api/FrameValues.h"
#include "Features/DrawcallLimitFix/Core/Extensions/EngineEvents.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Lookups.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Tables.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/SceneSet.h"
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"

namespace DCLF
{
	struct PipelineCatalog;
	struct EntrySpace;

	/**
	 * @brief What a scene revision is made of besides its publication's tables, lookups and catalog (the revision is its publication's
	 * alone: MakeRevision reads those from the publication): the counts the latch and the shadow reservations size by - the pipeline
	 * slots, and the shadow key slots the lookups resolved with the keys the tables' casters and occluders use.
	 */
	struct RevisionRequest
	{
		std::uint32_t pipelines = 0;
		std::size_t shadowSlotKeys = 0, shadowKeys = 0;
	};

	/**
	 * @brief A publication of the scene, complete and immutable: everything a frame reads of the scene, made by the
	 * coordinator (PublishScene) and handed to the snapshot builder (IndirectDraws::PostSnapshotWork), which posts it with its
	 * draws and its revision as one snapshot; the render thread adopts the newest snapshot whole (HandOverAtFrameStart installs its
	 * publication). The claims' changes and the frame inputs each publication's pass made (the placement plan, the shading names, the
	 * seed requests, the switches, the retired imports) travel on the publication log (PublicationDeltas), so a publication the builder
	 * skipped loses none of them.
	 */
	struct ScenePublication
	{
		std::uint64_t sequence = 0;
		std::uint32_t commitFrame = 0;        // the scene frame of the commit it applies (CommitSet's; stats and the install delay)
		std::uint32_t togglesGeneration = 0;  // the toggles generation that commit was made under (FrameInputs)
		// ... and the set: the frames' while it is installed (Toggles::Adopt), and its revision's (MakeRevision).
		ToggleSet toggles{};
		std::uint32_t tablesGeneration = 0;  // the tables' generation (GetTablesGeneration while it is the frame's)
		std::shared_ptr<const SceneTables> tables;
		std::shared_ptr<const SetSnapshot> claims;
		std::array<std::uint32_t, 3> lackingCount{};
		// The lookups the draws were built with, parallel to the tables, and the catalog they were resolved from, whose
		// set versions the frames that install it bind (DrawPipelines::HoldCatalog; a revision names its request's explicitly).
		std::shared_ptr<const Lookups> lookups;
		std::shared_ptr<const PipelineCatalog> catalog;
		// The sun and light candidates as its pass left them (UpdateSunCandidates, UpdateLightCandidates).
		std::shared_ptr<const EntrySpace> sunCandidates, lightCandidates;
		std::uint64_t lightEntriesAppeared = 0;
		// The category nodes as its pass left them (immutable, copied again only when the set changed, a cell attach or detach): what the
		// point lights' culls ask of the frame (IsCategoryNode), never the coordinator's set. The light entries change with every
		// tracked root, so they travel as changes on the publication log instead (PublicationDeltas::lightEntries).
		std::shared_ptr<const ankerl::unordered_dense::set<RE::NiNode*>> categoryNodes;
		// The scene revision's counts (the snapshot builder's).
		RevisionRequest revision;
		// The oldest event it is the first publication of (SceneTracker::Event::stampNs, stampFrame; 0: none), for the
		// event-to-adoption latency (IndirectDraws' snapshot report).
		std::uint64_t eventNs = 0;
		std::uint32_t eventFrame = 0;
	};

	/**
	 * @brief What FrameValues samples (dclf-architecture.md, "FrameValues"): the placements and palettes of the per-frame
	 * movers (the walk's kept movers, every one), the sun entries of the roots the walk took, and the slots the walk wrote in
	 * full - each an object's slot and the engine objects its row is read from, held. Made by the walk on the scene task, taken
	 * by the render thread at the next frame's start (FrameView::TakePlacementPlans) and released there: its references are the engine's to
	 * drop. The tables hold no placement or palette: these are the only rows the frames draw with.
	 *
	 * A plan is a delta of the placement set (the movers, and the listed roots with their dependents): what joined or changed, with
	 * references, and what left, by key. FrameValues applies every plan in order onto the set it keeps (a reset plan clears it first and
	 * carries the set whole), so none may be skipped: the publication log carries every plan since the last frame (TakeDeltas).
	 */
	struct PlacementPlan
	{
		struct Item
		{
			RE::NiPointer<RE::BSGeometry> geometry;
			RE::NiPointer<RE::NiAVObject> listedRoot;  // its listed root, the sun entry (ResolveSunEntry); none: the entry is unbounded
			std::uint32_t slot = kNoObjectSlot;
			std::uint32_t layerSlot = kNoObjectSlot;  // its layer's row (WriteLayer): the same placement, its layer property's fade node
			// A skin's palette block (SceneTables::boneOffset, boneRows), 0 rows without one: the engine's palette update, then its
			// rows (PaletteRowsOf).
			std::uint32_t boneOffset = 0, boneRows = 0;
			// A first-person entry's (a CategoryRoot view's: ViewRootEntry). The engine moves the first-person skeleton's world translations to its
			// root's space on the render thread (FUN_1414cb020 to FUN_1414cb270, and RenderDepth's block), within the read window: the
			// producer never samples it, and the render thread takes a claimed one's row and palette at the frame's start, before
			// the shift (FrameValues::Kick).
			bool firstPerson = false;
			// Its palette rows class (Api::PaletteRowsContribution; None: a skin's block, or none): a class's block (boneOffset and
			// boneRows) is filled by the class's registration every frame, which may give its placement row (Features/Particles: a particle
			// system's quads, five rows a particle, and its snapshot's world).
			Api::FrameValueItemClass itemClass = Api::FrameValueItemClass::None;
		};
		/** @brief An item as compared (raw pointers, never dereferenced): the coordinator's planned set, the parity's, FrameValues' kept one. */
		struct Key
		{
			const void* object = nullptr;
			std::uint32_t slot = kNoObjectSlot, layerSlot = kNoObjectSlot, boneOffset = 0, boneRows = 0;
			const void* sunEntry = nullptr;
			bool firstPerson = false;
			Api::FrameValueItemClass itemClass = Api::FrameValueItemClass::None;
			bool operator==(const Key&) const = default;
			static Key Of(const Item& a_item)
			{
				return { a_item.geometry.get(), a_item.slot, a_item.layerSlot, a_item.boneOffset, a_item.boneRows, a_item.listedRoot.get(), a_item.firstPerson,
					a_item.itemClass };
			}
		};
		struct Root
		{
			RE::NiPointer<RE::NiAVObject> root;
			std::vector<Item> dependents;  // joined or changed (by geometry); their sun entry is the root's bound (listedRoot unused)
		};
		/** @brief Parity (the coordinator's full rebuild of the set after this plan): movers by object, roots by node with their dependents, sorted. */
		struct Check
		{
			std::vector<Key> movers;
			std::vector<std::pair<const void*, std::vector<Key>>> roots;
		};
		// The set's changes, applied in this order: what left (movers, whole roots, single dependents), then what joined or changed.
		std::vector<const void*> moversLeft;                             // by geometry
		std::vector<const void*> rootsLeft;                              // by node, with every dependent
		std::vector<std::pair<const void*, const void*>> dependentsLeft;  // (root node, geometry) of a root that stays
		std::vector<Item> movers;                                        // joined or changed (by geometry)
		std::vector<Root> roots;                                         // listed, or whose dependents joined or changed (by node)
		bool reset = false;                                              // the kept set is cleared first (the plan carries it whole)
		std::vector<Item> written;  // once, by the frame that takes the plan
		std::uint32_t slots = 0;         // the object slots the tables held
		std::uint32_t boneCapacity = 0;  // the palette rows their blocks span (SceneTables::BoneCapacity)
		std::uint64_t walk = 0;          // the walk that made it
		std::shared_ptr<const Check> check;  // parity only (the set after this plan, rebuilt whole): FrameValues compares its kept set
	};

	/**
	 * @brief A slot whose shading FrameValues samples (SampleShading) at the next frame's start: named by its shading events (the
	 * walk: NameShadingEvents) and by the accumulate phase's patch. Holds its property, so the sample never reads a freed one.
	 */
	struct ShadingItem
	{
		RE::NiPointer<RE::BSShaderProperty> property;  // a BSLightingShaderProperty (the layer's for a layer's slot), or an effect property
		std::uint32_t slot = kNoObjectSlot;
		std::uint32_t pass = 0;   // its pipeline's pass descriptor
		bool member = false;      // a resident record's (SampleShading's a_member)
		// The shading fields it owns (SceneTables::ShadingFieldsOwned, a bit by Api::ShadingRowField: a member of the field's class, its
		// value the field's capture's); every other field zero.
		std::uint8_t fieldsOwned = 0;
	};

	/**
	 * @brief A fade root whose seed FrameValues samples (FadeState::StaticOf) in the frame that first draws its generation, into
	 * its seed row (FadeSeedRow): listed, reseeded by a placement snap, or owned; or a node a coordinator component listed, whose seed
	 * its feature takes at the frame's start (Api::CoordinatorComponentContext::RequestSeed; root: the component's slot). Holds the node.
	 */
	struct FadeSeedItem
	{
		RE::NiPointer<RE::NiAVObject> node;
		std::uint32_t root = kNoFadeRoot;
		std::uint32_t generation = 0;
		std::uint32_t row = 0;
	};

	/** @brief One member slot's shading field value, as the frame's start captured it (Extensions::CaptureShadingFields). */
	using ShadingFieldValue = Api::ShadingFieldValue;

	// The light entries (IsLightEntry): a key of lightDependents gaining its first geometry, losing its last, or every key gone, as
	// the coordinator's changes (onto the log: PublicationDeltas::lightEntries) and the render thread's set kept from them. Changes, not
	// copies of the set: it changes with every root tracked or untracked, every frame in motion.
	struct LightEntryChange
	{
		const RE::NiAVObject* node = nullptr;
		std::int8_t kind = 0;  // 1 added, -1 removed, 0 every entry cleared
	};

	/**
	 * @brief What one publication's passes made for the frame and the frame's start takes, carried by the publication log in
	 * the publication's order: the main claims' changes against the publication before it (taken up to the installed one), and the
	 * frame inputs (taken up to the newest one taken): the walks' placement plans, the slots named for their shading, the fade and
	 * components' seed requests, the switches the walks applied (switchResync: PrimaryCull reads them all), the import leases of the geometry
	 * slots cleared, and the published entry classes' membership changes (the shading fields' members: the actors' wetness).
	 */
	struct PublicationDeltas
	{
		std::uint64_t publication = 0;  // its publication's sequence
		std::uint32_t commitFrame = 0;  // that publication's commit (the install delay)
		// That publication's oldest event (ScenePublication::eventNs, eventFrame): the frame installing it, or one after it the
		// builder skipped past to, measures the event to its adoption.
		std::uint64_t eventNs = 0;
		std::uint32_t eventFrame = 0;
		std::vector<const RE::BSGeometry*> joined, left;
		std::vector<std::shared_ptr<const PlacementPlan>> plans;
		std::vector<ShadingItem> shading;
		std::vector<std::vector<FadeSeedItem>> componentSeeds;  // by coordinator component (Api::CoordinatorComponentId)
		std::vector<const RE::NiAVObject*> switches;
		bool switchResync = false;
		std::vector<std::shared_ptr<const void>> retiredImports;
		// By published entry class (Api::EntryClass): its membership writes (SceneTables::ClassMembers::changes), for the shading fields.
		std::array<std::vector<Api::ClassMemberChange>, Extensions::kPublishedEntryClasses> classMembers;
		std::vector<LightEntryChange> lightEntries;
		// By coordinator component (Api::CoordinatorComponentId): the deltas its passes published (CoordinatorComponentContext::PublishDelta),
		// handed to its adoption.
		std::vector<std::vector<std::shared_ptr<const void>>> componentDeltas;
	};

	/** @brief What a publication is made from (the tables' logs, stamps and sizes, the lookups, the catalog): a pass that changed none of it publishes nothing. */
	struct PublishKey
	{
		std::array<std::uint64_t, 17> words{};
		std::array<std::uint64_t, 6> lookups{};
		const void* catalog = nullptr;
		bool operator==(const PublishKey&) const = default;
	};
	namespace Scene
	{
		/** @brief The latency histograms' buckets (frames): 0, 1, 2, 3, 4-7, 8-15, 16-63, 64+. */
		inline constexpr std::size_t kAgeBuckets = 8;
		inline constexpr std::array<const char*, kAgeBuckets> kAgeBucketNames{ "0", "1", "2", "3", "4-7", "8-15", "16-63", "64+" };
		constexpr std::size_t AgeBucket(std::uint32_t a_frames)
		{
			return a_frames < 4 ? a_frames : a_frames < 8 ? 4 : a_frames < 16 ? 5 : a_frames < 64 ? 6 : 7;
		}
		/** @brief A slot's set phases in given tables, 0 past the column (sized when the set is applied). */
		inline std::uint8_t PhasesIn(const SceneTables& a_tables, std::uint32_t a_slot) { return a_slot < a_tables.setPhases.size() ? a_tables.setPhases[a_slot] : std::uint8_t{ 0 }; }
		/** @brief Whether a main-pass build can draw with the pipeline slot: compiled into the set for the slot's current key in a_lookups. */
		inline bool PipelineDrawableIn(const SceneTables& a_tables, const Lookups& a_lookups, std::uint32_t a_pipeline)
		{
			return a_pipeline < a_tables.pipelines.size() && a_pipeline < a_lookups.pipelines.size() && a_lookups.pipelines[a_pipeline].setIndex != Lookups::kNone &&
			       a_lookups.pipelines[a_pipeline].key == a_tables.pipelines[a_pipeline];
		}
	}
}
