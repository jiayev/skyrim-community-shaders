#pragma once

// Switch nodes (CS_DCLF_SWITCH_NODES; dclf-architecture.md, "Switch nodes"): the leaves under an NiSwitchNode DCLF draws, in the frames
// every switch on their path selects their branch (trees' variants, harvestables). The feature registers their switch and live toggle, and
// the parent contribution that admits them (Api::ParentContribution: the classification and the sun and light candidates read it as data).
//
// Core keeps the selections themselves: the primary cull stands in for whole roots and so follows every switch below them, whatever the
// toggle - the index stores' and child edits' events (Core/Engine/Capture/SceneEvents.h, SwitchEvents; without their hooks DCLF does not run),
// the coordinator's pending switches and their walk (ApplySwitchEvents), the render thread's catch-ups of NiSwitchNode::OnVisible's
// update (FrameView::CatchUpSwitches), the cut's selection reads (PrimaryCull, memberLive), tree LOD's switch (the fade roots'
// fadeRootSwitch, FadeRootTables), the parent reason (Ineligible::Switch) and the chain test (Coordinator::SwitchSelects). Included by the
// feature's sources only.

#include "Features/DrawcallLimitFix/Api/Feature.h"

namespace DCLF::Features::SwitchNodes
{
	// SwitchNodesFeature.cpp: the feature's entry (Features/FeatureList.cpp).
	Api::Feature Feature();
}
