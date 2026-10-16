// Interim (Fades stage F2): the engine side of Core's fade code registered through the generic contracts, called from
// Scene/Coordinator/FadeRegistration.cpp (RegisterCoreFades, Fades' place in the feature list: last). The fade snaps and fade amounts
// engine channels (their queues are SceneEvents.h's, the snap hook stays in Core's scene events), and the fade writers' stand-in observer
// (PrimaryCull: the writer tables and stores at Main::Update's entry, the animated batch at the list jobs' Finish). Stage F3 moves them
// into Features/Fades.

#include "Features/DrawcallLimitFix/Api/Feature.h"
#include "Features/DrawcallLimitFix/Core/Extensions/EngineEvents.h"
#include "Features/DrawcallLimitFix/Engine/Capture/SceneEvents.h"
#include "Features/DrawcallLimitFix/Engine/Cull/PrimaryCull.h"

namespace DCLF::Scene
{
	namespace
	{
		std::size_t DrainFadeSnaps(Api::KeySink& a_sink)
		{
			return fadeSnapEvents.Drain(a_sink);
		}

		std::size_t DrainFadeAmounts(Api::KeySink& a_sink)
		{
			return fadeAmountEvents.Drain(a_sink);
		}

		// Features/Actors' Actor::SetAlpha, through Extensions::PushChannelKey: Core's fadeAmount event (the fade roots' component's
		// RefreshFadeAmount, FadeRoots.cpp, rereads the node) and, for a fade node, the mirror's value.
		void PushFadeAmount(const void* a_key)
		{
			auto* root = static_cast<RE::NiAVObject*>(const_cast<void*>(a_key));
			fadeAmountEvents.Push(a_key);
			if (root->AsFadeNode())
				PushNodeUpdate(root, SceneCapture::NodeRecord::kFadeAmount);  // the mirror's value
		}

		void WriteFadeWriters(bool a_running)
		{
			PrimaryCull::Get().WriteFadeWriters(a_running);
		}

		void DrainAnimatedFades()
		{
			PrimaryCull::Get().DrainAnimatedFades();
		}
	}

	void RegisterFadeEngineChannels(Api::InstallContext& a_context, Api::EngineChannelId& a_snaps, Api::EngineChannelId& a_amounts)
	{
		// Drained and applied where Core's were: snaps, then amounts, before the property events (the other BeforeStructural channels
		// after them: none today). A load screen keeps both named for the mirror's parity; a normal pass's keys are not named (their
		// writers push the mirror's value). No wake: a snap comes with its cell's attach, an amount every frame of a fade.
		a_snaps = a_context.RegisterEngineChannel({ .name = "fade snaps",
			.kind = Api::ChannelKind::Keys,
			.apply = Api::ChannelApply::BeforeStructural,
			.loading = Api::ChannelLoading::KeepKeys,
			.drain = &DrainFadeSnaps });
		a_amounts = a_context.RegisterEngineChannel({ .name = "fade amounts",
			.kind = Api::ChannelKind::Keys,
			.apply = Api::ChannelApply::BeforeStructural,
			.loading = Api::ChannelLoading::KeepKeys,
			.drain = &DrainFadeAmounts,
			.push = &PushFadeAmount });
	}

	void RegisterFadeStandInObserver(Api::InstallContext& a_context)
	{
		// Last of the observers, so Main::Update's entry keeps its order: kAccumulated, the other observers' updateEntry (Portals' live
		// flag), the writer tables and stores, then the UpdateEntry event and afterWrites. Without the primary cull neither point runs.
		a_context.RegisterStandInObserver({ .updateEntry = &WriteFadeWriters, .listJobsDone = &DrainAnimatedFades });
	}
}
