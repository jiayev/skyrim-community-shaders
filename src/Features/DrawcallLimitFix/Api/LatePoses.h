#pragma once

// The late pose contract (Api::InstallContext::RegisterLatePose): a scene graph whose parts a plugin hides after the shadow maps are
// drawn, by shrinking nodes to almost nothing, so that the shadows keep them and the views drawn after the shadows do not. Improved
// Camera hides the first-person head and arms this way (Integrations/ImprovedCamera).
//
// The plugin sets a node's local scale to a tiny value and updates the world transforms; it puts the local scale back by the next
// frame's start. So after its step a hidden node's world scale is far below its parent's. DCLF looks for that at the depth commit
// (Core/LatePoses). The next frame's producer applies the same shrink to the rows it sampled at the frame's start, into a second
// copy of the placement and palette buffers. The epochs from the depth commit on read that copy when the frame's own check confirms
// the hide. Nothing waits: the copy goes up with the frame's other rows. A hide that starts shows one frame late.
//
// This stands in for an API that sets view masks on geometry. It will be replaced by that.

#include <string_view>

namespace DCLF::Api
{
	/**
	 * @brief A late pose contribution.
	 *
	 * - name: the contribution's (the install's failures, the report).
	 * - root: render thread, at the frame's start and at the depth commit: the root node (an RE::NiAVObject) to search for nodes
	 *   shrunk after the shadow maps, or null for none this frame. Name it only while the plugin may hide parts of it (Improved
	 *   Camera's player in first person).
	 */
	struct LatePoseContribution
	{
		std::string_view name;
		const void* (*root)() = nullptr;
	};
}
