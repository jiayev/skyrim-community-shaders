#include "LightingConstants.h"

#include <bit>

#include <cstring>

namespace DCLF
{
	namespace
	{
		void StoreRelative(float* a_out, const float (&a_world)[12], const RE::NiPoint3& a_posAdjust)
		{
			std::memcpy(a_out, a_world, sizeof(a_world));
			a_out[3] = a_world[3] - a_posAdjust.x;
			a_out[7] = a_world[7] - a_posAdjust.y;
			a_out[11] = a_world[11] - a_posAdjust.z;
		}

		// A variable's dword offset in the shader's cbuffer, or ~0 when the permutation lacks it (offset 0
		// is also what reflection leaves for missing variables, so it only counts for the group's first).
		std::uint32_t OffsetOf(std::span<const std::uint8_t> a_table, std::uint32_t a_variable, std::uint32_t a_firstVariable)
		{
			if (a_variable >= a_table.size())
				return ~0u;
			const std::uint32_t offset = a_table[a_variable];
			return (offset == 0 && a_variable != a_firstVariable) ? ~0u : offset;
		}
	}

	bool SameBindlessGeometry(const GeometryConstants& a_a, const GeometryConstants& a_b)
	{
		for (std::uint32_t stage = 0; stage < 2; ++stage) {
			const auto& layout = stage ? LightingPSLayout() : LightingVSLayout();
			const std::uint64_t read = (stage ? kPSGroups[kPerGeometry] & ~kPSBindlessGeometryUnread : kVSGroups[kPerGeometry] & ~kVSBindlessGeometryUnread);
			const auto& a = stage ? a_a.ps : a_a.vs;
			const auto& b = stage ? a_b.ps : a_b.vs;
			for (std::uint32_t v = 0; v < layout.count; ++v)
				if (((read >> v) & 1) && std::memcmp(&a.floats[layout.offset[v]], &b.floats[layout.offset[v]], layout.size[v] * sizeof(float)) != 0)
					return false;
		}
		return true;
	}

	namespace
	{
		/** @brief Each frame lighting component: its float in a PerGeometry PS block and its index in FrameLighting. */
		template <class F>
		void ForEachFrameLightingFloat(F&& a_f)
		{
			const auto& layout = LightingPSLayout();
			std::uint32_t row = 0;
			for (std::uint32_t i = 0; i < 4; ++i) {
				const std::uint32_t v = kPSFrameGeometry[i];
				for (std::uint32_t c = 0; c < layout.size[v] && row * 4 + c < kFrameLightingRows * 4; ++c)
					a_f(v, c, layout.offset[v] + c, row * 4 + c);
				row += (layout.size[v] + 3) / 4;
			}
		}
	}

	void MergeFrameLighting(const ConstantBlock& a_ps, FrameLighting& a_out, std::uint32_t& a_written)
	{
		static_assert(kFrameLightingRows * 4 <= 32);
		ForEachFrameLightingFloat([&](std::uint32_t, std::uint32_t, std::uint32_t a_float, std::uint32_t a_index) {
			if (!((a_written >> a_index) & 1) && a_ps.Written(a_float)) {
				a_out[a_index] = a_ps.floats[a_float];
				a_written |= 1u << a_index;
			}
		});
	}

	bool MatchesFrameLighting(const ConstantBlock& a_ps, const FrameLighting& a_lighting, std::string* a_first)
	{
		bool match = true;
		ForEachFrameLightingFloat([&](std::uint32_t a_variable, std::uint32_t a_component, std::uint32_t a_float, std::uint32_t a_index) {
			if (!match || !a_ps.Written(a_float) || std::bit_cast<std::uint32_t>(a_ps.floats[a_float]) == std::bit_cast<std::uint32_t>(a_lighting[a_index]))
				return;
			match = false;
			if (a_first)
				*a_first = fmt::format("PS{}[{}]: {} against the frame's {}", a_variable, a_component, a_ps.floats[a_float], a_lighting[a_index]);
		});
		return match;
	}

	const float* ExtraRowsOf(const SceneStore::Tables& a_tables, std::uint32_t a_objectIndex)
	{
		if (a_objectIndex >= a_tables.extraOffset.size())
			return nullptr;
		const std::uint32_t offset = a_tables.extraOffset[a_objectIndex];
		if (offset == kNoExtraRows || std::size_t(offset + kExtraRows) * 4 > a_tables.extraRows.size())
			return nullptr;
		return &a_tables.extraRows[std::size_t(offset) * 4];
	}

	GeometryConstants ObjectGeometryConstants(const SceneStore::Tables& a_tables, std::uint32_t a_objectIndex, std::uint32_t a_renderFlags, const RE::NiPoint3& a_eye,
		const RE::NiPoint3& a_previousEye)
	{
		const auto& object = a_tables.objects[a_objectIndex];
		GeometryConstants constants = a_tables.geometryConstants[object.pipelineIndex];
		const auto& vsLayout = LightingVSLayout();
		const auto& psLayout = LightingPSLayout();
		StoreRelative(&constants.vs.floats[vsLayout.offset[kVSWorld]], object.world, a_eye);
		// Render flag 0x10: the previous transform is the current one (engine notes: SetupGeometry).
		StoreRelative(&constants.vs.floats[vsLayout.offset[kVSPreviousWorld]], (a_renderFlags & 0x10) ? object.world : object.previousWorld, a_previousEye);
		const auto& shading = a_tables.shading[a_objectIndex];
		std::memcpy(&constants.ps.floats[psLayout.offset[kPSMaterialData]], shading.materialData, sizeof(shading.materialData));
		std::memcpy(&constants.ps.floats[psLayout.offset[kPSEmitColor]], shading.emitColor, sizeof(shading.emitColor));
		constants.ps.floats[psLayout.offset[kPSSSRParams] + 3] = shading.ssrSpecular;
		if ((object.flags & kObjectTreeAnim) && a_objectIndex < a_tables.treeAnim.size()) {
			const auto& tree = a_tables.treeAnim[a_objectIndex];
			std::memcpy(&constants.vs.floats[vsLayout.offset[kVSTreeParams]], tree.treeParams, sizeof(tree.treeParams));
			std::memcpy(&constants.vs.floats[vsLayout.offset[kVSWindTimers]], tree.windTimers, 2 * sizeof(float));
		}
		// The extras rows (SceneStore::RefreshFrameConstants fills them). ProjectedUVParams.y is never
		// written by the engine, so it keeps whatever the template left - the unwritten sentinel.
		if (const float* rows = ExtraRowsOf(a_tables, a_objectIndex)) {
			if (object.flags & kObjectLandBlend)
				std::memcpy(&constants.vs.floats[vsLayout.offset[kVSLandBlendParams]], rows + kExtraRowLandBlend * 4, 4 * sizeof(float));
			if (object.flags & kObjectProjectedUV) {
				std::memcpy(&constants.vs.floats[vsLayout.offset[kVSTextureProj]], rows + kExtraRowTextureProj * 4, 12 * sizeof(float));
				const float* params = rows + kExtraRowProjectedParams * 4;
				float* out = &constants.ps.floats[psLayout.offset[kPSProjectedUVParams]];
				out[0] = params[0];
				out[2] = params[2];
				out[3] = params[3];
				std::memcpy(&constants.ps.floats[psLayout.offset[kPSProjectedUVParams + 1]], params + 4, 4 * sizeof(float));
				std::memcpy(&constants.ps.floats[psLayout.offset[kPSProjectedUVParams + 2]], params + 8, 4 * sizeof(float));
			}
		}
		return constants;
	}

	GeometryPatchOffsets GeometryPatchOffsetsOf(std::span<const std::uint8_t> a_vsTable, std::span<const std::uint8_t> a_psTable)
	{
		const auto& vsLayout = LightingVSLayout();
		const auto& psLayout = LightingPSLayout();
		GeometryPatchOffsets offsets;
		offsets.vsWorld = OffsetOf(a_vsTable, kVSWorld, kVSFirstVariable[kPerGeometry]);
		offsets.vsWorldSize = vsLayout.size[kVSWorld];
		offsets.vsPreviousWorld = OffsetOf(a_vsTable, kVSPreviousWorld, kVSFirstVariable[kPerGeometry]);
		offsets.vsPreviousWorldSize = vsLayout.size[kVSPreviousWorld];
		offsets.psMaterialData = OffsetOf(a_psTable, kPSMaterialData, kPSFirstVariable[kPerGeometry]);
		offsets.psMaterialDataSize = psLayout.size[kPSMaterialData];
		offsets.psEmitColor = OffsetOf(a_psTable, kPSEmitColor, kPSFirstVariable[kPerGeometry]);
		offsets.psEmitColorSize = psLayout.size[kPSEmitColor];
		offsets.psSSRParams = OffsetOf(a_psTable, kPSSSRParams, kPSFirstVariable[kPerGeometry]);
		offsets.psSSRParamsSize = psLayout.size[kPSSSRParams];
		offsets.vsTreeParams = OffsetOf(a_vsTable, kVSTreeParams, kVSFirstVariable[kPerGeometry]);
		offsets.vsTreeParamsSize = vsLayout.size[kVSTreeParams];
		offsets.vsWindTimers = OffsetOf(a_vsTable, kVSWindTimers, kVSFirstVariable[kPerGeometry]);
		offsets.vsWindTimersSize = vsLayout.size[kVSWindTimers];
		offsets.vsLandBlendParams = OffsetOf(a_vsTable, kVSLandBlendParams, kVSFirstVariable[kPerGeometry]);
		offsets.vsLandBlendParamsSize = vsLayout.size[kVSLandBlendParams];
		offsets.vsTextureProj = OffsetOf(a_vsTable, kVSTextureProj, kVSFirstVariable[kPerGeometry]);
		offsets.vsTextureProjSize = vsLayout.size[kVSTextureProj];
		for (std::uint32_t i = 0; i < 3; ++i) {
			offsets.psProjectedUVParams[i] = OffsetOf(a_psTable, kPSProjectedUVParams + i, kPSFirstVariable[kPerGeometry]);
			offsets.psProjectedUVParamsSize[i] = psLayout.size[kPSProjectedUVParams + i];
		}
		return offsets;
	}

	void PatchObjectGeometry(const SceneStore::Tables& a_tables, std::uint32_t a_objectIndex, std::uint32_t a_renderFlags,
		const RE::NiPoint3& a_eye, const RE::NiPoint3& a_previousEye, const GeometryPatchOffsets& a_offsets,
		std::span<std::byte> a_vsOut, std::span<std::byte> a_psOut)
	{
		const auto& object = a_tables.objects[a_objectIndex];
		const auto& shading = a_tables.shading[a_objectIndex];

		// Writes a_count floats at a float offset, clipped to the group. Unwritten components (the quiet
		// NaN ConstantBlock uses as its sentinel) become zero, matching PackConstantGroup.
		auto write = [](std::span<std::byte> a_out, std::uint32_t a_offset, std::uint32_t a_count, const float* a_values, std::uint32_t a_available) {
			if (a_offset == ~0u)
				return;
			for (std::uint32_t c = 0; c < a_count; ++c) {
				const std::size_t at = (std::size_t(a_offset) + c) * 4;
				if (at + 4 > a_out.size())
					return;
				float value = c < a_available ? a_values[c] : 0.0f;
				if (std::bit_cast<std::uint32_t>(value) == kUnwrittenBits)
					value = 0.0f;
				std::memcpy(a_out.data() + at, &value, 4);
			}
		};

		float world[16] = {};
		StoreRelative(world, object.world, a_eye);
		write(a_vsOut, a_offsets.vsWorld, a_offsets.vsWorldSize, world, 12);
		// Render flag 0x10: the previous transform is the current one (engine notes: SetupGeometry).
		StoreRelative(world, (a_renderFlags & 0x10) ? object.world : object.previousWorld, a_previousEye);
		write(a_vsOut, a_offsets.vsPreviousWorld, a_offsets.vsPreviousWorldSize, world, 12);

		write(a_psOut, a_offsets.psMaterialData, a_offsets.psMaterialDataSize, shading.materialData,
			static_cast<std::uint32_t>(std::size(shading.materialData)));
		write(a_psOut, a_offsets.psEmitColor, a_offsets.psEmitColorSize, shading.emitColor,
			static_cast<std::uint32_t>(std::size(shading.emitColor)));
		if ((object.flags & kObjectTreeAnim) && a_objectIndex < a_tables.treeAnim.size()) {
			const auto& tree = a_tables.treeAnim[a_objectIndex];
			write(a_vsOut, a_offsets.vsTreeParams, a_offsets.vsTreeParamsSize, tree.treeParams, 4);
			write(a_vsOut, a_offsets.vsWindTimers, a_offsets.vsWindTimersSize, tree.windTimers, 2);
		}
		if (const float* rows = ExtraRowsOf(a_tables, a_objectIndex)) {
			if (object.flags & kObjectLandBlend)
				write(a_vsOut, a_offsets.vsLandBlendParams, a_offsets.vsLandBlendParamsSize, rows + kExtraRowLandBlend * 4, 4);
			if (object.flags & kObjectProjectedUV) {
				write(a_vsOut, a_offsets.vsTextureProj, a_offsets.vsTextureProjSize, rows + kExtraRowTextureProj * 4, 12);
				const float* params = rows + kExtraRowProjectedParams * 4;
				// x, then z and w: the engine never writes y (the template's zero stays).
				if (a_offsets.psProjectedUVParams[0] != ~0u) {
					write(a_psOut, a_offsets.psProjectedUVParams[0], 1, params, 1);
					if (a_offsets.psProjectedUVParamsSize[0] >= 4)
						write(a_psOut, a_offsets.psProjectedUVParams[0] + 2, 2, params + 2, 2);
				}
				write(a_psOut, a_offsets.psProjectedUVParams[1], a_offsets.psProjectedUVParamsSize[1], params + 4, 4);
				write(a_psOut, a_offsets.psProjectedUVParams[2], a_offsets.psProjectedUVParamsSize[2], params + 8, 4);
			}
		}
		// Only the w component of SSRParams is per-object; the rest stays as the template packed it.
		if (a_offsets.psSSRParams != ~0u && a_offsets.psSSRParamsSize > 3) {
			const std::size_t at = (std::size_t(a_offsets.psSSRParams) + 3) * 4;
			if (at + 4 <= a_psOut.size()) {
				float value = shading.ssrSpecular;
				if (std::bit_cast<std::uint32_t>(value) == kUnwrittenBits)
					value = 0.0f;
				std::memcpy(a_psOut.data() + at, &value, 4);
			}
		}
	}

	void StoreRelativeTo(float* a_out, const float (&a_world)[12], const RE::NiPoint3& a_eye)
	{
		StoreRelative(a_out, a_world, a_eye);
	}

	void BuildObjectRecord(const SceneStore::Tables& a_tables, std::uint32_t a_objectIndex, std::uint32_t a_renderFlags, BindlessObject& a_out)
	{
		const auto& object = a_tables.objects[a_objectIndex];

		std::memcpy(a_out.world, object.world, sizeof(a_out.world));
		// Render flag 0x10: the previous transform is the current one (engine notes: SetupGeometry).
		std::memcpy(a_out.previousWorld, (a_renderFlags & 0x10) ? object.world : object.previousWorld, sizeof(a_out.previousWorld));

		// The shading half is ObjectShading's own layout, except that an unwritten component packs as zero
		// the way PackConstantGroup and PatchObjectGeometry make it. The sweep stops at ObjectShading on
		// purpose: the tail below carries no unwritten sentinel, and 0 is a legal value in all four of its
		// fields.
		a_out.shading = a_tables.shading[a_objectIndex];
		auto* const floats = reinterpret_cast<float*>(&a_out.shading);
		for (std::size_t i = 0; i < sizeof(ObjectShading) / sizeof(float); ++i) {
			if (std::bit_cast<std::uint32_t>(floats[i]) == kUnwrittenBits)
				floats[i] = 0.0f;
		}

		const auto& lights = a_tables.lights[a_objectIndex];
		a_out.roomIndex = lights.roomIndex;
		a_out.shadowBitMask = lights.shadowBitMask;
		// The same reference the native draw's AlphaTestRefBuffer carries: threshold / 255, and 0 when the
		// object is not alpha tested (the shader's test is then compiled out anyway).
		a_out.alphaTestRef = (object.flags & kObjectAlphaTest) ? ((object.flags >> kObjectAlphaThresholdShift) & 0xFF) / 255.0f : 0.0f;
		a_out.emissiveMult = a_objectIndex < a_tables.emissiveMult.size() ? a_tables.emissiveMult[a_objectIndex] : 1.0f;
		a_out.tree = a_objectIndex < a_tables.treeAnim.size() ? a_tables.treeAnim[a_objectIndex] : ObjectTreeAnim{};
		// Skinning: the offsets are into the frame's bone tables, which the epoch packs into its bones
		// buffer in the same order (current rows first, then the previous rows after all of them).
		const bool skinned = (object.flags & kObjectSkinned) && a_objectIndex < a_tables.boneOffset.size();
		a_out.boneOffset = skinned ? a_tables.boneOffset[a_objectIndex] : 0u;
		a_out.boneRows = skinned ? a_tables.boneRows[a_objectIndex] : 0u;
		a_out.previousBoneOffset = skinned ? a_tables.boneOffset[a_objectIndex] + static_cast<std::uint32_t>(a_tables.bones.size() / 4) : 0u;
		// The extras rows follow every palette (current then previous) in the row buffer.
		const bool extras = a_objectIndex < a_tables.extraOffset.size() && a_tables.extraOffset[a_objectIndex] != kNoExtraRows;
		a_out.extraOffset = extras ? static_cast<std::uint32_t>(a_tables.bones.size() / 4) * 2 + a_tables.extraOffset[a_objectIndex] : 0u;
		const auto wetness = a_objectIndex < a_tables.skinWetness.size() ? a_tables.skinWetness[a_objectIndex] : std::array<float, 4>{};
		std::memcpy(a_out.skinPerGeometry, wetness.data(), sizeof(a_out.skinPerGeometry));
	}

	std::uint32_t PackedPositionOf(const StageLayout& a_layout, std::span<const std::uint8_t> a_table, std::uint64_t a_variables, std::uint32_t a_firstVariable,
		std::uint32_t a_float)
	{
		for (std::uint32_t i = 0; i < a_layout.count; ++i) {
			const std::uint32_t first = a_layout.offset[i];
			const std::uint32_t end = first + a_layout.size[i];
			if (!((a_variables >> i) & 1) || a_float < first || a_float >= end)
				continue;
			const auto offset = OffsetOf(a_table, i, a_firstVariable);
			return offset == ~0u ? ~0u : offset + (a_float - first);
		}
		return ~0u;
	}

	std::size_t ConstantGroupSize(const StageLayout& a_layout, std::span<const std::uint8_t> a_table, std::uint64_t a_variables, std::uint32_t a_firstVariable)
	{
		std::size_t end = 0;
		for (std::uint32_t i = 0; i < a_layout.count; ++i) {
			if (!((a_variables >> i) & 1))
				continue;
			if (const auto offset = OffsetOf(a_table, i, a_firstVariable); offset != ~0u)
				end = std::max<std::size_t>(end, (offset + a_layout.size[i]) * 4);
		}
		return (end + 15) & ~std::size_t(15);
	}

	std::size_t PackConstantGroup(const ConstantBlock& a_block, const StageLayout& a_layout, std::span<const std::uint8_t> a_table, std::uint64_t a_variables,
		std::uint32_t a_firstVariable, std::span<std::byte> a_out)
	{
		const std::size_t size = ConstantGroupSize(a_layout, a_table, a_variables, a_firstVariable);
		if (size > a_out.size())
			return 0;
		std::memset(a_out.data(), 0, size);
		for (std::uint32_t i = 0; i < a_layout.count; ++i) {
			if (!((a_variables >> i) & 1))
				continue;
			const auto offset = OffsetOf(a_table, i, a_firstVariable);
			if (offset == ~0u)
				continue;
			for (std::uint32_t c = 0; c < a_layout.size[i]; ++c) {
				if (a_block.Written(a_layout.offset[i] + c))
					std::memcpy(a_out.data() + (offset + c) * 4, &a_block.floats[a_layout.offset[i] + c], 4);
			}
		}
		return size;
	}
}
