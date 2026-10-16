#pragma once

// The object contributor contracts' Core side (Api/ObjectContributors.h): the technique contributions by technique (the Lighting port's
// eligibility and the joins' record flags read them as data) and their property flag rules, the geometry type, parent and object class
// contributions' toggles and rules (the eligibility, the walk, the captures, the classification, the plans and the category capture read
// them as data: a type's property flags, item class, movers, entry class and range capture, a class's category root), what a shading
// contribution is handed, and the calls the Lighting port's shading sample and the frame globals' capture make.

#include "Features/DrawcallLimitFix/Api/FrameValues.h"
#include "Features/DrawcallLimitFix/Api/ObjectContributors.h"
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace RE
{
	class BSShaderProperty;
	class NiNode;
}

namespace DCLF
{
	struct ObjectTreeAnim;
}

namespace DCLF::Api
{
	/**
	 * @brief A shading contribution's derive: the property sampled, and the shading row's per-object PerGeometry pair it writes
	 * (BindlessShading's TreeParams and WindTimers; the sample keeps xy of the timers, zero-filled before).
	 */
	class ShadingDerivation
	{
	public:
		const RE::BSShaderProperty& property;
		ObjectTreeAnim& out;
	};
}

namespace DCLF::Extensions
{
	/** @brief The Lighting techniques a contribution may name (SIE::ShaderCache::LightingShaderTechniques' values below it). */
	inline constexpr std::uint32_t kTechniqueSlots = 32;
	/** @brief The floats the frame globals hold for the features' contributions (FrameGlobals::features): grow it before more are registered. */
	inline constexpr std::uint32_t kFrameGlobalsFloats = 8;

	/** @brief A technique's contribution (none: no toggle, no flags). */
	struct TechniqueEntry
	{
		Api::ToggleId toggle;
		std::uint32_t objectFlags = 0;
	};
	using TechniqueEntries = std::array<TechniqueEntry, kTechniqueSlots>;

	/** @brief Any thread, after InstallFeatures (all none before): the technique contributions, by technique. */
	const TechniqueEntries& RegisteredTechniques();

	/** @brief The record flags a_technique's objects carry (its contribution's; 0 for none). */
	inline std::uint32_t TechniqueObjectFlags(const TechniqueEntries& a_techniques, std::uint32_t a_technique)
	{
		return a_technique < a_techniques.size() ? a_techniques[a_technique].objectFlags : 0u;
	}

	/** @brief The geometry types a contribution may name (RE::BSGeometry::Type's values below it). */
	inline constexpr std::uint32_t kGeometryTypeSlots = 16;
	/** @brief The geometry type contributions' toggles, by type (none: no contribution). */
	using GeometryTypeToggles = std::array<Api::ToggleId, kGeometryTypeSlots>;
	/** @brief Any thread, after InstallFeatures (all none before): the geometry type contributions' toggles. */
	const GeometryTypeToggles& RegisteredGeometryTypes();

	/** @brief Whether a_type's objects are a class DCLF ports under a_toggles (its contribution's toggle on; false for none). */
	inline bool GeometryTypeOn(const ToggleSet& a_toggles, std::uint8_t a_type)
	{
		const auto& types = RegisteredGeometryTypes();
		return a_type < types.size() && a_toggles.On(types[a_type]);
	}

	/** @brief The geometry type contributions, by type (geometryType 0xFF: none): their rules, read as data (no call per object). */
	using GeometryTypeContributions = std::array<Api::GeometryTypeContribution, kGeometryTypeSlots>;
	/** @brief Any thread, after InstallFeatures (all none before): the geometry type contributions. */
	const GeometryTypeContributions& RegisteredGeometryTypeContributions();

	/** @brief a_type's contribution (a none one past the slots). */
	inline const Api::GeometryTypeContribution& GeometryTypeContributionOf(std::uint8_t a_type)
	{
		static constexpr Api::GeometryTypeContribution kNone{};
		const auto& types = RegisteredGeometryTypeContributions();
		return a_type < types.size() ? types[a_type] : kNone;
	}

	/** @brief Whether a_type's objects are movers under a_toggles (its contribution's moves, its toggle on). */
	inline bool GeometryTypeMoves(const ToggleSet& a_toggles, std::uint8_t a_type)
	{
		const auto& type = GeometryTypeContributionOf(a_type);
		return type.moves && a_toggles.On(type.toggle);
	}

	/** @brief a_type's range capture (its contribution's ranges; null: its objects draw whole). */
	inline auto GeometryRangesOf(std::uint8_t a_type)
	{
		return GeometryTypeContributionOf(a_type).ranges;
	}

	/** @brief Whether a_type's objects draw captured ranges (its contribution's ranges), whatever its toggle: kept by the coordinator. */
	inline bool GeometryTypeRanged(std::uint8_t a_type)
	{
		return GeometryRangesOf(a_type) != nullptr;
	}

	/** @brief The item class a_type's skinless objects are under a_toggles (its contribution's itemClass, its toggle on; None for none). */
	inline Api::FrameValueItemClass GeometryItemClassOn(const ToggleSet& a_toggles, std::uint8_t a_type)
	{
		const auto& type = GeometryTypeContributionOf(a_type);
		return a_toggles.On(type.toggle) ? type.itemClass : Api::FrameValueItemClass::None;
	}

	/**
	 * @brief The item class the coordinator plans a record as (Coordinator::PlacedKeyOf), which the frame values' parity tests too: for a
	 * record with a palette block (a_paletteBlock: kObjectPaletteBlock with bone rows) of a skinless geometry of a_type, its contribution's
	 * itemClass, whatever the toggle (the block says it is one); else None (a skin's palette, or no block).
	 */
	inline Api::FrameValueItemClass PlannedItemClass(bool a_paletteBlock, std::uint8_t a_type, bool a_skinned)
	{
		return a_paletteBlock && !a_skinned ? GeometryTypeContributionOf(a_type).itemClass : Api::FrameValueItemClass::None;
	}

	/** @brief The entry classes' rules, by class (from the contribution that names it; a default rule for none). */
	using EntryClassRules = std::array<Api::GeometryTypeContribution::EntryRule, static_cast<std::size_t>(Api::EntryClass::Count)>;
	/** @brief Any thread, after InstallFeatures (all default before): the entry classes' rules. */
	const EntryClassRules& RegisteredEntryClassRules();

	/** @brief The entry class a geometry of a_type under a parent of mirrored kind a_parentKind is (its contribution's entry; None for none). */
	inline Api::EntryClass EntryClassOf(std::uint8_t a_type, std::uint32_t a_parentKind)
	{
		const auto& entry = GeometryTypeContributionOf(a_type).entry;
		return (entry.parentKind & a_parentKind) ? entry.entryClass : Api::EntryClass::None;
	}

	/** @brief a_class's rule (a default one for None). */
	inline const Api::GeometryTypeContribution::EntryRule& EntryClassRuleOf(Api::EntryClass a_class)
	{
		return RegisteredEntryClassRules()[a_class < Api::EntryClass::Count ? static_cast<std::size_t>(a_class) : 0];
	}

	/**
	 * @brief A Lighting property flag rule (Api::TechniqueContribution::propertyFlags): an object whose property has any of propertyFlags
	 * is DCLF's only while toggle is on and its geometry is geometryType (else Ineligible::Lod).
	 */
	struct PropertyFlagRule
	{
		std::uint64_t propertyFlags = 0;
		std::uint8_t geometryType = 0xFF;
		Api::ToggleId toggle;
	};
	/** @brief Any thread, after InstallFeatures (none before): the flag rules, once each (techniques that share one name it once). */
	std::span<const PropertyFlagRule> PropertyFlagRules();

	/** @brief The parent contributions' toggles, by parent and leaves (none: no contribution). */
	using ParentToggles = std::array<std::array<Api::ToggleId, static_cast<std::size_t>(Api::ParentLeaves::Count)>, static_cast<std::size_t>(Api::EngineParent::Count)>;
	/** @brief Any thread, after InstallFeatures (all none before): the parent contributions' toggles. */
	const ParentToggles& RegisteredParents();

	/** @brief Whether a_leaves under a_parent are DCLF's under a_toggles (their contribution's toggle on; false for none). */
	inline bool ParentAdmits(const ToggleSet& a_toggles, Api::EngineParent a_parent, Api::ParentLeaves a_leaves)
	{
		return a_toggles.On(RegisteredParents()[static_cast<std::size_t>(a_parent)][static_cast<std::size_t>(a_leaves)]);
	}

	/** @brief The object class contributions' toggles, by class (none: no contribution). */
	using ObjectClassToggles = std::array<Api::ToggleId, static_cast<std::size_t>(Api::ObjectClass::Count)>;
	/** @brief Any thread, after InstallFeatures (all none before): the object class contributions' toggles. */
	const ObjectClassToggles& RegisteredObjectClasses();

	/** @brief Whether a_class's objects are DCLF's under a_toggles (its contribution's toggle on; false for none). */
	inline bool ObjectClassOn(const ToggleSet& a_toggles, Api::ObjectClass a_class)
	{
		return a_toggles.On(RegisteredObjectClasses()[static_cast<std::size_t>(a_class)]);
	}

	/** @brief The object class contributions' category roots, by class (null: none). */
	using CategoryRoots = std::array<RE::NiNode* (*)(), static_cast<std::size_t>(Api::ObjectClass::Count)>;
	/** @brief Any thread, after InstallFeatures (all null before): the classes' category roots (Api::ObjectClassContribution::categoryRoot). */
	const CategoryRoots& RegisteredCategoryRoots();

	/**
	 * @brief Whether a feature took a_class at install, whatever its toggle: a class whose registration stands for a load-time switch, read
	 * on a path its own toggle's class already gates (ObjectClass::OpaqueEffect, inside the Effect port).
	 */
	inline bool ObjectClassTaken(Api::ObjectClass a_class)
	{
		return static_cast<bool>(RegisteredObjectClasses()[static_cast<std::size_t>(a_class)]);
	}

	/**
	 * @brief The shading sample (SampleShading): every shading contribution whose property flags a_property has, in registration order,
	 * deriving a_out (left as it is by none).
	 */
	void DeriveShading(const RE::BSShaderProperty& a_property, ObjectTreeAnim& a_out);
	/** @brief Render thread, FrameGlobals::Capture: every frame-globals contribution's floats into a_out (kFrameGlobalsFloats, zeroed). */
	void CaptureFrameGlobals(std::array<float, kFrameGlobalsFloats>& a_out);
}
