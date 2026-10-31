// The one ordered list of DCLF's features (see the header). A feature's place is the order its registrations run in beside the others':
// its pools after the earlier features' pools, its passes' setups after theirs, its products after theirs
// (where its passes run is the schedule's: Schedule/Frame.cpp).

#include "Features/DrawcallLimitFix/Features/FeatureList.h"
#include "Features/DrawcallLimitFix/Features/Actors/Actors.h"
#include "Features/DrawcallLimitFix/Features/Billboards/Billboards.h"
#include "Features/DrawcallLimitFix/Features/Decals/Decals.h"
#include "Features/DrawcallLimitFix/Features/Effects/Effects.h"
#include "Features/DrawcallLimitFix/Features/Fades/Fades.h"
#include "Features/DrawcallLimitFix/Features/Faces/Faces.h"
#include "Features/DrawcallLimitFix/Features/FirstPerson/FirstPerson.h"
#include "Features/DrawcallLimitFix/Features/Forward/Forward.h"
#include "Features/DrawcallLimitFix/Features/GpuOcclusion/GpuOcclusion.h"
#include "Features/DrawcallLimitFix/Features/Grass/Grass.h"
#include "Features/DrawcallLimitFix/Features/HiddenNodes/HiddenNodes.h"
#include "Features/DrawcallLimitFix/Features/Landscape/Landscape.h"
#include "Features/DrawcallLimitFix/Features/LocalLights/LocalLights.h"
#include "Features/DrawcallLimitFix/Features/Lod/Lod.h"
#include "Features/DrawcallLimitFix/Features/MainView/MainView.h"
#include "Features/DrawcallLimitFix/Features/OcclusionMaps/OcclusionMaps.h"
#include "Features/DrawcallLimitFix/Features/Particles/Particles.h"
#include "Features/DrawcallLimitFix/Features/Portals/Portals.h"
#include "Features/DrawcallLimitFix/Features/Reflection/Reflection.h"
#include "Features/DrawcallLimitFix/Features/Shadows/Engine/Engine.h"
#include "Features/DrawcallLimitFix/Features/Shadows/Local/Local.h"
#include "Features/DrawcallLimitFix/Features/Shadows/Sun/Sun.h"
#include "Features/DrawcallLimitFix/Features/Skins/Skins.h"
#include "Features/DrawcallLimitFix/Features/SwitchNodes/SwitchNodes.h"
#include "Features/DrawcallLimitFix/Features/TextureTransforms/TextureTransforms.h"
#include "Features/DrawcallLimitFix/Features/TreeLod/TreeLod.h"
#include "Features/DrawcallLimitFix/Features/TreeWind/TreeWind.h"

#include <array>

namespace DCLF::Features
{
	std::span<const Api::Feature> List()
	{
		// Portals first: its early hooks (Main::Update's portal walk, the call site the primary cull requires) before every other feature's,
		// and its install's room visibility (its install log right after the primary cull's, where the shell installed it) and stand-in
		// observer (Main::Update's entry, before the others', where the shell set it live); it registers the main cull input and no
		// switch, toggle, pool, pass or product.
		// The shadow epochs next (Shadows/Engine), before the features that build on them (DEPENDS: the occlusion maps, the shadow sources):
		// their passes (shadows.*, no setup: a Shadow epoch's pass has none, so their place among the registrations changes no order), the
		// shadow epochs' handlers (the only ones) and their triggers (found by name); no switch, toggle, hook, pool, product or report line,
		// so the occlusion maps' toggles stay the first registered.
		// The occlusion maps next: their toggles are the first registered, so their bits in the packed toggle word are where Core's were
		// (right after Core's flags), and their switches and toggles are listed before the reflection's and the first person's (which follow
		// them there); no early hooks, no pool, pass or product. The shadow sources next, local then sun (Shadows/Local, Shadows/Sun: the
		// sun's registration thunk calls the point culls' NoteMainRegistration, DEPENDS), then the local lights (LocalLights, after Portals:
		// its selection's room walks, DEPENDS): they register their sources (resolved by source, whatever the order) and source hooks
		// (installed by source: the sun's accumulation and camera, the light selection, then the point lights' culls and the focus cameras,
		// whatever the list's order); the sun also its switches and toggles (anchored after the shadows'), the local lights their light
		// volumes and parity; Local registers no toggle, switch or shared-point line, so its move before the sun changes no order; no early
		// hook, pool, pass or product.
		// Reflection next: its early hooks (the water reflection's update, add-roots and face render) run right after Portals', before every
		// other feature's and Core's observers, where the shell installed them; its toggle's bit follows
		// the maps', where Core's was; its switch and toggle are listed after the precipitation mask's, before the first person's (which
		// follow them there); it registers no pool, pass or product. Skins next: before the features that read skins (DEPENDS: Effects,
		// first person); its switches are listed after the culling's, the skin partitions' after the skinned's (the actors', tree wind's and
		// the decals' follow it there), its skinned toggle before the projected's (tree wind's and the decals' follow it there) and its skin
		// partitions' after the tree LOD's (the actors' follow it there); its toggles' bits follow the reflection's; its report line is the
		// first at the coordinator's last pass (before the decals'); no early hook, pool, pass or product. Actors next: its early hooks
		// (Actor::SetAlpha and the actors' move writers) right after the reflection's, its three move writers reserved first (slots 2-4,
		// before the particles'); its switch is listed after the skin partitions', before tree wind's and the decals' (which follow it
		// there); it registers no pool, pass or product. Tree wind next:
		// its seeds' pool runs right after the frame values' built-in parts, before the texture transforms' pool.
		// The main view before forward: its draw passes (main-view.*, no setup: their place among the registrations changes no order) and the
		// main pass's points (the primary view, the only one; the opaque.after trigger, found by name); no switch, toggle, hook, pool, product
		// or report line.
		// Forward before effects: its switches are listed where Core's were (right before the projected UV's, the effects' after its
		// forward epoch's, the particles' and billboards' after its refraction's), its "forward" toggle where Core's was (before the shadows',
		// after the projected's chain), which the effects' toggle needs by name, its "refraction" toggle after the effects'; its two engine
		// points (the main pass's blended draw, Main::Draw's refraction point) installed from its install, which no other hook shares; no
		// early hook, pool, pass or product.
		// Effects before particles and billboards: their toggles need its toggle, listed before theirs (after the forward epoch's); its
		// opaque effects' switch is listed after tree wind's list-all (both after the primary exclusion's); its report lines are alone among
		// the forward epoch's; no early hook, pool, pass or product.
		// Particles before billboards, then switch nodes: their toggles are listed after the refraction's in this order (the first two's
		// switches too), and the first two's report lines. LOD then tree LOD after switch nodes: their toggles follow the switch nodes'
		// after the refraction's, in this order, and their switches are listed before the LOD gates' in this order; LOD's early hooks (its
		// segment channel's source) then tree LOD's (its channel's source) after billboards', before hidden nodes' (no order between them);
		// LOD's coordinator components (the LOD gates', then the segment channel's keys alone) after tree wind's (which shares no hook with
		// the gates'), before the decals' and faces'; the gates' engine channel is the first (the segments' follows), their adoption guard and
		// reserved hidden site the only ones, their report lines alone at their points, their switch before the shadows'; tree LOD's scene resources and commit hook
		// after tree wind's (its tables' uploads after the trees'), its passes the schedule's; no pool or product.
		// Grass right after tree LOD: its switch and toggle follow tree LOD's, its scene resources, commit hook and report lines after
		// its, its early hooks (the shared grass capture's sink) after its (no shared site).
		// GPU occlusion right after grass: the depth pyramid (the HZB) the shared cull's occlusion mode tests, tree LOD's cull among them.
		// Hidden nodes' early hooks after billboards' and tree LOD's (no order between them). Decals' early hooks (the decal array edits)
		// after hidden nodes' (no order between them: no shared site); its node capture is the only one. Decals
		// after tree wind: its switches and toggles are listed after the trees' (after the actors' and the skinned), and its candidates'
		// report line after the skinned's. Landscape after them: its switch is listed right before the forward epoch's (after the decals'),
		// its toggle right after the projected's (Integrations/TerrainBlending's after each); no hook, pool, pass or product. Faces after it: its
		// coordinator component's hooks after the others' at the points they share (the slot tables' reset). First person last, after the
		// skins and the actors (its toggle needs the skinned and theirs) and after billboards (its report lines follow theirs at the draws' async work); its switches and toggle
		// are listed after the reflection's.
		// Fades last, where Core's fade code registered (after every feature, before the integrations): its family after the trees' (the
		// tables' report: "trees kept a, fade roots kept b"), its component after every other at the points they share (the slot tables'
		// reset, then the fade roots' owners handed back), its channels the last BeforeStructural ones (snaps, then amounts), its pool,
		// scene resources, commit hook and passes' setups after the others', its stand-in observer last (Main::Update's entry: the writer tables
		// and stores after Portals' and tree wind's); its switches are listed where Core's were (the empty fade node's right before the
		// render graph's, the parity's after the resident parity's); its early hook (FadeSnap) after every other feature's (no shared site).
		// Features/Actors needs its fade amounts channel by name.
		static const std::array kFeatures{
			Portals::Feature(),
			Shadows::Engine::Feature(),
			OcclusionMaps::Feature(),
			Shadows::Local::Feature(),
			Shadows::Sun::Feature(),
			LocalLights::Feature(),
			Reflection::Feature(),
			Skins::Feature(),
			Actors::Feature(),
			TreeWind::Feature(),
			TextureTransforms::Feature(),
			MainView::Feature(),
			Forward::Feature(),
			Effects::Feature(),
			Particles::Feature(),
			Billboards::Feature(),
			SwitchNodes::Feature(),
			Lod::Feature(),
			TreeLod::Feature(),
			Grass::Feature(),
			GpuOcclusion::Feature(),
			HiddenNodes::Feature(),
			Decals::Feature(),
			Landscape::Feature(),
			Faces::Feature(),
			FirstPerson::Feature(),
			Fades::Feature(),
		};
		return kFeatures;
	}
}
