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
#include "Features/DrawcallLimitFix/Scene/LodSegments.h"
#include "Features/DrawcallLimitFix/Scene/TreeLod.h"
#include "ActorValueIndex.h"
#include "Features/DrawcallLimitFix/Engine/FaceSnapshots.h"
#include "Features/DrawcallLimitFix/Common/KeptState.h"
#include "Features/DrawcallLimitFix/Published/SceneIdentity.h"
#include "Features/DrawcallLimitFix/Common/SlotTable.h"
#include "Features/DrawcallLimitFix/Common/Retirement.h"
#include "LightingDescriptors.h"
#include "LocalShadows.h"
#include "ConstantEvaluator.h"
#include "Features/DrawcallLimitFix/Scene/FrameTables.h"
#include "Lookups.h"
#include "Records.h"
#include "SceneSet.h"

namespace DCLF
{
	/** @brief Attributes the time since the last call to one BuildPart (SceneStore/Internal.h; CS_DCLF_PROFILE). */
	struct PartTimer;
	struct SunCandidates;

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
	 * @brief The sub-zones of the scene tables (DrawcallLimitFix::BeginSceneFrame): the events applied (ApplyEvents) and the scene phase,
	 * block by block. Each is a Tracy zone and a sum in Stats::scenePartMs, reported under CS_DCLF_STATS. The first
	 * evaluation round's split by entry (EvaluateKind) is measured only under CS_DCLF_PROFILE.
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
		Count
	};

	inline constexpr std::array<const char*, static_cast<std::size_t>(ScenePart::Count)> kScenePartNames{
		"category nodes", "attach/detach", "validate", "structural events", "prologue", "sweep slots", "cull hidden bits",
		"schedule", "switch events", "evaluate", "geometry scan", "later rounds", "shadow sets",
		"object sweep", "sun candidates", "face walk", "walk parity", "shading", "placement plan"
	};

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
	 * The render thread ingests the engine's events (IngestEvents, at Present and at the frame's start) and the scene work
	 * applies them (ApplyEvents: SceneTracker's events and the loaded-cell changes); BuildFrame's scene phase (BeginSceneFrame, at Main::Draw,
	 * before the main camera's cull) refreshes the tracked geometries' records, and its accumulate phase
	 * (EarlyPrepass, once the cull job is done) patches the half of each record the main camera's accumulator decides.
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
			// Parallel to materials: new whenever the record's frame floats (MaterialSources' frame components, the texture transform,
			// the character light's modes) are written by the frame's post (step 6e B, CS_DCLF_FRAME_FLOATS=published), the record's
			// own values kept: a row is written again, its record not evaluated again.
			std::vector<std::uint32_t> materialFrameVersion;
			// Versions the builds' kept bindings key on (IndirectDraws' PersistentBindings), each new (NextVersion) whenever
			// what it covers is written with a different value: per pipeline its PerGeometry floats and what its pairs' records
			// read of it (the key, the permutation), the technique's being its row's (TechniqueRow); per
			// material slot the frame's floats (RefreshFrameMaterials, RefreshTextureTransforms). materialVersion covers the
			// rest of a material.
			std::vector<std::uint32_t> pipelineBindingVersion;    // parallel to pipelines
			// The frame-sourced components, texture transforms and writer re-evaluations are the frame's (FrameTables).
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
			// Skins of several partitions (CS_DCLF_SKIN_PARTITIONS): bit i draws partition i, walking the
			// geometry slots' nextPartition links from the object's geometryIndex (partition 0). 0 for every
			// other object, which draws its one geometry; kNoPartitions for a skin its LOD level draws nothing of. The scene
			// phase sets it from the fade node's LOD level, the row GetRenderPasses gives the main pass and the shadow views
			// alike, and rewrites it when the level changes (the fade watch: FUN_14147a430 writes the level).
			std::vector<std::uint16_t> skinPartitions;            // parallel to objects
			// No frame globals (the lighting, the fog, the character light's noise): the frame's capture (FrameCapture).
			// The property whose lighting pass supplied each pipeline's per-frame constants, kept so
			// RefreshFrameConstants can re-evaluate them once the main camera's state is current.
			std::vector<RE::BSShaderProperty*> geometryTemplate;  // parallel to pipelines
			// The PerTechnique values (and the technique's filter modes and shadow mask), one row per TechniqueKey - what
			// EvaluateTechnique reads of a pass descriptor - which every pipeline of the key shares (pipelineTechnique).
			// RefreshFrameConstants evaluates each used row once a frame and writes it only when it differs, versioning its
			// floats (constantsVersion) and its bindings (bindingVersion) apart. Rows are never freed: there are a few dozen.
			// The rows' keys (TechniqueKey); their constants are the frame's (FrameTables::techniques, by row).
			std::vector<std::uint32_t> techniqueKeys;
			ankerl::unordered_dense::map<std::uint32_t, std::uint32_t> techniqueRow;  // TechniqueKey -> row
			std::vector<std::uint32_t> pipelineTechnique;  // parallel to pipelines: its row
			// The constants the builds pack into the pipeline rows (step 6e A): the render thread's evaluations (EvaluateGeometry of a
			// pipeline's template pass, EvaluateTechnique of a row's key; engine code) posted with the key they were made for and
			// applied by the coordinator (ApplyConstantsPosts), published with the tables: a change is drawn a frame or two late, as a
			// join. A pipeline's block holds its own values (what a DCLF_BINDLESS draw reads of it; the frame's globals are the frame
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
			// Membership transitions produced only when the shadow dependency index changes.
			// False removes the old import owner; true requests the successor.
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
			 * holds every bound record to it. geometryLastUsed is the walk's: the frame that last wrote the geometry slot. The
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
			std::vector<std::uint32_t> retiredMaterialSlots;
			std::vector<std::uint32_t> retiredPipelineSlots;
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
			void TakeRetiredMaterialSlots(std::vector<std::uint32_t>& a_out) { a_out.clear(); a_out.swap(retiredMaterialSlots); }
			void TakeRetiredPipelineSlots(std::vector<std::uint32_t>& a_out) { a_out.clear(); a_out.swap(retiredPipelineSlots); }
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
				kRetiredMaterialLookup,  // a material slot's lookups to drop (retiredMaterialSlots)
				kRetiredPipelineLookup,  // likewise a pipeline slot's (retiredPipelineSlots)
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
			std::uint32_t accumulatedWithoutRecord = 0;  // slots re-resolved in place: TriShape reallocated at its address, or references evicted
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
			std::uint32_t castResolved = 0;  // RTTI casts actually walked (the rest reused a witness)
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
			// MaterialSources: materials written this frame, and what became of their slots; the live
			// evaluations the frame-sourced components were taken from (one per signature).
			std::uint32_t materialWrites = 0;
			std::uint32_t materialsRewritten = 0;
			std::uint32_t materialsDropped = 0;
			std::uint32_t materialsHeld = 0;  // referenced slots whose write could not be evaluated yet: kept, asked again
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

		static SceneStore& Get();

		/**
		 * @brief Render thread, the scene work joined (Present, and the frame's start before the kick): the frame's ingestion
		 * (dclf-async-publication.md, "Step 5: ingestion"). The engine's queues - attach and detach, fade snaps, fades, property
		 * and node events, switch events, object LOD's segment writes - drained into the pending batch in push order, and tree
		 * LOD's mirror (DecideTreeLod reads it next). Nothing is walked or evaluated. While a load screen is up the queues and
		 * the batch are discarded instead, and the first frame after the load rescans.
		 */
		void IngestEvents();
		/**
		 * @brief The pending batch applied to the scene state: the category refresh, the attached subtrees walked and the detached
		 * entries erased, the validation slice, the structural events. The scene work's first part (RunSceneWork, on the
		 * coordinator), or the render thread's at Present for a batch no scene work took (EventsUnapplied). What it lets go of is
		 * handed back (HandBack).
		 */
		void ApplyEvents();
		/** @brief Render thread at Present, after IngestEvents: the batch has waited through a Present. */
		void NoteEventsPresent();
		/** @brief Render thread at Present: a batch has waited a whole frame for scene work that did not run (menus, switched off). */
		bool EventsUnapplied() const;
		/**
		 * @brief Render thread at Present, the scene work joined and the read window closed: the engine references the scene work
		 * let go of, released (dropping the last one runs the engine's destructors, which belong on its main thread).
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
		 * scene.
		 */
		static bool CatchUpSwitch(RE::NiSwitchNode& a_switch);
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
			std::uint64_t materialWaits = 0, materialsServed = 0;  // joins that waited for a material record, records the render thread served
			std::uint64_t membershipKept = 0;    // members written again whose binding stands
			std::uint64_t layerUnpaired = 0;     // a base or a layer that joined without the other, and left again
			std::array<std::uint64_t, 4> failedBy{};  // (unused), no record, a frame verdict, material or extras
			std::uint64_t parityChecks = 0, parityChecked = 0, parityPass = 0, parityRecord = 0;
			std::uint64_t parityPending = 0;  // residents whose fade root is fading: the feedback's next decode ends them
			std::uint64_t registeredUnbound = 0;  // main-pass registrations of eligible objects DCLF has not bound (DrainCapture)
			std::string registeredUnboundFirst;
		};
		ResidentStats TakeResidentStats() { return std::exchange(residentStats, {}); }

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
		 * runs at EarlyPrepass, once the registration jobs have finished, and patches those records with
		 * what the main pass draws them with. Object indices are fixed from Scene onwards.
		 */
		enum class Phase : std::uint32_t
		{
			Scene,
			Accumulate
		};

		/** @brief Rebuilds one half of the CPU tables from the tracked set. */
		void BuildFrame(Phase a_phase);

		/** @brief The `[DCLF] scene delta`, change log and scene parity report lines since the last call, or empty. */
		std::string SceneReport();

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
		 * object at epoch time with that epoch's own eye. Terrain LOD's HighDetailRange moves vertices, so it is
		 * camera-independent and taken before the Z-prepass too (RefreshLodTechniqueRanges).
		 */
		void RefreshFrameConstants();
		/**
		 * @brief Terrain LOD's HighDetailRange in its technique rows (LodHighDetailRange), before the Z-prepass build is kicked.
		 *
		 * The vertex shader lowers the LOD land inside it, so the Z-prepass and the colour pass must draw a frame with the same
		 * range: a vertex lowered in one and not the other fails the colour pass's EQUAL test, and the triangle is not shaded
		 * (dclf-lod.md, "Terrain LOD"). The terrain manager writes it in Main::Update; RefreshFrameConstants evaluates the
		 * same value again.
		 */
		void RefreshLodTechniqueRanges();

		/**
		 * @brief The four textures the engine binds for a ProjectedUV draw (pixel slots 3, 8, 10 and 11:
		 * the projected diffuse, normal and detail maps and the projection noise), as SetupGeometry left
		 * them at a native draw. They are globals of the engine, changed only by the ReloadProjectedUVTextures
		 * console command, so one capture stands; it is refreshed by every native projected draw seen.
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

		/** @brief Drops everything (feature disabled or game unloaded). */
		void Clear();

		/**
		 * @brief The tables as the frame reads them (step 6c): the snapshot it accepted at its start, equal to the coordinator's
		 * tables there and held still while the scene work changes those (FrameView). Render thread and the frame's builds.
		 */
		const Tables& GetTables() const { return FrameView(); }
		/** @brief The coordinator's tables (the scene work's, the next publication's): the revision made for the next frame reads them. */
		const Tables& GetSceneTables() const
		{
			GuardFrameAccess("GetSceneTables");
			return tables;
		}
		/** @brief The frame's per-frame engine values (FrameTables): render thread, and the frame's builds kicked after its writes. */
		const FrameTables& GetFrameTables() const { return frameTables; }
		/** @brief Render thread, the coordinator idle (the frame's start): the frame's values sized and keyed to the tables, before any build. */
		void SyncFrameTables()
		{
			const Tables& view = FrameView();
			frameTables.SyncPipelines(view.pipelines, view.pipelineBindingVersion, tablesGeneration);
			frameTables.SyncTechniques(view.techniqueKeys.size());
			SyncFrameMaterials();
		}
		/** @brief Render thread, the accumulate work joined: the PerGeometry blocks of pipelines that have none (new, or a slot reused). */
		void RefreshNewPipelineConstants();
		/**
		 * @brief Render thread, BeginSceneFrame (the scene work joined): the newest published tables snapshot accepted for the frame
		 * (dclf-async-publication.md, "Step 6"). Immutable while held. Null before the first publication.
		 */
		/**
		 * @brief Render thread, the frame's start, the scene work joined and the revision selected (step 6e E3): the newest publication
		 * a_applicable(its commit frame) takes (IndirectDraws::SetApplicable), with the ones before it, becomes the installed one and
		 * its tables the frame's; none: the installed one stands, tables and claims together. A change after the last publication is
		 * published first (the coordinator is idle).
		 */
		void SelectPublication(const std::function<bool(std::uint32_t, const std::shared_ptr<const void>&)>& a_applicable);
		/** @brief Render thread, after SelectPublication and the coverage decision: the installed publication's claims made the frame's. */
		void InstallClaims();
		/** @brief Whether a publication is installed (none: the frame has no claims, WithdrawSet). */
		bool HasInstalled() const { return installed != nullptr; }
		/** @brief The installed publication's draws (IndirectDraws::BuildAhead), null without one. */
		std::shared_ptr<const void> InstalledDraws() const { return installed ? installed->draws : nullptr; }
		/** @brief Whether the scene work is out (kicked, not joined): nothing of the coordinator's may be touched by the frame. */
		bool SceneTaskInFlight() const { return sceneTaskInFlight.load(std::memory_order_relaxed); }
		/** @brief The coordinator: its copy of the lookups, as the frame's start refreshed them (TakeLookupsView). */
		const Lookups& CoordinatorLookups() const { return lookupsView; }
		/** @brief The coordinator's lookups as an immutable copy (the builds ahead hold it), made again only when they changed. */
		const std::shared_ptr<const Lookups>& SharedLookups() const { return lookupsShared; }
		/** @brief The installed publication's commit frame (IndirectDraws::NoteSetApplied). */
		std::uint32_t InstalledCommitFrame() const { return installed ? installed->commitFrame : 0u; }
		struct PublicationStats
		{
			std::uint64_t installed = 0, kept = 0, skipped = 0, pending = 0;
		};
		PublicationStats TakePublicationStats() { return std::exchange(publicationStats, PublicationStats{}); }
		/**
		 * @brief Render thread, the frame's start with the scene work joined (step 6c): what passes between the frame and the
		 * coordinator, both ways. To the frame: the retired slots and imports, the texture changes, the switches applied, the sun
		 * and light candidates. To the coordinator: the fade roots the depth commit holds, PrimaryCull's fade ownership, the lookups
		 * the commit judges readiness by (a copy), a reset of the lookups the scene work asked for.
		 */
		void HandOverAtFrameStart();
		/** @brief Render thread, the frame's start after the lookups' refresh: the coordinator's copy of them (lookupsView). */
		void TakeLookupsView();
		/** @brief Coordinator state read by the frame while the scene work ran (GuardFrameAccess), since the last call: count, first name. */
		struct ConstantsPostStats
		{
			std::uint64_t pipelinesPosted = 0, techniquesPosted = 0, pipelinesApplied = 0, techniquesApplied = 0, stale = 0;
			std::uint64_t recordsPosted = 0, framesPosted = 0, recordsApplied = 0, framesApplied = 0, materialsStale = 0;
		};
		ConstantsPostStats TakeConstantsPostStats() { return std::exchange(constantsPostStats, ConstantsPostStats{}); }
		std::pair<std::uint64_t, const char*> TakeFrameAccessViolations() { return { frameAccessViolations.exchange(0), frameAccessFirst.exchange(nullptr) }; }
		const std::shared_ptr<Tables>& AcceptedTables() const { return acceptedTables; }
		struct TablesPublication
		{
			std::uint64_t published = 0, reused = 0, made = 0, republished = 0;  // republished: at the accept, the copy was not the tables
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
		};
		/** @brief Render thread, the scene work joined: the retirement chain's counts since the last call, and what waits in it now. */
		std::string TakeRetirementReport()
		{
			const auto s = std::exchange(retirementStats, RetirementStats{});
			return fmt::format("{} retired, {} recycled in {} batches, {} dropped by a clear; {} object slots retiring", s.retired, s.recycled, s.batches, s.dropped,
				tables.objectsRetiring);
		}
		TablesPublication TakeTablesPublication() { return std::exchange(tablesPublication, TablesPublication{ .pool = tablesPool.size() }); }
		/**
		 * @brief Render thread, the depth commit (the scene task joined): its fade root rows hold a_held (Tables::fadeRootsJournal's
		 * version): the journal forgets what it has, and the next write opens a new version.
		 */
		void FadeRootsSent(std::uint64_t a_held) { fadeRootsSentHeld = a_held; }
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
			// The character light's noise this frame (MaterialSources::CharacterLightView, from a character-light signature's live
			// sample): the frame record's kCharacterLightRegister. Null while no character-light material is drawn.
			ID3D11ShaderResourceView* characterLightView = nullptr;
		};
		/** @brief Render thread: the frame's capture (latched by every commit). */
		const FrameCapture& GetFrameCapture() const { return frameCapture; }
		void TakeRetiredMaterialSlots(std::vector<std::uint32_t>& a_out) { a_out.clear(); a_out.swap(frameRetiredMaterialSlots); }
		void TakeRetiredPipelineSlots(std::vector<std::uint32_t>& a_out) { a_out.clear(); a_out.swap(frameRetiredPipelineSlots); }
		void TakeShadowTextureChanges(std::vector<std::pair<ID3D11ShaderResourceView*, bool>>& a_out) { a_out.clear(); a_out.swap(frameShadowTextureChanges); }
		const Stats& GetStats() const { return stats; }
		/** @brief The screen-door fading objects given bindings since the last call, and in how many frames. */
		std::pair<std::uint32_t, std::uint32_t> TakeFadingDrawn()
		{
			const std::pair result{ stats.fadingDrawn, stats.fadingFrames };
			stats.fadingDrawn = stats.fadingFrames = 0;
			return result;
		}
		std::uint32_t GetFrame() const { return frame; }
		/**
		 * @brief The PerMaterial float positions refreshed every frame in the records without a new version
		 * (MaterialSources): the build repacks them into a reused group. PS: the shader object's and the
		 * engine globals' (RefreshFrameMaterials); VS: TexcoordOffset (RefreshTextureTransforms).
		 */
		const std::vector<std::uint32_t>& GetMaterialPatchedFloats() const;
		const std::vector<std::uint32_t>& GetMaterialPatchedVSFloats() const;
		/** @brief Bumped whenever the slot tables are reset or every cached verdict is dropped (InvalidateVerdicts). */
		std::uint32_t GetTablesGeneration() const { return tablesGeneration; }
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
		/** @brief Render thread: whether a node is a light entry (LightDependentsOf). */
		bool IsLightEntry(const RE::NiAVObject* a_node) const
		{
			GuardFrameAccess("IsLightEntry");
			return LightDependentsOf(a_node) != nullptr;
		}
		/** @brief Render thread: whether a node is one of the cells' category nodes DCLF tracks (RefreshCategoryNodes). */
		bool IsCategoryNode(const RE::NiAVObject* a_node) const
		{
			GuardFrameAccess("IsCategoryNode");
			return categoryNodes.contains(static_cast<RE::NiNode*>(const_cast<RE::NiAVObject*>(a_node)));
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
		 * @brief The pre-resolved service results an epoch's build reads (Lookups.h). Filled by the render
		 * thread: the pipeline entries at EarlyPrepass, the descriptor entries inside an epoch's preparation.
		 */
		const Lookups& GetLookups() const { return lookups; }
		Lookups& MutableLookups() { return lookups; }

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
		static std::uint32_t LodRowOf(const RE::BSRenderPass& a_pass);
		/**
		 * @brief Which partitions of the skin the engine draws with this LOD row (bit i = partition i):
		 * BSDismemberSkinInstance::Unk_25 (AE 140d31f40) first skips a partition whose flag (Data byte 0) is
		 * clear, then NiSkinPartition::Unk_25 applies the table. 0 when it draws none.
		 */
		static std::uint32_t SkinPartitionMask(const RE::NiSkinInstance& a_skin, std::uint32_t a_lodRow);
		/**
		 * @brief Tables::skinPartitions for a geometry: 0 without a skin partition, kNoPartitions where its LOD row draws
		 * none, the mask for a skin of several partitions, 0 for one of a single drawn partition.
		 */
		static std::uint16_t SkinPartitionsOf(const RE::BSGeometry& a_geometry);

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
			return PipelineDrawableIn(FrameView(), lookups, a_pipeline);
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
		 * With scene revisions (R3c) it is called only once the selected revision was made at or after the commit (IndirectDraws::
		 * SetApplicable): the frame's claims are then always within the revision's set. Until then the last applied claims stand,
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
		 * @brief The frame's scene work on DCLF's coordinator (dclf-async-publication.md, "Phase 3"): the walk (its placements
		 * inline, under EngineReadWindow leases) and the set's commit, from Main::Draw to the first reader that needs this frame's
		 * walk (PrimaryCull's full-frustum hook; every later DCLF hook joins too). It starts with the frame's events (ApplyEvents),
		 * which the render thread ingested before the kick (IngestEvents).
		 *
		 * Render thread, in order: BeginFrame (the frame number, the published sun candidates' generation), ApplySet, the frame's
		 * ingestion, what the frame's claims need (the filters), then KickSceneTask. Between the kick and JoinSceneTask
		 * nothing on the render thread or the engine's threads reads the store but GetFrame, GetPublishedSunGeneration and the
		 * immutable publications (the set snapshot, the filters). What the walk has for other modules is held and handed over at
		 * the join (FinishSceneWork): PrimaryCull's hidden keys and lost members, and the claims whose record stopped drawing; the
		 * engine references it let go of, at Present (ReleaseHandedBack).
		 */
		void BeginFrame();
		/** @brief Coordinator (or inline): the scene work itself. a_task: on the coordinator (its placements take read leases). */
		void RunSceneWork(bool a_task);
		/** @brief Render thread: runs a_work on the coordinator (a_name: the job's, as the wait report shows it), joined by JoinSceneTask. */
		void KickSceneTask(std::function<void()> a_work, const char* a_name = "scene");
		/**
		 * @brief EarlyPrepass, render thread, the scene work joined: the accumulate phase's render-thread half (step 6b). The
		 * registrations drained (diagnostics; the frame's lighting pass), the material evaluations the last joins asked for
		 * (ServeMaterialRequests: SetupMaterial is the engine's), the material writes, texture transforms and validation slice.
		 */
		void PrepareAccumulatePhase();
		/**
		 * @brief The accumulate phase's joins on the coordinator (a_task) or inline: membership bound, residents kept, decals ordered;
		 * then the tables published (PublishTables). What it drops of PrimaryCull's and the new pipelines' PerGeometry blocks are the
		 * join's (FinishSceneWork).
		 */
		void RunAccumulateWork(bool a_task);
		/** @brief Render thread: waits for the scene task if one runs, then FinishSceneWork. Cheap when there is none. */
		void JoinSceneTask();
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
			std::uint64_t publications = 0;
			std::uint64_t patchedMember = 0;  // must be 0: the accumulate phase patched a member's binding (CommitSet keeps rebinds out)
			// Why a bound object waits, summed over the commits: its pipeline, its material, its pipeline's shadow mask, the shared
			// lookups (samplers, null and projected textures), its geometry, its decal slot, its layer partner, its shadow pipelines
			// or occlusion pipelines (or an alpha-tested caster's diffuse).
			// The reflection phase (8): its forward pipeline, or the object was no main member at the last commit.
			std::array<std::uint64_t, 10> waitingBy{};
			std::string firstWaiting;
		};
		SetStats TakeSetStats() { return std::exchange(setStats, {}); }
		/**
		 * @brief How many objects take part in an occlusion map's phase this frame without being its members (not ready): the engine
		 * must then register the map's scene, and its registration withholds the members (PassCapture). 0: every occluder DCLF knows
		 * is drawn by DCLF, and the engine's cull of the map can be skipped.
		 */
		std::uint32_t SetLacking(std::uint8_t a_phase) const { return a_phase == kSetOccluderSky ? frameLackingCount[0] : a_phase == kSetOccluderPrecipitation ? frameLackingCount[1] : 0u; }
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
		/** @brief Render thread (PrimaryCull): applied to the coordinator's state at the next frame's start (HandOverAtFrameStart). */
		void SetFadeRootsOwned(const std::vector<OwnedFadeRoot>& a_owned) { pendingFadeOwned = a_owned; }
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
		std::size_t StoodInFadeRoots() const
		{
			return static_cast<std::size_t>(std::count_if(fadeRootOwned.begin(), fadeRootOwned.end(), [](const auto& a_root) { return a_root.second; }));
		}
		/** @brief PrimaryCull: after frames the engine culled every entry (its OnVisible ran on the nodes), every owned root again from its node. */
		void ReseedOwnedFadeRoots() { pendingFadeReseed = true; }
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

		/** @brief True when the geometry sits under a tracked category node (used by coverage checks). */
		bool IsTracked(const RE::BSGeometry* a_geometry) const;
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
		RE::NiNode* CategoryNodeOf(RE::NiAVObject* a_object) const { return FindCategoryNode(a_object, nullptr); }
		/**
		 * @brief The frame a category node was found, and why the refresh that found it ran (0 signature change,
		 * 1 forced by a detach or rescan); false when unknown (diagnostics).
		 */
		bool GetCategoryInfo(const RE::NiNode* a_node, std::uint32_t& a_frame, std::uint8_t& a_cause) const;


		/** @brief Full eligibility (static and per-frame) of a tracked geometry; NotTriShape if untracked. */
		Ineligible Classify(RE::BSGeometry* a_geometry) const;

		/** @brief Whether a negative verdict follows only from what the pointer witnesses cover. */
		static bool CacheableVerdict(Ineligible a_reason);
		/** @brief Static eligibility of an arbitrary geometry, without the per-frame checks. */
		static Ineligible ClassifyStatic(RE::BSGeometry& a_geometry, LightingDescriptors* a_descriptors, const AccumulatedPass* a_accumulated = nullptr,
			RE::BSLightingShaderProperty** a_castCache = nullptr);
		/**
		 * @brief Static eligibility of a geometry's main-pass layer (LayerPropertyOf): its descriptors as the layer's pass draws
		 * them (decal group 3), with a_accumulated the layer's membership pass, or null.
		 */
		static Ineligible ClassifyLayer(const RE::BSGeometry& a_geometry, LightingDescriptors* a_descriptors, const AccumulatedPass* a_accumulated);

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
		/** @brief One actor-owned slot's wetness, as the frame's start captured it (Skin::GetWetness). */
		struct WetnessValue
		{
			std::uint32_t slot = kNoObjectSlot;
			std::array<float, 4> value{};
		};
		/** @brief Render thread, with the scene task joined: the slots named since the last take, once. */
		std::vector<ShadingItem> TakeShadingItems() { return std::exchange(shadingNamed, {}); }
		/** @brief Render thread, with the scene task joined: the slots named for the next frame, not taken (diagnostics). */
		const std::vector<ShadingItem>& PeekShadingItems() const { return shadingNamed; }
		/**
		 * @brief Render thread, at the frame's start with the scene task joined: every actor's wetness (Skin::GetWetness, whose first
		 * call a frame advances the actor's fade and is not thread-safe), fanned out to the meshes whose value changed or that joined.
		 */
		std::vector<WetnessValue> CaptureWetness();
		/** @brief Render thread, with the scene task joined: the plan its last walk made, once (null when no walk ran since). */
		std::shared_ptr<const PlacementPlan> TakePlacementPlan() { return std::exchange(placementPlanReady, nullptr); }
		/** @brief Render thread, with the scene task joined: the plan its last walk made, not taken (diagnostics). */
		const PlacementPlan* PeekPlacementPlan() const { return placementPlanReady.get(); }

	private:
		std::vector<std::shared_ptr<const void>> retiredImports;  // TakeRetiredImports
		// The slots named for the next frame's shading sample (ShadingItem): by the walk and the accumulate phase, taken at the frame's
		// start (TakeShadingItems). Every slot when the shading events are not installed.
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
		struct Tracked
		{
			RE::NiPointer<RE::BSGeometry> geometry;
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
			 * @brief The RTTI cast result, remembered against the property pointer that produced it.
			 *
			 * `netimmerse_cast` walks a chain of RTTI pointers. They are pointers, not strings, so it is
			 * cheap in instructions - but every step is a dependent load into a different allocation, and
			 * at ~3000 classifications a frame those misses are the one part of ClassifyStatic that is
			 * neither computation a memo can remove nor memory BuildFrame re-reads later anyway. Whether
			 * a property is a BSLightingShaderProperty is a property of its type, so the pointer is a
			 * complete witness.
			 */
			const RE::BSShaderProperty* castProperty = nullptr;
			RE::BSLightingShaderProperty* castResult = nullptr;
			// The property's own type, recorded when the cast is resolved so the coverage probe costs a
			// pointer rather than an RTTI walk. Only meaningful when castResult is null.
			const RE::NiRTTI* castRtti = nullptr;

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
			std::uint32_t candidateFrame = 0;  // 0: never classified
			Ineligible candidateReason = Ineligible::None;
			// An NPC face shape: a BSDynamicTriShape under a BSFaceGenNiNode, whose positions are FaceSnapshots'.
			// Resolved once, by the walk.
			bool faceShape = false;
			bool faceShapeResolved = false;
			// Its head (a key), while listed in faceHeads; and whether its last write found no snapshot of the head, so that the
			// head's next publication writes it again.
			const RE::BSFaceGenNiNode* faceHead = nullptr;
			bool faceWaiting = false;
			// Owned by an actor (its GetUserData is an ActorCharacter): Advanced Skin gives its draws the actor's
			// wetness (Tables::skinWetness). Resolved once, by the walk.
			bool actorOwned = false;
			bool actorOwnedResolved = false;
			// The accumulate phase's verdict when it left the object without bindings, and the frame it did so
			// (ReasonThisFrame).
			Ineligible accumulateReason = Ineligible::None;
			std::uint32_t accumulateReasonFrame = 0;
			std::uint32_t skinUpdatedFrame = 0;  // the frame the engine's palette update last ran for it (render thread)
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
			std::uint32_t hiddenEventFrame = 0;
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
		static void ResolveSunEntry(Tracked& a_tracked, const RE::BSGeometry& a_geometry);

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
		static bool SunEntryAllows(const Tracked& a_tracked, bool a_switchNodes);
		/**
		 * @brief Whether a tracked geometry lets its entry leave the primary's cull (PrimaryCull): a main-pass table
		 * object (a verdict of None) that is not a decal and not alpha-blended, whose main pass the synthetic pass
		 * reproduces. Anything the main pass draws natively, or that the cull decides per frame (hidden, fading, a
		 * switch child), keeps the entry in.
		 */
		static bool PrimaryEntryAllows(const Tracked& a_tracked, const RE::BSGeometry& a_geometry);
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
		const RE::NiAVObject* LightEntryOf(const Tracked& a_tracked, const RE::BSGeometry& a_geometry) const;
		/**
		 * @brief Whether a tracked geometry lets its light entry leave the point lights' culls. A table object: the exclusion
		 * decides (BuildSunExclusion). Any other must give the light's registration no pass: what the sun's rule allows
		 * (SunEntryAllows: hidden, alpha-blended, fading, an unselected switch child), or no Lighting property. The light's bit
		 * in its activeLightMask, which the registration writes on every geometry the cull reaches, is LocalLightCull's.
		 */
		static bool LightEntryAllows(const Tracked& a_tracked, const RE::BSGeometry& a_geometry, bool a_switchNodes);
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
		std::uint32_t lightCandidatesBuilt = 0;
		std::uint64_t lightEntriesAppeared = 0;
		ankerl::unordered_dense::set<const RE::NiAVObject*> sunCandidateSet;
		// Per sun candidate, a signature of its geometries' PrimaryEntryAllows verdicts: a change bumps the generation.
		ankerl::unordered_dense::map<const RE::NiAVObject*, std::uint64_t> primarySignature;
		std::vector<const RE::NiAVObject*> sunEntriesDirty;
		std::shared_ptr<const SunCandidates> sunCandidates;
		std::uint32_t sunCandidatesGeneration = 0;
		std::uint32_t sunCandidatesBuilt = 0;  // the generation the snapshot was built for

		void RefreshCategoryNodes(bool a_force = false);
		// The signature the category set was last rebuilt for.
		std::uint64_t categorySignature = 0;
		RE::NiNode* FindCategoryNode(RE::NiAVObject* a_object, Ineligible* a_parentReason) const;
		void AddSubtree(RE::NiAVObject* a_root);
		void AddGeometry(RE::BSGeometry* a_geometry, RE::NiNode* a_categoryNode, Ineligible a_parentReason);
		void ValidateSlice();
		void FindLightingShader();
		/** @brief The main camera's batch renderers, for the diagnostics' filter of the capture. Cheap; every frame. */
		bool RefreshMainBatchRenderers();
		/**
		 * @brief Drains the capture: the frame's lighting pass (below), and the diagnostics (registered eligible objects DCLF has
		 * not bound, the decal order probe).
		 */
		void DrainCapture();
		/**
		 * @brief A Lighting pass the main camera registered this frame with its light list (FindLightingPass's test), valid
		 * until the frame's accumulator is cleared. A member's property has no main-camera pass (the engine does not register
		 * it), so a pipeline's PerGeometry block is evaluated from this pass under its own descriptor: what differs per object
		 * is overridden per draw, and the rest is the frame's or the descriptor's.
		 */
		const RE::BSRenderPass* frameLightingPass = nullptr;
		/** @brief The pass a pipeline's template evaluates from: the property's own Lighting pass, else the frame's. */
		const RE::BSRenderPass* TemplatePassOf(RE::BSShaderProperty* a_property) const;
		/** @brief A cheap hash of everything RefreshCategoryNodes reads to find category nodes. */
		std::uint64_t CategorySignature() const;
		// a_accumulated: the frame's registered pass, whose captured fade state then stands in for the live one.
		Ineligible ClassifyFrame(const Tracked& a_tracked, const AccumulatedPass* a_accumulated = nullptr) const;
		/**
		 * @brief The nodes whose kHidden bit the engine flips while the asynchronous walk runs, with the bit the
		 * walk's views (the main camera and the sun) see. Taken on the render thread just before the walk is kicked,
		 * sorted by pointer; ClassifyFrame reads a listed node's bit from here instead of from the node.
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
		void CaptureCullHiddenBits();
		bool HiddenForWalk(const RE::NiAVObject* a_object) const;
		std::vector<std::pair<const RE::NiAVObject*, bool>> cullHiddenBits;

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

		Tables tables;
		// Step 6: the tables as the scene work left them, published as an immutable snapshot (PublishTables, the coordinator), and the
		// one the frame accepted (AcceptTables). Snapshots are pooled: one no frame holds any more is written again.
		std::vector<std::shared_ptr<Tables>> tablesPool;
		// Parallel to tablesPool: whether the snapshot equals the tables as they stood at its change log's end (6d: it is brought up
		// to date by replay); a snapshot just made is copied whole once.
		std::vector<std::uint8_t> tablesPoolReplayable;
		std::vector<std::uint8_t> replaySlotMarks;  // scratch: the object slots a replay copied
		// The accepted snapshot is the frame's alone (the pool writes no snapshot anyone holds): the frame's start writes the set into
		// it as into the tables (WriteBoth), so the two stay equal there.
		std::shared_ptr<Tables> publishedTables, acceptedTables;
		// Step 6c: the scene work in flight (kicked, not joined) and its lane's thread; a frame-side read of the coordinator's state
		// meanwhile is a defect, counted and named (GuardFrameAccess).
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
		// The frame's copies, taken at its start (HandOverAtFrameStart).
		std::shared_ptr<const SunCandidates> frameSunCandidates, frameLightCandidates;
		std::uint32_t frameSunGeneration = 0, frameLightGeneration = 0;
		std::uint64_t frameLightEntriesAppeared = 0;
		std::vector<const RE::NiAVObject*> frameSwitchChanges;
		bool frameSwitchResync = true;
		std::vector<std::uint32_t> frameRetiredMaterialSlots, frameRetiredPipelineSlots;
		std::vector<std::pair<ID3D11ShaderResourceView*, bool>> frameShadowTextureChanges;
		std::vector<std::shared_ptr<const void>> frameRetiredImports;
		// To the coordinator, at the next frame's start.
		std::optional<std::uint64_t> fadeRootsSentHeld;
		std::optional<std::vector<SceneStore::OwnedFadeRoot>> pendingFadeOwned;
		bool pendingFadeReseed = false;
		bool lookupsResetPending = false;
		// The frame's constant evaluations for the coordinator (step 6e A): posted by the render thread (PostPipelineConstants,
		// PostTechniqueConstants), handed over at the frame's start (HandOverAtFrameStart), applied by the scene work
		// (ApplyConstantsPosts) where the slot still holds what they were made for.
		struct PipelineConstantsPost
		{
			std::uint32_t slot = 0, binding = 0, generation = 0;
			PipelineKey key{};
			GeometryConstants constants{};
		};
		struct TechniqueConstantsPost
		{
			std::uint32_t row = 0, key = 0, generation = 0;
			TechniqueConstants value{};
		};
		std::vector<PipelineConstantsPost> pipelineConstantsPosted, pipelineConstantsInbox;
		// The frame's material records for the coordinator (step 6e B): a writer event's re-evaluation (the record), and the frame
		// floats of a slot that changed them (its frame record: the coordinator takes its frame part, CopyFrameComponents). Applied
		// where the slot still holds the key they were made for.
		struct MaterialPost
		{
			std::uint32_t slot = 0, generation = 0;
			bool frameFloats = false;  // the frame part alone
			std::pair<const RE::BSShaderMaterial*, std::uint32_t> key{};
			MaterialRecord record;
		};
		std::vector<MaterialPost> materialPosted, materialInbox;
		std::vector<std::uint32_t> frameFloatsDirty;      // render thread: slots whose frame floats changed this frame
		std::vector<std::uint8_t> frameFloatsDirtyMark;  // parallel to the frame's materials
		void PostMaterialRecord(std::uint32_t a_slot);
		void NoteFrameFloats(std::uint32_t a_slot);
		void PostFrameFloats();
		void ApplyMaterialPosts();
		std::vector<TechniqueConstantsPost> techniqueConstantsPosted, techniqueConstantsInbox;
		ConstantsPostStats constantsPostStats;
		void PostPipelineConstants(std::uint32_t a_slot);
		void PostTechniqueConstants(std::uint32_t a_row);
		void ApplyConstantsPosts();
		// The lookups as the coordinator's set judges readiness by them (MainReady, the readiness witness): copied at the frame's
		// start when they moved.
		Lookups lookupsView;
		std::shared_ptr<const Lookups> lookupsShared;
		std::array<std::uint64_t, 3> lookupsViewKey{ ~0ull, ~0ull, ~0ull };
		const Tables& FrameView() const { return acceptedTables ? *acceptedTables : tables; }
		/** @brief A write of the frame's start (the set applied, withdrawn or revoked): to the tables and to the frame's snapshot alike. */
		template <class F>
		void WriteBoth(F&& a_write)
		{
			a_write(tables);
			if (acceptedTables)
				a_write(*acceptedTables);
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
		/** @brief The property a slot's record draws: its geometry's, or for a layer slot the layer's (LayerPropertyOf). */
		RE::BSShaderProperty* SlotProperty(std::uint32_t a_slot) const;
		/** @brief Whether a member's binding still holds for its object as it is now (the derivation cache's witnesses). */
		bool MemberBindingStands(std::uint32_t a_slot, const RE::BSGeometry& a_geometry, const Tracked& a_entry) const;
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
			explicit operator bool() const { return material != nullptr; }

		private:
			RE::BSShaderMaterial* material = nullptr;
		};
		// Parallel to Tables::materials: the slot itself is the only record cache.
		// Its engine reference prevents pointer reuse while the slot is live. Last-object-
		// reference events and table resets release it at the safe capture boundary.
		std::vector<MaterialReference> materialOwners;
		/**
		 * @brief The frame-sourced components of every record drawn this frame (MaterialSources): one live
		 * evaluation per signature at Prepass - the engine globals, the character light's t11 - copied into every
		 * record of that signature (MaterialSources: not IBLParams, which no stage reads). At Prepass, not
		 * EarlyPrepass: they are the frame's lighting state, and sampling them earlier left them a fraction of a
		 * frame behind the native draws during a fast lighting transition. Nothing depth-only reads them.
		 */
		void RefreshFrameMaterials();
		/** @brief End of the accumulate phase: this frame's TexcoordOffset into every record drawn this frame. */
		void RefreshTextureTransforms();
		/**
		 * @brief End of the accumulate phase: the materials written since the last frame (MaterialSources).
		 * A slot drawn this frame is re-evaluated; any other slot of such a material is dropped, as is its
		 * cache entry, so its next use evaluates it afresh.
		 */
		void ProcessMaterialWrites();
		// The main renderers' registrations of the frame (DrainCapture), for the accumulate work's diagnostics (CheckRegistrations).
		struct CapturedRegistration
		{
			const RE::BSGeometry* geometry = nullptr;
			std::uint32_t hint = 0;
		};
		std::vector<CapturedRegistration> capturedRegistrations;
		void CheckRegistrations();
		/** @brief The coordinator's residency of a slot (IsMember is the frame's). */
		bool ResidentObject(std::int32_t a_object) const { return a_object >= 0 && IsResidentSlot(static_cast<std::uint32_t>(a_object)); }
		ankerl::unordered_dense::set<const RE::BSShaderMaterial*> writtenMaterials;
		// The materials written since the coordinator last looked (ProcessMaterialWrites, the render thread): their slots no object
		// references are dropped by the coordinator (DropWrittenMaterials), whose the references are. All of them after an overflow.
		std::vector<const RE::BSShaderMaterial*> materialWritesPosted;
		bool materialWritesAll = false;
		void DropWrittenMaterials();
		// The frame's material copies (FrameTables::materials): kept in step with the tables, each listed for the frame's work, and
		// held (the frame evaluates their materials on the render thread).
		void SyncFrameMaterials();
		void ListFrameMaterial(std::uint32_t a_slot);
		std::vector<MaterialReference> frameMaterialOwners;

	public:
		/** @brief The materials this frame's accumulate phase drained as written (diagnostics). */
		const ankerl::unordered_dense::set<const RE::BSShaderMaterial*>& GetWrittenMaterials() const { return writtenMaterials; }

	private:
		/** @brief The alarm: a record that disagrees with a live evaluation outside its frame-sourced components. */
		void NoteStaleMaterial(std::uint32_t a_slot, const std::pair<const RE::BSShaderMaterial*, std::uint32_t>& a_key, const MaterialRecord& a_served,
			const MaterialRecord& a_live);
		std::uint32_t materialValidationCursor = 0;
		static constexpr std::uint32_t kMaterialValidationsPerFrame = 8;
		static constexpr std::uint32_t kMaterialValidationStride = 4;
		std::uint32_t frame = 0;
		std::uint32_t tablesGeneration = 0;
		std::uint32_t materialVersions = 0;  // the last Tables::materialVersion handed out

		Lookups lookups;
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
		/** @brief One entry of the scene walk: writes its record at its slot; false when it gets none this frame. */
		bool WriteObject(RE::BSGeometry* a_geometry, Tracked& a_tracked, PartTimer& a_timer, Ineligible& a_bucket);
		/**
		 * @brief The entry's layer record, written after its base record (a_base): the base's placement, the second index list,
		 * no shadow, no bindings until it joins with the base (BindByMembership). a_member: the base is a main-pass record
		 * (verdict None); without it, or without a layer, the layer slot is released. a_keepMember: the base kept its binding.
		 */
		void WriteLayer(RE::BSGeometry* a_geometry, Tracked& a_tracked, std::uint32_t a_base, bool a_member, bool a_keepMember, PartTimer& a_timer);
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
		static std::uint32_t PerFrameTraits(const Tracked& a_tracked, const RE::BSGeometry& a_geometry);
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
		static std::uint64_t ShadingInputsOf(const Tracked& a_tracked, const RE::BSGeometry& a_geometry);
		/**
		/**
		 * @brief Whether a kept record's skin stays as written: skinning on, its partitions drawn, its palette the
		 * record's size. Notes a partition mask that changed; the palette itself is the scene placement job's.
		 */
		bool KeepSkin(RE::BSGeometry* a_geometry, Tracked& a_tracked);
		/** @brief A kept record of a moving static: its transforms, its bound and its sun entry, nothing else. */
		/**
		 * @brief Whether an actor's record, written in an earlier frame, is what WriteObject would write again but for
		 * its placement and palette: its classification inputs (ClassifyInputsOf) and its verdict are unchanged.
		 */
		bool ActorRecordKept(RE::BSGeometry& a_geometry, Tracked& a_tracked);
		/** @brief A face shape's snapshot and region this walk (FaceSnapshots); false without one. */
		bool ResolveFace(RE::BSGeometry& a_geometry, FaceSnapshots::ShapeView& a_face, std::uint32_t& a_region);
		/** @brief Appends a face shape's entry to this walk's faceStreams and points its slot at it. */
		void PushFaceStream(RE::BSGeometry& a_geometry, std::uint32_t a_slot, const FaceSnapshots::ShapeView& a_face, std::uint32_t a_region);
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
		bool MovedRecently(const void* a_key) const
		{
			const auto it = movedFrame.find(a_key);
			return it != movedFrame.end() && frame - it->second <= 1;
		}
		static const void* MoveKeyOf(const RE::BSGeometry& a_geometry, const RE::NiNode* a_categoryNode);
		/**
		 * @brief Whether a reference root's subtree holds anything that moves (a controller, a non-fixed rigid body, a
		 * skin): the root's bound is then not fixed, and it is the sun entry of every geometry under it. Walked once per
		 * root, and again after an event under it (ScheduleRoot).
		 */
		bool RootMoves(const RE::NiAVObject* a_root);
		static bool RootMovesNow(const RE::NiAVObject* a_root);
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
		/** @brief A switch event on a tree root's LOD switch: the root's kFadeRootTreeLod again (ApplySwitchEvents). */
		void RefreshFadeRootSwitch(const RE::NiAVObject* a_switch);
		void CheckResidentParity();
		/** @brief What a classification reads from the geometry, its properties and its material, hashed. */
		static std::uint64_t ClassifyInputsOf(const RE::BSGeometry& a_geometry);
		/** @brief CS_DCLF_INPUT_WATCH: what ClassifyInputsOf and ShadingInputsOf hash, by component (kInputComponentNames). */
		static std::array<std::uint64_t, 12> InputComponentsOf(const RE::BSGeometry& a_geometry);
		/** @brief CS_DCLF_INPUT_WATCH: a re-read of kind a_kind (0 classify, 1 shading) that found the hash changed or not. */
		void NoteInputReread(Tracked& a_tracked, const RE::BSGeometry& a_geometry, std::uint32_t a_kind, bool a_changed);
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
		 * @brief CS_DCLF_CHANGE_LOG_PARITY=1: every 60 frames each slot's columns are kept, and a frame later every slot
		 * whose columns changed in between must be in the log for that frame with the causes that changed. At the scene
		 * phase's start, before anything of the frame writes.
		 */
		void CheckChangeLog();
		// The material frame components and texture transforms (RefreshFrameMaterials, RefreshTextureTransforms).
		struct MaterialFrameStats
		{
			std::uint64_t frames = 0, samples = 0, applications = 0, slotsApplied = 0, pending = 0, transformsWatched = 0, checks = 0, slotsChecked = 0,
						  componentsDiffer = 0, transformsDiffer = 0;
			std::string first;
		} materialFrameStats;
		void CheckMaterialFrame();
		// The pipelines' PerGeometry blocks (RefreshFrameConstants): full evaluations, frame samples, and the parity's findings.
		struct GeometryStats
		{
			std::uint64_t frames = 0, full = 0, samples = 0, changed = 0, checks = 0, pipelinesChecked = 0, techniquesChecked = 0, techniquesDiffer = 0,
						  lightingVersions = 0, lightingChecked = 0, lightingDiffer = 0;
			std::string lightingFirst;
			std::array<std::array<std::uint64_t, 64>, 2> differ{};
			std::string first;
		} geometryStats;
		/** @brief The technique row of a pass descriptor's TechniqueKey (Tables::techniqueKeys), made when new (the frame evaluates it). */
		std::uint32_t TechniqueRowFor(std::uint32_t a_passDescriptor);
		void CheckFrameGeometry(std::uint32_t a_pipeline, const GeometryConstants& a_reference, const GeometryConstants& a_held);
		// The first RefreshFrameConstants evaluates every pipeline in full and resamples every slot's shading. The full
		// evaluations are what seed the frame lighting (FrameCapture::lighting) with the variables the per-frame sample's
		// pipeline does not write, such as AmbientSpecularTintAndFresnelPower: pipelines made later are evaluated where
		// they are written (WriteObject), which does not publish lighting.
		bool constantsRefreshed = false;
		// The new pipelines' full evaluations since the last RefreshFrameConstants (RefreshNewPipelineConstants): merged into the
		// frame lighting where nothing else wrote it, so a component only a later pipeline writes is published too.
		std::vector<ConstantBlock> lightingSeeds;
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
		std::uint32_t lodParityFrame = 0;
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
		void EvaluateRound(PartTimer& a_timer, std::size_t a_first);
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
		std::uint32_t validatedFrame = ~0u;
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
		// Counts the fade roots that became owned or stopped being: an occluder's readiness reads it (IndirectDraws::PhaseReady).
		std::uint64_t fadeOwnershipSerial = 0;
		/**
		 * @brief The root's row owned or not; a root newly owned is seeded again from its node (a new generation). One whose
		 * stood-in state changes is reported to the fade watch: its dependents' shadow verdicts read its fade from elsewhere.
		 */
		void MarkFadeRootOwned(const void* a_node, bool a_owned, bool a_standIn);
		ankerl::unordered_dense::map<const RE::BSFadeNode*, std::vector<RE::BSGeometry*>> fadeDependents;
		// The structural events (SceneEvents in SceneStore.cpp), applied at ApplyEvents: properties by key, nodes held.
		std::vector<const void*> propertyChanged;
		std::vector<RE::NiPointer<RE::NiAVObject>> nodeChanged;
		// Ingestion (IngestEvents): the queues' events since the last apply, oldest first (Internal.h).
		struct EventBatch;
		std::shared_ptr<EventBatch> ingested;
		// The references the scene work let go of, and its applied batches (their tracker events hold subtrees): released at
		// Present (ReleaseHandedBack). Written by the scene work, cleared by the render thread with it joined.
		std::vector<RE::NiPointer<RE::NiRefObject>> handedBack;
		// The tree and fade-root nodes the tables list (Tables::treeNode, fadeRootNode), owned while listed: unlisted, they go
		// through the retirement chain (a kept publication still walks them).
		std::vector<RE::NiPointer<RE::NiAVObject>> treeOwners, fadeRootOwners;
		std::vector<std::shared_ptr<EventBatch>> spentBatches;
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
		};
		// What each resident slot was counted under (kNone: not counted).
		struct ResidentCounted
		{
			static constexpr std::uint32_t kNone = ~0u;
			std::uint32_t pipeline = kNone, material = kNone, fadeRoot = kNone;
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
		std::vector<std::uint32_t> setLagged, setLaggedNext;
		std::vector<std::uint8_t> setQueueMark;    // parallel to objects: in setQueue
		std::vector<std::uint8_t> setRebinding;    // parallel to objects: this commit's, its binding is taken again this frame
		std::vector<std::uint8_t> setLacking;      // parallel to objects: the occluder phases it takes part in and is no member of
		std::array<std::uint32_t, 2> setLackingCount{};  // SetLacking's, per occlusion map
		// The last commit's decision, applied at the next ApplySet: each slot's phases and lacking phases, and the slots it changed
		// with the geometry each held when the commit decided (a slot that holds another one at ApplySet takes nothing).
		std::vector<std::uint8_t> setPhasesNext, setLackingNext;
		std::vector<std::pair<std::uint32_t, const RE::BSGeometry*>> setApply;
		// Parallel to objects: the geometry the last commit that decided a slot decided for (what a whole application after a
		// withdrawal applies its setPhasesNext to); and whether the claims are withdrawn (WithdrawSet) until the next ApplySet.
		std::vector<const RE::BSGeometry*> setGeometryNext;
		bool setWithdrawn = false;  // the frame's claims (WithdrawSet), until the next installation
		/**
		 * @brief A publication of the scene (step 6e E3): the tables the coordinator published with its set applied, the claims (the
		 * set's snapshot less what the revocation took back), the main claims' changes against the publication before it, the lacking
		 * counts, and the commit it applies. Made by PublishScene, installed whole by the frame's start (SelectPublication).
		 */
		struct ScenePublication
		{
			std::uint64_t sequence = 0;
			std::uint32_t commitFrame = 0;
			std::shared_ptr<Tables> tables;
			std::shared_ptr<const SetSnapshot> claims;
			std::vector<const RE::BSGeometry*> joined, left;
			std::array<std::uint32_t, 2> lackingCount{};
			// What the frames that install it draw (IndirectDraws::BuildAhead, step 6e E3b): its stream views and main payloads.
			std::shared_ptr<const void> draws;
		};
		/** @brief The coordinator: ApplySet, RevokeUndrawnClaims, PublishTables, and the publication queued. */
		void PublishScene();
		std::deque<std::shared_ptr<ScenePublication>> publications;  // made, not installed (oldest first)
		std::shared_ptr<ScenePublication> installed;                  // the frame's
		std::uint64_t publicationSequence = 0;
		std::uint32_t publicationCommitFrame = 0;  // the commit the next publication applies
		std::shared_ptr<const SetSnapshot> publicationClaims;
		std::vector<const RE::BSGeometry*> publicationJoined, publicationLeft;
		// The frame's: the installed publications' claim changes not told to PrimaryCull yet, whether one is waiting, the claims it was
		// told of, and the lacking counts.
		std::vector<std::pair<std::vector<const RE::BSGeometry*>, std::vector<const RE::BSGeometry*>>> installNotes;
		bool installPending = false;
		std::shared_ptr<const SetSnapshot> notedClaims;
		std::array<std::uint32_t, 2> frameLackingCount{};
		PublicationStats publicationStats;
		std::vector<std::uint32_t> setApplyMark;  // parallel to objects: its index in setApply plus one, 0 when not in it
		std::uint32_t setCommitFrame = 0;
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
		/** @brief Render thread, after the scene work (the task's or inline): the held hand-overs, the revocations, the kicks. */
		void FinishSceneWork();
		/** @brief Render thread, after the accumulate work: its dropped members to PrimaryCull, its new pipelines evaluated. */
		void FinishAccumulateWork();
		/** @brief Render thread, the coordinator idle: the joins' material evaluations (SetupMaterial), for the next joins. */
		void ServeMaterialRequests();
		bool accumulateWorkPending = false;  // RunAccumulateWork ran; FinishAccumulateWork has not
		bool holdLostMembers = false;        // RunAccumulateWork: the members it drops are held for PrimaryCull (lostMembersHeld)
		std::vector<const RE::BSGeometry*> lostMembersHeld;
		// A join whose material record the engine has not evaluated yet asks for it and waits a frame (bindRetry). Requests
		// hold the material; a served record is taken by the next join (its reference moves into the slot's owner).
		struct MaterialRequest
		{
			MaterialReference owner;
			const RE::BSShaderMaterial* material = nullptr;
			std::uint32_t pass = 0;
		};
		struct MaterialServed
		{
			MaterialReference owner;
			MaterialRecord record;
			bool valid = false;
			std::uint32_t frame = 0;
		};
		std::vector<MaterialRequest> materialRequests;
		ankerl::unordered_dense::set<std::pair<const RE::BSShaderMaterial*, std::uint32_t>> materialRequested;
		ankerl::unordered_dense::map<std::pair<const RE::BSShaderMaterial*, std::uint32_t>, MaterialServed> materialsServed;
		std::vector<MaterialReference> materialsHandedBack;  // released at Present (ReleaseHandedBack)
		/**
		 * @brief What the coordinator let go of that a published version of the tables may still name (step 6e E3): the slots
		 * (Tables::retiring, the SlotTables'), the geometry slots' buffer owners, the material slots' engine references and every
		 * engine reference handed back. Each publication holds the chain's node opened with it (Tables::retirementHold); a batch comes
		 * back once no publication up to it lives, and RecycleRetired puts its slots on their free lists and its references where
		 * they were released before (retiredImports, materialsHandedBack, handedBack).
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

		// The scene task (KickSceneTask): its job, and what the work holds for the render thread while it runs.
		std::shared_ptr<void> sceneTask;  // AsyncWorker::JobHandle
		bool sceneTaskFailedLogged = false;
		bool sceneWorkPending = false;  // RunSceneWork ran; FinishSceneWork has not
		bool inSceneTask = false;       // RunSceneWork on the coordinator
		bool holdPrimaryNotes = false;  // RunSceneWork: PrimaryCull's notes are held until FinishSceneWork
		std::vector<const void*> hiddenKeysHeld;
		bool allMembersLostHeld = false;
		std::uint32_t publishedSunGeneration = 0;
		std::vector<const RE::BSGeometry*> setGeometry;  // parallel to objects: the geometry a base member is published under
		ankerl::unordered_dense::map<const RE::BSGeometry*, std::uint32_t> setMemberSlot;  // published geometry -> its slot
		// Members whose registration met a fade DCLF does not model (PassCapture::TakeUnmodelledFades): out of the set until it ends.
		ankerl::unordered_dense::set<const RE::BSGeometry*> setFadeHeld;
		LogCursor setCursor;
		std::uint64_t setReadiness = ~0ull;        // the lookups' generations the waiting slots were last checked against
		std::uint64_t setShadowModes = ~0ull;      // the shadow modes and states caster readiness was last taken under
		std::uint64_t setFadeOwnership = ~0ull;    // fadeOwnershipSerial as occluder readiness was last taken under
		std::uint32_t setPhaseMask = ~0u;          // the phases DCLF draws (toggles) the last commit evaluated with
		std::shared_ptr<SetSnapshot> setBuilding;  // the next publication, kept up to date by each commit
		std::shared_ptr<const SetSnapshot> setSnapshot;
		bool setSnapshotDirty = true;
		SetStats setStats;
		/** @brief Queues a slot for the next commit. */
		void QueueSet(std::uint32_t a_slot);
		/** @brief The phases DCLF draws this frame (toggles, the render graph): an object's mask is its participation within them. */
		static std::uint32_t SetPhasesDrawn();
		/** @brief The phases an object takes part in among a_drawn, from its record (no readiness). */
		std::uint8_t SetParticipation(std::uint32_t a_slot, std::uint32_t a_drawn) const;
		/** @brief Whether everything the object's main phase draws with is ready; a_why: SetStats::waitingBy's index when not. */
		bool MainReady(std::uint32_t a_slot, std::uint32_t& a_why) const;
		// Sun entry nodes something was attached under or detached from since the last walk (keys).
		std::vector<const RE::NiAVObject*> dirtyRoots;
		ankerl::unordered_dense::map<const void*, std::vector<RE::BSGeometry*>> propertyDependents;
		ankerl::unordered_dense::map<const RE::NiAVObject*, std::vector<RE::BSGeometry*>> rootDependents;
		// A listed reference root's reference, and back (the root is a key once its last dependent leaves: it may be gone).
		ankerl::unordered_dense::map<const RE::NiAVObject*, const void*> rootReference;
		ankerl::unordered_dense::map<const void*, const RE::NiAVObject*> referenceRoot;
		std::array<std::uint32_t, static_cast<std::size_t>(Ineligible::Count)> buckets{};
		struct DeltaStats
		{
			std::uint32_t walks = 0, full = 0;
			std::uint64_t evaluated = 0, perFrame = 0, pending = 0, property = 0, node = 0, roots = 0, fade = 0, geometryDirty = 0, settling = 0, restored = 0, moved = 0, kept = 0;
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
		std::uint32_t ResolveLayerGeometrySlot(RE::BSGeometry& a_geometry, PartTimer& a_timer);
		std::uint32_t ResolveGeometrySlot(RE::BSGeometry& a_geometry, const RE::BSGraphics::TriShape* a_triShape,
			const RE::NiSkinPartition::Partition* a_skinPartition, PartTimer& a_timer);
		/**
		 * @brief Object LOD (dclf-lod.md): the geometry slots of a partly hidden shape's visible ranges, linked by nextPartition from
		 * the returned first (Tables::kSlotFree when one cannot be resolved), each keyed by its range's first segment record.
		 */
		std::uint32_t ResolveLodRangeSlots(RE::BSGeometry& a_geometry, const std::vector<LodSegments::Range>& a_ranges, PartTimer& a_timer);
		/** @brief Capture a new material slot; false when nothing can be evaluated. */
		bool EvaluateMaterialForSlot(const RE::BSShaderMaterial* a_material, std::uint32_t a_pass, MaterialRecord& a_record);

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
		 * @brief Drops every cached classification verdict and derivation: a live toggle that enters the
		 * classification (Toggles.h) changed, and the caches witness the object rather than the switches.
		 */
		void InvalidateVerdicts();

	private:

		/** @brief After the loop: every object with bindings names live slots of this frame, or is neutralised. */
		void CheckObjectSlots(bool a_resolveBuffers);
		/**
		 * @brief CS_DCLF_SLOT_PROBE: re-derives what the used slots serve (a fresh material evaluation, a
		 * fresh buffer resolve) and logs the first difference of the frame. Startup diagnostics for the
		 * persistent tables; every frame, so it is a probe and not a mode.
		 */
		void ProbeSlots(bool a_resolveBuffers);
		/** @brief Re-evaluates a few live materials a frame against what their slots serve. */
		void ValidateMaterialSlice();
		Stats stats;
	};
}
