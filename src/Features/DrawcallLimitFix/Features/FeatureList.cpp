// The one ordered list of DCLF's features (see the header). A feature's place is the order its registrations run in beside the others':
// its pools after the earlier features' pools, its stages after theirs at the same slot, its products after theirs.

#include "Features/DrawcallLimitFix/Features/FeatureList.h"
#include "Features/DrawcallLimitFix/Features/Billboards/Billboards.h"
#include "Features/DrawcallLimitFix/Features/Decals/Decals.h"
#include "Features/DrawcallLimitFix/Features/HiddenNodes/HiddenNodes.h"
#include "Features/DrawcallLimitFix/Features/Particles/Particles.h"
#include "Features/DrawcallLimitFix/Features/SwitchNodes/SwitchNodes.h"
#include "Features/DrawcallLimitFix/Features/TextureTransforms/TextureTransforms.h"
#include "Features/DrawcallLimitFix/Features/TreeWind/TreeWind.h"

#include <array>

namespace DCLF::Features
{
	std::span<const Api::Feature> List()
	{
		// Tree wind first: its seeds' pool runs right after the frame values' built-in parts, before the texture transforms' pool.
		// Particles before billboards, then switch nodes: their toggles are listed after the refraction's in this order (the first two's
		// switches too), and the first two's report lines. Hidden nodes' early hooks after billboards' (no order between them). Decals
		// after tree wind: its switches and toggles are listed after the trees' (after the actors' and the skinned).
		static const std::array kFeatures{
			TreeWind::Feature(),
			TextureTransforms::Feature(),
			Particles::Feature(),
			Billboards::Feature(),
			SwitchNodes::Feature(),
			HiddenNodes::Feature(),
			Decals::Feature(),
		};
		return kFeatures;
	}
}
