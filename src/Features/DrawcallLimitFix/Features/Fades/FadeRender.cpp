// Fades' render side registered (RegisterRender, from the feature's install after its family, component and channels): the seeds pool,
// the scene resources, the depth commit hook, the passes at the depth epoch's cull state, the frame bindings t120/t119 the draws read and
// the Root* cull bindings Core's cull reads; and t117, the seeds the draws' alpha falls back to (DCLFAlphaFade).

#include "Features/DrawcallLimitFix/Features/Fades/FadeScene.h"
#include "Features/DrawcallLimitFix/Features/Fades/FadesInternal.h"
#include "Features/DrawcallLimitFix/Core/Extensions/CullInputs.h"
#include "Features/DrawcallLimitFix/Core/Extensions/FrameBindings.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelines.h"

namespace DCLF::Features::Fades
{
	namespace
	{
		// t120: the states a frame's draws read (the previous frame's published); t119: the roots' static rows.
		void SelectStatesBinding(Api::FrameBindingSelect& a_select)
		{
			if (const auto* fades = SceneOf(a_select.scene))
				a_select.buffer = &fades->FadeStatesRead(a_select.frame);
		}

		void SelectRootsBinding(Api::FrameBindingSelect& a_select)
		{
			if (const auto* fades = SceneOf(a_select.scene))
				a_select.buffer = &fades->fadeRoots;
		}

		// t117: the seeds the frame reads (a frame value pool's backing: the frame record's commit names it).
		void SelectSeedsBinding(Api::FrameBindingSelect& a_select)
		{
			if (a_select.renderThread && SceneOf(a_select.scene))
				a_select.index = SeedsIndex();
		}

		// The cull bindings: each root's program, the roots' rows (current while the depth commit sent the tables' roots), the states a
		// frame's cull reads, the states the step wrote, the seeds (the pool's backing) and the step's frame row.
		void SelectRootPrograms(Api::CullBindingSelect& a_select)
		{
			if (const auto* fades = SceneOf(a_select.scene))
				a_select.Versioned(fades->fadeRootPrograms);
		}

		void SelectRootRows(Api::CullBindingSelect& a_select)
		{
			if (const auto* fades = SceneOf(a_select.scene)) {
				a_select.Versioned(fades->fadeRoots);
				a_select.Rows(fades->fadeRootCount);
			}
		}

		void SelectRootStates(Api::CullBindingSelect& a_select)
		{
			if (const auto* fades = SceneOf(a_select.scene))
				a_select.Versioned(fades->FadeStatesRead(a_select.frame));
		}

		void SelectRootStepStates(Api::CullBindingSelect& a_select)
		{
			if (const auto* fades = SceneOf(a_select.scene))
				a_select.Versioned(fades->fadeStates);
		}

		void SelectRootSeeds(Api::CullBindingSelect& a_select)
		{
			a_select.Index(SeedsIndex());
		}

		void SelectRootFrame(Api::CullBindingSelect& a_select)
		{
			if (const auto* fades = SceneOf(a_select.scene))
				a_select.Buffer(fades->fadeFrameBuffer);
		}
	}

	void RegisterRender(Api::InstallContext& a_context)
	{
		// The seeds: the last frame value pool (after every feature's).
		RegisterSeeds(a_context);
		// The buffers, after every feature's scene resources (the visibility readback, made after them, reads their rows).
		RegisterResources(a_context);
		// The depth commit's rows, inputs, write-back and root programs, after every feature's hook (Portals' programs among them).
		RegisterCommit(a_context);
		RegisterPasses(a_context);
		// Named as the named-address parity names them.
		a_context.RegisterFrameBinding({ .name = "fade states SRV", .textureRegister = kFadeStatesRegister, .select = &SelectStatesBinding });
		a_context.RegisterFrameBinding({ .name = "fade roots SRV", .textureRegister = kFadeRootsRegister, .select = &SelectRootsBinding });
		a_context.RegisterFrameBinding({ .name = "fade seeds SRV", .textureRegister = kFadeSeedsRegister, .select = &SelectSeedsBinding });
		a_context.RegisterCullBinding({ .slot = Api::CullBindingSlot::RootPrograms, .name = "cs.dclf.fade-root-programs", .select = &SelectRootPrograms });
		a_context.RegisterCullBinding({ .slot = Api::CullBindingSlot::RootRows, .name = "cs.dclf.fade-roots", .select = &SelectRootRows });
		a_context.RegisterCullBinding({ .slot = Api::CullBindingSlot::RootStates, .name = "cs.dclf.fade-states-out", .select = &SelectRootStates });
		a_context.RegisterCullBinding({ .slot = Api::CullBindingSlot::RootStepStates, .name = "cs.dclf.fade-states", .select = &SelectRootStepStates });
		a_context.RegisterCullBinding({ .slot = Api::CullBindingSlot::RootSeeds, .name = "cs.dclf.frame-values.fade-seeds", .select = &SelectRootSeeds });
		a_context.RegisterCullBinding({ .slot = Api::CullBindingSlot::RootFrame, .name = "cs.dclf.fade-frame", .select = &SelectRootFrame });
	}
}
