#pragma once

// SceneTables: the scene's object, geometry, pipeline and material slots and every column parallel to them, with the change and
// write logs the persistent consumers read and the retirement list for freed slots. The coordinator owns and writes the live copy
// on the scene-work thread; a publication carries an immutable copy that the render thread and the builds read.

#include <array>
#include <atomic>
#include <bit>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <utility>
#include <vector>

#include <ankerl/unordered_dense.h>

#include "Features/DrawcallLimitFix/Api/MainRanges.h"
#include "Features/DrawcallLimitFix/Api/VertexStreams.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ShadingFields.h"
#include "Features/DrawcallLimitFix/Core/Extensions/TableFamilies.h"
#include "Features/DrawcallLimitFix/Core/Runtime/KeptState.h"
#include "Features/DrawcallLimitFix/Core/Runtime/SlotTable.h"
#include "Features/DrawcallLimitFix/Core/Shading/ConstantEvaluator.h"
#include "Features/DrawcallLimitFix/Core/Shading/FrameGlobals.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Records.h"

namespace DCLF
{
	inline constexpr std::uint32_t kNoObjectSlot = ~0u;

	/** @brief What a change-log entry changed (SceneTables::changeLog), by the columns its consumers read. */
	enum ChangeCause : std::uint32_t
	{
		kChangeBindings = 1u << 0,    // flags, material, pipeline (the draw's too), the fade distance, whether it has a fade node
		kChangeAncestorValue = 1u << 1,  // the ancestor value (Integrations/LightLimitFix's room index)
		kChangeRetired2 = 1u << 2,    // retired (was the record's tree animation, never written); noted only within kChangeAll
		kChangeSkin = 1u << 3,        // skin partitions, palette block (offset and rows)
		kChangeExtras = 1u << 4,      // the extras block and its rows
		kChangeShadow = 1u << 5,      // shadow technique and reason, sky technique, shadow material and diffuse
		kChangeGeometry = 1u << 6,    // geometry slot, the draw's geometry half, face stream, the object's geometry
		kChangeMembership = 1u << 7,  // residency
		// Noted beside another cause when a scene revision's shapes change: pipelines (buckets and capacities), the geometry half
		// and partitions (the draws), shadow and occlusion techniques and rejection (the shadow keys).
		kChangeStructure = 1u << 8,
	};
	// No placement, palette or shading: those are FrameValues', never a column.
	inline constexpr std::uint32_t kChangeCauseCount = 9;
	// What a shadow input carries (IndirectDraws' shadow build). Not placements: a caster's bound and sun entry are its record's.
	inline constexpr std::uint32_t kShadowChangeCauses = kChangeShadow | kChangeBindings | kChangeGeometry | kChangeSkin | kChangeMembership;
	// The retired bit keeps its name: kChangeAll notes still count it, and the reports print it.
	inline constexpr std::array<const char*, kChangeCauseCount> kChangeCauseNames{ "bindings", "lights", "tree", "skin", "extras", "shadow", "geometry",
		"membership", "structure" };
	inline constexpr std::uint32_t kChangeAll = (1u << kChangeCauseCount) - 1;
	// What revokes an earlier claim. Not residency (the accumulate phase makes every joiner resident), flags, materials or the
	// fade distance: the revision's shapes do not size by them.
	inline constexpr std::uint32_t kStructureCauses = kChangeStructure;

	/** @brief The scene phase's flags the accumulate phase's patch keeps; the rest of an object's flags are the patch's. */
	inline constexpr std::uint32_t kSceneKeptFlags = kObjectPaletteBlock | kObjectNoShadow | kObjectVolumetricOnly | kObjectShadowOnly;

	/**
	 * @brief The scene's tables: the object, geometry, pipeline and material records and every column parallel to them. The
	 * coordinator writes its own copy; the frame and the draws read a publication's immutable copy (ScenePublication::tables).
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
		// Parallel to materials: session-unique, new whenever the record is (re)evaluated but not when only its frame floats are
		// written. Consumers compare this instead of the record's 2.3 KB.
		std::vector<std::uint32_t> materialVersion;
		// Parallel to materials: new whenever only the record's frame floats are written (RefreshMaterialSignatures,
		// RefreshMaterialTransforms).
		std::vector<std::uint32_t> materialFrameVersion;
		// What MainBuild's kept bindings key on, new (NextVersion) whenever a covered value changes: a pipeline's PerGeometry
		// floats and what its pairs' records read of it (key, permutation; the technique's are its TechniqueRow's).
		std::vector<std::uint32_t> pipelineBindingVersion;    // parallel to pipelines
		// Write stamp of the pipeline family (PipelineFamily), bumped by every write of it (sizes included): the publication copies
		// the family only when it moved. Pipeline slots are written right after their allocation (AllocatePipelineSlot notes it).
		std::uint64_t pipelinesStamp = 0;
		void NotePipelinesWrite() { ++pipelinesStamp; }
		/** @brief The pipeline family's members of two tables, each pair to a_member (the publication's copy, its checks). */
		template <class T, class F>
		static void PipelineFamily(T& a_left, T& a_right, F&& a_member)
		{
			a_member(a_left.pipelines, a_right.pipelines);
			a_member(a_left.pipelineBindingVersion, a_right.pipelineBindingVersion);
			a_member(a_left.pipelineTechnique, a_right.pipelineTechnique);
			a_member(a_left.permutations, a_right.permutations);
		}
		std::uint32_t versionCounter = 0;
		std::uint32_t NextVersion() { return ++versionCounter; }
		// No shading (MaterialData, EmitColor, SSRParams.w, emissive multiplier, wetness): FrameValues' rows (BindlessShading),
		// sampled at frame start for the slots the walk names (ShadingItem).
		std::vector<ObjectAncestorValue> ancestorValue;       // parallel to objects
		// The features' table families (Api::TableFamily; Features/TreeWind's trees, the fade roots: Features/Fades/FadeRoots.h): their
		// columns, stamps, the objects' references into them and their rows' tags. Published with the rest: a family is copied only when its
		// stamp moved.
		Extensions::TableFamilies families;
		// The published entry classes' members (Api::EntryClassContribution::published: Features/Actors' actor-owned), by class: the slots
		// whose shading fields are their owner's (Api/ShadingFields.h: Integrations/AdvancedSkin's SkinPerGeometry, the owning actor's
		// wetness); zero for every other slot. Values are FrameValues' (the fields' captures); these say who has one.
		struct ClassMembers
		{
			std::vector<std::uint32_t> objects;  // sorted by object index (the walk sorts it)
			// By slot: its owner's group (0: not a member) and its identity (incarnations included). Membership only: the render thread's
			// fields keep their own from the changes.
			struct Owner
			{
				std::uint64_t group = 0, identity = 0;
			};
			std::vector<Owner> owners;
			// The membership writes in order, moved onto the publication log for the render thread's fields (PublicationDeltas::classMembers).
			std::vector<Api::ClassMemberChange> changes;
			/** @brief Whether the slot is a member (in an owner's group). */
			bool Grouped(std::uint32_t a_slot) const { return a_slot < owners.size() && owners[a_slot].group; }
		};
		// Write only through SetClassMembers / ClearClassMembers (the objects: the walk's).
		std::array<ClassMembers, Extensions::kPublishedEntryClasses> classMembers;
		/** @brief a_slot's membership of every published class: a_class's in a_group (0 or None: no class's), every other's none. */
		void SetClassMembers(std::uint32_t a_slot, Api::EntryClass a_class, std::uint64_t a_group, std::uint64_t a_identity, bool a_resetValue = false)
		{
			for (const auto c : Extensions::PublishedEntryClasses()) {
				auto& members = classMembers[static_cast<std::size_t>(c)];
				const std::uint64_t group = c == a_class ? a_group : 0;
				if (members.owners.size() <= a_slot)
					members.owners.resize(std::size_t(a_slot) + 1);
				members.owners[a_slot] = { group, a_identity };
				members.changes.push_back({ a_slot, group, a_identity, a_resetValue, false });
			}
		}
		void ClearClassMembers()
		{
			for (const auto c : Extensions::PublishedEntryClasses()) {
				auto& members = classMembers[static_cast<std::size_t>(c)];
				members.owners.clear();
				members.changes.clear();
				members.changes.push_back({ 0, 0, 0, false, true });
			}
		}
		/** @brief Whether a published class has membership writes not yet moved onto the log. */
		bool ClassMemberChangesPending() const
		{
			for (const auto& members : classMembers)
				if (!members.changes.empty())
					return true;
			return false;
		}
		/** @brief The shading fields a_slot owns (a bit by Api::ShadingRowField): each whose members' class has it as a member. */
		std::uint8_t ShadingFieldsOwned(std::uint32_t a_slot) const
		{
			std::uint8_t owned = 0;
			for (const auto& entry : Extensions::ShadingFields())
				if (classMembers[static_cast<std::size_t>(entry.contribution.members)].Grouped(a_slot))
					owned |= static_cast<std::uint8_t>(1u << static_cast<std::uint32_t>(entry.contribution.field));
			return owned;
		}
		// Multi-partition skins (CS_DCLF_SKIN_PARTITIONS): bit i draws partition i along nextPartition from geometryIndex; 0 for
		// one geometry. A LOD skin's (kMeshLOD under a fade node) is the cumulative superset (row 3), narrowed by the draw.
		std::vector<std::uint16_t> skinPartitions;            // parallel to objects
		// A LOD skin's partitions per level (byte L: level L's row of the engine's table). BuildDraws picks by FadeStateCS's level,
		// never the node's (+0x152, which GetRenderPasses reads), so the record is written once.
		std::vector<std::uint32_t> skinLodPartitions;         // parallel to objects
		// The property that supplied each pipeline's per-frame constants, for RefreshFrameConstants to re-evaluate. (Frame globals
		// are FrameCapture's.) The coordinator's alone: not published (a publication's is empty).
		std::vector<RE::BSShaderProperty*> geometryTemplate;  // parallel to pipelines
		// PerTechnique values (plus filter modes and shadow mask), one row per TechniqueKey, shared by its pipelines. Evaluated
		// when made (TechniqueRowFor) and when TechniqueInputs move (RefreshTechniqueRows). Never freed: a few dozen.
		std::vector<std::uint32_t> techniqueKeys;
		ankerl::unordered_dense::map<std::uint32_t, std::uint32_t> techniqueRow;  // TechniqueKey -> row
		std::vector<std::uint32_t> pipelineTechnique;  // parallel to pipelines: its row
		// The constants packed into pipeline rows: the render thread's pipeline blocks (GeometryPort), applied by the coordinator
		// (ApplyConstantsPosts), and the technique rows it evaluates from FrameGlobals. A change draws with the next publication.
		// Without them a pipeline is not drawable (MainReady, MainBuild::PackPipelines).
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
		// it moved; the set's readiness reads it.
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
		/** @brief Whether the pipeline's technique binds the shadow mask (TechniqueKey's low bit). */
		bool TechniqueShadowMask(std::size_t a_pipeline) const { return (techniqueKeys[pipelineTechnique[a_pipeline]] & 1u) != 0; }
		std::vector<PipelinePermutation> permutations;        // parallel to pipelines
		std::vector<DrawSequence> draws;  // one per object (templates: pipelineIndex is the table index)
		// The fixed main ranges (Api/MainRanges.h): each object's slot in its range of the colour segment's second pass, in the order the
		// range's feature gives (Features/Decals: the decals', in the engine's draw order - group, technique bucket, batch list, chain
		// position), and each range's slots. The colour epoch writes the draw, or a zero-count one, to that fixed slot so overlapping
		// objects keep their order (an atomic append would not). ~0u for objects in none. Every changed ordinal notes its slot.
		std::vector<std::uint32_t> rangeOrdinal;  // parallel to objects (sized by the writer: Features/Decals' OrderDecals)
		std::array<std::uint32_t, Api::kFixedMainRanges> rangeCount{};
		// Skinning (CS_DCLF_SKINNED): each skinned object's block in FrameValues' palettes (NiSkinInstance::boneMatrices and
		// prevBoneMatrices, three float4 rows a bone) and its row count. A block stays the slot's while the palette size holds
		// (PlaceBones / FreeBones); capacity grows in kBoneGrowRows steps, moving no block.
		static constexpr std::uint32_t kBoneGrowRows = 4096;
		std::vector<std::uint32_t> boneOffset;  // parallel to objects, in rows
		std::vector<std::uint32_t> boneRows;    // parallel to objects
		std::array<std::vector<std::uint32_t>, 81> boneFree{};  // freed blocks' offsets, by bones (rows / 3; at most 80)
		// Freed blocks of any other size (a palette rows class's block: a particle member's quads, by its rule), by their rows.
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
		// Per-object extras (kExtraRows float4 rows; kObjectLandBlend / kObjectProjectedUV), filled at Prepass by
		// RefreshFrameConstants and sent to the extras buffer (t126). A block persists while the object's patch holds it.
		std::vector<float> extraRows;
		std::vector<std::uint32_t> extraOffset;  // parallel to objects
		std::vector<std::uint32_t> extraFree;    // freed blocks' row offsets
		// Every block written, allocated or freed (row offset), for the published tables to replay.
		SharedEventLog<std::uint32_t> extrasBlockLog;
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
		// The Utility technique each object casts with, without a view's mode bits (ShadowViews.h: ShadowUtilityTechnique), and
		// why the engine would not draw it into a shadow map. Decided by the scene phase, since every shadow view is drawn before
		// the accumulate phase. 0 and ShadowReject::NotLighting for a non-caster.
		std::vector<std::uint32_t> shadowTechnique;  // parallel to objects
		std::vector<std::uint8_t> shadowReject;      // parallel to objects (ShadowReject)
		// Per occlusion view, parallel to objects: the Utility technique the map draws it with (Skylighting::OcclusionTechnique),
		// 0 when none or the map is off (Extensions::OcclusionOn). Alpha-tested ones use shadowDiffuse / shadowMaterial.
		std::array<std::vector<std::uint32_t>, kOcclusionViews> occlusionTechnique;
		// The object's entry in the sun's full-frustum culling processes' objectArray (Coordinator::ResolveSunEntry), null when
		// never tested (an actor's: its cell's container): the entry's listed root. Bound: BindlessPlacement::sunEntry; the node is for
		// diagnostics.
		std::vector<const RE::NiAVObject*> listedRoot;  // parallel to objects
		// Whether the property has a fade node (LodFadeNodeOf), whose centre is the placement row's: for the fade-out test and the
		// draw's specular and envmap LOD fades.
		std::vector<std::uint8_t> hasFadeNode;  // parallel to objects
		// A member's fade-out distance (kObjectFadeTest, AccumulatedPass::fadeDistance), against its fade node's centre
		// (BindlessPlacement::lodFadeNode): > 0 scaled by the camera's LOD factor, < 0 unscaled. Meaningless without the flag.
		std::vector<float> fadeDistance;  // parallel to objects
		// Which slots hold a resident record (PrimaryCull's).
		std::vector<std::uint8_t> residentSlot;  // parallel to objects
		// Each slot's DCLF set phases (SetPhase bits, 0: not a member), written by CommitSet alone; every build selects by them
		// (kObjectMember is the main phase's copy for the GPU). Every write notes the slot (the change log with the record's
		// kObjectMember, else publicationLog).
		std::vector<std::uint8_t> setPhases;  // parallel to objects
		// The change log (dclf-architecture.md, "Persistent draw state"): each write that changes a slot's columns appends the
		// slot and causes. Consumers read from their own LogCursor; one behind the trim, or on a new tables generation, rereads all.
		// The tables' logs are SharedEventLogs: every published snapshot shares their chunks (PublishTables assigns them).
		struct Change
		{
			std::uint32_t slot = 0;
			std::uint32_t causes = 0;  // ChangeCause bits
		};
		SharedEventLog<Change> changeLog;
		std::array<std::uint64_t, kChangeCauseCount> changeCounts{};  // notes by cause, for the report
		void NoteChange(std::uint32_t a_slot, std::uint32_t a_causes)
		{
			if (!a_causes)
				return;
			changeLog.Push({ a_slot, a_causes });
			for (std::uint32_t bits = a_causes; bits; bits &= bits - 1)
				++changeCounts[std::countr_zero(bits)];
		}
		// Every write of a geometry slot's columns, which the published tables replay and GeometryStore repacks by.
		SharedEventLog<std::uint32_t> geometryLog;
		void NoteGeometry(std::uint32_t a_slot) { geometryLog.Push(a_slot); }
		// Material slots whose versions changed (every write of a record), for MainBuild::UpdateRegionPairs and the publication's replay.
		SharedEventLog<std::uint32_t> materialLog;
		void NoteMaterial(std::uint32_t a_slot) { materialLog.Push(a_slot); }
		// Object slots whose published columns changed with no change-log note: the columns no change-log consumer reads (setPhases,
		// layerBase and layerOf of a link's other end, rangeOrdinal) where the write notes nothing. The publication replays them with
		// the logged slots; nothing else reads it.
		SharedEventLog<std::uint32_t> publicationLog;
		void NotePublished(std::uint32_t a_slot) { publicationLog.Push(a_slot); }
		/** @brief Every reader of the change log resyncs. */
		void InvalidateChangeLog()
		{
			changeLog.Invalidate();
			materialLog.Invalidate();
			extrasBlockLog.Invalidate();
			publicationLog.Invalidate();
		}
		/** @brief A slot's columns, everything a persistent consumer builds from, for the writers to compare against. */
		struct Columns
		{
			ObjectRecord object{};
			DrawSequence draw{};
			ObjectAncestorValue ancestorValue{};
			std::array<float, kExtraRows * 4> extras{};
			const RE::BSGeometry* geometry = nullptr;
			const RE::NiAVObject* listedRoot = nullptr;
			std::uint64_t identity = 0;
			std::uint64_t groupIdentity = 0;
			ID3D11ShaderResourceView* shadowDiffuse = nullptr;
			const RE::BSShaderMaterial* shadowMaterial = nullptr;
			float fadeDistance = 0.0f;
			std::uint32_t boneOffset = 0, boneRows = 0, extraOffset = kNoExtraRows, shadowTechnique = 0, streamRow = kNoStreamRow;
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
		// The vertex stream (Api/VertexStreams.h): per stream object (an NPC face shape, Features/Faces') its row - its vertices (its head's
		// snapshot, leased) and their range of the stream's buffer, uploaded on a new generation. Written and updated in place by the
		// component that follows the stream's entry class (Features/Faces': its heads' publications); read as data by the builds.
		std::vector<Api::VertexStreamRow> streamRows;
		std::vector<std::uint32_t> streamRow;  // parallel to objects: index in streamRows, or kNoStreamRow
		std::vector<std::uint32_t> streamRowFree;
		// The (source, first) of every row freed since the walk's end took them (Features/Faces' component frees the range once no row
		// of the shape holds it).
		std::vector<std::pair<const void*, std::uint32_t>> streamRowsReleased;
		/** @brief Writes a slot's stream row: in place when it has one, else at a free index. */
		void SetStreamRow(std::uint32_t a_slot, Api::VertexStreamRow a_row);
		void ClearStreamRow(std::uint32_t a_slot);
		// An alpha-tested caster's diffuse view and material (its binding key); null otherwise. The shadow build reads only the
		// material's texture transform, which BSLightingShaderPropertyFloatController moves between Main::Draw and BeforeShadowMaps.
		std::vector<ID3D11ShaderResourceView*> shadowDiffuse;    // parallel to objects
		std::vector<const RE::BSShaderMaterial*> shadowMaterial;  // parallel to objects
		// The distinct diffuse views among them (a few hundred), so lookups refresh per view rather than per caster.
		std::vector<ID3D11ShaderResourceView*> shadowTextureSet;
		ankerl::unordered_dense::set<ID3D11ShaderResourceView*> shadowTextureSeen;
		// Shadow texture membership transitions for Coordinator::UpdateSharedBindings (true: bind, false: release).
		std::vector<std::pair<ID3D11ShaderResourceView*, bool>> shadowTextureChanges;
		void TakeShadowTextureChanges(std::vector<std::pair<ID3D11ShaderResourceView*, bool>>& a_out)
		{
			a_out.clear();
			a_out.swap(shadowTextureChanges);
		}
		// The distinct shadow keys the casters need, without mode bits (a handful): compiled for, then built per view mode.
		std::vector<ShadowPipelineKey> shadowKeysUsed;
		// The same for the occlusion maps' occluders: complete techniques (RenderDepth included), no mode bits.
		std::array<std::vector<ShadowPipelineKey>, kOcclusionViews> occlusionKeysUsed;  // per occlusion view

		/**
		 * @brief The three shared tables keep their slots across frames.
		 *
		 * A slot lives while objects reference it (Coordinator::UpdateSlotReferences); the drain runs before the loop, so no object
		 * of the frame points at a reused slot. Columns are listed once (GeometryColumns, PipelineColumns, MaterialColumns).
		 *
		 * The used sets are the slots members' records name (KeepResidentsAlive rebuilds them when membership changes). Being in
		 * the set certifies a slot's raw engine pointers (lighting template, material key) for evaluation; CheckObjectSlots holds
		 * every bound record to it. Keys are kept per slot so a freed slot finds its map entry and a cached index can be checked.
		 */
		static constexpr std::uint32_t kSlotFree = ~0u;  // a geometryLastUsed never written; ResolveGeometrySlot's "no slot"
		SlotTable geometrySlots, pipelineSlots, materialSlots;
		std::vector<std::uint32_t> geometryLastUsed;  // parallel to geometries: the walk's last write (PassStamp)
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

		// Object slots persist while the object has a record; a free slot holds FreeObjectRecord(), and a walk sweeps the slots it
		// did not write. Walk parity's reference build (denseWalk) writes densely instead.
		// Parallel to objects: the walk (walkSerial) that last wrote the slot. The coordinator's alone: not published (a
		// publication's holds zeros).
		std::vector<std::uint32_t> objectSeen;
		// New on every tracked insertion, including detach/reattach and pointer reuse.
		std::vector<std::uint64_t> objectIdentity;  // parallel to objects; zero for a free slot
		std::vector<std::uint64_t> objectGroup;  // actor siblings share this ID
		// Parallel to objects: the flags as the scene phase wrote them. The accumulate phase patches the record in place; the delta
		// walk restores each patched slot's scene half from here.
		std::vector<std::uint32_t> sceneFlags;
		// A multi-index shape's main-pass layer (Coordinator::Tracked::layerSlot, drawn from its second index list) is an object
		// of its own: layerBase maps layer slot -> base slot, layerOf base -> layer, else kNoObjectSlot. Parallel to objects. The
		// publication replays both ends of a link with either's change-log note (the layer's write notes it); a write that changes
		// one end with no note of either notes it on publicationLog (ResetObject).
		std::vector<std::uint32_t> layerBase;
		std::vector<std::uint32_t> layerOf;
		bool IsLayer(std::uint32_t a_slot) const { return a_slot < layerBase.size() && layerBase[a_slot] != kNoObjectSlot; }
		std::vector<std::uint32_t> objectFree;
		std::uint32_t liveObjects = 0;
		/**
		 * @brief What was freed since the last publication and is not on a free list yet: nothing is freed while a published
		 * version that may name it lives. PublishTables moves it into the retirement chain (Coordinator::retirement), which returns
		 * it once every publication up to then is gone (Coordinator::RecycleRetired). SlotTables keep their own (TakeRetiring).
		 */
		enum RetiredKind : std::uint8_t
		{
			kRetiredObject,
			kRetiredFamilyRow,  // a table family's row (families); extra: the family's index
			kRetiredExtras,     // slot: the block's row offset
			kRetiredBones,      // slot: the block's offset, extra: its rows
			kRetiredStreamRow,  // a vertex stream row (streamRows)
			kRetiredMaterialLookup,  // a material slot's bindings to drop (Coordinator::RetireMaterialBinding)
			kRetiredPipelineLookup,  // a pipeline slot's lookups to drop (its mask binding: Coordinator::RetireMaskBinding)
			kRetiredGeometrySlot,    // a SlotTable's slot; extra: its generation when freed
			kRetiredMaterialSlot,
			kRetiredPipelineSlot,
			kRetiredComponentRange,  // a coordinator component's range (RetireRange; owner: the component), slot: its first unit, extra: its units
		};
		struct RetiredSlot
		{
			RetiredKind kind = kRetiredObject;
			std::uint8_t owner = 0;  // kRetiredComponentRange's component (Api::CoordinatorComponentId::index)
			std::uint32_t slot = 0, extra = 0;
			std::uint32_t epoch = 0;  // slotEpoch when retired: a clear since drops it
		};
		std::vector<RetiredSlot> retiring;
		std::uint32_t objectsRetiring = 0;  // object slots among them, and in the chain (neither live nor free)
		std::uint32_t slotEpoch = 0;        // moves on when every slot is cleared
		void Retire(RetiredKind a_kind, std::uint32_t a_slot, std::uint32_t a_extra = 0, std::uint8_t a_owner = 0)
		{
			retiring.push_back({ a_kind, a_owner, a_slot, a_extra, slotEpoch });
		}
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
		void Clear();
	};
}
