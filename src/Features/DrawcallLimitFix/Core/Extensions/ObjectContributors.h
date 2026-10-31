#pragma once

// The object contributor contracts' Core side (Api/ObjectContributors.h): the technique contributions by technique (the Lighting port's
// eligibility and the joins' record flags read them as data) and their property flag rules, the geometry type, parent and object class
// contributions' toggles and rules (the eligibility, the walk, the captures, the classification, the plans and the category capture read
// them as data: a type's property flags, item class, movers, entry class and range capture, a class's category root), the owner classes
// by form type (OwnerClassOf), what a shading
// contribution is handed and the feature block's rule, and the calls the Lighting port's shading sample and the frame globals' capture make.
// And the Lighting join rules' (section 5): what their callbacks are handed (LightingJoinView), the calls the captures, joins and derives
// make (ReferenceFactsOf, CapturePropertyBits, LightingObjectFlagsOf, LightingKeyBitsOf, LightingKeyPermutation, DeriveLightingPass), and
// the data the per-frame readers take (LightingRecordFlagsOf, LightingPixelDropped, LightingRawTechnique, GeometryExtraName).

#include "Features/DrawcallLimitFix/Api/FrameValues.h"
#include "Features/DrawcallLimitFix/Api/ObjectContributors.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ClassifyInputs.h"
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"
#include "Features/DrawcallLimitFix/Core/Engine/Capture/SceneCapture.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace RE
{
	class BSShaderProperty;
	class NiNode;
	class TESObjectREFR;
}

namespace DCLF::Api
{
	/**
	 * @brief A shading contribution's derive: the property sampled, and the feature block it writes (zero-filled before; the shading row
	 * keeps its contribution's blockWords of it: BindlessShading::featureBlock).
	 */
	class ShadingDerivation
	{
	public:
		const RE::BSShaderProperty& property;
		std::span<float, kShadingBlockWords> block;
	};

	/**
	 * @brief What a Lighting join rule reads of an object (section 5): its records (the property's, the leaf's), and the contributor facts
	 * its join takes from them (the caller fills them from the records: the geometry's declared extra value, its node's reference facts, its
	 * property's contributor bits). Built per join (the join pool) and per derive; read-only.
	 */
	class LightingJoinView
	{
	public:
		const SceneCapture::PropertyRecord& property;  // the Lighting property the object draws with
		const SceneCapture::LeafView& leaf;            // its leaf's records (alpha, geometry, node, the leaf's own property)
		std::int64_t geometryExtra = SceneCapture::GeometryRecord::kNoExtra;  // DeclareGeometryExtra's value (kNoExtra, kWrongExtra)
		std::uint32_t referenceFacts = 0;  // the node record's facts (a bit by ReferenceFactId)
		std::uint8_t contributorBits = 0;  // the property record's contributor bits (PropertyBits)

		std::uint64_t PropertyFlags() const { return property.flags; }  // BSShaderProperty::EShaderPropertyFlag bits
		float PropertyAlpha() const { return property.alpha; }
		std::uint32_t MaterialFeature() const { return property.feature; }  // BSShaderMaterial::Feature
		bool HasMaterial() const { return property.material != nullptr; }
		bool Fact(ReferenceFactId a_fact) const { return a_fact.index < 32 && ((referenceFacts >> a_fact.index) & 1u); }
		bool Skinned() const { return leaf.Skinned(); }
	};
}

namespace DCLF::Extensions
{
	/** @brief The Lighting techniques a contribution may name (SIE::ShaderCache::LightingShaderTechniques' values below it). */
	inline constexpr std::uint32_t kTechniqueSlots = 32;
	/** @brief The floats the frame globals hold for the features' contributions (FrameGlobals::features): grow it before more are registered. */
	inline constexpr std::uint32_t kFrameGlobalsFloats = 8;

	/**
	 * @brief A technique's contribution (none: no toggle, no flags), with its gate (Api::TechniqueGate) resolved into it (gateNeeds none:
	 * no gate).
	 */
	struct TechniqueEntry
	{
		Api::ToggleId toggle;
		std::uint32_t objectFlags = 0;
		Api::ObjectExtras extras = Api::ObjectExtras::None;
		Api::ClassifyInputId gateWhile;
		Api::ToggleId gateNeeds;
	};
	using TechniqueEntries = std::array<TechniqueEntry, kTechniqueSlots>;

	/** @brief Any thread, after InstallFeatures (all none before): the technique contributions, by technique. */
	const TechniqueEntries& RegisteredTechniques();

	/** @brief The record flags a_technique's objects carry (its contribution's; 0 for none). */
	inline std::uint32_t TechniqueObjectFlags(const TechniqueEntries& a_techniques, std::uint32_t a_technique)
	{
		return a_technique < a_techniques.size() ? a_techniques[a_technique].objectFlags : 0u;
	}

	/**
	 * @brief Whether a_technique's objects are DCLF's under a_toggles: its contribution's toggle on and, while its gate's input holds (the
	 * scene pass's snapshot: ClassifyInputOn), its gate's toggle too; false for none.
	 */
	inline bool TechniqueOn(const TechniqueEntries& a_techniques, const ToggleSet& a_toggles, std::uint32_t a_technique)
	{
		if (a_technique >= a_techniques.size())
			return false;
		const auto& entry = a_techniques[a_technique];
		return entry.toggle && a_toggles.On(entry.toggle) && (!entry.gateNeeds || !ClassifyInputOn(entry.gateWhile) || a_toggles.On(entry.gateNeeds));
	}

	/** @brief Core's extras rows a_technique's objects take (its contribution's; None for none), per join. */
	inline Api::ObjectExtras TechniqueExtras(std::uint32_t a_technique)
	{
		const auto& techniques = RegisteredTechniques();
		return a_technique < techniques.size() ? techniques[a_technique].extras : Api::ObjectExtras::None;
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
	 * @brief An owner class as the walk keeps it (Api::EntryClassContribution: its class and rules; ownerClass None, every rule false, for
	 * none). Tracked holds a copy, resolved once.
	 */
	struct OwnerClassRule
	{
		Api::EntryClass ownerClass = Api::EntryClass::None;
		bool everyFrame = false;
		bool ownMotion = false;
		bool published = false;
	};
	/** @brief The owner classes by their entries' reference's form type (RE::FormType: every value of the byte). */
	using OwnerClasses = std::array<OwnerClassRule, 256>;
	/** @brief Any thread, after InstallFeatures (all none before): the owner classes, by form type. */
	const OwnerClasses& RegisteredOwnerClasses();

	/** @brief The owner class of a reference of form type a_formType (none for none). */
	inline const OwnerClassRule& OwnerClassOfForm(std::uint8_t a_formType)
	{
		return RegisteredOwnerClasses()[a_formType];
	}

	/** @brief The owner class of a mirrored node's reference (its userData and formType; none without a reference). */
	inline const OwnerClassRule& OwnerClassOf(const SceneCapture::NodeRecord& a_record)
	{
		static constexpr OwnerClassRule kNone{};
		return a_record.userData ? OwnerClassOfForm(a_record.formType) : kNone;
	}

	/**
	 * @brief What the walk's timing report calls the owner classes' entries (EvaluateKind::Owned, OwnedNoRecord: the registered names,
	 * "<name>" and "<name> without a record"; "owned" while none is registered).
	 */
	std::string_view OwnedEntriesLabel(bool a_withoutRecord);

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
	 * deriving a_block (left as it is by none).
	 */
	void DeriveShading(const RE::BSShaderProperty& a_property, std::span<float, Api::kShadingBlockWords> a_block);
	/**
	 * @brief The feature block's rule (the shading contribution claiming it: Api::ShadingContribution's objectFlags, blockWords and
	 * vsVariables, and a bit per variable), none (no flags, no words) when no feature does.
	 */
	struct ShadingBlock
	{
		std::uint32_t objectFlags = 0;
		std::uint32_t blockWords = 0;
		std::span<const Api::ShadingVariable> variables;
		std::uint64_t vsMask = 0;
	};
	/** @brief Any thread, after InstallFeatures: the rule, resolved once (the features installed before any scene work). */
	const ShadingBlock& ShadingBlockRule();
	/** @brief Render thread, FrameGlobals::Capture: every frame-globals contribution's floats into a_out (kFrameGlobalsFloats, zeroed). */
	void CaptureFrameGlobals(std::array<float, kFrameGlobalsFloats>& a_out);

	// ---- Section 5: the Lighting join rules, as registered (Api::InstallContext::RegisterReferenceFact, DeclareGeometryExtra,
	// RegisterPropertyBits, RegisterLightingObjectFlag, RegisterLightingKeyBits, RegisterLightingPassRules).

	/** @brief The reference facts there may be (SceneCapture::NodeRecord's facts bits): grow it before more are registered. */
	inline constexpr std::size_t kReferenceFactSlots = 4;
	/** @brief The property bits registrations there may be (SceneCapture::PropertyRecord's 8 contributor bits between them). */
	inline constexpr std::size_t kPropertyBitsSlots = 4;
	/** @brief The Lighting object flags there may be (the record flags' pairs, read per record): grow it before more are registered. */
	inline constexpr std::size_t kLightingObjectFlagSlots = 2;
	/** @brief The pass rules' derivePass callbacks there may be (called per derive). */
	inline constexpr std::size_t kLightingPassDeriveSlots = 2;
	/** @brief The pass descriptor bits the engine's GetPixelTechnique drops (3-8: the light counts); a pass rule's pixelKeep keeps some back. */
	inline constexpr std::uint32_t kLightingPixelSplitBits = 0x1f8u;
	/** @brief The Lighting key's spare bits (Records.h, kRasterLightingSpareShift: 3 bits): LightingKeyBits' keyBits are below 1 << this. */
	inline constexpr std::uint32_t kLightingKeySpareBits = 3;

	/** @brief A registered reference fact (its name owned). */
	struct ReferenceFactEntry
	{
		std::string name;
		bool none = true;
		decltype(Api::ReferenceFact::sample) sample = nullptr;
	};
	/** @brief A registered property bits capture (its name owned). */
	struct PropertyBitsEntry
	{
		std::string name;
		std::uint8_t bits = 0;
		decltype(Api::PropertyBits::capture) capture = nullptr;
	};
	/** @brief The pass rules' masks, ORed over the registrations (Api::LightingPassRules'; none: all 0). */
	struct LightingPassMasks
	{
		std::uint32_t pixelKeep = 0;
		std::uint32_t rawWhen = 0, rawSet = 0, rawFlip = 0;
	};
	/** @brief Every Lighting join rule registered, written only during InstallFeatures (each registration in place), read-only after. */
	struct LightingJoinRules
	{
		std::array<ReferenceFactEntry, kReferenceFactSlots> facts{};  // by ReferenceFactId::index
		std::uint32_t factCount = 0;
		std::uint32_t factsNone = 0;  // bit f: fact f's none
		std::string geometryExtra;    // the declared extra data's name (empty: none)
		std::array<PropertyBitsEntry, kPropertyBitsSlots> propertyBits{};
		std::uint32_t propertyBitsCount = 0;
		std::uint8_t propertyBitsTaken = 0;
		std::array<Api::LightingObjectFlag, kLightingObjectFlagSlots> objectFlags{};
		std::uint32_t objectFlagCount = 0;
		Api::LightingKeyBits keyBits{};  // keyBits null: none
		LightingPassMasks passMasks{};
		std::array<decltype(Api::LightingPassRules::derivePass), kLightingPassDeriveSlots> derives{};
		std::uint32_t deriveCount = 0;
	};
	/** @brief Any thread, after InstallFeatures (all none before): the Lighting join rules. */
	const LightingJoinRules& RegisteredLightingJoinRules();

	/**
	 * @brief The node capture (SceneCapture's node record, the capturing thread, inside the engine-read window): every reference fact of
	 * a_reference (null: none of them, each its none), a bit each.
	 */
	std::uint32_t ReferenceFactsOf(const RE::TESObjectREFR* a_reference);
	/** @brief The facts of a node without a reference (each fact's none): a node record's default. */
	inline std::uint32_t ReferenceFactsNone() { return RegisteredLightingJoinRules().factsNone; }
	/** @brief The geometry extra data's name the geometry capture reads (GeometryRecord's extra value; empty: none, every value kNoExtra). */
	inline std::string_view GeometryExtraName() { return RegisteredLightingJoinRules().geometryExtra; }
	/** @brief The Lighting property capture (with a material): every property bits capture's bits of a_property, within its own. */
	std::uint8_t CapturePropertyBits(const RE::BSShaderProperty& a_property);
	/** @brief Per join (the join pool): the object flags whose applies holds for a_view (0 for none registered). */
	std::uint32_t LightingObjectFlagsOf(const Api::LightingJoinView& a_view);
	/** @brief Per record (data): the record flags of a_objectFlags' Lighting object flags (each registration's pair). */
	inline std::uint32_t LightingRecordFlagsOf(std::uint32_t a_objectFlags)
	{
		const auto& rules = RegisteredLightingJoinRules();
		std::uint32_t flags = 0;
		for (std::uint32_t i = 0; i < rules.objectFlagCount; ++i)
			flags |= (a_objectFlags & rules.objectFlags[i].objectFlag) ? rules.objectFlags[i].recordFlag : 0u;
		return flags;
	}
	/** @brief Per join (the join pool): the Lighting key's spare bits for a_view (the claimant's keyBits, below 8; 0 without one). */
	std::uint32_t LightingKeyBitsOf(const Api::LightingJoinView& a_view);
	/** @brief Per new pipeline key: the permutation's extraFeatureDescriptor for a key with a_keyBits (the claimant's; 0 without one). */
	std::uint32_t LightingKeyPermutation(std::uint32_t a_keyBits);
	/** @brief The pass rules' masks (data, per frame per pipeline: the pixel split, the raw technique). */
	inline const LightingPassMasks& LightingPassRule() { return RegisteredLightingJoinRules().passMasks; }
	/** @brief The pixel split's dropped bits (kLightingPixelSplitBits less the rules' pixelKeep): PixelDescriptorFromPass clears them. */
	inline std::uint32_t LightingPixelDropped() { return kLightingPixelSplitBits & ~LightingPassRule().pixelKeep; }
	/** @brief SetupGeometry's raw technique a_raw with the pass rule applied: with any of rawWhen it gains rawSet, then flips rawFlip. */
	inline std::uint32_t LightingRawTechnique(std::uint32_t a_raw)
	{
		const auto& rule = LightingPassRule();
		if (a_raw & rule.rawWhen) {
			a_raw |= rule.rawSet;
			a_raw ^= rule.rawFlip;
		}
		return a_raw;
	}
	/** @brief Per derive (Core's Lighting descriptors): a_pass after every derivePass, in registration order (a_pass for none). */
	std::uint32_t DeriveLightingPass(std::uint32_t a_pass, const Api::LightingJoinView& a_view);
}
