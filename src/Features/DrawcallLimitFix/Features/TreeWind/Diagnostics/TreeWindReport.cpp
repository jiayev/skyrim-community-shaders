// Tree wind's report line (CS_DCLF_STATS, among the primary cull's lines): the tree animation list and the tree list's loop, with the
// persistent parity's owned trees still on the list.

#include "Features/DrawcallLimitFix/Features/TreeWind/TreeWind.h"
#include "Features/DrawcallLimitFix/Features/TreeWind/TreeAnimation.h"
#include "Features/DrawcallLimitFix/Diagnostics/Observation.h"

#include <string>

namespace DCLF::Features::TreeWind
{
	namespace
	{
		/** @brief The tree animation list's line (a_checks, a_onList: the persistent parity's owned trees still on the list). */
		std::string TreeAnimationLine(std::uint64_t a_checks, std::uint64_t a_onList)
		{
			const auto s = TreeAnimation::Take();
			if (!s.installed)
				return fmt::format("[DCLF] tree animation list: off (not installed without the primary cull): {} on the list (the manager's last update {:.3f} ms)", s.listed, s.updateMs);
			const auto text = fmt::format("[DCLF] tree animation list: {} on the list, {} owned trees off it (the manager's last update {:.3f} ms); {} syncs: {} taken off, {} put back; engine: {} adds ({} taken off again), {} removes of trees off the list, {} clears; parity {} checks, {} owned trees on the list{}; "
										  "tree list (T7b5 fix): {} stood-in BSTreeNodes, {} loops given DCLF's verdicts: {} in / {} out (replacing the engine's {} stale / {} in / {} out), {} without a multibound",
				s.listed, s.held, s.updateMs, s.syncs, s.takenOff, s.putBack, s.engineAdds, s.takenOffAgain, s.engineRemoves, s.clears, a_checks, a_onList,
				a_checks ? (a_onList ? " <- TREE LIST" : " <- OK") : "", s.ownedTrees, s.treeLoops, s.treesIn, s.treesOut, s.replaced[0], s.replaced[1], s.replaced[2],
				s.noMultiBound);
			// With CS_DCLF_TREE_LIST_ALL: every BSTreeNode DCLF lists.
			const auto wideText = !ListAll() ? std::string() : fmt::format(
				"; every BSTreeNode (T7f; CS_DCLF_TREE_LIST_ALL): {} in / {} out written (replacing the engine's {} stale / {} in / {} out); against the engine's {} same, {} DCLF's in "
				"and the engine's out, {} DCLF's out and the engine's in, {} the engine's stale; aside {} without DCLF's verdict, {} without a multibound",
				s.wideIn, s.wideOut, s.wideReplaced[0], s.wideReplaced[1], s.wideReplaced[2], s.drySame, s.dryInOut, s.dryOutIn,
				s.dryStale, s.wideNoVerdict, s.wideNoMultiBound);
			return text + wideText;
		}

		void Lines(std::uint32_t)
		{
			std::uint64_t treeChecks = 0, treeOnList = 0;
			if (TreeAnimation::Installed() && Diagnostics::Observe().scene)
				treeChecks = TreeAnimation::OwnedOnList(treeOnList) ? 1 : 0;
			logger::info("{}", TreeAnimationLine(treeChecks, treeOnList));
		}
	}

	void RegisterReport(Api::InstallContext& a_context)
	{
		a_context.RegisterReportLines({ .name = "tree-wind", .point = Api::ReportPoint::PrimaryCull, .lines = &Lines });
	}
}
