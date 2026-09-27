#pragma once

#include <cstdint>
#include <memory>

namespace RE
{
	class BSRenderPass;
}

namespace DCLF
{
	/**
	 * @brief CS_DCLF_DECAL_PROBE=1: what the engine actually does when it draws a decal in the main pass.
	 *
	 * Measured, not inferred from the decompile, because the decompile leaves two things open: whether
	 * the pass's render flags apply the geometry's NiAlphaProperty to the blend state (flag 0x4 does, and
	 * the main pass runs with 0x41 or 0x45), and which of the engine's twelve depth-bias and thirteen
	 * write-mode states a decal group really lands on. Both go straight into the pipeline key the decal
	 * pass will build, so they have to be read off real draws first.
	 *
	 * At every native Lighting draw of a decal property inside the deferred pass it records the pass's
	 * accumulation hint, the alpha property's state, the depth flags, and the RendererShadowState's
	 * fixed-function indices as the draw is issued, and counts the distinct combinations. The first time
	 * a distinct rasterizer or blend state is seen its D3D11 description is logged, read from the engine's
	 * own state tables - the blend one as Community Shaders has overridden it for the deferred pass, which
	 * is what the draw uses.
	 *
	 * It also counts decal passes reaching the RenderPassImmediately thunks, which is the hook-coverage
	 * question: the decal groups are drawn by geometry-group batch renderers, and if their passes never
	 * pass through the thunks, withholding them would not stop the native draw.
	 */
	class DecalProbe
	{
	public:
		static bool Enabled();
		static DecalProbe& Get();

		/** @brief After BSLightingShader::SetupGeometry for a native draw (the state is complete here). */
		void OnNativeLightingDraw(const RE::BSRenderPass* a_pass, std::uint32_t a_renderFlags);

		/** @brief A pass offered to a RenderPassImmediately thunk (hook coverage). */
		void OnPassOffered(const RE::BSRenderPass* a_pass, bool a_inDepthPass);

		/** @brief Logs and resets the counters every a_interval frames. */
		void Report(std::uint32_t a_frame, std::uint32_t a_interval);

		~DecalProbe();

	private:
		DecalProbe();
		struct Impl;
		std::unique_ptr<Impl> impl;
	};
}
