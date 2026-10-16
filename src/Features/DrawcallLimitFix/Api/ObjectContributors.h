#pragma once

// The object contributor contracts (Api::InstallContext::RegisterTechnique, RegisterGeometryType, RegisterParent, RegisterObjectClass,
// RegisterEntryClass, RegisterShading, RegisterFrameGlobals): what a feature adds to the objects DCLF draws. Three kinds, a section each
// below:
//
// 1. Membership by an engine key: the objects of an engine technique or geometry type the feature ports (TechniqueContribution,
//    GeometryTypeContribution). The key is the engine's own value, so nothing here grows with a feature.
// 2. Membership by a Core class: objects Core recognises but rejects, or holds back, until a feature takes them - the leaves under an
//    engine parent (EngineParent and ParentLeaves: ParentContribution), an object class Core's ports derive (ObjectClass:
//    ObjectClassContribution), and an entry class whose records wait on a coordinator component (EntryClass: the component's
//    entryClass.follows and takes, Api/CoordinatorComponents.h; or an owner class, by its entries' reference's form type:
//    EntryClassContribution). A view is the same pattern for an engine view (EngineView, Api/Views.h).
// 3. Derivations: the per-object values the Lighting port derives from a property of the feature's (ShadingContribution), and the engine
//    globals that derivation reads, captured with the frame's (FrameGlobalsContribution).
// 4. Material ports: an engine shader other than Lighting whose objects Core classifies, captures and draws through the feature's port
//    (MaterialPortContribution, Api::InstallContext::RegisterMaterialPort): its property and material classes, its constant space, the rules
//    Core's classification and builds read as data, and the callbacks Core makes per event (a capture, a classification, an evaluation, a
//    shading sample).
//
// A membership is read as data where Core decides (Core/Extensions/ObjectContributors.h: Extensions::TechniqueObjectFlags,
// PropertyFlagRules, GeometryTypeOn, GeometryTypeMoves, PlannedItemClass, EntryClassOf, OwnerClassOf, ParentAdmits, ObjectClassOn,
// CategoryRoots; Core/Extensions/CoordinatorComponents.h: EntryClassTaken), never called per object (a class's category root: per frame). A derivation
// is called per sampled property or per frame, never per object.
//
// Adding a Core class for a feature (section 2) is the same three steps whichever enum it goes in:
// - its value in the enum here, before Count, saying what Core recognises (its Ineligible reason, its trait) and which feature takes it;
// - Core's test of it where Core decides, through the Extensions query above (Core names the value, never the feature);
// - the feature's registration of it, under its toggle (an entry class: the component that follows it).
// The install fails a value a second feature claims, so no value is shared. What a contribution sets that the shaders read (a technique's
// object flags) stays compile-time: the feature declares the bits in its own header, and Features/Manifest.cpp checks them against Core's
// and each other's; its record words go with its table family (Api/TableFamilies.h).

#include "Features/DrawcallLimitFix/Api/EntryRanges.h"
#include "Features/DrawcallLimitFix/Api/Settings.h"

#include <cstdint>
#include <span>
#include <string_view>
#include <utility>

namespace RE
{
	class BSShaderProperty;
	class NiNode;
}

namespace DCLF::Api
{
	// Defined by Core (Core/Extensions/ObjectContributors.h), which the features inside DCLF include for it.
	class ShadingDerivation;
	// Defined by Core (Core/Extensions/MaterialPorts.h), likewise.
	class MaterialPortCapture;
	class MaterialPortDerivation;
	class MaterialPortEvaluation;
	class MaterialPortShading;
	// Defined below (section 2) and by Api/FrameValues.h: a geometry type contribution names them (value None: 0).
	enum class EntryClass : std::uint8_t;
	enum class FrameValueItemClass : std::uint8_t;

	// ---- 1. Membership by an engine key.

	/**
	 * @brief A technique contribution: an engine Lighting technique whose objects the feature ports. Read where the objects are classified
	 * and joined, as data (no call per object).
	 *
	 * - technique: the Lighting technique (SIE::ShaderCache::LightingShaderTechniques; below Extensions::kTechniqueSlots), one feature's.
	 * - toggle: a registered toggle; the technique's objects are DCLF's while it is on (the Lighting port's eligibility).
	 * - objectFlags: the record flags its objects carry (the joins' static flags), the feature's own bits (Records.h, kFeatureObjectFlags).
	 * - propertyFlags, geometryType (optional): the Lighting property flags (BSShaderProperty::EShaderPropertyFlag bits) that mark its
	 *   objects whatever technique the flags pick, and the geometry type such an object must be: an object whose property has any of them
	 *   is DCLF's only while toggle is on and its geometry is geometryType, else Ineligible::Lod (object and terrain LOD: Features/Lod).
	 *   Resolved once into Core's flag rules (Extensions::PropertyFlagRules), which the Lighting port's eligibility reads.
	 */
	struct TechniqueContribution
	{
		std::uint32_t technique = ~0u;
		ToggleId toggle;
		std::uint32_t objectFlags = 0;
		std::uint64_t propertyFlags = 0;
		std::uint8_t geometryType = 0xFF;
	};

	/**
	 * @brief The mirrored node kind bits an entry class rule may test (GeometryTypeContribution::EntryRule::parentKind, the mirror's capture
	 * kind as is; Core checks they match it). FaceGen: a BSFaceGenNiNode.
	 */
	inline constexpr std::uint32_t kMirroredKindFaceGen = 1u << 7;

	/**
	 * @brief A geometry type contribution: an engine geometry type (RE::BSGeometry::Type) whose objects the feature ports, a class of DCLF's
	 * while its toggle is on. Read where the objects are classified, tracked and captured (the Lighting and Effect ports' eligibility, the
	 * walk's traits, the engine side's captures), as data (no call per object).
	 *
	 * - geometryType: the type (below Extensions::kGeometryTypeSlots), one feature's.
	 * - toggle: a registered toggle (none only for a contribution that sets entry alone: its type is no class of DCLF's).
	 * - propertyFlags (optional): its objects are only those whose Lighting property has any of these flags (BSShaderProperty::
	 *   EShaderPropertyFlag bits), drawn as a tri-shape while toggle is on, Ineligible::Lod while off (Eligibility::Static: object LOD's
	 *   sub-index shapes, Features/Lod); 0: every object of the type.
	 * - itemClass (optional): the FrameValues item class (Api/FrameValues.h) its skinless objects with a palette block are. The walk sizes
	 *   a member's block by the class's rule while toggle is on (for Core's capture of a complete particle system: its units), the class's
	 *   geometry source is its slot's, the coordinator plans the item as the class and the frame values' parity leaves its rows to the
	 *   class (Extensions::PlannedItemClass). None: none.
	 * - moves: its objects change every frame invisibly to the walk while toggle is on (a particle system's bound and quads): movers in
	 *   the walk's per-frame traits, placed by FrameValues every frame, their own updates their move events.
	 * - entry (optional): the entry class (Api::EntryClass) its objects under a parent with any of entry.parentKind are, whatever toggle:
	 *   resolved once per tracked entry by the walk, and read by the eligibility and the attach's prepare (Extensions::EntryClassOf).
	 *   - perFrame: its entries' records follow what the following component publishes: classified every frame while the class is taken,
	 *     and given the walk's per-frame entry-class trait (no root motion of their own; the light path's).
	 *   - holdsParent: an entry's parent is held from the attach's pins as its source node (EntryWrite::source: a face shape's head).
	 * - ranges (optional): its objects draw the index ranges this captures (Api/EntryRanges.h: object LOD's visible segments' runs,
	 *   Features/Lod), whatever toggle: called on the capturing thread inside the engine-read window, per record capture (an attach, or the
	 *   type's ranges update: Extensions::PushRangesUpdate), never per frame; a_out arrives empty. The coordinator keeps
	 *   them by tracked entry from its attach (Extensions::GeometryTypeRanged), and the walk draws them as a chain of range slots.
	 */
	struct GeometryTypeContribution
	{
		struct EntryRule
		{
			EntryClass entryClass{};
			std::uint32_t parentKind = 0;  // kMirroredKind* bits
			bool perFrame = false;
			bool holdsParent = false;
		};

		std::uint8_t geometryType = 0xFF;
		ToggleId toggle;
		std::uint64_t propertyFlags = 0;
		FrameValueItemClass itemClass{};
		bool moves = false;
		EntryRule entry;
		void (*ranges)(const void* a_geometry, std::vector<EntryRange>& a_out) = nullptr;
	};

	// ---- 2. Membership by a Core class (a value per class a feature takes: see the steps above).

	/** @brief An engine parent class Core rejects the leaves under (Eligibility's parent reasons), which a parent contribution may admit. */
	enum class EngineParent : std::uint8_t
	{
		Billboard,  // NiBillboardNode (Ineligible::Billboard): its OnVisible turns it to each culling camera
		Switch,     // NiSwitchNode (Ineligible::Switch): its OnVisible culls the selected child alone
		Actor,      // a node whose reference is an actor (the mirror's NodeRecord::actor; Ineligible::Actor): the actor's own 3D
		Count,
	};

	/** @brief The leaves under such a parent a parent contribution takes. */
	enum class ParentLeaves : std::uint8_t
	{
		// A skinless BSEffectShaderProperty BSTriShape under exactly one such parent whose mode Core reads (a billboard's face mode, 0-5):
		// its key carries the mode (its family's spare key bits: Records.h, kRasterEffectBillboardShift), which the vertex stage turns it
		// by.
		TurnedEffectShapes,
		// A particle system (ParticleGeometryType) under any number of them.
		ParticleSystems,
		// Every leaf under any number of them, in the frames each selects the branch it lies on: the classification's chain test
		// (Coordinator::SwitchSelects), on the selections Core follows by event for the primary cull (SwitchEvents).
		SelectedLeaves,
		// Every leaf under any number of them, whatever its class: the classification's chain test.
		AllLeaves,
		Count,
	};

	/**
	 * @brief A parent contribution: the leaves under an engine parent class Core rejects them under that the feature ports, while its
	 * toggle is on. Read where the objects are classified (Coordinator::ClassifyFrame, the joins' keys) and by the census, as data.
	 *
	 * - parent, leaves: what it admits, one feature's.
	 * - toggle: a registered toggle.
	 */
	struct ParentContribution
	{
		EngineParent parent = EngineParent::Billboard;
		ParentLeaves leaves = ParentLeaves::TurnedEffectShapes;
		ToggleId toggle;
	};

	/**
	 * @brief An object class Core's ports recognize and derive (its draw state, its range of the main pass) but leave native until a
	 * feature takes it.
	 */
	enum class ObjectClass : std::uint8_t
	{
		// A decal (kDecal, kDynamicDecal: accumulation hints 2 and 3), Lighting's or the Effect port's, and the opaque effects drawn through
		// the opaque decal group (Ineligible::Decal, kEffectVariantDecal, Ineligible::EffectOpaque).
		Decal,
		// A multi-index shape's layer (the additional property's passes, hint 12: the decal group 3) and the MultiIndexSparkle technique
		// (Ineligible::MultiIndex).
		Layer,
		// Object LOD: a BSSubIndexTriShape with the LOD object flags under TES::lodLandRoot, drawn by its visible segments (the LODObjects
		// and LODObjectHD techniques; Ineligible::Lod), and the LOD root listed as a category node (Features/Lod).
		LodObjects,
		// Terrain LOD: a land block's shape under TES::lodLandRoot (the LODLand and LODLandNoise techniques; Ineligible::Lod), and the LOD
		// root listed as a category node (Features/Lod).
		LodTerrain,
		// Tree LOD: the instance groups under the LOD trees root (BSDistantTreeShaderProperty shapes), drawn from the mirrored instance
		// records as the TreeInstances draw family (Api/DrawFamilies.h: its tables, its cull and draws in the depth segment and in a trees
		// view's faces; its decision) instead of the engine's passes (Features/TreeLod): the class gates the family.
		TreeLod,
		// An effect: a BSEffectShaderProperty object, the Effect port's (DeriveEffectDescriptors, Effect.hlsl): a blended mesh in the forward
		// epoch's sort, a decal in the decal groups, a skinned one through the skin palette, a particle system with its geometry type's
		// contribution (Ineligible::NotLightingShader without the class), and the effect controllers' material captures (Features/Effects).
		Effect,
		// An opaque effect (hint 0: no blending alpha property, no Falloff, depth tested and written) drawn through the opaque decal group
		// without its bias (Ineligible::EffectOpaque without the class, or while Decal is off). Read whether it is taken, whatever its toggle
		// (Extensions::ObjectClassTaken): the Effect port is reached only while Effect is on (Features/Effects).
		OpaqueEffect,
		Count,
	};

	/**
	 * @brief An object class contribution: the class's objects are DCLF's while the toggle is on. Read where the objects are classified
	 * (the Lighting and Effect ports' eligibility, the primary entries and membership passes, the category nodes listed, the material
	 * captures), and by the paths that draw a class (tree LOD's decision and pipelines, a view's LOD roots), as data (no call per object).
	 * A class standing for a load-time switch is read as taken, whatever the toggle (Extensions::ObjectClassTaken).
	 *
	 * - objectClass: the class, one feature's.
	 * - toggle: a registered toggle.
	 * - categoryRoot (optional): the engine root the class's objects are under, listed as a category node while the class is on (the
	 *   category capture: render thread, the frame's start, every frame; null: none now). Called per frame, never per object; two classes
	 *   may list the same root (TES::lodLandRoot: Features/Lod's).
	 */
	struct ObjectClassContribution
	{
		ObjectClass objectClass = ObjectClass::Count;
		ToggleId toggle;
		RE::NiNode* (*categoryRoot)() = nullptr;
	};

	/**
	 * @brief A class of tracked entries Core's walk classifies but whose records wait on a coordinator component: the component that
	 * follows it (CoordinatorComponent::entryClass, Api/CoordinatorComponents.h) supplies what their writes need, and its objects are
	 * DCLF's only while that component takes it. Read as data by the eligibility, the walk and the frame's addresses
	 * (Extensions::EntryClassTaken).
	 */
	enum class EntryClass : std::uint8_t
	{
		None,
		// A dynamic shape under a BSFaceGenNiNode (an NPC's head, mouth, hair, brows): its positions are its head's, published by the
		// engine's face morphing, not its buffers'. Its record waits for its second vertex stream (Features/Faces: matched by its geometry
		// type contribution's entry rule, classified every frame, its head held).
		FaceShape,
		// A tracked entry owned by an actor (its leaf's reference an ActorCharacter): the actor's 3D, carried items too. An owner class
		// (EntryClassContribution: Features/Actors, whatever its toggle), apart from the type-matched class (a face shape is both).
		ActorOwned,
		Count,
	};

	/**
	 * @brief An entry class contribution: an owner class, the tracked entries whose leaf's reference (the mirror's NodeRecord::userData) is
	 * of form type ownerFormType (RE::FormType). Resolved once per tracked entry by the walk from the mirror, whatever toggle
	 * (Extensions::OwnerClassOf: a table by form type, built at the install), and kept apart from the type-matched entry class
	 * (GeometryTypeContribution::entry), which an entry may also have. Read as data where Core decides, never called.
	 *
	 * - entryClass: the class (not None; one contribution's, and no geometry type's entry class).
	 * - name: what the walk's timing report calls its entries (EvaluateKind::Owned: "<name>", "<name> without a record").
	 * - ownerFormType: the form type (one contribution's).
	 * - everyFrame: its entries' records change with no event (an actor's equipment shown, hidden and swapped): per frame whatever their
	 *   traits (the walk's owner class trait), their frame verdict retaken by the light path (FrameVerdictKept), a hidden verdict never
	 *   static (Eligibility::HiddenStatic).
	 * - ownMotion: its entries move with no controller or body on their chain (an actor's skeleton: Havok's behaviour graph): placed every
	 *   frame (FrameValues' movers), their root motion never read, node and motion events never reclassify them. Needs everyFrame.
	 * - published: its members are listed for the frame (SceneTables::actorObjects and actorWetness: Advanced Skin's wetness).
	 */
	struct EntryClassContribution
	{
		EntryClass entryClass = EntryClass::None;
		std::string_view name;
		std::uint8_t ownerFormType = 0;
		bool everyFrame = false;
		bool ownMotion = false;
		bool published = false;
	};

	// ---- 3. Derivations.

	/**
	 * @brief The feature block's words: the shading row's and the object record's (BindlessShading::featureBlock, BindlessObject::
	 * featureBlock; DCLFObjects.hlsli's, frozen). One contribution claims it.
	 */
	inline constexpr std::uint32_t kShadingBlockWords = 8;

	/**
	 * @brief A Lighting VS PerGeometry variable a block supplies per object (ShadingContribution::vsVariables): the variable
	 * (ShaderConstants::LightingVS index), its first word in the block, the floats it takes from there (the rest of the variable the
	 * pipeline template's), and the first `compared` of those a capture parity compares with the engine's (the rest depend on native draw
	 * order).
	 */
	struct ShadingVariable
	{
		std::uint32_t variable = 0;
		std::uint8_t firstWord = 0, floats = 0, compared = 0;
	};

	/**
	 * @brief A shading contribution: the per-object values SetupGeometry derives for a property of the feature's (the shading rows' tree
	 * parameters and wind timers), what a member draws with until the feature's GPU stage has its own.
	 *
	 * - derive: FrameValues' pool (a shading sample: the slots shading events named, inside the engine-read window and the frame's
	 *   FrameGlobals scope) or a parity's sample, for a property with any of propertyFlags: writes ShadingDerivation::block (zero-filled
	 *   before). Called per sampled property that has the flags, never per object per frame.
	 * - objectFlags, blockWords, vsVariables: the feature block (one contribution's; a second claimant fails the install). The shading row
	 *   keeps the first blockWords words derive wrote (the rest zero); for an object with any of objectFlags (its technique contribution's)
	 *   the Lighting port draws vsVariables from them, not from the pipeline template. Read as data, resolved once
	 *   (Extensions::ShadingBlockRule); vsVariables kept as given (static storage).
	 */
	struct ShadingContribution
	{
		std::string_view name;
		std::uint64_t propertyFlags = 0;  // BSShaderProperty::EShaderPropertyFlag bits
		void (*derive)(const ShadingDerivation& a_derivation) = nullptr;
		std::uint32_t objectFlags = 0;
		std::uint32_t blockWords = 0;
		std::span<const ShadingVariable> vsVariables;
	};

	/** @brief A registered frame-globals contribution (RegisterFrameGlobals' result): its first float in FrameGlobals::features. */
	struct FrameGlobalsSlot
	{
		std::uint32_t first = ~0u;
		explicit operator bool() const { return first != ~0u; }
	};

	/**
	 * @brief A frame-globals contribution: engine globals a feature's derivations read, captured with the frame's (FrameGlobals), so the
	 * scene work and FrameValues' pool read them from the capture their frame carries, never from the engine.
	 *
	 * - capture: the render thread at the frame's start (FrameGlobals::Capture, the engine's update done): the frame's floats into
	 *   a_out[0, floats), zero-filled before.
	 */
	struct FrameGlobalsContribution
	{
		std::string_view name;
		std::uint32_t floats = 0;
		void (*capture)(float* a_out) = nullptr;
	};

	// ---- 4. Material ports.

	/**
	 * @brief A port's constant space for one stage: its engine shader's variables by index (as its Setup functions write them), each 4 floats
	 * of a ConstantBlock but the `sized` ones (variable, floats) and the `scratch` ones nothing reads (one shared region: StageLayout,
	 * Core/Extensions/MaterialPorts.h); which constant group each belongs to (PerTechnique, PerMaterial, PerGeometry: b0-b2) and each
	 * group's first variable; the shader's member names by variable.
	 */
	struct MaterialStageSpace
	{
		std::uint32_t variables = 0;
		std::span<const std::pair<std::uint32_t, std::uint32_t>> sized;
		std::span<const std::uint32_t> scratch;
		std::uint64_t groups[3]{};
		std::uint32_t first[3]{};
		std::span<const std::string_view> names;
	};

	/**
	 * @brief What Core's classification, joins and material records read of a port, as data (Extensions::PortRules; Core's Lighting has its
	 * own, kind 0).
	 */
	struct MaterialPortRules
	{
		std::uint32_t passEnumBase = 0;       // the engine's pass enum less the pass descriptor (MakeRenderPass's): a member's and a decal's order key
		std::uint32_t deferredPixelBit = 0;   // the pixel descriptor's G-buffer bit, dropped for a forward decal (Coordinator::ViewDecalForward)
		std::uint32_t projectedPassBit = 0;   // the pass descriptor's ProjectedUV binding bit (0: none)
		std::uint32_t frameVSVariable = ~0u;  // the VS variable a record takes from the frame (the texture transforms' TexcoordOffset)
		bool techniqueRows = true;            // false: its keys read technique row 0 (no Lighting technique, no shadow mask)
		bool rawPermutation = false;          // its permutation: the raw VS, the raw PS bits the lookup dropped, InWorld, no feature bits
		bool signatureComponents = true;      // false: its records have no signature-sourced component (MaterialSources::Signature)
		bool emissiveAlwaysListed = false;    // its property's emissive colour is listed whatever its flags (the walk's emittance events)
		bool turnedByBillboards = false;      // ParentLeaves::TurnedEffectShapes' leaves are its properties' (Coordinator::BillboardModeOf)
		bool particleGeometry = false;        // the particle geometry types' leaves with its property are its to classify (Eligibility::Static)
	};

	/**
	 * @brief A material port: the objects of an engine shader (other than Lighting, which is Core's) whose property and material Core captures
	 * and whose objects it classifies, joins and draws through the port while its class is on. Read where the objects are captured,
	 * classified and joined, and where the material records are evaluated and packed, as data resolved once at install
	 * (Extensions::MaterialPorts, by kind: the index records carry, PropertyRecord::port, MaterialSnapshot::port, LightingDescriptors::port,
	 * MaterialRecord::kind); the callbacks run per event, never per object per frame.
	 *
	 * - shaderType: its engine shader (RE::BSShader::Type), one port's; never Lighting.
	 * - objectClass: the class whose toggle turns it on (registered before).
	 * - propertyRtti, materialVtable: its property's RTTI and its material's vtable, called once at install.
	 * - materialBytes, textureFields: the material bytes a snapshot keeps, and its NiSourceTexture fields (offsets).
	 * - vs, ps: its constant space (at most 64 variables a stage).
	 * - rules: what Core reads as data.
	 * - capture: the property's capture (SceneCapture::CaptureProperty, in the engine-read window or under the engine call that holds it):
	 *   what the port's derive and shading read of the property and its material into the record.
	 * - derive: per classification, join or membership pass (any thread): the object's descriptors from its records, or why it stays
	 *   native. Pure.
	 * - evaluate: per material evaluation (the scene work and its pool): the record of a snapshot of its material under a pass descriptor;
	 *   false for one the port does not cover. Pure.
	 * - shading: per shading item (FrameValues' pool, in the engine-read window): the shading row of its property. Pure but for the reads.
	 * - variants, variantNames: the census's split of its variant reason (a bit each, named).
	 * - name: the census's name of its objects' classes ("<name>: ..." where Lighting's read "lighting: ...").
	 * - emissiveOf: per external emittance re-point (SetExternalEmittance's hook, any thread the engine calls it on): the colour its
	 *   property's emissive points at now (the record's emissive, its capture's); null: its emissive is never re-pointed. Pure.
	 */
	struct MaterialPortContribution
	{
		std::string_view name;
		std::uint8_t shaderType = 0xFF;
		ObjectClass objectClass = ObjectClass::Count;
		const void* (*propertyRtti)() = nullptr;
		const void* (*materialVtable)() = nullptr;
		std::uint32_t materialBytes = 0;
		std::span<const std::uint16_t> textureFields;
		MaterialStageSpace vs, ps;
		MaterialPortRules rules;
		void (*capture)(const MaterialPortCapture& a_capture) = nullptr;
		void (*derive)(MaterialPortDerivation& a_derivation) = nullptr;
		bool (*evaluate)(MaterialPortEvaluation& a_evaluation) = nullptr;
		void (*shading)(MaterialPortShading& a_shading) = nullptr;
		std::uint32_t (*variants)(const MaterialPortDerivation& a_derivation) = nullptr;
		std::span<const std::string_view> variantNames;
		const void* (*emissiveOf)(const RE::BSShaderProperty& a_property) = nullptr;
	};
}
