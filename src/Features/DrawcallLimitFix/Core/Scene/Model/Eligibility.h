#pragma once

// The eligibility rules: pure verdicts over the scene's records (SceneCapture::LeafView) and a tracked entry's verdict. The coordinator
// classifies with them (the walk, the joins, the candidates) and the observers recompute with them. No state; no engine read beyond the
// records passed in (ParentReason reads a live node's class, for the live chain walks).

#include <cstdint>

#include "Features/DrawcallLimitFix/Core/Extensions/ObjectContributors.h"
#include "Features/DrawcallLimitFix/Core/Engine/Capture/SceneCapture.h"
#include "Features/DrawcallLimitFix/Core/Handoff/ShadowViews.h"
#include "Features/DrawcallLimitFix/Core/Shading/LightingDescriptors.h"

namespace DCLF::Eligibility
{
	// What a node between a leaf and its category node makes of the leaf. A billboard turns to the camera in
	// the main cull (NiBillboardNode::OnVisible), after the scene walk read its world transform. A switch
	// node draws one child at a time: Switch marks the leaf for the per-frame test (SwitchSelects). An ordered
	// node orders its leaves' blended passes alone (OrderedLeaf), whose draw order does not survive being
	// drawn out of the native loop: UnsupportedParent marks the leaf for ClassifyFrame's test of its leaf.
	inline Ineligible ParentReason(RE::NiNode* a_node)
	{
		if (a_node->AsSwitchNode())
			return Ineligible::Switch;
		if (netimmerse_cast<RE::BSOrderedNode*>(a_node))
			return Ineligible::UnsupportedParent;
		if (netimmerse_cast<RE::NiBillboardNode*>(a_node))
			return Ineligible::Billboard;
		return Ineligible::None;
	}

	/** @brief ParentReason from a node's record (the capture's casts): the mirror's chain and subtree walks. */
	inline Ineligible ParentReasonOf(const SceneCapture::NodeRecord& a_record)
	{
		return (a_record.kind & SceneCapture::kKindSwitch)    ? Ineligible::Switch :
		       (a_record.kind & SceneCapture::kKindOrdered)   ? Ineligible::UnsupportedParent :
		       (a_record.kind & SceneCapture::kKindBillboard) ? Ineligible::Billboard :
		                                                         Ineligible::None;
	}

	// The stronger of two parent reasons: a billboard (whatever its leaf) outranks a switch (decided per frame), which
	// outranks an ordered node (decided by the leaf). ClassifyFrame's chain walk finds an ordered node under a
	// switch's verdict itself.
	inline Ineligible CombineParentReasons(Ineligible a_lhs, Ineligible a_rhs)
	{
		for (const auto reason : { Ineligible::Billboard, Ineligible::Switch, Ineligible::UnsupportedParent })
			if (a_lhs == reason || a_rhs == reason)
				return reason;
		return Ineligible::None;
	}

	/**
	 * @brief Whether a BSOrderedNode above a_leaf orders its draws (UnsupportedParent): its passes are blended. The node's
	 * OnVisible (0x141503970) opens a new group of the culling process (+0x301f0) and culls its children into it (AppendVirtual);
	 * the list process registers each group's geometries with the group (ProcessJob, 0x140e28af0: the accumulator's vfuncs 0x140
	 * and 0x148 around FUN_1414b2140), and the main modes' registration (FUN_1414b2330) gives the group only to a hint 1 pass
	 * (blended: FUN_1414f50b0 into the group's list). Every other pass (an opaque one's hint 0 through RegisterPass, the decal
	 * groups) goes where it would under any node, so an opaque leaf is drawn in the normal opaque path and is DCLF's like any.
	 * A blended leaf AlphaBlend's static verdict rejects first; what passes it (a blended decal; blended Lighting) stays.
	 */
	inline bool OrderedLeaf(const SceneCapture::LeafView& a_leaf)
	{
		const auto* lighting = SceneCapture::LeafView::Lighting(a_leaf.property);
		return a_leaf.AlphaBlending() || (lighting && lighting->materialAlpha < 1.0f);
	}

	/**
	 * @brief Whether an ineligibility verdict is one the scene phase cannot reach on its own.
	 *
	 * The decal rule is the only one: the group a decal draws in comes from its accumulated pass's
	 * accumulation hint (LightingDescriptors, the engine's GetRenderPasses), which does not exist
	 * before the shadow maps. Rejecting on it in the scene phase would leave every decal the main
	 * pass draws without a record, so the record is built and the verdict is taken again, with the
	 * pass, by the accumulate phase.
	 */
	inline bool DeferredToAccumulate(Ineligible a_reason)
	{
		return a_reason == Ineligible::Decal;
	}

	/**
	 * @brief Whether an object the main pass cannot take is still a shadow caster DCLF can draw: its reason is
	 * one the shadow views are indifferent to, and the engine would draw it into a shadow map. Per reason:
	 * - Technique: the lighting technique is outside the main pass's set (the terrain's landscape blocks);
	 *   a shadow view draws the Utility technique, which ShadowUtilityTechnique derives from the property.
	 * - UnsupportedParent: a blended leaf under a BSOrderedNode (OrderedLeaf), which orders blended draws and nothing else
	 *   (an opaque one is main-phase eligible).
	 * - MultiIndex: a BSMultiIndexTriShape, whose shadow passes draw as a tri-shape's (ClassifyStatic).
	 * Not a billboard: NiBillboardNode turns to the culling camera, which for a shadow view is the light's.
	 */
	inline bool ShadowOnlyReason(Ineligible a_reason)
	{
		return a_reason == Ineligible::Technique || a_reason == Ineligible::UnsupportedParent || a_reason == Ineligible::MultiIndex;
	}

	inline bool ShadowOnlyCaster(Ineligible a_reason, const SceneCapture::LeafView& a_leaf)
	{
		if (!ShadowOnlyReason(a_reason))
			return false;
		const auto reject = ShadowCasterReject(a_leaf);
		return reject == ShadowReject::None || reject == ShadowReject::VolumetricOnly;
	}

	/** @brief Whether a negative verdict follows only from what the pointer witnesses cover. */
	bool Cacheable(Ineligible a_reason);
	/**
	 * @brief Static eligibility of a geometry, without the per-frame checks: from its records (the mirror's leaf on the scene
	 * work, SceneCapture::LiveLeaf's on the render thread).
	 */
	Ineligible Static(const SceneCapture::LeafView& a_leaf, LightingDescriptors* a_descriptors, const AccumulatedPass* a_accumulated = nullptr);
	/**
	 * @brief Static eligibility of a geometry's main-pass layer (LayerPropertyOf): its descriptors as the layer's pass draws
	 * them (decal group 3), with a_accumulated the layer's membership pass, or null.
	 */
	Ineligible Layer(const SceneCapture::LeafView& a_leaf, LightingDescriptors* a_descriptors, const AccumulatedPass* a_accumulated);

	/** @brief A tracked entry's verdict as the candidate rules read it. */
	struct EntryVerdict
	{
		bool record = false;      // it holds an object slot
		bool classified = false;  // a classification ran (Tracked::candidateFrame != 0)
		Ineligible reason = Ineligible::None;  // that classification's (Tracked::candidateReason)
	};

	/** @brief A sun candidate geometry row's primary verdict (EntrySpace::primaryGeometry; PrimaryCull::PlanOf reads it). */
	inline constexpr std::uint8_t kPrimaryEngine = 0, kPrimaryMember = 1, kPrimaryHiddenStatic = 2;

	/**
	 * @brief A leaf of no per-frame owner class (Api::EntryClassContribution::everyFrame: an actor's): hidden, its hidden bit changes by its
	 * events alone (an actor's parts are shown and hidden by its behaviour graph).
	 */
	inline bool HiddenStatic(const SceneCapture::LeafView& a_leaf) { return !(a_leaf.node && Extensions::OwnerClassOf(*a_leaf.node).everyFrame); }

	/**
	 * @brief Whether a tracked geometry lets its sun entry leave the cascade culls (the coordinator's UpdateSunCandidates). a_leaf() gives the
	 * entry's leaf, called only where a rule reads it.
	 */
	template <class Leaf>
	bool Sun(const EntryVerdict& a_entry, bool a_switchNodes, Leaf&& a_leaf)
	{
		// A table object: its shadow is the shadow epoch's (the set decides, per frame), or the caster rule rejects it.
		if (a_entry.record)
			return true;
		if (!a_entry.classified)
			return false;
		// The verdicts are the snapshot's; the exclusion is a promise about the culls that run a frame or two later (as BuildSunExclusion
		// counts a faded record a caster). A fading non-member blocks its entry: its fade advances only in the culls the exclusion would
		// skip. A hidden part of an actor blocks it too: the behaviour graph shows and hides an actor's parts during Main::Draw (a
		// shield's BShkVisibilityController, every 150-360 ms), and a shown part with no record would cast into no shadow until the next
		// candidates (a soldier's shield symbol in interiors).
		switch (a_entry.reason) {
		case Ineligible::NotLightingShader:  // no class but Lighting casts into the cascades (measured)
		case Ineligible::AlphaBlend:         // the caster rule: no pass for an alpha-blended property
			return true;
		case Ineligible::Hidden:  // the cull skips app-culled nodes
			return HiddenStatic(a_leaf());
		case Ineligible::Fading:
			return false;
		case Ineligible::Switch:  // an unselected child: the switch node culls only the selected one
			return a_switchNodes;
		default:
			// A property no shadow view takes, whatever the frame (refraction, for one: a fire's embers, a stream's surface).
			return CastsNoShadow(a_leaf());
		}
	}

	/**
	 * @brief Whether a tracked geometry lets its light entry leave the point lights' culls. A table object: the exclusion
	 * decides (BuildSunExclusion). Any other must give the light's registration no pass: what the sun's rule allows
	 * (Sun: hidden, alpha-blended, fading, an unselected switch child), or no Lighting property. The light's bit
	 * in its activeLightMask, which the registration writes on every geometry the cull reaches, is LocalLightCull's.
	 */
	template <class Leaf>
	bool Light(const EntryVerdict& a_entry, bool a_switchNodes, Leaf&& a_leaf)
	{
		return Sun(a_entry, a_switchNodes, a_leaf) || (a_entry.classified && !SceneCapture::LeafView::Lighting(a_leaf().property));
	}

	/**
	 * @brief Whether a tracked geometry lets its entry leave the primary's cull (PrimaryCull): a main-pass table
	 * object (a verdict of None) that is not a decal and not alpha-blended, whose main pass the synthetic pass
	 * reproduces. Anything the main pass draws natively, or that the cull decides per frame (hidden, fading, a
	 * switch child), keeps the entry in.
	 */
	bool Primary(const EntryVerdict& a_entry, const SceneCapture::LeafView& a_leaf);

	/**
	 * @brief The primary verdict a sun candidate's geometry row carries (EntrySpace::primaryGeometry): kPrimaryMember when Primary, kPrimaryHiddenStatic
	 * for a hidden static's geometry, which nobody draws while hidden and whose unhiding is an event
	 * that changes this verdict (so its entry is planned again), kPrimaryEngine otherwise (the engine's).
	 */
	template <class Leaf>
	std::uint8_t PrimaryVerdict(const EntryVerdict& a_entry, Leaf&& a_leaf)
	{
		if (a_entry.record && a_entry.classified && a_entry.reason == Ineligible::None && Primary(a_entry, a_leaf()))
			return kPrimaryMember;
		return a_entry.classified && a_entry.reason == Ineligible::Hidden && HiddenStatic(a_leaf()) ? kPrimaryHiddenStatic : kPrimaryEngine;
	}
}
