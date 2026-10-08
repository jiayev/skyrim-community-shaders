#pragma once

namespace DCLF::MirrorWatch
{
	/**
	 * @brief CS_DCLF_MIRROR_WATCH=1 (step 6e F3): hardware write watchpoints (the debug registers, every thread of the process) on
	 * one fade node's statics (+0x108, covering +0x109; near +0x128; far +0x12C; +0x150, covering the LOD type at +0x153), armed
	 * by the hook that just set them (the cell's placement of a reference). Every write that changes a watched dword records the
	 * instruction after it, so a writer the mirror's hooks miss can be named. One node at a time; disarmed after 16 changes or
	 * 600 frames, then armed again on the next placement. Diagnostic only.
	 */
	void Arm(const RE::NiAVObject* a_node);
	/** @brief The report (empty when nothing), and the disarm when it is due. Render thread. */
	std::string TakeReport(std::uint32_t a_frame);
	bool Enabled();
}
