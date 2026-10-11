#pragma once

namespace DCLF::RoomVisibility
{
	/**
	 * @brief Main::IsRoomVisible (0x140649f20) against DCLF's PortalViews::VisibleSet (T7b3; skyrim-engine-notes.md, "Engine side
	 * effects of the culls (T7a)"). AE only.
	 *
	 * The engine answers whether a node (the parent, +0x30, of a reference's 3D) is in a room or portal the main camera sees, from
	 * the portal-graph entry of the scene lists' first process (+0x30190): the managed-nodes update (FUN_1402b41a0, from
	 * UpdateManagedNodesJob and UpdateAnimatedRefsJob), Actor::UpdateRefLight and the actor update FUN_140771520 call it, on job
	 * threads. Its three calls are detoured only under CS_DCLF_ROOM_PARITY: the engine's answer is returned, and compared with
	 * DCLF's (keys only, from the newest published set), the differences counted by kind and side. Without the switch nothing is
	 * installed and PortalViews publishes no set.
	 */
	void Install();
	bool Installed();
	/**
	 * @brief DCLF's answer for a_key (the node the engine was given), from the newest VisibleSet: true without one. Any thread; reads
	 * no engine memory. The hook point for T7f, when the engine's culls stop and the detour returns this instead.
	 */
	bool DclfAnswer(const void* a_key);
	/** @brief The parity's line (`<- ROOM`), counters reset. Render thread: PortalViews::Build, every 300 published sets. */
	void Report();
}
