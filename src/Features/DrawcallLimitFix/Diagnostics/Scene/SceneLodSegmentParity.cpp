#include "SceneObservers.h"
#include "SceneState.h"

#include "Features/DrawcallLimitFix/Scene/Coordinator/Internal.h"

// CS_DCLF_PERSISTENT_PARITY: every tracked object LOD shape's drawn ranges (kept by its segment events) against its live state, every 60th
// pass, once in it, after the pass's segment events.

namespace DCLF::Diagnostics
{
	namespace
	{
		using V = SceneView;

		void OnLodSegmentsApplied(const LodSegmentsApplied& a_event)
		{
			const auto& store = a_event.store;
			const auto& lodRanges = V::lodRanges(store);
			auto& l = SceneDiagnostics().lodSegments;
			if (lodRanges.empty() || !ParityDue(V::passSerial(store)) || std::exchange(l.pass, store.PassStamp()) == store.PassStamp())
				return;
			++l.checks;
			std::vector<LodSegments::Range> live;
			for (const auto& [shape, ranges] : lodRanges) {
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

	void InstallLodSegmentParity()
	{
		Subscribe<LodSegmentsApplied>(OnLodSegmentsApplied);
	}
}
