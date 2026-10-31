#pragma once

#include <cstdint>

namespace RE
{
	class BSGeometry;
}

namespace DCLF
{
	struct BindlessPlacement;

	/**
	 * @brief Billboards (CS_DCLF_BILLBOARDS; dclf-architecture.md, "Billboards and aligned particles"; engine notes, "Billboards"):
	 * BSEffectShaderProperty leaves under an NiBillboardNode, which DCLF draws turned to each view's camera on the GPU.
	 *
	 * The engine turns a billboard in its culls, not in its update: NiBillboardNode::OnVisible (0x140d3f720), when the culling process
	 * has cameraRelatedUpdates, rebuilds the node's world from its parent's world and its local (rotation R0 = parent * local, the
	 * translation and scale as any node's), multiplies a rotation M its face mode makes from the culling camera into it (0x140d3e650:
	 * ALWAYS_FACE_CAMERA, ROTATE_ABOUT_UP, RIGID_FACE_CAMERA, ALWAYS_FACE_CENTER, RIGID_FACE_CENTER, BSROTATE_ABOUT_UP) and runs its
	 * children's UpdateDownwardPass, so every leaf below holds the world of whichever camera culled it last. NiBillboardNode::
	 * UpdateWorldData (0x140d3e510) writes the translation and scale alone. DCLF reads none of that: a billboard leaf's placement row
	 * holds what no cull writes - the node's base (R0 and its translation, the pivot) and the leaf's unturned world (the base times the
	 * locals between the node and the leaf) - and the vertex stage builds M from the drawing view's camera (VS_PerFrame b12:
	 * CameraView's rows and the eye), so one row serves every view (Effect.hlsl, DCLFBillboardWorld; the mode is the key's,
	 * kRasterEffectBillboardShift, and the record's, the Effect family's billboard bits).
	 *
	 * The row (SampleBillboardRow): World = the unturned world (a world transform, so a pass that does not turn the leaf draws it where the node
	 * stands; the vertex stage turns it about the pivot by (R0 * M) * R0^T); PreviousWorld's rotation part = R0, its w column the leaf's world bound radius (Effect.hlsl's
	 * EffectRadius) and two zeros; Bound = the pivot, and a radius covering the leaf in every orientation (the distance of its bound's
	 * centre from the pivot plus its radius): the GPU cull's sphere, and the forward sort's centre.
	 *
	 * The row and the mode are Core's, not generalised into a feature contribution: an item has one class (a particle under an
	 * NiBillboardNode gets both treatments, and a class would change its rows and make them due every frame); three parities need the
	 * row per slot (FrameValues::SampleSlot); FrameValues' producer samples it for every moving object whatever the toggle; and its
	 * layout is the frozen decode of Effect.hlsl's DCLFBillboardWorld. The rest is the feature's (Features/Billboards: the switches and
	 * toggle, its parent contributions, the CPU twin of the turn and the parity comparing it with the engine's).
	 */
	namespace PlacementRows
	{
		/**
		 * @brief An NiBillboardNode's face mode: userFlags (+0x128) & 7, as 0x140d3e650 reads it. Read at the offset, not through
		 * CommonLib's member, whose offset in this build is not AE's (it reads 0 for ROTATE_ABOUT_UP nodes).
		 */
		std::uint32_t BillboardFaceMode(const void* a_node);

		/**
		 * @brief a_geometry's billboard row (above) into a_out's world, previousWorld and bound, when it is a BSEffectShaderProperty leaf
		 * with an NiBillboardNode above it (the nearest); false, and a_out untouched, otherwise. Inside the engine-read window (the parents'
		 * worlds and the locals are the update's; nothing a cull writes is read).
		 */
		bool SampleBillboardRow(const RE::BSGeometry& a_geometry, BindlessPlacement& a_out);
	}
}
