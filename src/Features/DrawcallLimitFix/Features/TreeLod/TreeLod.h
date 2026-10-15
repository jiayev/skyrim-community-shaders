#pragma once

// TreeLod (CS_DCLF_LOD_TREES): the tree LOD instances (the BSDistantTreeShaderProperty shapes under the LOD trees root) drawn by Core's
// tree LOD path in the engine's place, from the mirrored instance records. The feature registers its switches (CS_DCLF_LOD_TREES, the
// audit's CS_DCLF_TREE_LOD_AUDIT), its live toggle and the tree LOD class's contribution (Api::ObjectClass::TreeLod, Api/ObjectContributors.h:
// DecideTreeLod, the revisions' trees pipelines and a trees view's tree LOD per face read it as data), installs the source of Core's tree
// LOD channel at its early hooks (TreeLodHooks.cpp: the refill, block update, AddGroup and RemoveGroup, pushing onto TreeLod::events),
// and runs the audit of the open defect it probes (Diagnostics/TreeLodAudit.cpp).
//
// Core keeps the channel and the mirror (Scene/Ports/TreeLod.h, drained by FrameView), the GPU path that draws it (the tables and their
// uploads, TreeLodCullCS in the depth segment's two phases, the depth and colour draws, its pipelines in DrawPipelines, the revisions'
// parts, DecideTreeLod), a trees view's tree LOD per face (the view engine's: Core/Render/Views/ViewTreeLod.cpp, the views' epochs),
// PassCapture's withholding of the engine's tree LOD passes, the mirror's parity and the report lines of all of these.
// Included by the feature's sources only.

#include "Features/DrawcallLimitFix/Api/Feature.h"

namespace DCLF::Features::TreeLod
{
	// TreeLodFeature.cpp: the feature's entry (Features/FeatureList.cpp).
	Api::Feature Feature();

	// TreeLodHooks.cpp: the channel's source (the feature's early hooks), patched once at PostPostLoad; sets TreeLod::installed.
	void InstallHooks();

	// Diagnostics/TreeLodAudit.cpp: CS_DCLF_TREE_LOD_AUDIT=1 (a_switch), main thread at the scene frame's start (FrameBegin), with or
	// without DCLF: the attached tree LOD blocks' instances (BGSDistantTreeBlock) against their references. An instance shown while its
	// reference's full tree is loaded and visible is drawn over it (counted apart while the tree's fade is above 0: the engine crossfades
	// the two, the instance's alpha 1 - the fade); one hidden without a visible full tree leaves a hole (dclf-lod.md, "Tree instance
	// hiding").
	void SubscribeAudit(Api::SwitchId a_switch);
}
