// The sun exclusion's watch (SunCasters.h): at the sun's cascade registration, render thread.

#include "SunCasters.h"

#include "Events.h"
#include "SunAccumulationView.h"
#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"

namespace DCLF::Diagnostics::SunCasters
{
	namespace
	{
		Watch watch;

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

	void Install()
	{
		Subscribe<SunExclusionApplied>(Applied);
		Subscribe<SunCasterRegistered>(Registered);
	}

	Watch TakeWatch()
	{
		return std::exchange(watch, {});
	}
}
