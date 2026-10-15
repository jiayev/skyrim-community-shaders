// The views' entries (see the header): Core's own part of each view, the features' contributions and work over it, and what the
// per-object paths read of them.

// Internal.h first: volk must precede every Vulkan header of the translation unit.
#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Views.h"

#include <bit>

namespace DCLF::Extensions
{
	namespace
	{
		// The work Core keeps at the views' points until their features take it, where FrameDriver and the depth hook called it.
		void PrepareReflection(const Api::ViewPointContext&) { IndirectDraws::Get().PrepareReflection(); }
		void ExecuteReflection(const Api::ViewPointContext&) { IndirectDraws::Get().ExecuteReflection(); }

		ViewEntry& EntryOf(ViewSet& a_set, Api::EngineView a_view) { return a_set.views[static_cast<std::size_t>(a_view)]; }
		auto& WorkAt(ViewEntry& a_entry, Api::ViewPoint a_point) { return a_entry.work[static_cast<std::size_t>(a_point)]; }

		/** @brief A shape into an entry, its strings copied (PointShape points the shape's views at them once the entry is in place). */
		void SetShape(ViewEntry& a_entry, const Api::ViewShape& a_shape)
		{
			a_entry.shape = a_shape;
			a_entry.name = a_shape.name;
			a_entry.prefix = a_shape.prefix;
			a_entry.blocks = a_shape.blocks;
			a_entry.target = a_shape.target;
			a_entry.labels = { std::string(a_shape.labels.map), std::string(a_shape.labels.tables), std::string(a_shape.labels.latches),
				std::string(a_shape.labels.blocks), std::string(a_shape.labels.trees) };
		}

		void PointShape(ViewEntry& a_entry)
		{
			auto& shape = a_entry.shape;
			shape.name = a_entry.name;
			shape.prefix = a_entry.prefix;
			shape.blocks = a_entry.blocks;
			shape.target = a_entry.target;
			shape.labels = { a_entry.labels[0], a_entry.labels[1], a_entry.labels[2], a_entry.labels[3], a_entry.labels[4] };
		}

		/** @brief What Core keeps of each view: the reflection whole. */
		void FillCoreEntries(ViewSet& a_set)
		{
			auto& reflection = EntryOf(a_set, Api::EngineView::Reflection);
			reflection.coreToggle = &ToggleSet::reflections;
			reflection.phase = kSetReflection;
			reflection.viewBit = Draws::kViewReflection;
			reflection.membership = Api::ViewMembership::LodTechniques;
			// The LOD techniques' slots only, front faces culled unless two-sided (dclf-lod.md, "Water reflections").
			reflection.keys = { .lodTechniquesOnly = true };
			SetShape(reflection, { .faces = Draws::kReflectionFaces, .trees = true, .viewBits = Draws::kViewReflection, .name = "reflection",
									 .prefix = "cs.dclf.reflection", .blocks = "face-blocks", .target = "cube",
									 .labels = { "reflection bucket map", "reflection bucket tables", "reflection culling latches", "reflection face per-frame data",
										 "reflection tree rows" } });
			reflection.life = Api::ViewShapeLife::KeepLast;
			reflection.area = Api::ViewArea::Target;
			reflection.withholding = Api::ViewWithholding::OwnRenderers;
			reflection.targetImport = Api::ViewTargetImport::ColourWithOwnDepth;
			// Covered from the start (ReflectionState) and on a failed graph (CoverFailedGraph).
			reflection.coveredInitially = reflection.coveredOnFailedGraph = true;
			WorkAt(reflection, Api::ViewPoint::FrameStartAfterTreeLod).push_back(&PrepareReflection);
			WorkAt(reflection, Api::ViewPoint::DepthCommitted).push_back(&ExecuteReflection);
		}

		/** @brief A feature's contribution into its view's entry (Registry refused the views Core keeps and incomplete ones). */
		void FillRegistered(ViewEntry& a_entry, const Api::ViewContribution& a_view)
		{
			a_entry.registered = true;
			a_entry.toggle = a_view.toggle;
			a_entry.forwardDecals = a_view.forwardDecals;
			a_entry.phase = a_view.phase;
			a_entry.viewBit = a_view.shape.viewBits;
			a_entry.membership = a_view.membership;
			a_entry.keys = a_view.keys;
			SetShape(a_entry, a_view.shape);
			a_entry.life = a_view.life;
			a_entry.area = a_view.area;
			a_entry.withholding = a_view.withholding;
			a_entry.targetImport = a_view.targetImport;
			a_entry.coveredInitially = a_view.coveredInitially;
			a_entry.coveredOnFailedGraph = a_view.coveredOnFailedGraph;
			a_entry.covered = a_view.covered;
			a_entry.captureRoot = a_view.captureRoot;
			a_entry.accumulators = a_view.accumulators;
		}

		/** @brief Each registered view's phase and view bit: one bit, none Core's (its phases and view bits) nor another view's. */
		void CheckViewBits(const ViewSet& a_set)
		{
			std::uint32_t phases = kCoreSetPhases, viewBits = Draws::kCoreViewBits;
			for (const auto& entry : a_set.views) {
				if (entry.registered)
					continue;
				phases |= entry.phase;
				viewBits |= entry.viewBit;
			}
			for (std::size_t v = 0; v < a_set.views.size(); ++v) {
				const auto& entry = a_set.views[v];
				if (!entry.registered)
					continue;
				if (!std::has_single_bit(static_cast<std::uint32_t>(entry.phase)) || (entry.phase & phases) || !std::has_single_bit(entry.viewBit) || (entry.viewBit & viewBits))
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: view {} has a set phase ({:#x}) or view bit ({:#x}) that is none, more than one, or Core's or another view's",
						v, entry.phase, entry.viewBit));
				phases |= entry.phase;
				viewBits |= entry.viewBit;
			}
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
				const bool root = entry.membership == Api::ViewMembership::CategoryRoot;
				a_set.phases |= entry.phase;
				(root ? a_set.rootPhases : a_set.lodPhases) |= entry.phase;
				a_set.viewBits |= entry.viewBit;
				a_set.rootViewBits |= root ? entry.viewBit : 0u;
				AddPhaseBits(a_set, entry.phase, entry.viewBit);
			}
			for (std::uint32_t mask = 0; mask < a_set.viewBitsOfPhases.size(); ++mask)
				for (std::uint32_t b = 0; b < a_set.viewBitsOfPhaseBit.size(); ++b)
					if (mask & (1u << b))
						a_set.viewBitsOfPhases[mask] |= a_set.viewBitsOfPhaseBit[b];
		}

		/** @brief The root views, the set decision's order (the CategoryRoot views first) and the InMainRenderers views, in slot order. */
		void DeriveOrders(ViewSet& a_set)
		{
			for (std::size_t v = 0; v < a_set.views.size(); ++v) {
				const auto& entry = a_set.views[v];
				const auto view = static_cast<Api::EngineView>(v);
				if (!entry.phase)
					continue;
				if (entry.membership == Api::ViewMembership::CategoryRoot) {
					a_set.rootViews[a_set.rootCount++] = view;
					a_set.decideOrder[a_set.decideCount++] = view;
				}
				if (entry.withholding == Api::ViewWithholding::InMainRenderers)
					a_set.mainRendererViews[a_set.mainRendererCount++] = view;
			}
			for (std::size_t v = 0; v < a_set.views.size(); ++v)
				if (a_set.views[v].phase && a_set.views[v].membership != Api::ViewMembership::CategoryRoot)
					a_set.decideOrder[a_set.decideCount++] = static_cast<Api::EngineView>(v);
		}
	}

	bool CoreKeepsView(Api::EngineView a_view) { return a_view == Api::EngineView::Reflection; }

	void ResolveViews(std::span<const Api::ViewContribution, kEngineViews> a_views, std::span<const Api::ViewWork> a_work)
	{
		static bool once = false;
		if (std::exchange(once, true))
			stl::report_and_fail("Drawcall Limit Fix: the views were resolved twice");
		ViewSet set;
		FillCoreEntries(set);
		for (const auto& view : a_views)
			if (view.view < Api::EngineView::Count)
				FillRegistered(EntryOf(set, view.view), view);
		// The features' work after Core's, in registration order.
		for (const auto& work : a_work)
			WorkAt(EntryOf(set, work.view), work.point).push_back(work.run);
		CheckViewBits(set);
		DeriveViewBits(set);
		DeriveOrders(set);
		Detail::resolvedViews = std::move(set);
		// The shapes' views into the strings where they now stand.
		for (auto& entry : Detail::resolvedViews.views)
			PointShape(entry);
	}
}
