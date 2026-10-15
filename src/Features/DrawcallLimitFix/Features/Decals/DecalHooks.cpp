// The decal nodes' arrays: the engine's edits of a BGSDecalNode's decal array, hooked at the feature's early hooks (where Core's scene
// events installed them, which share no site with them), each pushing the node's update on the editing thread; and the node capture
// contribution that records the array with the node (and again on each update), on the capturing thread inside the engine-read window.

#include "Features/DrawcallLimitFix/Features/Decals/Decals.h"

#include "Features/DrawcallLimitFix/Core/Extensions/NodeCaptures.h"

namespace DCLF::Features::Decals
{
	namespace
	{
		// A BGSDecalNode's decal array edits (AE 1.6.1170): append (FUN_1401fdfb0, from BSTempEffectGeometryDecal::Attach and simple
		// decals' attach); erases: a decal's or reference's decals (FUN_1401fdc80 via FUN_1401fe020, from the decal manager), the
		// geometry decal's update (FUN_1401fdcb0), erase-and-detach (FUN_1401fdd50, FUN_1401fde40). Node first, other arguments passed
		// through; the node's decals after the call.
		template <int N>
		struct DecalArrayEdit
		{
			static void thunk(RE::NiAVObject* a_node, void* a_1, void* a_2, void* a_3)
			{
				func(a_node, a_1, a_2, a_3);
				Extensions::PushNodeUpdate(a_node, Api::kNodeFieldCaptured);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

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
		// A decal node's array edits: DecalArrayEdit.
		stl::detour_thunk<DecalArrayEdit<0>>(REL::Offset(0x1fdfb0).address());
		stl::detour_thunk<DecalArrayEdit<1>>(REL::Offset(0x1fdc80).address());
		stl::detour_thunk<DecalArrayEdit<2>>(REL::Offset(0x1fdcb0).address());
		stl::detour_thunk<DecalArrayEdit<3>>(REL::Offset(0x1fdd50).address());
		stl::detour_thunk<DecalArrayEdit<4>>(REL::Offset(0x1fde40).address());
	}

	void RegisterDecalCapture(Api::InstallContext& a_context)
	{
		a_context.RegisterNodeCapture({ .name = "decal node", .rtti = &DecalNodeRtti, .capture = &CaptureDecals });
	}
}
