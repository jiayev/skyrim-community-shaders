#pragma once

// The material-write sink's Core side (Api/MaterialWrites.h): whether a feature patches the frame VS variable, read where a texture-transform
// write or capture is skipped (the material write hooks, Coordinator::DrainMaterialCaptures); and the transform moves, the kept shadow
// build's.

#include "Features/DrawcallLimitFix/Api/MaterialWrites.h"

#include <cstdint>
#include <vector>

namespace DCLF::Extensions
{
	/**
	 * @brief Any thread, lock-free (false before the features' install): whether a feature patches every material row's frame VS variable
	 * and the ports' declared frame variables with the frame's value (Api::InstallContext::RegisterFrameVSVariablePatch:
	 * Features/TextureTransforms), so the texture transforms and those variables are no record's.
	 */
	bool FrameVSVariablePatched();
	/** @brief Main thread, at the features' install (the registry's RegisterFrameVSVariablePatch): a feature patches it. Once or more. */
	void NoteFrameVSVariablePatch();

	/**
	 * @brief The kept shadow build's thread alone (its one consumer, ShadowBuild's WatchTransformMoves): every material whose texture
	 * transform a controller moved since the last call (the material write hooks queue them, lock-free, whatever patches the frame VS
	 * variable). Of the two shadow builds, the kept one rewrites a material row on events, so it needs every move (its rows read the
	 * transform on the CPU in every configuration); the plain one rereads every row each build (ShadowTexcoord) and drains nothing. The
	 * records do not use this channel (they follow the writes' captures, or the frame VS variable's patch: FrameVSVariablePatched).
	 * Defined with the hooks (Core/Shading/MaterialWrites.cpp).
	 *
	 * Returns true when moves were dropped: the hooks stop queueing while no build has drained for a while (DCLF off, or not
	 * drawing shadows), and before the first drain. The caller then treats every material it keeps as moved.
	 */
	bool DrainTransformMoves(std::vector<const RE::BSShaderMaterial*>& a_out);

	/** @brief The transform moves' counts since startup (any thread): moves the full queue allocated for, and moves dropped while no build drained. */
	struct TransformMoveCounts
	{
		std::uint64_t spilled = 0;
		std::uint64_t dropped = 0;
	};
	TransformMoveCounts TransformMoveStats();
}
