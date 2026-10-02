#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace RE
{
	class BSBatchRenderer;
	class BSRenderPass;
	class NiAVObject;
	class NiNode;
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

		/**
		 * The category filter (CS_DCLF_LIGHT_LIST, on with the skip): inside a point light's cull, a category node (an exact
		 * NiNode that is the parent of excluded entries) culls its children less those entries, through NiNode::OnVisible's
		 * vtable slot, instead of reaching each to skip it. Everything above (the cells and their multibound tests, kAllPass,
		 * the portals' compound frustum) stays the engine's, so every light takes it, portal-strict ones included. Built at
		 * the selection for a new exclusion; a node a child is attached to or detached from is walked natively until the
		 * next selection builds it again. Parity frames filter nothing and check that every caster the engine registers
		 * under a filtered node hangs from a child it keeps.
		 */
		/** @brief Any thread, the attach and detach detours (SceneTracker): a_child attached to or detached from a_parent. */
		void NoteStructure(const RE::NiNode* a_parent, RE::NiAVObject* a_child, bool a_attached);
		/**
		 * @brief SunAccumulation's registration hook (FUN_1414b2140), a shadow light's accumulator: on parity frames, a point
		 * light's mask write (the light's bit into activeLightMask) on a geometry the engine draws in the main pass with a
		 * Lighting property, which a cut entry would lose. a_owned: DCLF draws its main pass (its mask stays 0).
		 */
		void NoteMaskWrite(const void* a_accumulator, const RE::BSGeometry* a_geometry, bool a_owned);
		/** @brief At Present, render thread: retired filters are released (two Presents later). */
		void EndFrame();
	}
}
