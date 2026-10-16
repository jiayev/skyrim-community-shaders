#pragma once

// The main range contract (Api::InstallContext::RegisterMainRange): a feature's ordered range of the colour segment's second pass, drawn
// after every opaque draw at Core's fixed slot for it in the main sequence buffer. The slots, their count words and the object flag bits
// that select them are BuildDrawsCS's compile-time layout (GpuLayouts.h: MainRangeCountWord; Features/Manifest.cpp checks a feature's
// slots against it); what a registration names is which slot is drawn, where in the pass, under which nvperf range, and whether the slot
// also draws a depth prepass before the main colour draws. And the held group contract (RegisterHeldGroup), which registers an appended
// slot's range with the objects that fill it, and the near twin contract (RegisterNearTwin), its twin's target.

#include "Features/DrawcallLimitFix/Api/ClassifyInputs.h"
#include "Features/DrawcallLimitFix/Api/ObjectContributors.h"
#include "Features/DrawcallLimitFix/Api/Settings.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace DCLF::Api
{
	/**
	 * @brief Core's fixed slot per range (GpuLayouts.h). A fixed slot's objects take an ordinal each (SceneTables::rangeOrdinal, written
	 * by the slot's feature, with SceneTables::rangeCount its slots): the colour epoch writes each one's draw, or a zero-count one, to its
	 * ordinal, so the range keeps the feature's order. An appended slot's draws are appended by the culling (no order within it; it reuses
	 * phase 2's range once the depth segment is done with it).
	 */
	enum class MainRangeSlot : std::uint8_t
	{
		Fixed0,     // the objects of decal group 1 (kObjectDecal, group bits 1)
		Fixed1,     // decal group 2
		Fixed2,     // decal group 3
		Appended0,  // terrain group 1 (no kObjectDecal, group bits 1)
		Appended1,  // terrain group 2
		Count,
	};
	inline constexpr std::size_t kFixedMainRanges = static_cast<std::size_t>(MainRangeSlot::Appended0);

	/**
	 * @brief A main range: drawn in the second pass by ascending order (every registration's order its own), each from its slot's range
	 * and count word, under the nvperf range "cs.dclf.colour.<name>" (nested in "cs.dclf.colour.decals"). Read as data per frame (no call).
	 *
	 * - slot: the slot, one registration's.
	 * - name: its nvperf range's suffix.
	 * - order: its place in the second pass.
	 * - depthPrepass: a fixed slot only: its draws also go first, depth only (the depth variants), in a pass of their own before the main
	 *   colour draws ("cs.dclf.colour.decal-depth"), so a host fragment under an opaque texel fails the colour pass's EQUAL test.
	 * - objectClass: the class whose objects fill it (registered before); Count for a held group's (RegisterHeldGroup registers it).
	 */
	struct MainRangeContribution
	{
		MainRangeSlot slot = MainRangeSlot::Count;
		std::string_view name;
		std::uint8_t order = 0;
		bool depthPrepass = false;
		ObjectClass objectClass = ObjectClass::Count;
	};

	/**
	 * @brief A held group (Api::InstallContext::RegisterHeldGroup): objects a CS feature takes out of the opaque pass and replays after it
	 * (Integrations/TerrainBlending: its terrain blocks, blended over what lies beneath, then the meshes it redraws testing EQUAL). DCLF
	 * draws them as it does, in an appended slot; the slot's state and depth test are Core's (as the decal groups' are). Read as data by the
	 * Lighting port's derive, per classification (Extensions::HeldGroups; no call).
	 *
	 * - heldWhile: the registered classify input under which the feature holds them; while it does not, they are ordinary objects.
	 * - slot: Appended0 or Appended1, registered as a main range here (name, order, objectClass Count: the refusals of RegisterMainRange).
	 * - name, order: the main range's.
	 * - propertyFlags: the Lighting property flags (BSShaderProperty::EShaderPropertyFlag bits) that mark its objects: the first group in
	 *   registration order whose flags an object's property has takes it.
	 * - toggle (optional): a registered toggle its objects need, Ineligible::Held while it is off; none: always DCLF's. A decal or a layer
	 *   (which has a group of its own) is always Ineligible::Held.
	 */
	struct HeldGroupContribution
	{
		ClassifyInputId heldWhile;
		MainRangeSlot slot = MainRangeSlot::Count;
		std::string_view name;
		std::uint8_t order = 0;
		std::uint64_t propertyFlags = 0;
		ToggleId toggle;
	};

	/**
	 * @brief A near twin (Api::InstallContext::RegisterNearTwin): Appended0's near objects drawn twice in the depth segment, offset into the
	 * main depth and into the feature's own depth target (Integrations/TerrainBlending: its near terrain blocks' depth, as its depth hook
	 * draws them). One registration.
	 *
	 * - name: what the scene resources' setup calls the target in its NotReady texts ("<name> cannot be described", "... could not be
	 *   imported": built once).
	 * - debugName: the imported image's name.
	 * - resource: its ORG resource identifier.
	 * - target: the scene resources' setup (render thread): the engine depth texture (ID3D11Texture2D*) to import, which the main depth's
	 *   description fits; null: no twin target (the feature not loaded).
	 * - nearTest: render thread, per depth commit: the near test's eye (xyz) and distance (w) into a_eyeDistance; false (a_eyeDistance
	 *   left as it is): no test this frame (w 0).
	 */
	struct NearTwinContribution
	{
		std::string_view name;
		std::string_view debugName;
		std::string_view resource;
		void* (*target)() = nullptr;
		bool (*nearTest)(float (&a_eyeDistance)[4]) = nullptr;
	};
}
