#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace DCLF::Diagnostics::Accumulated
{
	/**
	 * @brief CS_DCLF_ACCUMULATED_PARITY's engine side. kAccumulated (NiAVObject flag bit 26) is DCLF's output: VisibilityEventsCS's change
	 * events, read back and decoded (IndirectDraws, VisibilityReadback), written on the roots the cut stands in for at Main::Update's
	 * entry (IndirectDraws::WriteAccumulated). The observers here:
	 * - <- ACCUMULATED: the bit as the engine's cull left it, sampled on the render thread at the list jobs' end (ListJobsDone: the cull
	 *   is done, nothing else writes the bit until the next frame's), kept until the decode reaches the same frame and compared there
	 *   exactly (no margin, no hysteresis);
	 * - <- ANIM RATE: FUN_1402cff60, the gate on ref->Get3D() that decides whether an animated reference updates (UpdateAnimationJob)
	 *   and whether its managed nodes do (FUN_1402b41a0, FUN_1402b52e0), detoured at its three calls. For each root the cut stands in
	 *   for, its result against DCLF's prediction: the bit WriteAccumulated left at Main::Update's entry. A stood-in root DCLF wrote
	 *   visible that the gate rejects (not faded out) is a difference; the stood-in animated references are counted by kind (never
	 *   fading: ignore-fade, zero radius or flag bit 29; fading);
	 * - <- FRAME CAMERA: the list processes' camera against the main camera BeginFrameCamera read (FadeEye, the camera cut and the
	 *   height test come from it).
	 */
	void Install();

	/** @brief A frame's sample: per slot of the frame's tables, the node (null: no root or object there) and its kSample* bits. */
	struct Sample
	{
		std::uint32_t frame = 0;
		std::vector<const void*> rootNodes;       // Tables::fadeRootNode
		std::vector<std::uint8_t> rootBits;
		std::vector<const void*> objectGeometry;  // Tables::objectGeometry (a layer's: null, its base slot's geometry is sampled)
		std::vector<std::uint8_t> objectBits;
	};
	inline constexpr std::uint8_t kSampleAccumulated = 1u << 0;      // the node's kAccumulated
	inline constexpr std::uint8_t kSampleStoodIn = 1u << 1;          // a root the engine's cull did not visit (the cut stood in), or a geometry under one
	inline constexpr std::uint8_t kSampleRootAccumulated = 1u << 2;  // a geometry: its fade root's kAccumulated (set for one without a root)
	inline constexpr std::uint8_t kSampleHidden = 1u << 3;           // a root: app-culled (kHidden): the engine's cull does not reach it

	/**
	 * @brief Render thread, the frame-start wake: the samples up to a_through (in frame order), for the decode that reaches them; then
	 * those before a_oldestPending no batch will reach, dropped and returned as a count.
	 */
	std::vector<std::unique_ptr<Sample>> TakeSamples(std::uint32_t a_through);
	std::uint64_t DropSamples(std::uint32_t a_oldestPending);

	/** @brief The animation gate's three call sites, named. */
	extern const std::array<const char*, 3> kAnimGateCallers;
	/** @brief <- ANIM RATE's counts since the last Take. */
	struct AnimRate
	{
		std::uint64_t predictions = 0, roots = 0, set = 0;
		struct Site
		{
			std::uint64_t calls = 0, neverFade = 0, fading = 0, rejected = 0, rejectedNeverFade = 0, predictedOut = 0, acceptedOut = 0, fadedOut = 0, differ = 0;
			const void* firstDiffer = nullptr;
		};
		std::array<Site, 3> sites{};
	};
	AnimRate TakeAnimRate();

	/** @brief <- FRAME CAMERA's counts since the last Take: frames checked, and those whose list processes carried another camera. */
	struct FrameCamera
	{
		std::uint64_t checked = 0, differ = 0;
	};
	FrameCamera TakeFrameCamera();
}
