#pragma once

#include <cstdint>
#include <string>

#include "Features/DrawcallLimitFix/Scene/Records.h"

namespace DCLF
{
	/**
	 * @brief The fade roots' OnVisible for the main camera, ported from the engine (AE 1.6.1170; skyrim-engine-notes.md,
	 * "Fade state: inputs, outputs and their other readers"): BSFadeNode::OnVisible (0x141479f50), BSLeafAnimNode's
	 * (0x14147c9c0), BSTreeNode's (0x14147d3c0) and what they call, the fade value (FUN_14147b110), the fade update
	 * (FUN_14147a160) and the LOD step (FUN_14147a430). Instruction for instruction (the operations' order, MINSS / MAXSS
	 * and the comparisons' unordered cases), so that the port, FadeStateCS.hlsl and the engine agree to the bit.
	 *
	 * The state is the node's fields (FadeNodeState); the inputs are the root's static row and the frame (FadeFrame). The
	 * one value the port does not compute is the LOD step's distance scale (logf / powf of the bound radius), which the
	 * static row carries from the CPU's CRT (FadeRootStatic::lodScale).
	 */
	namespace FadeState
	{
		/**
		 * @brief The root's update for a frame its bound is in view (Process1's test passed): the tree height test, then the
		 * OnVisible of its class. Returns the verdict bits (kFadeVerdict*), kFadeVerdictInView included.
		 */
		std::uint32_t OnVisible(FadeNodeState& a_state, const FadeRootStatic& a_root, const float a_centre[3], const FadeFrame& a_frame);
		/** @brief The animation job's update (FUN_1402cff60's FUN_14147a160) with the frame's anim* inputs: FadeStateCS's AnimatedUpdate. */
		void AnimatedUpdate(FadeNodeState& a_state, const FadeRootStatic& a_root, const float a_centre[3], const FadeFrame& a_frame, std::uint32_t a_updates);

		/** @brief Render thread: this frame's inputs, from the main camera (null: none this frame) and the engine's globals. */
		FadeFrame SampleFrame(const RE::NiCamera* a_camera);

		/** @brief The node's fade fields as it holds them. */
		FadeNodeState ReadNode(const RE::NiAVObject& a_node);
		/**
		 * @brief The state onto the node (render thread, while nothing reads the fade nodes): the fade bit of the flags atomically
		 * (other writers own the other bits), the rest plainly. Returns whether the fade or the LOD level changed, which the fade
		 * watch is told of.
		 */
		bool WriteNode(RE::NiAVObject& a_node, const FadeNodeState& a_state);
		/** @brief The bytes a node of the plan holds (BSFadeNode, BSLeafAnimNode, BSTreeNode), for a copy the engine's functions may run on. */
		std::size_t NodeBytes(std::uint32_t a_plan);
		inline constexpr std::size_t kMaxNodeBytes = 0x1C0;
		/** @brief The plan (kFadeRoot*) by the node's OnVisible: BSFadeNode's, BSLeafAnimNode's, BSTreeNode's, or another. */
		std::uint32_t PlanOf(const RE::NiAVObject& a_node);
		/** @brief The node's static row (its inputs, and its state as `initial`), for listing it. Generation and object left 0. */
		FadeRootStatic StaticOf(const RE::NiAVObject& a_node);
		/** @brief kFadeRootTreeLod: a tree whose LOD switch selects past child 0 (BSTreeNode::OnVisible's LOD fix-up). */
		bool TreeLodSelected(const RE::NiAVObject& a_node);
		/** @brief The tree's LOD switch (+0x180), whose selection changes by switch event (kFadeRootTreeLod), or null. */
		const RE::NiAVObject* TreeLodSwitch(const RE::NiAVObject& a_node);

		/**
		 * @brief CS_DCLF_FADE_PARITY, render thread, before the list jobs: the port against the engine's own functions, run on
		 * a copy of the node (they read and write only the node and the globals) with the fade watch muted. The node itself is
		 * not touched. Counts into the report; a_first takes the first difference.
		 */
		struct PortCheck
		{
			std::uint64_t checked = 0, differ = 0;
			std::string first;
		};
		void CheckPort(const RE::NiAVObject& a_node, const RE::NiCamera& a_camera, PortCheck& a_check);
		/** @brief The fields that differ between two states (empty when none). */
		std::string Differences(const FadeNodeState& a_expected, const FadeNodeState& a_actual);
	}
}
