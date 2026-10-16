// CS_DCLF_PERSISTENT_PARITY: every tracked object LOD shape's drawn ranges (kept by its segment events) against its live state, every 60th
// pass, once in it, after the pass's segment events. Reported with the segments' line (LodSegmentChannel.cpp).

#include "Features/DrawcallLimitFix/Features/Lod/LodSegmentEvents.h"
#include "Features/DrawcallLimitFix/Features/Lod/LodSegments.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineReadWindow.h"
#include "Features/DrawcallLimitFix/Core/Extensions/CoordinatorComponents.h"

#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"
#include "Features/DrawcallLimitFix/Diagnostics/Observation.h"

#include <utility>
#include <vector>

namespace DCLF::Features::Lod
{
	namespace
	{
		void OnSegmentsApplied(const SegmentsApplied& a_event)
		{
			const auto& context = a_event.context;
			const auto& entryRanges = context.EntryRanges();
			auto& l = context.State<SegmentState>().parity;
			if (entryRanges.empty() || !Diagnostics::ParityDue(context.PassSerial()) || std::exchange(l.pass, context.PassStamp()) == context.PassStamp())
				return;
			++l.checks;
			std::vector<Api::EntryRange> live;
			for (const auto& [shape, ranges] : entryRanges) {
				// An item's lease (the scene work may run beside the engine's update).
				const EngineReadWindow::Lease lease;
				if (!lease)
					break;
				++l.shapes;
				LodSegments::DrawnRanges(shape, live);
				if (live == ranges)
					continue;
				if (l.differ++ == 0)
					l.first = fmt::format("'{}' ({} segments, dirty {}): {} ranges held, {} live (first {}+{} against {}+{})", shape->name.c_str() ? shape->name.c_str() : "",
						LodSegments::At<std::uint32_t>(shape, LodSegments::kSegmentCount), LodSegments::At<std::uint8_t>(shape, LodSegments::kDirty), ranges.size(), live.size(),
						ranges.empty() ? 0u : ranges[0].firstIndex, ranges.empty() ? 0u : ranges[0].indexCount, live.empty() ? 0u : live[0].firstIndex, live.empty() ? 0u : live[0].indexCount);
			}
		}
	}

	void SubscribeSegmentParity()
	{
		Diagnostics::Subscribe<SegmentsApplied>(OnSegmentsApplied);
	}
}
