// Fades: the fade roots' OnVisible state, the one producer of root states. Its switches, the fade roots' table family and coordinator
// component, the fade snaps and fade amounts channels, the render side (the seeds pool, the scene resources with their depth commit hook,
// the passes, the frame and cull bindings), the root states (the family's view, the root test, the write-back's kick and wait), the
// diagnostics, the fade writers (the stand-in observer and engine hooks, last) and the early hook (FadeSnap).

// FadeScene.h first: volk before every Vulkan header (the write-back's kick and wait).
#include "Features/DrawcallLimitFix/Features/Fades/FadeScene.h"
#include "Features/DrawcallLimitFix/Features/Fades/Fades.h"
#include "Features/DrawcallLimitFix/Features/Fades/FadesInternal.h"

#include "Features/DrawcallLimitFix/Core/Extensions/RootStates.h"
#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"
#include "Features/DrawcallLimitFix/Core/Engine/Capture/SceneCapture.h"
#include "Features/DrawcallLimitFix/Features/Fades/FadeRoots.h"
#include "Features/DrawcallLimitFix/Features/Fades/FadeWriters.h"
#include "Features/DrawcallLimitFix/Core/Shading/FrameGlobals.h"

#include <bit>

namespace DCLF::Scene
{
	namespace
	{
		Api::EngineChannelId fadeSnaps, fadeAmounts;  // install; read-only after
	}

	Api::EngineChannelId FadeSnapsChannelId()
	{
		return fadeSnaps;
	}

	Api::EngineChannelId FadeAmountsChannelId()
	{
		return fadeAmounts;
	}
}

namespace DCLF::Features::Fades
{
	namespace
	{
		Api::SwitchId emptyFadeNodeSwitch, fadeParitySwitch;  // install

		// The switches, where Core's were listed: the empty fade node's right before the render graph's (after the primary exclusion's and
		// the switches anchored after it), the parity's after the resident parity's.
		void RegisterSettings(Api::InstallContext& a_context)
		{
			emptyFadeNodeSwitch = a_context.RegisterSwitch({ .name = "CS_DCLF_EMPTY_FADE_NODE", .kind = Api::SwitchKind::Feature, .reduced = Api::SwitchReduced::Zero,
				.description = "default on (T7f item 7): the engine's empty fade node (the BSFadeNode 0x14332a2a0 holds, which every BSShaderProperty starts with and the engine reads as none) is no fade root: such geometry (terrain, object LOD, some clutter) takes its entry's root; 0: it is listed as one fade root of all of them, outside the scene graph",
				.before = "CS_ORG_EPOCHS" });
			fadeParitySwitch = a_context.RegisterSwitch({ .name = "CS_DCLF_FADE_PARITY", .kind = Api::SwitchKind::Parity, .reduced = Api::SwitchReduced::Never,
				.description = "1: the fade port against the engine's functions, and FadeStateCS against the port", .after = "CS_DCLF_RESIDENT_PARITY" });
		}

		// The root test (per join): a fade node's fade-out distance for BuildDraws' fade test (measured from the node's centre); trees add
		// the height test.
		void RootTest(const Api::RootTestInput& a_input, Api::RootTestOutput& a_out)
		{
			a_out.distance = FadeDistanceOf(*a_input.node);
			a_out.objectFlags = (a_input.node->kind & SceneCapture::kKindTree) ? kObjectHeightTest : 0u;
		}

		// The fade distances' globals (FadeDistanceOf) into the membership witness, in the order MembershipWitness mixed them.
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

		// The root states: the family's rows, the root test per join and its globals in the membership witness, and the write-back task
		// (kicked at the frame's start, joined when DCLF's async work drains).
		void RegisterRootStates(Api::InstallContext& a_context)
		{
			a_context.RegisterRootStates({ .name = "fade roots",
				.family = Scene::FadeRootFamilyId(),
				.view = &Scene::ViewFadeRoots,
				.objectFlags = kObjectFadeTest | kObjectHeightTest,
				.test = &RootTest,
				.globalsKey = &RootGlobalsKey,
				.kick = &KickFadeWriteBack,
				.wait = &WaitFadeWriteBack });
		}

		void Install(Api::InstallContext& a_context)
		{
			RegisterSettings(a_context);
			// The family (after TreeWind's trees) and the component, last of the components.
			RegisterScene(a_context);
			// The channels: "fade snaps" then "fade amounts", drained and applied where Core's queues were (BeforeStructural).
			RegisterEngine(a_context, Scene::fadeSnaps, Scene::fadeAmounts);
			// The render side: the seeds pool, the scene resources, the depth commit hook, the passes, the frame and cull bindings.
			RegisterRender(a_context);
			RegisterRootStates(a_context);
			RegisterDiagnostics(a_context);
			// The fade writers, last (Main::Update's entry keeps its order: the writer tables and stores after the other observers').
			RegisterWriters(a_context);
		}
	}

	Api::Feature Feature()
	{
		return { .name = "fades", .install = &Install, .earlyHooks = &InstallEarlyHooks };
	}

	bool FadeParity()
	{
		return SwitchEnabled(fadeParitySwitch);
	}

	bool EmptyFadeNodeIsNone()
	{
		return SwitchValue(emptyFadeNodeSwitch) != "0";
	}
}
