// Fades' growth lines for the diagnostic capture (FadeCensus.h).

#include "Features/DrawcallLimitFix/Features/Fades/FadeCensus.h"

#include "Features/DrawcallLimitFix/Features/Fades/FadeScene.h"
#include "Features/DrawcallLimitFix/Features/Fades/FadeWriters.h"

namespace DCLF::Fades
{
	std::vector<std::string> RenderGrowthLines()
	{
		std::vector<std::string> lines;
		const auto writeBack = Features::Fades::PeekWriteBackCensus(false);
		if (writeBack.present) {
			lines.push_back(fmt::format("fade write-back pending batches {}", writeBack.pendingBatches));
			lines.push_back(fmt::format("fade write-back ready stores {}", writeBack.readyStores));
			lines.push_back(fmt::format("fade write-back applied frames {}", writeBack.appliedFrameSize));
		} else
			lines.emplace_back("fade write-back none");
		const auto seeds = Features::Fades::PeekSeedCounts();
		lines.push_back(fmt::format("fade seeds unsent {}", seeds.unsent));
		lines.push_back(fmt::format("fade seeds owed {}", seeds.owed));
		lines.push_back(fmt::format("fade writer roots {}", Features::Fades::PeekFadeWriterCounts().roots));
		return lines;
	}
}
