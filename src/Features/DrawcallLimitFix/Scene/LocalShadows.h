#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace RE
{
	class BSShaderProperty;
}

namespace DCLF
{
	/**
	 * @brief The frame's local shadow lights (spot and point), as the main pass's light selection sees them.
	 *
	 * BSLightingShaderProperty::GetRenderPasses takes an object's shadow lights from FUN_1414fcf80 (engine notes: light
	 * selection): every shadow-caster array entry after the sun's whose bit is set in the object's activeLightMask - the
	 * light's accumulation (CalculateActiveShadowCasterLights, one Accumulate per light) culled the object - unless the
	 * light's +0x61 is clear and the property is landscape (kLandscape 14, kNoLODLandFade 46; BSLightingShaderProperty's
	 * FUN_14147c340). Light Limit Fix's ShadowBitMask is then the OR of those lights' maskIndex bits (the accumulation's
	 * order, BSShadowLight +0x520). The accumulation's cull is each shadowmap descriptor's culling process: its frustum
	 * planes and, when set, its custom planes.
	 */
	struct LocalShadowLights
	{
		struct Light
		{
			std::uint32_t maskBit = 0;  // 1 << maskIndex
			bool affectsLand = false;   // +0x61: taken by landscape properties too
			// The light's sphere: its accumulation walks only the scene around it (BSShadowLight::sceneAccumArray).
			float center[3]{};
			float radius = 0.0f;
			// Per shadowmap descriptor, its planes ((normal, constant), inside where dot(normal, p) - constant >= 0) and
			// their active mask: the process's six frustum planes, then its six custom planes.
			struct Volume
			{
				std::array<std::array<float, 4>, 12> planes{};
				std::uint32_t masks[2]{};
			};
			std::vector<Volume> volumes;
		};
		std::vector<Light> lights;

		/** @brief This frame's, from the shadow scene node's shadow-caster array (render thread, after the lights accumulated). */
		static LocalShadowLights Sample();
		/** @brief The LLF ShadowBitMask the selection gives an object with this property and world bound. */
		std::uint32_t MaskOf(const RE::BSShaderProperty* a_property, const float a_center[3], float a_radius) const;
	};
}
