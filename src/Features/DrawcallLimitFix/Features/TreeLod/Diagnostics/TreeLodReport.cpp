// Tree LOD's report lines: its draws and two-phase cull in the draws' async work (ReportPoint::AsyncWorkFamilies), and its mirror's line
// with its parity's among the render thread's scene lines (ReportPoint::SceneFrame); and the depth commits' holes' log (always on), which
// the draws' line counts.

#include "Features/DrawcallLimitFix/Features/TreeLod/TreeLodTables.h"
#include "Features/DrawcallLimitFix/Features/TreeLod/TreeLodMirror.h"
#include "Features/DrawcallLimitFix/Features/TreeLod/Diagnostics/TreeLodEvents.h"
#include "Features/DrawcallLimitFix/Features/TreeLod/Diagnostics/TreeLodParity.h"
#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"
#include "Features/DrawcallLimitFix/Diagnostics/Draws/DrawsView.h"

#include <string>

namespace DCLF::Features::TreeLod
{
	namespace
	{
		// The depth commits that could not draw a withheld frame (render thread, LogMissed): the draws' line's count.
		std::uint32_t missedCommits = 0;

		void LogMissed(const TreeLodMissed& a_missed)
		{
			if (missedCommits++ < 8)
				logger::warn("[DCLF] tree LOD: frame {} withheld the engine's passes but the depth commit could not draw (tables {} of {} shape slots)", a_missed.frame,
					a_missed.shapeSlots, a_missed.bufferSlots);
		}

		/** @brief The cull's line (newline-terminated): in view, drawn by phase 1, occluded by the last frame's HZB, brought back by the rebuilt one. */
		void CullLine(TreeLodCounts& a_counts, std::string& a_text)
		{
			if (const auto frames = a_counts.frames.exchange(0, std::memory_order_relaxed)) {
				const double phaseOne = double(a_counts.phaseOne.exchange(0, std::memory_order_relaxed)) / frames;
				const double retests = double(a_counts.retests.exchange(0, std::memory_order_relaxed)) / frames;
				const double phaseTwo = double(a_counts.phaseTwo.exchange(0, std::memory_order_relaxed)) / frames;
				a_text += fmt::format("[DCLF] tree LOD cull over {} frames: per frame {:.0f} instances in view, {:.0f} drawn by phase 1, {:.0f} occluded by the last "
									  "frame's HZB, {:.0f} of them brought back by phase 2 ({:.0f} drawn, {:.1f}% occluded)\n",
					frames, phaseOne + retests, phaseOne, retests, phaseTwo, phaseOne + phaseTwo,
					phaseOne + retests > 0.0 ? 100.0 * (retests - phaseTwo) / (phaseOne + retests) : 0.0);
			}
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
			a_text += fmt::format("[DCLF] tree LOD draws: {} ({} depth commits, {} shape slots sent; tables {} shape and {} mesh slots; {} commits that could not draw a "
								  "withheld frame){}\n",
				IndirectDraws::Get().DrawFamilyOwned(Api::DrawFamily::TreeInstances) ? "DCLF's" : "the engine's", tables->uploads, tables->slotsSent,
				Draws::NamedUnits(adopted, tables->shapes), Draws::NamedUnits(adopted, tables->meshes), missed, missed ? " <- TREE LOD HOLES" : "");
			tables->uploads = tables->slotsSent = 0;
			if (auto& counts = tables->counts)
				CullLine(*counts, a_text);
		}

		/** @brief The mirror's line (newline-terminated), its counters and its parity's reset. */
		std::string MirrorLine(Mirror& a_mirror)
		{
			const auto s = a_mirror.TakeStats();
			auto& p = ParityState();
			auto text = fmt::format("[DCLF] tree LOD mirror: {} shapes, {} instances{}; {} adds, {} removes ({} off the render thread); parity {} checks, {} shapes compared, {} not settled, {} missing, {} stale, {} refilled after the drain, {} differ{}{}; {} groups drawn by the rule where the engine keeps hidden records\n",
				a_mirror.Size(), a_mirror.InstanceCount(), installed ? "" : ", events not installed", s.adds, s.removes, TakeOffThread(), p.checks, p.shapes, p.unsettled,
				p.missing, p.stale, p.late, p.differ, p.checks ? (p.missing || p.stale || p.differ ? " <- TREE LOD MIRROR; first: " : " <- OK") : "", p.first, s.intended);
			p = { .suspects = std::move(p.suspects), .frame = p.frame };
			return text;
		}

		// The render thread's scene lines: the mirror's, once it holds shapes or its events are in.
		void MirrorText(std::string& a_text)
		{
			auto& mirror = RenderMirror();
			if (mirror.Size() || installed)
				a_text += MirrorLine(mirror);
		}
	}

	void RegisterReport(Api::InstallContext& a_context)
	{
		// Where Core printed them: after the draws' async work features' lines, before the frame values' line; after the extras parity's
		// line, before the mirror watch's.
		a_context.RegisterReportLines({ .name = "tree-lod draws", .point = Api::ReportPoint::AsyncWorkFamilies, .text = &DrawsText });
		a_context.RegisterReportLines({ .name = "tree-lod mirror", .point = Api::ReportPoint::SceneFrame, .text = &MirrorText });
		// The depth commits' holes (TreeLodTables.cpp's commit hook), logged whatever the observers.
		Diagnostics::Subscribe<TreeLodMissed>(&LogMissed);
	}
}
