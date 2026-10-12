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

	/**
	 * @brief T7f checklist item 6, the actors' animation rate (skyrim-engine-notes.md, "T7f: the fade nodes' OnVisible stores and the
	 * actors' animation stamp"). A high actor whose 3D the actor pre-cull left kNotVisible (bit 20; the race's flag bit 7 clear) animates at full rate in a frame only when its skin
	 * property (MiddleHighProcessData +0x178) carries that frame's gFrameCounter at +0x80 (Actor::ModifyAnimationUpdateData,
	 * 0x1406a37ef: the difference must be under fMaxFrameCounterDifferenceToConsiderVisible, so 0). Only the depth and shadow
	 * registrations stamp it (FUN_1414b2a60 and its siblings, before the batch renderer's RegisterPass), so it is lost once the culls
	 * stop, and for an actor the sun's entry exclusion keeps out of the cascade culls.
	 *
	 * Main thread at Main::Update's entry (no job runs; gFrameCounter is this frame's until Renderer::End): DCLF's verdict per high actor
	 * is its 3D's world bound inside any of the last scene frame's shadow views (SunViews' cascade culls, LightViews' local views) - the
	 * views DCLF draws, from the cameras the engine set up, no cull result read. The producer stamps the property
	 * of every actor with that verdict; CS_DCLF_ACCUMULATED_PARITY first compares, for the kNotVisible actors, the stamp the last frame
	 * left (the engine's, where DCLF did not stamp it) with the verdict of the same frame's views: <- ACTOR RATE on an actor the engine
	 * animated and DCLF would not.
	 */
	void UpdateActorRate(bool a_running);

	/**
	 * @brief CS_DCLF_FADE_PARITY's <- FADE DOUBLE attribution: FUN_14147aa20, a fade node's placement snap (a cell attach of its reference),
	 * writes +0x13C as an OnVisible does. DCLF follows it as an event (the root's row seeded again from the node: SceneStore's
	 * fadeSnapEvents), so a root it snapped is not a second writer. Any thread (the snap's); nothing is kept without the switch.
	 */
	void NoteFadeSnap(const void* a_node);
	/** @brief The main thread at Main::Update's entry: the nodes snapped since the last call. */
	void TakeFadeSnaps(std::vector<const void*>& a_out);
	/** @brief The actor rate line since the last call (empty when neither the producer nor the observer ran). */
	std::string ActorRateReport();
}
