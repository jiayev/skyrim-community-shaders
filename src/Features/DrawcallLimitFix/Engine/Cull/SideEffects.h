#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>


namespace DCLF::SideEffects
{
	/**
	 * @brief The engine side effects of the main camera's cull that DCLF produces where the culls are stood in for
	 * (skyrim-engine-notes.md, "Engine side effects of the culls"). kAccumulated (NiAVObject flag bit 26) is DCLF's own output: VisibilityEventsCS's change events,
	 * read back and decoded (IndirectDraws, VisibilityReadback), written on the roots the cut stands in for at Main::Update's entry
	 * (IndirectDraws::WriteAccumulated); its observers are Diagnostics::Accumulated's. The actors' animation stamp is here.
	 *
	 * The actors' animation rate (skyrim-engine-notes.md, "the fade nodes' OnVisible stores and the actors' animation stamp"). A
	 * high actor whose 3D the actor pre-cull left kNotVisible (bit 20; the race's flag bit 7 clear) animates at full rate in a frame only
	 * when its skin property (MiddleHighProcessData +0x178) carries that frame's gFrameCounter at +0x80 (Actor::ModifyAnimationUpdateData,
	 * 0x1406a37ef: the difference must be under fMaxFrameCounterDifferenceToConsiderVisible, so 0). Only the depth and shadow
	 * registrations stamp it (FUN_1414b2a60 and its siblings, before the batch renderer's RegisterPass), so it is lost once the culls
	 * stop, and for an actor the sun's entry exclusion keeps out of the cascade culls.
	 *
	 * Main thread at Main::Update's entry (no job runs; gFrameCounter is this frame's until Renderer::End): DCLF's verdict per high actor
	 * is its 3D's world bound inside any of the last scene frame's shadow views (the Sun board's cascade culls, the Lights board's local views) - the
	 * views DCLF draws, from the cameras the engine set up, no cull result read. The property of every actor with that verdict is
	 * stamped.
	 */
	void UpdateActorRate(bool a_running);

	/** @brief The actor rate's counts since the last Take (main thread; the report reads them on the render thread between frames). */
	struct ActorRate
	{
		std::uint64_t entries = 0, actors = 0, noProperty = 0, stamped = 0, inSun = 0, inLocal = 0;
	};
	ActorRate TakeActorRate();
}
