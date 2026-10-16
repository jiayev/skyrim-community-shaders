// Extended Translucency: the AnisotropicAlphaMaterial geometry extra and the Lighting key's spare bits (its material model, and the
// permutation's ExtraFeatureDescriptor from them). Both gates read the feature live, as does SkinnedOnly (the join pool).

#include "Features/DrawcallLimitFix/Integrations/ExtendedTranslucency/ExtendedTranslucency.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ObjectContributors.h"

#include "Features/ExtendedTranslucency.h"

#include <cstdint>

namespace DCLF::Integrations::ExtendedTranslucency
{
	namespace
	{
		using Model = ::ExtendedTranslucency::MaterialModel;

		/** @brief ExtendedTranslucency::MaterialModelOf, from the records. */
		std::uint32_t TranslucencyModelOf(const Api::LightingJoinView& a_view)
		{
			const auto& leaf = a_view.leaf;
			const auto* alpha = leaf.alpha && leaf.alpha->rtti == globals::rtti::NiAlphaPropertyRTTI.get() ? leaf.alpha : nullptr;
			const auto* light = leaf.property && leaf.property->rtti == globals::rtti::BSLightingShaderPropertyRTTI.get() ? leaf.property : nullptr;
			// Only blended geometry: an alpha property that blends, or a Lighting property's alpha below one.
			if (!(light && light->alpha < 0.999f) && (!alpha || !(alpha->flags & 1u)))
				return Model::DescriptorDisabled;
			const auto value = a_view.geometryExtra;
			if (value == SceneCapture::GeometryRecord::kNoExtra) {
				const auto& feature = globals::features::extendedTranslucency;
				return !feature.settings.SkinnedOnly || a_view.Skinned() ? Model::DescriptorUseDefault : Model::DescriptorDisabled;
			}
			if (value == SceneCapture::GeometryRecord::kWrongExtra)
				return Model::DescriptorDisabled;
			const auto material = static_cast<std::uint32_t>(value) & ::ExtendedTranslucency::ExtraFeatureDescriptorMask;
			return material == Model::Disabled ? Model::DescriptorDisabled : material;
		}

		// The key's 3 bits: the model XOR DescriptorDisabled (0 for unblended geometry, and while the feature is unloaded).
		std::uint32_t KeyBits(const Api::LightingJoinView& a_view)
		{
			return globals::features::extendedTranslucency.loaded ? (Model::DescriptorDisabled ^ TranslucencyModelOf(a_view)) & 7u : 0u;
		}

		// Extended Translucency's material model, as its SetupGeometry hook sets it (the key carries it).
		std::uint32_t Permutation(std::uint32_t a_keyBits)
		{
			return globals::features::extendedTranslucency.loaded ?
			           (Model::DescriptorDisabled ^ a_keyBits) << ::ExtendedTranslucency::ExtraFeatureDescriptorShift :
			           0u;
		}

		void Install(Api::InstallContext& a_context)
		{
			a_context.DeclareGeometryExtra("AnisotropicAlphaMaterial");
			a_context.RegisterLightingKeyBits({ .keyBits = &KeyBits, .permutation = &Permutation });
		}
	}

	Api::Feature Integration()
	{
		return { "extended translucency", &Install };
	}
}
