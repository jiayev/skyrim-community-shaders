#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace DCLF::SideEffects
{
	/**
	 * @brief The engine side effects of the main camera's cull that DCLF must produce before the culls stop (T7b;
	 * skyrim-engine-notes.md, "Engine side effects of the culls (T7a)"). T7b1 makes the first, kAccumulated (NiAVObject flag bit 26),
	 * as DCLF's own output: VisibilityEventsCS's change events, read back and decoded (IndirectDraws, VisibilityReadback). This is
	 * the engine's side of its observer, CS_DCLF_ACCUMULATED_PARITY (<- ACCUMULATED): the bit as the engine's cull left it, sampled
	 * on the render thread at the list jobs' end (PrimaryCull::AfterListJobs: the cull is done, nothing else writes the bit until
	 * the next frame's), kept until the decode reaches the same frame and compared there exactly (no margin, no hysteresis).
	 *
	 * T7b2 writes the bit on the roots the cut stands in for (IndirectDraws::WriteAccumulated, at Main::Update's entry); the readback
	 * and the decoder are the normal path for it. The switch gates the observers alone: this sampling and <- ANIM RATE below.
	 */
	bool AccumulatedParity();

	/** @brief A frame's sample: per slot of the frame's tables, the node (null: no root or object there) and its kSample* bits. */
	struct AccumulatedSample
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
	 * @brief Render thread, after the list jobs' Finish (the switch on): the bit on the frame's tables' fade roots and geometries.
	 * a_stoodIn(root): whether the cut stood in for the root this frame (its node's bit is not the engine cull's).
	 */
	void SampleAccumulated(std::uint32_t a_frame, const std::function<bool(const void*)>& a_stoodIn);
	/**
	 * @brief Render thread, the frame-start wake: the samples up to a_through (in frame order), for the decode that reaches them; then
	 * those before a_oldestPending no batch will reach, dropped and returned as a count.
	 */
	std::vector<std::unique_ptr<AccumulatedSample>> TakeAccumulatedSamples(std::uint32_t a_through);
	std::uint64_t DropAccumulatedSamples(std::uint32_t a_oldestPending);

	/**
	 * @brief <- ANIM RATE (T7b2, CS_DCLF_ACCUMULATED_PARITY): FUN_1402cff60, the gate on ref->Get3D() that decides whether an animated
	 * reference updates (UpdateAnimationJob) and whether its managed nodes do (FUN_1402b41a0, FUN_1402b52e0), detoured at its three
	 * calls (skyrim-engine-notes.md, "T7b0"). For each root the cut stands in for, its result against DCLF's prediction: the bit
	 * WriteAccumulated left at Main::Update's entry (PublishAnimPrediction). A stood-in root DCLF wrote visible that the gate rejects
	 * (not faded out) is a difference; the stood-in animated references are counted by kind (never fading: ignore-fade, zero radius or
	 * flag bit 29, the case the old clear froze; fading), so the freeze is measured. Nothing is installed without the switch.
	 */
	void InstallAnimRate();
	/**
	 * @brief Main thread, Main::Update's entry, after the writes (no animation job or cell animation runs: the last frame's cycle was
	 * waited for): the stood-in roots and their kAccumulated as now, for this frame's gates. The last frame's prediction is let go here.
	 */
	void PublishAnimPrediction(const std::vector<const void*>& a_stoodInRoots);
	/** @brief The <- ANIM RATE line since the last call (empty without the switch). */
	std::string AnimRateReport();
}
