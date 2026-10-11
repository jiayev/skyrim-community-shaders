#pragma once

#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <deque>
#include <map>
#include <set>
#include <utility>
#include <vector>

#include "Features/DrawcallLimitFix/Common/AsyncWorker.h"
#include "Features/DrawcallLimitFix/Engine/EngineReadWindow.h"
#include "Features/DrawcallLimitFix/Scene/MaterialBindings.h"
#include "Features/DrawcallLimitFix/Scene/SharedBindings.h"
#include "Features/DrawcallLimitFix/Scene/MaterialPort.h"
#include "Features/DrawcallLimitFix/Scene/MaterialSources.h"
#include "Features/DrawcallLimitFix/Scene/GeometryPort.h"
#include "Features/DrawcallLimitFix/Scene/MaterialPortParity.h"
#include "Features/DrawcallLimitFix/Scene/SceneMirror.h"
#include "Features/DrawcallLimitFix/Scene/LodSegments.h"
#include "Features/DrawcallLimitFix/Scene/TreeLod.h"
#include "ActorValueIndex.h"
#include "Features/DrawcallLimitFix/Engine/FaceSnapshots.h"
#include "Features/DrawcallLimitFix/Common/KeptState.h"
#include "Features/DrawcallLimitFix/Published/SceneIdentity.h"
#include "Features/DrawcallLimitFix/Common/SlotTable.h"
#include "Features/DrawcallLimitFix/Common/Retirement.h"
#include "Features/DrawcallLimitFix/Common/EventQueue.h"
#include "Features/DrawcallLimitFix/Common/LatestSlot.h"
#include "Features/DrawcallLimitFix/Common/SceneWake.h"
#include "Features/DrawcallLimitFix/Common/PublicationLog.h"
#include "Features/DrawcallLimitFix/Common/Toggles.h"
#include "LightingDescriptors.h"
#include "LocalShadows.h"
#include "ConstantEvaluator.h"
#include "Features/DrawcallLimitFix/Scene/FrameTables.h"
#include "Lookups.h"
#include "Records.h"
#include "SceneSet.h"

namespace DCLF
{
	namespace Scene
	{
		struct SwitchEvent;
	}
	/** @brief Attributes the time since the last call to one BuildPart (SceneStore/Internal.h; CS_DCLF_PROFILE). */
	struct FrameGlobals;
	struct PartTimer;
	struct SunCandidates;
	struct PipelineCatalog;

	/**
	 * @brief The parts of SceneStore::BuildFrame, each measuring one thing.
	 *
	 * Timed only under CS_DCLF_PROFILE. One part per unrelated piece of work, so each can be optimised on its own.
	 */
	enum class BuildPart : std::uint32_t
	{
		Walk,            // the pass map, built before the loop
		PassLookup,      // FindAccumulatedPass per object
		ClassifyStatic,  // type, skin, property, material alpha, DeriveLightingDescriptors
		ClassifyFrame,   // the parent chain (hidden, actor) and the fade
		Diagnostics,     // the derivation counters, which only feed a log line
		Resolve,         // GpuResources::Resolve for the vertex and index buffers
		Dedup,           // the geometry, pipeline and material map probes
		PipelineEval,    // the new-pipeline body: EvaluateGeometry and EvaluateTechnique
		MaterialEval,    // the new-material body: EvaluateMaterial (the stand-in)
		Record,          // scene writes and accumulator patch bookkeeping
		CapturePatch,    // accumulator's remaining engine-affine shading/light/tree sampling
		ApplyPatch,      // accumulator's value-only table propagation
		DedupHit,   // the three map probes on the HIT path (Dedup measures the misses)
		LoopTail,   // per TRACKED object: the continue path of a rejected one, and the iteration itself
		Skinning,   // skinned objects: the engine's palette update and the row copy
		Count
	};

	inline constexpr std::array<const char*, static_cast<std::size_t>(BuildPart::Count)> kBuildPartNames{
		"walk", "pass-lookup", "classify-static", "classify-frame", "diagnostics",
		"resolve", "dedup", "pipeline-eval", "material-eval", "record", "capture-patch", "apply-patch",
		"dedup-hit", "loop-tail", "skinning"
	};

	/**
	 * @brief The sub-zones of the scene tables (DrawcallLimitFix::BeginSceneFrame): the events applied (ApplyEvents), the scene phase
	 * block by block, and (T6b3c, one pass) the joins, the commit and the publication. Each is a Tracy zone and a sum in
	 * Stats::scenePartMs, reported under CS_DCLF_STATS. The first evaluation round's split by entry (EvaluateKind) is measured only
	 * under CS_DCLF_PROFILE.
	 */
	enum class ScenePart : std::uint32_t
	{
		CategoryNodes,     // the attach queue's drain and RefreshCategoryNodes
		AttachDetach,      // AddSubtree per attach, EraseTracked per detach
		Validate,          // ValidateSlice
		StructuralEvents,  // the fade, property, node and switch queues' drains
		Prologue,          // the used sets, the change log trim and check, LOD fade settings, GpuResources::BeginFrame
		SweepSlots,        // UpdateSlotReferences and the slot expiry
		CullHiddenBits,    // CaptureCullHiddenBits
		Schedule,          // BeginWalk and the order: the per-frame set and the events' dependents
		SwitchEvents,      // ApplySwitchEvents
		Evaluate,          // the first round: per-frame entries and those the events scheduled
		GeometryScan,      // FinishDeltaWalk's geometry slot touches and the stale-slot object scan
		LaterRounds,       // the rounds for stale and refreshed geometry slots
		ShadowSets,        // RefreshShadowSets and its parity
		ObjectSweep,       // SweepObjectSlots (a full walk only)
		SunCandidates,     // UpdateSunCandidates
		FaceWalk,          // EndFaceWalk
		WalkParity,        // CheckWalkParity (CS_DCLF_WALK_PARITY only)
		Shading,           // NameShadingEvents: the shading events' slots named for FrameValues
		Placements,        // PublishPlacementPlan: the roots taken, and the walk's plan for FrameValues
		Joins,             // T6b3c, the pass's joins: BuildAccumulatePhase, the new pipelines' blocks, the bindings, the lookups resolved
		Commit,            // T6b3c: CommitSet, after the joins
		Publish,           // T6b3c: PublishScene (the set applied, the claims revoked, the tables and lookups published)
		// T6b3e, inside the parts above (ScenePartNested): the evaluation rounds' and the joins' two halves, every round's.
		EvaluateFanout,    // EvaluateEntry over the round (on the preparation pool from 128 entries, CS_DCLF_FANOUT=2)
		EvaluateMerge,     // MergeEntry over the round, in order, on the scene lane
		JoinsFanout,       // the joins' evaluations (EvaluateJoinPre over the bind queue, EvaluateJoin over the joins)
		JoinsMerge,        // the joins' merges, in order
		Count
	};

	inline constexpr std::array<const char*, static_cast<std::size_t>(ScenePart::Count)> kScenePartNames{
		"category nodes", "attach/detach", "validate", "structural events", "prologue", "sweep slots", "cull hidden bits",
		"schedule", "switch events", "evaluate", "geometry scan", "later rounds", "shadow sets",
		"object sweep", "sun candidates", "face walk", "walk parity", "shading", "placement plan", "joins", "commit", "publish",
		"evaluate (fan-out)", "evaluate merge", "joins (fan-out)", "joins merge"
	};

	/** @brief T6b3e: a part measured inside another (the fan-outs' halves), left out of the parts' sum. */
	inline constexpr bool ScenePartNested(std::size_t a_part)
	{
		return a_part >= static_cast<std::size_t>(ScenePart::EvaluateFanout);
	}

	/** @brief What the first evaluation round did with an entry, for its per-entry timing (CS_DCLF_PROFILE). */
	enum class EvaluateKind : std::uint32_t
	{
		Light,        // the light path kept or moved it
		LightMissed,  // the light path tried and fell through to the full write
		Actor,        // written in full: an actor's (kTraitActor) whose verdict lets it have a record
		ActorNoRecord,  // written in full: an actor's whose verdict gives it none (hidden, fading, ...)
		Face,         // written in full: a face shape
		PerFrame,     // written in full: any other per-frame entry
		Event,        // written in full: scheduled by an event, a pending evaluation or a full walk
		Count
	};

	inline constexpr std::array<const char*, static_cast<std::size_t>(EvaluateKind::Count)> kEvaluateKindNames{
		"light path", "light path missed", "actor", "actor without a record", "face", "other per-frame", "event"
	};

	inline constexpr std::uint32_t kNoObjectSlot = ~0u;

	/** @brief What a change-log entry changed (SceneStore::Tables::changeLog), by the columns its consumers read. */
	enum ChangeCause : std::uint32_t
	{
		kChangeBindings = 1u << 0,    // flags, material, pipeline (the draw's too), the fade distance, whether it has a fade node
		kChangeLights = 1u << 1,      // Light Limit Fix's room index and shadow mask
		kChangeTree = 1u << 2,        // tree animation
		kChangeSkin = 1u << 3,        // skin partitions, palette block (offset and rows)
		kChangeExtras = 1u << 4,      // the extras block and its rows
		kChangeShadow = 1u << 5,      // shadow technique and reason, sky technique, shadow material and diffuse
		kChangeGeometry = 1u << 6,    // geometry slot, the draw's geometry half, face stream, the object's geometry
		kChangeMembership = 1u << 7,  // residency
		// What a scene revision's shapes are made from (R3b, structural stamps), beside the cause it is also noted with: the
		// pipelines (the record's and the draw's: buckets and their capacities), the geometry half and partitions (the draws), the
		// shadow and occlusion techniques and rejection (the shadow keys).
		kChangeStructure = 1u << 8,
	};
	// No placement, palette or shading: those are FrameValues', never a column.
	inline constexpr std::uint32_t kChangeCauseCount = 9;
	// What a shadow input carries (IndirectDraws' shadow build): its caster state, bindings, geometry and partitions, residency.
	// Not placements: a caster's bound and sun entry are its object record's.
	inline constexpr std::uint32_t kShadowChangeCauses = kChangeShadow | kChangeBindings | kChangeGeometry | kChangeSkin | kChangeMembership;
	inline constexpr std::array<const char*, kChangeCauseCount> kChangeCauseNames{ "bindings", "lights", "tree", "skin", "extras", "shadow", "geometry",
		"membership", "structure" };
	inline constexpr std::uint32_t kChangeAll = (1u << kChangeCauseCount) - 1;
	// What revokes a claim made before it (R3b, structural stamps): kChangeStructure. Residency is not (the accumulate phase makes
	// every joiner resident), nor flags, materials or the fade distance (values the revision's shapes do not size by).
	inline constexpr std::uint32_t kStructureCauses = kChangeStructure;

	/** @brief The scene phase's flags the accumulate phase's patch keeps; the rest of an object's flags are the patch's. */
	inline constexpr std::uint32_t kSceneKeptFlags = kObjectSkinned | kObjectNoShadow | kObjectVolumetricOnly | kObjectShadowOnly;

	/**
	 * @brief The render thread's view of the static scene content Drawcall Limit Fix can draw.
	 *
	 * T6b3d: the coordinator's pump (the scene lane) drains the engine's events and applies them (CollectEvents, ApplyEvents:
	 * SceneTracker's events and the loaded-cell changes) in each scene pass, woken by its producers; the pass's walk refreshes the
	 * tracked geometries' records and its joins patch the half of each record the main camera's accumulator decides, then it commits
	 * the set and publishes. The render thread adopts the newest snapshot at the frame's start and serves the pass's requests.
	 *
	 * Tracked content is everything under the Static, Dynamic, MultiBound and Actor category nodes of attached cells
	 * (engine notes: cell 3D category nodes; an interior moves its actors into its rooms). Terrain, markers and water
	 * live under the other category nodes and are never tracked.
	 */
	class SceneStore
	{
	public:
		struct Tables
		{
			std::vector<ObjectRecord> objects;
			std::vector<RE::BSGeometry*> objectGeometry;  // parallel to objects
			std::vector<GeometryRecord> geometries;
			struct GeometryImport
			{
				std::uint64_t vertexGeneration = 0, indexGeneration = 0;
				std::shared_ptr<const void> vertexOwner, indexOwner;
			};
			// Import leases are independent of GpuResources' age-based cache.
			std::vector<GeometryImport> geometryImports;  // parallel to geometries
			std::vector<PipelineKey> pipelines;
			std::vector<MaterialRecord> materials;
			// Parallel to materials: a session-unique number, new whenever the slot's record is (re)written - a
			// first evaluation, or a stale record replaced by validation - but not when RefreshMaterialPatch writes
			// the frame's floats into it. A consumer that kept something derived from the record compares this
			// instead of the record's 2.3 KB.
			std::vector<std::uint32_t> materialVersion;
			// Parallel to materials: new whenever the record's frame floats (MaterialSources' frame components, the texture transform)
			// are written by the scene work (T6b2c step 7: RefreshMaterialSignatures, RefreshMaterialTransforms), the record's own values
			// kept: a row is written again, its record not evaluated again.
			std::vector<std::uint32_t> materialFrameVersion;
			// Versions the builds' kept bindings key on (IndirectDraws' PersistentBindings), each new (NextVersion) whenever
			// what it covers is written with a different value: per pipeline its PerGeometry floats and what its pairs' records
			// read of it (the key, the permutation), the technique's being its row's (TechniqueRow); per
			// material slot the frame's floats (RefreshMaterialSignatures, RefreshMaterialTransforms). materialVersion covers the
			// rest of a material.
			std::vector<std::uint32_t> pipelineBindingVersion;    // parallel to pipelines
			// The frame-sourced components, texture transforms and writer re-evaluations are the scene work's (T6b2c step 7).
			std::uint32_t versionCounter = 0;
			std::uint32_t NextVersion() { return ++versionCounter; }
			// No shading (MaterialData, EmitColor, SSRParams.w, the emissive multiplier, the wetness): FrameValues' rows
			// (BindlessShading), sampled at the frame's start for the slots the walk names (ShadingItem).
			std::vector<ObjectLights> lights;                     // parallel to objects
			// Tree animation, per object. Only technique 12 fills it; everything else leaves the engine's
			// defaults, which is what the template block already carried for them.
			std::vector<ObjectTreeAnim> treeAnim;                 // parallel to objects: the record's, as the member joined
			// Tree wind on the GPU (Records.h, TreeStatic): a slot per tree node a member draws under, held by its members
			// (treeRefs), its static row written when listed; objectTree is each object slot's tree slot (kNoTree when none);
			// treeObjects every member drawing under a node, for TreeWindCS to write their records. The versions are what the
			// commits upload against.
			std::vector<TreeStatic> trees;
			std::vector<std::uint32_t> treeRefs, treeFree;
			std::vector<const void*> treeNode;
			ankerl::unordered_dense::map<const void*, std::uint32_t> treeIndex;
			std::vector<std::uint32_t> objectTree;
			std::vector<TreeObject> treeObjects;
			std::uint64_t treesVersion = 0, treeObjectsVersion = 0;
			std::uint32_t treeGenerations = 0;
			// Fade roots on the GPU (Records.h, FadeRootStatic; FadeStateCS): a slot per fade node a member draws under, held by
			// its members (fadeRootRefs), its static row written when listed and again when an input changes by event (a tree's
			// LOD switch: fadeRootSwitch). objectFadeRoot is each object slot's root slot (kNoFadeRoot). A row's object is one of
			// its members, whose record's fade node row is the root's centre, chosen again when membership changes.
			std::vector<FadeRootStatic> fadeRoots;
			std::vector<std::uint32_t> fadeRootRefs, fadeRootFree;
			std::vector<const void*> fadeRootNode;
			ankerl::unordered_dense::map<const void*, std::uint32_t> fadeRootIndex;
			ankerl::unordered_dense::map<const void*, std::uint32_t> fadeRootSwitch;  // a tree root's LOD switch -> its root slot
			std::vector<std::uint32_t> objectFadeRoot;
			// The rows written, by root slot, since what the depth commit holds (FadeRootsSent trims it): it sends those runs, not
			// the table (about 1,350 rows of 80 B at the bridge, re-sent whole every few frames in motion before).
			ChangeJournal fadeRootsJournal;
			std::uint64_t FadeRootsVersion() const { return fadeRootsJournal.Version(); }
			void NoteFadeRoot(std::uint32_t a_root)
			{
				fadeRootsJournal.Mark(a_root);
				NoteFadeRootsWrite();
			}
			std::uint32_t fadeRootGenerations = 0;
			// Write stamps of the tree and fade-root families (trees, treeRefs, treeFree, treeNode, treeIndex, treeObjects, objectTree;
			// fadeRoots, fadeRootRefs, fadeRootFree, fadeRootNode, fadeRootIndex, fadeRootSwitch, objectFadeRoot): every write of a
			// family bumps its stamp (a row's through NoteFadeRoot), and they only grow, so a published snapshot with the tables' stamp
			// holds the family as the tables do and the publication copies it only when they differ (step 6d). The fade roots' journal
			// (the depth commit's hand-over moves it every frame) is not the family's: it is copied with the rest.
			std::uint64_t treesStamp = 0, fadeRootsStamp = 0;
			void NoteTreesWrite() { ++treesStamp; }
			void NoteFadeRootsWrite() { ++fadeRootsStamp; }
			// Advanced Skin's SkinPerGeometry (PS b7): the owning actor's sweat, water wetness, height and water depth,
			// Skin::GetWetness, which its SetupGeometry hook binds for every Lighting draw. Zero for everything not owned
			// by an actor (actorObjects lists those that are). The values are FrameValues' (CaptureWetness); these, who has one.
			std::vector<std::uint32_t> actorObjects;  // sorted by object index (the walk sorts it)
			ActorValueIndex actorWetness;
			// T6b3a: every membership write of actorWetness, in order, for the render thread's own index (CaptureWetness samples and fans
			// out there): the coordinator moves them onto the publication log (PublicationDeltas::actorWetness). Every write goes through
			// SetActorWetness or ClearActorWetness.
			struct ActorWetnessChange
			{
				std::uint32_t slot = 0;
				std::uint64_t group = 0, identity = 0;
				bool resetValue = false;
				bool clear = false;  // the index cleared (the slot ignored)
			};
			std::vector<ActorWetnessChange> actorWetnessChanges;
			void SetActorWetness(std::uint32_t a_slot, std::uint64_t a_group, std::uint64_t a_identity, bool a_resetValue = false)
			{
				actorWetness.Set(a_slot, a_group, a_identity, a_resetValue);
				actorWetnessChanges.push_back({ a_slot, a_group, a_identity, a_resetValue, false });
			}
			void ClearActorWetness()
			{
				actorWetness.Clear();
				actorWetnessChanges.clear();
				actorWetnessChanges.push_back({ 0, 0, 0, false, true });
			}
			// Skins of several partitions (CS_DCLF_SKIN_PARTITIONS): bit i draws partition i, walking the
			// geometry slots' nextPartition links from the object's geometryIndex (partition 0). 0 for every
			// other object, which draws its one geometry; kNoPartitions for a skin no LOD level draws anything of. A LOD skin's
			// (kMeshLOD under a fade node) is every level's partitions (row 3, the cumulative superset), narrowed by the draw.
			std::vector<std::uint16_t> skinPartitions;            // parallel to objects
			// A LOD skin's partitions per LOD level (SkinLodPartitionsOf: byte L, level L's row of the engine's table), 0 for any
			// other object. BuildDraws picks the byte by the level in its fade root's FadeStateCS state (T1b): the node's level
			// (+0x152), which GetRenderPasses reads, is never read; the record is written once, not at each level change.
			std::vector<std::uint32_t> skinLodPartitions;         // parallel to objects
			// No frame globals (the lighting, the fog, the character light's noise): the frame's capture (FrameCapture).
			// The property whose lighting pass supplied each pipeline's per-frame constants, kept so
			// RefreshFrameConstants can re-evaluate them once the main camera's state is current.
			std::vector<RE::BSShaderProperty*> geometryTemplate;  // parallel to pipelines
			std::vector<std::uint32_t> geometryTemplateObject;    // parallel to pipelines: the member whose property the template is (T6)
			// The PerTechnique values (and the technique's filter modes and shadow mask), one row per TechniqueKey - what
			// EvaluateTechnique reads of a pass descriptor - which every pipeline of the key shares (pipelineTechnique).
			// The coordinator evaluates a row when it is made (TechniqueRowFor) and every row again when its frame's TechniqueInputs move
			// (RefreshTechniqueRows, T6b2c), writing it only where it differs, versioning its floats (constantsVersion) and its bindings
			// (bindingVersion) apart. Rows are never freed: there are a few dozen.
			// The rows' keys (TechniqueKey); their constants are techniqueConstants (by row).
			std::vector<std::uint32_t> techniqueKeys;
			ankerl::unordered_dense::map<std::uint32_t, std::uint32_t> techniqueRow;  // TechniqueKey -> row
			std::vector<std::uint32_t> pipelineTechnique;  // parallel to pipelines: its row
			// The constants the builds pack into the pipeline rows (step 6e A): the render thread's pipeline blocks (GeometryPort) posted
			// with the key they were made for and applied by the coordinator (ApplyConstantsPosts), and the technique rows the coordinator
			// writes itself (EvaluateTechnique from its frame's FrameGlobals, T6b2c), published with the tables: a change is drawn with
			// the next publication, as a join. A pipeline's block holds its own values (what a DCLF_BINDLESS draw reads of it; the frame's globals are the frame
			// blocks'). A pipeline, or its technique row, without them is not drawable (MainReady, MainBuild::PackPipelines).
			struct PipelineConstantsRow
			{
				GeometryConstants constants{};
				PipelineKey key{};         // what it was evaluated for, with the slot's binding version
				std::uint32_t binding = 0;
				std::uint32_t version = 0;  // NextVersion, whenever it is written
				bool valid = false;
			};
			std::vector<PipelineConstantsRow> pipelineConstants;  // parallel to pipelines
			struct TechniqueConstantsRow
			{
				TechniqueConstants value{};
				std::uint32_t constantsVersion = 0, bindingVersion = 0;  // the floats', the bindings' (filter modes, the shadow mask)
				std::uint32_t passDescriptor = 0;  // a pass descriptor of the row's key (the first): what it is evaluated again for
				TechniqueInputs inputs{};          // the frame sample its last write was made from (the parity's: late or differ)
				bool valid = false;
			};
			std::vector<TechniqueConstantsRow> techniqueConstants;  // parallel to techniqueKeys
			// Bumped by every write of pipelineConstants or techniqueConstants (sizes included): the publication copies them only when
			// it moved (step 6d's stamps); the set's readiness reads it (a pipeline's constants arrived).
			std::uint64_t constantsStamp = 0;
			void NoteConstantsWrite() { ++constantsStamp; }
			bool PipelineConstantsCurrent(std::size_t a_pipeline) const
			{
				if (a_pipeline >= pipelineConstants.size() || a_pipeline >= pipelines.size() || a_pipeline >= pipelineBindingVersion.size())
					return false;
				const auto& row = pipelineConstants[a_pipeline];
				return row.valid && row.key == pipelines[a_pipeline] && row.binding == pipelineBindingVersion[a_pipeline];
			}
			bool TechniqueConstantsValid(std::size_t a_pipeline) const
			{
				return a_pipeline < pipelineTechnique.size() && pipelineTechnique[a_pipeline] < techniqueConstants.size() &&
				       techniqueConstants[pipelineTechnique[a_pipeline]].valid;
			}
			const TechniqueConstantsRow& TechniqueOf(std::size_t a_pipeline) const { return techniqueConstants[pipelineTechnique[a_pipeline]]; }
			/** @brief Whether the pipeline's technique binds the shadow mask (TechniqueKey's low bit, as its evaluation finds). */
			bool TechniqueShadowMask(std::size_t a_pipeline) const { return (techniqueKeys[pipelineTechnique[a_pipeline]] & 1u) != 0; }
			std::vector<PipelinePermutation> permutations;        // parallel to pipelines
			std::vector<DrawSequence> draws;  // one per object (templates: pipelineIndex is the table index)
			// Decals (CS_DCLF_DECALS): each decal object's slot in its group's draw range, in the engine's
			// own draw order (group, technique bucket, batch list, chain position), and how many slots
			// each group has. The colour epoch writes a decal's sequence to its slot - either the draw or
			// a zero-count one when culled - so that overlapping decals land in the same order every
			// frame, which an atomic append cannot promise. ~0u for everything that is not a decal.
			std::vector<std::uint32_t> decalOrdinal;  // parallel to objects
			std::array<std::uint32_t, 3> decalCount{};
			// Skinning (CS_DCLF_SKINNED): every skinned object's palette block - where the frame's palettes (FrameValues, the
			// engine's own NiSkinInstance::boneMatrices and prevBoneMatrices, three float4 rows a bone) hold its rows
			// (PaletteRowsOf) - and how many rows; 0 rows for anything that is not skinned. A skin's block is its own for as long
			// as its slot keeps a palette of that size (PlaceBones / FreeBones); the blocks handed out grow the capacity in
			// kBoneGrowRows steps, which moves none of them.
			static constexpr std::uint32_t kBoneGrowRows = 4096;
			std::vector<std::uint32_t> boneOffset;  // parallel to objects, in rows
			std::vector<std::uint32_t> boneRows;    // parallel to objects
			std::array<std::vector<std::uint32_t>, 81> boneFree{};  // freed blocks' offsets, by bones (rows / 3; at most 80)
			std::uint32_t boneTop = 0;       // rows handed out
			std::uint32_t boneCapacity = 0;  // rows the blocks span (FrameValues' palettes hold twice as many)
			std::uint32_t BoneCapacity() const { return boneCapacity; }
			/** @brief The slot's block for a palette of a_rows rows (its own when the size is the same): its first row. */
			std::uint32_t PlaceBones(std::uint32_t a_slot, std::uint32_t a_rows);
			void FreeBones(std::uint32_t a_slot);
			// Per-object extras (Records.h kExtraRows): the landscape blend parameters and the ProjectedUV
			// matrix and pixel parameters, filled at Prepass by RefreshFrameConstants for the objects that
			// carry kObjectLandBlend / kObjectProjectedUV. Rows of float4; per object the row offset, or
			// kNoExtraRows. The epoch sends them to its extras buffer (t126). A block is the object's
			// for as long as its patch holds it (AllocateExtras / FreeExtras), so it persists across walks.
			std::vector<float> extraRows;
			std::vector<std::uint32_t> extraOffset;  // parallel to objects
			std::vector<std::uint32_t> extraFree;    // freed blocks' row offsets
			// The blocks whose rows were written, allocated or freed (their row offsets): what the published tables replay the rows by
			// (step 6d). Every write of extraRows is named here (NoteExtrasBlock).
			EventLog<std::uint32_t> extrasBlockLog;
			void NoteExtrasBlock(std::uint32_t a_offset) { extrasBlockLog.Push(a_offset); }
			std::uint32_t AllocateExtras()
			{
				if (!extraFree.empty()) {
					const std::uint32_t offset = extraFree.back();
					extraFree.pop_back();
					std::fill_n(extraRows.begin() + std::ptrdiff_t(offset) * 4, std::size_t(kExtraRows) * 4, 0.0f);
					NoteExtrasBlock(offset);
					return offset;
				}
				const auto offset = static_cast<std::uint32_t>(extraRows.size() / 4);
				extraRows.resize(extraRows.size() + std::size_t(kExtraRows) * 4, 0.0f);
				NoteExtrasBlock(offset);
				return offset;
			}
			void FreeExtras(std::uint32_t a_slot)
			{
				if (a_slot < extraOffset.size() && extraOffset[a_slot] != kNoExtraRows) {
					NoteExtrasBlock(extraOffset[a_slot]);
					Retire(kRetiredExtras, extraOffset[a_slot]);
					extraOffset[a_slot] = kNoExtraRows;
				}
			}
			// The Utility technique each object casts with, without a view's mode bits (ShadowViews.h:
			// ShadowUtilityTechnique), and why the engine would not draw it into a shadow map. Both are
			// decided by the scene phase, because every shadow view is drawn before the accumulate phase
			// runs. 0 and ShadowReject::NotLighting for an object that is not a caster.
			std::vector<std::uint32_t> shadowTechnique;  // parallel to objects
			std::vector<std::uint8_t> shadowReject;      // parallel to objects (ShadowReject)
			// The Utility technique Skylighting's occlusion map draws the object with (Skylighting::OcclusionTechnique,
			// its own map's rule), 0 when it draws none or DCLF's variant of that map is off (SkyOcclusionEnabled). An
			// alpha-tested one's diffuse is in shadowDiffuse and shadowMaterial, as a caster's is.
			// Per occlusion view (kOcclusionSky, kOcclusionPrecipitation), parallel to objects.
			std::array<std::vector<std::uint32_t>, kOcclusionViews> occlusionTechnique;
			// The object's entry in the sun's full-frustum culling processes (their objectArray, which the cascade culls walk;
			// SceneStore::ResolveSunEntry), null when the entry is never tested (an actor's, whose entry is its cell's container).
			// Its bound is the object's placement row's (FrameValues, BindlessPlacement::sunEntry); the node is the diagnostics'.
			std::vector<const RE::NiAVObject*> sunEntryNode;  // parallel to objects
			// Whether the property has a fade node (LodFadeNodeOf), whose centre is the object's placement row's (FrameValues): the
			// fade-out test's and the draw's specular and envmap LOD fades'.
			std::vector<std::uint8_t> hasFadeNode;  // parallel to objects
			// A member's fade-out distance (kObjectFadeTest, AccumulatedPass::fadeDistance), against its fade node's centre
			// (BindlessPlacement::lodFadeNode): > 0 scaled by the camera's LOD factor, < 0 unscaled. Meaningless without the flag.
			std::vector<float> fadeDistance;  // parallel to objects
			// Which slots hold a resident record (PrimaryCull's).
			std::vector<std::uint8_t> residentSlot;  // parallel to objects
			// The DCLF set's phases of each slot (SetPhase bits, 0: not a member), CommitSet's alone; every build selects by them (the
			// record's kObjectMember is the main phase's copy the GPU reads).
			std::vector<std::uint8_t> setPhases;  // parallel to objects
			// The change log (drawcall-limit-fix.md, "Persistent draw state"): every write that changes a slot's columns
			// appends the slot with what changed (ChangeCause), whenever it happens. The persistent structures built from
			// the tables read it from their own position (LogCursor); one that fell behind the trimmed head, or whose tables
			// generation changed, reads every slot again. A write that leaves the columns as they were appends nothing.
			struct Change
			{
				std::uint32_t slot = 0;
				std::uint32_t causes = 0;  // ChangeCause bits
			};
			EventLog<Change> changeLog;
			std::array<std::uint64_t, kChangeCauseCount> changeCounts{};  // notes by cause, for the report
			void NoteChange(std::uint32_t a_slot, std::uint32_t a_causes)
			{
				if (!a_causes)
					return;
				changeLog.Push({ a_slot, a_causes });
				for (std::uint32_t bits = a_causes; bits; bits &= bits - 1)
					++changeCounts[std::countr_zero(bits)];
			}
			// The geometry slots written: a record resolved (ResolveGeometrySlot), a slot added (AllocateGeometrySlot), a
			// partition link that changed, a slot cleared (ClearGeometrySlot). Every write of geometries, geometryImports,
			// geometrySlotKey and geometryLayerKey is named here (the published tables replay them by it). What the persistent geometry tables repack (IndirectDraws' GeometryStore), read
			// like the change log.
			EventLog<std::uint32_t> geometryLog;
			void NoteGeometry(std::uint32_t a_slot) { geometryLog.Push(a_slot); }
			// The material slots whose record (materialVersion) or frame components (materialFrameVersion) changed: what the
			// resident region's pairs are resolved again for (MainBuild::UpdateRegionPairs), read like the change log.
			EventLog<std::uint32_t> materialLog;
			void NoteMaterial(std::uint32_t a_slot) { materialLog.Push(a_slot); }
			/** @brief Every slot is to be read again: every reader of the change log resyncs. */
			void InvalidateChangeLog()
			{
				changeLog.Invalidate();
				materialLog.Invalidate();
				extrasBlockLog.Invalidate();
			}
			/** @brief A slot's columns, everything a persistent consumer builds from, for the writers to compare against. */
			struct Columns
			{
				ObjectRecord object{};
				DrawSequence draw{};
				ObjectLights lights{};
				ObjectTreeAnim tree{};
				std::array<float, kExtraRows * 4> extras{};
				const RE::BSGeometry* geometry = nullptr;
				const RE::NiAVObject* sunEntryNode = nullptr;
				std::uint64_t identity = 0;
				std::uint64_t groupIdentity = 0;
				ID3D11ShaderResourceView* shadowDiffuse = nullptr;
				const RE::BSShaderMaterial* shadowMaterial = nullptr;
				float fadeDistance = 0.0f;
				std::uint32_t boneOffset = 0, boneRows = 0, extraOffset = kNoExtraRows, shadowTechnique = 0, faceStream = kNoFaceStream;
				std::array<std::uint32_t, kOcclusionViews> occlusionTechnique{};
				std::uint32_t sceneFlags = 0;
				std::uint16_t skinPartitions = 0;
				std::uint32_t skinLodPartitions = 0;
				std::uint8_t shadowReject = 0, resident = 0, hasFadeNode = 0;
			};
			Columns ColumnsOf(std::uint32_t a_slot) const;
			/** @brief What differs between two snapshots of a slot, as ChangeCause bits. */
			static std::uint32_t CausesBetween(const Columns& a_before, const Columns& a_after);
			/** @brief Notes what a write changed, against the snapshot taken before it. */
			void NoteWrite(std::uint32_t a_slot, const Columns& a_before) { NoteChange(a_slot, CausesBetween(a_before, ColumnsOf(a_slot))); }
			// NPC face shapes (Tracked::faceShape): per face object its positions in its head's current snapshot
			// (FaceSnapshots), retained by the lease, and the region of the positions buffer they go to. The epochs upload a
			// region when its generation changed, and bind it as the second stream. Kept with the object's record: a head's
			// publication updates its streams in place (SceneStore::ApplyFacePublications); a free entry has kNoFaceObject.
			static constexpr std::uint32_t kNoFaceObject = ~0u;
			struct FaceStream
			{
				std::uint32_t object = kNoFaceObject;
				std::uint32_t region = kNoFaceRegion;  // first vertex in the positions buffer
				std::uint32_t vertexCount = 0;
				std::uint64_t generation = 0;
				const float* positions = nullptr;
				std::shared_ptr<const std::vector<float>> owner;
				// Complete sibling set for a future consistency-group publication.
				std::shared_ptr<const FaceSnapshots::HeadView> headView;
				// The shape and its head, as keys: a publication finds the stream's positions by them (FaceSnapshots::View).
				const RE::BSGeometry* geometry = nullptr;
				const RE::BSFaceGenNiNode* head = nullptr;
			};
			std::vector<FaceStream> faceStreams;
			std::vector<std::uint32_t> faceStream;  // parallel to objects: index in faceStreams, or kNoFaceStream
			std::vector<std::uint32_t> faceStreamFree;
			// The (shape, region) of every stream freed since the walk's end took them (SceneStore::EndFaceWalk frees the region
			// when no stream of the shape holds it any more).
			std::vector<std::pair<const RE::BSGeometry*, std::uint32_t>> faceStreamsReleased;
			/** @brief Writes a slot's face stream: in place when it has one, else at a free index. */
			void SetFaceStream(std::uint32_t a_slot, FaceStream a_stream);
			/** @brief Frees a slot's face stream, if it has one. */
			void ClearFaceStream(std::uint32_t a_slot);
			// What an alpha-tested caster's shadow draw samples: its material's diffuse view, and the material
			// as the key its binding record is shared under. Read off the property here; the shadow epoch's
			// build reads only the material's texture transform, which shader-property controllers
			// (BSLightingShaderPropertyFloatController) move between Main::Draw, where this walk starts, and
			// BeforeShadowMaps (the async scene probe saw a scrolling UV one frame behind). Null otherwise.
			std::vector<ID3D11ShaderResourceView*> shadowDiffuse;    // parallel to objects
			std::vector<const RE::BSShaderMaterial*> shadowMaterial;  // parallel to objects
			// The distinct diffuse views among them (a few hundred), so the lookups are refreshed per view
			// rather than per caster.
			std::vector<ID3D11ShaderResourceView*> shadowTextureSet;
			ankerl::unordered_dense::set<ID3D11ShaderResourceView*> shadowTextureSeen;
			// Membership transitions produced only when the shadow dependency index changes, for the scene work's shadow texture
			// bindings (SceneStore::UpdateSharedBindings): false lets the binding go, true asks for it.
			std::vector<std::pair<ID3D11ShaderResourceView*, bool>> shadowTextureChanges;
			void TakeShadowTextureChanges(std::vector<std::pair<ID3D11ShaderResourceView*, bool>>& a_out)
			{
				a_out.clear();
				a_out.swap(shadowTextureChanges);
			}
			// The distinct shadow pipelines the frame's casters need, without a view's mode bits: a
			// handful in practice (twelve techniques in the Whiterun exterior). What the shadow programs
			// are compiled for, and what the shadow pipelines are built from once a view's mode is known.
			std::vector<ShadowPipelineKey> shadowKeysUsed;
			// The same for the occluders of Skylighting's map: complete techniques (RenderDepth included), no mode bits.
			std::array<std::vector<ShadowPipelineKey>, kOcclusionViews> occlusionKeysUsed;  // per occlusion view

			/**
			 * @brief The three shared tables keep their slots across frames (CS_DCLF_DERIVED_CACHE).
			 *
			 * Which slots are alive is each table's SlotTable: a slot lives while objects reference it
			 * (SceneStore::UpdateSlotReferences counts the references from the logs), and the drain runs before the loop,
			 * so no object of the frame can point at a slot it reuses. A table's columns
			 * are listed once (GeometryColumns, PipelineColumns, MaterialColumns), which is what grows and clears them.
			 *
			 * The used sets (usedPipelineBits, usedMaterialBits) are the slots a member's record names: a joining member marks
			 * its slots, and KeepResidentsAlive sets them again from the residents whenever membership changed, so a slot
			 * leaves the set with its last member and a drained one leaves it at once. Being in the set is what certifies the
			 * raw engine pointers a slot holds (a pipeline's lighting template, a material's key) for evaluation; CheckObjectSlots
			 * holds every bound record to it. geometryLastUsed is the walk's: the pass that last wrote the geometry slot (T6b3d: PassStamp). The
			 * keys are kept per slot so a freed slot can find its map entry, and so a cached slot index can be checked
			 * against what it was derived for.
			 */
			static constexpr std::uint32_t kSlotFree = ~0u;  // a geometryLastUsed never written; ResolveGeometrySlot's "no slot"
			SlotTable geometrySlots, pipelineSlots, materialSlots;
			std::vector<std::uint32_t> geometryLastUsed;  // parallel to geometries
			std::vector<const RE::BSGraphics::TriShape*> geometrySlotKey;
			// A layer's geometry slot (SceneStore::ResolveLayerGeometrySlot): its key is the shape's second index list (a
			// BSGraphics::IndexBuffer standing in geometrySlotKey, never read as a TriShape).
			std::vector<std::uint8_t> geometryLayerKey;  // parallel to geometries
			std::vector<std::pair<const RE::BSShaderMaterial*, std::uint32_t>> materialSlotKey;
			// The used sets, in slot order: 64 slots per word.
			std::vector<std::uint64_t> usedMaterialBits, usedPipelineBits;
			static bool BitSet(const std::vector<std::uint64_t>& a_bits, std::size_t a_slot) { return a_slot / 64 < a_bits.size() && ((a_bits[a_slot / 64] >> (a_slot % 64)) & 1); }
			template <class F>
			static void ForEachBit(const std::vector<std::uint64_t>& a_bits, F&& a_function)
			{
				for (std::size_t word = 0; word < a_bits.size(); ++word)
					for (std::uint64_t remaining = a_bits[word]; remaining; remaining &= remaining - 1)
						a_function(static_cast<std::uint32_t>(word * 64 + std::countr_zero(remaining)));
			}
			bool MaterialUsed(std::size_t a_slot) const { return BitSet(usedMaterialBits, a_slot); }
			void UnmarkMaterial(std::uint32_t a_slot) { if (a_slot / 64 < usedMaterialBits.size()) usedMaterialBits[a_slot / 64] &= ~(1ull << (a_slot % 64)); }
			void UnmarkPipeline(std::uint32_t a_slot) { if (a_slot / 64 < usedPipelineBits.size()) usedPipelineBits[a_slot / 64] &= ~(1ull << (a_slot % 64)); }
			void ClearUsed()
			{
				std::fill(usedMaterialBits.begin(), usedMaterialBits.end(), 0);
				std::fill(usedPipelineBits.begin(), usedPipelineBits.end(), 0);
			}
			void MarkMaterialUsed(std::uint32_t a_slot);
			void MarkPipelineUsed(std::uint32_t a_slot);
			void SetMaterialUsed(std::uint32_t a_slot, bool a_used)
			{
				if (a_used)
					MarkMaterialUsed(a_slot);
				else if (a_slot / 64 < usedMaterialBits.size())
					usedMaterialBits[a_slot / 64] &= ~(1ull << (a_slot % 64));
			}
			void SetPipelineUsed(std::uint32_t a_slot, bool a_used)
			{
				if (a_used)
					MarkPipelineUsed(a_slot);
				else if (a_slot / 64 < usedPipelineBits.size())
					usedPipelineBits[a_slot / 64] &= ~(1ull << (a_slot % 64));
			}
			bool PipelineUsed(std::size_t a_slot) const { return BitSet(usedPipelineBits, a_slot); }
			bool PipelineAlive(std::size_t a_slot) const { return pipelineSlots.Alive(a_slot); }
			// Each table's columns: a_column(vector, initial value...) for every vector parallel to it.
			template <class F>
			void GeometryColumns(F&& a_column)
			{
				a_column(geometries);
				a_column(geometryImports);
				a_column(geometryLastUsed, kSlotFree);
				a_column(geometrySlotKey);
				a_column(geometryLayerKey, std::uint8_t(0));
			}
			template <class F>
			void PipelineColumns(F&& a_column)
			{
				a_column(pipelines);
				a_column(geometryTemplate);
				a_column(geometryTemplateObject, kNoObjectSlot);
				a_column(pipelineTechnique);
				a_column(permutations);
				a_column(pipelineBindingVersion);
				a_column(pipelineConstants);
			}
			template <class F>
			void MaterialColumns(F&& a_column)
			{
				a_column(materials);
				a_column(materialVersion);
				a_column(materialFrameVersion);
				a_column(materialSlotKey);
			}

			// The object slots: an object keeps its index for as long as it has a record. The per-object arrays above
			// are indexed by slot and persist across walks, a free slot holds FreeObjectRecord(), and a walk sweeps the
			// slots it did not write. Walk parity's reference build (denseWalk) writes them densely instead.
			std::vector<std::uint32_t> objectSeen;  // parallel to objects: the walk (walkSerial) that last wrote the slot
			// New on every tracked insertion, including detach/reattach and pointer reuse.
			std::vector<std::uint64_t> objectIdentity;  // parallel to objects; zero for a free slot
			std::vector<std::uint64_t> objectGroup;  // actor siblings share this ID
			// Parallel to objects: the flags as the scene phase wrote them. The accumulate phase patches the record in
			// place; the delta walk restores the scene half of every slot it patched from here.
			std::vector<std::uint32_t> sceneFlags;
			// A geometry's main-pass layer (SceneStore::Tracked::layerSlot: a multi-index shape's additional property, drawn
			// from its second index list) is an object of its own: layerBase is a layer slot's base slot, layerOf a base slot's
			// layer slot, kNoObjectSlot otherwise. Both parallel to objects.
			std::vector<std::uint32_t> layerBase;
			std::vector<std::uint32_t> layerOf;
			bool IsLayer(std::uint32_t a_slot) const { return a_slot < layerBase.size() && layerBase[a_slot] != kNoObjectSlot; }
			std::vector<std::uint32_t> objectFree;
			std::uint32_t liveObjects = 0;
			/**
			 * @brief What was freed since the last publication, not on its free list yet (step 6e E3; BasicRenderer's rule: nothing
			 * is logically freed while a published version that may name it lives). PublishTables moves it into the retirement chain
			 * (SceneStore::retirement), which gives it back once every publication up to then is gone (SceneStore::RecycleRetired).
			 * The SlotTables keep their own (SlotTable::TakeRetiring).
			 */
			enum RetiredKind : std::uint8_t
			{
				kRetiredObject,
				kRetiredTree,
				kRetiredFadeRoot,
				kRetiredExtras,     // slot: the block's row offset
				kRetiredBones,      // slot: the block's offset, extra: its rows
				kRetiredFaceStream,
				kRetiredMaterialLookup,  // a material slot's bindings to drop (SceneStore::RetireMaterialBinding)
				kRetiredPipelineLookup,  // a pipeline slot's lookups to drop (its mask binding: SceneStore::RetireMaskBinding)
				kRetiredGeometrySlot,    // a SlotTable's slot; extra: its generation when freed
				kRetiredMaterialSlot,
				kRetiredPipelineSlot,
				kRetiredFaceRegion,  // slot: the region's first vertex in the face positions, extra: its vertices (faceRegionFree)
			};
			struct RetiredSlot
			{
				RetiredKind kind = kRetiredObject;
				std::uint32_t slot = 0, extra = 0;
				std::uint32_t epoch = 0;  // slotEpoch when retired: a clear since drops it
			};
			std::vector<RetiredSlot> retiring;
			std::uint32_t objectsRetiring = 0;  // object slots among them, and in the chain (neither live nor free)
			std::uint32_t slotEpoch = 0;        // moves on when every slot is cleared
			void Retire(RetiredKind a_kind, std::uint32_t a_slot, std::uint32_t a_extra = 0) { retiring.push_back({ a_kind, a_slot, a_extra, slotEpoch }); }
			/** @brief An object slot freed: retiring until no publication names it (ResetObject first). */
			void RetireObject(std::uint32_t a_slot)
			{
				Retire(kRetiredObject, a_slot);
				++objectsRetiring;
			}
			/** @brief The live objects: neither free nor retiring. */
			std::uint32_t CountLive() const { return static_cast<std::uint32_t>(objects.size() - objectFree.size() - objectsRetiring); }
			// What the publication made from these tables holds of the retirement chain (published snapshots only).
			std::shared_ptr<const void> retirementHold;
			/** @brief Grows every per-object array to a_count, the new slots free. */
			void GrowObjects(std::size_t a_count);
			/** @brief Returns a slot to the free state (not to the free list). */
			void ResetObject(std::uint32_t a_slot);

			/** @brief Drops the per-frame arrays, and the per-object ones too unless the slots persist. */
			void ClearFrame(bool a_keepObjects);
			/** @brief Drops everything. */
			void Clear();
		};

		struct Stats
		{
			std::uint32_t tracked = 0;
			std::uint32_t objects = 0;
			std::uint32_t geometries = 0;
			std::uint32_t pipelines = 0;
			std::uint32_t materials = 0;
			std::uint32_t shadowMaskPipelines = 0;  // pipelines whose technique binds the shadow mask (not derived yet)
			std::uint32_t categoryNodes = 0;
			std::uint64_t attachedEvents = 0;
			std::uint64_t detachedEvents = 0;
			std::uint64_t detachMoves = 0;  // detached geometries attached again in the same batch (kept, not erased)
			std::uint64_t validationDrops = 0;
			std::array<std::uint32_t, static_cast<std::size_t>(Ineligible::Count)> ineligible{};
			// Decal candidates this frame, by group (Records.h ObjectDecalGroup - 1).
			std::array<std::uint32_t, 3> decals{};
			// CS_DCLF_FADING: objects given bindings with the screen-door fade (AdditionalAlphaMask), summed over
			// frames until the report takes them (TakeFadingDrawn), since fades are brief.
			std::uint32_t fadingDrawn = 0;
			std::uint32_t fadingFrames = 0;  // frames in that sum with at least one
			std::uint32_t skinned = 0;  // skinned candidates this frame, and their palette rows
			std::uint32_t boneRows = 0;
			std::uint32_t projectedUV = 0;  // candidates with the ProjectedUV bit, and terrain ones
			std::uint32_t landBlend = 0;
			// CS_DCLF_DERIVED_CACHE: accumulated objects served from their cached derivation, and under
			// `probe` how many were recomputed and how many disagreed (the gate: 0).
			std::uint32_t derivedHits = 0;
			std::uint32_t derivedChecked = 0;
			std::uint32_t derivedDiffers = 0;
			std::uint32_t slotsSwept = 0;
			std::uint32_t geometriesRefreshed = 0;
			std::uint32_t slotViolations = 0;  // objects whose slots failed CheckObjectSlots (the gate: 0)
			std::uint32_t shadowCasters = 0;  // records the engine would draw into a shadow map
			std::array<std::uint32_t, 16> shadowRejects{};  // by ShadowReject, over the frame's records
			// Objects the engine accumulated that the scene phase had left out of the tables, so the frame
			// cannot draw them. One frame of staleness at most (the verdict is cleared for them); the gate
			// is 0 in steady state.
			std::uint32_t geometriesAlive = 0, pipelinesAlive = 0, materialsAlive = 0;
			// CS_DCLF_CLASSIFY_CACHE: objects served from the cached verdict, and - under `probe` - how
			// many were recomputed and how many disagreed. Zero disagreements is the gate.
			// Objects left native by Ineligible::Technique, by technique id (index 63 = refraction). This
			// is what says which technique to bring into coverage next, instead of guessing.
			std::array<std::uint32_t, 64> techniqueRejects{};
			// CS_DCLF_COVERAGE_PROBE=1: objects left native by NotLightingShader, by property type. The
			// class is the largest one outside coverage and "not a lighting property" says nothing about
			// which shader would have to be brought in.
			std::map<const RE::NiRTTI*, std::uint32_t> propertyRejects;
			// Of those, how many are alpha blended. This is the scoping question for a second shader:
			// blended geometry is not drawn in the pass DCLF owns at all, so covering it would mean a new
			// epoch after the deferred composite rather than another shader inside the existing one.
			std::uint32_t rejectedBlended = 0;
			std::uint32_t rejectedOpaque = 0;
			std::uint32_t rejectedOpaqueAlphaTest = 0;
			std::uint32_t classifyHits = 0;
			std::uint32_t classifyChecked = 0;
			std::uint32_t classifyDiffers = 0;
			// BuildFrame time by part (BuildPart), summed since the last ResetTimes (ms). Only filled under
			// CS_DCLF_PROFILE; with it off the loop makes no clock calls at all.
			std::array<double, static_cast<std::size_t>(BuildPart::Count)> partMs{};
			// The accumulator subset of partMs. It is measured at EarlyPrepass on
			// the render thread, distinct from the preceding scene walk's parts.
			std::array<double, static_cast<std::size_t>(BuildPart::Count)> accumulatePartMs{};
			// The scene tables by sub-zone (ScenePart), summed since the last ResetTimes (ms).
			std::array<double, static_cast<std::size_t>(ScenePart::Count)> scenePartMs{};
			// The current frame's time by part, and the largest frame's since the last ResetTimes (EndSceneFrame).
			std::array<double, static_cast<std::size_t>(ScenePart::Count)> scenePartFrameMs{};
			std::array<double, static_cast<std::size_t>(ScenePart::Count)> scenePartMaxMs{};
			// The light path since the last ResetTimes: entries it kept, by trait (an entry counts under each of its
			// lightTraits bits, SceneStore::kTrait*); the movers it listed for FrameValues (PlacementPlan::movers); the kept skins.
			std::array<std::uint64_t, 8> lightByTrait{};
			std::uint64_t lightPlaced = 0, lightSkins = 0;
			// The move events drained.
			std::uint64_t moveEvents = 0;
			// Walks whose schedule had to look the per-frame set up again (an entry was added or erased).
			std::uint64_t perFrameRelookups = 0;
			// Hidden events drained; actor frame verdicts taken again and left alone; on parity frames, verdicts that
			// changed with no event (the first named).
			std::uint64_t hiddenEvents = 0, verdictsChecked = 0, verdictsSkipped = 0, verdictsMissed = 0;
			std::uint64_t hiddenRetaken = 0;  // statics classified again for a hidden event on their chain (T6b4)
			std::string firstVerdictMissed;
			// CS_DCLF_INPUT_WATCH: per input component (kInputComponentNames), the re-reads that found it changed; per kind
			// of re-read (classify, shading), the re-reads that found any change; the first such change named.
			std::array<std::uint64_t, 12> inputChanged{};
			std::array<std::uint64_t, 2> inputRereads{}, inputRereadsChanged{};
			std::string firstInputChange;
			// The first evaluation round by EvaluateKind: time (ms) and entries, under CS_DCLF_PROFILE.
			std::array<double, static_cast<std::size_t>(EvaluateKind::Count)> evaluateKindMs{};
			std::array<std::uint64_t, static_cast<std::size_t>(EvaluateKind::Count)> evaluateKindCount{};
			// The property-derived descriptor against the accumulated one (Phase 5 readiness): objects
			// compared, and objects the derivation would have left native.
			//
			// The comparison is reported in two halves, because they mean different things. Outside
			// kRuntimePassBits the derivation is meant to be exact, and a difference is a defect. Inside
			// kRuntimePassBits it is guessing at values GetRenderPasses computes from per-frame light and
			// shadow assignment, so a difference there is expected until those bits are derived properly —
			// and how large it is decides whether GetRenderPasses can ever be skipped outright. The earlier
			// counter masked the runtime half out entirely, which made "0 differ" read as a much stronger
			// result than it was.
			std::uint32_t derivationChecked = 0;
			std::uint32_t derivationFadeBits = 0;  // differ only where the engine's LOD fades ran out (the draw fades them)
			// The draw's LOD fades against the engine's, on the registered objects with a fade node (CS_DCLF_DERIVE_PROBE).
			std::uint32_t lodFadeChecked = 0, lodMetricDiffers = 0, lodFadeDiffers = 0;
			std::string lodFadeFirst;
			std::uint32_t derivationDiffers = 0;        // differ outside kRuntimePassBits
			std::uint32_t derivationBits = 0;           // OR of those differing bits
			std::uint32_t derivationRuntimeDiffers = 0;  // differ inside kRuntimePassBits
			std::uint32_t derivationRuntimeBits = 0;     // OR of those differing bits
			// Objects differing per bit position, over the whole descriptor, so a single dominant bit can
			// be told apart from a smear across several.
			std::array<std::uint32_t, 32> derivationBitCounts{};
			std::uint32_t derivationNative = 0;
			// Material evaluations this frame.
			std::uint32_t materialsEvaluated = 0;
			// Which component of a changed record moved: bit 0 VS floats, 1 PS floats, 2 textures,
			// 3 address modes, 4 filter modes, 5 the written-texture mask.
			std::uint32_t materialDiffMask = 0;
			std::uint32_t materialsValidated = 0;     // cache entries re-evaluated and compared this frame
			std::uint32_t materialCacheStale = 0;     // of those, ones that disagreed: must be 0
			std::uint32_t materialCacheEntries = 0;
			std::uint32_t materialCacheEvicted = 0;
			std::uint32_t materialEvictedMember = 0;  // of them, slots a membership resident had been bound to (materialMember)
			// T6b2c step 7, the scene work's last pass (RefreshMaterialRecords): the materials captured since the one before, the slots'
			// records they rewrote, the materials a rewrite could not evaluate yet (asked again), the signature samples.
			std::uint32_t materialWrites = 0;
			std::uint32_t materialsRewritten = 0;
			std::uint32_t materialsHeld = 0;
			std::uint32_t frameMaterialSamples = 0;
		};

		void ResetTimes()
		{
			stats.partMs = stats.accumulatePartMs = {};
			stats.scenePartMs = {};
			stats.scenePartMaxMs = {};
			stats.lightByTrait = {};
			stats.lightPlaced = stats.lightSkins = 0;
			stats.moveEvents = stats.perFrameRelookups = 0;
			stats.hiddenEvents = stats.verdictsChecked = stats.verdictsSkipped = stats.verdictsMissed = 0;
			stats.hiddenRetaken = 0;
			stats.firstVerdictMissed.clear();
			stats.inputChanged = {};
			stats.inputRereads = stats.inputRereadsChanged = {};
			stats.firstInputChange.clear();
			stats.evaluateKindMs = {};
			stats.evaluateKindCount = {};
		}

		/** @brief The placement plan's line for the report (its roots, the move writers' calls), or empty; resets its counters. */
		std::string PlacementReport();
		static constexpr std::array<const char*, 12> kInputComponentNames{ "renderer data", "skin", "skin partition", "small bound", "shader property",
			"property flags", "material", "fade state", "material alpha", "alpha property", "alpha flags", "diffuse view" };
		/** @brief The end of the scene tables zone: folds this frame's part times into the maximums. */
		void EndSceneFrame()
		{
			for (std::size_t i = 0; i < stats.scenePartFrameMs.size(); ++i)
				stats.scenePartMaxMs[i] = std::max(stats.scenePartMaxMs[i], stats.scenePartFrameMs[i]);
			stats.scenePartFrameMs = {};
		}

		/** @brief Whether CS_DCLF_PROFILE is on: the per-part timing in BuildFrame. Read once. */
		static bool ProfileEnabled();
		/** @brief CS_DCLF_TIMELINE (T6b0): the stage timelines, the residue by stage, the latency histograms and the import share. */
		static bool TimelineEnabled();

		static SceneStore& Get();

		/**
		 * @brief T6b3d, render thread (PostPostLoad, once DCLF is installed): the coordinator's pump configured and its wakes allowed
		 * (WakeScenePass, by source). From here the scene passes (RunSceneWork) run on the scene lane whenever a producer wakes the pump: an
		 * engine event pushed, an answer, a catalog, a retirement returned, a frame input posted; never kicked or joined by the frame.
		 */
		void StartScenePump();
		/**
		 * @brief T6b3d, render thread (every frame's start, and the menu's toggle): a_full - DCLF runs (loaded and switched on): the passes
		 * do their whole work; else they apply the events alone, so the tracked set stays current (what Present's apply did). a_inline -
		 * the parities that observe the store from the render thread (CS_DCLF_PERSISTENT_PARITY, CS_DCLF_WALK_PARITY, the capture
		 * parity): the producers' wakes are dropped and the frame's start runs the pass itself (RunScenePassInline).
		 */
		void SetScenePassMode(bool a_full, bool a_inline);
		/**
		 * @brief T6b3d, render thread, the frame's start under the parities (SetScenePassMode's a_inline): one scene pass run here
		 * (SerializedTaskPump::TryRunInline), or, while a pass the lane started still runs (a wake from before the switch), after it ends
		 * (a wait, the parities' alone), so the render thread's observers (the mirror's leases, the validation slice, the extras and LOD
		 * parities) keep their placement. Then the notes it held for PrimaryCull, delivered.
		 */
		void RunScenePassInline();
		/**
		 * @brief T6b3d, render thread at Present under the parities, when no frame's start ran since the last Present (menus, load
		 * screens): one inline pass applying the events alone (Present's apply before T6b3d), so the queues do not wait for a frame.
		 */
		void ApplyEventsInline();
		/** @brief Whether a scene pass runs now (the pump not idle): a frame-side fallback that reads what the pass writes refuses then. */
		bool SceneTaskInFlight() const;
		/**
		 * @brief T6b3d, render thread, at Present and the frame's start (in menus too): whether a load screen is up. Nothing may walk the
		 * scene graph while one is: the first time a load is seen a marker is posted (the coordinator drops what the load invalidates:
		 * ApplyLoading; its passes drain the queues into the mirror's carry, the tracking discarding them), the set is withdrawn
		 * (PassCapture::PublishSet(nullptr)), and the first frame after the load catches every switch up and captures every category
		 * node. Also where the switch events' thread is recorded (PushSwitch).
		 */
		bool NoteLoadingScreen();
		/**
		 * @brief T6b3d, render thread, the frame's start, not while a load screen is up: the coordinator's requests served and the render
		 * thread's captures posted - the switch catch-ups (engine writes: the attached roots and switch events the passes drained, the world
		 * after a load), the category capture (when its signature moved or a pass saw a detach), the mirror's capture requests (captured
		 * with their chains, onto the tracker's stack, mirror-only), the mirror parity's probe (CS_DCLF_MIRROR_PARITY), tree LOD's mirror.
		 * a_running false (DCLF does not run: no frame start of its own): the switch catch-ups and tree LOD's mirror alone, as Present's
		 * ingestion served them before.
		 */
		void ServeFrameRequests(bool a_running);
		/** @brief Coordinator reports' interval (DrawcallLimitFix::kReportInterval's): the coordinator reports as its frame crosses it. */
		static constexpr std::uint32_t kReportInterval = 300;
		/**
		 * @brief Render thread at Present, the read window closed: the material references and applied batches the coordinator let go of
		 * (T6b3a: pushed where it let go of them; its engine references go to EngineReleases), released (dropping the last one runs the
		 * engine's destructors, which belong on its main thread).
		 */
		void ReleaseHandedBack();
		/**
		 * @brief Hooks the engine's writers the delta walk takes events from: BSFadeNode::currentFade's, the shader
		 * properties' flags and materials, Havok's node transforms and the controllers' targets.
		 */
		static void InstallSceneEvents();
		/**
		 * @brief Hooks the engine's move writers (MoveEvents in SceneStore/Internal.h): the animation and 3D-position
		 * updates of actors and references, ragdolls, and the cells' update passes. The light path then places a mover
		 * only when its reference (or its category node) had one this frame or the last.
		 */
		static void InstallMoveEvents();
		static bool MoveEventsLive();
		/** @brief The hidden-bit stores are patched (HiddenStores.cpp). */
		static bool HiddenEventsLive();
		/**
		 * @brief The switch-selection events are installed (dclf-cull-job-elimination.md, "Phase 3"; false only when the
		 * engine's writers are not the expected code): every writer of an NiSwitchNode's selected index and of its children
		 * is hooked, the newly selected child is brought up to date when the event is applied (CatchUpSwitch), and an
		 * entry under a switch is no longer evaluated every frame for its selection.
		 */
		static bool SwitchEventsLive();
		/**
		 * @brief NiSwitchNode::OnVisible's catch-up (AE 0x140d29700), outside the cull: when the selected child has not
		 * been updated since the switch's last update pass (childRevID[index] != revID), its revision is marked current
		 * and it takes UpdateDownwardPass with the switch's saved time. True when it ran. Render thread, the node in the
		 * scene: engine code that writes the subtree's transforms, so never the scene work's (step 6e F1, CatchUpSwitches).
		 */
		static bool CatchUpSwitch(RE::NiSwitchNode& a_switch);
		/**
		 * @brief Render thread, at ingestion (step 6e F1): the catch-ups the scene work used to run itself (ApplySwitchEvents,
		 * AddSubtree), for what the drain brought - every switch event whose selection changed and every switch under an attached
		 * subtree, each in the world (reached from Main::WorldRootNode: a subtree a loader still assembles is not walked) - and,
		 * on the first ingestion after a load, every switch in the world (the rescan's AddSubtree did that). Before the culls
		 * when called from the frame's start.
		 */
		void CatchUpSwitches(std::span<RE::NiAVObject* const> a_attached, std::span<const Scene::SwitchEvent> a_switches);
		/**
		 * @brief PrimaryCull, before the list jobs (render thread): the switch nodes whose selection the walks applied since
		 * the last call, as keys (never dereferenced). True when the caller must read every switch again instead (a full
		 * walk, dropped events, or more changes than the list keeps).
		 */
		bool TakeSwitchChanges(std::vector<const RE::NiAVObject*>& a_out);

		// Scene members' records (drawcall-limit-fix.md, "Scene membership"): the accumulated half of a member's record is
		// patched once, from its membership pass (AccumulatedPass::resident, BindByMembership), and kept across frames: it is
		// not restored at the next walk, and only its slots are kept alive each frame (KeepResidentsAlive). A rewrite keeps it
		// while its binding stands; a release or a failed patch ends it.
		/** @brief Render thread: ends every membership now (the frame globals a membership pass reads changed). */
		void EndAllResidency();
		/** @brief CS_DCLF_RESIDENT_PARITY=1: every 60 frames, each resident's pass built again and its record, against its patch. */
		static bool ResidentParityEnabled();
		struct ResidentStats
		{
			std::uint64_t joined = 0, failed = 0, rewritten = 0, released = 0, frames = 0, resident = 0;
			std::uint64_t membershipQueued = 0;  // records BindByMembership handed a pass
			std::uint64_t materialWaits = 0, materialsServed = 0;  // joins that waited for a material capture, captures the render thread served
			std::uint64_t materialsStale = 0;  // T6b1b: requests whose property had another material at the serve (the swap's event not applied yet)
			std::uint64_t membershipKept = 0;    // members written again whose binding stands
			std::uint64_t layerUnpaired = 0;     // a base or a layer that joined without the other, and left again
			std::array<std::uint64_t, 4> failedBy{};  // (unused), no record, a frame verdict, material or extras
			std::uint64_t parityChecks = 0, parityChecked = 0, parityPass = 0, parityRecord = 0;
			std::uint64_t parityPending = 0;  // residents whose fade root is fading: the feedback's next decode ends them
			std::uint64_t registeredUnbound = 0;  // main-pass registrations of eligible objects DCLF has not bound (DrainCapture)
			std::uint64_t lightingShaderDiffers = 0;  // T6 parity: a registered Lighting pass whose shader is not the engine instance DCLF uses
			std::string registeredUnboundFirst;
			// The registration parity (T6b2c step 8, CS_DCLF_PERSISTENT_PARITY): the frames whose registrations were observed, and the
			// main camera's registrations checked (CheckRegistrations). 0 frames: not observed (the normal path reads none).
			std::uint64_t registrationFrames = 0, registrationsChecked = 0;
		};
		ResidentStats TakeResidentStats() { return std::exchange(residentStats, {}); }
		/**
		 * @brief The joins' frame inputs (T6b2c step 8), since the last call: LightLimitFix's room map and the pipeline frame
		 * the render thread posted (latest wins) and the scene lane took, and the PerGeometry blocks the coordinator made from the
		 * pipeline frame (MakeNewPipelineConstants) or could not make yet (no frame taken: the slot waited).
		 */
		struct AccumulateInputStats
		{
			std::uint64_t roomMapsPosted = 0, roomMapsTaken = 0, pipelineFramesPosted = 0, pipelineFramesTaken = 0;
			std::uint64_t pipelineBlocksMade = 0, pipelineBlocksWaited = 0;
		};
		AccumulateInputStats TakeAccumulateInputStats();

		/**
		 * @brief Whether a load screen is up, i.e. the scene graph is being rebuilt under us.
		 *
		 * Nothing may walk the scene graph while this holds, and frame counters meant to be comparable
		 * between runs should not advance across it.
		 */
		static bool IsLoadingScreenUp();

		/**
		 * @brief Which half of the frame's tables to build.
		 *
		 * The shadow views are drawn before the main camera's passes exist (engine notes: shadow maps),
		 * so the tables are built in two halves. Scene runs before the shadow maps and holds everything
		 * that does not depend on the accumulator - the object records a shadow epoch reads. Accumulate
		 * (the joins) patches those records with what the main pass draws them with: from the mirror and the
		 * membership passes, not the registrations, so since T6b3c it follows Scene in the same scene pass.
		 * Object indices are fixed from Scene onwards.
		 */
		enum class Phase : std::uint32_t
		{
			Scene,
			Accumulate
		};

		/** @brief Rebuilds one half of the CPU tables from the tracked set. */
		void BuildFrame(Phase a_phase);

		/**
		 * @brief The `[DCLF] scene delta`, change log and scene parity report lines since the last call, or empty. T6b3d: the coordinator's
		 * (ReportCoordinator, in its pass); the render thread's own lines are FrameSceneReport's.
		 */
		std::string SceneReport();
		/**
		 * @brief T6b3d, render thread (the frame's report): the report lines of what the render thread keeps - the material parity, the
		 * pipeline constants and templates, the shading parity, the extras parity, tree LOD's mirror, the mirror watch, the frame capture.
		 */
		std::string FrameSceneReport();

		/**
		 * @brief Latches the main camera's accumulator, from a point in the frame where it is identifiable.
		 *
		 * Call where `globals::game::currentAccumulator` is set - it is a *currently rendering* pointer, so
		 * it is null before the main pass begins even though the accumulator itself has been fully built
		 * since before the shadow maps. BuildFrame runs earlier than that now and reads the latch instead.
		 * A change of accumulator is logged once; the pointer is stable in practice.
		 */
		void LatchAccumulator();

		/**
		 * @brief Re-evaluates the per-pipeline per-frame constants against the main camera's state.
		 *
		 * BuildFrame runs at EarlyPrepass so both DCLF epochs share a table generation, but there the
		 * renderer's shadow state still belongs to the shadow-map camera just drawn. Most of what
		 * SetupGeometry writes is per frame, and one of those - EyePosition - is relative to posAdjust,
		 * so evaluating it that early produced the shadow camera's eye and capture parity failed on that
		 * one variable across every draw. The object, geometry and material tables are camera-independent
		 * and stay where they are built; only this is deferred.
		 *
		 * Call from Prepass. The Z-prepass epoch runs in between and so uses the previous frame's values
		 * for these - harmless only for what the vertex position does not depend on: World is patched per
		 * object at epoch time with that epoch's own eye. Terrain LOD's HighDetailRange moves vertices: it is a technique
		 * row's (the coordinator's, T6b2c), so both epochs of a frame draw the one value of the frame's tables.
		 *
		 * The technique rows are not evaluated here (RefreshTechniqueRows); the frame's fog is, from the frame's sample.
		 */
		void RefreshFrameConstants();

		/**
		 * @brief The four textures the engine binds for a ProjectedUV draw (pixel slots 3, 8, 10 and 11:
		 * the projected diffuse, normal and detail maps and the projection noise), as SetupGeometry left
		 * them at a native draw. They are globals of the engine, changed only by the ReloadProjectedUVTextures
		 * console command, so one capture stands; it is refreshed by every native projected draw seen. A capture whose views changed is
		 * posted to the scene work with a reference on each (SharedBindings::projectedPosted), which asks for their bindings.
		 */
		struct ProjectedTextures
		{
			static constexpr std::array<std::uint32_t, 4> kSlots{ 3, 8, 10, 11 };
			std::array<ID3D11ShaderResourceView*, 4> views{};
			bool valid = false;
		};
		void NoteProjectedTextures();
		const ProjectedTextures& GetProjectedTextures() const { return projectedTextures; }

		/**
		 * @brief The render flags the native main pass passes to SetupGeometry: 0x41, or 0x45 for blended decals (engine
		 * notes). SetupGeometry reads only bits 0x2 (SSRParams.w zeroed), 0x8 (with a shadow-mask global) and 0x10
		 * (PreviousWorld from the current world), none of which the main pass sets, so 0x41 stands for both.
		 */
		static constexpr std::uint32_t kMainPassRenderFlags = 0x41;


		/**
		 * @brief The tables as the frame reads them (step 6c): the installed publication's (immutable), the newest taken while none is
		 * installed, empty before the first (T6b3a: never the coordinator's). Render thread and the frame's builds.
		 */
		const Tables& GetTables() const { return FrameView(); }
		/** @brief The frame's per-frame engine values (FrameTables): render thread, and the frame's builds kicked after its writes. */
		const FrameTables& GetFrameTables() const { return frameTables; }
		/** @brief Render thread, the frame's start: the frame's values sized and keyed to the frame's tables, before any build. */
		void SyncFrameTables()
		{
			const Tables& view = FrameView();
			frameTables.SyncPipelines(view.pipelines, view.pipelineBindingVersion, frameTablesGeneration);
		}
		/**
		 * @brief What the snapshot builder reserves a scene revision's capacities for and assembles it from (IndirectDraws, Impl::
		 * MakeRevision; T6b3b): a publication's tables and lookups (immutable), with its counts. The snapshot carries the revision with the
		 * publication it was made from. Its Reserve* walk the tables' logs and columns (the draw bound, the index pool), so the request
		 * carries the tables, not only their counts.
		 */
		struct RevisionRequest
		{
			std::uint64_t publication = 0;  // the publication's sequence
			// The scene frame of the commit it applies and the coordinator's pass that published it (T6b3b): what the snapshot builder's
			// revision counts its frame by (MakeRevision's a_frame).
			std::uint32_t commitFrame = 0;
			std::uint64_t passSerial = 0;
			std::uint32_t tablesGeneration = 0;
			std::shared_ptr<const Tables> tables;
			std::shared_ptr<const Lookups> lookups;
			// The catalog those lookups were resolved from: the revision's pipeline sets (named explicitly by MakeRevision), so a revision is
			// its publication's alone.
			std::shared_ptr<const PipelineCatalog> catalog;
			// The counts the latch and the shadow reservations size by: the pipeline slots, and the shadow key slots the lookups resolved
			// with the keys the tables' casters and occluders use.
			std::uint32_t pipelines = 0;
			std::size_t shadowSlotKeys = 0, shadowKeys = 0;
		};
		/**
		 * @brief A publication of the scene (step 6e E3; T6b3a: complete and immutable): everything a frame reads of the scene, made by the
		 * coordinator (PublishScene) and handed to the snapshot builder (IndirectDraws::PostSnapshotWork, T6b3b), which posts it with its
		 * draws and its revision as one snapshot; the render thread adopts the newest snapshot whole (HandOverAtFrameStart installs its
		 * publication). The claims' changes and the frame inputs each publication's pass made (the placement plan, the shading names, the
		 * seed requests, the switches, the retired imports) travel on the publication log (PublicationDeltas), so a publication the builder
		 * skipped loses none of them.
		 */
		struct ScenePublication
		{
			std::uint64_t sequence = 0;
			std::uint32_t commitFrame = 0;        // the scene frame of the commit it applies (CommitSet's; stats and the install delay)
			std::uint32_t togglesGeneration = 0;  // the toggles that commit was made under (FrameInputs): the toggle reinstall waits for one
			// T6b5: the LOD gates' forced releases its pass's events were complete for (FrameInputs::lodForcedGeneration): an older one may
			// claim the outgoing blocks a forced release detached, and the frames withdraw until one built after it is installed.
			std::uint64_t lodForcedGeneration = 0;
			std::uint32_t tablesGeneration = 0;   // the tables' generation (GetTablesGeneration while it is the frame's)
			std::shared_ptr<const Tables> tables;
			std::shared_ptr<const SetSnapshot> claims;
			std::array<std::uint32_t, 3> lackingCount{};
			// T6b2c step 5: the lookups the draws were built with, parallel to the tables, and the catalog they were resolved from, whose
			// set versions the frames that install it bind (DrawPipelines::HoldCatalog; a revision names its request's explicitly).
			std::shared_ptr<const Lookups> lookups;
			std::shared_ptr<const PipelineCatalog> catalog;
			// The sun and light candidates as its pass left them (UpdateSunCandidates, UpdateLightCandidates), with their generations.
			std::shared_ptr<const SunCandidates> sunCandidates, lightCandidates;
			std::uint32_t sunGeneration = 0, lightGeneration = 0;
			std::uint64_t lightEntriesAppeared = 0;
			// The category nodes as its pass left them (immutable, copied again only when the set changed, a cell attach or detach): what the
			// point lights' culls ask of the frame (IsCategoryNode), never the coordinator's set. The light entries change with every
			// tracked root, so they travel as changes on the publication log instead (PublicationDeltas::lightEntries).
			std::shared_ptr<const ankerl::unordered_dense::set<RE::NiNode*>> categoryNodes;
			// The scene revision's request (the snapshot builder's, T6b3b).
			RevisionRequest revision;
			// T6b3d: the oldest event it is the first publication of (SceneTracker::Event::stampNs, stampFrame; 0: none), for the
			// event-to-adoption latency (IndirectDraws' snapshot report).
			std::uint64_t eventNs = 0;
			std::uint32_t eventFrame = 0;
		};
		/**
		 * @brief Render thread, EarlyPrepass (T6b2c step 8): the joins' frame inputs posted (LightLimitFix's room map, latest wins:
		 * PostRoomMap; the capture's drain, PostRegistrationDrain) and the parity's observers of the frame's registrations (the members'
		 * light masks, PrimaryCull::CheckLightMasks). No scene work: the next scene pass takes the inputs (TakeAccumulateInputs, T6b3c).
		 */
		void PostAccumulateInputs();
		/**
		 * @brief Render thread, Prepass (RefreshFrameConstants): the frame's pipeline sample with a sun, posted latest-wins for the
		 * coordinator's new pipelines' PerGeometry blocks (MakeNewPipelineConstants).
		 */
		void PostPipelineFrame(const GeometryPort::PipelineFrame& a_frame);
		/** @brief Render thread, after HandOverAtFrameStart and the coverage decision: the installed publication's claims made the frame's. */
		void InstallClaims();
		/** @brief Whether a publication is installed (none: the frame has no claims, WithdrawSet). */
		bool HasInstalled() const { return installed != nullptr; }
		/** @brief The installed publication's sequence, 0 without one. */
		std::uint64_t InstalledSequence() const { return installed ? installed->sequence : 0u; }
		/** @brief The toggles generation the installed publication's commit was made under (the toggle reinstall), 0 without one. */
		std::uint32_t InstalledToggles() const { return installed ? installed->togglesGeneration : 0u; }
		/** @brief T6b5: the LOD gates' forced-release generation the installed publication was built for, and the frame's (BeginFrame's read). */
		std::uint64_t InstalledLodForced() const { return installed ? installed->lodForcedGeneration : 0u; }
		std::uint64_t FrameLodForced() const { return frameLodForced; }
		/**
		 * @brief T6b5, render thread, the frame's start after InstallClaims or WithdrawSet (before the culls): the LOD gates the installed
		 * publications flipped, released (LodGates::Flip: the incoming blocks shown and the outgoing hidden in the frame whose claims are
		 * post-swap; a withdrawn frame's too, every later publication being post-swap), then the coordinator woken for their outcomes. Under
		 * the set or persistent parity, the installed claims checked against each flip (<- GATE HOLE, <- GATE COVERAGE).
		 */
		void ReleaseGates();
		struct PublicationStats
		{
			// Publications installed with an adopted snapshot (and those the builder skipped past), frames whose snapshot brought no newer one.
			std::uint64_t installed = 0, kept = 0, skipped = 0;
			// T6b0: the frames from a publication's commit to its installation, by the geometries it joined (kAgeBuckets).
			std::array<std::uint64_t, 8> installDelay{};
		};
		PublicationStats TakePublicationStats() { return std::exchange(publicationStats, PublicationStats{}); }
		/**
		 * @brief Render thread, the frame's start (T6b3b: no coordinator state read; the join is not needed): a_adopted, the publication of
		 * the snapshot the frame adopted (IndirectDraws::AdoptSnapshot; null: none yet), installed when newer than the installed one - the
		 * publication log walked up to it (the frame inputs: the placement plans, shading names, seed requests, switches applied, retired
		 * imports, the actors' wetness membership; then the claims' changes), its tables, lookups and catalog the frame's (DrawPipelines::
		 * HoldCatalog), the replaced one back to the coordinator - and the sun and light candidates the installed one's, and the held
		 * PrimaryCull notes delivered. What goes to the coordinator (the fade roots the depth commit holds, PrimaryCull's fade ownership and
		 * reseed, the pipeline blocks) is posted where it is made and taken at the coordinator's next pass (TakeCoordinatorInputs).
		 */
		void HandOverAtFrameStart(std::shared_ptr<const void> a_adopted);
		/** @brief Coordinator state read by the frame while the scene work ran (GuardFrameAccess), since the last call: count, first name. */
		struct ConstantsPostStats
		{
			std::uint64_t pipelinesPosted = 0, pipelinesApplied = 0, stale = 0;
			// The coordinator's technique rows (T6b2c): evaluations (new rows, the frame inputs moved), the rows written, the inputs' moves.
			std::uint64_t techniquesEvaluated = 0, techniquesWritten = 0, techniqueInputsMoved = 0;
		};
		ConstantsPostStats TakeConstantsPostStats() { return std::exchange(constantsPostStats, ConstantsPostStats{}); }
		std::pair<std::uint64_t, const char*> TakeFrameAccessViolations() { return { frameAccessViolations.exchange(0), frameAccessFirst.exchange(nullptr) }; }
		const std::shared_ptr<const Tables>& AcceptedTables() const { return acceptedTables; }
		struct TablesPublication
		{
			std::uint64_t published = 0, reused = 0, made = 0;
			double ms = 0.0, maxMs = 0.0;
			// 6d, replay: the material records written again (their version moved) against the slots, and the time the rest took
			// (copied whole still); the replay's parity (CS_DCLF_PERSISTENT_PARITY, every 60 publications) against the tables.
			std::uint64_t materialsReplayed = 0, materialSlots = 0;
			double restMs = 0.0;
			std::uint64_t parityChecks = 0, parityMaterials = 0, parityDiffer = 0;
			// The object slots the change log named (copied by slot) against the slots; publications copied whole (a snapshot just
			// made, or one the log no longer reaches); the parity's slots and logs compared.
			std::uint64_t objectsReplayed = 0, objectSlots = 0, wholeCopies = 0, parityObjects = 0, parityObjectsDiffer = 0, parityLogsDiffer = 0;
			std::uint64_t geometriesReplayed = 0, geometrySlots = 0, geometryWholeCopies = 0, parityGeometries = 0, parityGeometriesDiffer = 0;
			std::uint64_t treesKept = 0, fadeRootsKept = 0, parityFamiliesDiffer = 0;  // publications that kept the family (its stamp stood)
			std::size_t pool = 0;
			// T6b2c step 5, the lookups each publication carries (PublishLookups): copied (a new Snapshot), or the last one shared again
			// (unchanged since); the chunks written again since the last copy (SharedChunks, each a chunk's copy), and the copies' time.
			std::uint64_t lookupsCopied = 0, lookupsShared = 0, lookupsChunks = 0;
			double lookupsMs = 0.0, lookupsMaxMs = 0.0;
		};
		/** @brief The coordinator (T6b3d: its report): the retirement chain's counts since the last call, and what waits in it now. */
		std::string TakeRetirementReport()
		{
			const auto s = std::exchange(retirementStats, RetirementStats{});
			return fmt::format("{} retired, {} recycled in {} batches, {} dropped by a clear; {} object slots retiring", s.retired, s.recycled, s.batches, s.dropped,
				tables.objectsRetiring);
		}
		TablesPublication TakeTablesPublication() { return std::exchange(tablesPublication, TablesPublication{ .pool = tablesPool.size() }); }
		/**
		 * @brief Render thread, the depth commit: its fade root rows hold a_held (Tables::fadeRootsJournal's version), posted latest-wins
		 * (T6b3a): at its next pass the coordinator's journal forgets what it has, and the next write opens a new version.
		 */
		void FadeRootsSent(std::uint64_t a_held)
		{
			fadeRootsSentPosted.store(a_held, std::memory_order_release);
			// T6b3d: no wake: the frame's next pass takes it.
		}
		/**
		 * @brief The frame's globals the engine's evaluations give (RefreshFrameConstants, at Prepass): no column of the tables, a
		 * capture of the frame each commit latches into its frame blocks.
		 */
		struct FrameCapture
		{
			// The frame's lighting (DirLightDirection, DirLightColor, DirectionalAmbient, AmbientSpecularTintAndFresnelPower):
			// FrameLighting's rows (LightingConstants.h), which the DCLF_BINDLESS draws read from their own frame block (PS b13)
			// instead of each pipeline's PerGeometry block. Merged from the frame's evaluations over the last frame's (a component
			// none of them writes keeps its value); the pipelines' geometryConstants still hold the values, for the constant-buffer
			// path and the parity checks, but a change of them alone versions no pipeline.
			std::array<float, 24> lighting{};
			// The frame's fog (FrameFog, LightingConstants.h): what the DCLF_BINDLESS vertex stage reads (VS b13) instead of each
			// technique row's, which keep the fog they were made with.
			std::array<float, 12> fog{};
			// The character light's noise this frame (MaterialSources::FrameCharacterLightView, from the frame's FrameGlobals sample at
			// Prepass): the frame record's kCharacterLightRegister. The last one kept while the sample has none (a cell loading).
			ID3D11ShaderResourceView* characterLightView = nullptr;
		};
		/** @brief Render thread: the frame's capture (latched by every commit). */
		const FrameCapture& GetFrameCapture() const { return frameCapture; }
		const Stats& GetStats() const { return stats; }
		/** @brief The screen-door fading objects given bindings since the last call, and in how many frames. */
		std::pair<std::uint32_t, std::uint32_t> TakeFadingDrawn()
		{
			const std::pair result{ stats.fadingDrawn, stats.fadingFrames };
			stats.fadingDrawn = stats.fadingFrames = 0;
			return result;
		}
		/**
		 * @brief The frame number: the render thread's (BeginFrame), or on a scene work thread the coordinator's (T6b3a: sceneFrame, the
		 * frame inputs' it last took; T6b3d: behind the render thread's by the pump's lag, and the same for every pass of a burst). Inline
		 * scene work runs with both equal.
		 */
		std::uint32_t GetFrame() const { return sceneWorkThread ? sceneFrame : frame; }
		/** @brief The coordinator's passes since the start (T6b3a): what a once-a-pass rule counts once the passes leave the frames. */
		std::uint64_t PassSerial() const { return passSerial; }
		/**
		 * @brief T6b3d: the pass as a 32-bit stamp for the per-entry "this pass" marks (Tracked::candidateFrame, hiddenEventFrame,
		 * Tables::geometryLastUsed): never 0 ("never") nor Tables::kSlotFree, unique over 2^32 - 2 passes.
		 */
		std::uint32_t PassStamp() const { return static_cast<std::uint32_t>(passSerial % 0xFFFFFFFEull) + 1u; }
		/** @brief T6b3d: a once-a-pass parity's turn (ParityDue by passes: every 60th pass, with a_offset as ParityDue's). */
		bool PassParityDue(std::uint32_t a_offset = 0) const { return passSerial % 60 == a_offset; }
		/**
		 * @brief The PerMaterial float positions refreshed every frame in the records without a new version
		 * (MaterialSources): the build repacks them into a reused group. PS: the shader object's and the
		 * engine globals' (RefreshMaterialSignatures); VS: TexcoordOffset (RefreshMaterialTransforms).
		 */
		const std::vector<std::uint32_t>& GetMaterialPatchedFloats() const;
		const std::vector<std::uint32_t>& GetMaterialPatchedVSFloats() const;
		/**
		 * @brief The frame's tables' generation (T6b3a: the publication's they are, 0 before the first): the coordinator's moves whenever
		 * the slot tables are reset or every cached verdict is dropped (InvalidateVerdicts), and a publication carries the one it was made
		 * at. Render thread; the coordinator reads its own (tablesGeneration).
		 */
		std::uint32_t GetTablesGeneration() const { return frameTablesGeneration; }
		/** @brief The sun entries DCLF can take out of the cascade culls (UpdateSunCandidates), and their generation now. */
		std::shared_ptr<const SunCandidates> GetSunCandidates() const { return frameSunCandidates; }
		std::uint32_t GetSunCandidatesGeneration() const { return frameSunGeneration; }
		/**
		 * @brief The entries DCLF can take out of the point lights' culls (UpdateLightCandidates), and their generation now:
		 * the light entries (LightEntryOf: an actor's root, a reference's, a terrain block's multibound node) whose every
		 * tracked geometry is a table object (the paraboloid exclusion then asks that it be drawn in the mode:
		 * BuildSunExclusion), or one the light's registration takes nothing from (LightEntryAllows). The lights' bits of their
		 * geometries are LocalLightCull's.
		 */
		std::shared_ptr<const SunCandidates> GetLightCandidates() const { return frameLightCandidates; }
		/** @brief The coordinator: the candidates as its work last made them (the builds ahead take them with the publication). */
		std::shared_ptr<const SunCandidates> CoordinatorSunCandidates() const { return sunCandidates; }
		std::shared_ptr<const SunCandidates> CoordinatorLightCandidates() const { return lightCandidates; }
		/**
		 * @brief Render thread: a count of the light entries that gained their first tracked geometry. An entry the point
		 * lights' filter cut for holding none (LocalLightCull) is judged again when it moves.
		 */
		std::uint64_t GetLightEntriesAppeared() const { return frameLightEntriesAppeared; }
		/**
		 * @brief The frame (the point lights' culls): whether a node is a light entry (LightDependentsOf's rule: a key with tracked
		 * geometries, not a category node). T6b3a: the render thread's set, kept from the publication log's changes up to the newest
		 * publication taken (written only at the frame's start); never the coordinator's.
		 */
		bool IsLightEntry(const RE::NiAVObject* a_node) const
		{
			return frameLightEntrySet.contains(a_node) && !IsCategoryNode(a_node);
		}
		/** @brief The frame: whether a node is one of the cells' category nodes DCLF tracks (RefreshCategoryNodes; the newest publication's set). */
		bool IsCategoryNode(const RE::NiAVObject* a_node) const
		{
			return frameCategoryNodes && frameCategoryNodes->contains(static_cast<RE::NiNode*>(const_cast<RE::NiAVObject*>(a_node)));
		}
		std::uint32_t GetLightCandidatesGeneration() const { return frameLightGeneration; }
		/**
		 * @brief Whether DCLF's native variant of Skylighting's occlusion map is on: the toggle (CS_DCLF_SKYLIGHT) and
		 * the Skylighting feature loaded. The objects' sky techniques are classified only then.
		 */
		static bool SkyOcclusionEnabled() { return OcclusionEnabled(kOcclusionSky); }
		/** @brief Whether DCLF draws the occlusion view (kOcclusion*): its toggle, and Skylighting, whose hook drives both maps. */
		static bool OcclusionEnabled(std::uint32_t a_view);
		/**
		 * @brief The frame's lookups (Lookups.h), read by its epochs and diagnostics: the installed publication's, the newest published
		 * while none is installed (as the frame's tables), empty before the first (T6b2c step 5). Made by the scene lane, immutable.
		 */
		const Lookups& GetLookups() const { return frameLookups ? *frameLookups : EmptyLookups(); }
		static const Lookups& EmptyLookups()
		{
			static const Lookups empty;
			return empty;
		}

		/**
		 * @brief An NiSwitchNode's own fields, read at their AE offsets (NiSwitchNode::OnVisible, AE 140d29700):
		 * CommonLib declares them after NiNode, whose declared size in a multi-runtime build is VR's, so its members
		 * read the wrong memory.
		 */
		struct SwitchState
		{
			std::uint16_t flags = 0;  // bit 0: the update pass updates only the selected child
			std::int32_t index = -1;
			std::uint32_t revID = 0;
			const std::uint32_t* childRevID = nullptr;
			std::uint16_t childRevCapacity = 0;
		};
		static SwitchState ReadSwitch(const RE::NiSwitchNode& a_switch);
		/**
		 * @brief The child a switch selects: children[index], null when the index or its revision is out of range
		 * (bounded by capacity, not size: a Gamebryo array is indexed by slot). Current when childRevID[index] ==
		 * revID; otherwise NiSwitchNode::OnVisible brings the child up to date before culling it.
		 */
		struct SwitchSelection
		{
			RE::NiAVObject* child = nullptr;
			std::uint16_t index = 0;
			bool current = false;
		};
		static SwitchSelection SelectionOf(const RE::NiSwitchNode& a_switch, const SwitchState& a_state);
		// Whether the switch node draws a_child (its direct child on the leaf's path) this frame.
		static bool SwitchSelects(const RE::NiSwitchNode& a_switch, const RE::NiAVObject* a_child);

		/**
		 * @brief The row of the engine's skin-partition LOD table a pass draws with (NiSkinPartition::Unk_25,
		 * AE 140d43a10): LODMode.index + LODMode.singleLevel * 4. For a geometry, the row both of
		 * GetRenderPasses and GetRenderPasses_ShadowMapOrMask give it: a kMeshLOD geometry's fade node LOD
		 * level (+0x152 & 0xF), cumulative; everything else level 3, every LOD byte.
		 */
		static std::uint32_t LodRowOf(const RE::BSGeometry& a_geometry, const RE::BSShaderProperty* a_property);
		/** @brief LodRowOf from the records (T6b1b): the geometry's kMeshLOD, a_property's fade node's LOD level. */
		static std::uint32_t LodRowOf(const SceneCapture::LeafView& a_leaf, const SceneCapture::PropertyRecord* a_property);
		static std::uint32_t LodRowOf(const RE::BSRenderPass& a_pass);
		/**
		 * @brief Which partitions of the skin the engine draws with this LOD row (bit i = partition i):
		 * BSDismemberSkinInstance::Unk_25 (AE 140d31f40) first skips a partition whose flag (Data byte 0) is
		 * clear, then NiSkinPartition::Unk_25 applies the table. 0 when it draws none.
		 */
		static std::uint32_t SkinPartitionMask(const RE::NiSkinInstance& a_skin, std::uint32_t a_lodRow);
		/** @brief SkinPartitionMask from the geometry's record (T6b1b): its partitions' LOD bytes and a dismember skin's shown flags. */
		static std::uint32_t SkinPartitionMask(const SceneCapture::GeometryRecord& a_geometry, std::uint32_t a_lodRow);
		/**
		 * @brief Tables::skinPartitions for a geometry: 0 without a skin partition, kNoPartitions where its LOD row draws
		 * none, the mask for a skin of several partitions, 0 for one of a single drawn partition. The row is 3 (every level's
		 * partitions) whatever the node's level: a LOD skin's level is the draw's (SkinLodPartitionsOf).
		 */
		static std::uint16_t SkinPartitionsOf(const SceneCapture::GeometryRecord& a_geometry);
		/** @brief Tables::skinLodPartitions for a geometry: a kMeshLOD skin's masks per level (byte L: row L), 0 for any other. */
		static std::uint32_t SkinLodPartitionsOf(const SceneCapture::LeafView& a_leaf);

		/** @brief Index into GetTables().objects for this frame, or -1 when the geometry is not drawn by DCLF. */
		std::int32_t FindObject(const RE::BSGeometry* a_geometry) const;
		/** @brief The geometry's layer object (Tracked::layerSlot) in this walk's tables, or -1. */
		std::int32_t FindLayerObject(const RE::BSGeometry* a_geometry) const;
		/** @brief The layer's membership pass this frame (BindByMembership), or null. */
		const AccumulatedPass* FindAccumulatedLayerPass(const RE::BSGeometry* a_geometry) const;
		/**
		 * @brief Whether the object is bound by scene membership (drawn from its record whenever the GPU finds it). Read-only:
		 * the list jobs ask it, while nothing binds (the accumulate phase runs after them).
		 */
		/** @brief Whether the slot is a resident record in the frame's tables (their residentSlot). */
		bool IsMember(std::int32_t a_object) const
		{
			const auto& resident = FrameView().residentSlot;
			return a_object >= 0 && static_cast<std::size_t>(a_object) < resident.size() && resident[a_object] != 0;
		}
		/** @brief Render thread (diagnostics): the shader property a slot draws with (its layer property for a layer's). */
		RE::BSShaderProperty* SlotPropertyOf(std::uint32_t a_slot) const { return SlotProperty(a_slot); }
		/**
		 * @brief Whether a main-pass build can draw with the pipeline slot now: compiled into the set for the slot's current key
		 * (MainBuild::PackPipelines' rule). A pipeline a join has just made is compiled in the background, a frame or more.
		 */
		bool PipelineDrawable(std::uint32_t a_pipeline) const
		{
			return PipelineDrawableIn(FrameView(), GetLookups(), a_pipeline);
		}
		/** @brief A slot's set phases in given tables (the coordinator's: its own), 0 past the column (sized when the set is applied). */
		static std::uint8_t PhasesIn(const Tables& a_tables, std::uint32_t a_slot) { return a_slot < a_tables.setPhases.size() ? a_tables.setPhases[a_slot] : std::uint8_t{ 0 }; }
		/** @brief PipelineDrawable against given tables (the coordinator's commit: its own). */
		static bool PipelineDrawableIn(const Tables& a_tables, const Lookups& a_lookups, std::uint32_t a_pipeline)
		{
			return a_pipeline < a_tables.pipelines.size() && a_pipeline < a_lookups.pipelines.size() && a_lookups.pipelines[a_pipeline].setIndex != Lookups::kNone &&
			       a_lookups.pipelines[a_pipeline].key == a_tables.pipelines[a_pipeline];
		}
		/**
		 * @brief The DCLF set (SceneSet.h): decided once a frame by CommitSet, at the scene phase, after the walk and before the
		 * main camera's cull. Every phase of the frame reads the same decision: the record's kObjectMember, which the builds select
		 * by, and the snapshot the engine's registration hooks withhold by. Nothing between two commits changes it.
		 *
		 * Readiness is part of it: an object joins only once everything its phases draw with is ready (its pipeline compiled, its
		 * material's textures imported), and readiness is taken by events (a lookups generation, the tables' change log), never by
		 * a scan of the scene. A member whose binding is taken again this frame (its record's inputs changed) leaves before the
		 * accumulate phase rebinds it, so no frame's builds draw a member from a binding that is not ready.
		 *
		 * The commit decides the next frame's set; ApplySet makes it the frame's (dclf-async-publication.md, "What each
		 * carries"): the engine's claims are installed at Main::Draw, before the frame's scene work can have run, so the frame
		 * draws exactly the set its claims are - the last commit's. The commit writes setPhasesNext and the snapshot; the
		 * records, Tables::setPhases and the lacking counts change only at ApplySet.
		 */
		void CommitSet();
		/**
		 * @brief The last commit's set made the frame's: its phases into the records (kObjectMember, Tables::setPhases) and the
		 * lacking counts, and its snapshot published as the engine's claims (PassCapture). Main::Draw (BeginSceneFrame), before the
		 * frame's events. A slot freed or given to another geometry since the commit (an event at Present) takes no phase.
		 *
		 * With scene snapshots (T6b3b) the claims applied are the adopted snapshot's publication's, whose revision was made from the same
		 * publication: the frame's claims are always within the revision's set. Until then the last applied claims stand,
		 * and the commits in between merge into one application (setApply, by slot, its geometry the latest commit's).
		 */
		void ApplySet();
		/**
		 * @brief Render thread, BeginSceneFrame, in place of ApplySet: a frame no scene revision covers (none selected yet, or its
		 * recordings are of the graph before a build: IndirectDraws::DecideCoverage) has no claims - every one taken back, as if
		 * every member left (the records, the engine's claims, PrimaryCull's admission, the sun exclusion), so the engine draws
		 * everything and DCLF's epochs draw nothing. The commits go on deciding; the next ApplySet applies the whole set again.
		 */
		void WithdrawSet();
		/** @brief Whether the frame's claims were withdrawn (WithdrawSet) and not applied since. */
		bool SetWithdrawn() const { return setWithdrawn; }
		/** @brief The frame of the last commit (CommitSet): what an ApplySet would apply. */
		std::uint32_t SetCommitFrame() const { return setCommitFrame; }

		/**
		 * @brief The frame's start (render thread): the frame number, the frame's globals captured, the frame inputs posted for the
		 * coordinator (T6b3d: a message that wakes its pump; nothing is kicked or joined).
		 *
		 * Render thread, in order: BeginFrame, the newest snapshot adopted and its publication installed, its claims installed, the
		 * coordinator's requests served and the captures posted (ServeFrameRequests), what the frame's claims need (the filters). The
		 * frame reads only its own state, the immutable publications and the log; what the passes have for other modules is a message:
		 * PrimaryCull's hidden keys and lost members (primaryNotes, delivered at the frame's start), the claims' changes and the frame
		 * inputs (the publication log), the engine references they let go of (released at Present: ReleaseHandedBack, EngineReleases).
		 */
		void BeginFrame();
		/**
		 * @brief The coordinator's pass (T6b3d: the pump's drain, ScenePass, on the scene lane; or inline under the parities): the frame
		 * inputs and the posts taken, the engine's queues drained up to where each stood at the pass's start (CollectEvents) and applied,
		 * the walk, the joins (membership bound, residents kept, decals ordered, the new pipelines' PerGeometry blocks and the bindings),
		 * the set's commit, then the publication (PublishScene: the set applied, the tables and lookups published, the snapshot builder
		 * handed the publication) - held back while the builder has not taken the last one (one publication per builder take, the
		 * passes between coalescing into it). Any number of passes a frame (bursts, or none in menus). While DCLF does not run (not
		 * loaded, switched off) a pass applies the events alone. What it drops of PrimaryCull's is held (primaryNotes). a_task: on the
		 * lane (its placements take read leases).
		 */
		void RunSceneWork(bool a_task);
		/**
		 * @brief EarlyPrepass, render thread: the material tail (T6b2c step 7's): the material captures the last joins asked for
		 * (ServeMaterialRequests, answered for the next scene pass), the material writes and the validation slice. The registrations and
		 * the frame inputs are PostAccumulateInputs' (step 8). Reads nothing of the coordinator's (T6b3c: the scene pass may run).
		 */
		void PrepareAccumulatePhase();
		/** @brief The sun candidates' generation as the frame's claims were installed (BeginFrame): the engine's hooks in the window. */
		std::uint32_t GetPublishedSunGeneration() const { return publishedSunGeneration; }
		/** @brief Claims revoked mid-frame since the last call (RevokeUndrawnClaims): geometries, and the phases taken back. */
		std::pair<std::uint64_t, std::uint64_t> TakeRevokedClaims() { return { std::exchange(revokedGeometries, 0), std::exchange(revokedMain, 0) }; }
		/** @brief Of them, those taken back for a structural change after the selected revision's join (since the last call). */
		std::uint64_t TakeStructureRevocations() { return std::exchange(revokedStructureGeometries, 0); }
		/** @brief The set's phases of an object slot (SetPhase bits), 0 when it is not a member. Render thread, or any thread between commits. */
		std::uint8_t SetPhasesOf(std::int32_t a_object) const
		{
			const auto& phases = FrameView().setPhases;
			return a_object >= 0 && static_cast<std::size_t>(a_object) < phases.size() ? phases[a_object] : std::uint8_t{ 0 };
		}
		/** @brief The frame's set as the engine's hooks read it (CommitSet's publication). */
		std::shared_ptr<const SetSnapshot> GetSet() const { return setSnapshot; }
		struct SetStats
		{
			std::uint64_t commits = 0, evaluated = 0, joined = 0, left = 0, readinessEvents = 0, resyncs = 0;
			std::uint64_t members = 0, waiting = 0, rebinding = 0;  // summed over the commits
			// Summed over the commits: the objects out of the set although some of their phases are ready (U5: a claim is the whole
			// object), what per-phase claims would have drawn of them.
			std::uint64_t partial = 0;
			std::uint64_t publications = 0;
			std::uint64_t patchedMember = 0;  // must be 0: the accumulate phase patched a member's binding (CommitSet keeps rebinds out)
			std::uint64_t leftAfterCommit = 0;  // slots out of phases of the commit's decision after it (LeaveSet)
			// Why a bound object waits, summed over the commits: its pipeline, its material, its pipeline's shadow mask, the shared
			// lookups (samplers, null and projected textures), its geometry, its decal slot, its layer partner, its shadow pipelines
			// or occlusion pipelines (or an alpha-tested caster's diffuse).
			// The reflection phase (8): its forward pipeline.
			std::array<std::uint64_t, 14> waitingBy{};
			// T6b3d: why a member left (its commit's verdict): the scene not built (a load), no phase to take part in (its record freed or
			// ineligible now, its phases off), not bound (its membership dropped: the joins), waiting (by waitingBy's index: leftWaiting),
			// its layer partner or a phase of the whole object not ready.
			std::array<std::uint64_t, 5> leftBy{};
			std::array<std::uint64_t, 14> leftWaiting{};
			std::uint64_t waitingRequeued = 0;  // waiting slots taken again for a readiness source they wait on
			std::uint64_t commitParityChecks = 0, commitParityDiffer = 0;  // CS_DCLF_SET_PARITY: full evaluations, slots that differed
			std::string firstWaiting;
		};
		SetStats TakeSetStats() { return std::exchange(setStats, {}); }
		/**
		 * @brief How many objects take part in an occlusion map's phase this frame without being its members (not ready): the engine
		 * must then register the map's scene, and its registration withholds the members (PassCapture). 0: every occluder DCLF knows
		 * is drawn by DCLF, and the engine's cull of the map can be skipped.
		 */
		std::uint32_t SetLacking(std::uint8_t a_phase) const
		{
			return a_phase == kSetOccluderSky ? frameLackingCount[0] : a_phase == kSetOccluderPrecipitation ? frameLackingCount[1] : a_phase == kSetReflection ? frameLackingCount[2] : 0u;
		}
		/** @brief Whether a main-pass build can draw the object now: it has bindings and its pipeline is drawable. */
		bool ObjectDrawable(std::int32_t a_object) const
		{
			const auto& objects = FrameView().objects;
			if (a_object < 0 || static_cast<std::size_t>(a_object) >= objects.size())
				return false;
			const auto& record = objects[a_object];
			return !(record.flags & kObjectNoBindings) && PipelineDrawable(record.pipelineIndex);
		}
		/**
		 * @brief For the on-demand build warnings: how many objects use the pipeline slot, and the first one (its geometry,
		 * reference and the reference's base form type). Walks the objects: only for a pipeline that is being built.
		 */
		std::string DescribePipelineUsers(std::uint32_t a_pipeline) const;
		/** @brief The same for a shadow caster key (no mode bits), of the shadow casters or of an occlusion view's (a_occlusion). */
		std::string DescribeShadowKeyUsers(const ShadowPipelineKey& a_key, std::uint32_t a_occlusion = ~0u) const;
		/** @brief PrimaryCull::MembershipWitness when the residents were last bound (BindByMembership). */
		std::uint32_t BoundMembershipWitness() const { return membershipWitness; }
		/**
		 * @brief Any thread: a fade node's currentFade changed outside the engine's own writers (PrimaryCull's fade service), for
		 * the fade watch (its dependents' Faded shadow verdicts).
		 */
		static void NoteFadeChanged(const RE::NiAVObject* a_fadeNode);
		/**
		 * @brief This thread's fade watch muted, or not again: the engine's fade functions run on a copy of a node
		 * (FadeState::CheckPort) push nothing.
		 */
		static void MuteFadeEvents(bool a_muted);
		/**
		 * @brief PrimaryCull: the fade roots whose OnVisible DCLF services (the admitted, stood-in entries' roots; kFadeRootOwned),
		 * as a whole set, on a new snapshot or an admission. A root newly owned is seeded again from its node, which the engine
		 * kept until then.
		 */
		struct OwnedFadeRoot
		{
			const RE::NiAVObject* node = nullptr;
			bool standIn = false;  // kFadeRootStoodIn: no engine-drawn part
		};
		/** @brief Render thread (PrimaryCull): posted latest-wins, applied at the coordinator's next scene pass (TakeCoordinatorInputs). */
		void SetFadeRootsOwned(const std::vector<OwnedFadeRoot>& a_owned)
		{
			fadeOwnedPosted.Post(std::make_unique<std::vector<OwnedFadeRoot>>(a_owned));
			// T6b3d: no wake: the frame's next pass takes it.
		}
		void ApplyFadeRootsOwned(const std::vector<OwnedFadeRoot>& a_owned);
		/**
		 * @brief Render thread: whether the fade node's state is the GPU's alone (a stood-in root, kFadeRootStoodIn): the engine
		 * does not cull it, so its node keeps what the engine last left there, and a reader takes the GPU's state or the settled
		 * one instead.
		 */
		bool FadeOnGpu(const RE::NiAVObject* a_fadeNode) const
		{
			const auto it = a_fadeNode ? fadeRootOwned.find(a_fadeNode) : fadeRootOwned.end();
			return it != fadeRootOwned.end() && it->second;
		}
		/**
		 * @brief CS_DCLF_COVERAGE_PROBE, render thread, at a report: every tracked geometry the main pass leaves to the engine
		 * for its technique, by technique, shadow verdict and record; and the light entries kept in the point lights' culls,
		 * by what blocks them (the first geometry LightEntryAllows refuses, or a caster the paraboloid views do not draw).
		 */
		std::string CoverageCensus() const;
		/**
		 * @brief Render thread, the scene work joined (a report): why a tracked geometry's sun entry rule allows it - its record, its
		 * ineligibility reason and the caster rule's verdict - or that it is not tracked. Looked up by address, never dereferenced
		 * unless tracked.
		 */
		std::string DescribeSunCandidate(const void* a_geometry) const;
		std::size_t StoodInFadeRoots() const
		{
			return static_cast<std::size_t>(std::count_if(fadeRootOwned.begin(), fadeRootOwned.end(), [](const auto& a_root) { return a_root.second; }));
		}
		/** @brief PrimaryCull: after frames the engine culled every entry (its OnVisible ran on the nodes), every owned root again from its node. */
		void ReseedOwnedFadeRoots()
		{
			fadeReseedPosted.store(true, std::memory_order_release);
			// T6b3d: no wake: the frame's next pass takes it.
		}
		void ApplyReseedOwnedFadeRoots();
		/** @brief A listed fade root's row from its node again (a new generation: the GPU's state restarts from it), owned bits kept. */
		void ReseedFadeRoot(const void* a_node);
		/**
		 * @brief For reports: why a tracked geometry has no bindings this frame - the accumulate phase's
		 * verdict if it made one this frame (a_accumulate set), else the scene phase's cached one. None when it
		 * is eligible or not tracked.
		 */
		Ineligible ReasonThisFrame(const RE::BSGeometry* a_geometry, bool* a_accumulate = nullptr) const;

		/** @brief The main camera's batch renderers, as of the last BuildFrame. */
		const ankerl::unordered_dense::set<const RE::BSBatchRenderer*>& GetMainBatchRenderers() const { return mainBatchRenderers; }

		/** @brief Tree LOD's mirror of the engine's groups (TreeLod.h): the depth commit uploads what it changed. Render thread. */
		TreeLod::Mirror& TreeLodMirror() { return treeLod; }

		/** @brief True when the geometry sits under a tracked category node (used by coverage checks). The coordinator's set. */
		bool IsTracked(const RE::BSGeometry* a_geometry) const;
		/**
		 * @brief Render thread (T6b3a): whether the installed publication claims the geometry, so it is alive while the frame holds that
		 * publication (its tables name it: the retirement chain keeps what a held publication names). PrimaryCull's liveness check for the
		 * claims' changes (NoteSetChanges), in place of the coordinator's tracked set.
		 */
		bool HeldByInstalled(const RE::BSGeometry* a_geometry) const
		{
			return installed && installed->claims && installed->claims->phases.contains(a_geometry);
		}
		/** @brief Object LOD: the index ranges a tracked BSSubIndexTriShape draws (its visible segments' runs), or null. */
		const std::vector<LodSegments::Range>* LodRangesOf(const RE::BSGeometry* a_shape) const
		{
			const auto it = lodRanges.find(a_shape);
			return it != lodRanges.end() ? &it->second : nullptr;
		}

		/** @brief How a tracked geometry came to be tracked (diagnostics: CaptureParity's untracked draws). */
		enum class TrackSource : std::uint8_t
		{
			AttachEvent,       // an attach event's subtree walk
			CategoryAppeared,  // the walk of a category node RefreshCategoryNodes found new
			Rescan,            // the full rescan after a load
		};
		/** @brief The frame a tracked geometry was added, and how; false when it is not tracked. */
		bool GetTrackInfo(const RE::BSGeometry* a_geometry, std::uint32_t& a_frame, TrackSource& a_source) const;
		/** @brief The category node an object hangs under, or null (diagnostics). */
		RE::NiNode* CategoryNodeOf(RE::NiAVObject* a_object) const { return FindCategoryNodeLive(a_object, nullptr); }
		/**
		 * @brief The frame a category node was found, and why the refresh that found it ran (0 signature change,
		 * 1 forced by a detach or rescan); false when unknown (diagnostics).
		 */
		bool GetCategoryInfo(const RE::NiNode* a_node, std::uint32_t& a_frame, std::uint8_t& a_cause) const;


		/** @brief Full eligibility (static and per-frame) of a tracked geometry; NotTriShape if untracked. */
		Ineligible Classify(RE::BSGeometry* a_geometry) const;

		/** @brief Whether a negative verdict follows only from what the pointer witnesses cover. */
		static bool CacheableVerdict(Ineligible a_reason);
		/**
		 * @brief Static eligibility of a geometry, without the per-frame checks: from its records (T6b1b: the mirror's leaf on the scene
		 * work, SceneCapture::LiveLeaf's on the render thread).
		 */
		static Ineligible ClassifyStatic(const SceneCapture::LeafView& a_leaf, LightingDescriptors* a_descriptors, const AccumulatedPass* a_accumulated = nullptr);
		/**
		 * @brief Static eligibility of a geometry's main-pass layer (LayerPropertyOf): its descriptors as the layer's pass draws
		 * them (decal group 3), with a_accumulated the layer's membership pass, or null.
		 */
		static Ineligible ClassifyLayer(const SceneCapture::LeafView& a_leaf, LightingDescriptors* a_descriptors, const AccumulatedPass* a_accumulated);

		/** @brief The lighting pass the main-camera accumulator holds for a geometry this frame, or null. */
		const AccumulatedPass* FindAccumulatedPass(const RE::BSGeometry* a_geometry) const;
		/** @brief Every lighting pass the main-camera accumulator holds this frame, by geometry (diagnostics). */
		const ankerl::unordered_dense::map<const RE::BSGeometry*, AccumulatedPass>& GetAccumulatedPasses() const { return accumulatedPasses; }

		/**
		 * @brief Render thread, at an execution's commit: the import leases of the geometry slots cleared since the last call, for
		 * the execution to hold until the GPU retires it. The frames already submitted read those buffers by their device
		 * addresses, which DXVK does not see: a lease released at once let it free the buffer and give its memory to the next one
		 * the game created (a LOD chunk streamed in after a teleport), so an in-flight frame's colour pass drew other vertices
		 * than its Z-prepass had. Any execution submitted after the clear completes after every one before it (one queue).
		 */
		std::vector<std::shared_ptr<const void>> TakeRetiredImports() { return std::exchange(frameRetiredImports, {}); }
		/** @brief Render thread: a GPU object the frame's executions may still read, released with the next main commit's (TakeRetiredImports). */
		void RetireImport(std::shared_ptr<const void> a_owner) { frameRetiredImports.push_back(std::move(a_owner)); }

		/**
		 * @brief What FrameValues samples (dclf-async-publication.md, "FrameValues"): the placements and palettes of the per-frame
		 * movers (the walk's kept movers, every one), the sun entries of the roots the walk took, and the slots the walk wrote in
		 * full - each an object's slot and the engine objects its row is read from, held. Made by the walk on the scene task, taken
		 * by the render thread at the next frame's start (TakePlacementPlan) and released there: its references are the engine's to
		 * drop. The tables hold no placement or palette: these are the only rows the frames draw with.
		 */
		struct PlacementPlan
		{
			struct Item
			{
				RE::NiPointer<RE::BSGeometry> geometry;
				RE::NiPointer<RE::NiAVObject> sunEntryNode;  // its sun entry (ResolveSunEntry); none: the entry is unbounded
				std::uint32_t slot = kNoObjectSlot;
				std::uint32_t layerSlot = kNoObjectSlot;  // its layer's row (WriteLayer): the same placement, its layer property's fade node
				// A skin's palette block (Tables::boneOffset, boneRows), 0 rows without one: the engine's palette update, then its
				// rows (PaletteRowsOf).
				std::uint32_t boneOffset = 0, boneRows = 0;
			};
			struct Root
			{
				RE::NiPointer<RE::NiAVObject> root;
				std::vector<Item> dependents;  // their sun entry is the root's bound (sunEntryNode unused)
			};
			std::vector<Item> movers;   // every frame until the next plan
			std::vector<Item> written;  // once, by the frame that takes the plan
			std::vector<Root> roots;    // every frame until the next plan
			std::uint32_t slots = 0;         // the object slots the tables held
			std::uint32_t boneCapacity = 0;  // the palette rows their blocks span (Tables::BoneCapacity)
			std::uint64_t walk = 0;          // the walk that made it
		};
		/**
		 * @brief A slot whose shading FrameValues samples (SampleShading) at the next frame's start: named by its shading events (the
		 * walk: NameShadingEvents) and by the accumulate phase's patch. Holds its property, so the sample never reads a freed one.
		 */
		struct ShadingItem
		{
			RE::NiPointer<RE::BSShaderProperty> property;  // a BSLightingShaderProperty (the layer's for a layer's slot)
			std::uint32_t slot = kNoObjectSlot;
			std::uint32_t pass = 0;   // its pipeline's pass descriptor
			bool member = false;      // a resident record's (SampleShading's a_member)
			bool actor = false;       // owned by an actor: its wetness is CaptureWetness's; else zero
		};
		/**
		 * @brief T6b1a: a fade root whose seed FrameValues samples (FadeState::StaticOf) in the frame that first draws its generation, into
		 * its seed row (FadeSeedRow): listed, reseeded by a placement snap, or owned. Holds the node.
		 */
		struct FadeSeedItem
		{
			RE::NiPointer<RE::NiAVObject> node;
			std::uint32_t root = kNoFadeRoot;
			std::uint32_t generation = 0;
			std::uint32_t row = 0;
		};
		/** @brief Render thread, the frame's start: the seeds requested since the last take (the publication log's, HandOverAtFrameStart). */
		std::vector<FadeSeedItem> TakeFadeSeeds() { return std::exchange(frameFadeSeeds, {}); }
		/** @brief T6b1a: a listed tree node whose values FrameValues' tree seeds take at the frame's start (row: 2 * slot + seedOdd). */
		using TreeSeedItem = FadeSeedItem;
		std::vector<TreeSeedItem> TakeTreeSeeds() { return std::exchange(frameTreeSeeds, {}); }
		/** @brief Render thread: the frame's engine globals (FrameValues binds them for the seeds' LOD scale). */
		const std::shared_ptr<const FrameGlobals>& FrameGlobalsOfFrame() const { return frameGlobals; }
		/** @brief One actor-owned slot's wetness, as the frame's start captured it (Skin::GetWetness). */
		struct WetnessValue
		{
			std::uint32_t slot = kNoObjectSlot;
			std::array<float, 4> value{};
		};
		/** @brief Render thread, the frame's start: the slots named since the last take, once (the publication log's). */
		std::vector<ShadingItem> TakeShadingItems() { return std::exchange(frameShading, {}); }
		/**
		 * @brief Render thread (diagnostics, the parities' inline scene work): the slots named for the next frame, not taken: those the
		 * publication log holds past the frame's cursor, then those the coordinator has named since its last publication.
		 */
		std::vector<ShadingItem> PeekShadingItems() const;
		/**
		 * @brief Render thread, at the frame's start: every actor's wetness (Skin::GetWetness, whose first call a frame advances the
		 * actor's fade and is not thread-safe), fanned out to the meshes whose value changed or that joined. T6b3a: the actors' membership
		 * is the render thread's own index (frameActorWetness), kept from the log's changes; the geometries are the newest publication's.
		 */
		std::vector<WetnessValue> CaptureWetness();
		/**
		 * @brief Render thread, the frame's start: the plans the walks made since the last take, oldest first (the publication log's;
		 * FrameValues samples every one's written slots and draws with the newest).
		 */
		std::vector<std::shared_ptr<const PlacementPlan>> TakePlacementPlans() { return std::exchange(framePlans, {}); }
		/** @brief Render thread (diagnostics, the parities' inline scene work): the plan the last walk made, not published yet. */
		const PlacementPlan* PeekPlacementPlan() const { return placementPlanReady.get(); }

	private:
		std::vector<std::shared_ptr<const void>> retiredImports;  // the coordinator's, into the publication log (TakeRetiredImports)
		// The slots named for the next frame's shading sample (ShadingItem): by the walk and the accumulate phase, into the publication
		// log (TakeShadingItems). Every slot when the shading events are not installed.
		std::vector<ShadingItem> shadingNamed;
		FrameCapture frameCapture;  // GetFrameCapture
		FrameTables frameTables;    // GetFrameTables: the frame's values, out of the tables (render thread)
		/** @brief A slot's ShadingItem into shadingNamed (none for a free, unbound or propertyless slot). */
		void NameShading(std::uint32_t a_slot, bool a_member);
		/**
		 * @brief The walk's: the shading events (LOD fades, emittance, the controllers' MaterialSources writes) drained and their
		 * dependents named, with their extras' static rows written again; every slot the first time and while the events are not
		 * installed. a_slots: the slots named (the parity's), when given.
		 */
		void NameShadingEvents(std::vector<std::uint32_t>* a_slots = nullptr);
		/** @brief The dependents of shading event keys (properties, emittance colours) named; their slots into a_slots when given. */
		void NameShadingKeys(const std::vector<const void*>& a_keys, std::vector<std::uint32_t>* a_slots);
		// The walk's placement plan (PlacementPlan): filled by its rounds and PublishPlacementPlan, ready once the walk is done.
		std::shared_ptr<PlacementPlan> placementPlan, placementPlanReady;
		// T6b3d: plans no publication took yet, oldest first: the next publication carries them all (TakeDeltas), as the frame takes every
		// plan (a plan makes its pass publish: DeltasPending; kept for a pass that threw before its publication).
		std::vector<std::shared_ptr<PlacementPlan>> placementPlansHeld;
		// T6b3d: the per-frame entries are walked once a frame input (DeltaWalk): the frame the last such walk was for, whether this walk
		// took them, and the movers that walk listed (a later walk of the same frame's plan carries them: PublishPlacementPlan).
		std::uint32_t perFrameFrame = ~0u;
		bool perFrameWalked = true;
		// T6b3e: keys and slots only, never references: a reference kept across passes would be released by the pass that drops it (a
		// load, the next per-frame walk), on the pump's thread, where an engine destructor must not run. A carrying plan copies its
		// references again from the entries and the root owners (PublishPlacementPlan).
		struct CarriedItem
		{
			const RE::BSGeometry* geometry = nullptr;
			std::uint32_t slot = kNoObjectSlot;
		};
		std::vector<CarriedItem> lastMovers;
		// T6b3e (e0): the roots that walk listed (QueueRoots: the per-frame work), carried likewise, with their dependents.
		struct CarriedRoot
		{
			const RE::NiAVObject* root = nullptr;
			std::vector<CarriedItem> dependents;
		};
		std::vector<CarriedRoot> lastRoots;
		struct Tracked
		{
			RE::NiPointer<RE::BSGeometry> geometry;
			// T6b1b: its shader property and layer property, the mirror's (its geometry record's), held from the pins of the event that
			// named them (HoldProperties): what the scene work's references to them are copies of, never the live geometry's.
			RE::NiPointer<RE::BSShaderProperty> property, layerProperty;
			RE::BSShaderProperty* HeldProperty(bool a_layer) const { return (a_layer ? layerProperty : property).get(); }
			// T6b1b: written while the mirror held no record of it (WriteObject): evaluated again when a capture's batch names it.
			bool leafWaiting = false;
			std::uint64_t identity = 0;
			std::uint64_t groupIdentity = 0;
			const RE::TESObjectREFR* actorOwner = nullptr;  // lookup only, never published
			const RE::NiNode* roomNode = nullptr;  // lookup key, refreshed on structural attachment
			std::uint64_t roomMapGeneration = 0;
			int roomIndex = -1;
			RE::NiNode* categoryNode = nullptr;
			// Ineligible::UnsupportedParent or Billboard for what lies between the leaf and its category node
			// (ParentReason in SceneStore.cpp); Switch when a switch node lies there, which ClassifyFrame
			// decides per frame; else None.
			Ineligible parentReason = Ineligible::None;
			// When and how it was added (diagnostics, GetTrackInfo).
			std::uint32_t trackedFrame = 0;
			TrackSource trackedBy = TrackSource::AttachEvent;
			// T6b0: the frames its record was last taken anew (a slot), it was last bound (made resident: CS_DCLF_TIMELINE) and it last
			// joined the set (CS_DCLF_TIMELINE). 0: never.
			std::uint32_t writtenFrame = 0, boundFrame = 0, memberFrame = 0;
			// T6b0: its last membership pass that gave none (PrimaryCull::LastSyntheticFail), with the frame; a record so left is bound
			// only when written again.
			std::uint32_t bindFailFrame = 0;
			std::uint8_t bindFail = 0;

			/**
			 * @brief A cached "this object cannot be drawn", and the witnesses that keep it honest.
			 *
			 * Only *negative* verdicts are cached. A positive one is not a property of the object: the
			 * descriptors it produces come from the engine's per-frame render pass, so it has to be
			 * derived again each frame. A negative is a property of the object - of its type, its skin
			 * instance, its shader property's flags and its material - and re-deriving it every frame is
			 * the single largest piece of waste in BuildFrame. In the Whiterun exterior 6097 of 9022
			 * tracked objects produce one, every frame, and it is thrown away.
			 *
			 * The witnesses are pointers that change when the thing behind them does, compared in full
			 * every frame. Cheap, and they close the one dangerous case: a stale *positive* over freed
			 * renderer data is a device loss, which is why positives are not cached at all and why
			 * rendererData is witnessed even so - a negative that became stale the other way would
			 * silently leave an object native forever.
			 *
			 * Entry lifetime covers attach, detach, cell teardown and the post-load rescan, because the
			 * whole Tracked is destroyed. ValidateSlice covers what has no cheap witness.
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
			 * @brief The positive derivation, cached (CS_DCLF_DERIVED_CACHE): everything the loop derives
			 * for an accumulated object that is fixed until a witness changes - the descriptors, the static
			 * object flags, the pipeline key and the three table slots. The witnesses are the four pointers
			 * StaticVerdict uses, the fade state, the accumulated pass's technique / sub-pass / hint (the
			 * per-frame bits live in the technique), the interior flag, whether the material alpha is
			 * below one, and the decal bias modes of the frame; a slot is also checked against its key
			 * before it is served, so a swept and reused slot cannot be handed back.
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
			 * @brief The cull-only verdict: whether the object is a culling candidate when the engine did not keep
			 * it, as the full classification last found it, with the frame it was found on. It stands until one of
			 * the delta walk's events takes it again (dclf-event-driven-tables.md, "Phase 3").
			 */
			std::uint32_t candidateFrame = 0;  // the pass that classified it (T6b3d: PassStamp, "classified in this walk"); 0: never classified
			Ineligible candidateReason = Ineligible::None;
			// An NPC face shape: a BSDynamicTriShape under a BSFaceGenNiNode, whose positions are FaceSnapshots'.
			// Resolved once, by the walk.
			bool faceShape = false;
			bool faceShapeResolved = false;
			// Its head (a key), while listed in faceHeads; and whether its last write found no snapshot of the head, so that the
			// head's next publication writes it again.
			const RE::BSFaceGenNiNode* faceHead = nullptr;
			bool faceWaiting = false;
			// T6b1b: a dynamic shape under a BSFaceGenNiNode (the mirror's parent): the head, held from the batch's pins at its attach (the
			// face snapshot's record's reference is a copy).
			RE::NiPointer<RE::BSFaceGenNiNode> faceHeadRef;
			// T6b3e: its property's fade node (the property record's fadeNode: the BSFadeNode or BSTreeNode above it), held from the pins of
			// the event that named the property (HoldProperties): what the listed tree and fade-root owners are copied from (ListTree,
			// ListFadeRoot), never a reference made from the mirror's key.
			RE::NiPointer<RE::NiAVObject> fadeNodeRef;
			const void* fadeNodeRequested = nullptr;  // T6b3e: the fade node key asked of the render thread (FadeNodeRequest), until answered
			// T6b5: the LOD gate whose roots it hangs under (its token; 0: none) and on which side (kGateIncoming, kGateOutgoing), until the
			// gate's outcome; an outgoing block's geometry held out of the set after its gate flipped, until its detach erases the entry; and
			// whether any gate ever tagged it (the residue's "shown with no gate").
			std::uint64_t gate = 0;
			std::uint8_t gateSide = 0;
			bool gateHeldOut = false;
			bool gateSeen = false;
			// Owned by an actor (its GetUserData is an ActorCharacter): Advanced Skin gives its draws the actor's
			// wetness (Tables::skinWetness). Resolved once, by the walk.
			bool actorOwned = false;
			bool actorOwnedResolved = false;
			// The accumulate phase's verdict when it left the object without bindings, and the frame it did so
			// (ReasonThisFrame).
			Ineligible accumulateReason = Ineligible::None;
			std::uint32_t accumulateReasonFrame = 0;  // the frame (T6b0's timeline compares it with writtenFrame)
			std::uint64_t accumulateReasonPass = 0;   // T6b3d: the pass (a join failed for a verdict of this pass)
			// Its index in this walk's tables, valid while objectStamp equals SceneStore::objectStamp. Kept here
			// rather than in a geometry -> index map rebuilt by every walk: the map's insert was ~0.12 us an object,
			// and every consumer already has the entry (the accumulate phase) or looks it up by the same key.
			std::uint32_t objectStamp = 0;
			std::uint32_t objectId = 0;
			// Its persistent object slot: the index objectId holds while the object keeps a record, walk after walk.
			// kNoObjectSlot while it has none.
			std::uint32_t slot = kNoObjectSlot;
			// The node whose bound decides whether the object is a sun caster candidate (ResolveSunEntry), resolved
			// once: the scene graph above a tracked geometry does not change while it is tracked.
			const RE::NiAVObject* sunEntryNode = nullptr;
			bool sunEntryResolved = false;
			// Its light entry (LightEntryOf), listed in lightDependents for as long as it is tracked; null: none.
			const RE::NiAVObject* lightRoot = nullptr;
			// Its main-pass layer's object slot (LayerPropertyOf: a multi-index shape's additional property, drawn from the
			// second index list in decal group 3), kNoObjectSlot while it has none. Written with the base record (WriteLayer),
			// bound and dropped with it; its positive derivation is layerDerived.
			std::uint32_t layerSlot = kNoObjectSlot;
			Derived layerDerived;
			const void* listedLayerProperty = nullptr;  // the layer's property in propertyDependents
			// T6b3e: its membership passes' derived descriptors, the base's and the layer's (PrimaryCull::MembershipPass, which returns the
			// entry BindByMembership stores here): PrimaryCull's per-geometry maps, which nothing erased, moved into the entry they belong to.
			MembershipDerived membershipDerived, membershipLayerDerived;
			// The delta walk's. perFrame: inputs that change from frame to frame (PerFrameTraits: owned by an actor,
			// skinned, a face shape, under a switch, or a controller or a non-fixed rigid body on its chain), so the
			// delta walk evaluates it every frame; set at an evaluation that classified it, and kept. scheduledWalk:
			// the walk that last scheduled it. bucket: the histogram bucket it is counted in (kNoBucket: none).
			// fadeNode: the fade node it is listed under in fadeDependents.
			bool perFrame = false;
			bool perFrameListed = false;
			// lightTraits: the entry is per frame only because it moves (kTraitMoves, kTraitRootMoves), follows a switch
			// (kTraitSwitch), has animated shading (kTraitAnimatedShading) or is skinned (kTraitSkin; a tree's wind moves
			// its bones). While its classification stands, the full walk would take nothing else again: a switch's
			// verdict is taken again and the shading's inputs compared (the record is written in full when either
			// changes), and a skin or a record that moves is listed for FrameValues (PlacementPlan::movers; KeepSkin checks a skin's
			// palette size first). 0 for every other entry. movedWalk: the walk that took this path.
			std::uint32_t lightTraits = 0;
			std::uint32_t movedWalk = 0;
			// The key its move events carry (MoveKeyOf): the one reference on its chain up to the category node, set at
			// classification. Null when there is none or more than one, and the light path places it every frame.
			const void* moveKey = nullptr;
			// An actor's light-path entry: the nodes from it up to its category node, listed in hiddenDependents, and the
			// frame one of them last had a hidden-bit event (DrainHiddenEvents).
			std::vector<const RE::NiAVObject*> hiddenChain;
			std::uint32_t hiddenEventFrame = 0;  // T6b3d: the pass (PassStamp) whose batch announced a hidden event on its chain
			// kTraitAnimatedShading: what the record read from the animated shader and alpha properties when it was
			// last written in full (ShadingInputsOf); the light path writes it in full again when they differ.
			std::uint64_t shadingInputs = 0;
			// kTraitSwitch: the one switch node on its chain and its child on the leaf's path, found at classification,
			// so the light path reads the selection without walking the chain (null: none, or several).
			RE::NiSwitchNode* switchNode = nullptr;
			const RE::NiAVObject* switchChild = nullptr;
			// The keys it is listed under: its shader and alpha properties in propertyDependents (the last evaluation's),
			// its sun entry node in rootDependents (for as long as it is tracked).
			const void* listedProperty = nullptr;
			const void* listedAlpha = nullptr;
			const void* listedEmittance = nullptr;  // external emittance's shared colour (emittanceEvents)
			const RE::NiAVObject* listedRoot = nullptr;
			// A per-frame entry written in full (an actor's, a face's): what its classification read from the geometry,
			// its properties and its material (ClassifyInputsOf), re-read every frame. Its classification is taken again
			// when they differ; the hidden, actor and switch half is taken again every frame.
			std::uint64_t classifyInputs = 0;
			// CS_DCLF_INPUT_WATCH: the components the two hashes were taken from (InputComponentsOf), when they were stored.
			std::unique_ptr<std::array<std::uint64_t, 12>> inputComponents;
			std::uint32_t fullWalk = 0;  // the walk that must write it in full (an event, not the per-frame set)
			std::uint8_t bucket = kNoBucket;
			std::uint32_t scheduledWalk = 0;
			const RE::BSFadeNode* fadeNode = nullptr;
			static constexpr std::uint8_t kNoBucket = 0xFF;
		};
		/**
		 * @brief The object's entry bound for the sun's cascade culls. The cascade cull (FUN_140e305c0) walks
		 * only the entries of the full-frustum culling processes' objectArray, which the full-frustum cull
		 * (FUN_141511f30) fills with the items passing their planes. Measured against the engine's culls (0 false
		 * rejects): a static reference's entry is its reference root (the topmost ancestor carrying the
		 * geometry's userData); an actor's is its cell's container, never tested; a geometry without a
		 * reference (a terrain block) has its nearest BSMultiBoundNode.
		 */
		void ResolveSunEntry(Tracked& a_tracked, const RE::BSGeometry& a_geometry, bool a_live = false);
		/** @brief ResolveSunEntry's from the mirror alone. */
		const void* MirrorSunEntry(const void* a_geometry) const;

		/**
		 * @brief The sun entries whose whole content DCLF can take out of the engine's cascade culls (SunCandidates,
		 * SunAccumulation), kept from the scene events: at the end of the delta walk, every entry a change touched (a
		 * dependent attached, detached, or gaining or losing its record or its verdict) is judged again.
		 *
		 * Any change bumps the generation at once; the snapshot is rebuilt only on a walk that changed nothing, since
		 * an exclusion built for an older generation is never applied (SunAccumulation::ExcludeEntries) and a cell
		 * load would otherwise rebuild it every frame.
		 */
		void UpdateSunCandidates(bool a_full);
		void DropSunCandidates();
		/** @brief Whether a tracked geometry lets its sun entry leave the cascade culls (UpdateSunCandidates). */
		bool SunEntryAllows(const Tracked& a_tracked, bool a_switchNodes) const;
		/**
		 * @brief Whether a tracked geometry lets its entry leave the primary's cull (PrimaryCull): a main-pass table
		 * object (a verdict of None) that is not a decal and not alpha-blended, whose main pass the synthetic pass
		 * reproduces. Anything the main pass draws natively, or that the cull decides per frame (hidden, fading, a
		 * switch child), keeps the entry in.
		 */
		bool PrimaryEntryAllows(const Tracked& a_tracked, const RE::BSGeometry& a_geometry) const;
		void MarkSunEntryDirty(const RE::NiAVObject* a_entry)
		{
			if (a_entry)
				sunEntriesDirty.push_back(a_entry);
		}
		void MarkLightEntryDirty(const RE::NiAVObject* a_entry)
		{
			if (a_entry)
				lightEntriesDirty.push_back(a_entry);
		}
		/**
		 * @brief A tracked geometry's light entry (UpdateLightCandidates): its actor's root when it hangs from one (its category
		 * node's child, whose reference is an actor: an actor has no sun entry), else its sun entry unless that is a category
		 * node (a cell's or a room's multibound node, which holds other entries).
		 */
		const RE::NiAVObject* LightEntryOf(const Tracked& a_tracked, const RE::BSGeometry& a_geometry, bool a_live) const;
		/**
		 * @brief Whether a tracked geometry lets its light entry leave the point lights' culls. A table object: the exclusion
		 * decides (BuildSunExclusion). Any other must give the light's registration no pass: what the sun's rule allows
		 * (SunEntryAllows: hidden, alpha-blended, fading, an unselected switch child), or no Lighting property. The light's bit
		 * in its activeLightMask, which the registration writes on every geometry the cull reaches, is LocalLightCull's.
		 */
		bool LightEntryAllows(const Tracked& a_tracked, const RE::BSGeometry& a_geometry, bool a_switchNodes) const;
		void UpdateLightCandidates(bool a_full);
		/**
		 * @brief A light entry's tracked geometries (lightDependents), or null; null too for a node that has become a category
		 * node since (left out of a point light's cull, it would take every entry under it along).
		 */
		const std::vector<RE::BSGeometry*>* LightDependentsOf(const RE::NiAVObject* a_root) const;
		ankerl::unordered_dense::map<const RE::NiAVObject*, std::vector<RE::BSGeometry*>> lightDependents;  // by light entry
		ankerl::unordered_dense::set<const RE::NiAVObject*> lightCandidateSet;
		ankerl::unordered_dense::map<const RE::NiAVObject*, std::uint64_t> lightSignature;  // its geometries, as a set
		std::vector<const RE::NiAVObject*> lightEntriesDirty;
		std::shared_ptr<const SunCandidates> lightCandidates;
		std::uint32_t lightCandidatesGeneration = 0;
		std::uint64_t lightEntriesAppeared = 0;
		ankerl::unordered_dense::set<const RE::NiAVObject*> sunCandidateSet;
		// Per sun candidate, a signature of its geometries' PrimaryEntryAllows verdicts: a change bumps the generation.
		ankerl::unordered_dense::map<const RE::NiAVObject*, std::uint64_t> primarySignature;
		std::vector<const RE::NiAVObject*> sunEntriesDirty;
		ankerl::unordered_dense::set<const RE::NiAVObject*> sunEntriesForced;  // candidates whose subtree an attach or detach changed
		std::shared_ptr<const SunCandidates> sunCandidates;
		std::uint32_t sunCandidatesGeneration = 0;
		/**
		 * @brief A candidate set's stable index space (SunCandidates' indices), the coordinator's: the entries' and their geometries'
		 * indices with their free lists, each entry's geometries and its version (moved by every change of the index, from
		 * candidateVersions). A snapshot is brought up to it at the indices whose versions moved (PublishCandidates), reusing one of
		 * the pool no reader holds any more.
		 */
		struct CandidateTable
		{
			ankerl::unordered_dense::map<const RE::NiAVObject*, std::uint32_t> index;
			std::vector<const RE::NiAVObject*> nodes;
			std::vector<std::uint32_t> version;
			std::vector<std::vector<std::uint32_t>> geometries;  // per entry
			std::vector<std::uint32_t> free;
			ankerl::unordered_dense::map<const RE::BSGeometry*, std::uint32_t> geometryIndex;
			std::vector<const RE::BSGeometry*> geometryNodes;
			std::vector<std::uint32_t> geometryEntry;
			std::vector<std::int32_t> geometrySlot;
			std::vector<std::uint8_t> primaryGeometry;
			std::vector<std::uint32_t> geometryFree;
			std::vector<std::shared_ptr<SunCandidates>> pool;
			bool changed = true;  // an index moved since the last snapshot
			std::uint64_t entriesWritten = 0, snapshots = 0;  // since the last report
		};
		CandidateTable sunTable, lightTable;
		std::uint32_t candidateVersions = 0;
		/**
		 * @brief a_root's entry in a_table as its dependents now are (a_dependents; null: not a candidate): written again, its
		 * geometries with it, under a new version; a_sun: its geometries' PrimaryEntryAllows verdicts.
		 */
		void SetCandidate(CandidateTable& a_table, const RE::NiAVObject* a_root, const std::vector<RE::BSGeometry*>* a_dependents, bool a_sun);
		void ResetCandidateTable(CandidateTable& a_table);
		/** @brief CS_DCLF_PERSISTENT_PARITY: a_table against a_set and the dependents now; the first difference, or empty. */
		std::string CheckCandidateTable(const CandidateTable& a_table, const ankerl::unordered_dense::set<const RE::NiAVObject*>& a_set, bool a_sun) const;
		struct CandidateParity
		{
			std::uint64_t checks = 0, mismatches = 0;
		} candidateParity;
		/** @brief A snapshot of a_table, a new generation (a_generation), brought up to it at the indices that moved. */
		std::shared_ptr<const SunCandidates> PublishCandidates(CandidateTable& a_table, std::uint32_t& a_generation);

		void RefreshCategoryNodes(bool a_force = false);
		/**
		 * @brief The category nodes and the portal graphs' parentless roots, as the render thread found them (step 6e F2: the scene
		 * work reads no TES, cell or portal graph). Immutable; it holds every node it names. Made again only when CategorySignature
		 * moved, a detach was ingested (a detach can take a category node with it before the signature sees the cell go) or a load
		 * ended; RefreshCategoryNodes diffs its own set against the newest.
		 */
		struct CategoryCapture
		{
			std::uint64_t generation = 0;
			std::uint64_t signature = 0;
			ankerl::unordered_dense::set<RE::NiNode*> nodes;
			std::vector<RE::NiPointer<RE::NiNode>> held;
			struct Root
			{
				RE::NiPointer<RE::NiAVObject> root;
				RE::NiNode* category = nullptr;
			};
			ankerl::unordered_dense::map<const RE::NiAVObject*, Root> roots;
			// T6b1c: the category nodes new to the scene work's set (all of them before a rescan): their children's keys.
			ankerl::unordered_dense::map<const RE::NiNode*, std::vector<const RE::NiAVObject*>> children;
			// T6b1c: every node and geometry under its new category nodes and new roots, held for the scene work's references (copies of
			// these) until the coordinator has applied a newer capture. T6b3a: in the capture (no list the two threads share); "new" is
			// against the capture the coordinator applied last, so each capture pins all it names.
			std::vector<RE::NiPointer<RE::NiRefObject>> pins;
		};
		/**
		 * @brief Render thread, the frame's start (not while a load screen is up): the newest CategoryCapture, made when a_force or the
		 * signature moved, and posted (categoryPosted); the portal roots it finds new or gone pushed onto the tracker's stack, mirror-only
		 * (SceneTracker::PushCaptured), before the post: a pass that takes the post drains them first.
		 */
		struct EventBatch;
		struct BatchUnwindGuard;  // T6b3e: a batch to the render thread on a throw (Internal.h)
		void CaptureCategories(bool a_force);
		/**
		 * @brief Render thread, the frame's start: what the coordinator found the mirror lacking (mirrorCaptureRequests, each held by its
		 * request) captured with its chain up to the world's root (SceneCapture::CaptureAttached) and pushed onto the tracker's stack,
		 * mirror-only. T6b3d: the mirror is not read here (the coordinator asked because it lacked the record).
		 */
		void CaptureMirrorRequests();
		/** @brief T6b3d, the coordinator, first in its pass (before the drains): the newest category capture posted, taken. */
		void TakeCategoryCapture();
		// T6b3d: a pass drained a detach (the coordinator -> the render thread): the next frame start's category capture is forced (a
		// detach can take a category node with it before the signature sees its cell go).
		std::atomic<bool> categoryDetachSeen{ false };
		std::shared_ptr<const CategoryCapture> categoryCapture;  // render thread: the newest made
		// Render thread (T6b3a): the captures made and not yet superseded by one the coordinator applied, oldest first. Their pins and
		// nodes are dropped here (the render thread's), once the coordinator has let go of its copy and moved categoryAppliedGeneration on.
		std::deque<std::shared_ptr<const CategoryCapture>> categoryCapturesHeld;
		// Render thread -> the coordinator (latest wins), and the coordinator's taken copy (RefreshCategoryNodes diffs against it).
		LatestSlot<std::shared_ptr<const CategoryCapture>> categoryPosted;
		std::shared_ptr<const CategoryCapture> categoryTaken;
		std::uint64_t categoryCaptures = 0;  // render thread: the last capture's generation
		// The coordinator's: the capture RefreshCategoryNodes last diffed against (stored after it let go of the one before); the render
		// thread reads it to know which captures it may let go of and what is new against the coordinator's set.
		std::atomic<std::uint64_t> categoryAppliedGeneration{ 0 };
		// Render thread, since the last report: the frame captures' time (FrameGlobals, CaptureCategories), the category captures
		// made, and the frames.
		std::uint64_t captureNs = 0, categoryCaptureNs = 0, categoryCapturesMade = 0, captureFrames = 0;
		std::uint64_t categoryMirrorCaptures = 0, categoryMirrorRecords = 0;  // T6b1a: CaptureCategories' captures for the mirror
		// T6b1a: tracked geometries the mirror held no record of (AddGeometry), captured by the render thread at the next frame's start.
		// T6b3a: a lock-free queue (the coordinator pushes, the render thread drains), not a list the two threads share.
		EventQueue<RE::NiPointer<RE::NiAVObject>, 256> mirrorCaptureRequests;
		/**
		 * @brief T6b3e: a fade node the coordinator holds no reference to (no pin, no root owner: a property whose fade node is not on its
		 * geometry's chain, set by another instance's clone of a shared model), asked of the render thread. It reads the geometry's property
		 * live where the engine reads it (the frame's start), checks the property still names that node and the geometry is in the world
		 * (the engine draws it with that node: alive), takes a reference and posts it back (fadeNodeAnswers). The request holds the geometry.
		 */
		struct FadeNodeRequest
		{
			RE::NiPointer<RE::BSGeometry> geometry;
			const void* fadeNode = nullptr;
		};
		struct FadeNodeAnswer
		{
			const RE::BSGeometry* geometry = nullptr;  // a key
			const void* fadeNode = nullptr;            // the requested key
			RE::NiPointer<RE::NiAVObject> node;        // null: the property names another node now, or the geometry left the world
		};
		EventQueue<FadeNodeRequest, 256> fadeNodeRequests;
		EventQueue<FadeNodeAnswer, 256> fadeNodeAnswers;
		/** @brief T6b3e, render thread, the frame's start (ServeFrameRequests): the fade node requests answered. */
		void ServeFadeNodeRequests();
		/** @brief T6b3e, the coordinator (ApplyBatch): the answers into their entries (Tracked::fadeNodeRef), each entry written again. */
		void ApplyFadeNodeAnswers();
		// The signature the category set was last rebuilt for.
		std::uint64_t categorySignature = 0;
		/**
		 * @brief The category node a_object hangs under (null for none), and the strongest parent reason on the way: the mirror's
		 * chain (T6b1b), the scene as the applied batch left it. The pointer is a key (never read through). CS_DCLF_MIRROR_PARITY
		 * checks it against the live chain (MirrorReads).
		 */
		RE::NiNode* FindCategoryNode(RE::NiAVObject* a_object, Ineligible* a_parentReason, bool* a_unmirrored = nullptr) const;
		/** @brief The same from the live parent chain: the parities' and the diagnostics' (the object alive). */
		RE::NiNode* FindCategoryNodeLive(RE::NiAVObject* a_object, Ineligible* a_parentReason) const;
		/**
		 * @brief T6b1b: the lane's reads ported to the mirror, each checked against the live read under CS_DCLF_MIRROR_PARITY (the
		 * object alive: the lane holds it). Counted per read since the last report: checked, differing, and the reads that found no
		 * record on the way (the mirror lacking an object the lane names).
		 */
		enum class MirrorRead : std::uint8_t
		{
			CategoryNode,
			Subtree,
			SwitchEvent,
			NodeEvent,
			HiddenChain,
			MoveKey,
			LightEntry,
			SunEntry,
			Reference,  // T6b1c: a geometry the mirror's walk found with no pin (lagged: the live scene no longer has it there)
			kCount
		};
		static constexpr std::array<const char*, static_cast<std::size_t>(MirrorRead::kCount)> kMirrorReadNames{ "category node", "subtree", "switch event", "node event",
			"hidden chain", "move key", "light entry", "sun entry", "unpinned geometry" };
		/** @brief A mirror read's check: counts it; a difference (a_describe() it) is checked again after the next batch (DeferredRead). */
		template <class Describe, class Recheck>
		void NoteMirrorRead(MirrorRead a_read, bool a_differ, Describe&& a_describe, Recheck&& a_recheck) const
		{
			++MirrorReads().checked[static_cast<std::size_t>(a_read)];
			if (a_differ)
				deferredReads.push_back({ a_read, a_describe(), std::function<bool()>(std::forward<Recheck>(a_recheck)) });
		}
		/** @brief The mirror's parent of a_key (null: none, or no record). */
		const void* MirrorParent(const void* a_key) const
		{
			const auto* record = mirror.Node(a_key);
			return record ? record->parent : nullptr;
		}
		/** @brief The T6b1b check of a walk below a_root: the tracked geometries the mirror gives against the live ones. */
		void CheckTrackedBelow(MirrorRead a_read, RE::NiAVObject* a_root);
		/** @brief The geometries under a_root as the mirror's children give them, each with the parent reasons below a_reason, in the walk's order. */
		void MirrorSubtree(const void* a_root, Ineligible a_reason, std::vector<std::pair<RE::BSGeometry*, Ineligible>>& a_out, bool& a_unmirrored) const;
		/**
		 * @brief T6b1d: a live check's lease. The scene work is not joined at Present, so its observers' live reads each take one (the
		 * engine-read window); refused (the window closed: the engine's update may be running), the check is skipped and counted.
		 */
		EngineReadWindow::Lease LiveCheckLease(MirrorRead a_read) const
		{
			EngineReadWindow::Lease lease;
			if (!lease)
				++MirrorReads().refused[static_cast<std::size_t>(a_read)];
			return lease;
		}
		struct MirrorReadStats
		{
			std::array<std::uint64_t, static_cast<std::size_t>(MirrorRead::kCount)> reads{}, checked{}, lagged{}, differ{}, unmirrored{}, refused{};
			std::array<std::string, static_cast<std::size_t>(MirrorRead::kCount)> first;
		};
		mutable MirrorReadStats mirrorReads;
		/**
		 * @brief T6b3e: where this thread counts its mirror reads: an evaluation chunk's counters (EvalCounters::mirror, installed by its
		 * ShardScope, merged in chunk order), else the store's. Every count goes through MirrorReads().
		 */
		static inline thread_local MirrorReadStats* mirrorReadSink = nullptr;
		MirrorReadStats& MirrorReads() const { return mirrorReadSink ? *mirrorReadSink : mirrorReads; }
		/**
		 * @brief A read the check found differing: the mirror is the scene as the batch left it, the live read is later (what changed
		 * since is the next batch's). Checked again after the next batch (a_recheck: whether the mirror now gives what the live read
		 * gave, from the mirror and the values it captured alone): agreeing, it lagged; differing still, it is counted (TakeDeferredReads).
		 */
		struct DeferredRead
		{
			MirrorRead read;
			std::string what;
			std::function<bool()> recheck;
		};
		mutable std::vector<DeferredRead> deferredReads;
		void RecheckDeferredReads();
		/** @brief FindCategoryNode's chain, the mirror's alone (no counting, no check). */
		RE::NiNode* MirrorCategoryNode(const void* a_object, Ineligible& a_reason, bool& a_unmirrored) const;
		/** @brief The mirror's chain from a_from up to a_stop (left out) or its top. */
		std::vector<const void*> MirrorChain(const void* a_from, const void* a_stop) const
		{
			std::vector<const void*> chain;
			for (const void* key = a_from; key && key != a_stop && chain.size() <= kMaxParentDepthForChains; key = MirrorParent(key))
				chain.push_back(key);
			return chain;
		}
		static constexpr std::size_t kMaxParentDepthForChains = 64;
		bool mirrorReadParity = false;  // CS_DCLF_MIRROR_PARITY, read once
		std::string MirrorReadReport();
		/**
		 * @brief Tracks the geometries under a_root (a category node above it). One the mirror holds no chain for (an attach no hook
		 * captured) is captured by the render thread at the next frame's start (mirrorCaptureRequests) and tried again after that batch
		 * (pendingSubtrees, once). An attach event's root the mirror has no chain for was out of the world at its attach (its world
		 * attach brings its own event): not tried again.
		 */
		enum class SubtreeSource : std::uint8_t
		{
			AttachEvent,
			CategoryWalk,
			Retry,
		};
		void AddSubtree(RE::NiAVObject* a_root, SubtreeSource a_source = SubtreeSource::AttachEvent);
		std::vector<RE::NiPointer<RE::NiAVObject>> pendingSubtrees;
		std::vector<RE::BSGeometry*> validationSuspects;  // ValidateSlice's: keys, checked again only while tracked
		std::uint64_t subtreesPended = 0, subtreesDropped = 0;  // since the last report: waiting for the mirror, and still without a chain after
		std::uint64_t categoryChildrenMissing = 0;  // a new category node the capture listed no children of (T6b1c: should not be)
		/**
		 * @brief Tracks a_geometry under a_categoryNode. Its reference is the batch's pin of it (T6b1c), or with a_live a live walk's
		 * pointer (the category walk's, until the render thread's category capture pins); without either it is left out (counted).
		 */
		void AddGeometry(RE::BSGeometry* a_geometry, RE::NiNode* a_categoryNode, Ineligible a_parentReason, bool a_live);
		/**
		 * @brief T6b1c: the applied batch's pins by key (its attach captures' and leaf updates' (T6b1b: with the properties), the category
		 * capture's), valid while the scene work applies it; and a reference made from one (null: no pin). Counted since the last report:
		 * references made from pins, from a live walk's pointers, and refused for want of a pin.
		 */
		ankerl::unordered_dense::map<const void*, RE::NiRefObject*> batchPins;
		void PinBatch(const std::vector<RE::NiPointer<RE::NiRefObject>>& a_pins)
		{
			for (const auto& pin : a_pins) {
				batchPins.insert_or_assign(pin.get(), pin.get());
				++referenceStats.pins;
			}
		}
		template <class T>
		RE::NiPointer<T> Pinned(const T* a_key)
		{
			if (const auto it = batchPins.find(a_key); it != batchPins.end()) {
				++referenceStats.pinned;
				return RE::NiPointer<T>(static_cast<T*>(it->second));
			}
			return nullptr;
		}
		struct ReferenceStats
		{
			std::uint64_t pins = 0, pinned = 0, live = 0, refused = 0;
			std::uint64_t properties = 0, propertiesRefused = 0;  // T6b1b: a tracked geometry's properties held anew, and with no pin
			std::uint64_t fadeNodes = 0, fadeNodesRefused = 0;   // T6b3e: their fade nodes held anew, and with no pin
			// T6b3e: of the refused, copied from a root owner (the node is another tracked reference's root), asked of the render thread, and
			// its answers: held, or none (the property names another node now, or the geometry left the world); the first few named.
			std::uint64_t fadeNodesFromRoots = 0, fadeNodesRequested = 0, fadeNodesAnswered = 0, fadeNodesAnsweredNone = 0;
			std::uint32_t fadeNodeNamed = 0;
			std::string fadeNodeFirst;
			std::uint64_t fadeRootsUnowned = 0;                  // T6b3e: a tree or fade root left unlisted for want of its entry's reference
			std::string firstPropertyRefused;
		} referenceStats;
		/** @brief T6b1b: a_entry's properties as the mirror's geometry record names them, each held anew from the batch's pins. */
		void HoldProperties(Tracked& a_entry);
		/** @brief T6b1b: Light Limit Fix's room node for an object (GetParentRoomNode), from the mirror's chain. */
		const RE::NiNode* RoomNodeOf(const void* a_object) const;
		/** @brief T6b1b: records written without the mirror's leaf (none yet), and evaluated again when a capture named it. */
		struct LeafStats
		{
			std::uint64_t missing = 0, rescheduled = 0;
		} leafStats;
		/**
		 * @brief Skylighting::OcclusionTechnique (its radius ignored: the view's BuildDraws test it), from the records (T6b1b): the nearest
		 * fade node above the leaf's BSX flags walked up the mirror.
		 */
		std::uint32_t OcclusionTechniqueOf(const SceneCapture::LeafView& a_leaf, bool a_skylighting) const;
		void ValidateSlice();
		/** @brief The main camera's batch renderers, for the parity's filter of the capture (DrainCapture, observed frames only). */
		bool RefreshMainBatchRenderers();
		// The main renderers' registrations of the frame (DrainCapture), for the scene pass's diagnostics (CheckRegistrations).
		struct CapturedRegistration
		{
			const RE::BSGeometry* geometry = nullptr;
			std::uint32_t hint = 0;
		};
		// A reflection residue geometry (DrainCapture -> ClassifyResidue) with its names, read by the render thread at the drain: the
		// scene pass classifies it a frame later (T6b3c), past a Present that may have released it, so it dereferences none untracked.
		struct CapturedResidue
		{
			const RE::BSGeometry* geometry = nullptr;
			std::string names;  // "'name' under 'parent' (RTTI)"
		};
		/**
		 * @brief Render thread (DrainCapture, EarlyPrepass) -> the next scene pass (TakeAccumulateInputs, T6b3c): the frame's
		 * reflection residue and, while observed, its registrations. Latest wins, but for the residue: a post no pass took carries its
		 * residue into the next post (PostRegistrationDrain), so every residue geometry is classified once.
		 */
		struct RegistrationDrain
		{
			std::vector<CapturedResidue> residue;
			std::vector<CapturedRegistration> registrations;
			bool observed = false;  // the frame's registrations were observed (CS_DCLF_PERSISTENT_PARITY), so registrations holds them
		};
		/**
		 * @brief Render thread, EarlyPrepass: drains the capture every frame (its fixed-capacity buffer, the withholding counters, the
		 * reflection residue), and only while observed (CS_DCLF_PERSISTENT_PARITY; the decal order probe its own switch) reads what
		 * was registered: the Lighting shader parity, the frame's lighting pass (below), the registrations CheckRegistrations checks,
		 * into a_drain. The normal path reads none of it (T6b2c step 8).
		 */
		void DrainCapture(RegistrationDrain& a_drain);
		/** @brief Render thread (PostAccumulateInputs): the drain posted for the next scene pass, an unread post's residue carried in. */
		void PostRegistrationDrain(std::shared_ptr<RegistrationDrain> a_drain);
		/** @brief Render thread, the frame's start (BeginFrame, the coordinator idle): the engine's BSLightingShader instance, once. */
		void CaptureLightingShader();
		// Render thread (PostRegistrationDrain) -> the scene pass (TakeAccumulateInputs): the newest drain not taken yet. The pass's
		// own: the newest taken (CheckRegistrations, ClassifyResidue), null until the first.
		std::atomic<std::shared_ptr<const RegistrationDrain>> registrationDrainPosted;
		std::shared_ptr<const RegistrationDrain> registrationDrain;
		/** @brief Render thread (PostAccumulateInputs): LightLimitFix's room map copied and posted when its generation moves. */
		void PostRoomMap();
		/** @brief The scene pass, before the walk (T6b3c): the frame inputs posted since its last pass (room map, pipeline frame, drain). */
		void TakeAccumulateInputs();
		/** @brief The scene pass, after the joins: a used pipeline without a current PerGeometry block made from the pipeline frame. */
		void MakeNewPipelineConstants();
		// The joins' frame inputs (T6b2c step 8): render thread -> scene lane, latest wins (a pointer swap each way; a post
		// the scene lane never took is freed by the next post or by the take, on whichever thread: plain memory, nothing of the engine's
		// or the GPU's).
		std::atomic<std::shared_ptr<const GeometryPort::PipelineFrame>> pipelineFramePosted;
		std::shared_ptr<const GeometryPort::PipelineFrame> scenePipelineFrame;  // the scene lane's: the newest taken
		struct AccumulateInputCounters
		{
			std::atomic<std::uint64_t> roomMapsPosted{ 0 }, roomMapsTaken{ 0 }, pipelineFramesPosted{ 0 }, pipelineFramesTaken{ 0 };
			std::atomic<std::uint64_t> pipelineBlocksMade{ 0 }, pipelineBlocksWaited{ 0 };
		} accumulateInputStats;
		/**
		 * @brief A Lighting pass the main camera registered this frame with its light list (FindLightingPass's test), valid
		 * until the frame's accumulator is cleared: the template parity's fallback reference (RegisteredTemplatePassOf). Set only on
		 * observed frames (DrainCapture); null otherwise.
		 */
		const RE::BSRenderPass* frameLightingPass = nullptr;
		/** @brief The pass a pipeline's template evaluates from: the property's own Lighting pass, else the frame's. */
		/**
		 * @brief The pass the parity evaluates a pipeline's PerGeometry block with (T6; since T6b2b the block is GeometryPort's, so this
		 * is CS_DCLF_PERSISTENT_PARITY's reference alone): a synthetic one, of no registration. The template
		 * member's geometry and property, the Lighting shader, and the frame's lights SetupGeometry reads: the sun first
		 * (ShadowSceneNode::sunLight), then a shadow light for every further one the descriptor counts (FUN_1414df650 reads
		 * sceneLights[1..n] by the descriptor's counts, a shadow light's mask index at +0x520; numLights 1). Only per-object outputs read them,
		 * which every DCLF draw overrides. Null without a template member or the lights. Render thread; valid until the next call.
		 */
		const RE::BSRenderPass* TemplatePassOf(const Tables& a_view, std::uint32_t a_pipeline);
		/** @brief The parity's: a Lighting pass the engine built for a_property (its last GetRenderPasses), else the frame's registered one. */
		const RE::BSRenderPass* RegisteredTemplatePassOf(RE::BSShaderProperty* a_property) const;
		RE::BSRenderPass syntheticTemplate{};
		std::array<RE::BSLight*, 8> templateLights{};

	public:
		/** @brief The reflection residue's classes (T6): Ineligible's reasons, then these. */
		static constexpr std::size_t kResidueUntracked = static_cast<std::size_t>(Ineligible::Count), kResidueUnbound = kResidueUntracked + 1,
									 kResidueNotReflection = kResidueUntracked + 2, kResidueReflection = kResidueUntracked + 3, kResidueKinds = kResidueUntracked + 4;
		static constexpr std::size_t kAgeBuckets = 8;
		static constexpr std::array<const char*, kAgeBuckets> kAgeBucketNames{ "0", "1", "2", "3", "4-7", "8-15", "16-63", "64+" };
		static constexpr std::size_t AgeBucket(std::uint32_t a_frames)
		{
			return a_frames < 4 ? a_frames : a_frames < 8 ? 4 : a_frames < 16 ? 5 : a_frames < 64 ? 6 : 7;
		}
		/** @brief The furthest stage a residue geometry reached (T6b0). */
		enum ResidueStage : std::uint8_t
		{
			kStageUntracked,
			kStageIneligible,     // a verdict other than hidden
			kStageHiddenNow,      // hidden, and its chain is hidden now
			kStageHiddenStale,    // hidden, and its chain is shown now: a show the verdict was not taken again for
			kStageNoRecord,       // eligible, no record
			kStageUnbound,        // a record, not bound
			kStageWaiting,        // bound, waiting for readiness (waitingBy)
			kStageNotReflection,  // bound, not waiting, no reflection phase
			kStageApplied,        // a reflection member in the coordinator's tables, not installed yet
			kStageUngated,        // T6b5: shown since, not ready, and no LOD gate ever tagged it (a show the gates do not cover)
			kStageCount
		};
		static constexpr std::array<const char*, kStageCount> kStageNames{ "untracked", "ineligible", "hidden", "hidden, shown since", "no record", "a record, unbound",
			"bound, waiting", "bound, no reflection phase", "a member, not installed", "shown with no gate" };
		struct ResidueClasses
		{
			std::array<std::uint64_t, kResidueKinds> counts{};
			std::array<std::string, kResidueKinds> first;
			struct Seen
			{
				std::uint32_t frame = ~0u, frames = 0;
			};
			ankerl::unordered_dense::map<const void*, Seen> seen;
			std::uint64_t persistent = 0;
			// T6b0 (CS_DCLF_TIMELINE): by the furthest stage reached, with the frames since it was tracked (kAgeBuckets).
			std::array<std::uint64_t, kStageCount> stages{};
			std::array<std::array<std::uint64_t, kAgeBuckets>, kStageCount> ages{};
			std::array<std::string, kStageCount> stageFirst;
			std::array<std::uint64_t, 14> waitingBy{};  // kStageWaiting: SetStats::waitingBy's reasons
			std::array<std::uint64_t, 6> unboundBy{};   // kStageUnbound: its last membership pass's failure (0: none failed)
			// kStageUnbound: the accumulate join's verdict that left it without bindings since its record (Tracked::accumulateReason).
			std::array<std::uint64_t, static_cast<std::size_t>(Ineligible::Count)> unboundJoin{};
			// kStageHiddenStale: the store of the last show on its chain (HiddenStoreSiteAt's index; ~0u: none seen), and the frames since.
			ankerl::unordered_dense::map<std::uint32_t, std::uint64_t> staleSites;
			std::array<std::uint64_t, kAgeBuckets> sinceShow{};
		};
		/** @brief Render thread, the reflection report: the classes since the last call. */
		ResidueClasses TakeResidueClasses();
		/** @brief T6b0: the frames from an event to the set, by geometry joining (kAgeBuckets), since the last call. */
		struct TimelineStats
		{
			std::array<std::uint64_t, kAgeBuckets> attachToMember{};  // a geometry's first join since it was tracked
			std::array<std::uint64_t, kAgeBuckets> writtenToBound{};  // a record taken anew to its binding
			std::array<std::uint64_t, kAgeBuckets> writtenToMember{};  // a record taken anew to its join
			std::array<std::uint64_t, kAgeBuckets> showToMember{};  // a stale hidden verdict's show (its chain's last) to its join
			std::uint64_t joins = 0, firstJoins = 0, bound = 0;
			std::array<std::uint64_t, 6> bindFailures{};  // membership passes that gave none, by PrimaryCull::LastSyntheticFail
			std::string bindFailFirst;
		};
		TimelineStats TakeTimelineStats();

	private:
		/** @brief A cheap hash of everything RefreshCategoryNodes reads to find category nodes. */
		std::uint64_t CategorySignature() const;
		// a_accumulated: the frame's registered pass, whose captured fade state then stands in for the live one.
		Ineligible ClassifyFrame(const Tracked& a_tracked, const AccumulatedPass* a_accumulated = nullptr) const;
		/**
		 * @brief Whether a node is hidden for the walk's views: the frame's capture for the nodes whose kHidden bit the engine
		 * flips while the asynchronous walk runs (FrameGlobals::cullHidden, taken on the render thread just before the walk is
		 * kicked, sorted by pointer), else the node's own bit.
		 *
		 * - ShadowSceneNode::OnVisible hides a portal graph's always-render children and its shared node for the
		 *   room traversal, then restores each bit. It runs in the main camera's cull jobs.
		 * - TESWaterReflections::Update hides the player's 3D while a cube-map reflection updates, then restores
		 *   it. Main::Draw calls it on the render thread while the cull jobs run.
		 * - Main::Draw hides the player's first-person skeleton just after the walk is kicked and keeps it hidden
		 *   for every world view: it is listed as hidden.
		 *
		 * Reading the nodes instead took the player's face shapes (classified every frame) out of the tables on
		 * the frames a reflection updated.
		 */
		bool HiddenForWalk(const RE::NiAVObject* a_object) const;
		/** @brief HiddenForWalk from a_object's record (T6b1b: the mirror's hidden bit where the frame's capture has none). */
		bool HiddenForWalk(const void* a_object, const SceneCapture::NodeRecord& a_record) const;
		/** @brief SwitchSelects from the switch's record (T6b1b): its selected child is a_child, and current. */
		static bool SwitchSelects(const SceneCapture::NodeRecord& a_switch, const void* a_child)
		{
			return (a_switch.kind & SceneCapture::kKindSwitch) && a_switch.switchChild && a_switch.switchChild == a_child && a_switch.switchCurrent;
		}

		ankerl::unordered_dense::map<RE::BSGeometry*, Tracked> tracked;
		SceneIdentity sceneIdentity;
		/**
		 * @brief BuildFrame's iteration order: the objects the engine kept, then the rest.
		 *
		 * Members rather than locals so their capacity survives the frame, and each entry carries the
		 * accumulated pass it was found under so the loop does not look it up a second time.
		 */
		struct OrderEntry
		{
			RE::BSGeometry* geometry;
			Tracked* tracked;  // mutable: BuildFrame updates the cached verdict through it
			const AccumulatedPass* accumulated;
			bool layer = false;  // the accumulate phase's: the entry's layer (Tracked::layerSlot) rather than its base
		};
		std::vector<OrderEntry> order;
		// Lookup keys are valid only for this walk; the captured recordId is durable.
		ankerl::unordered_dense::map<const RE::BSFaceGenNiNode*, std::shared_ptr<const FaceSnapshots::HeadView>> capturedFaceHeads;
		// The accumulate phase's iteration: the frame's membership joins. A member for its capacity, like `order`.
		std::vector<OrderEntry> accumulateOrder;
		// Member decals (DecalOrder.cpp): object -> its chain, the decal key's group, technique and sub-pass. Joined with the
		// membership patch, left with the membership (DropResidentSlot). Their draw order within a chain is the scene's
		// (CS_DCLF_DECAL_ORDER, default stable) or the engine's scene lists' of the frame (=engine).
		ankerl::unordered_dense::map<std::uint32_t, std::uint64_t> memberDecals;
		// The decals whose membership or key changed since the last OrderDecals (joined, patched again, left), and whether the
		// order is to be made again whole (the members cleared).
		std::vector<std::uint32_t> decalsChanged;
		bool decalsRebuild = true;
		void NoteDecalChanged(std::uint32_t a_object) { decalsChanged.push_back(a_object); }
		void NoteDecalsCleared()
		{
			decalsChanged.clear();
			decalsRebuild = true;
		}
		// The member decals in draw order, each with its key (DecalOrder.cpp): kept, so a change re-keys only the decals it names.
		struct KeptDecalOrder;
		std::shared_ptr<KeptDecalOrder> keptDecals;
		/** @brief Tables::decalOrdinal and decalCount: the member decals, in the engine's draw order. */
		void OrderDecals();
		// CS_DCLF_PERSISTENT_PARITY: the kept order against one made whole (keys taken again), on parity frames.
		struct DecalOrderParity
		{
			std::uint64_t checks = 0, decals = 0, differ = 0, staleKeys = 0, unordered = 0;
			std::string first, staleFirst;
		} decalOrderParity;
		// T6b2a: the material port against the engine's evaluation (CheckMaterialPort), and the frame's sample it ran with.
		MaterialPort::ParityStats materialPortParity;
		std::optional<MaterialPort::MaterialFrame> materialPortFrame;
		std::uint32_t materialPortFrameNumber = ~0u;
		// T6b2b: the pipeline template port against the template's evaluation (persistent-parity frames), and the frame sample the held
		// blocks were made from (RefreshFrameConstants' last with a sun; posted to the coordinator too, PostPipelineFrame).
		struct GeometryPortParity
		{
			std::uint64_t checked = 0, differ = 0, uncovered = 0;
			std::string first;
		} geometryPortParity;
		std::optional<GeometryPort::PipelineFrame> geometryPortFrame;
		std::uint32_t geometryPortFrameNumber = ~0u;
		void CheckMaterialPort(const RE::BSShaderMaterial* a_material, std::uint32_t a_pass, const MaterialRecord& a_port, const MaterialPort::MaterialSnapshot& a_snapshot,
			const MaterialRecord* a_engine = nullptr);
		/**
		 * @brief T6b2a: the scene work's material captures (MaterialPort::captures: a material's writer, an attach's leaf, a request), the
		 * newest per material, each with the reference its capture took; one unused for kMaterialSnapshotFrames frames is let go (its
		 * reference to the render thread's releases). The join makes a record from one at once (MaterialPort::Evaluate, the frame's
		 * sources in FrameGlobals); a material with none is asked of the render thread (materialRequestQueue: ServeMaterialRequests
		 * captures it at EarlyPrepass).
		 */
		struct HeldMaterial
		{
			std::unique_ptr<MaterialPort::HeldSnapshot> held;
			std::uint32_t frame = 0;  // taken or used
		};
		static constexpr std::uint32_t kMaterialSnapshotFrames = 8;
		ankerl::unordered_dense::map<const RE::BSShaderMaterial*, HeldMaterial> materialSnapshots;
		struct MaterialSnapshotStats
		{
			std::uint64_t captured = 0, replaced = 0, stale = 0, aged = 0, made = 0, uncovered = 0, requested = 0;
		} materialSnapshotStats;
		/** @brief The scene work (ApplyEvents): the captures pushed since the last call, the newest kept; the render thread's answers to requests. */
		void DrainMaterialCaptures();
		/** @brief A capture's reference, to the render thread's releases (materialsHandedBack). */
		void HandBackHeld(std::unique_ptr<MaterialPort::HeldSnapshot>&& a_held);
		/**
		 * @brief T6b2c step 7: the material records kept by the scene work (FrameConstants.cpp), each scene pass after the captures are
		 * drained, from its frame's sources (FrameGlobals::Current().material), no engine read:
		 * - a material captured since the last pass (its writer's, its attach's, a request's): every slot of it evaluated again from the
		 *   capture (MaterialPort::Evaluate), its own values written where they differ (RewriteCapturedMaterials);
		 * - the frame-sourced components: per signature, one port evaluation of a kept capture (a probe, pure data) against the frame's
		 *   sources, written into every slot of the signature when it moves (RefreshMaterialSignatures);
		 * - TexcoordOffset: a material captured or keyed is watched while its two buffers differ (and two passes after), the frame's
		 *   buffer written into its slots (RefreshMaterialTransforms).
		 * The tables' versions and material log carry the writes to the bindings and the publication. Replaces the render thread's
		 * frame copies (FrameTables' materials), their posts and the coordinator's inbox.
		 */
		void RefreshMaterialRecords();
		void RewriteCapturedMaterials();
		void RefreshMaterialSignatures();
		void RefreshMaterialTransforms();
		/** @brief The scene work: a slot's record made (a join) or written from a_snapshot: listed under its signature, its material watched. */
		void NoteMaterialRecord(std::uint32_t a_slot, const MaterialPort::MaterialSnapshot& a_snapshot);
		void WatchMaterialTransforms(const RE::BSShaderMaterial* a_material, const MaterialPort::MaterialSnapshot& a_snapshot);
		/** @brief The tables reset (ResetSlotTables): the signatures' slot lists, the watch and the retries dropped. */
		void ResetMaterialRecords();
		// The materials whose capture DrainMaterialCaptures kept since the last RefreshMaterialRecords, and those a rewrite could not
		// evaluate yet (an Advanced Skin key not set up: asked again while their capture is held).
		std::vector<const RE::BSShaderMaterial*> materialsCaptured;
		std::vector<const RE::BSShaderMaterial*> materialRewritesPending;
		struct MaterialSignature
		{
			std::vector<std::uint32_t> slots;      // listed when keyed (materialSignatureListed), dropped as the list is walked
			MaterialPort::MaterialSnapshot probe;  // a capture of one of its materials (pure data: no pointer in it is followed)
			std::uint32_t probePass = 0;
			bool probeValid = false;
			bool probeFailed = false;  // its last evaluation failed (an Advanced Skin key not set up): the next slot noted replaces it
			MaterialRecord applied;  // the frame components last written into its slots
			bool appliedValid = false;
		};
		ankerl::unordered_dense::map<std::uint32_t, MaterialSignature> materialSignatures;
		std::vector<std::uint32_t> materialSignatureListed;  // per material slot: its signature + 1, 0 when unlisted
		struct TransformWatch
		{
			MaterialSources::TextureTransforms transforms;  // the newest capture's
			std::uint32_t frame = 0;                         // the frame it was captured or keyed (T6b3d: a frame, not a pass)
		};
		ankerl::unordered_dense::map<const RE::BSShaderMaterial*, TransformWatch> transformWatch;
		struct MaterialRecordStats
		{
			std::uint64_t passes = 0, captured = 0, rewritten = 0, retried = 0, uncovered = 0, samples = 0, applications = 0, slotsApplied = 0,
						  transformsWatched = 0, transformsWritten = 0;
		} materialRecordStats;
		// T6b2c: the material slots' texture bindings, the scene work's (MaterialBindings; SceneStore/MaterialBindings.cpp).
		MaterialBindings materialBindings;
		/**
		 * @brief The scene work, after the records' writers (RefreshMaterialRecords, the joins): the answers drained (the slots waiting on
		 * them written), then the slots whose record changed (the tables' material log) and the slots newly used, each resolved.
		 */
		void UpdateMaterialBindings();
		/** @brief The scene work: a used slot's entry written from its record, its views' bindings asked for where none is known. */
		void ResolveMaterialBinding(std::uint32_t a_slot);
		/** @brief The scene lane, before a commit or a publication: the entries written since the last call, each a new version, logged. */
		void VersionMaterialBindings();
		/** @brief A view's binding: answered in this drain, cached from an earlier answer, else asked for (pending; a_slot waits on it). */
		GpuTextures::Binding MaterialViewBinding(ID3D11ShaderResourceView* a_view, std::uint32_t a_sourceTag, std::uint32_t a_slot);
		/** @brief The scene work: a material slot's retirement came back (no publication names it): its entry dropped. */
		void RetireMaterialBinding(std::uint32_t a_slot);
		/** @brief The tables reset (ResetSlotTables): every entry and request dropped; an answer to an earlier request is stale. */
		void ResetMaterialBindings();
		// T6b2c: the shared, projected, shadow mask and shadow texture bindings, the scene work's (SharedBindings; SceneStore/SharedBindings.cpp).
		SharedBindings sharedBindings;
		/**
		 * @brief The scene work, after the technique rows' refresh: the answers drained, then the fixed bindings and the projected capture
		 * taken where they are new, each used pipeline's technique mask resolved, and the shadow textures the dependency index added or
		 * removed since the last pass (Tables::shadowTextureChanges) asked for or let go.
		 */
		void UpdateSharedBindings();
		/** @brief A view's binding: answered in this drain, cached from an earlier answer (here or the material bindings'), else asked for. */
		GpuTextures::Binding SharedViewBinding(ID3D11ShaderResourceView* a_view, std::uint32_t a_sourceTag, SharedBindings::Kind a_kind);
		/** @brief The scene work: the projected entries from the capture they are for, the pending ones asked for. */
		void ResolveProjectedBindings(std::uint32_t a_textures);
		/** @brief The scene work: a used pipeline's technique mask entry from its row. */
		void ResolveMaskBinding(std::uint32_t a_slot, ID3D11ShaderResourceView* a_frameMask);
		/** @brief The scene work: a shadow texture's entry from an answer or a known binding, asked for when there is none. */
		void ResolveShadowTextureBinding(ID3D11ShaderResourceView* a_view);
		/** @brief The scene work: a pipeline slot's lookups retirement came back (no publication names it): its mask entry dropped. */
		void RetireMaskBinding(std::uint32_t a_slot);
		/** @brief The tables reset (ResetSlotTables): every entry and request dropped (but the render thread's posted capture). */
		void ResetSharedBindings();
		/**
		 * @brief T6b2a: a material record from the port (the caller's thread reads the material now; the frame's sources from FrameGlobals),
		 * the engine's SetupMaterial never run; under CS_DCLF_PERSISTENT_PARITY checked against it (CheckMaterialPort).
		 */
		bool PortMaterial(const RE::BSShaderMaterial* a_material, std::uint32_t a_pass, MaterialRecord& a_out);
		ankerl::unordered_dense::set<RE::NiNode*> categoryNodes;
		// The portal graphs' parentless roots (alwaysRenderChildren), each filed under its graph's shared portal node, a
		// category node: FindCategoryNode, RefreshCategoryNodes. The root is held while it is listed here.
		struct AlwaysRenderRoot
		{
			RE::NiPointer<RE::NiAVObject> root;
			RE::NiNode* category = nullptr;
		};
		ankerl::unordered_dense::map<const RE::NiAVObject*, AlwaysRenderRoot> alwaysRenderRoots;
		// Diagnostics: the frame each category node was found and the refresh's cause (GetCategoryInfo), and
		// the source AddGeometry stamps on new entries.
		ankerl::unordered_dense::map<const RE::NiNode*, std::pair<std::uint32_t, std::uint8_t>> categoryFound;
		TrackSource addSource = TrackSource::AttachEvent;
		std::size_t validationCursor = 0;
		// Set while a load screen is up, so the first frame after it rebuilds the tracked set from
		// scratch instead of trusting anything discovered across the load (ProcessEvents).
		bool rescanPending = false;
		// The frame's engine globals (FrameGlobals::Capture at BeginFrame): the render thread's (FrameValues binds them for its seeds).
		std::shared_ptr<const FrameGlobals> frameGlobals;
		/**
		 * @brief What the coordinator's passes take of the frame (T6b3a), posted latest-wins by the render thread after BeginFrame's
		 * capture and taken at the start of each scene pass (TakeFrameInputs): the engine globals the passes read (FrameGlobals::Scope),
		 * the frame number (sceneFrame), the membership witness the commit and the accumulate phase's binds read (step 6e F1: frame
		 * globals are captured, never read by the scene work), the toggles generation and the verdicts generation (a toggle that enters
		 * the classification: the coordinator drops its cached verdicts, InvalidateVerdicts). T6b3d: the post is a message that wakes the
		 * pump (folded with the frame start's other posts: ScenePassWakeBatch); a pass reads only what it took, and keeps the last it took
		 * while none newer is posted (passes between frames).
		 */
		struct FrameInputs
		{
			std::shared_ptr<const FrameGlobals> globals;
			std::uint32_t frame = 0;
			std::uint32_t membershipWitness = 0;
			std::uint32_t togglesGeneration = 0;
			std::uint32_t verdictsGeneration = 0;
			std::uint64_t lodForcedGeneration = 0;  // T6b5: LodGates::ForcedGeneration at the frame's start (ScenePublication's)
		};
		LatestSlot<FrameInputs> frameInputsSlot;
		std::uint64_t frameLodForced = 0;     // T6b5, render thread: the frame's LodGates::ForcedGeneration (BeginFrame)
		std::uint64_t publishedLodForced = 0; // T6b5, the coordinator's: the generation of its last publication (NotePublished)
		FrameInputs sceneInputs;              // the coordinator's: the newest taken
		std::uint32_t verdictsRequested = 0;  // render thread: bumped by each request to drop the cached verdicts
		std::uint32_t verdictsApplied = 0;    // the coordinator's: the requests it has applied
		/** @brief Render thread, after BeginFrame's capture: the frame's inputs posted for the coordinator. */
		void PostFrameInputs();
		/** @brief The coordinator, first in each scene pass: the newest frame inputs taken (sceneInputs, sceneFrame), the verdicts dropped when asked. */
		void TakeFrameInputs();
		// The coordinator's frame number (the frame inputs' it last took) and its passes: GetFrame on a scene work thread, PassSerial.
		std::uint32_t sceneFrame = 0;
		std::uint64_t passSerial = 0;
		// LightLimitFix's room map (its room nodes' indices), copied by the render thread when its generation moves and posted, latest
		// wins (PostRoomMap, T6b2c step 8); the scene pass takes it (TakeAccumulateInputs) and reads only its own copy (roomMap,
		// roomMapGeneration: the scene lane's), never the map the render thread swaps.
		struct RoomMapInput
		{
			std::shared_ptr<const ankerl::unordered_dense::map<const RE::NiNode*, int>> map;
			std::uint64_t generation = 0;
		};
		std::atomic<std::shared_ptr<const RoomMapInput>> roomMapPosted;
		std::uint64_t roomMapPostedGeneration = ~0ull;  // render thread: the generation last posted
		std::shared_ptr<const ankerl::unordered_dense::map<const RE::NiNode*, int>> roomMap;
		std::uint64_t roomMapGeneration = ~0ull;
		// Render thread: the first frame's start after a load catches up every switch in the world (CatchUpSwitches), and its category
		// capture is forced (as one after a pass saw a detach: categoryDetachSeen).
		bool worldCatchUpPending = false;
		bool categoryCapturePending = false;
		/**
		 * @brief The scene mirror (step 6e F3): the hooks' records, applied by the scene work in event order (ApplyMirrorEvents).
		 * Beside the live reads for now, checked by CS_DCLF_MIRROR_PARITY: the render thread's probe (ProbeMirror, a slice of the
		 * tracked set captured live at the frame's start) against it after the frame's events (CheckMirror).
		 */
		SceneMirror mirror;
		void ApplyMirrorEvents(const EventBatch& a_batch);
		/**
		 * @brief T6b3d, render thread, the frame's start (CS_DCLF_MIRROR_PARITY): the slice the coordinator asked for (PostMirrorProbeRequest:
		 * tracked geometries, each held by the request) captured live with their chains up to the world's root, and posted
		 * (mirrorProbePosted) for the next pass, which takes it before its drains (so every event older than the capture is applied
		 * first, and every newer one names its object: CheckMirror leaves those out).
		 */
		void ProbeMirror();
		/** @brief T6b3d, the coordinator, once a frame (CS_DCLF_MIRROR_PARITY): the next slice of the tracked set asked of the render thread. */
		void PostMirrorProbeRequest();
		void CheckMirror();
		// The coordinator's (T6b3d): a load screen's attach and detach events, for the mirror (the first pass after the load takes them).
		std::shared_ptr<EventBatch> loadingCarry;
		std::size_t mirrorProbeCursor = 0;
		// T6b3d: the mirror probe's slice, the coordinator -> the render thread (each geometry held: the render thread drops them), the
		// records back (they hold nothing), and whether one is asked and not served (one at a time, once a frame: mirrorProbeFrame).
		struct MirrorProbeRequest
		{
			std::vector<RE::NiPointer<RE::BSGeometry>> geometries;
		};
		EventQueue<std::shared_ptr<MirrorProbeRequest>, 16> mirrorProbeRequests;
		LatestSlot<SceneCapture::Records> mirrorProbePosted;
		std::atomic<bool> mirrorProbeAsked{ false };
		std::uint32_t mirrorProbeFrame = ~0u;
		// The scene work: the applied batch's probe, and the objects the frame's events named (the parity's).
		std::unique_ptr<SceneCapture::Records> mirrorProbe;
		SceneMirror::KeySet mirrorEventKeys;
		SceneMirror::FieldMap mirrorEventFields;
		std::atomic<const void*> mirrorWatchRequest{ nullptr };
		std::atomic<const void*> mirrorWatchNode{ nullptr };      // CS_DCLF_MIRROR_WATCH=parity:current: a fade node found stale in its currentFade
		std::atomic<const void*> mirrorWatchAlpha{ nullptr };     // ... and its stale alpha property  // the scene work's stale property for the render thread's watch  // the scene work's: the fields the batch's updates carried (step 6e F3b)  // render thread: the next frame start's capture is forced (a detach ingested, a load's end)
		std::vector<RE::NiAVObject*> attachedRoots;  // the ingestion's attached subtrees, for CatchUpSwitches (scratch)
		// Render thread, since the last report: switches caught up at ingestion (by switch event, under an attached subtree or the
		// world after a load), and the time taken.
		std::atomic<std::uint64_t> catchUpsBySwitch{ 0 }, catchUpsByAttach{ 0 };
		std::atomic<std::uint64_t> catchUpNs{ 0 };

		Tables tables;
		// Step 6: the tables as the scene work left them, published as an immutable snapshot (PublishTables, the coordinator), and the
		// one the frame accepted (AcceptTables). Snapshots are pooled: one no frame holds any more is written again.
		std::vector<std::shared_ptr<Tables>> tablesPool;
		// Parallel to tablesPool: whether the snapshot equals the tables as they stood at its change log's end (6d: it is brought up
		// to date by replay); a snapshot just made is copied whole once.
		std::vector<std::uint8_t> tablesPoolReplayable;
		std::vector<std::uint8_t> replaySlotMarks;  // scratch: the object slots a replay copied
		// The last published snapshot (the coordinator's), and the one the frame accepted (the render thread's: its installed
		// publication's, the newest taken while none is installed). The pool writes no snapshot anyone holds.
		std::shared_ptr<Tables> publishedTables;
		std::shared_ptr<const Tables> acceptedTables;
		// Step 6c, T6b3d: whether passes may run beside the frame (the pump started and its passes not inline: from then on, at any
		// moment) and the lane's thread; a frame-side read of the coordinator's state then is a defect, counted and named
		// (GuardFrameAccess).
		std::atomic<bool> sceneTaskInFlight{ false };
		std::atomic<std::uint32_t> sceneLaneThread{ 0 };
		mutable std::atomic<std::uint64_t> frameAccessViolations{ 0 };
		mutable std::atomic<const char*> frameAccessFirst{ nullptr };
	public:
		/** @brief A thread running scene work (the lane, or a parallel loop's chunk of it): no frame-side access to guard. */
		static inline thread_local bool sceneWorkThread = false;
		void GuardFrameAccess(const char* a_what) const
		{
			if (!sceneTaskInFlight.load(std::memory_order_relaxed) || sceneWorkThread)
				return;
			if (frameAccessViolations.fetch_add(1, std::memory_order_relaxed) == 0)
				frameAccessFirst.store(a_what, std::memory_order_relaxed);
		}

	private:
		// The frame's copies (the render thread's), taken at its start from the newest publication taken and the publication log
		// (HandOverAtFrameStart).
		std::shared_ptr<const SunCandidates> frameSunCandidates, frameLightCandidates;
		std::uint32_t frameSunGeneration = 0, frameLightGeneration = 0;
		std::uint64_t frameLightEntriesAppeared = 0;
		std::uint32_t frameTablesGeneration = 0;  // the frame's tables' (GetTablesGeneration)
		std::vector<const RE::NiAVObject*> frameSwitchChanges;
		bool frameSwitchResync = true;
		bool switchResyncNext = false;  // NoteLoadingScreen: PrimaryCull reads every switch again from the next frame's start
		std::vector<std::shared_ptr<const void>> frameRetiredImports;
		std::vector<std::shared_ptr<const PlacementPlan>> framePlans;
		std::vector<ShadingItem> frameShading;
		std::vector<FadeSeedItem> frameFadeSeeds, frameTreeSeeds;
		// The newest publication's category nodes (IsCategoryNode), and the coordinator's last copy with whether its set changed since
		// (copied again at the next publication: PublishScene).
		std::shared_ptr<const ankerl::unordered_dense::set<RE::NiNode*>> frameCategoryNodes, publishedCategoryNodes;
		bool nodeSetsDirty = true;  // the coordinator's: the category nodes changed
		// The light entries (IsLightEntry): a key of lightDependents gaining its first geometry, losing its last, or every key gone, as
		// the coordinator's changes (onto the log: PublicationDeltas::lightEntries) and the render thread's set kept from them. Copying the
		// whole set at each change (a root tracked or untracked: every frame in motion) lengthened the accumulate pass.
		struct LightEntryChange
		{
			const RE::NiAVObject* node = nullptr;
			std::int8_t kind = 0;  // 1 added, -1 removed, 0 every entry cleared
		};
		std::vector<LightEntryChange> lightEntryChanges;               // the coordinator's, since the last publication
		ankerl::unordered_dense::set<const RE::NiAVObject*> frameLightEntrySet;  // the render thread's
		/** @brief The coordinator: whether a node is one of its category nodes (its own set; the frame reads IsCategoryNode). */
		bool CategoryNodeOwn(const RE::NiAVObject* a_node) const
		{
			return categoryNodes.contains(static_cast<RE::NiNode*>(const_cast<RE::NiAVObject*>(a_node)));
		}
		// CaptureWetness's actor index (T6b3a): the membership the coordinator's walks set (Tables::actorWetnessChanges, through the log),
		// the values and the fan-out the render thread's.
		ActorValueIndex frameActorWetness;
		// To the coordinator (T6b3a: posted where they are made, taken at its next scene pass: TakeCoordinatorInputs). The fade root rows
		// the depth commit holds (latest wins), PrimaryCull's fade ownership (a whole set: latest wins) and its reseed of every owned root.
		static constexpr std::uint64_t kNoFadeRootsSent = ~0ull;
		std::atomic<std::uint64_t> fadeRootsSentPosted{ kNoFadeRootsSent };
		LatestSlot<std::vector<SceneStore::OwnedFadeRoot>> fadeOwnedPosted;
		std::atomic<bool> fadeReseedPosted{ false };
		/** @brief The coordinator, first in its scene pass: what the render thread posted for it since (above), applied in that order. */
		void TakeCoordinatorInputs();
		// The frame's pipeline blocks for the coordinator (step 6e A): posted by the render thread (PostPipelineConstants) through a
		// lock-free queue (T6b3a), applied by the scene work (ApplyConstantsPosts) where the slot still holds what they were made for. The
		// technique rows are the coordinator's own (RefreshTechniqueRows).
		struct PipelineConstantsPost
		{
			std::uint32_t slot = 0, binding = 0, generation = 0;
			PipelineKey key{};
			GeometryConstants constants{};
		};
		EventQueue<std::unique_ptr<PipelineConstantsPost>, 256> pipelineConstantsPosted;  // T6b3d: no wake (the frame's next pass takes them)
		// The material records are the scene work's own (T6b2c step 7: RefreshMaterialRecords): nothing posted.
		ConstantsPostStats constantsPostStats;
		void PostPipelineConstants(std::uint32_t a_slot);
		void ApplyConstantsPosts();
		// T6b2c: the technique rows' witness, the coordinator's - the frame inputs (FrameGlobals: TechniqueInputs, SetupTechniqueDescriptor's
		// bytes) every row was last evaluated against. The fog is left out: a row keeps the fog it was made with (KeepTechniqueFog).
		struct TechniqueWitness
		{
			TechniqueInputs inputs{};
			std::uint8_t byte12 = 0, byte7 = 0;
			bool valid = false;
		} techniqueWitness;
		/**
		 * @brief The coordinator, at the start of each scene work pass (its frame's FrameGlobals bound): when the frame's technique inputs
		 * differ from the witness, every technique row evaluated again (EvaluateTechniqueRow). New rows are evaluated where they are made
		 * (TechniqueRowFor), after this, from the same sample.
		 */
		void RefreshTechniqueRows();
		/** @brief The coordinator: a technique row from its frame's sample, written and versioned where it differs (a new row in full). */
		void EvaluateTechniqueRow(std::uint32_t a_row);
		/** @brief The frame's tables (T6b3a): the accepted publication's, empty immutable tables before the first (never the coordinator's). */
		const Tables& FrameView() const { return acceptedTables ? *acceptedTables : EmptyTables(); }
		static const Tables& EmptyTables()
		{
			static const Tables empty;
			return empty;
		}
		// The change log's position at the last commit (CommitSet): what the applied set's revocation checks from.
		LogCursor setCommitCursor;
		TablesPublication tablesPublication;
		/** @brief The coordinator, at the end of the scene work: the tables into a pooled snapshot no one holds, published. */
		void PublishTables();
		// The walk whose object indices the Tracked entries' objectStamp must match; a new walk or a teardown
		// takes a new value, which invalidates every entry's index at once.
		std::uint32_t objectStamp = 1;
		void InvalidateObjectIndices() { ++objectStamp; }
		// Set only for the dense rebuild CS_DCLF_WALK_PARITY compares against: the walk then lays the objects out
		// densely and leaves the Tracked entries' slots and indices alone.
		bool denseWalk = false;
		/** @brief The slot the walk writes this object at: its own, a free one, or a new one. */
		std::uint32_t AcquireObjectSlot(Tracked& a_tracked, RE::BSGeometry* a_geometry);
		/** @brief After a walk: frees every slot it did not write, and sorts the per-frame index lists. */
		void SweepObjectSlots();
		/**
		 * @brief Frees an entry's slot as the entry leaves the tracked set (render thread, outside the walk), so a
		 * rescan's new entries reuse the slots of the ones it replaces instead of growing the arrays past them.
		 */
		void ReleaseObjectSlot(Tracked& a_entry);
		/** @brief The tracked entry's layer slot for its base slot a_base: its own, or a new one (Tracked::layerSlot). */
		std::uint32_t AcquireLayerSlot(Tracked& a_tracked, RE::BSGeometry* a_geometry, std::uint32_t a_base);
		void ReleaseLayerSlot(Tracked& a_entry);
		/** @brief The property a slot's record draws: its entry's held one (T6b1b: the mirror's), its layer's for a layer slot. */
		RE::BSShaderProperty* SlotProperty(std::uint32_t a_slot) const;
		/** @brief Whether a member's binding still holds for its object as it is now (the derivation cache's witnesses). */
		bool MemberBindingStands(std::uint32_t a_slot, const Tracked& a_entry) const;
		void EraseTracked(RE::BSGeometry* a_geometry);
		/** @brief CS_DCLF_WALK_PARITY=1: every 60 frames, the slot tables against a dense rebuild, object by object. */
		void CheckWalkParity();
		struct WalkParityStats
		{
			std::uint32_t checks = 0;
			std::uint32_t objects = 0;
			std::uint32_t differ = 0;
			std::uint32_t missing = 0;
			std::uint32_t extra = 0;
			std::uint32_t staleVerdicts = 0;  // entries whose kept classification differs from the reference's
			std::uint32_t staleTraits = 0;    // entries the delta walk keeps that a fresh classification would evaluate every frame
			std::string first;
			std::string firstStale;
			std::string firstStaleTraits;
			std::map<std::string, std::uint32_t> byWhat;  // the differences by what differs
		} walkParity;
		// The dense rebuild classifies every entry from scratch, without the verdict caches, and keeps its verdicts
		// here: the classification itself is what walk parity checks the kept one against.
		ankerl::unordered_dense::map<const RE::BSGeometry*, Ineligible> referenceReasons;
		// The per-frame dedup maps. Members, not locals, so their buckets survive the frame
		ankerl::unordered_dense::map<const RE::BSGraphics::TriShape*, std::uint32_t> geometryIndex;
		ankerl::unordered_dense::map<PipelineKey, std::uint32_t, PipelineKeyHash> pipelineIndex;
		ankerl::unordered_dense::map<std::pair<const RE::BSShaderMaterial*, std::uint32_t>, std::uint32_t> materialIndex;
		ankerl::unordered_dense::map<const RE::BSShaderMaterial*, std::vector<std::uint32_t>> materialDependents;
		void ListMaterialDependent(const RE::BSShaderMaterial* a_material, std::uint32_t a_slot);
		void UnlistMaterialDependent(const RE::BSShaderMaterial* a_material, std::uint32_t a_slot);
		// The frame's membership joins' passes (BindByMembership), by geometry.
		ankerl::unordered_dense::map<const RE::BSGeometry*, AccumulatedPass> accumulatedPasses;
		// The frame's membership joins of layers (BindByMembership: PrimaryCull::MembershipLayerPass), by their geometry.
		ankerl::unordered_dense::map<const RE::BSGeometry*, AccumulatedPass> accumulatedLayerPasses;
		// The main camera's accumulator, latched non-null.
		RE::BSGraphics::BSShaderAccumulator* latchedAccumulator = nullptr;
		// The batch renderers the main accumulator draws from (RefreshMainBatchRenderers).
		// Registration is captured from every batch renderer in the game, including the shadow cameras',
		// so the captured set has to be filtered to these before it can be compared or used.
		ankerl::unordered_dense::set<const RE::BSBatchRenderer*> mainBatchRenderers;
		/**
		 * @brief A reference on a BSShaderMaterial, taken and dropped the way the engine does it (AE).
		 *
		 * Materials live in the engine's material database (the manager at 0x143187758), which shares one
		 * material among every property with the same contents. Its release (FUN_1414f7a40, what
		 * BSShaderProperty::SetMaterial calls for the old material) decrements the count under the database's
		 * lock and, at zero, takes the material out of the database before deleting it. A BSTSmartPointer
		 * deletes it directly instead.
		 */
		class MaterialReference
		{
		public:
			MaterialReference() = default;
			MaterialReference(const MaterialReference&) = delete;
			MaterialReference& operator=(const MaterialReference&) = delete;
			MaterialReference(MaterialReference&& a_other) noexcept :
				material(std::exchange(a_other.material, nullptr)) {}
			MaterialReference& operator=(MaterialReference&& a_other) noexcept
			{
				if (this != &a_other) {
					reset();
					material = std::exchange(a_other.material, nullptr);
				}
				return *this;
			}
			~MaterialReference() { reset(); }
			/** @brief Takes a reference on a_material (null: none), then drops the one held. */
			void reset(RE::BSShaderMaterial* a_material = nullptr);
			/** @brief Takes over a count already taken on a_material (a HeldSnapshot's); holds none before. */
			void Adopt(RE::BSShaderMaterial* a_material) { material = a_material; }
			explicit operator bool() const { return material != nullptr; }

		private:
			RE::BSShaderMaterial* material = nullptr;
		};
		// Parallel to Tables::materials: the slot itself is the only record cache.
		// Its engine reference prevents pointer reuse while the slot is live. Last-object-
		// reference events and table resets release it at the safe capture boundary.
		std::vector<MaterialReference> materialOwners;
		/**
		 * @brief Render thread, Prepass: the character light's view this frame (FrameCapture::characterLightView) from the frame's sample
		 * (MaterialSources::FrameCharacterLightView). The records' frame components are the scene work's (RefreshMaterialSignatures).
		 */
		void RefreshCharacterLightView();
		std::mutex residueClassesLock;
		ResidueClasses residueClasses;  // under residueClassesLock
		TimelineStats timelineStats;    // under residueClassesLock
		// T6b0: a node's last show (hidden events, DrainHiddenEvents) -> {frame, the store's index}; pruned by age.
		ankerl::unordered_dense::map<const void*, std::pair<std::uint32_t, std::uint32_t>> unhideKeys;
		// T6b1a: the fade roots' seed requests (TakeFadeSeeds), and a row's coordinator half from the mirror (the seed brings the rest).
		std::vector<FadeSeedItem> fadeSeedRequests;
		std::vector<FadeSeedItem> treeSeedRequests;
		FadeRootStatic FadeRootFromMirror(const void* a_node);
		/** @brief A new generation for root a_root's row, its seed row toggled, and its seed requested. */
		void RequestFadeSeed(std::uint32_t a_root, FadeRootStatic& a_row);
		/** @brief T6b0: the last show on a_geometry's chain up to its category node {frame, store}, or {0, ~0u}; and whether the chain is hidden now. */
		std::pair<std::uint32_t, std::uint32_t> LastShowOnChain(const RE::BSGeometry& a_geometry, const Tracked& a_entry, bool& a_hiddenNow) const;
		/** @brief T6b0: a slot joined the set (CommitSet): its geometry's timeline stamped and the latency counted. */
		void NoteTimelineJoin(std::uint32_t a_slot);
		/** @brief T6b0: a residue geometry's furthest stage into residueClasses (ClassifyResidue's, under its lock); a_names: the drain's. */
		void StageResidue(const RE::BSGeometry* a_geometry, const std::string& a_names, const Tracked* a_entry);
		void ClassifyResidue();
		void CheckRegistrations();
		/** @brief The coordinator's residency of a slot (IsMember is the frame's). */
		bool ResidentObject(std::int32_t a_object) const { return a_object >= 0 && IsResidentSlot(static_cast<std::uint32_t>(a_object)); }
		// Render thread, CS_DCLF_CAPTURE_PARITY alone: the materials written since the last frame (MaterialSources::Drain, at
		// ValidateMaterialSlice), for its diagnostics. The records follow the writes' captures on the scene work.
		ankerl::unordered_dense::set<const RE::BSShaderMaterial*> writtenMaterials;

	public:
		/** @brief CS_DCLF_CAPTURE_PARITY: the materials this frame drained as written (diagnostics; empty without the switch). */
		const ankerl::unordered_dense::set<const RE::BSShaderMaterial*>& GetWrittenMaterials() const { return writtenMaterials; }

	private:
		/** @brief The alarm: a record that disagrees with a live evaluation outside its frame-sourced components. */
		void NoteStaleMaterial(std::uint32_t a_slot, const std::pair<const RE::BSShaderMaterial*, std::uint32_t>& a_key, const MaterialRecord& a_served,
			const MaterialRecord& a_live);
		std::uint32_t materialValidationCursor = 0;
		static constexpr std::uint32_t kMaterialValidationsPerFrame = 8;
		static constexpr std::uint32_t kMaterialValidationStride = 4;
		/**
		 * @brief The parity's suspects (render thread, CS_DCLF_PERSISTENT_PARITY): an installed slot whose record differed from the
		 * engine's evaluation in a part (its own values but IBLParams, its frame components, its TexcoordOffset). The publication lags
		 * the frame (a record is made from the captures and the sample of a frame or two before), so a suspect is evaluated live every
		 * frame from then on (its history) and judged once kMaterialLateFrames have passed: a part the installed record then agrees with
		 * any of the history in was late (the producers delivered a value the material had); a part it agrees with none of is a miss
		 * (STALE, or the frame parts' differ), in motion too: a frozen record falls out of a moving material's history.
		 */
		enum class MaterialPart : std::uint8_t
		{
			Own,
			Frame,
			Transform
		};
		static constexpr std::uint32_t kMaterialParts = 3;
		struct MaterialSuspect
		{
			std::pair<const RE::BSShaderMaterial*, std::uint32_t> key{};
			std::uint32_t frame = 0;     // first seen
			std::uint8_t parts = 0;      // 1 << MaterialPart, the parts that differed
			std::uint32_t version = 0;   // the installed record's Tables::materialVersion when first seen
			std::vector<MaterialRecord> history;  // the live evaluations since, one a frame
		};
		ankerl::unordered_dense::map<std::uint32_t, MaterialSuspect> materialSuspects;  // by slot
		static constexpr std::uint32_t kMaterialLateFrames = 4;
		/** @brief Whether a record and a live evaluation differ in a_part (pure). */
		static bool MaterialPartDiffers(MaterialPart a_part, const MaterialRecord& a_record, const MaterialRecord& a_live, std::uint32_t a_pass);
		/** @brief Pure: the first variable a_part differs in ("PS 34.y 0.2 -> 0.3", "t11"), for the report. */
		static std::string DescribePartDifference(MaterialPart a_part, const MaterialRecord& a_record, const MaterialRecord& a_live, std::uint32_t a_pass);
		/** @brief Render thread, parity: an installed slot's record against a_live in every part; a differing slot becomes a suspect. */
		void JudgeMaterial(std::uint32_t a_slot, const std::pair<const RE::BSShaderMaterial*, std::uint32_t>& a_key, const MaterialRecord& a_record,
			const MaterialRecord& a_live);
		/** @brief Render thread, parity: every suspect evaluated live into its history, the due ones judged against the installed record. */
		void SampleMaterialSuspects();
		/** @brief Render thread, parity: the port of a live capture of the slot's material against a_engine (CheckMaterialPort). */
		void CheckSlotPort(const std::pair<const RE::BSShaderMaterial*, std::uint32_t>& a_key, const MaterialRecord& a_engine);
		LogCursor materialParityCursor;  // the installed tables' material log, read by the parity (the slots written since)
		std::uint32_t frame = 0;
		std::uint32_t tablesGeneration = 0;
		std::uint32_t materialVersions = 0;  // the last Tables::materialVersion handed out

		// T6b2c step 5: the lookups (Lookups.h), the scene lane's: written by its bindings where they are made (MaterialBindings,
		// SharedBindings) and resolved against the pipeline lane's newest catalog before each commit and publication (ResolveLookups),
		// which the set's readiness reads and each publication carries. The render thread reads them only with the coordinator idle.
		Lookups lookups;
		LookupsResolveState lookupsResolve;
		std::shared_ptr<const PipelineCatalog> lookupsCatalog;  // the catalog `lookups` were last resolved against
		// The last publication's lookups (shared again while unchanged: Lookups::ChangeKey), with their catalog; the chunk copies counted.
		std::shared_ptr<const Lookups> publishedLookups;
		std::shared_ptr<const PipelineCatalog> publishedCatalog;
		std::array<std::uint64_t, 6> publishedLookupsKey{};
		std::uint64_t lookupsChunkCopies = 0;
		// The frame's (render thread, HandOverAtFrameStart): the installed publication's lookups and catalog, the newest published while none
		// is installed.
		std::shared_ptr<const Lookups> frameLookups;
		std::shared_ptr<const PipelineCatalog> frameCatalog;
		/**
		 * @brief The scene lane, before a commit (CommitSet) or a publication (PublishScene), with its bindings written: the material
		 * entries versioned (VersionMaterialBindings), then the pipeline entries and the shadow pipelines resolved against the pipeline
		 * lane's newest catalog (DrawPipelines::TakeCatalog; IndirectDraws::ResolveLookups), which becomes lookupsCatalog.
		 */
		void ResolveLookups();
		/** @brief The scene lane, publishing: the lookups as an immutable copy with their catalog (the last copy again if unchanged). */
		void PublishLookups();
		/** @brief The lookups made new (the tables reset): a new instance, its generations past the last, its versions continuing. */
		void ResetLookups();
		// Frame state the scene phase reads once and the accumulate phase reuses, so that both halves of
		// one frame see the same answer even though they run either side of the shadow maps.
		bool sceneBuilt = false;  // the scene phase ran and the records are this frame's
		bool frameResolveBuffers = false;
		bool frameInterior = false;
		std::array<std::uint32_t, 4> frameDecalBias{};
		bool graphWasActive = false;  // resolveBuffers of the previous BuildFrame, to log the flip
		ProjectedTextures projectedTextures;
		/** @brief Fills one object's extras rows (Prepass: the main camera's state is current). */
		/**
		 * @brief An object's extras rows' static parts (Tables::extraRows; the draw completes them from ExtrasFrame and its
		 * placement, CompleteExtras), from its property and geometry now; true when they changed (the caller notes kChangeExtras).
		 */
		bool WriteObjectExtras(std::uint32_t a_object);
		/** @brief CS_DCLF_PERSISTENT_PARITY: the rows complete as the engine's own routines make them now (render thread). */
		bool ReferenceExtras(std::uint32_t a_object, float* a_out);
		struct ExtrasParity
		{
			std::uint64_t objects = 0, differ = 0, staleStatic = 0;
			float maxDifference = 0.0f;
			std::string first;
		} extrasParity;
		void BuildScenePhase();
		void BuildAccumulatePhase();
		// Engine-affine sampling ends before this packet is applied. Its values
		// can be propagated by a publication coordinator without dereferencing a
		// geometry, material, shader property or render pass.
		struct AccumulatePatch
		{
			std::uint32_t object = 0, material = 0, pipeline = 0, flags = 0;
			float fadeDistance = 0.0f;
			ObjectLights lights{};
			ObjectTreeAnim tree{};
			bool projectedUV = false, landBlend = false;
			std::uint64_t decalKey = 0;
		};
		void ApplyAccumulatePatch(const AccumulatePatch& a_patch);
		/** @brief Walk parity's reference: every entry of `order` written from scratch into dense tables (denseWalk). */
		void DenseWalk();
		/** @brief Clears the per-frame tables and the walk's per-frame counters; a_keepIndices: a delta walk's, which keeps the objects' indices. */
		void BeginWalk(bool a_keepIndices = false);

		/**
		 * @brief The scene phase evaluates only the objects whose inputs can have changed, on the render thread at Main::Draw's early hook, and every other
		 * slot keeps its record. What it evaluates:
		 *
		 * - the per-frame objects (Tracked::perFrame), every frame;
		 * - new entries, and entries something marked (pendingEvaluation);
		 * - the dependents of a fade node whose currentFade changed (FadeWatch, fadeDependents);
		 * - the dependents of a shader or alpha property whose flags, material or controllers changed
		 *   (propertyDependents), classified again;
		 * - the entries under a node Havok moved or gave a controller, and the dependents of every sun entry node
		 *   above it (rootDependents), classified again;
		 * - the dependents of a sun entry node something was attached under or detached from;
		 * - a slot whose geometry slot went stale, or was re-resolved in place for another object.
		 *
		 * A classification stands until one of these events (dclf-event-driven-tables.md, "Phase 3"), and
		 * CS_DCLF_WALK_PARITY checks it against a full walk that classifies from scratch.
		 */
		void DeltaWalk();
		/** @brief Lays out the whole tracked set in `order` (a full evaluation, the parity's dense walk). */
		void BuildFullOrder();
		// T6b3e: the walk's evaluation and merge halves (defined with EvaluateRound).
		struct EvalCounters;
		struct TrackedDelta;
		struct EvalResult;
		/**
		 * @brief One entry of the scene walk: writes its record at its slot; false when it gets none this frame. T6b3e: its evaluation
		 * (EvaluateWrite) then its merge (MergeWrite), one after the other (the dense walk's).
		 */
		bool WriteObject(RE::BSGeometry* a_geometry, Tracked& a_tracked, PartTimer& a_timer, Ineligible& a_bucket);
		/**
		 * @brief The entry's layer record, written after its base record (a_base): the base's placement, the second index list,
		 * no shadow, no bindings until it joins with the base (BindByMembership). a_member: the base is a main-pass record
		 * (verdict None); without it, or without a layer, the layer slot is released. a_keepMember: the base kept its binding.
		 */
		void WriteLayer(RE::BSGeometry* a_geometry, const SceneCapture::LeafView& a_leaf, Tracked& a_tracked, std::uint32_t a_base, bool a_member, bool a_keepMember,
			PartTimer& a_timer);
		/** @brief Why an entry's inputs change from frame to frame (Tracked::perFrame): PerFrameTrait bits, 0 when they do not. */
		enum PerFrameTrait : std::uint32_t
		{
			kTraitFace = 1u << 0,
			kTraitActor = 1u << 1,
			kTraitSwitch = 1u << 2,
			kTraitSkin = 1u << 3,
			kTraitAnimatedShading = 1u << 4,  // a controller on the shader or alpha property
			kTraitMoves = 1u << 5,            // a controller or a non-fixed rigid body on its chain
			kTraitRootMoves = 1u << 6,        // its reference root's subtree moves (RootMoves)
		};
		std::uint32_t PerFrameTraits(const Tracked& a_tracked, const RE::BSGeometry& a_geometry) const;
		/** @brief T6b3e: PerFrameTraits from the entry's face shape, actor ownership and category node as its evaluation has them. */
		std::uint32_t PerFrameTraitsOf(bool a_faceShape, bool a_actorOwned, const RE::NiNode* a_categoryNode, const RE::BSGeometry& a_geometry) const;
		/**
		 * @brief Whether an entry with this verdict is evaluated every frame (Tracked::perFrame), and its light path's
		 * traits (Tracked::lightTraits); a_traits gets its traits. a_freshMotion: walk parity's, which takes the reference
		 * roots' motion from scratch (RootMovesNow) instead of from rootMotion.
		 */
		std::pair<bool, std::uint32_t> PerFrameOf(const Tracked& a_tracked, const RE::BSGeometry& a_geometry, Ineligible a_reason, std::uint32_t& a_traits,
			ankerl::unordered_dense::map<const RE::NiAVObject*, bool>* a_freshMotion = nullptr);
		/** @brief Adds an entry to this walk's `order` once; a_full: written in full even if it only moved. */
		void Schedule(RE::BSGeometry* a_geometry, Tracked& a_tracked, bool a_full = true);
		/**
		 * @brief A hash of what a record reads from a geometry's shader and alpha properties and their material (flags,
		 * the alpha test and threshold, the material alpha, the material and its diffuse view): the inputs a property
		 * controller can animate.
		 */
		std::uint64_t ShadingInputsOf(const RE::BSGeometry& a_geometry) const;
		/**
		/**
		 * @brief Whether a kept record's skin stays as written: skinning on, its partitions drawn, its palette the
		 * record's size. Notes a partition mask that changed; the palette itself is the scene placement job's. T6b3e: the merge's half,
		 * the record's skin and rows (and the toggle) being the evaluation's (EvalResult::lightSkin): the slot's flags and palette rows.
		 */
		bool KeepSkin(RE::BSGeometry* a_geometry, Tracked& a_tracked, const EvalResult& a_result);
		/** @brief A kept record of a moving static: its transforms, its bound and its sun entry, nothing else. */
		/**
		 * @brief Whether an actor's record, written in an earlier frame, is what WriteObject would write again but for
		 * its placement and palette: its classification inputs (ClassifyInputsOf) and its verdict are unchanged.
		 */
		bool ActorRecordKept(const RE::BSGeometry& a_geometry, const Tracked& a_tracked, EvalCounters& a_counters, EvalResult& a_out) const;
		/** @brief A face shape's snapshot and region this walk (FaceSnapshots); false without one. */
		bool ResolveFace(RE::BSGeometry& a_geometry, const Tracked& a_tracked, FaceSnapshots::ShapeView& a_face, std::uint32_t& a_region);
		/** @brief Appends a face shape's entry to this walk's faceStreams and points its slot at it. */
		void PushFaceStream(RE::BSGeometry& a_geometry, const Tracked& a_tracked, std::uint32_t a_slot, const FaceSnapshots::ShapeView& a_face, std::uint32_t a_region);
		/** @brief A kept face shape's record: its stream for this walk (ResolveFace, PushFaceStream); false to write it in full. */
		/**
		 * @brief The heads' publications (FaceSnapshots::BeginWalk): a kept face stream takes its head's new snapshot in place;
		 * a shape waiting for one, or one its head's record no longer holds, is written again; every shape of a stale head is
		 * written again (which rebuilds its record).
		 */
		void ApplyFacePublications();
		/** @brief A tracked face shape joins or leaves its head's list (faceHeads); the head's last one releases its record. */
		void ListFaceShape(RE::BSGeometry* a_geometry, Tracked& a_tracked);
		void UnlistFaceShape(RE::BSGeometry* a_geometry, Tracked& a_tracked);
		/** @brief Every face shape forgotten (the tracked entries were cleared). */
		void ClearFaceShapes();
		/**
		 * @brief The placements and palettes are FrameValues' (PlacementPlan): the walk lists what it read them from, and the
		 * frame's producer samples them on the pool. A moving reference root whose bound its dependents' sun entries are: an entry
		 * whose only motion is its root's bound (traits kTraitRootMoves alone) is not per frame: the root is listed instead (the
		 * plan's roots), when its reference or its category node had a move event this frame or the last (movingRoots, QueueRoots).
		 */
		struct MovingRoot
		{
			const void* key = nullptr;  // its reference
			const RE::NiNode* categoryNode = nullptr;
		};
		// Every root RootMoves found moving, until ScheduleRoot or its last dependent forgets it.
		ankerl::unordered_dense::map<const RE::NiAVObject*, MovingRoot> movingRoots;
		std::vector<const RE::NiAVObject*> rootPlacements;  // QueueRoots': the roots the plan lists
		void QueueRoots();
		struct PlacementStats
		{
			std::uint64_t roots = 0, stillRoots = 0, rootsGated = 0;
			std::uint64_t fadeRootsUnmirrored = 0;  // T6b1a: fade roots listed with no mirror record (FadeRootFromMirror)
		} placementStats;
		/** @brief The walk's plan item for an entry with a record (its slot and layer row, its resolved sun entry, its palette block). */
		PlacementPlan::Item PlanItemOf(RE::BSGeometry* a_geometry, Tracked& a_tracked);
		/** @brief The walk's plan (PlacementPlan), opened by its first round and made ready by PublishPlacementPlan. */
		PlacementPlan& WalkPlan();
		/** @brief After the walk: the roots it takes into the plan (QueueRoots), and the plan ready for the next frame. */
		void PublishPlacementPlan();
		/**
		 * @brief The movers by event (MoveEvents): the frame each key last had one, drained at the delta walk. A moving root's
		 * bound is listed when its key or its category node had one this frame or the last (QueueRoots), and every one on the
		 * frame after a full evaluation (it drops the node events).
		 */
		ankerl::unordered_dense::map<const void*, std::uint32_t> movedFrame;
		// The keys with a move event this frame and the last (movedKeys[0] this frame's): QueueRoots takes the bound of a root
		// whose reference had one, whatever RootMoves says (the cells' update passes recompute a still root's bound).
		std::array<std::vector<const void*>, 2> movedKeys;
		// T6b3d: movedKeys turn over once a frame (the frame they were drained in, not a pass: a burst's passes add to this frame's), and
		// movedFrame is pruned once 256 frames went by (a frame a pass may not see).
		std::uint32_t movedKeysFrame = 0, movedPruneFrame = 0;
		bool moveGating = false;
		std::uint32_t moveUngatedThrough = 0;
		void DrainMoveEvents(bool a_full);
		// The actor entries by the nodes on their chain (ListHiddenChain), for the hidden events.
		ankerl::unordered_dense::map<const void*, std::vector<RE::BSGeometry*>> hiddenDependents;
		bool hiddenGating = false;
		bool hiddenWitness = false;
		void ListHiddenChain(RE::BSGeometry* a_geometry, Tracked& a_tracked);
		void UnlistHiddenChain(RE::BSGeometry* a_geometry, Tracked& a_tracked);
		void DrainHiddenEvents();
		/**
		 * @brief T6b1b: the applied batches' hidden stores (the mirror updates carrying a site: their key, site and the bit's value), taken
		 * by DrainHiddenEvents.
		 */
		struct BatchHidden
		{
			const void* key = nullptr;
			std::uint32_t site = 0;
			bool hidden = false;
		};
		std::vector<BatchHidden> batchHidden;
		bool MovedRecently(const void* a_key) const
		{
			const auto it = movedFrame.find(a_key);
			return it != movedFrame.end() && sceneFrame - it->second <= 1;
		}
		const void* MoveKeyOf(const RE::BSGeometry& a_geometry, const RE::NiNode* a_categoryNode) const;
		/**
		 * @brief Whether a reference root's subtree holds anything that moves (a controller, a non-fixed rigid body, a
		 * skin): the root's bound is then not fixed, and it is the sun entry of every geometry under it. Walked once per
		 * root, and again after an event under it (ScheduleRoot).
		 */
		bool RootMoves(const RE::NiAVObject* a_root, const bool* a_now = nullptr);
		bool RootMovesNow(const RE::NiAVObject* a_root) const;
		ankerl::unordered_dense::map<const RE::NiAVObject*, bool> rootMotion;
		/** @brief Lists an evaluated entry under its properties; UnlistDependents takes it off every list. */
		void ListDependents(RE::BSGeometry* a_geometry, Tracked& a_tracked);
		void UnlistDependents(RE::BSGeometry* a_geometry, Tracked& a_tracked, bool a_root);
		/** @brief Evaluates the entry this walk with its classification taken again. */
		void Reclassify(RE::BSGeometry* a_geometry, Tracked& a_tracked);
		/**
		 * @brief Whether a placement event (a node moved, a sun entry node's bound changed) can change the entry's tables: it
		 * has a record, or a verdict of the frame's (None, Hidden, Switch, Actor). A static negative verdict cannot change
		 * with where the object is, and without a record nothing of it is placed.
		 */
		static bool PlacementMatters(const Tracked& a_tracked);
		/** @brief The dependents of a sun entry node, classified again, and its motion forgotten; a key, never dereferenced. */
		void ScheduleRoot(const RE::NiAVObject* a_root);
		/** @brief A node Havok moved or gave a controller: the entries under it and the dependents of every node above it. */
		void ApplyNodeEvent(RE::NiAVObject* a_node);
		/**
		 * @brief Before the walk's first round: the drained switch events whose selection changed (the index differs from
		 * the oldest event's value, or a child was attached, detached or replaced), for switches in the scene. Each is
		 * brought up to date (CatchUpSwitch) and, in a delta walk, the entries under it are classified again.
		 */
		void ApplySwitchEvents(bool a_full);
		// Scene members' records.
		struct ResidentPatch
		{
			AccumulatedPass pass;
			std::uint32_t pipeline = 0, material = 0;
		};
		static constexpr std::uint32_t kNotResident = ~0u;
		bool IsResidentSlot(std::uint32_t a_slot) const { return a_slot < residentPos.size() && residentPos[a_slot] != kNotResident; }
		void MarkResidentSlot(std::uint32_t a_slot, const ResidentPatch& a_patch);
		/** @brief Ends a slot's membership: a_restore resets its accumulated half now. */
		void DropResidentSlot(std::uint32_t a_slot, bool a_restore);
		/** @brief A slot's accumulated half back as the scene phase wrote it (its patch lapsed, or its residency ended). */
		void ResetAccumulatedHalf(std::uint32_t a_slot);
		/** @brief The accumulate phase: the residents' pipeline and material slots used this frame, and a template when none was. */
		void KeepResidentsAlive();
		/** @brief A member's tree slot (Tables::objectTree), taken for a tree member and given back otherwise or when it leaves. */
		void ListTree(std::uint32_t a_slot);
		void UnlistTree(std::uint32_t a_slot);
		/** @brief The member's fade root (Tables::fadeRoots): listed when it joins, released with its last member. */
		void ListFadeRoot(std::uint32_t a_slot);
		void UnlistFadeRoot(std::uint32_t a_slot);
		/**
		 * @brief T6b3e: the reference a newly listed tree or fade root's owner is copied from: the slot's entry's fade node (Tracked::
		 * fadeNodeRef, held from pins) when it is a_node; else null, and the root is not listed (counted: fadeRootsUnowned).
		 */
		RE::NiPointer<RE::NiAVObject> HeldFadeNode(std::uint32_t a_slot, const void* a_node);
		/**
		 * @brief T6b3e: a_entry's fade node reference for a_node (Tracked::fadeNodeRef): the batch's pin, else a root owner's copy (the node is
		 * a tracked reference's root), else asked of the render thread (FadeNodeRequest, once a key) and named in the report (a_path: where
		 * it was wanted). True when held now.
		 */
		bool AcquireFadeNode(Tracked& a_entry, const void* a_node, const char* a_path);
		/** @brief A switch event on a tree root's LOD switch: the root's kFadeRootTreeLod again (ApplySwitchEvents). */
		void RefreshFadeRootSwitch(const RE::NiAVObject* a_switch);
		/** @brief A fadeAmount event (Actor::SetAlpha): a listed root's fadeAmount again from its node, its state row kept. */
		void RefreshFadeAmount(const void* a_node);
		void CheckResidentParity();
		/** @brief What a classification reads from the geometry, its properties and its material, hashed. */
		std::uint64_t ClassifyInputsOf(const RE::BSGeometry& a_geometry) const;
		/** @brief CS_DCLF_INPUT_WATCH: what ClassifyInputsOf and ShadingInputsOf hash, by component (kInputComponentNames). */
		std::array<std::uint64_t, 12> InputComponentsOf(const RE::BSGeometry& a_geometry) const;
		/** @brief CS_DCLF_INPUT_WATCH: a re-read of kind a_kind (0 classify, 1 shading) that found the hash changed or not. */
		void NoteInputReread(const Tracked& a_tracked, const RE::BSGeometry& a_geometry, std::uint32_t a_kind, bool a_changed, EvalCounters& a_counters,
			TrackedDelta& a_delta) const;
		void StoreInputComponents(Tracked& a_tracked, const RE::BSGeometry& a_geometry);
		void MoveBucket(Tracked& a_tracked, Ineligible a_bucket);
		void ListFadeDependent(RE::BSGeometry* a_geometry, Tracked& a_tracked);
		void UnlistFadeDependent(RE::BSGeometry* a_geometry, Tracked& a_tracked);
		/**
		 * @brief Scene membership: binds the bind queue's records from passes built from the objects (PrimaryCull::MembershipPass),
		 * as residents, which the engine's registrations and the lapse leave alone until the record is written again.
		 */
		void BindByMembership();
		/**
		 * @brief T6b3e: one bind queue element's evaluation (BindByMembership's JoinPre, pure over the tables as the joins found them, the
		 * mirror and the entry): skipped, its binding standing, or its membership pass (the derivation cache entry to store with it, why
		 * none, its fade distance and height test). The merge, in queue order, stores the cache entry, leaves the set, drops a failed
		 * member and queues the join.
		 */
		struct JoinPre
		{
			enum class Kind : std::uint8_t
			{
				Skip,    // not a record to bind (free, shadow only, another entry's slot, not this walk's record, not a verdict of None)
				Kept,    // a member whose binding stands (MemberBindingStands)
				Failed,  // no membership pass (PrimaryCull::LastSyntheticFail's reason in fail)
				Pass,    // a membership pass: joined by BuildAccumulatePhase
			};
			Kind kind = Kind::Skip;
			RE::BSGeometry* geometry = nullptr;
			bool layer = false;
			SceneCapture::LeafView leaf;
			MembershipDerived derived;  // the cache entry the pass leaves (Tracked::membershipDerived or membershipLayerDerived)
			AccumulatedPass pass;
			std::uint8_t fail = 0;
		};
		/** @brief T6b3e: bind queue element a_slot evaluated (pure). */
		void EvaluateJoinPre(std::uint32_t a_slot, JoinPre& a_out) const;
		/** @brief T6b3e, CS_DCLF_FANOUT_PARITY: the first field two JoinPre evaluations differ in, or null. */
		static const char* JoinPreDifference(const JoinPre& a_left, const JoinPre& a_right);
		/**
		 * @brief T6b3e: one membership join's evaluation (BuildAccumulatePhase's, pure: the mirror, the entry, the tables as the joins
		 * found them, the room map): the held property's check, the derivation cache's witnesses (not its slots: the merge's), the
		 * classification and its descriptors, the pipeline key and static flags of a classified join, the room index. a_classify: classify
		 * even where the cache's witnesses hold (the merge's, when the cache's slots are gone).
		 */
		struct JoinResult
		{
			enum class Kind : std::uint8_t
			{
				Skip,      // the mirror's property is not the held one (a swap not applied yet)
				Rejected,  // a verdict of this frame (the classification or ClassifyFrame) leaves it without bindings
				Joined,    // bound: the merge patches it
			};
			Kind kind = Kind::Skip;
			SceneCapture::LeafView leaf;
			RE::BSShaderProperty* property = nullptr;        // the entry's held property: the template's, a material request's
			const RE::BSShaderMaterial* material = nullptr;  // the record's material (the witness, the material slot's key)
			std::uint8_t fadeState = 0;
			bool alphaBelowOne = false;
			std::uint32_t geometrySlot = 0;
			bool derivedWitness = false;  // the derivation cache's witnesses hold
			bool classified = false;      // ClassifyStatic or ClassifyLayer ran: descriptors, key and static flags are this join's
			Ineligible reason = Ineligible::None;
			LightingDescriptors descriptors;
			PipelineKey key{};
			std::uint32_t staticFlags = 0;
			bool roomSet = false;  // Light Limit Fix's room index taken again (the room map moved on since the entry's)
			int roomIndex = -1;
		};
		void EvaluateJoin(const OrderEntry& a_entry, bool a_classify, JoinResult& a_out) const;
		/** @brief T6b3e, CS_DCLF_FANOUT_PARITY: the first field two join evaluations differ in, or null. */
		static const char* JoinDifference(const JoinResult& a_left, const JoinResult& a_right);
		/**
		 * @brief T6b3e: a_count evaluations a_evaluate(index, result) into a_results, by chunks of kFanoutGrain on the pool (a_pool:
		 * FanoutMode's 2) or on this thread; CS_DCLF_FANOUT_PARITY compares a second, serial evaluation (a_difference), a_what naming the
		 * round in its report. Defined in AccumulatePhase.cpp, its only user.
		 */
		template <class Result, class Evaluate, class Difference>
		void EvaluateJoins(const char* a_what, std::size_t a_count, bool a_pool, std::vector<Result>& a_results, Evaluate&& a_evaluate, Difference&& a_difference);
		std::vector<JoinPre> joinPres;
		std::vector<JoinResult> joinResults;
		/**
		 * @brief CS_DCLF_CHANGE_LOG_PARITY=1: every 60 frames each slot's columns are kept, and a frame later every slot
		 * whose columns changed in between must be in the log for that frame with the causes that changed. At the scene
		 * phase's start, before anything of the frame writes.
		 */
		void CheckChangeLog();
		// The material parity (render thread, CS_DCLF_PERSISTENT_PARITY: ValidateMaterialSlice): the installed records against the engine's
		// evaluation, by part, through the suspects. The upkeep's own counters are the scene work's (materialRecordStats).
		struct MaterialFrameStats
		{
			std::uint64_t checks = 0, slotsChecked = 0, written = 0, componentsDiffer = 0, transformsDiffer = 0;
			std::array<std::uint64_t, 3> suspected{}, late{};  // by MaterialPart
			std::array<std::string, 3> lateFirst{};            // the first late difference of each part (what moved)
			// Own values the window did not hold although the producers rewrote the record within it (its materialVersion moved): a
			// covered writer animating the material faster than the publication follows it within kMaterialLateFrames, not a missed one.
			std::uint64_t lagging = 0;
			std::string laggingFirst;
			std::string first;
		} materialFrameStats;
		/** @brief Render thread, Prepass, CS_DCLF_PERSISTENT_PARITY: the used sets against the slots the installed records name. */
		void CheckMaterialFrame();
		// The pipelines' PerGeometry blocks (RefreshFrameConstants): full evaluations, frame samples, and the parity's findings.
		struct GeometryStats
		{
			std::uint64_t frames = 0, full = 0, samples = 0, changed = 0, checks = 0, pipelinesChecked = 0, techniquesChecked = 0, techniquesDiffer = 0,
						  lightingVersions = 0, lightingChecked = 0, lightingDiffer = 0;
			std::string lightingFirst;
			// T6b2c: a technique row against the live sample that differs because the sample moved since the row's write (the
			// publication's lag: counted apart, not as differ), and the first real difference.
			std::uint64_t techniquesLate = 0;
			std::string techniqueFirst;
			// T6: the synthetic template's evaluation against the registered one's, where the engine registered one.
			std::uint64_t templateChecked = 0, templateDiffer = 0, templateLightingDiffer = 0, templateMissing = 0;
			std::string templateFirst;
			std::array<std::array<std::uint64_t, 64>, 2> differ{};
			std::string first;
		} geometryStats;
		/** @brief The coordinator: the technique row of a pass descriptor's TechniqueKey (Tables::techniqueKeys), made and evaluated when new. */
		std::uint32_t TechniqueRowFor(std::uint32_t a_passDescriptor);
		void CheckFrameGeometry(std::uint32_t a_pipeline, const GeometryConstants& a_reference, const GeometryConstants& a_held);
		// The first RefreshFrameConstants evaluates every pipeline in full and resamples every slot's shading. The full
		// evaluations are what seed the frame lighting (FrameCapture::lighting) with the variables the per-frame sample's
		// pipeline does not write, such as AmbientSpecularTintAndFresnelPower: pipelines made later are evaluated where
		// they are written (WriteObject), which does not publish lighting.
		bool constantsRefreshed = false;
		bool shadingNamedAll = false;  // NameShadingEvents named every slot once
		// CS_DCLF_PERSISTENT_PARITY: every 60 frames every drawn slot's shading as the engine has it now against the frame's rows
		// (FrameValues), at Prepass: a difference is named for the next frame (an event the walk took), its event is queued (taken
		// then, and named), or missed.
		struct ShadingParity
		{
			std::uint64_t checks = 0, slots = 0, missing = 0, named = 0, late = 0, wetness = 0, wetnessDiffer = 0, resampled = 0, lodFadeEvents = 0,
				emittanceEvents = 0, frames = 0;
			std::string first;
		} shadingParity;
		/** @brief Render thread, persistent-parity frames at Prepass (the scene task joined): ShadingParity's check. */
		void CheckShadingParity();
		std::vector<const void*> lodFadeChanged;
		// Object LOD (dclf-lod.md): each tracked BSSubIndexTriShape's drawn index ranges (LodSegments::DrawnRanges), taken when it is
		// tracked and again at each of its segment events (lodSegmentEvents); CS_DCLF_PERSISTENT_PARITY compares them with the
		// shape's live state on parity frames.
		ankerl::unordered_dense::map<const RE::BSGeometry*, std::vector<LodSegments::Range>> lodRanges;
		struct LodSegmentStats
		{
			std::uint64_t events = 0, changed = 0, checks = 0, shapes = 0, differ = 0;
			std::string first;
		} lodSegmentStats;
		std::uint32_t lodParityFrame = 0;  // T6b3d: the pass (PassStamp) whose batch ran the parity: once a pass
		// Tree LOD (dclf-lod.md, L4): the engine's tree LOD groups and their instance records, by events (TreeLod.h).
		TreeLod::Mirror treeLod;
		void SampleLodRanges(const RE::BSGeometry& a_shape, bool a_event);
		void ApplyLodSegmentEvents(const std::vector<const void*>& a_shapes);
		struct ChangeLogParity
		{
			std::vector<Tables::Columns> snapshot;
			LogCursor cursor;  // the log where the snapshot was taken; active while one is waiting to be checked
			std::uint64_t checks = 0, slots = 0, changed = 0, missing = 0, skipped = 0;
			std::string first;
		} changeParity;
		std::array<std::uint64_t, kChangeCauseCount> reportedChangeCounts{};
		/** @brief After the delta walk's evaluations: the geometry slots of the slots it kept, and the shadow sets. */
		void FinishDeltaWalk(PartTimer& a_timer);
		/**
		 * @brief One round of the walk (T6b3e: the fan-out): every entry of `order` from a_first evaluated (EvaluateEntry: pure, into
		 * evalResults), then merged in index order (MergeEntry: the slots, the columns, the change log, the plan, the dependents). By
		 * CS_DCLF_FANOUT: 0 each entry evaluated and merged before the next (the walk as it was: the reference), 1 every entry
		 * evaluated then merged, on the scene lane, 2 (default) the evaluations on the preparation pool in chunks of kFanoutGrain from
		 * kFanoutMinEntries entries (fewer: as 1), each chunk merged as soon as it is ready while later ones are evaluated (the ordered
		 * streaming merge; under CS_DCLF_FANOUT_PARITY's passes every chunk first). Under the mirror-read parity the round runs as 0 (its
		 * live checks note on the lane).
		 */
		void EvaluateRound(PartTimer& a_timer, std::size_t a_first);

		// T6b3e: the fan-out's grain (one chunk's entries, one EvalCounters each) and the least entries a round fans out for.
		static constexpr std::size_t kFanoutGrain = 64;
		static constexpr std::size_t kFanoutMinEntries = 128;
		/**
		 * @brief T6b3e: an evaluation chunk's counters (a chunk: kFanoutGrain entries from its first; its index begin / kFanoutGrain, so
		 * they do not depend on the thread that ran it). Merged into the store's in chunk order before the round's merges
		 * (MergeCounters): a first string is taken only where the store has none yet, so the first named is the first in `order`, as the
		 * serial walk named it; the rate-limited warnings are logged there.
		 */
		struct EvalCounters
		{
			std::array<std::uint32_t, static_cast<std::size_t>(Ineligible::Count)> ineligible{};
			std::array<std::uint32_t, 64> techniqueRejects{};
			std::map<const RE::NiRTTI*, std::uint32_t> propertyRejects;
			std::uint32_t rejectedBlended = 0, rejectedOpaque = 0, rejectedOpaqueAlphaTest = 0;
			std::uint32_t classifyHits = 0, classifyChecked = 0, classifyDiffers = 0;
			std::string classifyDiffersFirst;  // the classify cache's warning for its first difference here
			std::uint64_t verdictsChecked = 0, verdictsSkipped = 0, verdictsMissed = 0;
			std::string firstVerdictMissed;
			std::array<std::uint64_t, 12> inputChanged{};
			std::array<std::uint64_t, 2> inputRereads{}, inputRereadsChanged{};
			std::string firstInputChange;
			std::uint64_t leafMissing = 0;  // leafStats.missing
			std::uint64_t reread = 0;       // delta.reread
			std::array<double, static_cast<std::size_t>(BuildPart::Count)> partMs{};  // the chunk's PartTimer (CS_DCLF_PROFILE)
			std::array<double, static_cast<std::size_t>(EvaluateKind::Count)> evaluateKindMs{};
			MirrorReadStats mirror;  // the mirror reads counted here (mirrorReadSink)
			ankerl::unordered_dense::map<const RE::NiAVObject*, bool> rootMotion;  // RootMovesNow's answers in this chunk
		};
		/** @brief T6b3e: what an evaluation decided for its entry's Tracked, applied by its merge (MergeWrite) as WriteObject wrote it. */
		struct TrackedDelta
		{
			bool faceShapeSet = false, faceShape = false;      // the face shape resolved (once)
			bool actorOwnedSet = false, actorOwned = false;    // owned by an actor, resolved (once)
			bool verdictSet = false;                           // the static verdict's cache
			Tracked::StaticVerdict verdict;
			bool candidateSet = false;                         // classified (or its frame verdict changed)
			std::uint32_t candidateFrame = 0;
			Ineligible candidateReason = Ineligible::None;
			bool classifyInputsSet = false;
			std::uint64_t classifyInputs = 0;
			bool inputComponentsSet = false;                   // CS_DCLF_INPUT_WATCH's components, the last stored
			std::array<std::uint64_t, 12> inputComponents{};
		};
		/**
		 * @brief T6b3e: one entry's evaluation (EvaluateEntry), for its merge. Everything in it was read from the frozen mirror, the entry's
		 * own Tracked, lodRanges, categoryNodes, the walk's stamps and the pass's snapshot (ShardScope): no table, slot, face, root motion
		 * or other entry. Compared field by field under CS_DCLF_FANOUT_PARITY (FanoutDifference).
		 */
		struct EvalResult
		{
			enum class Write : std::uint8_t
			{
				None,      // the light path kept it (no write)
				NoLeaf,    // the mirror holds no leaf yet: leafWaiting
				Rejected,  // its verdict gives it no record
				Record,    // a record, its slot-free inputs below; the merge resolves the face, the geometry and the slots
			};
			EvaluateKind kind = EvaluateKind::Event;
			// The light path (the first round): kept, recorded, and KeepSkin's table half left to the merge (the record's rows and
			// partitions); the nodes ActorRecordKept arms for CS_DCLF_HIDDEN_WATCH.
			bool light = false;
			bool lightRecorded = false;
			bool lightSkin = false;
			std::uint32_t skinRows = 0;
			std::uint16_t skinPartitions = 0;
			std::uint32_t skinLodPartitions = 0;
			bool armHiddenWatch = false;
			std::array<const RE::NiAVObject*, 4> hiddenWatch{};
			// The write (WriteObject's evaluation).
			Write write = Write::None;
			TrackedDelta delta;
			SceneCapture::LeafView leaf;
			Ineligible reason = Ineligible::None;  // its bucket
			bool shadowOnly = false;
			bool faceShape = false;        // a face shape with FaceSnapshots on: its snapshot is the merge's (ResolveFace)
			bool referenceReason = false;  // the dense walk's: reason into referenceReasons
			// The record's inputs with no slot in them.
			std::uint16_t partitionMask = 0;
			bool lodChain = false;
			bool skinCandidate = false;  // skinned with the toggle on: in this walk's skinnedObjects
			std::uint32_t boneRows = 0;  // its palette's rows, 0 without one (or past 240)
			std::uint8_t shadowReject = 0;
			std::uint32_t shadowTechnique = 0;
			const RE::BSShaderMaterial* shadowMaterial = nullptr;  // the alpha-tested techniques' diffuse (the property record's)
			ID3D11ShaderResourceView* shadowDiffuse = nullptr;
			std::array<std::uint32_t, kOcclusionViews> occlusion{};
			std::uint32_t skinLodWord = 0;  // SkinLodPartitionsOf (0 for a LOD chain)
			// An entry classified in this walk: its per-frame traits without its root's motion (PerFrameFrom's input), whether its verdict
			// may give it a record, its sun entry as the merge's PerFrameOf will find it and that root's motion now, its move key.
			bool classified = false;
			std::uint32_t traits = 0;
			bool mayRecord = false;
			const RE::NiAVObject* rootCandidate = nullptr;
			bool rootMovesNow = false;
			const void* moveKey = nullptr;
			// ShadingInputsOf, taken when the merge may store it (the traits an animated shading or actor entry has).
			bool shadingInputsSet = false;
			std::uint64_t shadingInputs = 0;
		};
		/** @brief T6b3e, CS_DCLF_FANOUT_PARITY: the first field two evaluations of one entry differ in, or null when they agree. */
		static const char* FanoutDifference(const EvalResult& a_left, const EvalResult& a_right);
		/** @brief T6b3e: entry a_index of `order` evaluated (a_light: the first round, which tries the light path). Pure: reads only. */
		void EvaluateEntry(std::size_t a_index, bool a_light, EvalCounters& a_counters, EvalResult& a_out, PartTimer& a_timer) const;
		/** @brief T6b3e: WriteObject's evaluation half (the classification and the record's slot-free inputs), into a_out. */
		void EvaluateWrite(RE::BSGeometry* a_geometry, const Tracked& a_tracked, EvalCounters& a_counters, EvalResult& a_out, PartTimer& a_timer) const;
		/**
		 * @brief T6b3e: an entry classified in this walk (its candidateFrame as its merge leaves it is the pass's), its bookkeeping's
		 * evaluation: the per-frame traits without the root's motion, whether its verdict may give it a record, its sun entry and that
		 * root's motion now (the chunk's memo), its move key; and its shading inputs where the merge may store them.
		 */
		void EvaluateClassified(RE::BSGeometry* a_geometry, const Tracked& a_tracked, EvalCounters& a_counters, EvalResult& a_out) const;
		/** @brief T6b3e: PerFrameOf at the merge, from the evaluation's traits and root motion (rootMotion holds the root first: RootMoves). */
		std::pair<bool, std::uint32_t> PerFrameMerge(const Tracked& a_tracked, const EvalResult& a_result);
		/** @brief T6b3e: PerFrameOf's verdict from the traits (its root's motion in them), the verdict and whether it may give a record. */
		static std::pair<bool, std::uint32_t> PerFrameFrom(std::uint32_t a_traits, Ineligible a_reason, bool a_mayRecord);
		/** @brief T6b3e: entry a_index's merge, after the evaluations of its round (a_profileKinds: its EvaluateKind timed). */
		void MergeEntry(std::size_t a_index, bool a_profileKinds, EvalResult& a_result, PartTimer& a_timer);
		/** @brief T6b3e: WriteObject's merge half: a_result's Tracked delta, then the face, the geometry slots and the record. */
		bool MergeWrite(RE::BSGeometry* a_geometry, Tracked& a_tracked, EvalResult& a_result, PartTimer& a_timer, Ineligible& a_bucket);
		/** @brief T6b3e: a chunk's counters into the store's (in chunk order), its warnings logged. */
		void MergeCounters(EvalCounters& a_counters);
		/** @brief T6b3e: the mode a round of a_count entries runs in (CS_DCLF_FANOUT, the parities, the pass's thread); a_joins: the joins'. */
		std::uint32_t FanoutMode(std::size_t a_count, bool a_joins) const;
		/** @brief T6b3e: what a pass's evaluations read in place of the live toggles and Terrain Blending (BuildScenePhase takes it). */
		struct PassSnapshot
		{
			ToggleSet toggles;
			bool terrainDefers = false;
		};
		PassSnapshot passSnapshot;
		std::shared_ptr<const FrameGlobals> passGlobals;  // the frame globals the pass binds (ScenePass), bound on the pool too
		struct ShardScope;
		std::vector<EvalResult> evalResults;
		std::vector<EvalCounters> evalCounters;
		/** @brief T6b3e: the fan-out since the report (ReportCoordinator). */
		struct FanoutStats
		{
			std::uint64_t rounds = 0, parallelRounds = 0, entries = 0, parallelEntries = 0, chunks = 0;
			std::uint64_t joinRounds = 0, joinParallelRounds = 0, joinEntries = 0;
			std::uint64_t parityRounds = 0, parityEntries = 0, parityDiffers = 0;
			std::uint64_t lightMissed = 0;  // light-path entries KeepSkin's table half wrote in full at the merge
			std::string parityFirst;
		} fanoutStats;
		/** @brief Its report line (empty when no round ran), the counters reset. */
		std::string FanoutReport();
		/** @brief A slot's inputs to the frame's shadow sets (the casters' textures and pipelines). */
		struct ShadowInputs
		{
			ID3D11ShaderResourceView* diffuse = nullptr;
			std::uint64_t vertexDesc = 0;
			std::uint32_t technique = 0;
			std::uint32_t flags = 0;
			std::uint32_t reject = 0;
			std::array<std::uint32_t, kOcclusionViews> occlusionTechnique{};
			bool operator==(const ShadowInputs&) const = default;
		};
		ShadowInputs ShadowInputsOf(std::uint32_t a_slot) const;
		void RefreshShadowSets(bool a_forceRebuild);
		// Whether a slot's shadow inputs changed this walk: only the changed contributions are updated.
		bool shadowSetsDirty = true;
		bool shadowIndexNeedsRebuild = true;
		std::vector<std::uint32_t> shadowDirtySlots;
		std::vector<ShadowInputs> shadowIndexedInputs;
		std::vector<std::uint8_t> shadowIndexedLive;
		ankerl::unordered_dense::map<ID3D11ShaderResourceView*, std::set<std::uint32_t>> shadowTextureMembers;
		ankerl::unordered_dense::map<ShadowPipelineKey, std::set<std::uint32_t>, ShadowPipelineKeyHash> shadowKeyMembers;
		std::array<ankerl::unordered_dense::map<ShadowPipelineKey, std::set<std::uint32_t>, ShadowPipelineKeyHash>, kOcclusionViews> occlusionKeyMembers;
		std::uint32_t keptShadowCasters = 0;
		std::array<std::uint32_t, 16> keptShadowRejects{};
		bool fullEvaluation = true;  // the next delta walk evaluates every entry (a reset, a load, a live toggle)
		std::uint32_t walkSerial = 0;
		/**
		 * @brief The entries evaluated every frame, with their entry's address. `tracked` keeps its values in one array,
		 * so an insert or an erase may move them: trackedLayout counts those, and the schedule trusts the addresses only
		 * while it equals perFrameLayout, the count they were taken at, and looks every entry up again otherwise.
		 */
		struct PerFrameItem
		{
			RE::BSGeometry* geometry = nullptr;
			Tracked* tracked = nullptr;
		};
		std::vector<PerFrameItem> perFrameSet;
		std::uint64_t trackedLayout = 0;
		std::uint64_t perFrameLayout = ~0ull;
		std::uint32_t validatedFrame = ~0u;  // T6b3d: the pass (PassStamp) that ran the validation slice: once a pass
		std::vector<RE::BSGeometry*> pendingEvaluation;
		// CS_DCLF_DERIVE_PROBE's LOD fade parity: the frame's inputs, sampled once a frame when first needed.
		LodFadeFrame lodFadeSample;
		bool lodFadeSampled = false;
		// Scene membership: the eligible records the scene phase wrote this frame, bound by the accumulate phase from a pass
		// built from the object (PrimaryCull::MembershipPass) and kept as residents until written again or released.
		std::vector<std::uint32_t> bindQueue;
		std::vector<std::uint8_t> materialMember;  // per material slot: a membership resident was bound to it (diagnostic)
		std::uint32_t membershipWitness = ~0u;  // PrimaryCull::MembershipWitness when the residents were bound
		std::vector<std::uint32_t> refreshedGeometry;  // geometry slots ResolveGeometrySlot re-resolved in place this walk
		// Slot liveness (Tables' SlotTables): the references, counted from the logs. Per object slot, the geometry, pipeline
		// and material it was counted as referencing (slot and generation); per geometry slot, the partition after it in a
		// skin's chain (a chain's slots live while its first does). What makes a geometry slot stale is an event: its buffer
		// references failing the staggered touch, buffers starting to resolve with the slot unresolved, or
		// ResolveGeometrySlot freeing it (freedGeometry); the objects drawing a stale slot are written again (FinishDeltaWalk).
		struct SlotReferences
		{
			static constexpr std::uint32_t kNone = ~0u;
			struct Reference
			{
				std::uint32_t slot = kNone, generation = 0;
				bool operator==(const Reference&) const = default;
			};
			struct Counted
			{
				Reference geometry, pipeline, material;
			};
			LogCursor objects, links;
			std::vector<Counted> object;  // per object slot
			std::vector<Reference> link;  // per geometry slot
		} slotReferences;
		/** @brief The slot tables' reference counts, from the change and geometry logs since the last call. */
		void UpdateSlotReferences();
		/** @brief Drops a geometry slot's contents: the reference it holds on the next partition, its map entry and its buffer leases. */
		void ClearGeometrySlot(std::uint32_t a_slot);
		/** @brief ClearGeometrySlot, then returns the slot to the free list. */
		void FreeGeometrySlot(std::uint32_t a_slot);
		/**
		 * @brief Drops a material slot's contents: its map entry, its dependents entry and its owner, and lists it retired
		 * (its record is dead to the kept stores). The slot table's own state is the caller's (freed, or drained).
		 */
		void ClearMaterialSlot(std::uint32_t a_slot);
		std::vector<std::uint32_t> freedGeometry;
		std::vector<std::uint32_t> staleGeometrySlots;
		bool geometryResolvedLastWalk = false;
		// Set when a slot of any kind was freed this frame; CheckObjectSlots runs only then (or under parity), since a
		// bound object can reference a slot that is not live only after one was.
		bool slotsFreedThisFrame = true;
		// The decals given an ordinal last frame, whose decalOrdinal entries are reset this frame.
		std::vector<std::uint32_t> decalOrdered;
		std::vector<const RE::BSFadeNode*> fadeChanged;  // FadeWatch's, applied at ApplyEvents
		// SetFadeRootsOwned's set (kFadeRootOwned), by node: whether each is stood in (kFadeRootStoodIn).
		ankerl::unordered_dense::map<const void*, bool> fadeRootOwned;
		/**
		 * @brief The root's row owned or not; a root newly owned is seeded again from its node (a new generation). One whose
		 * stood-in state changes is reported to the fade watch: its dependents' shadow verdicts read its fade from elsewhere.
		 */
		void MarkFadeRootOwned(const void* a_node, bool a_owned, bool a_standIn);
		ankerl::unordered_dense::map<const RE::BSFadeNode*, std::vector<RE::BSGeometry*>> fadeDependents;
		// The structural events (SceneEvents in SceneStore.cpp), applied at ApplyEvents: properties by key, nodes held.
		std::vector<const void*> propertyChanged;
		std::vector<RE::NiPointer<RE::NiAVObject>> nodeChanged;
		// T6b3d: the engine's queues are the coordinator's alone (one consumer): each pass drains them up to where each stood at its start
		// (CollectEvents) into its own batch, and applies it (ApplyEvents). A load screen's marker (EventBatch::loading, posted once a load
		// by NoteLoadingScreen, which raises loadingSeen first) makes the coordinator drop what the load invalidates (ApplyLoading); while
		// loadingSeen holds, a pass drains the queues into the mirror's carry (loadingCarry) and discards them for the tracking.
		EventQueue<std::shared_ptr<EventBatch>, 64> eventBatches{ &WakeSceneLoadMarker };
		std::atomic<bool> loadingSeen{ false };
		bool loadingPosted = false;  // render thread: the load's marker is posted
		/**
		 * @brief T6b3d, the coordinator, in its pass after the posts are taken: the engine's queues (the tracker's stack, the fade snaps and
		 * amounts, the fades, the property, node and switch events, object LOD's segments) drained, each up to where it stood at the call,
		 * into the pass's batch (null while a load screen is up: into the carry, the references the discarded events hold to Present),
		 * with the render thread's mirror probe; the switch catch-ups the batch asks for posted to the render thread (switchCatchUps).
		 */
		std::shared_ptr<EventBatch> CollectEvents();
		/**
		 * @brief The coordinator: the load markers posted since applied (ApplyLoading), then a_batch (none: an empty one) - the category
		 * refresh, the attached subtrees walked and the detached entries erased, the validation slice, the structural events. What it lets
		 * go of is handed back (HandBack).
		 */
		void ApplyEvents(std::shared_ptr<EventBatch> a_batch);
		/** @brief The coordinator (ApplyEvents' start): what a load screen invalidated dropped (NoteLoadingScreen's marker, T6b3a). */
		void ApplyLoading();
		/** @brief The coordinator: one batch applied (ApplyEvents' body). */
		void ApplyBatch(std::shared_ptr<EventBatch> a_batch);
		bool rescanCaptureAll = false;  // render thread: a load's marker posted, so the next category capture pins every category node
		/**
		 * @brief T6b3d: what CatchUpSwitches needs of a pass's batch (the attached roots, its switch events), the coordinator -> the render
		 * thread, served at the frame's start (engine writes). Each holds references (copies of the batch's, made while the batch holds
		 * them), dropped on the render thread.
		 */
		struct SwitchCatchUp;
		EventQueue<std::shared_ptr<SwitchCatchUp>, 64> switchCatchUps;
		/**
		 * @brief T6b3d: the scene pump. passesFull: SetScenePassMode's (render thread -> the passes). The rest is the
		 * coordinator's: the oldest stamp applied and not yet published, what the last publication was made from (PublishKey: a pass that
		 * changed none of it publishes nothing), and the report's samples (ReportCoordinator).
		 */
		std::atomic<bool> passesFull{ false };
		std::uint64_t unpublishedNs = 0;
		std::uint32_t unpublishedFrame = 0;
		struct PublishKey
		{
			std::array<std::uint64_t, 18> words{};
			std::array<std::uint64_t, 6> lookups{};
			const void* catalog = nullptr;
			bool operator==(const PublishKey&) const = default;
		};
		PublishKey publishedKey;
		bool publishedKeyValid = false;
		bool publishForced = false;  // an input the coordinator took for the frame (TakeCoordinatorInputs) publishes
		/** @brief What a publication is made from, now (the tables' logs, stamps and sizes, the lookups, the catalog, the candidates). */
		PublishKey CurrentPublishKey() const;
		/** @brief The deltas the next publication carries (the log's node): any pending. */
		bool DeltasPending() const;
		/** @brief The coordinator, at a pass's publication point: whether anything changed since the last publication (else none). */
		bool PublicationNeeded();
		/** @brief The coordinator, after PublishScene: the key it was made from. */
		void NotePublished();
		struct PumpStats
		{
			std::vector<double> passMs;                // a pass's whole time, each pass
			std::vector<std::uint32_t> passEvents;     // the tracker events and queue entries a pass drained
			std::vector<double> publishMs;             // the oldest unpublished event to its publication (ms)
			std::vector<std::uint32_t> publishFrames;  // the same in the render thread's frames
			std::uint64_t passes = 0, eventsOnly = 0, loading = 0, unchanged = 0, published = 0, inlinePasses = 0, failed = 0;
			// The passes by what woke them (SceneWake, a pass counting under each source it took; the last: none - an inline pass, a burst's
			// continuation) and those of them that published nothing; the walks that took the per-frame entries (once a frame) and those
			// that did not.
			std::array<std::uint64_t, static_cast<std::size_t>(SceneWake::Count) + 1> bySource{}, unchangedBySource{};
			std::uint64_t perFrameWalks = 0, eventWalks = 0;
			double fullMs = 0.0, fullMaxMs = 0.0;  // the whole passes' (not events alone): the scene pass CPU line's
			// The sceneFrames the passes saw since the report (the first, the last), the frames that had one, the most in one frame.
			std::uint32_t firstFrame = 0, lastFrame = 0, framesWithPass = 0, passesThisFrame = 0, maxPassesInFrame = 0;
			bool any = false;
		} pumpStats;
		std::uint32_t reportedInterval = 0;  // the coordinator's: the last kReportInterval its report ran for (sceneFrame / kReportInterval)
		/** @brief The coordinator, at a pass's end when its sceneFrame crossed kReportInterval: its own report lines (T6b3d), logged. */
		void ReportCoordinator();
		/** @brief The pump's drain (the lane, or RunScenePassInline): the thread marked, the frame's globals bound, one RunSceneWork. */
		void ScenePass(bool a_inline);
		// The engine references the scene work let go of go to EngineReleases (T6b3a), and its applied batches (their tracker events hold
		// subtrees) here: released at Present (ReleaseHandedBack), pushed where they are let go (RecycleRetired), never handed over.
		EventQueue<std::shared_ptr<EventBatch>, 64> batchesReleased;
		// The tree and fade-root nodes the tables list (Tables::treeNode, fadeRootNode), owned while listed: unlisted, they go
		// through the retirement chain (a kept publication still walks them).
		std::vector<RE::NiPointer<RE::NiAVObject>> treeOwners, fadeRootOwners;
		template <class T>
		void HandBack(RE::NiPointer<T>&& a_reference)
		{
			// Through the retirement chain: a published version of the tables may still name it (step 6e E3).
			if (a_reference)
				retirement.Open().references.emplace_back(std::move(a_reference));
		}
		template <class T>
		void HandBack(std::vector<RE::NiPointer<T>>& a_references)
		{
			for (auto& reference : a_references)
				HandBack(std::move(reference));
			a_references.clear();
		}
		// The switch events (SwitchEvents in SceneStore.cpp) drained so far, one per switch node: the index before its
		// oldest event, and whether a child changed. Applied, and cleared, by the next walk.
		struct SwitchPending
		{
			RE::NiPointer<RE::NiAVObject> node;
			std::int32_t before = -1;
			bool structural = false;
		};
		std::vector<SwitchPending> switchPending;
		ankerl::unordered_dense::map<const RE::NiAVObject*, std::uint32_t> switchPendingIndex;
		// The switches the walks applied, for PrimaryCull (TakeSwitchChanges); switchResync when it must read them all.
		std::vector<const RE::NiAVObject*> switchesApplied;
		bool switchResync = true;
		// Resident records: the slots, each one's position in the list, and the patch it was given.
		std::vector<std::uint32_t> residents;
		std::vector<std::uint32_t> residentPos;
		std::vector<ResidentPatch> residentPatches;
		// What the residents keep (KeepResidentsAlive): the used pipeline and material sets, each pipeline's template member, the
		// trees' members and each fade root's centre member. Kept by changes: a join, a patch, a drop or a tree's listing names
		// its slot (NoteResidentTouched), and only those are counted again; made whole again only when every residency ended or
		// the tables were reset (residentMaintenanceDirty).
		bool residentMaintenanceDirty = true;
		void NoteResidentTouched(std::uint32_t a_slot) { residentsTouched.push_back(a_slot); }
		/** @brief Slots by a key (a pipeline, a fade root, the trees), each in one list at a time: swap-removed, the first kept. */
		struct SlotMembers
		{
			std::vector<std::vector<std::uint32_t>> lists;  // by key
			std::vector<std::uint32_t> position;            // by slot: its place in its key's list
			void Add(std::uint32_t a_key, std::uint32_t a_slot)
			{
				if (lists.size() <= a_key)
					lists.resize(std::size_t(a_key) + 1);
				if (position.size() <= a_slot)
					position.resize(std::size_t(a_slot) + 1, ~0u);
				position[a_slot] = static_cast<std::uint32_t>(lists[a_key].size());
				lists[a_key].push_back(a_slot);
			}
			void Remove(std::uint32_t a_key, std::uint32_t a_slot)
			{
				auto& list = lists[a_key];
				const std::uint32_t at = position[a_slot];
				const std::uint32_t last = list.back();
				list[at] = last;
				position[last] = at;
				list.pop_back();
				position[a_slot] = ~0u;
			}
			std::uint32_t First(std::uint32_t a_key) const { return a_key < lists.size() && !lists[a_key].empty() ? lists[a_key].front() : ~0u; }
			std::size_t Count(std::uint32_t a_key) const { return a_key < lists.size() ? lists[a_key].size() : 0; }
			void Clear()
			{
				lists.clear();
				position.clear();
			}
			/** @brief Whether a_slot is listed under a_key (Remove's precondition). */
			bool Has(std::uint32_t a_key, std::uint32_t a_slot) const
			{
				return a_key < lists.size() && a_slot < position.size() && position[a_slot] < lists[a_key].size() && lists[a_key][position[a_slot]] == a_slot;
			}
		};
		// What each resident slot was counted under (kNone: not counted).
		struct ResidentCounted
		{
			static constexpr std::uint32_t kNone = ~0u;
			std::uint32_t pipeline = kNone, material = kNone;
			bool tree = false;
		};
		std::vector<ResidentCounted> residentCounted;  // by slot
		std::vector<std::uint32_t> residentsTouched;   // since the last KeepResidentsAlive
		SlotMembers pipelineMembers, fadeRootMembers, treeMembers;  // the trees' under key 0
		std::vector<std::uint32_t> materialMembers;    // by material slot: how many residents
		// The pipelines and materials the accumulate phase's joins marked used this frame: cleared again where no resident holds them.
		std::vector<std::uint32_t> joinMarkedPipelines, joinMarkedMaterials;
		ankerl::unordered_dense::set<const RE::BSGeometry*> residentJoining;  // this frame's resident passes, until patched
		ankerl::unordered_dense::set<const RE::BSGeometry*> residentLayerJoining;  // the layers' (accumulatedLayerPasses)
		ResidentStats residentStats;
		// The DCLF set (CommitSet): each object slot's committed phases, the bound slots waiting for readiness, and the
		// publication. The commit reads the change log from its own cursor, and the waiting slots again when a lookups
		// generation moved.
		std::vector<std::uint32_t> setWaiting;
		std::vector<std::uint8_t> setWaitingMark;  // parallel to objects: in setWaiting
		std::vector<std::uint32_t> setQueue;
		// Slots that joined the main phase at the last commit and take part in the reflection's: evaluated again at this one, when
		// their last frame's main membership gives them the phase.
		std::vector<std::uint8_t> setQueueMark;    // parallel to objects: in setQueue
		std::vector<std::uint8_t> setLacking;      // parallel to objects: the occluder and reflection phases it takes part in and is no member of
		std::array<std::uint32_t, 3> setLackingCount{};  // SetLacking's: per occlusion map, then the reflection
		std::vector<std::uint8_t> setPartialMark;  // per slot: out of the set with some of its phases ready (SetStats::partial)
		std::uint32_t setPartialCount = 0;
		// The last commit's decision, applied at the next ApplySet: each slot's phases and lacking phases, and the slots it changed
		// with the geometry each held when the commit decided (a slot that holds another one at ApplySet takes nothing).
		std::vector<std::uint8_t> setPhasesNext, setLackingNext;
		std::vector<std::pair<std::uint32_t, const RE::BSGeometry*>> setApply;
		// Parallel to objects: the geometry the last commit that decided a slot decided for (what a whole application after a
		// withdrawal applies its setPhasesNext to); and whether the claims are withdrawn (WithdrawSet) until the next ApplySet.
		std::vector<const RE::BSGeometry*> setGeometryNext;
		bool setWithdrawn = false;  // the frame's claims (WithdrawSet), until the next installation
		/** @brief T6b5: a LOD gate flipped by a commit (UpdateGates), released by the frame installing its publication (ReleaseGates). */
		struct GateFlip
		{
			std::uint64_t token = 0;
			std::vector<std::pair<const RE::BSGeometry*, std::uint8_t>> incoming;  // the incoming base geometries claimed, with the flip's phases
			std::vector<const RE::BSGeometry*> outgoing;                          // the outgoing base geometries dropped
			std::uint64_t requestNs = 0, flipNs = 0;                              // its event's push, the flip (steady clock)
			std::uint32_t requestFrame = 0, flipFrame = 0;                        // the render thread's frames then (SceneCapture::Frame)
		};
		/**
		 * @brief What one publication's passes made for the frame and the frame's start takes (T6b3a), carried by the publication log in
		 * the publication's order: the main claims' changes against the publication before it (taken up to the installed one), and the
		 * frame inputs (taken up to the newest one taken): the walks' placement plans, the slots named for their shading, the fade and
		 * tree seed requests, the switches the walks applied (switchResync: PrimaryCull reads them all), the import leases of the geometry
		 * slots cleared, and the actors' wetness membership changes.
		 */
		struct PublicationDeltas
		{
			std::uint64_t publication = 0;  // its publication's sequence
			std::uint32_t commitFrame = 0;  // that publication's commit (the install delay)
			// T6b3d: that publication's oldest event (ScenePublication::eventNs, eventFrame): the frame installing it, or one after it the
			// builder skipped past to, measures the event to its adoption.
			std::uint64_t eventNs = 0;
			std::uint32_t eventFrame = 0;
			std::vector<const RE::BSGeometry*> joined, left;
			std::vector<std::shared_ptr<const PlacementPlan>> plans;
			std::vector<ShadingItem> shading;
			std::vector<FadeSeedItem> fadeSeeds, treeSeeds;
			std::vector<const RE::NiAVObject*> switches;
			bool switchResync = false;
			std::vector<std::shared_ptr<const void>> retiredImports;
			std::vector<Tables::ActorWetnessChange> actorWetness;
			std::vector<LightEntryChange> lightEntries;
			std::vector<GateFlip> gateFlips;  // T6b5: the LOD gates its commits flipped (ReleaseGates)
		};

		/**
		 * @brief T6b5: the LOD gates (Engine/LodGates.h; dclf-async-publication.md, "T6b5"), the coordinator's. A gate opens with its event
		 * (ApplyGateEvent): its incoming roots gated in the mirror (their own hide not the scene's), the tracked geometries under its roots
		 * tagged (a walk of the mirror per root; later arrivals by AddGeometry). The commit holds a gated incoming slot's phases out of the set
		 * (GateWithhold: stored per slot with its readiness), and flips the gate in the commit that finds every incoming geometry a whole
		 * member or ineligible and every root mirrored (UpdateGates): the held phases applied, the outgoing blocks' dropped and held out until
		 * their detach, the flip carried by the publication (PublicationDeltas::gateFlips) to the frame that installs it (ReleaseGates). The
		 * gate retires with its outcome (ApplyGateOutcomes: flipped, or forced by a safety path on the engine side).
		 */
		static constexpr std::uint8_t kGateIncoming = 1, kGateOutgoing = 2;
		struct LodGate
		{
			enum class State : std::uint8_t
			{
				Open,     // the incoming withheld, the outgoing members
				Flipped,  // the flip published: the incoming applied, the outgoing held out (Tracked::gateHeldOut); waiting for its outcome
			};
			State state = State::Open;
			std::vector<const void*> incoming, outgoing;  // the roots (keys: the blocks' nodes), less those detached since
			ankerl::unordered_dense::set<RE::BSGeometry*> incomingGeometries, outgoingGeometries;  // the tracked ones tagged (keys)
			std::uint32_t pending = 0;     // incoming geometries neither whole members nor ineligible (UpdateGates)
			std::uint32_t unmirrored = 0;  // incoming roots, or nodes under them, the mirror lacks a record of
			std::uint64_t stampNs = 0;     // its event's push (the request), steady clock
			std::uint32_t stampFrame = 0;
			std::uint64_t flipNs = 0;
			std::uint32_t flipFrame = 0;
		};
		struct GateSlot
		{
			const RE::BSGeometry* geometry = nullptr;  // the gated geometry the commit last evaluated the slot for
			std::uint8_t phases = 0;                   // its phases then (the held ones while its gate is open)
			bool ready = false;                        // whole: its phases all it takes part in, not waiting
		};
		struct GateOutcome
		{
			std::uint64_t token = 0;
			bool forced = false;
		};
		struct GateStats
		{
			std::uint64_t opened = 0, flipped = 0, retiredFlipped = 0, retiredForced = 0, forcedOpen = 0, unknownOutcomes = 0, superseded = 0;
			std::uint64_t rootsDetached = 0, taggedIncoming = 0, taggedOutgoing = 0, heldOut = 0, emptyIncoming = 0;
			// Flips deferred for an incoming record the next publication claims first (UpdateGates); under the parities, flipped incoming
			// geometries the flip's own publication does not claim (TakeDeltas: the coordinator's side of <- GATE HOLE).
			std::uint64_t structureDeferred = 0, publishHoles = 0;
			std::string firstPublishHole;
			std::vector<double> requestToFlipMs;
			std::vector<std::uint32_t> requestToFlipFrames;
		};
		ankerl::unordered_dense::map<std::uint64_t, LodGate> lodGates;     // by token
		struct GateRoot
		{
			std::uint64_t token = 0;
			std::uint8_t side = 0;  // kGateIncoming, kGateOutgoing
		};
		ankerl::unordered_dense::map<const void*, GateRoot> gateOfRoot;     // a root (incoming or outgoing) -> its gate
		std::vector<GateSlot> gateSlots;                                     // by object slot
		std::vector<GateFlip> gateFlipsMade;                                 // since the last publication (TakeDeltas)
		std::vector<GateOutcome> gateOutcomesHeld;                           // taken, not applied yet (TakeGateOutcomes)
		std::uint32_t gateHeldOutCount = 0;                                  // entries with Tracked::gateHeldOut
		// Slots a gate's tag or retirement wants evaluated again, queued by the next commit at its start (a QueueSet after a commit's queue
		// ran would be dropped with it: UpdateGates tags inside the commit).
		std::vector<std::uint32_t> gateQueued;
		GateStats gateStats;
		/**
		 * @brief The coordinator, ApplyMirrorEvents: a Gate event (SceneTracker::EventType::Gate, in order with the batch's other events) opens
		 * its gate: its token, its roots (keys), its push stamp.
		 */
		void ApplyGateEvent(std::uint64_t a_token, std::span<const void* const> a_incoming, std::span<const void* const> a_outgoing, std::uint64_t a_stampNs,
			std::uint32_t a_stampFrame);
		/** @brief The coordinator, ApplyMirrorEvents: a detach in the world; the gates' roots among a_root and a_nodes leave their gates. */
		void NoteGateRootsDetached(const void* a_root, std::span<const void* const> a_nodes);
		/** @brief The tracked geometries the mirror holds under a_root tagged on a_token's a_side; whether the mirror lacked a record there. */
		bool TagGateRoot(std::uint64_t a_token, const void* a_root, std::uint8_t a_side);
		/** @brief AddGeometry: a_geometry tagged when a gate's root is on its mirror chain (up to its category node). */
		void TagGateAbove(RE::BSGeometry* a_geometry, Tracked& a_entry);
		void TagGate(RE::BSGeometry* a_geometry, Tracked& a_entry, std::uint64_t a_token, std::uint8_t a_side);
		/** @brief Off its gate (EraseTracked, a retag, the outcome) and no longer held out. */
		void UntagGate(RE::BSGeometry* a_geometry, Tracked& a_entry);
		/** @brief The rescan (ApplyBatch): the gates' tags dropped with the entries (the rescan's adds tag again). */
		void ResetGateTags();
		/** @brief CommitSet: a_slot's phases as applied (0 for a gated incoming slot whose gate is open, or one held out), recorded per slot. */
		std::uint8_t GateWithhold(std::uint32_t a_slot, std::uint8_t a_phases, bool a_wait, std::uint32_t a_drawn, bool a_live);
		/** @brief GateWithhold's verdict alone (the set parity's): whether the commit applies 0 to a_slot for a gate. */
		bool GateWithholds(std::uint32_t a_slot) const;
		/** @brief Whether a gated incoming geometry is ready: ineligible for a verdict of the gate's time, or every slot of it whole. */
		bool GateGeometryReady(const RE::BSGeometry* a_geometry, const Tracked& a_entry) const;
		/**
		 * @brief CommitSet, after its queue: the open gates' pending counts taken again, and those ready (every root mirrored, every incoming
		 * geometry ready, a live scene) flipped: the slots and phases to apply into a_apply, the flip onto gateFlipsMade.
		 */
		void UpdateGates(bool a_live, std::vector<std::pair<std::uint32_t, std::uint8_t>>& a_apply);
		/** @brief The coordinator, before CollectEvents: LodGates' outcomes posted so far, held (gateOutcomesHeld). */
		void TakeGateOutcomes();
		/**
		 * @brief The coordinator, after the batch: the held outcomes applied - each gate retired, its tags and gated roots cleared (a forced one's
		 * incoming classified again and committed with their own phases) - unless a_applied is false (a load screen's pass: its events carried).
		 */
		void ApplyGateOutcomes(bool a_applied);
		void RetireGate(std::uint64_t a_token, bool a_forced);
		/** @brief The coordinator's report line (ReportCoordinator), empty without any gate activity; resets the counts. */
		std::string GateReport();
		// T6b5, render thread: the flips of the publications installed since the last ReleaseGates (HandOverAtFrameStart), and the releases'
		// counts and observers.
		std::vector<GateFlip> frameGateReleases;
		struct GateReleaseStats
		{
			std::uint64_t released = 0, refused = 0, checked = 0, holes = 0, coverage = 0, withdrawn = 0;
			std::vector<double> flipToReleaseMs, requestToReleaseMs;
			std::vector<std::uint32_t> flipToReleaseFrames;
			std::string firstHole, firstCoverage;
			std::uint32_t interval = 0;
			bool any = false;
		} gateReleaseStats;
		/**
		 * @brief The coordinator: ApplySet, RevokeUndrawnClaims, PublishTables, then the publication's deltas onto the log and the publication
		 * to the snapshot builder (IndirectDraws::PostSnapshotWork; in that order: whoever adopts the publication finds its node).
		 */
		void PublishScene();
		/** @brief The coordinator: what its passes made for the frame since the last publication, moved into a_deltas (the log's node). */
		void TakeDeltas(PublicationDeltas& a_deltas);
		PublicationLog<PublicationDeltas> publicationLog;  // the coordinator -> the frame's start (every one)
		// The render thread's: the installed publication (the adopted snapshot's, HandOverAtFrameStart).
		std::shared_ptr<const ScenePublication> installed;
		/** @brief Render thread: the installed publication, null before the first. */
		std::shared_ptr<const ScenePublication> Newest() const { return installed; }
		// The render thread's cursors into the log: the frame inputs taken and the claims' changes taken (both up to the installed one,
		// the claims' never past the frame inputs'). Null: the log's first node.
		PublicationLog<PublicationDeltas>::Node* deltasCursor = nullptr;
		PublicationLog<PublicationDeltas>::Node* notesCursor = nullptr;
		/** @brief Render thread: the replaced or skipped publication back to the coordinator, which drops it (retiredPublications). */
		void ReturnPublication(std::shared_ptr<const ScenePublication>&& a_publication);
		// The render thread's leases the coordinator drops (T6b3a: retirement off the render thread), at the start of its passes.
		// T6b3d: no wake: the frame returns them at its start, whose frame-input post brings the pass that drops them.
		EventQueue<std::shared_ptr<const ScenePublication>, 64> retiredPublications;
		/** @brief The coordinator, at the start of its passes: the leases the render thread gave back, dropped. */
		void DropReturnedPublications();
		std::uint64_t publicationSequence = 0;
		std::uint32_t publicationCommitFrame = 0;    // the commit the next publication applies
		std::uint32_t publicationCommitToggles = 0;  // the toggles generation that commit was made under
		std::shared_ptr<const SetSnapshot> publicationClaims;
		std::vector<const RE::BSGeometry*> publicationJoined, publicationLeft;
		// The frame's: the installed publications' claim changes not told to PrimaryCull yet, whether one is waiting, the claims it was
		// told of, and the lacking counts.
		std::vector<std::pair<std::vector<const RE::BSGeometry*>, std::vector<const RE::BSGeometry*>>> installNotes;
		bool installPending = false;
		std::shared_ptr<const SetSnapshot> notedClaims;
		std::array<std::uint32_t, 3> frameLackingCount{};
		PublicationStats publicationStats;
		std::vector<std::uint32_t> setApplyMark;  // parallel to objects: its index in setApply plus one, 0 when not in it
		std::uint32_t setCommitFrame = 0;
		std::uint32_t setCommitToggles = 0;  // the toggles generation of the frame inputs the last commit was made under
		// The claims as applied (the frame's), kept apart from Tables::setPhases, which a freed slot clears: what RevokeUndrawnClaims
		// checks the records against, with the geometry each base slot is claimed under.
		std::vector<std::uint8_t> setPhasesApplied;
		std::vector<const RE::BSGeometry*> setGeometryApplied;
		LogCursor revokeCursor;
		std::uint64_t revokedGeometries = 0, revokedMain = 0;
		// R3b, structural stamps: the slots whose structure (kStructureCauses) changed since the selected revision's join, read
		// off revokeCursor before ApplySet's own notes and at the join; a claim among them is taken back at the join (its shapes
		// were made without the change), and is the engine's until a revision made after it is selected (the next ApplySet).
		std::vector<std::uint32_t> structureChanged;
		std::uint64_t revokedStructureGeometries = 0;
		/** @brief The structural changes revokeCursor has not read, into structureChanged (its position advanced). */
		void NoteStructureChanges();
		/**
		 * @brief After the frame's scene work: a claimed phase whose record the work stopped drawing (a slot freed, a main member
		 * whose binding went) is taken back from the snapshot the engine's hooks read, so the engine draws it this frame. By the
		 * tables' change log since ApplySet.
		 */
		void RevokeUndrawnClaims();
		/**
		 * @brief What the scene work tells PrimaryCull (render-thread state), held while it runs (T6b3a: a lock-free queue the render
		 * thread drains at the joins and the frame's start, in the order the work made them): a hidden store's key (the kept scene lists
		 * built again), a member the accumulate phase dropped, every residency ended.
		 */
		struct PrimaryNote
		{
			enum class Kind : std::uint8_t
			{
				HiddenKey,
				MemberLost,
				AllMembersLost
			};
			Kind kind = Kind::HiddenKey;
			const void* key = nullptr;  // the hidden store's key, or the member's geometry (keys: nothing is dereferenced)
		};
		EventQueue<PrimaryNote> primaryNotes;
		/** @brief Render thread (the joins, the frame's start): the held notes to PrimaryCull. */
		void DeliverPrimaryNotes();
		/**
		 * @brief Render thread, EarlyPrepass (a frame input, T6b2c step 7): the captures the joins asked for (materialRequestQueue),
		 * each read off the property the join held, onto MaterialPort::captures; every request answered (materialRequestsAnswered).
		 */
		void ServeMaterialRequests();
		bool holdLostMembers = false;  // RunSceneWork, from the joins on: the members it drops are held for PrimaryCull (primaryNotes)
		// A join whose material has no capture held (MaterialPort::captures: its writer's, its attach's; one unused for
		// kMaterialSnapshotFrames is let go) asks the render thread for one and waits (bindRetry). The scene work can read no
		// property's material: the render thread's capture at EarlyPrepass is the capture point for it.
		struct MaterialRequest
		{
			// T6b1b: the property the join read the material of (its entry's, held), not the material: the render thread takes the
			// material's reference, when the property has it still (else the request is stale: its event the next batch's).
			RE::NiPointer<RE::BSShaderProperty> property;
			const RE::BSShaderMaterial* material = nullptr;
			std::uint32_t pass = 0;
		};
		// The scene work's requests to the render thread, and its answers back (the material, captured or stale): lock-free both ways.
		struct MaterialAnswer
		{
			const RE::BSShaderMaterial* material = nullptr;
			bool captured = false;  // false: the property held another material by then (the request stale)
		};
		EventQueue<MaterialRequest> materialRequestQueue;
		EventQueue<MaterialAnswer> materialRequestsAnswered{ &WakeSceneMaterialAnswer };
		ankerl::unordered_dense::set<const RE::BSShaderMaterial*> materialRequested;  // the scene work's: asked, not answered yet
		// The material references the coordinator let go of (RecycleRetired, HandBackHeld), released at Present (ReleaseHandedBack; T6b3a:
		// pushed where they are let go, never handed over at a join).
		EventQueue<MaterialReference> materialsReleased;
		/**
		 * @brief What the coordinator let go of that a published version of the tables may still name (step 6e E3): the slots
		 * (Tables::retiring, the SlotTables'), the geometry slots' buffer owners, the material slots' engine references and every
		 * engine reference handed back. Each publication holds the chain's node opened with it (Tables::retirementHold); a batch comes
		 * back once no publication up to it lives, and RecycleRetired puts its slots on their free lists and its references where
		 * they were released before (retiredImports; materialsReleased, EngineReleases, batchesReleased).
		 */
		struct RetiredBatch
		{
			std::vector<Tables::RetiredSlot> slots;
			std::vector<std::shared_ptr<const void>> imports;
			std::vector<MaterialReference> materials;
			std::vector<RE::NiPointer<RE::NiRefObject>> references;
			std::vector<std::shared_ptr<EventBatch>> events;  // applied event batches (their tracker events hold detached subtrees)
			bool Empty() const { return slots.empty() && imports.empty() && materials.empty() && references.empty() && events.empty(); }
		};
		RetirementChain<RetiredBatch> retirement;
		struct RetirementStats
		{
			std::uint64_t retired = 0, recycled = 0, dropped = 0, batches = 0;
		} retirementStats;
		/** @brief The coordinator: the tables' retiring slots into the chain's open node (before a publication). */
		void RetireIntoChain();
		/** @brief The coordinator: the batches no publication names any more, back on the free lists. */
		void RecycleRetired();
		std::vector<std::uint32_t> bindRetry;                // joins that waited for a material, bound again next phase

		// The scene pass (RunSceneWork): what it holds for the render thread while it runs.
		bool inSceneTask = false;       // RunSceneWork on the lane (not inline)
		bool holdPrimaryNotes = false;  // the scene work's passes: PrimaryCull's notes are held (primaryNotes)
		std::uint32_t publishedSunGeneration = 0;  // render thread: the newest publication's sun generation as the frame took it
		std::vector<const RE::BSGeometry*> setGeometry;  // parallel to objects: the geometry a base member is published under
		ankerl::unordered_dense::map<const RE::BSGeometry*, std::uint32_t> setMemberSlot;  // published geometry -> its slot
		// Members whose registration met a fade DCLF does not model (PassCapture::TakeUnmodelledFades): out of the set until it ends.
		ankerl::unordered_dense::set<const RE::BSGeometry*> setFadeHeld;
		LogCursor setCursor;
		// The readiness sources a waiting slot may wait on (SetWaitCause bits by index), and their stamps as the waiting slots were last
		// checked against: the lookups' versions, the shadow lookups' generation, the constants' stamp, the scene buffers' fit serial.
		static constexpr std::uint32_t kWaitSources = 4;
		static constexpr std::uint8_t kWaitLookups = 1, kWaitShadow = 2, kWaitConstants = 4, kWaitFit = 8;
		static constexpr std::uint8_t kWaitOther = 16;  // any source moving takes it again (geometry, decal slot, reflection, layer partner)
		/** @brief The readiness source a waiting reason (SetStats::waitingBy's index) is released by. */
		static constexpr std::uint8_t WaitCauseOf(std::uint32_t a_why)
		{
			switch (a_why) {
			case 0:
			case 1:
			case 2:
			case 3:
				return kWaitLookups;
			case 7:
			case 13:
				return kWaitShadow;
			case 9:
				return kWaitConstants;
			case 10:
			case 12:
				return kWaitFit;
			default:
				return kWaitOther;
			}
		}
		std::array<std::uint64_t, kWaitSources> setReadiness{ ~0ull, ~0ull, ~0ull, ~0ull };
		std::vector<std::uint8_t> setWaitCause;  // by slot: what it waits on (its last evaluation's), when waiting
		std::vector<std::uint8_t> setWaitWhy;    // by slot: the first reason (SetStats::waitingBy's index) of its last wait (T6b0)
		std::uint64_t setShadowModes = ~0ull;      // the shadow modes and states caster readiness was last taken under
		std::uint32_t setPhaseMask = ~0u;          // the phases DCLF draws (toggles) the last commit evaluated with
		std::shared_ptr<SetSnapshot> setBuilding;  // the next publication, kept up to date by each commit
		std::shared_ptr<const SetSnapshot> setSnapshot;
		bool setSnapshotDirty = true;
		SetStats setStats;
		/** @brief Queues a slot for the next commit. */
		void QueueSet(std::uint32_t a_slot);
		/**
		 * @brief A slot taken out of phases of the applied set after its commit (the joins binding it again before this pass's commit,
		 * a claim revoked after it): out of the commit's decision and the snapshot the claims are made from too, queued, so the next
		 * commit decides it again from what it has now, and marked for ApplySet (MarkSetApply), so a slot that commit leaves out still
		 * reaches the claims' notes. Without this the commit's copy keeps the phases: its next decision of the same phases is no change,
		 * the record stays out of them and the claims keep them, and nobody draws it. A base and its layer leave the main phases
		 * together, the applied tables too (CommitSet's rule: the claim is the base geometry's).
		 */
		void LeaveSet(std::uint32_t a_slot, std::uint8_t a_lost, bool a_partner = true);
		/**
		 * @brief A slot whose committed phases or lacking phases changed, for the next ApplySet, with the geometry it holds now (an entry
		 * not applied yet takes this one's geometry: the decision is now for it).
		 */
		void MarkSetApply(std::uint32_t a_slot);
		/** @brief The claims' snapshot of setBuilding, made again when a commit or LeaveSet changed it. */
		void RefreshSetSnapshot();
		/**
		 * @brief The phases DCLF can draw (the toggles, and what it has set up: IndirectDraws::ShadowCapability, ReflectionDrawable): an
		 * object's mask is its participation within them. A capability, not what a frame drew: it changes only on a toggle, a load,
		 * a failure or a setup completing, so which views the engine asks for never changes the set.
		 */
		static std::uint32_t SetCapability();
		/** @brief The phases an object takes part in among a_drawn, from its record (no readiness). */
		std::uint8_t SetParticipation(std::uint32_t a_slot, std::uint32_t a_drawn) const;
		/** @brief Whether everything the object's main phase draws with is ready; a_why: SetStats::waitingBy's index when not. */
		bool MainReady(std::uint32_t a_slot, std::uint32_t& a_why) const;
		// Sun entry nodes something was attached under or detached from since the last walk (keys).
		std::vector<const RE::NiAVObject*> dirtyRoots;
		ankerl::unordered_dense::map<const void*, std::vector<RE::BSGeometry*>> propertyDependents;
		ankerl::unordered_dense::map<const RE::NiAVObject*, std::vector<RE::BSGeometry*>> rootDependents;
		/**
		 * @brief A reference to every root listed in rootDependents or lightDependents (step 6e F1), taken when it is first listed (a
		 * live ancestor of the geometry listing it) and handed back when neither lists it any more. What the scene work hands out
		 * of a root - the placement plan's roots and entries, the candidates' snapshots - is a copy of it: no reference is ever
		 * made from a key, which may name a node already gone.
		 */
		ankerl::unordered_dense::map<const RE::NiAVObject*, RE::NiPointer<RE::NiAVObject>> rootOwners;
		/** @brief Owns a_root while a dependents list names it: from the batch's pin (a_live: a live walk's pointer, the category walk's). */
		bool OwnRoot(const RE::NiAVObject* a_root, bool a_live);
		/** @brief Hands the root's reference back once neither dependents list names it. */
		void ReleaseRootOwner(const RE::NiAVObject* a_root);
		void ReleaseRootOwners();
		/** @brief The listed root's reference (a copy); null, and counted, for a root no list names. */
		RE::NiPointer<RE::NiAVObject> OwnedRoot(const RE::NiAVObject* a_root);
		std::uint64_t unownedRoots = 0;  // since the last report: roots handed out that no list names (a defect)
		// A listed reference root's reference, and back (the root is a key once its last dependent leaves: it may be gone).
		ankerl::unordered_dense::map<const RE::NiAVObject*, const void*> rootReference;
		ankerl::unordered_dense::map<const void*, const RE::NiAVObject*> referenceRoot;
		std::array<std::uint32_t, static_cast<std::size_t>(Ineligible::Count)> buckets{};
		struct DeltaStats
		{
			std::uint32_t walks = 0, full = 0;
			std::uint64_t evaluated = 0, perFrame = 0, pending = 0, property = 0, node = 0, roots = 0, fade = 0, geometryDirty = 0, restored = 0, moved = 0, kept = 0;
			std::uint64_t propertyEvents = 0, nodeEvents = 0, reread = 0;
			std::uint64_t facePublished = 0, faceUpdated = 0, faceWritten = 0;
			std::uint64_t switchEvents = 0, switchChanges = 0, switchCatchUps = 0, switchReclassified = 0, attachCatchUps = 0;
			std::uint64_t live = 0;
			std::uint32_t evaluatedMax = 0;
		} delta;
		/**
		 * @brief A face shape's region of the positions buffer, kept while a stream of the shape holds it (the buffer grows to
		 * hold every region: IndirectDraws' ReserveSceneTables). EndFaceWalk frees the regions of the streams freed since
		 * (Tables::faceStreamsReleased). The walk's thread alone.
		 */
		std::uint32_t FaceRegionOf(const RE::BSGeometry* a_geometry, std::uint32_t a_vertexCount);
		void EndFaceWalk();
		void ClearFaceRegions();
		struct FaceRegion
		{
			std::uint32_t first = 0, count = 0;
		};
		ankerl::unordered_dense::map<const RE::BSGeometry*, FaceRegion> faceRegions;
		std::vector<std::pair<std::uint32_t, std::uint32_t>> faceRegionFree;  // (first, count), sorted, coalesced
		std::uint32_t faceRegionTop = 0;
		// The tracked face shapes by head (membership: ListFaceShape, UnlistFaceShape), and the heads' publications this walk.
		ankerl::unordered_dense::map<const RE::BSFaceGenNiNode*, std::vector<RE::BSGeometry*>> faceHeads;
		std::vector<const RE::BSFaceGenNiNode*> facePublished, faceStale;
		std::vector<RE::BSGeometry*> skinnedObjects;  // this walk's skinned objects, in object order
		std::uint32_t AllocateGeometrySlot();
		std::uint32_t AllocatePipelineSlot();
		std::uint32_t AllocateMaterialSlot();
		/**
		 * @brief The geometry slot for a TriShape: found, refreshed in place, or newly resolved.
		 * @return the slot, or Tables::kSlotFree when the buffers cannot be made stable for the graph.
		 */
		/** @brief What a geometry slot is resolved from: its key in geometryIndex, its buffers and its counts. */
		struct GeometrySource
		{
			const RE::BSGraphics::TriShape* key = nullptr;
			ID3D11Buffer* vertexBuffer = nullptr;
			ID3D11Buffer* indexBuffer = nullptr;
			std::uint64_t vertexDesc = 0;
			std::uint32_t vertexCount = 0;
			std::uint32_t indexCount = 0;
			std::uint32_t firstIndex = 0;  // a range of the index list (object LOD's visible ranges); 0 for the whole
			bool layer = false;            // a layer's second index list (Tables::geometryLayerKey)
		};
		/** @brief The buffers order[a_first...]'s geometry slots will resolve, prefetched in one batch (GpuResources::Prefetch). */
		void PrefetchGeometryBuffers(std::size_t a_first);
		std::uint32_t ResolveGeometrySource(const GeometrySource& a_source, PartTimer& a_timer);
		/** @brief The geometry slot of a multi-index shape's second index list (its layer's draw), or Tables::kSlotFree. */
		std::uint32_t ResolveLayerGeometrySlot(const SceneCapture::GeometryRecord& a_geometry, PartTimer& a_timer);
		/** @brief The geometry slot of a geometry's TriShape, or of its skin partition a_partition (T6b1b: from its record). */
		std::uint32_t ResolveGeometrySlot(const SceneCapture::GeometryRecord& a_geometry, const SceneCapture::GeometryRecord::Partition* a_partition, PartTimer& a_timer);
		/**
		 * @brief Object LOD (dclf-lod.md): the geometry slots of a partly hidden shape's visible ranges, linked by nextPartition from
		 * the returned first (Tables::kSlotFree when one cannot be resolved), each keyed by its range's first segment record.
		 */
		std::uint32_t ResolveLodRangeSlots(const SceneCapture::GeometryRecord& a_geometry, const std::vector<LodSegments::Range>& a_ranges, PartTimer& a_timer);

		/** @brief Consume reference changes; retire unused materials and expire idle geometry/pipeline slots before capture. */
		void SweepSlots();
		/**
		 * @brief Drops the slot tables, their maps and every cached derivation (by generation): the
		 * teardown paths, where the tables are cleared outright, must not leave a map or a Derived
		 * pointing at slots that no longer exist.
		 */
		void ResetSlotTables();

	public:
		/**
		 * @brief Render thread (T6b3a: a message): every cached classification verdict and derivation is to be dropped - a live toggle
		 * that enters the classification (Toggles.h) changed, or the feature came back on - by the coordinator at its next scene pass
		 * (the frame inputs' verdicts generation: InvalidateVerdicts).
		 */
		void RequestVerdictsInvalidated() { ++verdictsRequested; }

	private:
		/**
		 * @brief The coordinator: drops every cached classification verdict and derivation: a live toggle that enters the
		 * classification (Toggles.h) changed, and the caches witness the object rather than the switches.
		 */
		void InvalidateVerdicts();

		/** @brief After the loop: every object with bindings names live slots of this frame, or is neutralised. */
		void CheckObjectSlots(bool a_resolveBuffers);
		/**
		 * @brief CS_DCLF_SLOT_PROBE: re-derives what the used slots serve (a fresh material evaluation, a
		 * fresh buffer resolve) and logs the first difference of the frame. Startup diagnostics for the
		 * persistent tables; every frame, so it is a probe and not a mode.
		 */
		void ProbeSlots(bool a_resolveBuffers);
		/**
		 * @brief Render thread, CS_DCLF_PERSISTENT_PARITY (the parity observer): a few installed records a frame (every used one on a
		 * parity frame) against the engine's evaluation, in every part (JudgeMaterial), and the suspects due judged again.
		 */
		void ValidateMaterialSlice();
		Stats stats;
	};
}
