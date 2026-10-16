// The one ordered list of DCLF's integrations (see the header): each a CS feature DCLF supports, written against Api/ as a feature is, and
// installed after every feature, so what it registers may name what a feature registered. Its place is the order its registrations run in
// beside the other integrations'.

#include "Features/DrawcallLimitFix/Integrations/IntegrationList.h"
#include "Features/DrawcallLimitFix/Integrations/AdvancedSkin/AdvancedSkin.h"

#include <array>

namespace DCLF::Integrations
{
	std::span<const Api::Feature> List()
	{
		// Advanced Skin: its wetness, a shading field of Features/Actors' published actor-owned class; no early or source hook.
		static const std::array kIntegrations{
			AdvancedSkin::Integration(),
		};
		return kIntegrations;
	}
}
