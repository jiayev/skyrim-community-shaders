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
		 * The light list (CS_DCLF_LIGHT_LIST, on with the skip): a light without a root list of its own culls the object root's
		 * entries less the excluded ones, through the engine's list path (FUN_140e28f70: BSShadowLight::sceneAccumArray, lent
		 * for the call), instead of walking the whole object root to skip them. The list: below the object root, every plain
		 * NiNode down to the category nodes is gone through (not hidden; its OnVisible only culls its children), and whatever
		 * else is reached is listed whole (the two whole entries, an entry, a node of another class), the excluded entries
		 * left out. Kept by the attach and detach events under those nodes and the hidden events on them; built again when
		 * the exclusion changes. A light uses it only while nothing has moved since the frame's selection; parity frames walk
		 * the object root and check that every caster the engine registers is under a listed root.
		 */
		/** @brief Any thread, the attach and detach detours (SceneTracker): a_child attached to or detached from a_parent. */
		void NoteStructure(const RE::NiNode* a_parent, RE::NiAVObject* a_child, bool a_attached);
		/** @brief Render thread, SceneStore's hidden events: a hidden bit written on a node. */
		void NoteHiddenKey(const void* a_key);
		/** @brief Any thread, the list cull: whether a_list is a light list lent on this thread (its hidden entries are skipped). */
		bool OwnsList(const void* a_list);
		/** @brief At Present, render thread: the roots the list let go of are released. */
		void EndFrame();
	}
}
