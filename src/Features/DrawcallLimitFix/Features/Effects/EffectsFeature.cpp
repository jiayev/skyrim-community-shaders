// Effects: the effect-shader objects DCLF draws through the Effect port. Its switches and the effects toggle, the effect and opaque effect
// object classes' contributions, the Effect port's registrations and write hooks, the report lines and the parity.

#include "Features/DrawcallLimitFix/Features/Effects/Effects.h"
#include "Features/DrawcallLimitFix/Features/Effects/EffectDescriptors.h"
#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"
#include "Features/DrawcallLimitFix/Diagnostics/Observation.h"

namespace DCLF::Features::Effects
{
	namespace
	{
		Api::ToggleId effectsToggle;  // install

		void Install(Api::InstallContext& a_context)
		{
			// Where Core's were listed: the effects after the forward epoch's; the opaque effects after the primary exclusion's (behind
			// Features/TreeWind's list-all, which registers before this feature), with Features/OcclusionMaps', Features/Reflection's and
			// Features/FirstPerson's switches listed before it and Features/Billboards' particles after it.
			const auto effects = a_context.RegisterSwitch({ .name = "CS_DCLF_EFFECTS", .kind = Api::SwitchKind::Feature, .reduced = Api::SwitchReduced::UnlessOne,
				.description = "1: static blended BSEffectShaderProperty meshes drawn by DCLF's forward epoch, sorted with blended Lighting (T7d's Effect port; needs CS_DCLF_FORWARD); 0 they stay native (live toggle's seed)",
				.after = "CS_DCLF_FORWARD" });
			const auto opaque = a_context.RegisterSwitch({ .name = "CS_DCLF_OPAQUE_EFFECTS", .kind = Api::SwitchKind::Feature, .reduced = Api::SwitchReduced::Zero,
				.description = "default on (T7f item 7): opaque BSEffectShaderProperty meshes (hint 0: no blending alpha property, no Falloff, depth tested and written; forge coals, slag) drawn by DCLF in the G-buffer with the DEFERRED effect variant, through the opaque decal group's fixed slots and its decal depth pass without the bias (needs CS_DCLF_EFFECTS and CS_DCLF_DECALS); 0 they stay the engine's (effect-opaque)",
				.after = "CS_DCLF_PRIMARY_EXCLUDE" });
			// Static blended BSEffectShaderProperty meshes, drawn by the forward epoch in the same GPU sort (needs forward; on by default:
			// CS_DCLF_EFFECTS=0 seeds it off): an object class. Right after the forward epoch's, where Core's was (before Features/Particles'
			// and Features/Billboards', which need it and register after this feature).
			effectsToggle = a_context.RegisterToggle({ .name = "effects", .label = "Effect meshes: clouds, glows, mist",
				.tooltip = "T7d: DCLF draws blended effect-shader meshes in the forward epoch, sorted with the blended Lighting objects, and (with Decals) effect decals in its decal groups, skinned ones (with Skinned) through the skin palette. On by default (validated w174/w175; decals and skinned effects not yet).",
				.seed = effects, .entersClassification = true, .after = "forward", .needs = { "forward" } });
			a_context.RegisterObjectClass({ .objectClass = Api::ObjectClass::Effect, .toggle = effectsToggle });
			// The opaque effects (read once, here): taken unless CS_DCLF_OPAQUE_EFFECTS is 0, read whatever the toggle (the Effect port runs
			// only while the effects are on; the decals' toggle is its own test).
			if (SwitchValue(opaque) != "0")
				a_context.RegisterObjectClass({ .objectClass = Api::ObjectClass::OpaqueEffect, .toggle = effectsToggle });
			// The variants' Light Limit Fix (Integrations/LightLimitFix's input, by name: the integrations install after the features).
			NeedLightLimitFix(a_context);
			// The Effect port, after its class: the material port (kind 1), its program family, its frame constant rows and soft depth.
			RegisterPort(a_context);
			RegisterPrograms(a_context);
			RegisterFrame(a_context);
			RegisterReport(a_context);
			// The port's material-write hooks (once Core's Lighting write hooks are in: Core/Shading/MaterialWrites).
			InstallWrites();
			// CS_DCLF_SET_PARITY: the members' withheld Lighting-variant passes.
			if (Diagnostics::Observe().claims)
				SubscribeLight();
		}
	}

	Api::Feature Feature()
	{
		return { "effects", &Install };
	}

	Api::ToggleId Toggle()
	{
		return effectsToggle;
	}
}
