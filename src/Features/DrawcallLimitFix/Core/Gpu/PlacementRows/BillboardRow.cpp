// BillboardRow: the billboard row (SampleBillboardRow, FrameValues' producer inside the engine-read window) and the face mode
// (BillboardFaceMode, the scene capture's). The CPU port of the engine's turn is the feature's (Features/Billboards, Orient: for its parity).

#include "Features/DrawcallLimitFix/Core/Gpu/PlacementRows/BillboardRow.h"

#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Shading/LightingConstants.h"

#include <cmath>

namespace DCLF::PlacementRows
{
	std::uint32_t BillboardFaceMode(const void* a_node)
	{
		return Engine::At<std::uint16_t>(a_node, 0x128) & 7u;
	}

	bool SampleBillboardRow(const RE::BSGeometry& a_geometry, BindlessPlacement& a_out)
	{
		static const REL::Relocation<const RE::NiRTTI*> effectRtti{ RE::BSEffectShaderProperty::Ni_RTTI };
		const auto* property = a_geometry.GetGeometryRuntimeData().shaderProperty.get();
		if (!property || property->GetRTTI() != effectRtti.get())
			return false;
		// The leaf relative to the nearest billboard node: the locals between them, composed as the children's world updates compose them
		// (NiAVObject's world = the parent's world * the local).
		RE::NiTransform relative = a_geometry.local;
		const RE::NiNode* node = a_geometry.parent;
		while (node && !netimmerse_cast<RE::NiBillboardNode*>(const_cast<RE::NiNode*>(node))) {
			relative = node->local * relative;
			node = node->parent;
		}
		if (!node)
			return false;
		// The node's base, as 0x140d3e650 rebuilds it before turning it: its parent's world times its local (the identity without a parent).
		const RE::NiTransform base = node->parent ? node->parent->world * node->local : node->local;
		// The unturned world (the node's turn the identity): a world transform like any row's, so a pass that does not turn the leaf still
		// draws it where the node stands.
		const RE::NiTransform unturned = base * relative;
		const auto& r = unturned.rotate.entry;
		const float scale = unturned.scale;
		const float t[3]{ unturned.translate.x, unturned.translate.y, unturned.translate.z };
		for (int row = 0; row < 3; ++row) {
			a_out.world[row * 4 + 0] = r[row][0] * scale;
			a_out.world[row * 4 + 1] = r[row][1] * scale;
			a_out.world[row * 4 + 2] = r[row][2] * scale;
			a_out.world[row * 4 + 3] = t[row];
			a_out.previousWorld[row * 4 + 0] = base.rotate.entry[row][0];
			a_out.previousWorld[row * 4 + 1] = base.rotate.entry[row][1];
			a_out.previousWorld[row * 4 + 2] = base.rotate.entry[row][2];
			a_out.previousWorld[row * 4 + 3] = 0.0f;
		}
		// The leaf's bound: its model bound by the unturned world (its centre's distance from the pivot does not change as the node turns).
		const auto& model = a_geometry.GetModelData().modelBound;
		float offset = 0.0f;
		for (int row = 0; row < 3; ++row) {
			const float c = (r[row][0] * model.center.x + r[row][1] * model.center.y + r[row][2] * model.center.z) * scale + t[row] -
			                (row == 0 ? base.translate.x : row == 1 ? base.translate.y : base.translate.z);
			offset += c * c;
		}
		const float radius = model.radius * scale;
		a_out.previousWorld[3] = radius;
		a_out.bound[0] = base.translate.x;
		a_out.bound[1] = base.translate.y;
		a_out.bound[2] = base.translate.z;
		a_out.bound[3] = std::sqrt(offset) + radius;
		return true;
	}
}
