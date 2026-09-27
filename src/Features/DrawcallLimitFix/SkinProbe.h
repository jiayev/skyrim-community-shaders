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
	 * @brief CS_DCLF_SKIN_PROBE=1: what the skinned objects the main pass draws actually are.
	 *
	 * The plan for skinned coverage rests on reading the engine's own palette (NiSkinInstance::boneMatrices,
	 * three float4 rows a bone, refreshed once a frame by the bone setter) and drawing the skin partition's
	 * own buffers. Before any of that is built this records, at every native skinned Lighting draw of the
	 * deferred pass: the skin instance's class, its partition count and LOD bytes, its bone count, whether
	 * the geometry's rendererData is the partition's buffer, the geometry type and technique, whether DCLF
	 * tracks the object, and a per-name histogram. It also hashes the palette against the previous frame's
	 * to count static poses, and checks VS_PerFrame's BonesPivot against posAdjust, which is what decides
	 * whether the pivot needs patching per epoch.
	 */
	class SkinProbe
	{
	public:
		static bool Enabled();
		static SkinProbe& Get();

		void OnNativeLightingDraw(const RE::BSRenderPass* a_pass, std::uint32_t a_renderFlags);
		void Report(std::uint32_t a_frame, std::uint32_t a_interval);

		~SkinProbe();

	private:
		SkinProbe();
		struct Impl;
		std::unique_ptr<Impl> impl;
	};
}
