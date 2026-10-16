// Interim (Fades stage F2): Core's fade code registered through the Fades contracts, from the end of InstallFeatures (Fades' place in
// the feature list); stage F3 moves it into Features/Fades.

#include "Features/DrawcallLimitFix/Api/Feature.h"
#include "Features/DrawcallLimitFix/Core/Extensions/RootStates.h"
#include "Features/DrawcallLimitFix/Engine/Capture/SceneCapture.h"
#include "Features/DrawcallLimitFix/Engine/Cull/PrimaryCull.h"
#include "Features/DrawcallLimitFix/Scene/Coordinator/FadeRoots.h"
#include "Features/DrawcallLimitFix/Scene/Ports/FrameGlobals.h"
#include "Features/DrawcallLimitFix/Scene/Ports/LightingConstants.h"

#include <bit>
#include <cstddef>

namespace DCLF::Scene
{
	// The engine side's registrations (Engine/Cull/FadeEngineRegistration.cpp).
	void RegisterFadeEngineChannels(Api::InstallContext& a_context, Api::EngineChannelId& a_snaps, Api::EngineChannelId& a_amounts);
	void RegisterFadeStandInObserver(Api::InstallContext& a_context);
}

namespace DCLF::Draws
{
	// The render side's (Core/Render/FadeRenderRegistration.cpp; the write-back's kick and wait in Payload/Uploads.cpp).
	void RegisterCoreFadeRender(Api::InstallContext& a_context);
	void KickFadeWriteBack();
	void WaitFadeWriteBack();
}

namespace DCLF::Scene
{
	namespace
	{
		// Install; read-only after.
		Api::TableFamilyId fadeRootFamily;
		Api::CoordinatorComponentId fadeRootComponent;
		Api::EngineChannelId fadeSnaps, fadeAmounts;

		// The record word the root reference rides in: BindlessObject::recordFlags, bits 8-31 the root + 1 (RecordFlagsOf's Core bits stay
		// below them).
		constexpr std::uint32_t kFadeRootRecordWord = static_cast<std::uint32_t>(offsetof(BindlessObject, recordFlags) / 4);
		static_assert(kRecordFadeRootShift == 8, "the fade roots' record reference is bits 8-31 of recordFlags");

		// The root test (per join): a fade node's fade-out distance for BuildDraws' fade test (measured from the node's centre); trees add
		// the height test.
		void RootTest(const Api::RootTestInput& a_input, Api::RootTestOutput& a_out)
		{
			a_out.distance = PrimaryCull::MembershipFadeDistance(*a_input.node);
			a_out.objectFlags = (a_input.node->kind & SceneCapture::kKindTree) ? kObjectHeightTest : 0u;
		}

		// The fade distances' globals (PrimaryCull::FadeDistanceOf) into the membership witness, in the order MembershipWitness mixed them.
		void RootGlobalsKey(const Api::RootTestInput& a_input, std::uint32_t& a_witness)
		{
			const auto& g = a_input.globals;
			const auto mix = [&](std::uint32_t a_value) { a_witness = (a_witness ^ a_value) * 16777619u; };
			mix(g.fadesOn | (g.fadeLodUpdates << 8) | (g.fadeSpecialA == g.fadeSpecialB ? 0x10000u : 0u));
			mix(std::bit_cast<std::uint32_t>(g.fadeDistanceMult));
			mix(std::bit_cast<std::uint32_t>(g.fadeOutThreshold));
			mix(std::bit_cast<std::uint32_t>(g.fadeDefaultScale));
			for (std::uint32_t type = 0; type < 13; ++type)
				mix(std::bit_cast<std::uint32_t>(g.fadeTypeDivisors[type]));
		}
	}

	Api::TableFamilyId FadeRootFamilyId()
	{
		return fadeRootFamily;
	}

	Api::CoordinatorComponentId FadeRootComponentId()
	{
		return fadeRootComponent;
	}

	Api::EngineChannelId FadeSnapsChannelId()
	{
		return fadeSnaps;
	}

	Api::EngineChannelId FadeAmountsChannelId()
	{
		return fadeAmounts;
	}

	void RegisterCoreFades(Api::InstallContext& a_context)
	{
		// The family: registered after TreeWind's trees, so the tables' report reads "trees kept a, fade roots kept b". The record's root
		// reference is (root + 1) << 8 in recordFlags, none past 24 bits (RecordFlagsOf's cut-off); a row carries its generation, so no tag.
		fadeRootFamily = a_context.RegisterTableFamily({ .name = "fade roots",
			.make = &MakeFadeRoots,
			.assign = &AssignFadeRoots,
			.same = &SameFadeRoots,
			.clear = &ClearFadeRoots,
			.recycle = &RecycleFadeRoot,
			.recordWord = kFadeRootRecordWord,
			.recordShift = 8,
			.recordBias = 1,
			.recordTag = false,
			.journaled = true,
			.rows = &FadeRootRows });
		// The component, last of the components: every shared point (ResetSlotTables' resets, then the owners' hand-back) keeps its order.
		fadeRootComponent = a_context.RegisterCoordinatorComponent(FadeRootComponent());
		// The channels: "fade snaps" then "fade amounts", drained and applied where Core's queues were (BeforeStructural).
		RegisterFadeEngineChannels(a_context, fadeSnaps, fadeAmounts);
		// The render side: the seeds pool, the scene resources, the depth commit hook, the stages, the frame and cull bindings.
		Draws::RegisterCoreFadeRender(a_context);
		// The root states: the family's rows, the root test per join and its globals in the membership witness, and the write-back task
		// (kicked at the frame's start, joined when DCLF's async work drains).
		a_context.RegisterRootStates({ .name = "fade roots",
			.family = fadeRootFamily,
			.view = &ViewFadeRoots,
			.objectFlags = kObjectFadeTest | kObjectHeightTest,
			.test = &RootTest,
			.globalsKey = &RootGlobalsKey,
			.kick = &Draws::KickFadeWriteBack,
			.wait = &Draws::WaitFadeWriteBack });
		// The stand-in observer, last (Main::Update's entry keeps its order: the writer tables and stores after the other observers').
		RegisterFadeStandInObserver(a_context);
	}
}
