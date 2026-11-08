#pragma once

// The geometry only the engine draws in a view DCLF otherwise draws, as the one subset every such view's engine cull is limited to.
//
// Each tracked geometry with no record whose settled verdict leaves it to the engine is kept by the coordinator as it is classified and
// untracked (NoteEngineCaster), in the views it matters to:
// - Caster: it casts a shadow the engine would draw (Eligibility::EngineCasterReason and the caster rule): a skin the skinned path
//   refuses, a buffer DCLF cannot import. Fading and hidden parts are not counted: they settle within a few frames and their rules
//   already cover them. The shadow views' culls.
// - Colour: the engine would draw it in a colour view DCLF takes over by root (Eligibility::EngineColourReason), and it is under an
//   object class's category root (Api::ObjectClassContribution::categoryRoot: the LOD land root). The water reflection's LOD roots.
//
// The coordinator publishes the subset with each publication (PublishEngineCasters; the frame reads the installed one's,
// FrameView::GetEngineCasters). A view never culls the whole scene for it: the sun's whole skip hands its full-frustum cull the caster
// roots, the light culls walk down to them alone (Admit, through EngineOnlyCull's scope), and the reflection adds the colour roots under
// the LOD roots it skips. The members under a root are withheld at registration as always.

#include <array>
#include <cstdint>
#include <vector>

#include <ankerl/unordered_dense.h>

namespace RE
{
	class NiAVObject;
}

namespace DCLF::Scene
{
	/** @brief One publication's engine-only geometry, by the views it matters to. Immutable once published. */
	struct EngineOnlySubset
	{
		enum View : std::uint8_t
		{
			kCaster = 0,
			kColour = 1,
			kViewCount = 2
		};
		static constexpr std::uint8_t ViewBit(View a_view) { return static_cast<std::uint8_t>(1u << a_view); }

		// What a cull does with an object it reaches: cull it (a root: the engine's own cull of it and all below), descend (an ancestor of a
		// root: the engine's own cull, so its portal, room and bound tests stay on the path), or skip it.
		enum class Admit : std::uint8_t
		{
			Skip,
			Descend,
			Cull
		};

		/** @brief One view's roots. */
		struct Roots
		{
			// Each geometry's light entry (an actor's root) or sun entry while the coordinator owns it, else the geometry itself (a tracked
			// geometry, held until the retirement chain frees it). Either lives while the publication does. Deduplicated.
			std::vector<const RE::NiAVObject*> roots;
			// Per root, its geometry's category node (a key: the LOD land root for an LOD shape), or null.
			std::vector<const void*> category;
			// Keys only, never dereferenced: the roots, and every node from each one's parent up to the scene root (the mirror's chain).
			ankerl::unordered_dense::set<const void*> rootSet, ancestors;
		};

		std::array<Roots, kViewCount> views;
		std::uint32_t geometries = 0;     // the geometries under the roots
		std::uint32_t geometryRoots = 0;  // of the roots, geometries culled alone (no owned root above them)

		const Roots& Of(View a_view) const { return views[a_view]; }

		Admit AdmitObject(View a_view, const void* a_object) const
		{
			const auto& view = views[a_view];
			if (view.rootSet.contains(a_object))
				return Admit::Cull;
			return view.ancestors.contains(a_object) ? Admit::Descend : Admit::Skip;
		}
	};
}
