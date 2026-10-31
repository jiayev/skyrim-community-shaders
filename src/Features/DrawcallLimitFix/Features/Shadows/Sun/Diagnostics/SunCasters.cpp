// The sun exclusion's watch (SunCasters.h): at the sun's cascade registration, render thread.

#include "Features/DrawcallLimitFix/Features/Shadows/Sun/Diagnostics/SunCasters.h"

#include "Features/DrawcallLimitFix/Diagnostics/Engine/Events.h"
#include "Features/DrawcallLimitFix/Features/Shadows/Sun/Diagnostics/SunAccumulationView.h"
#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"

namespace DCLF::Features::Shadows::Sun::SunCasters
{
	namespace
	{
		Watch watch;

		void Applied(const Diagnostics::SunExclusionApplied& a_event)
		{
			watch.frames += a_event.skipExcluded ? 0 : 1;
		}

		void Registered(const Diagnostics::SunCasterRegistered& a_event)
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
		Diagnostics::Subscribe<Diagnostics::SunExclusionApplied>(Applied);
		Diagnostics::Subscribe<Diagnostics::SunCasterRegistered>(Registered);
	}

	Watch TakeWatch()
	{
		return std::exchange(watch, {});
	}
}
