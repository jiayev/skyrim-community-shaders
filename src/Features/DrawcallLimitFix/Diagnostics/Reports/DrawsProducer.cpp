#include "Spread.h"
#include "Features/DrawcallLimitFix/Diagnostics/Draws/DrawsState.h"
#include "Features/DrawcallLimitFix/Features/Shadows/Local/ExclusionBuild.h"

// The scene revisions' and snapshots' report lines: the frame's (SnapshotReport, RevisionReport) and the snapshot builder's, composed in
// its pass and posted for the frame's report (ProducerReport: the revisions, the growths, the shapes made, the draw bound, and the stores
// the builds ahead write - main rows, object records, shadow state, extras rows, geometry table, exclusions).

namespace DCLF
{
	std::string IndirectDraws::Impl::SnapshotReport()
	{
		// The builder's times since the last report (its queue, drained here alone).
		std::vector<double> ahead, shapes, recordings;
		snapshotTimings.Drain([&](SnapshotTiming&& a_timing) {
			if (a_timing.ahead)
				ahead.push_back(a_timing.aheadMs);
			shapes.push_back(a_timing.shapesMs);
			recordings.push_back(a_timing.recordingsMs);
		});
		const std::size_t aheadBuilt = ahead.size();
		std::sort(adoptionLatency.begin(), adoptionLatency.end());
		const auto percentile = [this](std::size_t a_percent) {
			return adoptionLatency.empty() ? 0u : adoptionLatency[(std::min)(adoptionLatency.size() - 1, adoptionLatency.size() * a_percent / 100)];
		};
		std::string text = fmt::format("[DCLF] scene snapshots (T6b3b): {} built, {} publications coalesced by the builder, {} adopted, {} frames adopting nothing new; "
									   "passed over as stale: build {}, targets {}, shadow format {}, main resources {}; built without draws for the main "
									   "resources {}\n",
			snapshotsBuilt.exchange(0, std::memory_order_relaxed), snapshotsCoalesced.exchange(0, std::memory_order_relaxed), snapshotsAdopted, snapshotFramesKept,
			snapshotsStale[kStaleBuild], snapshotsStale[kStaleTargets], snapshotsStale[kStaleShadowFormat], snapshotsStale[kStaleMain],
			snapshotsWithoutDraws.exchange(0, std::memory_order_relaxed));
		text += fmt::format("[DCLF] snapshot builds (ms, avg/p95/max): draws ahead {} ({} built), shapes {}, recordings wait {}; commit to adoption (frames): p50 {}, p95 {}, "
							"max {}\n",
			Reports::Spread(ahead, 2), aheadBuilt, Reports::Spread(shapes, 2), Reports::Spread(recordings, 2), percentile(50), percentile(95), adoptionLatency.empty() ? 0u : adoptionLatency.back());
		// From the oldest event a publication carried to the frame that installed it (the publications the builder skipped past
		// included: FrameView::HandOverAtFrameStart walks their log nodes).
		if (!eventAdoptionMs.empty()) {
			std::sort(eventAdoptionFrames.begin(), eventAdoptionFrames.end());
			const auto framesAt = [this](std::size_t a_percent) {
				return eventAdoptionFrames[(std::min)(eventAdoptionFrames.size() - 1, eventAdoptionFrames.size() * a_percent / 100)];
			};
			std::vector<double> sorted = eventAdoptionMs;
			std::sort(sorted.begin(), sorted.end());
			const auto msAt = [&sorted](std::size_t a_percent) { return sorted[(std::min)(sorted.size() - 1, sorted.size() * a_percent / 100)]; };
			text += fmt::format("[DCLF] event to adoption (T6b3d: the oldest event a publication carried, to the frame installing it): {} adoptions; ms p50 {:.2f}, "
								"p95 {:.2f}, max {:.2f}; frames p50 {}, p95 {}, max {}\n",
				eventAdoptionMs.size(), msAt(50), msAt(95), sorted.back(), framesAt(50), framesAt(95), eventAdoptionFrames.back());
			eventAdoptionMs.clear();
			eventAdoptionFrames.clear();
		}
		// The growths' adoption to the first draws embedding their versions (DecideCoverage).
		std::sort(growthToDraws.begin(), growthToDraws.end());
		text += fmt::format("[DCLF] snapshot draws: growth adoption to draws embedding it (frames): {} growths, p50 {}, max {}\n", growthToDraws.size(),
			growthToDraws.empty() ? 0u : growthToDraws[growthToDraws.size() / 2], growthToDraws.empty() ? 0u : growthToDraws.back());
		growthToDraws.clear();
		snapshotsAdopted = snapshotFramesKept = 0;
		snapshotsStale = {};
		adoptionLatency.clear();
		// The builder's own lines (the last it posted: a report behind), and the next asked for.
		producerReportSlot.TakeInto(producerReport);
		text += producerReport;
		producerReportWanted.store(true, std::memory_order_release);
		WakeSnapshotBuilder();
		return text;
	}

	std::string IndirectDraws::Impl::ProducerReport()
	{
		// The snapshot builder's (composed in its pass, posted for the frame's report): nothing here is the frame's.
		auto& rv = revisions;
		std::string epochs;
		for (std::uint32_t e = 0, count = EpochCount(); e < count; ++e) {
			auto& s = rv.epochs[e];
			const auto recorded = s.recorded.exchange(0), failedCount = s.failed.exchange(0), dropped = s.dropped.exchange(0), shapes = s.shapes.exchange(0);
			// A hosted epoch (the reflection's): its shape's changes alone; its host's recordings carry its passes.
			if (const auto hostedBy = EpochTraitsOf(e).hostedBy; hostedBy != kNoEpoch) {
				epochs += fmt::format("{}{} {} changed (recorded in the {} epoch's)", epochs.empty() ? "" : "; ", EpochTraitsOf(e).name, s.changed, EpochTraitsOf(hostedBy).name);
				s.changed = s.requested = s.refused = 0;
				continue;
			}
			epochs += fmt::format("{}{} {} changed, {} requested, {} recorded ({} shapes), {} failed{}{}", epochs.empty() ? "" : "; ", EpochTraitsOf(e).name, s.changed,
				s.requested, recorded, shapes, failedCount, dropped ? fmt::format(", {} dropped by a graph build", dropped) : std::string(),
				s.refused ? fmt::format(", {} refused", s.refused) : std::string());
			s.changed = s.requested = s.refused = 0;
		}
		const auto& assembled = rv.assembler.GetStats();
		const auto growths = SceneBufferSet::Get().Report();
		std::string madeBy;
		for (std::size_t g = 0; g < kKeyGroups; ++g)
			if (const auto made = std::exchange(shapesMadeBy[g], 0))
				madeBy += fmt::format("{}{} {}", madeBy.empty() ? " (moved: " : ", ", kKeyGroupNames[g], made);
		if (!madeBy.empty())
			madeBy += ")";
		std::string text = fmt::format("[DCLF] revision shapes: made at {} makes{}, kept at {}\n", std::exchange(shapesMade, 0), madeBy, std::exchange(shapesKept, 0));
		// The builder's wakes: its passes, those that found nothing, the publications it passed through (only deltas moved: the last
		// snapshot posted again), and the scene passes' ends that woke nobody (no publication waiting).
		text += fmt::format("[DCLF] snapshot builder: {} passes, {} found nothing to do, {} publications passed through (deltas only); scene pass ends not waking it: {}\n",
			std::exchange(snapshotBuilder.passes, 0), std::exchange(snapshotBuilder.idlePasses, 0), std::exchange(snapshotBuilder.passedThrough, 0),
			passDoneWakesSkipped.exchange(0, std::memory_order_relaxed));
		// One build per adoption: the draws built and those replaced before a snapshot posted them, the passes that held work for the
		// frame's take; the builds ahead whose stages ran on the builder for a small change, and on the pool.
		{
			auto& b = snapshotBuilder;
			text += fmt::format("[DCLF] snapshot builder pacing: {} builds started, {} discarded unposted; {} passes held work for the frame's take; build stages on "
								"the builder {} (a small change), on the pool {}; {} work contexts replaced by the frame's for new main resources\n",
				std::exchange(b.buildsStarted, 0), std::exchange(b.buildsDiscarded, 0), std::exchange(b.heldPasses, 0), std::exchange(b.stagesInline, 0),
				std::exchange(b.stagesPooled, 0), std::exchange(b.contextsReplaced, 0));
		}
		// The makes that kept the last revision (their requirement pass's inputs unchanged: RevisionReuse) and those that ran it (with no
		// last revision to keep, or by what of the key moved), and the parities (CS_DCLF_PERSISTENT_PARITY): the pass run anyway for an
		// unchanged key, the shapes made anyway for an unchanged shapes key.
		if (auto& reuse = revisionReuse; reuse.kept || reuse.ran) {
			std::string moved;
			for (std::size_t part = 0; part < reuse.movedBy.size(); ++part)
				if (reuse.movedBy[part])
					moved += fmt::format("{}{} {}", moved.empty() ? "" : ", ", reuse.kMovedNames[part], reuse.movedBy[part]);
			text += fmt::format("[DCLF] revision reuse: {} makes kept the last revision (requirement pass unchanged), {} ran it ({} with none to keep; moved: {}); "
								"parity: unchanged passes {} checked, {} differ{}; kept shapes {} checked, {} differ{}\n",
				reuse.kept, reuse.ran, reuse.ranUnkept, moved.empty() ? std::string("nothing") : moved, reuse.parity.checks, reuse.parity.mismatches,
				reuse.parity.Verdict(true), reuse.shapesParity.checks, reuse.shapesParity.mismatches, reuse.shapesParity.Verdict(true));
			reuse.kept = reuse.ran = reuse.ranUnkept = 0;
			reuse.movedBy = {};
			reuse.parity.Reset();
			reuse.shapesParity.Reset();
		}
		text += fmt::format("[DCLF] scene revisions (R3c, the snapshot builder's): {} sealed ({} version sets), {} pending; assembler {} published, {} failed, {} superseded, "
							"{} abandoned; {}{}\n",
			rv.seals, rv.versionSets, rv.assembler.Pending(), assembled.published, assembled.failed, assembled.superseded, assembled.abandoned, epochs,
			rv.sealFailures ? fmt::format(" <- {} NOT SEALED", rv.sealFailures) : std::string());
		rv.seals = rv.versionSets = 0;
		if (!growths.empty() || rv.growthWaits || rv.buildWaits)
			text += fmt::format("[DCLF] scene buffers: {}; {} makes sealed no revision for a pending growth, {} for a graph build asked of the frame\n",
				growths.empty() ? std::string("no seal") : growths, std::exchange(rv.growthWaits, 0), std::exchange(rv.buildWaits, 0));
		if (auto& bound = producer.drawBound; bound.updates) {
			auto& parity = Diagnostics::DrawsState::Get().kept.drawBound;
			text += fmt::format("[DCLF] scene draw bound: {} updates, {:.1f} slots changed an update, {} draws over {} slots, {} resyncs; parity {} checked, {} differ{}\n",
				bound.updates, static_cast<double>(bound.changes) / bound.updates, bound.draws, bound.produced.size(), bound.resyncs, parity.checks,
				parity.mismatches, parity.Verdict(true));
			bound.updates = bound.changes = bound.resyncs = 0;
			parity.Reset();
		}
		// The stores the builds write (the builder's alone).
		text += StoresReport();
		return text;
	}

	std::string IndirectDraws::Impl::RevisionReport()
	{
		auto& rv = revisions;
		std::string covered;
		for (std::uint32_t e = 0, count = EpochCount(); e < count; ++e) {
			// A hosted epoch's (the reflection's): its host's submissions with its passes' work.
			const auto unrevised = std::exchange(rv.unrevised[e], 0), clamped = std::exchange(rv.latchClamped[e], 0), variants = std::exchange(rv.variants[e], 0);
			const auto hostedBy = EpochTraitsOf(e).hostedBy;
			covered += fmt::format("{}{} {}{}{}{}{}", covered.empty() ? "" : "; ", EpochTraitsOf(e).name, std::exchange(rv.submitted[e], 0),
				hostedBy != kNoEpoch ? fmt::format(" (in the {} epoch)", EpochTraitsOf(hostedBy).name) : std::string(),
				variants ? fmt::format(" ({} of a later recording)", variants) : std::string(),
				unrevised ? fmt::format(", {} without the revision's shape <- UNREVISED", unrevised) : std::string(),
				clamped ? fmt::format(", {} with values past its latch <- LATCH", clamped) : std::string());
		}
		std::string text;
		if (!covered.empty())
			text += fmt::format("[DCLF] epochs submitted by the revision: {}; {} values staged that a revision's latched copies lacked\n", covered,
				std::exchange(rv.latchedMisses, 0));
		{
			std::string uncovered;
			for (std::uint32_t e = 0, count = EpochCount(); e < count; ++e)
				uncovered += fmt::format("{}{} {}", e ? ", " : "", EpochTraitsOf(e).name, std::exchange(rv.uncovered[e], 0));
			const std::uint64_t unbuiltTotal = unbuilt[kAsyncZPrepass] + unbuilt[kAsyncColour] + shadowUnbuilt;
			text += fmt::format("[DCLF] strict epochs: {} frames without the adopted snapshot's main recordings (DCLF not drawing yet, or a load screen up); "
								"waits at the frame's start for a snapshot current for the graph as built: {} ({:.2f} ms, max {:.2f} ms); {} graph builds at the "
								"build point, {} tickets of epochs not submitted given back for recordings; frames whose revision lacked an epoch's current recordings: {}; "
								"shadow views left to the engine: {} frames without the revision's shadow recording or the installed shadow payload; occlusion maps "
								"left to the engine for want of the revision's shape: {}; covered epochs without the installed publication's payload: Z-prepass {}, "
								"colour {}, shadow {}{}\n",
				std::exchange(rv.uncoveredFrames, 0), std::exchange(snapshotWaits, 0), std::exchange(snapshotWaitMs, 0.0), std::exchange(snapshotWaitMaxMs, 0.0),
				std::exchange(rv.builds, 0), std::exchange(rv.ticketsReleased, 0), uncovered, std::exchange(shadowUnrecorded, 0), std::exchange(occlusionUnrecorded, 0),
				unbuilt[kAsyncZPrepass], unbuilt[kAsyncColour], shadowUnbuilt, unbuiltTotal ? " <- UNBUILT" : "");
			unbuilt = {};
			shadowUnbuilt = 0;
		}
		return text;
	}

	std::string IndirectDraws::Impl::StoresReport()
	{
		// The snapshot builder's (its pass, with ProducerReport): the stores RunAhead writes, read and reset here alone - the render thread's
		// report never touches them.
		std::string text;
		if (auto& rows = mainRows; rows.builds) {
			text += fmt::format("[DCLF] main rows: {} builds; a build: {:.1f} material and {:.1f} pipeline rows written; {} material and {} pipeline rows held, "
								"{} resyncs; material rows written again by what moved: record {}, frame values {}, lookup {}, shared {}, "
								"technique bindings {}, projected {}, constant tables {}\n",
				rows.builds, double(rows.materialsWritten) / rows.builds, double(rows.pipelinesWritten) / rows.builds, rows.material.Size(), rows.pipeline.Size(), rows.resyncs,
				rows.keyMoved[0], rows.keyMoved[1], rows.keyMoved[2], rows.keyMoved[3], rows.keyMoved[4], rows.keyMoved[5], rows.keyMoved[6] + rows.keyMoved[7]);
			rows.builds = rows.materialsWritten = rows.pipelinesWritten = rows.resyncs = 0;
			rows.keyMoved = {};
			// The material rows' copies since the last report (KeptArray copies only the chunks a held view shares).
			static KeptCopyStats lastCopies;
			const KeptCopyStats copies = rows.material.CopyStats();
			text += fmt::format("[DCLF] material rows copied: {} tables, {} chunks ({:.1f} KB), {} chunks reused, {} allocated\n", copies.tableCopies - lastCopies.tableCopies,
				copies.chunkCopies - lastCopies.chunkCopies, double(copies.bytesCopied - lastCopies.bytesCopied) / 1024.0, copies.chunkReuses - lastCopies.chunkReuses,
				copies.chunkAllocations - lastCopies.chunkAllocations);
			lastCopies = copies;
		}
		text += Draws::MainPairsReport(mainRows);
		{
			// The payload's pools (PayloadBuilder, D5): what each publication caught up per part, the backings made, the parts shared with
			// the last publication (unchanged: nothing sent), the live census and the journal floor; under CS_DCLF_PERSISTENT_PARITY, the
			// leased count against the lease-leak bound. Then each part's changed publications against the last publication's backing
			// (AppendClasses: where the runs since its contentEpoch lie against its sealed mark, with the KB a catch-up from it would
			// send each), the appends made, and under CS_DCLF_PERSISTENT_PARITY each append's first byte against the mark.
			static constexpr std::array<const char*, Draws::kPayloadParts> kParts{ "objects", "extras", "geometries", "material rows", "pipeline rows", "inputs", "shadow rows" };
			const auto pool = payloadBuilder.TakeStats();
			const double publications = static_cast<double>(std::max<std::uint64_t>(pool.publications, 1));
			const bool watch = Diagnostics::Observe().scene;
			auto& watches = Diagnostics::DrawsState::Get().payloadPools;
			std::string sent, backings, appends;
			const auto each = [](std::uint64_t a_bytes, std::uint64_t a_count) { return a_count ? a_bytes / 1024.0 / static_cast<double>(a_count) : 0.0; };
			for (std::size_t p = 0; p < Draws::kPayloadParts; ++p) {
				const auto& a = pool.appends[p];
				appends += fmt::format("{}{} {} pure ({:.1f} KB), {} past capacity ({:.1f} KB), {} mixed ({:.1f} KB), {} rewrites ({:.1f} KB), {} without a head, {} appended",
					p ? "; " : "", kParts[p], a.pure, each(a.pureBytes, a.pure), a.grown, each(a.grownBytes, a.grown), a.mixed, each(a.mixedBytes, a.mixed), a.rewrites,
					each(a.rewriteBytes, a.rewrites), a.headless, a.appended);
				const auto& census = pool.pools[p];
				const auto floor = payloadBuilder.OldestContentEpoch(static_cast<Draws::PayloadPart>(p));
				sent += fmt::format("{}{} {:.1f} KB in {:.1f} runs ({} made, {} shared)", p ? ", " : "", kParts[p], pool.bytes[p] / 1024.0 / publications, pool.runs[p] / publications,
					pool.allocated[p], pool.shared[p]);
				backings += fmt::format("{}{} {} live ({} leased, {} free, {:.1f} MB), floor {}{}", p ? "; " : "", kParts[p], census.live, census.leased, census.free,
					census.bytes / (1024.0 * 1024.0), floor ? fmt::format("{}", *floor) : std::string("none"), watch ? watches[p].Window(census.leased) : "");
			}
			// Publications against the payloads the frame installed (each is for a snapshot the frame takes), the sealed draws whose
			// publication waited for the frame, and those newer work replaced before it.
			auto& b = snapshotBuilder;
			const std::string appendParity = !pool.appendChecks ? std::string() :
				fmt::format("; appends checked against the mark {}, {} below{}", pool.appendChecks, pool.appendsBelowMark,
					pool.appendsBelowMark ? " <- APPEND BELOW MARK; first: " + pool.firstBelowMark : std::string(" <- OK"));
			text += fmt::format("[DCLF] payload pools (D5): {} publications, {} installed; {} draws waited for the frame to publish, {} replaced unpublished; "
								"catch-up a publication: {}; changed parts against the head (CS_DCLF_PAYLOAD_APPEND {}): {}{}\n",
				pool.publications, payloadsInstalled.exchange(0, std::memory_order_relaxed), std::exchange(b.publicationsWaited, 0),
				std::exchange(b.replacedUnpublished, 0), sent, payloadBuilder.AppendsInPlace() ? "on" : "off", appends, appendParity);
			text += fmt::format("[DCLF] payload pools' backings: {}\n", backings);
		}
		auto& kept = Diagnostics::DrawsState::Get().kept;
		if (auto* store = &objectStore; store->updates) {
			auto& parity = kept.objects;
			text += fmt::format("[DCLF] persistent object records: {} updates, {:.1f} records rewritten an update, {} records held, {} resyncs, {} collisions; parity {} checked, {} differ{}\n",
				store->updates, static_cast<double>(store->rewritten) / store->updates, store->records.Size(), store->resyncs, store->collisions, parity.checks,
				parity.mismatches, store->collisions && parity.checks ? std::string(" <- DIFFER") : parity.Verdict(true));
			std::string causes;
			for (std::uint32_t c = 0; c < kChangeCauseCount; ++c)
				if (store->byCause[c])
					causes += fmt::format("{}{} {}", causes.empty() ? "" : ", ", kChangeCauseNames[c], store->byCause[c]);
			text += fmt::format("[DCLF] object record updates by what changed: {} only placements and palettes, {} structural{}{}\n", store->streamOnly,
				store->structural, causes.empty() ? "" : " (by cause: ", causes.empty() ? "" : causes + ")");
			store->streamOnly = store->structural = 0;
			store->byCause = {};
			store->updates = store->rewritten = store->resyncs = store->collisions = 0;
			parity.Reset();
		}
		if (auto& k = shadowEngine->shadowKept; k.builds) {
			// The objects it holds (their words are the scene list's entries'; its own region only under CS_DCLF_BUILD_PARITY).
			const std::size_t entries = static_cast<std::size_t>(std::count_if(k.heldModes.begin(), k.heldModes.end(), [](std::uint8_t a_modes) { return a_modes != 0; }));
			text += fmt::format(
				"[DCLF] persistent shadow state: {} builds, {:.1f} entries and {:.1f} material rows written a build, {:.1f} list entries recounted a build, {} entries "
				"and {} material rows held, {} resyncs; parity {} inputs checked, {} differ{}\n",
				k.builds, static_cast<double>(k.entriesWritten) / k.builds, static_cast<double>(k.rowsWritten) / k.builds, static_cast<double>(k.entriesCounted) / k.builds,
				entries, k.rows.Size(), k.resyncs, kept.shadow.checks, kept.shadow.mismatches, kept.shadow.Verdict(true));
			if (!kept.shadowMissingBy.empty()) {
				std::string why;
				for (const auto& [reason, count] : kept.shadowMissingBy)
					why += fmt::format("{}{} {} (first {})", why.empty() ? "" : ", ", reason, count, kept.shadowMissingFirst[reason]);
				text += fmt::format("[DCLF] persistent shadow state: inputs of the per-frame build only, by why: {}\n", why);
				kept.shadowMissingBy.clear();
				kept.shadowMissingFirst.clear();
			}
			k.builds = k.entriesWritten = k.rowsWritten = k.resyncs = k.entriesCounted = 0;
			kept.shadow.Reset();
		}
		if (auto* store = &extrasStore; store->updates || kept.extras.checks) {
			// The payload's extras backings, each against its own mirror (KeptChecks.cpp, every publication).
			text += fmt::format("[DCLF] persistent extras rows: {} updates, {} resyncs; payload backings parity {} catch-ups checked ({} backings mirrored), {} differ{}\n",
				store->updates, store->resyncs, kept.extras.checks, kept.extrasMirrors.size(), kept.extras.mismatches, kept.extras.Verdict(true));
			store->updates = store->resyncs = 0;
			kept.extras.Reset();
		}
		if (auto* store = &geometryStore; store->updates) {
			text += fmt::format("[DCLF] persistent geometry table: {} updates, {:.2f} slots repacked an update, {} slots held, {} resyncs; parity {} checked, {} differ{}\n",
				store->updates, static_cast<double>(store->rewritten) / store->updates, store->packed.Size(), store->resyncs, kept.geometry.checks, kept.geometry.mismatches,
				kept.geometry.Verdict(true));
			store->updates = store->rewritten = store->resyncs = 0;
			kept.geometry.Reset();
		}
		// The exclusions' caches are the shadow exclusion contribution's (Features/Shadows/Local), on this thread.
		auto& sunExclusionCache = Features::Shadows::Local::ExclusionCacheOf(Api::ShadowSource::Sun);
		for (auto* cache : { &sunExclusionCache, &Features::Shadows::Local::ExclusionCacheOf(Api::ShadowSource::Point) }) {
			if (auto& c = *cache; c.builds) {
				auto& parity = cache == &sunExclusionCache ? kept.sunExclusion : kept.parabolicExclusion;
				text += fmt::format("[DCLF] {} exclusion: {} builds: {} the same exclusion again (nothing moved), {} updated by what moved ({} for newer candidates), {} judged whole; "
									"{:.4f} ms a build (max {:.3f}); parity {} checked, {} differ{}\n",
					cache == &sunExclusionCache ? "sun" : "paraboloid", c.builds, c.same, c.reused - c.same, c.translated, c.builds - c.reused, c.buildMs / c.builds,
					c.buildMaxMs, parity.checks, parity.mismatches, parity.Verdict());
				c.builds = c.reused = c.translated = c.same = 0;
				c.buildMs = c.buildMaxMs = 0.0;
				parity.Reset();
			}
		}
		return text;
	}
}
