#pragma once

namespace DCLF::HiddenWatch
{
	/**
	 * @brief CS_DCLF_HIDDEN_WATCH=1: hardware write watchpoints (the debug registers, every thread of the process) on up to
	 * four NiAVObject::flags fields, armed when an actor's frame verdict changed with no hidden event
	 * (SceneStore::ActorRecordKept). A write that flips kHidden (bit 0) records the instruction after it, so the store the
	 * hidden events miss (HiddenStores.cpp) can be named. At most one arming at a time; disarmed after 600 reports' worth
	 * of frames or 16 flips. Diagnostic only.
	 */
	void Arm(const std::array<const RE::NiAVObject*, 4>& a_nodes);
	/** @brief The report, once per interval: what the watch recorded (empty when nothing), and the disarm when it is due. */
	std::string TakeReport(std::uint32_t a_frame);
	bool Enabled();
}
