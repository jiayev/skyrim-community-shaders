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
	 * @brief CS_DCLF_TREE_TRACE=1: every geometry of a TREE reference, followed frame by frame, to find why trees
	 * flicker for a frame at fixed distances.
	 *
	 * Each traced geometry is sampled twice a frame: before the scene walk and the main cull (Main::Draw's early
	 * hook) and after both (EarlyPrepass, once the accumulate phase and the hand-back are done). Between the two
	 * the cull may move it (a switch node's newly selected child is updated there), change its LOD level or its
	 * switch index, and the scene walk has already taken its transform and run the engine's palette update. The
	 * native draws of the frame are counted as they happen, and the frame is judged at the next one's start:
	 * who drew it (native, DCLF, both, nobody), and whether what DCLF drew was taken before something moved.
	 */
	class TreeTrace
	{
	public:
		static bool Enabled();
		static TreeTrace& Get();

		/** @brief Main::Draw's early hook, before the scene walk: judges the last frame, samples this one's "pre". */
		void BeforeScene();
		/** @brief EarlyPrepass, after the accumulate phase and HandBackUndrawable: this frame's "post" and DCLF state. */
		void AfterAccumulate();
		void OnNativeLightingDraw(const RE::BSRenderPass* a_pass);
		void Report(std::uint32_t a_frame, std::uint32_t a_interval);

		~TreeTrace();

	private:
		TreeTrace();
		struct Impl;
		std::unique_ptr<Impl> impl;
	};
}
