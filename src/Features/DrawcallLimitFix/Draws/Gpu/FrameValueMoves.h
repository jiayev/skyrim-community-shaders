#pragma once

// FrameValues' own move events (dclf B, "FrameValues samples only what moved"): the move writers' keys and Havok's node writes, pushed by
// the same hooks as the scene pass's MoveEvents and node events (SceneEvents.cpp) into a second pair of queues, whose one consumer is
// FrameValues' producer (one at a time, in order). The scene pass keeps its own queues; neither drains the other's.

#include "Features/DrawcallLimitFix/Core/Runtime/EventQueue.h"

namespace DCLF::FrameValueMoves
{
	/**
	 * @brief Keys of what an update pass moved, as MoveEvents has them (a reference, or the node an update pass keyed itself by). Never
	 * dereferenced: the producer looks them up in its plan's index (FrameValues: a mover's keys are its geometry, the references on its
	 * chain, and with none every node above it).
	 */
	// Per-frame: no wake (thousands a frame); the next producer drains them.
	inline EventQueue<const void*> keys;
	/**
	 * @brief Nodes Havok wrote from their rigid bodies (FUN_140ea55a0) or that gained a controller (PrependController). Held: the producer
	 * names the reference above each under an engine-read lease, then lets the reference go through EngineReleases.
	 */
	inline EventQueue<RE::NiPointer<RE::NiAVObject>> nodes;

	/** @brief Any thread. */
	inline void PushKey(const void* a_key)
	{
		if (a_key)
			keys.Push(a_key);
	}

	/** @brief Any thread. */
	inline void PushNode(RE::NiAVObject* a_node)
	{
		if (a_node)
			nodes.Push(RE::NiPointer<RE::NiAVObject>(a_node));
	}
}
