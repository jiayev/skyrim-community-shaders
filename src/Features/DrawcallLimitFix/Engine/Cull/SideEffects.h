#pragma once

namespace DCLF::SideEffects
{
	/**
	 * @brief The engine side effects of the main camera's cull that DCLF produces where the culls are stood in for
	 * (skyrim-engine-notes.md, "Engine side effects of the culls"). kAccumulated (NiAVObject flag bit 26) is DCLF's own output: VisibilityEventsCS's change events,
	 * read back and decoded (IndirectDraws, VisibilityReadback), written on the roots the cut stands in for at Main::Update's entry
	 * (IndirectDraws::WriteAccumulated); its observers are Diagnostics::Accumulated's. The actors' animation stamp is Features/Actors'
	 * (ActorRate.cpp, at the stand-in observers' afterWrites).
	 */
}
