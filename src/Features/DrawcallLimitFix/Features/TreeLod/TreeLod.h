#pragma once

// TreeLod (CS_DCLF_LOD_TREES): the tree LOD instances (the BSDistantTreeShaderProperty shapes under the LOD trees root) drawn in the
// engine's place, from the mirrored instance records, as the TreeInstances draw family (Api/DrawFamilies.h). The feature registers its
// switches (CS_DCLF_LOD_TREES, the audit's CS_DCLF_TREE_LOD_AUDIT), its live toggle and the tree LOD class's contribution
// (Api::ObjectClass::TreeLod, which gates the family), installs the source of its channel at its early hooks (TreeLodHooks.cpp: the refill,
// block update, AddGroup and RemoveGroup, pushing onto events) and keeps the render thread's mirror (TreeLodMirror.h, drained by the
// family's serve). It draws it: the tables as scene resources, with what they post with the revision's inputs and latch in the depth
// commit, and the depth commit's uploads as a commit hook (TreeLodTables.cpp); TreeLodCullCS (its HLSL in the feature's shader folder,
// Features/TreeLod/TreeLodCullCS.hlsl) in the depth segment's two phases and the counts' readback as its passes (TreeLodPasses.cpp);
// the family - its frame's decision and program source, its depth and colour draws (TreeLodDraws.cpp) and a trees view's faces (TreeLodFaces.cpp: the per-face cull, draw and row, the lists' units). Its report lines
// (Diagnostics/TreeLodReport.cpp), the mirror's parity (Diagnostics/TreeLodParity.cpp) and the audit of the open defect it probes
// (Diagnostics/TreeLodAudit.cpp) are its own.
//
// Core keeps the family's slot (Core/Render/DrawFamilies/FamilySlots.h): its pipelines on the lane (DrawPipelines' tree LOD family,
// ForwardPipelineKey::kForwardTreeLod) from the program source, the ownership it publishes from the decision (PassCapture's withholding of
// the engine's tree LOD passes), the shapes key's part, and a trees view's lists, rows and latch (sized by the family's faces data: the
// rows, list header and units of the GPU layout the shaders mirror, TreeInstancesLayout.h, and the cull's thread group, kTreeLodCullGroup).
// Included by the feature's sources only.

#include "Features/DrawcallLimitFix/Api/Feature.h"

#include <cstdint>

namespace DCLF::Features::TreeLod
{
	// AE 1.6.1170 (module offsets) of the engine state this feature alone reads.
	namespace Address
	{
		inline constexpr std::uintptr_t kTreeLodAtlas = 0x33dcd18;  // NiSourceTexture*: tree LOD's atlas
	}

	// The engine root a trees view's faces draw in the engine's place while they draw the family (Api::DrawFamilyContribution::faces.root;
	// one owner: Features/Manifest.cpp lists it).
	inline constexpr Api::ViewRoot kFacesRoot = Api::ViewRoot::LodTrees;

	// CS_DCLF_TREE_LOD_TINT, set at install: the main colour draw's push word 3 (DistantTree.hlsl's DCLFTreeDebugTint).
	inline bool debugTint = false;
	// CS_DCLF_TREE_LOD_STAGES, set at install: the main draws' stage stamps (TreeInstancesLayout.h, kStageSlots), read back with the counts.
	inline bool debugStages = false;

	// TreeLodFeature.cpp: the feature's entry (Features/FeatureList.cpp).
	Api::Feature Feature();

	// TreeLodHooks.cpp: the channel's source (the feature's early hooks), patched once at PostPostLoad; sets installed (TreeLodMirror.h).
	void InstallHooks();

	// TreeLodTables.cpp: the tables (scene resources) and the depth commit's uploads (commit hook).
	void RegisterTables(Api::InstallContext& a_context);
	// TreeLodPasses.cpp: the cull in the depth segment's two phases and the counts' readback (tree-lod.cull, tree-lod.cull-phase2,
	// tree-lod.counts).
	void RegisterPasses(Api::InstallContext& a_context);
	// TreeLodDraws.cpp: the TreeInstances draw family (after the tables: its resources).
	void RegisterFamily(Api::InstallContext& a_context);
	// TreeLodFaces.cpp: the family's part in a trees view's faces, and their cull (tree-lod.faces-cull, in a trees view's depth epoch).
	Api::DrawFamilyContribution::Faces FacesContribution();
	void RegisterFacesCull(Api::InstallContext& a_context);
	// Diagnostics/TreeLodReport.cpp: the draws' and the mirror's report lines.
	void RegisterReport(Api::InstallContext& a_context);
	// Diagnostics/TreeLodParity.cpp: CS_DCLF_PERSISTENT_PARITY's mirror check (Diagnostics::Observe().scene).
	void SubscribeParity();

	// Diagnostics/TreeLodAudit.cpp: CS_DCLF_TREE_LOD_AUDIT=1 (a_switch), main thread at the scene frame's start (FrameBegin), with or
	// without DCLF: the attached tree LOD blocks' instances (BGSDistantTreeBlock) against their references. An instance shown while its
	// reference's full tree is loaded and visible is drawn over it (counted apart while the tree's fade is above 0: the engine crossfades
	// the two, the instance's alpha 1 - the fade); one hidden without a visible full tree leaves a hole (dclf-architecture.md, "Tree instance
	// hiding").
	void SubscribeAudit(Api::SwitchId a_switch);
}
