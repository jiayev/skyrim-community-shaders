// The maps' occluder rules (their contributions' technique): Skylighting::OcclusionTechnique's and the precipitation mask's, from the
// walked object's records (Api::OccluderLeaf); and the maps' camera and accumulator, the precipitation accumulator's.

#include "Features/DrawcallLimitFix/Core/Extensions/ShadowViews.h"
#include "Features/DrawcallLimitFix/Features/OcclusionMaps/OcclusionMaps.h"
#include "Features/Skylighting.h"

namespace DCLF::Features::OcclusionMaps
{
	using enum RE::BSShaderProperty::EShaderPropertyFlag;

	namespace
	{
		/** @brief Whether any of a_flags is in the object's property flags. */
		template <class... Flags>
		bool Any(const Api::OccluderLeaf& a_leaf, Flags... a_flags)
		{
			return (a_leaf.flags & (static_cast<std::uint64_t>(a_flags) | ...)) != 0;
		}

		/** @brief The depth technique both rules draw an occluder with. */
		std::uint32_t DepthTechnique(const Api::OccluderLeaf& a_leaf)
		{
			using enum RE::BSUtilityShader::Flags;
			stl::enumeration<RE::BSUtilityShader::Flags> technique;
			technique.set(RenderDepth);
			if (Any(a_leaf, kVertexColors))
				technique.set(Vc);
			if (a_leaf.alphaTesting) {
				technique.set(Texture);
				technique.set(AlphaTest);
			}
			if (Any(a_leaf, kLODObjects, kHDLODObjects))
				technique.set(LodObject);
			if (Any(a_leaf, kTreeAnim))
				technique.set(TreeAnim);
			return technique.underlying();
		}

		/** @brief The precipitation accumulator (Precipitation::occlusionData), which draws both maps. */
		bool PrecipitationAccumulator(const Api::OcclusionAccumulator& a_accumulator)
		{
			auto* sky = globals::game::sky;
			auto* precip = sky ? sky->precip : nullptr;
			return precip && precip->occlusionData.accumulator.get() == a_accumulator.accumulator;
		}
	}

	std::uint32_t SkylightingTechnique(const Api::OccluderLeaf& a_leaf)
	{
		if (Any(a_leaf, kSkinned) && !Any(a_leaf, kTreeAnim))
			return 0;
		if (a_leaf.referenced) {
			// The nearest fade node's BSX flags.
			using Bsx = RE::BSXFlags::Flag;
			constexpr auto kRejected = static_cast<std::int32_t>(Bsx::kRagdoll) | static_cast<std::int32_t>(Bsx::kEditorMarker) | static_cast<std::int32_t>(Bsx::kDynamic) |
			                           static_cast<std::int32_t>(Bsx::kAddon) | static_cast<std::int32_t>(Bsx::kNeedsTransformUpdate) |
			                           static_cast<std::int32_t>(Bsx::kMagicShaderParticles) | static_cast<std::int32_t>(Bsx::kLights) |
			                           static_cast<std::int32_t>(Bsx::kBreakable) | static_cast<std::int32_t>(Bsx::kSearchedBreakable);
			const std::int32_t bsx = a_leaf.NearestFadeNodeBsx();
			if (bsx != -1 && (bsx & kRejected))
				return 0;
		}
		if (!Any(a_leaf, kZBufferWrite) || Any(a_leaf, kRefraction, kTempRefraction, kLODLandscape, kEyeReflect, kDecal, kDynamicDecal))
			return 0;
		return DepthTechnique(a_leaf);
	}

	std::uint32_t PrecipitationTechnique(const Api::OccluderLeaf& a_leaf)
	{
		if (Any(a_leaf, kSkinned))
			return 0;
		if (!Any(a_leaf, kZBufferWrite) ||
			Any(a_leaf, kRefraction, kTempRefraction, kMultiTextureLandscape, kNoLODLandBlend, kLODLandscape, kEyeReflect, kDecal, kDynamicDecal))
			return 0;
		return DepthTechnique(a_leaf);
	}

	std::uint32_t EnginePrecipitationTechnique(const Api::OccluderLeaf& a_leaf)
	{
		// BSShaderProperty's GetPrecipitationOcclusionMapRenderPasses (vfunc 0x2D, AE 0x1414afd50), BSLightingShaderProperty's without
		// Skylighting: no size test, and tree animation and anisotropic lighting occlude nothing. Its technique (RenderDepth with the
		// vertex inputs' bits) writes the same depth as DepthTechnique's.
		if (!Any(a_leaf, kZBufferWrite) || Any(a_leaf, kSkinned, kRefraction, kTempRefraction, kMultiTextureLandscape, kNoLODLandBlend, kLODLandscape,
												 kEyeReflect, kTreeAnim, kDecal, kDynamicDecal, kAnisotropicLighting))
			return 0;
		return DepthTechnique(a_leaf);
	}

	void PrecipitationCamera(Api::OcclusionCamera& a_camera)
	{
		auto* sky = globals::game::sky;
		auto* precip = sky ? sky->precip : nullptr;
		a_camera.camera = precip ? precip->occlusionData.camera.get() : nullptr;
	}

	bool SkyOwnsAccumulator(const Api::OcclusionAccumulator& a_accumulator)
	{
		return PrecipitationAccumulator(a_accumulator) && globals::features::skylighting.inOcclusion;
	}

	bool PrecipitationOwnsAccumulator(const Api::OcclusionAccumulator& a_accumulator)
	{
		return PrecipitationAccumulator(a_accumulator) && !globals::features::skylighting.inOcclusion;
	}
}
