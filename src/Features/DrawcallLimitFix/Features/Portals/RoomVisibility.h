#pragma once

#include <cstdint>

namespace DCLF::RoomVisibility
{
	/**
	 * @brief Main::IsRoomVisible (0x140649f20) answered from DCLF's PortalViews::VisibleSet (skyrim-engine-notes.md,
	 * "Main::IsRoomVisible and its callers").
	 *
	 * The engine answers whether a node (the parent, +0x30, of a reference's 3D) is in a room or portal the main camera sees, from
	 * the portal-graph entry of the scene lists' first process (+0x30190): the managed-nodes update (FUN_1402b41a0, from
	 * UpdateManagedNodesJob and UpdateAnimatedRefsJob), Actor::UpdateRefLight and the actor update FUN_140771520 call it, on job
	 * threads. Its three calls are detoured: while DCLF runs (SetLive, Main::Update's entry) and a set is published, the call is
	 * answered from DCLF's VisibleSet (DclfAnswer) and the engine's function is not called; otherwise (DCLF off) the engine answers.
	 */
	void Install();
	bool Installed();
	/** @brief The main thread at Main::Update's entry: whether DCLF runs this frame (DCLF answers only then). */
	void SetLive(bool a_live);
	/**
	 * @brief DCLF's answer for a_key (the node the engine was given), from the newest VisibleSet: true without one. Any thread; reads
	 * no engine memory. The detour's answer.
	 */
	bool DclfAnswer(const void* a_key);
	/** @brief The calls answered since the last Take, by DCLF's VisibleSet and by the engine (Features::Portals::RoomVisibilityLines). */
	struct Answers
	{
		std::uint64_t dclf = 0, engine = 0;
	};
	Answers Take();
}
