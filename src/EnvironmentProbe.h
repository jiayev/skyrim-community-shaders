#pragma once

#include <RE/N/NiPoint3.h>
#include <cstdint>

struct ID3D11RenderTargetView;

namespace EnvironmentProbe
{
	/** @brief Returns a face mask; face index bits encode negative X, Y, Z respectively. */
	constexpr uint32_t Face(bool a_negativeX, bool a_negativeY, bool a_negativeZ)
	{
		return 1u << (uint32_t(a_negativeX) | (uint32_t(a_negativeY) << 1) | (uint32_t(a_negativeZ) << 2));
	}

	struct Request
	{
		RE::NiPoint3 position{};
		uint32_t faces = 0xFF;
		bool cullDynamic = false;  ///< Excludes skinning, actors and interactive/movable references; retains static/tree references and unowned static geometry.
		float nearDistance = 1.0f;
		float farDistance = 16384.0f;
		ID3D11RenderTargetView* radiance = nullptr;  ///< Optional RGBA16_FLOAT/RGBA32_FLOAT, linear working-space radiance.
		ID3D11RenderTargetView* depth = nullptr;     ///< Optional R32_FLOAT, radial distance in game units; farDistance means no hit.
	};

	enum class Result
	{
		Success,
		InvalidRequest,
		Unavailable,
		ShaderUnavailable
	};

	/**
	 * @brief Rasterizes selected octahedron faces directly into caller-owned 2D render targets.
	 * Call on Skyrim's render thread after main world rendering, before first-person/post-processing,
	 * between techniques (not from a draw hook). The caller retains ownership of the RTVs.
	 * Targets must be square, single-sample Texture2D RTVs of equal size; at least one output is required.
	 * Selected faces are cleared and redrawn; other texels are untouched. Depth follows depth-writing Lighting geometry.
	 * Uses the loaded world, its current lights/shadow maps, and Lighting material features; no sky, other shader classes,
	 * screen-space effects, fog, or post processing. No cubemap or reprojection is used. First use compiles permutations
	 * synchronously. Any failure can leave selected faces incomplete. SE/AE only; VR returns Unavailable.
	 */
	[[nodiscard]] Result Render(const Request& a_request);

	/** @brief Installs the geometry filter; called once during core hook installation. */
	void InstallHooks();
	/** @brief Whether the render thread is currently drawing a probe. */
	bool IsRendering();
	/** @brief Rejects a failed capture permutation instead of falling back to vanilla shaders. */
	void ShaderFailed();
	/** @brief Keeps the engine's output bindings on the probe before applying dirty state. */
	void BeforeDraw();
	/** @brief Binds probe outputs and constants after the engine and feature draw hooks. */
	void AfterDraw();
}
