// The decal nodes' arrays: the engine's edits of a BGSDecalNode's decal array, hooked at the feature's early hooks (where Core's scene
// events installed them, which share no site with them), each pushing the node's update on the editing thread; and the node capture
// contribution that records the array with the node (and again on each update), on the capturing thread inside the engine-read window.

#include "Features/DrawcallLimitFix/Features/Decals/Decals.h"

#include "Features/DrawcallLimitFix/Core/Engine/Capture/SceneWriter.h"
#include "Features/DrawcallLimitFix/Core/Extensions/NodeCaptures.h"

namespace DCLF::Features::Decals
{
	namespace
	{
		// A BGSDecalNode's decal array edits (SWF_HOOK_DECAL_ARRAY_EDIT, AE 1.6.1170): append (FUN_1401fdfb0, from
		// BSTempEffectGeometryDecal::Attach and simple decals' attach); erases: a decal's or reference's decals (FUN_1401fdc80 via
		// FUN_1401fe020, from the decal manager), the geometry decal's update (FUN_1401fdcb0), erase-and-detach (FUN_1401fdd50,
		// FUN_1401fde40). The node is the first argument; its decals after the call.
		void OnDecalArrayEdit(const SWF_EventHeader* a_event, std::uintptr_t*, void*)
		{
			Extensions::PushNodeUpdate(SceneWriter::Arg<RE::NiAVObject*>(a_event, 0), Api::kNodeFieldCaptured);
		}

		// A decal node's decals' 3D (BSTempEffect::Get3D, null for none) in array order; OnVisible culls them last to first, not its
		// children.
		void CaptureDecals(const void* a_node, std::vector<const void*>& a_out)
		{
			for (const auto& decal : static_cast<const RE::BGSDecalNode*>(a_node)->GetRuntimeData().decals)
				a_out.push_back(decal ? decal->Get3D() : nullptr);
		}
	}

	const void* DecalNodeRtti()
	{
		static const REL::Relocation<const RE::NiRTTI*> decalNode{ RE::BGSDecalNode::Ni_RTTI };
		return decalNode.get();
	}

	void InstallDecalHooks()
	{
		// A decal node's array edits: SceneWriterFramework's decal calls, which DrawcallLimitFix::PostPostLoad required.
		if (!SceneWriter::SubscribeCall(SWF_HOOK_DECAL_ARRAY_EDIT, &OnDecalArrayEdit, SWF_PHASE_POST))
			stl::report_and_fail("Drawcall Limit Fix: the scene writer framework refused the decal array edits after reporting them installed");
	}

	void RegisterDecalCapture(Api::InstallContext& a_context)
	{
		a_context.RegisterNodeCapture({ .name = "decal node", .rtti = &DecalNodeRtti, .capture = &CaptureDecals });
	}
}
