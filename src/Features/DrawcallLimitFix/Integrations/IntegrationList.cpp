// The one ordered list of DCLF's integrations (see the header): each a CS feature DCLF supports, written against Api/ as a feature is, and
// installed after every feature, so what it registers may name what a feature registered. Its place is the order its registrations run in
// beside the other integrations'.

#include "Features/DrawcallLimitFix/Integrations/IntegrationList.h"
#include "Features/DrawcallLimitFix/Integrations/AdvancedSkin/AdvancedSkin.h"
#include "Features/DrawcallLimitFix/Integrations/ExtendedTranslucency/ExtendedTranslucency.h"
#include "Features/DrawcallLimitFix/Integrations/LightLimitFix/LightLimitFix.h"
#include "Features/DrawcallLimitFix/Integrations/LinearLighting/LinearLighting.h"
#include "Features/DrawcallLimitFix/Integrations/SubsurfaceScattering/SubsurfaceScattering.h"
#include "Features/DrawcallLimitFix/Integrations/TerrainBlending/TerrainBlending.h"
#include "Features/DrawcallLimitFix/Integrations/TruePBR/TruePBRIntegration.h"

#include <array>

namespace DCLF::Integrations
{
	std::span<const Api::Feature> List()
	{
		// Advanced Skin: its wetness, a shading field of Features/Actors' published actor-owned class, its material contributor (After, t71
		// and t74) and its per-draw SkinPerGeometry (PS b7); no early or source hook.
		// Terrain Blending: its classify input, switch and toggle (after Features/Landscape's, which its toggle needs), the gates on
		// Features/Landscape's techniques, its held groups (main range orders 2 and 3), its mask and its near twin; no early or source hook.
		// Light Limit Fix: its classify input (which Features/Effects needs by name), its room index (an ancestor value) and its graph
		// buffers (t35-t37) and its per-draw StrictLightData (PS b3); no early or source hook.
		// TruePBR: its material contributor (Replace, t80-t91, its class), its glints property bits and its Lighting pass rules; no early
		// or source hook.
		// Subsurface Scattering: its "beast race" reference fact and its Lighting object flag; no early or source hook.
		// Extended Translucency: its geometry extra and the Lighting key's spare bits; no early or source hook.
		// Linear Lighting: its per-draw LLPerGeometry (PS b8); no early or source hook.
		static const std::array kIntegrations{
			AdvancedSkin::Integration(),
			TerrainBlending::Integration(),
			LightLimitFix::Integration(),
			TruePBR::Integration(),
			SubsurfaceScattering::Integration(),
			ExtendedTranslucency::Integration(),
			LinearLighting::Integration(),
		};
		return kIntegrations;
	}
}
