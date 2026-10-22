// Grass's report lines: its draws and two-phase cull in the draws' async work (ReportPoint::AsyncWorkFamilies), and its mirror's line
// with its parity's among the render thread's scene lines (ReportPoint::SceneFrame); and the depth commits' holes' log (always on), which
// the draws' line counts.

#include "Features/DrawcallLimitFix/Features/Grass/GrassTables.h"
#include "Features/DrawcallLimitFix/Features/Grass/GrassMirror.h"
#include "Features/DrawcallLimitFix/Features/Grass/Diagnostics/GrassEvents.h"
#include "Features/DrawcallLimitFix/Features/Grass/Diagnostics/GrassParity.h"
#include "Features/DrawcallLimitFix/Core/Render/IndirectDraws.h"
#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"
#include "Features/DrawcallLimitFix/Diagnostics/Draws/DrawsView.h"

#include <string>

namespace DCLF::Features::Grass
{
	namespace
	{
		// The depth commits that could not draw a withheld frame (render thread, LogMissed): the draws' line's count.
		std::uint32_t missedCommits = 0;

		void LogMissed(const GrassMissed& a_missed)
		{
			if (missedCommits++ < 8)
				logger::warn("[DCLF] grass: frame {} withheld the engine's passes but the depth commit could not draw (tables {} of {} instance slots, {} of {} bucket slots)",
					a_missed.frame, a_missed.instanceSlots, a_missed.bufferInstances, a_missed.bucketSlots, a_missed.bufferBuckets);
		}

		/** @brief The cull's line (newline-terminated): per frame, the cull's verdicts and the draws each array executed. */
		void CullLine(GrassCounts& a_counts, std::string& a_text)
		{
			const auto frames = a_counts.frames.exchange(0, std::memory_order_relaxed);
			if (!frames)
				return;
			auto per = [&](std::atomic<std::uint64_t>& a_sum) { return double(a_sum.exchange(0, std::memory_order_relaxed)) / frames; };
			const double live = per(a_counts.live), outside = per(a_counts.outside), thinned = per(a_counts.thinned), faded = per(a_counts.faded);
			const double tested = per(a_counts.tested), phaseOne = per(a_counts.phaseOne), retests = per(a_counts.retests), phaseTwo = per(a_counts.phaseTwo);
			const double drawsOne = per(a_counts.phaseOneDraws), drawsTwo = per(a_counts.phaseTwoDraws), colour = per(a_counts.colourDraws), pbr = per(a_counts.pbrDraws);
			a_text += fmt::format("[DCLF] grass cull over {} frames: per frame {:.0f} live instances, {:.0f} outside, {:.0f} thinned, {:.0f} faded, {:.0f} tested, "
								  "{:.0f} drawn by phase 1, {:.0f} occluded by the last frame's HZB, {:.0f} of them brought back by phase 2; draws per frame "
								  "{:.1f} phase 1, {:.1f} phase 2, {:.1f} colour ({:.1f} base, {:.1f} pbr)\n",
				frames, live, outside, thinned, faded, tested, phaseOne, retests, phaseTwo, drawsOne, drawsTwo, colour + pbr, colour, pbr);
		}

		// The draws' async work: the depth commits' uploads since the last report, and its two-phase cull.
		void DrawsText(std::string& a_text)
		{
			const auto* impl = Diagnostics::DrawsView::Find();
			auto* tables = impl && impl->scene ? TablesOf(*impl->scene) : nullptr;
			if (!tables || !tables->uploads)
				return;
			const auto& adopted = impl->Adopted();
			const auto missed = missedCommits;
			a_text += fmt::format("[DCLF] grass draws: {} ({} depth commits, {} shape slots and {} buckets sent; tables {} instance, {} shape, {} bucket and {} mesh "
								  "slots; {} buckets waiting for a texture, {} commits past {} buckets, {} drawing the tables' last changes while they grow; {} commits "
								  "that could not draw a withheld frame){}\n",
				IndirectDraws::Get().DrawFamilyOwned(Api::DrawFamily::Grass) ? "DCLF's" : "the engine's", tables->uploads, tables->shapesSent, tables->bucketsSent,
				Draws::NamedUnits(adopted, tables->instances), Draws::NamedUnits(adopted, tables->shapes), Draws::NamedUnits(adopted, tables->buckets),
				Draws::NamedUnits(adopted, tables->meshes), tables->unboundBuckets, tables->overBuckets, GrassInstances::kMaxBuckets, tables->outgrown, missed,
				missed ? " <- GRASS HOLES" : "");
			tables->uploads = tables->shapesSent = tables->bucketsSent = tables->unboundBuckets = tables->overBuckets = tables->outgrown = 0;
			if (auto& counts = tables->counts)
				CullLine(*counts, a_text);
		}

		/** @brief The mirror's line (newline-terminated), its counters and its parity's reset. */
		std::string MirrorLine(Mirror& a_mirror)
		{
			const auto s = a_mirror.TakeStats();
			auto& p = ParityState();
			const bool failed = p.missing || p.stale || p.differ;
			const auto ready = a_mirror.ReadinessOf();
			auto text = fmt::format("[DCLF] grass mirror: {} shapes, {} instances, {} buckets ({} drawable), {} meshes ({} leased, {} failed, {} pending){}; {} adds, {} removes, {} stale-seq drops, {} rejected ({} live); "
									"parity {} checks, {} shapes compared ({} captured instances, {} mirrored), {} missing, {} stale, {} late, {} differ{}{}\n",
				a_mirror.Size(), a_mirror.InstanceCount(), a_mirror.BucketCount(), ready.bucketsDrawable, a_mirror.MeshCount(), ready.meshesResolved, ready.meshesFailed,
				ready.meshesPending, installed ? "" : ", sink not installed", s.adds, s.removes,
				s.stale, s.rejected, a_mirror.RejectedLive(), p.checks, p.shapes, p.liveInstances, p.mirroredInstances, p.missing, p.stale, p.late, p.differ,
				p.checks ? (failed ? " <- GRASS MIRROR; first: " : " <- OK") : "", p.first);
			p = { .suspects = std::move(p.suspects), .frame = p.frame };
			return text;
		}

		// The render thread's scene lines: the mirror's, once it holds shapes or its sink is in.
		void MirrorText(std::string& a_text)
		{
			auto& mirror = RenderMirror();
			if (mirror.Size() || installed)
				a_text += MirrorLine(mirror);
		}
	}

	void RegisterReport(Api::InstallContext& a_context)
	{
		// After tree LOD's (which registers before this feature) at both points.
		a_context.RegisterReportLines({ .name = "grass draws", .point = Api::ReportPoint::AsyncWorkFamilies, .text = &DrawsText });
		a_context.RegisterReportLines({ .name = "grass mirror", .point = Api::ReportPoint::SceneFrame, .text = &MirrorText });
		// The depth commits' holes (GrassTables.cpp's commit hook), logged whatever the observers.
		Diagnostics::Subscribe<GrassMissed>(&LogMissed);
	}
}
