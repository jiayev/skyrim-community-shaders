#pragma once

#include "EngineFix.h"

/**
 * @brief Releases a renderer texture's D3D objects once on Skyrim 1.7.
 *
 * 1.7's renderer-texture release (BSShaderResourceManager's texture release, Address Library id 77322) releases the
 * texture, its UAV and its view twice: once explicitly, then again in the members' destructors. Every creator still takes
 * one reference per object, as on 1.6. Native D3D11 destroys objects lazily, so the second Release usually lands on live
 * memory. DXVK frees an unbound view at once, and the second Release reads freed memory, which crashes once the heap reuses
 * the block. The fix skips the explicit releases and keeps the destructors'.
 */
struct TextureDoubleReleaseFix : EngineFix
{
	std::string GetName() override { return "Texture Double Release Fix"; }

	void Install() override;
};
