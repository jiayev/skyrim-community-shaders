// Interim (Fades stage F2): Core's fade roots' render parts registered through the Fades contracts, from Scene::RegisterCoreFades (Fades'
// place in the feature list): the seeds pool, the scene resources (FadeScene) with their fit, the depth commit hook, the GPU stages at
// DepthSegmentCullState, the frame bindings t120/t119 and the Root* cull bindings Core's cull reads. Each part's code stays where it
// was (Resources.cpp, Common.cpp, Requirements.cpp, Uploads.cpp, SceneState.cpp, FrameValues.cpp); stage F3 moves them into
// Features/Fades.

// Internal.h first: volk must precede every Vulkan header of the translation unit.
#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Api/Feature.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Commits.h"
#include "Features/DrawcallLimitFix/Core/Extensions/SceneResources.h"

namespace DCLF::Draws
{
	namespace
	{
		Api::SceneResourcesId sceneId;  // install

		// What the versions a set names hold of the roots (the root states' family's rows): one past them waits for their growth. Without
		// the fade program no draw reads them.
		void FitFadeScene(Api::SceneResourcesFit& a_fit)
		{
			const auto& fades = a_fit.State<FadeScene>();
			const auto family = Extensions::RootStatesFamily();
			if (!fades.fadeState || !fades.fadeRoots || family.index >= Extensions::kTableFamilySlots)
				return;
			const auto units = [&](const Versioned& a_buffer) {
				return static_cast<std::uint32_t>(std::min<std::uint64_t>(NamedUnits(a_fit.sealed, a_buffer), UINT32_MAX));
			};
			a_fit.fit.familyRows[family.index] = std::min({ units(fades.fadeRoots), units(fades.fadeStatesOut[0]), units(fades.fadeStatesOut[1]) });
		}

		// t120: the states a frame's draws read (the previous frame's published); t119: the roots' static rows.
		void SelectStatesBinding(Api::FrameBindingSelect& a_select)
		{
			if (const auto* fades = FadeSceneOf(a_select.scene))
				a_select.buffer = &fades->FadeStatesRead(a_select.frame);
		}

		void SelectRootsBinding(Api::FrameBindingSelect& a_select)
		{
			if (const auto* fades = FadeSceneOf(a_select.scene))
				a_select.buffer = &fades->fadeRoots;
		}

		// The cull bindings: each root's program, the roots' rows (current while the depth commit sent the tables' roots), the states a
		// frame's cull reads, the states the step wrote, the seeds (the pool's backing) and the step's frame row.
		void SelectRootPrograms(Api::CullBindingSelect& a_select)
		{
			if (const auto* fades = FadeSceneOf(a_select.scene))
				a_select.Versioned(fades->fadeRootPrograms);
		}

		void SelectRootRows(Api::CullBindingSelect& a_select)
		{
			if (const auto* fades = FadeSceneOf(a_select.scene)) {
				a_select.Versioned(fades->fadeRoots);
				a_select.Rows(fades->fadeRootCount);
			}
		}

		void SelectRootStates(Api::CullBindingSelect& a_select)
		{
			if (const auto* fades = FadeSceneOf(a_select.scene))
				a_select.Versioned(fades->FadeStatesRead(a_select.frame));
		}

		void SelectRootStepStates(Api::CullBindingSelect& a_select)
		{
			if (const auto* fades = FadeSceneOf(a_select.scene))
				a_select.Versioned(fades->fadeStates);
		}

		void SelectRootSeeds(Api::CullBindingSelect& a_select)
		{
			a_select.Index(FrameValues::Get().FadeSeedsIndex());
		}

		void SelectRootFrame(Api::CullBindingSelect& a_select)
		{
			if (const auto* fades = FadeSceneOf(a_select.scene))
				a_select.Buffer(fades->fadeFrameBuffer);
		}
	}

	FadeScene* FadeSceneOf(const SceneBuffers& a_scene)
	{
		return Extensions::SceneResourcesOf<FadeScene>(a_scene, sceneId);
	}

	std::shared_ptr<FadeScene> SharedFadeSceneOf(const SceneBuffers& a_scene)
	{
		return Extensions::SharedSceneResourcesOf<FadeScene>(a_scene, sceneId);
	}

	void RegisterCoreFadeRender(Api::InstallContext& a_context)
	{
		// The seeds: the last frame value pool (after every feature's).
		FrameValues::RegisterFadeSeeds(a_context);
		// The buffers, after every feature's scene resources (the visibility readback, made after them, reads their rows).
		sceneId = a_context.RegisterSceneResources({ .name = "fade roots", .setup = &SetUpFadeScene, .registerGraph = &RegisterFadeSceneGraph,
			.require = &RequireFadeScene, .fit = &FitFadeScene });
		// The depth commit's rows, inputs, write-back and root programs, after every feature's hook (Portals' programs among them).
		a_context.RegisterCommitHook({ .name = "fade roots", .commit = &CommitFadeRoots });
		RegisterFadeStages(a_context);
		// Named as the named-address parity names them.
		a_context.RegisterFrameBinding({ .name = "fade states SRV", .textureRegister = kFadeStatesRegister, .select = &SelectStatesBinding });
		a_context.RegisterFrameBinding({ .name = "fade roots SRV", .textureRegister = kFadeRootsRegister, .select = &SelectRootsBinding });
		a_context.RegisterCullBinding({ .slot = Api::CullBindingSlot::RootPrograms, .name = "cs.dclf.fade-root-programs", .select = &SelectRootPrograms });
		a_context.RegisterCullBinding({ .slot = Api::CullBindingSlot::RootRows, .name = "cs.dclf.fade-roots", .select = &SelectRootRows });
		a_context.RegisterCullBinding({ .slot = Api::CullBindingSlot::RootStates, .name = "cs.dclf.fade-states-out", .select = &SelectRootStates });
		a_context.RegisterCullBinding({ .slot = Api::CullBindingSlot::RootStepStates, .name = "cs.dclf.fade-states", .select = &SelectRootStepStates });
		a_context.RegisterCullBinding({ .slot = Api::CullBindingSlot::RootSeeds, .name = "cs.dclf.frame-values.fade-seeds", .select = &SelectRootSeeds });
		a_context.RegisterCullBinding({ .slot = Api::CullBindingSlot::RootFrame, .name = "cs.dclf.fade-frame", .select = &SelectRootFrame });
	}
}
