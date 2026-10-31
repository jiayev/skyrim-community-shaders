#include "Features/DrawcallLimitFix/Core/Extensions/ObjectContributors.h"
#include "Features/DrawcallLimitFix/Integrations/VanillaFresnel/VanillaFresnel.h"
#include "Features/VanillaFresnel.h"

namespace DCLF::Integrations::VanillaFresnel
{
	namespace
	{
		bool EyeMaterial(const Api::LightingJoinView& a_view)
		{
			// The raw Eye technique wins over other property flags in GetRenderPasses.
			if (a_view.PropertyFlags() & static_cast<std::uint64_t>(RE::BSShaderProperty::EShaderPropertyFlag::kEyeReflect))
				return true;
			const auto* node = a_view.leaf.node;
			return a_view.HasMaterial() && ::VanillaFresnel::IsEyeMaterial(
											   static_cast<RE::BSShaderMaterial::Feature>(a_view.MaterialFeature()),
											   node && node->name ? node->name : "");
		}

		void Install(Api::InstallContext& a_context)
		{
			// SetupGeometry is bypassed by indirect draws. Keep the classification per
			// object: an eye and ordinary envmapped geometry can share one pipeline.
			a_context.RegisterLightingObjectFlag({ .objectFlag = kObjectEye, .recordFlag = kRecordEye, .applies = &EyeMaterial });
		}
	}

	Api::Feature Integration()
	{
		return { "vanilla fresnel", &Install };
	}
}
