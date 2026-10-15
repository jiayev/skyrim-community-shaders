// The one ordered list of DCLF's features (see the header). A feature's place is the order its registrations run in beside the others':
// its pools after the earlier features' pools, its stages after theirs at the same slot, its products after theirs.

#include "Features/DrawcallLimitFix/Features/FeatureList.h"
#include "Features/DrawcallLimitFix/Features/TextureTransforms/TextureTransforms.h"
#include "Features/DrawcallLimitFix/Features/TreeWind/TreeWind.h"

#include <array>

namespace DCLF::Features
{
	std::span<const Api::Feature> List()
	{
		// Tree wind first: its seeds' pool runs right after the frame values' built-in parts, before the texture transforms' pool.
		static const std::array kFeatures{
			TreeWind::Feature(),
			TextureTransforms::Feature(),
		};
		return kFeatures;
	}
}
