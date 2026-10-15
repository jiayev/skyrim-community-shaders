// FirstPerson: the player's first-person skeleton drawn by DCLF as a view of the one scene. Its switches and the first person toggle, the
// view's contribution, its report lines and the skeleton's parity.

#include "Features/DrawcallLimitFix/Features/FirstPerson/FirstPerson.h"
#include "Features/DrawcallLimitFix/Core/Render/IndirectDraws.h"
#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"
#include "Features/DrawcallLimitFix/Diagnostics/Observation.h"

#include <string>

namespace DCLF::Features::FirstPerson
{
	namespace
	{
		// The view's lines (its epochs, its forward part, the engine's registrations of its members): Core's view state, taken here.
		void Lines(std::string& a_text)
		{
			a_text += IndirectDraws::Get().FirstPersonReport();
		}

		void Install(Api::InstallContext& a_context)
		{
			// Where Core's were listed: after the reflections'.
			const auto firstPersonSwitch = a_context.RegisterSwitch({ .name = "CS_DCLF_FIRST_PERSON", .kind = Api::SwitchKind::Feature, .reduced = Api::SwitchReduced::UnlessOne,
				.description = "1: the player's first-person arms, weapons and magic hands drawn by DCLF as a view of the one scene (T7e: kSetFirstPerson members, the view bit kViewFirstPerson at the GPU cull, a depth epoch in RenderDepth and a colour epoch at Main::RenderFirstPersonView); 0 they stay native (live toggle's seed)",
				.after = "CS_DCLF_REFLECTIONS" });
			const auto decalsSwitch = a_context.RegisterSwitch({ .name = "CS_DCLF_FIRST_PERSON_DECALS", .kind = Api::SwitchKind::Feature, .reduced = Api::SwitchReduced::Zero,
				.description = "default on (T7f item 7): a decal on a first-person entry (an effect's skinned blood decal on the arms, a weapon's BloodLighting) drawn by the first-person forward part as a forward object with its decal group's depth bias, test and blend (needs CS_DCLF_FIRST_PERSON and CS_DCLF_DECALS); 0 it stays the engine's",
				.after = "CS_DCLF_REFLECTIONS" });
			// The player's first-person skeleton (arms, weapons, shield, magic hands) as a view of the one scene: its geometry tracked under the
			// first-person root, members of kSetFirstPerson drawn by the first-person epochs (on by default: CS_DCLF_FIRST_PERSON=0 seeds it
			// off). After the reflections, last; it needs the skinned and the actors (the player's skeleton).
			const auto firstPerson = a_context.RegisterToggle({ .name = "first person", .label = "Draw the first-person arms and weapons (CS_DCLF_FIRST_PERSON)",
				.tooltip = "T7e: DCLF draws the first-person skeleton's geometry (arms, weapons, shield, magic hands) as a view of its scene, with the first-person camera, in the engine's depth and first-person passes. On by default (validated fp6).",
				.section = "First person", .seed = firstPersonSwitch, .entersClassification = true, .after = "reflections", .needs = { "skinned", "actors" } });
			// The view (Core's view machinery: its epochs, revisions, frame requests and classification read the toggle as data), and with
			// CS_DCLF_FIRST_PERSON_DECALS (read once, here) a decal on a first-person entry as a forward object of its forward part.
			a_context.RegisterView({ .view = Api::EngineView::FirstPerson, .toggle = firstPerson, .forwardDecals = SwitchValue(decalsSwitch) != "0" });
			// After Features/Particles' and Features/Billboards' lines, where Core printed the view's.
			a_context.RegisterReportLines({ .name = "first person", .point = Api::ReportPoint::AsyncWork, .text = &Lines });
			// CS_DCLF_SET_PARITY: the skeleton's slots and claimed phases.
			if (Diagnostics::Observe().claims)
				SubscribeSkeletonParity();
		}
	}

	Api::Feature Feature()
	{
		return { "first person", &Install };
	}
}
