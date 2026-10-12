#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace DCLF::TreeList
{
	/**
	 * @brief T7b5: the engine's tree manager list loop, ported as a DCLF computation and run as a dry run against the engine's
	 * (drawcall-limit-fix.md, "T7b5"; skyrim-engine-notes.md, "Tree manager list (T7b5)"). AE only.
	 *
	 * BSTreeManager's update (FUN_140437d40, Main::Update 0x140646176, main thread) runs FUN_140437e50 over its tree list (+0x20:
	 * BSTreeNodes, exact RTTI) and then FUN_1404381e0 over its animation list (+0x50: BSLeafAnimNodes, exact RTTI). Per tree:
	 *   - the tree list: visited only when its first child's multibound has the cull result 1 (or the process's cull mode is 3); a
	 *     visited tree's squared camera distance (+0x158); within the manager's range (+0x80) it joins the near list (+0x38), which is
	 *     sorted by distance and cut to the nearest 40 (0x14200d670), the rest at or past range * 0.25 (0x141ad2848) forced to the LOD
	 *     child; past the range its LOD switch (+0x180) takes the LOD child. FUN_1404381e0 then animates the near list's trees (their
	 *     bones, FUN_140438ed0, and their clock +0x164);
	 *   - the animation list: the clock (+0x164) and gust advance for a node with a base form (userData +0x40) and kAccumulated set.
	 *
	 * Decide is that decision from DCLF's own state: the adopted tables' tree roots (the fade roots whose seed's plan is BSTreeNode's
	 * or BSLeafAnimNode's), the trees' positions (the tree seeds), DCLF's camera (PrimaryCull's list camera of the frame) and the
	 * visibility readback's root verdicts. It never reads the engine's cull or registration results.
	 *
	 * FUN_140437e50's call is detoured always: around it TreeAnimation gives the loop the stood-in trees' multibound results from DCLF's
	 * verdicts and puts the engine's back (T7b5's fix; TreeAnimation.h). CS_DCLF_TREE_PARITY (a parity, off by default; no sample
	 * without it): after the call, before the engine's values are put back, the main thread samples, at that safe point (no list cull or cell animation runs; the engine's lists under their own locks), the
	 * engine's answer (each list's nodes, the multibound results, the near list, the distances, kAccumulated) and DCLF's inputs from
	 * the frame's tables. The visibility decode compares them when it reaches the frame (the tree manager reads the cull of the last
	 * scene frame), exactly: `<- TREE MANAGER` (a stood-in tree aside: its result is DCLF's written verdict). The observer changes
	 * nothing of the engine's behaviour (its time is given back to the manager's budget).
	 */
	void Install();
	bool Installed();

	/** @brief One frame's sample (the engine's answer and DCLF's inputs); opaque outside TreeList. */
	struct Sample;
	struct SampleDelete
	{
		void operator()(Sample* a_sample) const noexcept;
	};
	using SamplePtr = std::unique_ptr<Sample, SampleDelete>;
	std::uint32_t FrameOf(const Sample& a_sample);

	/** @brief Render thread: the samples up to a_through (frame order), for the decode that reaches them. */
	std::vector<SamplePtr> TakeSamples(std::uint32_t a_through);
	/** @brief Render thread: the samples before a_oldestPending no batch will reach, dropped; returns how many. */
	std::uint64_t DropSamples(std::uint32_t a_oldestPending);

	/** @brief DCLF's main-camera verdict of a root slot, as the decode has it after the frame's batch. */
	struct RootVerdict
	{
		bool known = false;    // the slot's listing has a verdict (its generation, FadeStateCS updated it)
		bool visible = false;  // in view (kFadeVerdictInView)
	};
	/**
	 * @brief The decode's thread, for each batch in frame order, after it was applied: the samples of earlier frames (no batch reached
	 * them) counted, and a_frame's compared when its batch is complete: DCLF's answer (the port) against the engine's. a_rootNode(slot)
	 * is the batch tables' node of a root slot (a slot whose node moved since the sample is counted aside); a_verdict(slot) the
	 * decoder's verdict. Reads no engine memory. a_next is the decode's position in a_samples.
	 */
	void OnBatch(const std::vector<SamplePtr>& a_samples, std::size_t& a_next, std::uint32_t a_frame, bool a_complete,
		const std::function<const void*(std::uint32_t)>& a_rootNode, const std::function<RootVerdict(std::uint32_t)>& a_verdict);
	/** @brief The decode's thread, after its batches: the samples none reached, counted. */
	void OnDecodeEnd(const std::vector<SamplePtr>& a_samples, std::size_t a_next);
	/**
	 * @brief Render thread, before the decoded batches are let go: the differences' nodes named (they are held by those batches'
	 * tables, or by a decode still running).
	 */
	void NameFirsts();

	/** @brief The `<- TREE MANAGER` line since the last call (empty without the switch). */
	std::string Report();
}
