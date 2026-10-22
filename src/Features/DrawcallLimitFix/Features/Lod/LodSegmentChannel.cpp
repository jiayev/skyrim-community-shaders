// Object LOD's segment events on an engine channel (Api/EngineEvents.h): the channel its writers' hooks feed (LodSegmentHooks.cpp), applied
// after the switch events; its coordinator component, which takes each named shape's drawn ranges again from the mirror into Core's kept
// ranges (the walk reads them as data: each range a geometry slot of the shape's chain); and its report line.

#include "Features/DrawcallLimitFix/Features/Lod/LodSegmentEvents.h"
#include "Features/DrawcallLimitFix/Features/Lod/LodSegments.h"
#include "Features/DrawcallLimitFix/Core/Extensions/CoordinatorComponents.h"
#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"

#include <memory>
#include <span>
#include <vector>

namespace DCLF::Features::Lod
{
	namespace
	{
		Api::EngineChannelId segmentChannel;

		std::shared_ptr<void> MakeState()
		{
			return std::make_shared<SegmentState>();
		}

		void ApplySegmentEvents(const Api::CoordinatorComponentContext& a_context, Api::EngineChannelId a_channel, std::span<const void* const> a_keys)
		{
			if (a_channel != segmentChannel)
				return;
			auto& stats = a_context.State<SegmentState>().stats;
			for (const void* key : a_keys) {
				++stats.events;
				// Only a tracked shape is known to be alive (its entry holds it).
				const auto* kept = a_context.EntryRangesOf(key);
				if (!kept)
					continue;
				const auto* mirrored = a_context.MirroredRanges(key);
				std::vector<Api::EntryRange> next = mirrored ? *mirrored : std::vector<Api::EntryRange>{};
				// Changed by an event: rewrite the record (its range slot chain) this frame.
				const bool changed = next != *kept;
				if (changed)
					++stats.changed;
				a_context.SetEntryRanges(key, std::move(next), changed);
			}
			Diagnostics::Notify(SegmentsApplied{ a_context });
		}

		void SegmentLines(const Api::CoordinatorComponentContext& a_context, std::string& a_text)
		{
			auto& segments = a_context.State<SegmentState>();
			auto& l = segments.stats;
			auto& lp = segments.parity;
			const auto& ranges = a_context.EntryRanges();
			if (!l.events && !lp.checks && ranges.empty())
				return;
			std::size_t partial = 0;
			for (const auto& [shape, shapeRanges] : ranges) {
				const std::uint32_t whole = LodSegments::At<std::uint16_t>(shape, LodSegments::kTriangleCount) * 3u;
				partial += shapeRanges.size() != 1 || shapeRanges[0].firstIndex != 0 || shapeRanges[0].indexCount != whole ? 1 : 0;
			}
			a_text += fmt::format("[DCLF] object LOD segments: {} shapes ({} partly hidden){}; {} segment events, {} drawn-range changes; parity {} checks, {} shapes compared, {} differ{}{}\n",
				ranges.size(), partial, SegmentHooksInstalled() ? "" : ", events not installed", l.events, l.changed, lp.checks, lp.shapes, lp.differ,
				lp.checks ? (lp.differ ? " <- LOD SEGMENTS; first: " : " <- OK") : "", lp.first);
			l = {};
			lp = { .pass = lp.pass };
		}
	}

	void RegisterSegmentChannel(Api::InstallContext& a_context)
	{
		// Applied where Core applied the segment events: after the switch events (drained last); the mirror's parity counts the keys named
		// (the writers' events carry the ranges in the mirror's update, not here).
		segmentChannel = a_context.RegisterEngineChannel({ .name = "object LOD segments",
			.kind = Api::ChannelKind::Keys,
			.apply = Api::ChannelApply::AfterSwitches,
			.namesKeys = true,
			.drain = &DrainSegmentEvents });
		const auto component = a_context.RegisterCoordinatorComponent({ .name = "object LOD segments",
			.make = &MakeState,
			.events = { .keys = &ApplySegmentEvents } });
		// Where Core's line was: the coordinator's segments point.
		a_context.RegisterReportLines({ .name = "object LOD segments",
			.point = Api::ReportPoint::SceneCoordinatorSegments,
			.component = component,
			.componentText = &SegmentLines });
	}
}
