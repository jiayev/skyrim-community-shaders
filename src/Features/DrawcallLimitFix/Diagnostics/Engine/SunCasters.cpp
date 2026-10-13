#include "SunCasters.h"

#include "Events.h"
#include "SunAccumulationView.h"
#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"

namespace DCLF::Diagnostics::SunCasters
{
	namespace
	{
		bool recording = false;
		std::vector<SunAccumulation::CascadeRegistration> recorded;  // the Accumulate in progress
		std::vector<SunAccumulation::CascadeRegistration> claimed;
		std::uint32_t claimedFrame = ~0u;
		Watch watch;

		void Begin(const SunAccumulateBegin& a_event)
		{
			recording = (a_event.frame % 30) == 0;
			recorded.clear();
		}

		void OnClaimed(const SunCasterClaimed& a_event)
		{
			if (recording)
				recorded.push_back({ a_event.geometry, a_event.descriptor });
		}

		void End(const SunAccumulateEnd& a_event)
		{
			if (!recording)
				return;
			claimed = std::move(recorded);
			recorded.clear();
			claimedFrame = a_event.frame;
			recording = false;
		}

		void Applied(const SunExclusionApplied& a_event)
		{
			watch.frames += a_event.skipExcluded ? 0 : 1;
		}

		void Registered(const SunCasterRegistered& a_event)
		{
			if (SunAccumulationView::SkipExcluded() || !SunAccumulationView::ExclusionLive() || !SunAccumulationView::UnderExcludedEntry(a_event.geometry))
				return;
			++watch.lost;
			if (watch.first.empty()) {
				watch.first = fmt::format("'{}'", a_event.geometry->name.c_str());
				watch.firstGeometry = a_event.geometry;
			}
		}
	}

	void Install(bool a_claims, bool a_watch)
	{
		if (a_claims) {
			Subscribe<SunAccumulateBegin>(Begin);
			Subscribe<SunCasterClaimed>(OnClaimed);
			Subscribe<SunAccumulateEnd>(End);
		}
		if (a_watch) {
			Subscribe<SunExclusionApplied>(Applied);
			Subscribe<SunCasterRegistered>(Registered);
		}
	}

	const std::vector<SunAccumulation::CascadeRegistration>& Claimed(std::uint32_t& a_frame)
	{
		a_frame = claimedFrame;
		return claimed;
	}

	Watch TakeWatch()
	{
		return std::exchange(watch, {});
	}
}
