#pragma once

#include <array>
#include <bit>
#include <cstdint>
#include <map>
#include <memory>
#include <utility>
#include <vector>

#include <ankerl/unordered_dense.h>

#include "Features/DrawcallLimitFix/Runtime/KeptState.h"
#include "Features/DrawcallLimitFix/Runtime/SlotTable.h"
#include "Features/DrawcallLimitFix/Engine/Capture/FaceSnapshots.h"
#include "Features/DrawcallLimitFix/Scene/Model/ActorValueIndex.h"
#include "Features/DrawcallLimitFix/Scene/Ports/ConstantEvaluator.h"
#include "Features/DrawcallLimitFix/Scene/Ports/FrameGlobals.h"
#include "Features/DrawcallLimitFix/Scene/Model/Records.h"

namespace DCLF
{
	inline constexpr std::uint32_t kNoObjectSlot = ~0u;

	/** @brief What a change-log entry changed (SceneTables::changeLog), by the columns its consumers read. */
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
	 * @brief The scene's tables: the object, geometry, pipeline and material records and every column parallel to them. The coordinator
	 * writes its own copy; a publication carries an immutable copy (ScenePublication::tables), which is what the frame and the draws read.
	 */
	struct SceneTables
	{
		std::vector<ObjectRecord> objects;
		std::vector<RE::BSGeometry*> objectGeometry;  // parallel to objects
		std::vector<GeometryBuffers> geometries;
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
		/** @brief The tree family's members of two tables, each pair to a_member (the publication's copy, its checks). */
		template <class T, class F>
		static void TreeFamily(T& a_left, T& a_right, F&& a_member)
		{
			a_member(a_left.trees, a_right.trees);
			a_member(a_left.treeRefs, a_right.treeRefs);
			a_member(a_left.treeFree, a_right.treeFree);
			a_member(a_left.treeNode, a_right.treeNode);
			a_member(a_left.treeIndex, a_right.treeIndex);
			a_member(a_left.treeObjects, a_right.treeObjects);
			a_member(a_left.objectTree, a_right.objectTree);
		}
		/** @brief The fade-root family's members of two tables, each pair to a_member. */
		template <class T, class F>
		static void FadeRootFamily(T& a_left, T& a_right, F&& a_member)
		{
			a_member(a_left.fadeRoots, a_right.fadeRoots);
			a_member(a_left.fadeRootRefs, a_right.fadeRootRefs);
			a_member(a_left.fadeRootFree, a_right.fadeRootFree);
			a_member(a_left.fadeRootNode, a_right.fadeRootNode);
			a_member(a_left.fadeRootIndex, a_right.fadeRootIndex);
			a_member(a_left.fadeRootSwitch, a_right.fadeRootSwitch);
			a_member(a_left.objectFadeRoot, a_right.objectFadeRoot);
		}
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
		// T7e: freed blocks of any other size (a particle member's quads: ParticleSnapshots::RowsOf), by their rows.
		std::map<std::uint32_t, std::vector<std::uint32_t>> boneFreeOther;
		/** @brief The free list of a_rows-row blocks: a skin's palette by its bones, any other block by its rows. */
		std::vector<std::uint32_t>& BoneFreeList(std::uint32_t a_rows)
		{
			if (a_rows % 3 == 0 && a_rows / 3 < boneFree.size())
				return boneFree[a_rows / 3];
			return boneFreeOther[a_rows];
		}
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
		// Coordinator::ResolveSunEntry), null when the entry is never tested (an actor's, whose entry is its cell's container).
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
		// publication updates its streams in place (Coordinator::ApplyFacePublications); a free entry has kNoFaceObject.
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
		// The (shape, region) of every stream freed since the walk's end took them (Coordinator::EndFaceWalk frees the region
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
		// bindings (Coordinator::UpdateSharedBindings): false lets the binding go, true asks for it.
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
		 * (Coordinator::UpdateSlotReferences counts the references from the logs), and the drain runs before the loop,
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
		// A layer's geometry slot (Coordinator::ResolveLayerGeometrySlot): its key is the shape's second index list (a
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
		// A geometry's main-pass layer (Coordinator::Tracked::layerSlot: a multi-index shape's additional property, drawn
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
		 * (Coordinator::retirement), which gives it back once every publication up to then is gone (Coordinator::RecycleRetired).
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
			kRetiredMaterialLookup,  // a material slot's bindings to drop (Coordinator::RetireMaterialBinding)
			kRetiredPipelineLookup,  // a pipeline slot's lookups to drop (its mask binding: Coordinator::RetireMaskBinding)
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
}
