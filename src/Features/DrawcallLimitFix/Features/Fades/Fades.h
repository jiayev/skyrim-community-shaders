#pragma once

// Fades (dclf-architecture.md, "Fades on the GPU"; skyrim-engine-notes.md, "Fade state: inputs, outputs and their other readers"): the
// BSFadeNode/BSLeafAnimNode/BSTreeNode OnVisible port and the producer of root states (Api/RootStates.h). The feature lists the fade nodes
// its members draw under in the scene's tables (the fade roots: a journaled table family, written by its coordinator component:
// FadeRoots), steps their OnVisible on the GPU (FadeStateCS.hlsl: a pass at the depth segment's cull state, its scene resources, seeds
// pool and depth commit hook), writes the stood-in roots' milestones back onto the nodes and records the engine's own fade writers (the
// fade writers' hooks and channels). Core keeps the root-state vocabulary (the rows and bits in Core/Scene/Model/Records.h) and reads the roots
// as data. Listed last in Features/FeatureList.cpp: every shared point keeps its order.

#include "Features/DrawcallLimitFix/Api/Feature.h"

namespace DCLF::Features::Fades
{
	// FadesFeature.cpp: the feature's entry (Features/FeatureList.cpp).
	Api::Feature Feature();
}
