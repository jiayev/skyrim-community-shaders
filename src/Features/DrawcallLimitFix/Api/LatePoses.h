#pragma once

// The late pose contract (Api::InstallContext::RegisterLatePose): a reference whose skins a plugin poses again after the shadow maps
// are drawn, so that the shadows keep one pose and the views drawn after them show another. Improved Camera hides the first-person
// head and arms this way (Integrations/ImprovedCamera).
//
// DCLF samples every skin once, at the frame's start, and every view draws from that sample. For a reference named here, DCLF samples
// its skins again at the depth commit, on its own pool, after running their palette update (which runs once a frame: a skin whose
// frame id was set back is posed again, one the engine posed again already is read as it is). The epochs from the depth commit on (depth, colour, forward) read new placement and palette buffers, filled with the
// new rows; the shadow epoch keeps the first ones. The new buffers are filled on every frame the reference is named, so name it only
// while the plugin may pose it (Improved Camera's: in first person). Nothing waits on the render thread, and the new rows' copies wait
// only for earlier frames' work, never for this frame's (FrameRetirement's SealedGpuPoint): on both pipelines, with a copy queue of
// its own or one shared with the graph's copies.

#include <string_view>

namespace DCLF::Api
{
	/**
	 * @brief A late pose contribution.
	 *
	 * - name: the contribution's (the install's failures, the report).
	 * - reference: render thread, every frame, twice: at the frame's start (a reference named then reserves the new buffers) and at the
	 *   depth commit (before the depth epoch is submitted): the reference whose skins may have been posed again since the frame's start,
	 *   or null for none this frame. Read as a key only: never dereferenced by DCLF. A frame that names it only at the depth commit keeps
	 *   the first sample.
	 */
	struct LatePoseContribution
	{
		std::string_view name;
		const void* (*reference)() = nullptr;
	};
}
