#pragma once

#include <array>
#include <cstdint>

#include "Features/DrawcallLimitFix/Scene/Ports/MaterialPortFeature.h"
#include "Features/DrawcallLimitFix/Scene/Ports/MaterialPortVanilla.h"

namespace DCLF::MaterialPort
{
	/**
	 * @brief What of a frame's sources the signature-sourced frame components read (MaterialSources::ApplyFrameComponents: PS 6,
	 * 24.z, 31.zw, 34 and 35, t11's writtenness and modes) and the gates their evaluation passes (the shader known, the technique
	 * remap, the hooks loaded, Advanced Skin's keys), as the port uses them: two frames with equal witnesses give every signature's
	 * probe the same frame components (Coordinator::RefreshMaterialSignatures evaluates again only when it moves).
	 *
	 * Left out, as no signature application reads them: what alternates frame to frame, the texture-transform buffer (VS 11, the
	 * transform watch's: RefreshMaterialTransforms) and the character light's render target (its index within the array and every
	 * view: a record holds no t11 view); IBLParams (a record keeps its first evaluation's); the default textures (the material's
	 * own texture choices, no frame component).
	 */
	struct FrameComponentsWitness
	{
		std::array<std::uint32_t, 20> words{};
		bool operator==(const FrameComponentsWitness&) const = default;
	};

	/** @brief The per-frame sources SetupMaterial reads (sampled by the render thread at the frame's start: FrameGlobals::material). */
	struct MaterialFrame
	{
		VanillaFrame vanilla;  // MaterialPortVanilla.h
		FeatureFrame feature;  // MaterialPortFeature.h
		FrameComponentsWitness components;  // of the two (SampleFrame)
	};

	/** @brief Pure: a_frame's witness (SampleFrame sets MaterialFrame::components from it). */
	FrameComponentsWitness WitnessOf(const VanillaFrame& a_vanilla, const FeatureFrame& a_feature);
}
