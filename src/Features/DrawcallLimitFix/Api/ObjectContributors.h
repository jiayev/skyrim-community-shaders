#pragma once

// The object contributor contracts (Api::InstallContext::RegisterTechnique, RegisterGeometryType, RegisterParent, RegisterObjectClass,
// RegisterShading, RegisterFrameGlobals): what a feature adds to the objects DCLF draws. Three kinds, a section each below:
//
// 1. Membership by an engine key: the objects of an engine technique or geometry type the feature ports (TechniqueContribution,
//    GeometryTypeContribution). The key is the engine's own value, so nothing here grows with a feature.
// 2. Membership by a Core class: objects Core recognises but rejects, or holds back, until a feature takes them - the leaves under an
//    engine parent (EngineParent and ParentLeaves: ParentContribution), an object class Core's ports derive (ObjectClass:
//    ObjectClassContribution), and an entry class whose records wait on a coordinator component (EntryClass: the component's
//    entryClass.follows and takes, Api/CoordinatorComponents.h). A view is the same pattern for an engine view (EngineView, Api/Views.h).
// 3. Derivations: the per-object values the Lighting port derives from a property of the feature's (ShadingContribution), and the engine
//    globals that derivation reads, captured with the frame's (FrameGlobalsContribution).
//
// A membership is read as data where Core decides (Core/Extensions/ObjectContributors.h: Extensions::TechniqueObjectFlags,
// GeometryTypeOn, ParentAdmits, ObjectClassOn; Core/Extensions/CoordinatorComponents.h: EntryClassTaken), never called per object. A
// derivation is called per sampled property or per frame, never per object.
//
// Adding a Core class for a feature (section 2) is the same three steps whichever enum it goes in:
// - its value in the enum here, before Count, saying what Core recognises (its Ineligible reason, its trait) and which feature takes it;
// - Core's test of it where Core decides, through the Extensions query above (Core names the value, never the feature);
// - the feature's registration of it, under its toggle (an entry class: the component that follows it).
// The install fails a value a second feature claims, so no value is shared. What a contribution sets that the shaders read (a technique's
// object flags) stays compile-time: the feature declares the bits in its own header, and Features/Manifest.cpp checks them against Core's
// and each other's; its record words go with its table family (Api/TableFamilies.h).

#include "Features/DrawcallLimitFix/Api/Settings.h"

#include <cstdint>
#include <string_view>

namespace DCLF::Api
{
	// Defined by Core (Core/Extensions/ObjectContributors.h), which the features inside DCLF include for it.
	class ShadingDerivation;

	// ---- 1. Membership by an engine key.

	/**
	 * @brief A technique contribution: an engine Lighting technique whose objects the feature ports. Read where the objects are classified
	 * and joined, as data (no call per object).
	 *
	 * - technique: the Lighting technique (SIE::ShaderCache::LightingShaderTechniques; below Extensions::kTechniqueSlots), one feature's.
	 * - toggle: a registered toggle; the technique's objects are DCLF's while it is on (the Lighting port's eligibility).
	 * - objectFlags: the record flags its objects carry (the joins' static flags), the feature's own bits (Records.h, kFeatureObjectFlags).
	 */
	struct TechniqueContribution
	{
		std::uint32_t technique = ~0u;
		ToggleId toggle;
		std::uint32_t objectFlags = 0;
	};

	/**
	 * @brief A geometry type contribution: an engine geometry type (RE::BSGeometry::Type) whose objects the feature ports, a class of DCLF's
	 * while its toggle is on. Read where the objects are classified, tracked and captured (the Lighting and Effect ports' eligibility, the
	 * walk's traits, the engine side's captures), as data (no call per object).
	 *
	 * - geometryType: the type (below Extensions::kGeometryTypeSlots), one feature's.
	 * - toggle: a registered toggle.
	 */
	struct GeometryTypeContribution
	{
		std::uint8_t geometryType = 0xFF;
		ToggleId toggle;
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
		// its key carries the mode (Records.h, RasterEffectBillboard), which the vertex stage turns it by.
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
	 */
	struct ObjectClassContribution
	{
		ObjectClass objectClass = ObjectClass::Count;
		ToggleId toggle;
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
		// engine's face morphing, not its buffers'. Its record waits for its second vertex stream (Features/Faces).
		FaceShape,
		Count,
	};

	// ---- 3. Derivations.

	/**
	 * @brief A shading contribution: the per-object values SetupGeometry derives for a property of the feature's (the shading rows' tree
	 * parameters and wind timers), what a member draws with until the feature's GPU stage has its own.
	 *
	 * - derive: FrameValues' pool (a shading sample: the slots shading events named, inside the engine-read window and the frame's
	 *   FrameGlobals scope) or a parity's sample, for a property with any of propertyFlags: writes ShadingDerivation::out. Called per
	 *   sampled property that has the flags, never per object per frame.
	 */
	struct ShadingContribution
	{
		std::string_view name;
		std::uint64_t propertyFlags = 0;  // BSShaderProperty::EShaderPropertyFlag bits
		void (*derive)(const ShadingDerivation& a_derivation) = nullptr;
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
}
