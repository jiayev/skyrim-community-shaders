#pragma once

#include <RE/B/BSRenderPass.h>

namespace ExternalEmittance
{
	/**
	 * @brief Check whether the external emittance flag should be suppressed for a render pass.
	 *
	 * Returns true when the pass is in an interior cell, the shader has the external emittance
	 * flag set, but the geometry has no emittance source attached.
	 *
	 * @param a_pass The render pass to evaluate.
	 * @return True if external emittance should be suppressed.
	 */
	bool ShouldSuppress(const RE::BSRenderPass* a_pass);

	/** @brief ShouldSuppress for a geometry and its shader property, without a render pass. */
	bool ShouldSuppress(const RE::BSShaderProperty* a_shaderProperty, const RE::BSGeometry* a_geometry);

	/**
	 * @brief ShouldSuppress with the interior test already answered.
	 *
	 * The interior test is the same for every object in a frame, so a caller evaluating this over many
	 * geometries can answer it once and pass it in rather than paying two singleton dereferences per
	 * object. The overload above is this one with `Util::IsInterior()` supplied.
	 */
	bool ShouldSuppress(bool a_interior, const RE::BSShaderProperty* a_shaderProperty, const RE::BSGeometry* a_geometry);

	/**
	 * @brief Update the extra shader descriptor bitmask for external emittance suppression.
	 *
	 * Sets or clears the SuppressExternalEmittance bit in the global permutation data
	 * based on the result of ShouldSuppress for the given pass.
	 *
	 * @param a_pass The render pass to evaluate.
	 */
	void UpdatePermutation(const RE::BSRenderPass* a_pass);
}
