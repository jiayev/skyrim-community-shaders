// The one ordered list of DCLF's features (see the header). A feature's place is the order its registrations run in beside the others':
// its pools after the earlier features' pools, its stages after theirs at the same slot, its products after theirs.

#include "Features/DrawcallLimitFix/Features/FeatureList.h"
#include "Features/DrawcallLimitFix/Features/Actors/Actors.h"
#include "Features/DrawcallLimitFix/Features/Billboards/Billboards.h"
#include "Features/DrawcallLimitFix/Features/Decals/Decals.h"
#include "Features/DrawcallLimitFix/Features/Effects/Effects.h"
#include "Features/DrawcallLimitFix/Features/Faces/Faces.h"
#include "Features/DrawcallLimitFix/Features/FirstPerson/FirstPerson.h"
#include "Features/DrawcallLimitFix/Features/HiddenNodes/HiddenNodes.h"
#include "Features/DrawcallLimitFix/Features/Landscape/Landscape.h"
#include "Features/DrawcallLimitFix/Features/Lod/Lod.h"
#include "Features/DrawcallLimitFix/Features/OcclusionMaps/OcclusionMaps.h"
#include "Features/DrawcallLimitFix/Features/Particles/Particles.h"
#include "Features/DrawcallLimitFix/Features/Reflection/Reflection.h"
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
		// The occlusion maps first: their toggles are the first registered, so their bits in the packed toggle word are where Core's were
		// (right after Core's flags), and their switches and toggles are listed before the reflection's and the first person's (which follow
		// them there); no early hooks, no pool, stage or product. The shadow sources next, sun then local (Shadows/Sun, Shadows/Local): they
		// register their sources (resolved by source, whatever the order) and source hooks (installed by source: the sun's accumulation and
		// camera, Core's light selection, then the focus cameras, whatever the list's order); the sun also its switches and toggles (anchored
		// after the shadows'); no early hook, pool, stage or product.
		// Reflection next: its early hooks (the water reflection's update, add-roots and face render) run before every other feature's and
		// Core's observers, where the shell installed them; its toggle's bit follows
		// the maps', where Core's was; its switch and toggle are listed after the precipitation mask's, before the first person's (which
		// follow them there); it registers no pool, stage or product. Skins next: before the features that read skins (DEPENDS: Effects,
		// first person); its switches are listed after the culling's, the skin partitions' after the skinned's (the actors', tree wind's and
		// the decals' follow it there), its skinned toggle before the projected's (tree wind's and the decals' follow it there) and its skin
		// partitions' after the tree LOD's (the actors' follow it there); its toggles' bits follow the reflection's; its report line is the
		// first at the coordinator's last pass (before the decals'); no early hook, pool, stage or product. Actors next: its early hooks
		// (Actor::SetAlpha and the actors' move writers) right after the reflection's, its three move writers reserved first (slots 2-4,
		// before the particles'); its switch is listed after the skin partitions', before tree wind's and the decals' (which follow it
		// there); it registers no pool, stage or product. Tree wind next:
		// its seeds' pool runs right after the frame values' built-in parts, before the texture transforms' pool.
		// Effects before particles and billboards: their toggles need its toggle, listed before theirs (after the forward epoch's); its
		// opaque effects' switch is listed after tree wind's list-all (both after the primary exclusion's); its report lines are alone among
		// the forward epoch's; no early hook, pool, stage or product.
		// Particles before billboards, then switch nodes: their toggles are listed after the refraction's in this order (the first two's
		// switches too), and the first two's report lines. LOD then tree LOD after switch nodes: their toggles follow the switch nodes'
		// after the refraction's, in this order, and their switches are listed before the LOD gates' in this order; LOD's early hooks (its
		// segment channel's source) then tree LOD's (its channel's source) after billboards', before hidden nodes' (no order between them);
		// LOD's coordinator components (the LOD gates', then the segment channel's keys alone) after tree wind's (which shares no hook with
		// the gates'), before the decals' and faces'; the gates' engine channel is the first (the segments' follows), their adoption guard and
		// reserved hidden site the only ones, their report lines alone at their points, their switch before the shadows'; tree LOD's scene resources and commit hook
		// after tree wind's (its tables' uploads after the trees'), its stages alone at the depth segment's draw-family slots; no pool or product.
		// Hidden nodes' early hooks after billboards' and tree LOD's (no order between them). Decals' early hooks (the decal array edits)
		// after hidden nodes' (no order between them: no shared site); its node capture is the only one. Decals
		// after tree wind: its switches and toggles are listed after the trees' (after the actors' and the skinned), and its candidates'
		// report line after the skinned's. Landscape after them: its switch is listed right before the forward epoch's (after the decals'),
		// its toggle right after the projected's (Integrations/TerrainBlending's after each); no hook, pool, stage or product. Faces after it: its
		// coordinator component's hooks after the others' at the points they share (the slot tables' reset). First person last, after the
		// skins and the actors (its toggle needs the skinned and theirs) and after billboards (its report lines follow theirs at the draws' async work); its switches and toggle
		// are listed after the reflection's.
		static const std::array kFeatures{
			OcclusionMaps::Feature(),
			Shadows::Sun::Feature(),
			Shadows::Local::Feature(),
			Reflection::Feature(),
			Skins::Feature(),
			Actors::Feature(),
			TreeWind::Feature(),
			TextureTransforms::Feature(),
			Effects::Feature(),
			Particles::Feature(),
			Billboards::Feature(),
			SwitchNodes::Feature(),
			Lod::Feature(),
			TreeLod::Feature(),
			HiddenNodes::Feature(),
			Decals::Feature(),
			Landscape::Feature(),
			Faces::Feature(),
			FirstPerson::Feature(),
		};
		return kFeatures;
	}
}
