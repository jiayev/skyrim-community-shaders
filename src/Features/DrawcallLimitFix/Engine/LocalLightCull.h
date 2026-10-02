#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace RE
{
	class BSBatchRenderer;
	class BSGeometry;
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
	 * views draw and the registration then withholds (PassCapture). The exclusion is BuildSunExclusion's for the
	 * paraboloid mode, over the light candidates (SceneStore::GetLightCandidates): an entry all of whose shadow casters are that
	 * mode's inputs. The process's Process1 (vtable slot 0x16)
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
		 * The point lights' bits. A point light's registration (FUN_1414b2140) ORs its bit into the activeLightMask of every
		 * geometry its cull reaches, which the main pass reads (GetRenderPasses: the shadow lights). A cut entry's geometries
		 * take them from DCLF instead, at their main registration: per light whose cull reached the entry (its category node in
		 * the filter, or the entry where Process1 skipped it), the light's bit when the geometry's bound passes the light's
		 * test (FUN_14151a1e0, its sphere), or untested where the cull reached it untested (cull mode 1). A portal-strict light's
		 * compound frustum is not tested. Parity frames compare them with the engine's.
		 */
		/**
		 * @brief SunAccumulation's registration hook, a main registration (+0x160 0xFFFF: it reads the mask, then clears it) of
		 * a geometry DCLF does not draw in the main pass, before the call: a cut entry's takes the lights' bits.
		 */
		void NoteMainRegistration(RE::BSGeometry* a_geometry);
		/** @brief At Present, render thread: retired filters are released (two Presents later). */
		void EndFrame();
	}
}
