// The Effect port's property callbacks (Api::MaterialPortContribution): its capture (what the descriptors and PropertyColor read of the
// property and its material, into the PropertyRecord) and its shading row (PropertyColor, EmitColor). Pure but for the engine reads, made
// where Core holds the property (the capture's read window, FrameValues' shading items).

#include "Features/DrawcallLimitFix/Features/Effects/Effects.h"
#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Core/Extensions/MaterialPorts.h"
#include "Features/DrawcallLimitFix/Features/Effects/EffectConstants.h"

#include <algorithm>

namespace DCLF::Features::Effects
{
	void CaptureEffectProperty(const Api::MaterialPortCapture& a_capture)
	{
		// What the effect's descriptor (FUN_1414bde90) and PropertyColor read of the property and its material, taken here where the
		// property holds the material. GetRenderPasses tests a texture path by its string's pointer (FUN_140cec900 against null), not by
		// the texture.
		a_capture.emissive = static_cast<const RE::BSEffectShaderProperty&>(a_capture.property).emittanceColor;
		if (const auto* material = a_capture.property.material) {
			const auto* bytes = reinterpret_cast<const std::byte*>(material);
			const auto pathAt = [&](std::size_t a_offset) { return *reinterpret_cast<const char* const*>(bytes + a_offset) != nullptr; };
			a_capture.feature = (pathAt(0x70) ? kEffectHasSourcePath : 0u) | (pathAt(0x78) ? kEffectHasGreyscalePath : 0u);
		}
	}

	void SampleEffectShading(Api::MaterialPortShading& a_shading)
	{
		// Effect port: PropertyColor (SetupGeometry 0x1414eab10): the emittance colour or white, and the property's alpha, which
		// GetRenderPasses leaves as the material's (the base colour's) times the fade node's currentFade; a member's is the material's.
		const auto& effect = static_cast<const RE::BSEffectShaderProperty&>(a_shading.property);
		const auto* material = static_cast<const RE::BSEffectShaderMaterial*>(effect.material);
		auto& row = a_shading.out;
		auto& shading = row.shading;
		shading.materialData[0] = shading.materialData[1] = shading.materialData[3] = 0.0f;
		shading.materialData[2] = a_shading.member && material ? material->baseColor.alpha : effect.alpha;
		const auto* emittance = effect.emittanceColor;
		shading.emitColor[0] = emittance ? emittance->red : 1.0f;
		shading.emitColor[1] = emittance ? emittance->green : 1.0f;
		shading.emitColor[2] = emittance ? emittance->blue : 1.0f;
		shading.ssrSpecular = 0.0f;
		row.emissiveMult = 1.0f;
		row.written = 4u | 0x70u;
		row.padding[0] = row.padding[1] = 0u;
		std::fill(std::begin(row.treeParams), std::end(row.treeParams), 0.0f);
		std::fill(std::begin(row.windTimers), std::end(row.windTimers), 0.0f);
	}
}
