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
		// Moving a feature can change the order of its menu entries and report lines, as well as the order its registrations run in.
		// Toggles and switches placed at the same anchor are listed in registration order.
		//
		// These orders matter:
		// - Portals is first. Its early hooks and stand-in observer run before every other feature's.
		// - Reflection's early hooks run right after Portals'.
		// - Actors reserves its move writers before any other feature, which gives them slots 2-4.
		// - Tree wind's seed pool runs before the texture transforms' pool.
		// - Forward comes before Effects, and Effects before Particles and Billboards. A toggle must be listed after the toggles it needs.
		// - Skins and Actors come before First person, for the same reason. Skins also comes before Effects, which reads skins.
		// - Tree LOD's table uploads run after tree wind's, and Grass's scene resources and commit hook run after Tree LOD's.
		// - Faces' coordinator component runs after the others', except Fades'. Both act on the slot tables' reset.
		// - Fades is last. Its pool, scene resources, commit hook, pass setups, coordinator component and stand-in observer run after every
		//   other feature's. Its channels are the last BeforeStructural ones.
		//
		// Requests made by name, such as Actors needing the Fades amounts channel, are bound after every install, so they set no order here.
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
