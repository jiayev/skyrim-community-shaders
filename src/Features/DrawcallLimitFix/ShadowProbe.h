#pragma once

#include <cstdint>
#include <memory>

namespace RE
{
	class BSBatchRenderer;
	class BSRenderPass;
}

namespace DCLF
{
	/**
	 * @brief CS_DCLF_SHADOW_PROBE=1: what the engine does when it draws the shadow views, measured on the
	 * running game before DCLF owns any of them.
	 *
	 * The reverse engineering (engine notes: shadow maps) says how the views are organised - one
	 * `ShadowmapDescriptor` per cascade, spot light, paraboloid hemisphere or focus shadow, each with its own
	 * accumulator and batch renderer, all drawn through `BSShadowLight::RenderShadowmap` and
	 * `FinishAccumulatingPreResolveDepth` - but the design of the shadow epochs rests on a few facts the
	 * decompile leaves open: whether every descriptor's port is the full array slice (else the render pass
	 * needs a viewport origin), what the Utility technique per registration is against what the property
	 * flags derive, which objects the engine rejects as casters and why, whether the Utility World is
	 * eye-relative to the *shadow* camera's posAdjust, and what all of it costs on the CPU.
	 *
	 * Per frame it enumerates the views from the shadow scene node's caster list before the shadow maps
	 * are drawn, records the render state at every shadow accumulator's FinishAccumulatingPreResolveDepth,
	 * counts the Utility draws and samples their constants at BSUtilityShader::SetupGeometry, times each
	 * light's Render and the whole shadow-map call, and cross-tabulates the Utility registrations the pass
	 * capture admitted for it against the derivation and the rejection rule DCLF will use.
	 */
	class ShadowProbe
	{
	public:
		static bool Enabled();
		static ShadowProbe& Get();

		/** @brief Installs the accumulator, Utility shader and light hooks (once, when enabled). */
		void Install();

		/** @brief Before the engine draws the shadow maps (Main_RenderShadowMaps, before the call). */
		void OnBeforeShadowMaps();
		/** @brief After the shadow maps were drawn. */
		void OnAfterShadowMaps();

		/**
		 * @brief At EarlyPrepass, after BuildFrame: drains the frame's Utility registrations from the pass
		 * capture and compares them with the views, the derivation and the rejection rule.
		 */
		void OnFrameBuilt();

		/** @brief Logs and resets the counters every a_interval frames. */
		void Report(std::uint32_t a_frame, std::uint32_t a_interval);

		~ShadowProbe();

	private:
		ShadowProbe();
		struct Impl;
		std::unique_ptr<Impl> impl;
		struct Hooks;
		friend struct Hooks;
	};
}
