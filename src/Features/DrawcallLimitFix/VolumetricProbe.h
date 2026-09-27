#pragma once

#include <cstdint>
#include <memory>

namespace RE
{
	class BSBatchRenderer;
	class BSRenderPass;
	namespace BSGraphics
	{
		class BSShaderAccumulator;
	}
}

namespace DCLF
{
	/**
	 * @brief [TEMP] CS_DCLF_VOLUMETRIC_PROBE=1: how much work the engine's volumetric lighting copy is, to
	 * decide whether DCLF should take it over.
	 *
	 * The copy (kVOLUMETRIC_LIGHTING_SHADOWMAPS_ESRAM) is drawn from the sun's descriptors with flag 0x100,
	 * which draws batch group 15 alone: the passes registered with accumulation hint 8. The probe counts
	 * those registrations per frame in the sun's batch renderers (a renderer is the sun's when its
	 * accumulator is finished into the copy), the distinct geometries behind them, what they are, and what
	 * DCLF's caster rule says of them at registration. It times the engine's CPU work: RegisterPass per
	 * Utility pass (hint 8 and the rest), and FinishAccumulatingPreResolveDepth per target (the copy, the
	 * cascades, the other shadow maps). Reported every 240 frames, with DCLF running or not.
	 */
	class VolumetricProbe
	{
	public:
		static bool Enabled();
		static VolumetricProbe& Get();

		/** @brief A Utility pass registered into a batch renderer, and the time the engine's RegisterPass took. */
		void OnRegister(const RE::BSBatchRenderer* a_batch, const RE::BSRenderPass* a_pass, std::int64_t a_ticks);

		/** @brief Hooks BSUtilityShader::SetupGeometry, to see every Utility draw the engine issues in a view. */
		void Install();

		/** @brief A shadow-mode accumulator is about to be finished into a_target (the engine's draw). */
		void BeginFinish(std::uint32_t a_target);

		/** @brief A shadow-mode accumulator finished (the engine's draw), and the time it took. */
		void OnFinish(const RE::BSGraphics::BSShaderAccumulator* a_accumulator, std::uint32_t a_renderFlags, std::uint32_t a_target, std::int64_t a_ticks);

		/** @brief BSShadowDirectionalLight::Render took a_ticks (culling, accumulation and both draws). */
		void OnSunRender(std::int64_t a_ticks);

		/** @brief Once a frame, after the shadow maps are drawn (Prepass). */
		void EndFrame(bool a_running);

		~VolumetricProbe();

	private:
		VolumetricProbe();
		struct Impl;
		std::unique_ptr<Impl> impl;
		struct UtilitySetupGeometry;
		struct SunRender;
		struct SunAccumulate;
		struct SunFullFrustum;
	};
}
