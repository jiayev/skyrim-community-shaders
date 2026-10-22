#include "Features/DrawcallLimitFix/Diagnostics/Draws/DrawsState.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Reports.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ShadowViews.h"
#include "Draws.h"
#include "Engine.h"

// IndirectDraws' report lines at the render thread's report (Reports::RenderThread): the async work, the views' epochs, the snapshots and
// revisions (the payload's pools are the snapshot builder's: Impl::ProducerReport).

namespace DCLF
{
	std::string IndirectDraws::AsyncReport()
	{
		std::string text;
		if (auto window = Reports::EngineReadWindowLine(); !window.empty())
			text += window + "\n";
		// The write-backs' lines here (Features/Fades: the fade write-back's and the fade writers').
		Extensions::ReportTextAt(Api::ReportPoint::AsyncWorkWriteBack, text);
		text += VisibilityReport();
		// The views' lines here, before the forward epoch's (Features/Reflection: the reflection faces').
		Extensions::ReportTextAt(Api::ReportPoint::AsyncWorkViews, text);
		text += SharedEpochReport();
		// The features' lines here (Features/Particles, Features/Billboards, Features/FirstPerson: the first-person view's).
		Extensions::ReportTextAt(Api::ReportPoint::AsyncWork, text);
		// The draw families' lines here (Features/TreeLod's draws and cull).
		Extensions::ReportTextAt(Api::ReportPoint::AsyncWorkFamilies, text);
		// The builds' stores (main rows, object records, shadow state, extras rows, geometry table, exclusions) are the snapshot builder's:
		// their lines are composed in its pass (Impl::StoresReport, with the producer's report) and printed with it, a report behind.
		if (auto line = Reports::FrameValuesLine(); !line.empty())
			text += line + "\n";
		if (auto line = Reports::FrameDataLine(); !line.empty())
			text += line + "\n";
		// The rows naming descriptors (ContentOwners): steady while the scene's bindings are (a growing count is a range never replaced).
		if (impl->scene) {
			const auto content = impl->scene->content.TakeCounts();
			text += fmt::format("[DCLF] content owners: {} ranges naming descriptors, {} owners; {} snapshots made since the last report\n", content.ranges,
				content.owners, content.snapshotsMade);
		}
		{
			text += impl->RevisionReport();
			// The snapshots (the frame's counters, the builder's times) and the builder's own lines (the revisions, the growths,
			// the shapes made, the draw bound), composed in its pass and taken here.
			text += impl->SnapshotReport();
		}
		static constexpr const char* kNames[3] = { "colour", "zprepass", "shadow" };
		for (std::size_t i = 0; i < stats.async.size(); ++i) {
			auto& a = stats.async[i];
			if (!a.used)
				continue;
			text += fmt::format("[DCLF] {} epochs: {} committed the payload built ahead with their publication\n", kNames[i], a.used);
			if (i == kAsyncZPrepass)
				text += fmt::format("[DCLF] frame slots supplied from an earlier capture (6e E5): {}\n", std::exchange(impl->frameSlotsCarried, 0));
			a = {};
		}
		if (auto& p = Diagnostics::DrawsState::Get().viewMasks; p.checks.load(std::memory_order_relaxed)) {
			// The main lists, the caster modes', then each occlusion map's (its entry's list label).
			static constexpr std::array<const char*, 2 + kFirstOcclusionMode> kLists{ "main", "main drawable", "plain", "clamped", "paraboloid" };
			const auto listName = [](std::size_t a_list) -> std::string_view {
				const std::size_t list = std::min<std::size_t>(a_list, 1 + kShadowModeCount);
				return list < kLists.size() ? std::string_view(kLists[list]) : std::string_view(Extensions::OcclusionMapOf(static_cast<std::uint32_t>(list - kLists.size())).list);
			};
			const auto first = p.first.exchange(~0ull);
			const auto differ = p.differ.exchange(0);
			text += fmt::format("[DCLF] view mask parity (U3): {} publications, {} inputs, {} with a mask other than their object's or outside their list's views{}{}\n",
				p.checks.exchange(0), p.inputs.exchange(0), differ, differ ? " <- VIEW MASK" : " <- OK",
				first == ~0ull ? std::string() :
								 fmt::format("; first: {} input of object {}, mask {:#x}, expected {:#x}", listName(first >> 56),
									 (first >> 24) & 0xFFFFFFFFu, p.firstMask.load(std::memory_order_relaxed), first & 0xFFFFFFu));
		}
		return text;
	}
}
