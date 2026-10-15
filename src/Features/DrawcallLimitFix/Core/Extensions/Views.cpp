// The views' entries (see the header): Core's own part of each view, the features' contributions and work over it, and what the
// per-object paths read of them.

// Internal.h first: volk must precede every Vulkan header of the translation unit.
#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Views.h"

namespace DCLF::Extensions
{
	namespace
	{
		// Written only by ResolveViews (the main thread, before DCLF's threads exist: their start orders these writes before every
		// read), read-only after.
		ViewSet resolved;

		// The work Core keeps at the views' points until their features take it, where FrameDriver and the depth hook called it.
		void BeginFirstPersonFrame(const Api::ViewPointContext&) { IndirectDraws::Get().BeginFirstPersonFrame(); }
		void PrepareReflection(const Api::ViewPointContext&) { IndirectDraws::Get().PrepareReflection(); }
		void ExecuteReflection(const Api::ViewPointContext&) { IndirectDraws::Get().ExecuteReflection(); }

		ViewEntry& EntryOf(ViewSet& a_set, Api::EngineView a_view) { return a_set.views[static_cast<std::size_t>(a_view)]; }
		auto& WorkAt(ViewEntry& a_entry, Api::ViewPoint a_point) { return a_entry.work[static_cast<std::size_t>(a_point)]; }

		/** @brief What Core keeps of each view: the reflection whole, the first person's set phase, view bit and shape choices. */
		void FillCoreEntries(ViewSet& a_set)
		{
			auto& reflection = EntryOf(a_set, Api::EngineView::Reflection);
			reflection.coreToggle = &ToggleSet::reflections;
			reflection.phase = kSetReflection;
			reflection.viewBit = Draws::kViewReflection;
			reflection.membership = Api::ViewMembership::LodTechniques;
			reflection.life = Api::ViewShapeLife::KeepLast;
			reflection.area = Api::ViewArea::Target;
			reflection.withholding = Api::ViewWithholding::OwnRenderers;
			reflection.targetImport = Api::ViewTargetImport::ColourWithOwnDepth;
			// Covered from the start (ReflectionState) and on a failed graph (CoverFailedGraph).
			reflection.coveredInitially = reflection.coveredOnFailedGraph = true;
			WorkAt(reflection, Api::ViewPoint::FrameStartAfterTreeLod).push_back(&PrepareReflection);
			WorkAt(reflection, Api::ViewPoint::DepthCommitted).push_back(&ExecuteReflection);

			auto& firstPerson = EntryOf(a_set, Api::EngineView::FirstPerson);
			firstPerson.phase = kSetFirstPerson;
			firstPerson.viewBit = Draws::kViewFirstPerson;
			firstPerson.membership = Api::ViewMembership::CategoryRoot;
			firstPerson.life = Api::ViewShapeLife::DropWhenOff;
			firstPerson.area = Api::ViewArea::MainColourViewport;
			firstPerson.withholding = Api::ViewWithholding::InMainRenderers;
			firstPerson.targetImport = Api::ViewTargetImport::DepthTarget;
			// Nothing of the first person's is withheld from a failed graph (the engine draws it).
			firstPerson.coveredInitially = firstPerson.coveredOnFailedGraph = false;
			WorkAt(firstPerson, Api::ViewPoint::FrameStartBeforeCoverage).push_back(&BeginFirstPersonFrame);
		}

		void AddPhaseBits(ViewSet& a_set, std::uint8_t a_phase, std::uint32_t a_viewBits)
		{
			for (std::uint32_t b = 0; b < a_set.viewBitsOfPhaseBit.size(); ++b)
				if (a_phase & (1u << b))
					a_set.viewBitsOfPhaseBit[b] |= a_viewBits;
		}

		/** @brief The set's phases and view bits from its entries, and the view bits by phase (Core's phases', then the views'). */
		void DeriveViewBits(ViewSet& a_set)
		{
			AddPhaseBits(a_set, kSetMain, Draws::kViewMain);
			AddPhaseBits(a_set, kSetCaster, Draws::kViewSunCaster | Draws::kViewSpotCaster);
			AddPhaseBits(a_set, kSetCasterPoint, Draws::kViewPointCaster);
			AddPhaseBits(a_set, kSetOccluderSky, Draws::kViewSkyOccluder);
			AddPhaseBits(a_set, kSetOccluderPrecipitation, Draws::kViewPrecipOccluder);
			for (const auto& entry : a_set.views) {
				a_set.phases |= entry.phase;
				(entry.membership == Api::ViewMembership::CategoryRoot ? a_set.rootPhases : a_set.lodPhases) |= entry.phase;
				a_set.viewBits |= entry.viewBit;
				AddPhaseBits(a_set, entry.phase, entry.viewBit);
			}
			for (std::uint32_t mask = 0; mask < a_set.viewBitsOfPhases.size(); ++mask)
				for (std::uint32_t b = 0; b < a_set.viewBitsOfPhaseBit.size(); ++b)
					if (mask & (1u << b))
						a_set.viewBitsOfPhases[mask] |= a_set.viewBitsOfPhaseBit[b];
		}
	}

	const ViewSet& Views() { return resolved; }

	bool CoreKeepsView(Api::EngineView a_view) { return a_view == Api::EngineView::Reflection; }

	void ResolveViews(std::span<const Api::ViewContribution, kEngineViews> a_views, std::span<const Api::ViewWork> a_work)
	{
		static bool once = false;
		if (std::exchange(once, true))
			stl::report_and_fail("Drawcall Limit Fix: the views were resolved twice");
		ViewSet set;
		FillCoreEntries(set);
		// A feature's contribution: its toggle and forward decals (Registry refused the views Core keeps).
		for (const auto& view : a_views) {
			if (view.view >= Api::EngineView::Count)
				continue;
			auto& entry = EntryOf(set, view.view);
			entry.registered = true;
			entry.toggle = view.toggle;
			entry.forwardDecals = view.forwardDecals;
		}
		// The features' work after Core's, in registration order.
		for (const auto& work : a_work)
			WorkAt(EntryOf(set, work.view), work.point).push_back(work.run);
		DeriveViewBits(set);
		resolved = std::move(set);
	}
}
