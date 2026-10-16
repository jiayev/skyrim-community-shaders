#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ObjectContributors.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Reports.h"
#include "Features/DrawcallLimitFix/Diagnostics/Draws/DrawsState.h"
#include "Engine.h"

// The views' epochs' report lines (taken by IndirectDraws::AsyncReport): the shared epochs' point, the accumulated visibility, the shadow
// capability. (The forward epoch's: Features/Forward; the reflection's and the first-person view's: Features/Reflection,
// Features/FirstPerson; the effects': Features/Effects.)

namespace DCLF
{
	std::string IndirectDraws::SharedEpochReport()
	{
		if (!impl)
			return {};
		// On a report where a shared epoch reached a frame since the last, or its gate class is on (the forward epoch's: its toggle).
		const ToggleSet toggles = ActiveToggles();
		bool due = false;
		for (std::uint32_t e = 0; e < EpochCount() && !due; ++e) {
			const auto& traits = EpochTraitsOf(e);
			if (traits.sharesWith == kNoEpoch || EpochTraitsOf(traits.sharesWith).shape != ShapeKind::Main)
				continue;
			due = impl->sharedEpochs[e].reached || (traits.gate != Api::ObjectClass::Count && Extensions::ObjectClassOn(toggles, traits.gate));
		}
		if (!due)
			return {};
		// Features/Forward's forward and refraction lines (taking the counters: TakeSharedEpochStats), then the forward epoch's features'
		// (Features/Effects: the Effect port's members and its Lighting variant's parity).
		std::string text;
		Extensions::ReportTextAt(Api::ReportPoint::AsyncWorkForward, text);
		return text;
	}

	SharedEpochStats IndirectDraws::TakeSharedEpochStats(std::uint32_t a_epoch)
	{
		if (!impl || a_epoch >= EpochCount())
			return {};
		auto& f = impl->sharedEpochs[a_epoch];
		const SharedEpochStats taken = f;
		f = {};
		f.submittedFrame = taken.submittedFrame;
		return taken;
	}

	std::string IndirectDraws::VisibilityReport()
	{
		using Readback = VisibilityReadback;
		if (!impl->scene || !impl->scene->visibilityReadback)
			return {};
		auto& r = *impl->scene->visibilityReadback;
		const auto take = [](std::atomic<std::uint64_t>& a_counter) { return a_counter.exchange(0, std::memory_order_relaxed); };
		// The writes (the normal path).
		std::string text = fmt::format(
			"[DCLF] accumulated writes (T7b2): {} write lists made at Main::Update's entry ({} full), {} set, {} cleared, {} already so, "
			"{} clears held by a camera cut ({} cuts, {} roots set by them); {:.1f} us on the main thread\n",
			take(r.writeBatches), take(r.fullWrites), take(r.writesSet), take(r.writesCleared), take(r.writesUnchanged), take(r.writesHeldByCut),
			take(r.cuts), take(r.cutWrites), double(take(r.writeNs)) / 1000.0);
		// Every listed root.
		{
			text += fmt::format(
				"[DCLF] accumulated, every listed root (T7f checklist item 1): wide writes {} set, {} cleared, {} already so; {} nodes between "
				"set ({} geometries whose walk missed their root); cell roots {} seen, {} set\n",
				take(r.wideSet), take(r.wideCleared), take(r.wideUnchanged), take(r.wideNodesSet), take(r.wideNoChain), take(r.cellRootsSeen), take(r.cellRootsSet));
			if (!r.wideNoChainFirst.empty())
				text += fmt::format("[DCLF] accumulated, geometries whose walk missed their root (T7f): {}\n", r.wideNoChainFirst);
			r.wideNoChainFirst.clear();
		}
		// The accumulated writes' features' lines here (Features/Actors: the actors' animation stamp).
		Extensions::ReportTextAt(Api::ReportPoint::AccumulatedWrites, text);
		if (!Diagnostics::Observe().accumulated)
			return text;
		text += Reports::AnimRateLine();
		struct Taken
		{
			std::uint64_t both, dclfOnly, engineOnly, neither;
		};
		const auto takeCounts = [&](Readback::Counts& a_counts) { return Taken{ take(a_counts.both), take(a_counts.dclfOnly), take(a_counts.engineOnly), take(a_counts.neither) }; };
		const auto roots = takeCounts(r.rootCounts), geometry = takeCounts(r.geometryCounts);
		const auto stoodRoots = takeCounts(r.stoodInRootCounts), stoodGeometry = takeCounts(r.stoodInGeometryCounts);
		const auto frames = take(r.framesCompared), incomplete = take(r.framesIncomplete), unmatched = take(r.samplesUnmatched), moved = take(r.moved);
		const auto staleGeometry = take(r.staleGeometry), noVerdictEngine = take(r.noVerdictEngine), noVerdictNeither = take(r.noVerdictNeither);
		const auto hidden = take(r.hiddenDclfOnly);
		const auto batches = take(r.batches), events = take(r.events), stale = take(r.stale), overflowed = take(r.overflowed), missed = take(r.missed);
		const auto setsAdded = take(r.setsAdded), busy = take(r.busy), decodeNs = take(r.decodeNs);
		std::uint32_t sets = 0, listEvents = 0;
		for (const auto* set = r.sets.load(std::memory_order_acquire); set; set = set->next) {
			++sets;
			listEvents = std::max(listEvents, set->events);
		}
		std::string firsts;
		static constexpr std::array<const char*, Readback::kCategories> kCategoryNames{ "roots DCLF's alone", "roots the engine's alone", "geometry DCLF's alone",
			"geometry the engine's alone" };
		for (std::size_t c = 0; c < Readback::kCategories; ++c) {
			if (!r.firstNames[c].empty())
				firsts += fmt::format("; first {}: {}", kCategoryNames[c], r.firstNames[c]);
			r.firstNames[c].clear();
		}
		const bool differ = roots.dclfOnly || roots.engineOnly || geometry.dclfOnly || geometry.engineOnly;
		return text + fmt::format(
			"[DCLF] accumulated (T7b1): {} frames compared ({} left out: a batch past its list; {} samples no batch reached; {} slots moved), engine-culled roots "
			"{} both / {} DCLF's alone / {} the engine's alone / {} neither, geometry {} / {} / {} / {} ({} of the engine's alone stale under a failed root); stood-in roots "
			"{} / {} / {} / {}, geometry {} / {} / {} / {}; aside: roots with no DCLF verdict {} set by the engine / {} not, {} DCLF's alone on an app-culled root; "
			"readback {} batches, {} events ({} stale), {} past their list, {} arms missed, {} staging sets (+{}), event list {}, {} wakes busy, decode {:.1f} us{}{}\n",
			frames, incomplete, unmatched, moved, roots.both, roots.dclfOnly, roots.engineOnly, roots.neither, geometry.both, geometry.dclfOnly, geometry.engineOnly, geometry.neither,
			staleGeometry, stoodRoots.both, stoodRoots.dclfOnly, stoodRoots.engineOnly, stoodRoots.neither, stoodGeometry.both, stoodGeometry.dclfOnly, stoodGeometry.engineOnly,
			stoodGeometry.neither, noVerdictEngine, noVerdictNeither, hidden, batches, events, stale, overflowed, missed, sets, setsAdded, listEvents, busy,
			double(decodeNs) / 1000.0, !frames ? " <- NOT COMPARED" : differ ? " <- ACCUMULATED DIFFER" : " <- OK", firsts);
	}

	std::string IndirectDraws::ShadowCapabilityReport() const
	{
		const auto& c = impl->shadowEngine->capabilityStats;
		return fmt::format("[DCLF] shadow capability: phases {:#x}, {} rasterizer states in the catalog; {} changes since startup, {} of them views it did not foresee{}{}",
			ShadowCapability(), DrawPipelines::Get().ShadowRasterStateCount(), c.changes, c.defects, c.defects ? " <- PHASES" : " <- OK",
			c.lastCause.empty() ? "" : " (last: " + c.lastCause + ")");
	}
}
