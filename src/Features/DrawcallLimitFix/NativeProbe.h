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
	 * @brief CS_DCLF_NATIVE_PROBE=1: what the Lighting draws the main pass still issues natively are, and why
	 * DCLF left each one there.
	 *
	 * Under static ownership every object DCLF draws is withheld from the batch renderer, so what reaches
	 * the native draw hook is exactly the uncovered remainder. Each draw is keyed by the base object's form
	 * type (TREE, NPC_, STAT, ...), DCLF's verdict (or `untracked` when it hangs under no category node DCLF
	 * walks), the technique, the geometry type and, for a skinned shape, the skin instance's class,
	 * partition count, LOD bytes and bone count with the pass's LODMode - the inputs that decide which
	 * partitions the engine draws. A per-name histogram per form type goes with it.
	 */
	class NativeProbe
	{
	public:
		static bool Enabled();
		static NativeProbe& Get();

		void OnNativeLightingDraw(const RE::BSRenderPass* a_pass, std::uint32_t a_renderFlags);
		void Report(std::uint32_t a_frame, std::uint32_t a_interval);

		~NativeProbe();

	private:
		NativeProbe();
		struct Impl;
		std::unique_ptr<Impl> impl;
	};
}
