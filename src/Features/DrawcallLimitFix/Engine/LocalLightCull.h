#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace RE
{
	class BSBatchRenderer;
	class BSRenderPass;
}

namespace DCLF
{
	struct SunExclusion;

	/**
	 * @brief The point lights' shadow culls without the entries DCLF draws (drawcall-limit-fix.md, "Point lights' shadow culls
	 * without DCLF's entries"). AE only.
	 *
	 * BSShadowParabolicLight::Accumulate (0x14151b960) culls through BSCullingProcess::Process (FUN_1414bf320) with the static
	 * BSParabolicCullingProcess (0x14335bfa0); without a light's own root list (sceneAccumArray) it walks the scene's whole object
	 * root, every frame, every point light: BSFadeNode::OnVisible on hundreds of DCLF's roots, whose casters DCLF's paraboloid
	 * views draw and the registration then withholds (PassCapture). The exclusion is the sun's (BuildSunExclusion) for the
	 * paraboloid mode: an entry all of whose shadow casters are that mode's inputs. The process's Process1 (vtable slot 0x16)
	 * skips such an entry while that mode's claims are live this frame and the exclusion was built for the current candidates.
	 * CS_DCLF_LIGHT_EXCLUDE=0 turns it off; =probe skips nothing and counts. On persistent-parity frames nothing is skipped
	 * and a paraboloid pass the registration did not withhold under an excluded entry counts as one the skip would lose.
	 */
	namespace LocalLightCull
	{
		void Install();
		/** @brief Render thread, after the shadow epoch drew the paraboloid views: next frame's exclusion (null: none). */
		void Publish(std::shared_ptr<SunExclusion> a_exclusion);
		/** @brief Render thread, at the scene frame's start (after the frame's claims are selected): this frame's exclusion. */
		void SelectFrame(std::uint32_t a_frame);
		/** @brief PassCapture's registration hook, on parity frames: a pass the skip would have kept from the engine. */
		void NoteRegistration(const RE::BSBatchRenderer* a_batch, const RE::BSRenderPass* a_pass, bool a_withheld, std::uint32_t a_source = 0);
		std::string Report();
	}
}
