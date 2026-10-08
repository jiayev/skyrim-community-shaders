#pragma once

namespace DCLF::MirrorWatch
{
	/**
	 * @brief CS_DCLF_MIRROR_WATCH (step 6e F3): hardware write watchpoints (the debug registers, every thread of the process) on
	 * one object's mirrored fields, armed by the hook that just captured or set them. Every write that changes a watched dword
	 * records the instruction after it, so a writer the mirror's hooks miss can be named. One object at a time; disarmed after 16
	 * changes or 600 frames, then armed again on the next. Diagnostic only. The value:
	 * - 1, or a node's name: a fade node at the cell's placement of its reference, its statics (+0x108, covering +0x109; near
	 *   +0x128; far +0x12C; +0x150, covering the LOD type at +0x153);
	 * - geometry:<name>: a geometry at its world attach's capture, its alpha (+0x120) and shader property (+0x128) pointers and
	 *   the property's flags' high dword (+0x3C: bits 32-63) and fade node (+0x60).
	 */
	void Arm(const RE::NiAVObject* a_node);
	void ArmGeometry(const RE::BSGeometry* a_geometry);
	/** @brief node:<name>: a node at its world attach's capture, its collision object (+0x40) and flags (+0xF4, the frame's bits ignored). */
	void ArmNode(const RE::NiAVObject* a_node);
	/** @brief parity: a shader property the mirror parity found stale (its flags, both dwords, and its fade node). Render thread. */
	void ArmProperty(const void* a_property);
	/** @brief parity: an alpha property the mirror parity found stale (its flags and threshold). Render thread. */
	void ArmAlpha(const void* a_alpha);
	/** @brief The report (empty when nothing), and the disarm when it is due. Render thread. */
	std::string TakeReport(std::uint32_t a_frame);
	bool Enabled();
}
