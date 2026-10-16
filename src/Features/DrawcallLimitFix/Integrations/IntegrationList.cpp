// The one ordered list of DCLF's integrations (see the header): each a CS feature DCLF supports, written against Api/ as a feature is, and
// installed after every feature, so what it registers may name what a feature registered. Its place is the order its registrations run in
// beside the other integrations'.

#include "Features/DrawcallLimitFix/Integrations/IntegrationList.h"
#include "Features/DrawcallLimitFix/Integrations/AdvancedSkin/AdvancedSkin.h"
#include "Features/DrawcallLimitFix/Integrations/LightLimitFix/LightLimitFix.h"
#include "Features/DrawcallLimitFix/Integrations/TerrainBlending/TerrainBlending.h"

#include <array>

namespace DCLF::Integrations
{
	std::span<const Api::Feature> List()
	{
		// Advanced Skin: its wetness, a shading field of Features/Actors' published actor-owned class; no early or source hook.
		// Terrain Blending: its classify input, switch and toggle (after Features/Landscape's, which its toggle needs), the gates on
		// Features/Landscape's techniques, its held groups (main range orders 2 and 3), its mask and its near twin; no early or source hook.
		// Light Limit Fix: its classify input (which Features/Effects needs by name), its room index (an ancestor value) and its graph
		// buffers (t35-t37); no early or source hook.
		static const std::array kIntegrations{
			AdvancedSkin::Integration(),
			TerrainBlending::Integration(),
			LightLimitFix::Integration(),
		};
		return kIntegrations;
	}
}
