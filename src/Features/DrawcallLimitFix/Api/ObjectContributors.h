#pragma once

// The object contributor contracts (Api::InstallContext::RegisterTechnique, RegisterGeometryType, RegisterParent, RegisterObjectClass,
// RegisterShading, RegisterFrameGlobals): what a feature adds to the objects DCLF draws of an engine technique, geometry type, parent
// class or object class it ports -
// their eligibility and record flags, the per-object values the Lighting port derives from their properties, and the engine globals that
// derivation reads, captured with the frame's. The records' layouts
// stay compile-time: a feature declares its flag bits in its own header (Features/Manifest.cpp checks them against Core's and each
// other's), and its record words with its table family (Api/TableFamilies.h).

#include "Features/DrawcallLimitFix/Api/Settings.h"

#include <cstdint>
#include <string_view>

namespace DCLF::Api
{
	// Defined by Core (Core/Extensions/ObjectContributors.h), which the features inside DCLF include for it.
	class ShadingDerivation;

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
		Count,
	};

	/**
	 * @brief An object class contribution: the class's objects are DCLF's while the toggle is on. Read where the objects are classified
	 * (the Lighting and Effect ports' eligibility, the primary entries), as data (no call per object).
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
