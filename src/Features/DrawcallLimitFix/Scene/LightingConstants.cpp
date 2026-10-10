#include "LightingConstants.h"
#include "Features/DrawcallLimitFix/Engine/EngineReadWindow.h"

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

	std::string BindlessGeometryDifferences(const GeometryConstants& a_a, const GeometryConstants& a_b)
	{
		std::string out;
		for (std::uint32_t stage = 0; stage < 2; ++stage) {
			const auto& layout = stage ? LightingPSLayout() : LightingVSLayout();
			const std::uint64_t read = (stage ? kPSGroups[kPerGeometry] & ~kPSBindlessGeometryUnread : kVSGroups[kPerGeometry] & ~kVSBindlessGeometryUnread);
			const auto& a = stage ? a_a.ps : a_a.vs;
			const auto& b = stage ? a_b.ps : a_b.vs;
			for (std::uint32_t v = 0; v < layout.count; ++v)
				if (((read >> v) & 1) && std::memcmp(&a.floats[layout.offset[v]], &b.floats[layout.offset[v]], layout.size[v] * sizeof(float)) != 0) {
					std::string values;
					for (std::uint32_t c = 0; c < std::min<std::uint32_t>(layout.size[v], 4); ++c)
						values += fmt::format("{}{:#x}/{:#x}", c ? " " : "", std::bit_cast<std::uint32_t>(a.floats[layout.offset[v] + c]),
							std::bit_cast<std::uint32_t>(b.floats[layout.offset[v] + c]));
					out += fmt::format("{}{}{} [{}]", out.empty() ? "" : ", ", stage ? "PS" : "VS", v, values);
				}
		}
		return out;
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

	namespace
	{
		/** @brief Each fog float: its stage, its block's float and its FrameFog index (FogColor: none, ~0u). */
		template <class F>
		void ForEachFogFloat(F&& a_visit)
		{
			const auto& vs = LightingVSLayout();
			for (std::uint32_t row = 0; row < kFrameFogRows; ++row)
				for (std::uint32_t c = 0; c < 4 && c < vs.size[kVSFrameFog[row]]; ++c)
					a_visit(false, vs.offset[kVSFrameFog[row]] + c, row * 4 + c);
			const auto& ps = LightingPSLayout();
			for (std::uint32_t c = 0; c < 4 && c < ps.size[kPSFogColor]; ++c)
				a_visit(true, ps.offset[kPSFogColor] + c, ~0u);
		}
	}

	void MergeFrameFog(const TechniqueConstants& a_technique, FrameFog& a_out, std::uint32_t& a_written)
	{
		static_assert(std::tuple_size_v<FrameFog> <= 32);
		ForEachFogFloat([&](bool a_pixel, std::uint32_t a_float, std::uint32_t a_index) {
			if (a_pixel)
				return;
			if (!((a_written >> a_index) & 1) && a_technique.vs.Written(a_float)) {
				a_out[a_index] = a_technique.vs.floats[a_float];
				a_written |= 1u << a_index;
			}
		});
	}

	void KeepTechniqueFog(const TechniqueConstants& a_from, TechniqueConstants& a_to)
	{
		ForEachFogFloat([&](bool a_pixel, std::uint32_t a_float, std::uint32_t) {
			(a_pixel ? a_to.ps : a_to.vs).floats[a_float] = (a_pixel ? a_from.ps : a_from.vs).floats[a_float];
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

	ExtrasFrame SampleExtrasFrame()
	{
		EngineReadWindow::Touch("SampleExtrasFrame");
		ExtrasFrame frame;
		auto at = [](std::uintptr_t a_offset) { return *reinterpret_cast<const float*>(REL::Offset(a_offset).address()); };
		// BSLightingShader::SetupGeometry, techniques 8 and 19 (engine notes: per-object constants): a blend between two
		// BSShaderManager::State positions by a clock the same state holds.
		float t = (at(0x2033080) - at(0x2033118)) * (at(0x1ad2840) / at(0x20330f8));
		t = t <= 0.0f ? 0.0f : 1.0f <= t ? 1.0f : t;
		frame.landBlend[0] = (at(0x2033110) - at(0x2033108)) * t + at(0x2033108);
		frame.landBlend[1] = (at(0x2033114) - at(0x203310c)) * t + at(0x203310c);
		// The ProjectedUV projection (SetupGeometry's ProjectedUV block): a fixed rotation about Z at posAdjust, through the engine's
		// own NiTransform-to-matrix routine (which subtracts posAdjust).
		using ToMatrix = void (*)(float*, const RE::NiTransform*);
		static const REL::Relocation<ToMatrix> toMatrix{ REL::Offset(0x14aaf10) };
		RE::NiTransform projection;
		projection.rotate.entry[0][0] = 0.0f;
		projection.rotate.entry[0][1] = 1.0f;
		projection.rotate.entry[0][2] = 0.0f;
		projection.rotate.entry[1][0] = -1.0f;
		projection.rotate.entry[1][1] = 0.0f;
		projection.rotate.entry[1][2] = 0.0f;
		projection.rotate.entry[2][0] = 0.0f;
		projection.rotate.entry[2][1] = 0.0f;
		projection.rotate.entry[2][2] = 1.0f;
		projection.translate = globals::game::shadowState->GetRuntimeData().posAdjust.getEye();
		projection.scale = 1.0f;
		toMatrix(frame.projection, &projection);
		// The pixel parameters' globals (FUN_1414e00c0): the two tilings and the projected-normals switch.
		frame.projectedGlobals[0] = at(0x2035560);
		frame.projectedGlobals[1] = at(0x2035578);
		frame.projectedGlobals[2] = 0.0f;
		frame.projectedGlobals[3] = *reinterpret_cast<const std::uint8_t*>(REL::Offset(0x2035518).address()) ? 1.0f : 0.0f;
		return frame;
	}

	void CompleteExtras(const float* a_static, std::uint32_t a_objectFlags, const float (&a_world)[12], const ExtrasFrame& a_frame, float* a_out)
	{
		std::memcpy(a_out, a_static, sizeof(float) * kExtraRows * 4);
		if (a_objectFlags & kObjectLandBlend) {
			float* land = a_out + kExtraRowLandBlend * 4;
			land[2] = a_frame.landBlend[0] - a_world[3];
			land[3] = a_frame.landBlend[1] - a_world[7];
		}
		if (!(a_objectFlags & kObjectProjectedUV))
			return;
		const float mode = a_static[kExtraRowLandBlend * 4];
		float* proj = a_out + kExtraRowTextureProj * 4;
		if (mode != kTextureProjShape) {
			// m = W x P, W the placement as a row-vector matrix (DCLFTextureProjOf); TextureProj's rows are m's columns.
			const float* p = a_frame.projection;
			float w[16]{};
			for (int i = 0; i < 3; ++i) {
				for (int k = 0; k < 3; ++k)
					w[i * 4 + k] = a_world[k * 4 + i];
				w[12 + i] = a_world[i * 4 + 3];
			}
			w[15] = 1.0f;
			float m[16];
			if (mode == kTextureProjProjection) {
				std::memcpy(m, p, sizeof(m));
			} else {
				for (int i = 0; i < 4; ++i)
					for (int j = 0; j < 4; ++j)
						m[i * 4 + j] = w[i * 4 + 0] * p[0 * 4 + j] + w[i * 4 + 1] * p[1 * 4 + j] + w[i * 4 + 2] * p[2 * 4 + j] + w[i * 4 + 3] * p[3 * 4 + j];
			}
			for (int r = 0; r < 3; ++r)
				for (int i = 0; i < 4; ++i)
					proj[r * 4 + i] = m[i * 4 + r];
		}
		std::memcpy(a_out + (kExtraRowProjectedParams + 2) * 4, a_frame.projectedGlobals, sizeof(a_frame.projectedGlobals));
	}

	GeometryConstants ObjectGeometryConstants(const SceneStore::Tables& a_tables, const FrameTables& a_frame, std::uint32_t a_objectIndex, const BindlessPlacement& a_placement,
		const BindlessShading& a_shading, const ExtrasFrame& a_extrasFrame, const RE::NiPoint3& a_eye, const RE::NiPoint3& a_previousEye)
	{
		const auto& object = a_tables.objects[a_objectIndex];
		GeometryConstants constants = a_frame.geometryConstants[object.pipelineIndex];
		const auto& vsLayout = LightingVSLayout();
		const auto& psLayout = LightingPSLayout();
		// The placement row's (render flag 0x10 already applied: the previous transform is the current one).
		StoreRelative(&constants.vs.floats[vsLayout.offset[kVSWorld]], a_placement.world, a_eye);
		StoreRelative(&constants.vs.floats[vsLayout.offset[kVSPreviousWorld]], a_placement.previousWorld, a_previousEye);
		// The row's components, the ones SetupGeometry does not write for the pass unwritten (the sentinel), as the engine's.
		ObjectShading shading = a_shading.shading;
		auto* const shadingFloats = reinterpret_cast<float*>(&shading);
		for (std::uint32_t i = 0; i < sizeof(ObjectShading) / sizeof(float); ++i)
			if (!(a_shading.written & (1u << i)))
				shadingFloats[i] = std::bit_cast<float>(kUnwrittenBits);
		std::memcpy(&constants.ps.floats[psLayout.offset[kPSMaterialData]], shading.materialData, sizeof(shading.materialData));
		std::memcpy(&constants.ps.floats[psLayout.offset[kPSEmitColor]], shading.emitColor, sizeof(shading.emitColor));
		constants.ps.floats[psLayout.offset[kPSSSRParams] + 3] = shading.ssrSpecular;
		if (object.flags & kObjectTreeAnim) {
			std::memcpy(&constants.vs.floats[vsLayout.offset[kVSTreeParams]], a_shading.treeParams, sizeof(a_shading.treeParams));
			std::memcpy(&constants.vs.floats[vsLayout.offset[kVSWindTimers]], a_shading.windTimers, 2 * sizeof(float));
		}
		// The extras rows, as the draw completes them. ProjectedUVParams.y is never written by the engine, so it keeps whatever the
		// template left - the unwritten sentinel.
		float completed[kExtraRows * 4];
		if (const float* staticRows = ExtraRowsOf(a_tables, a_objectIndex)) {
			CompleteExtras(staticRows, object.flags, a_placement.world, a_extrasFrame, completed);
			const float* rows = completed;
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

	void PatchObjectGeometry(const SceneStore::Tables& a_tables, std::uint32_t a_objectIndex, const BindlessPlacement& a_placement,
		const BindlessShading& a_shading, const RE::NiPoint3& a_eye, const RE::NiPoint3& a_previousEye, const GeometryPatchOffsets& a_offsets,
		std::span<std::byte> a_vsOut, std::span<std::byte> a_psOut)
	{
		const auto& object = a_tables.objects[a_objectIndex];
		const auto& shading = a_shading.shading;

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
		StoreRelative(world, a_placement.world, a_eye);
		write(a_vsOut, a_offsets.vsWorld, a_offsets.vsWorldSize, world, 12);
		StoreRelative(world, a_placement.previousWorld, a_previousEye);
		write(a_vsOut, a_offsets.vsPreviousWorld, a_offsets.vsPreviousWorldSize, world, 12);

		write(a_psOut, a_offsets.psMaterialData, a_offsets.psMaterialDataSize, shading.materialData,
			static_cast<std::uint32_t>(std::size(shading.materialData)));
		write(a_psOut, a_offsets.psEmitColor, a_offsets.psEmitColorSize, shading.emitColor,
			static_cast<std::uint32_t>(std::size(shading.emitColor)));
		if (object.flags & kObjectTreeAnim) {
			write(a_vsOut, a_offsets.vsTreeParams, a_offsets.vsTreeParamsSize, a_shading.treeParams, 4);
			write(a_vsOut, a_offsets.vsWindTimers, a_offsets.vsWindTimersSize, a_shading.windTimers, 2);
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
		const auto& lights = a_tables.lights[a_objectIndex];
		a_out.roomIndex = lights.roomIndex;
		a_out.recordFlags = RecordFlagsOf(object.flags, a_objectIndex < a_tables.objectFadeRoot.size() ? a_tables.objectFadeRoot[a_objectIndex] : kNoFadeRoot);
		// The same reference the native draw's AlphaTestRefBuffer carries: threshold / 255, and 0 when the
		// object is not alpha tested (the shader's test is then compiled out anyway).
		a_out.alphaTestRef = (object.flags & kObjectAlphaTest) ? ((object.flags >> kObjectAlphaThresholdShift) & 0xFF) / 255.0f : 0.0f;
		a_out.tree = a_objectIndex < a_tables.treeAnim.size() ? a_tables.treeAnim[a_objectIndex] : ObjectTreeAnim{};
		// The tree listing the member draws under, for the trees' wind (DCLFTreeWind): its slot, and its generation (never 0).
		if (const std::uint32_t treeSlot = a_objectIndex < a_tables.objectTree.size() ? a_tables.objectTree[a_objectIndex] : kNoTree; treeSlot != kNoTree) {
			const std::uint32_t generation = treeSlot == kNodelessTree ? kNodelessTree : treeSlot < a_tables.trees.size() ? a_tables.trees[treeSlot].generation : 0u;
			a_out.tree.windTimers[2] = std::bit_cast<float>(treeSlot);
			a_out.tree.windTimers[3] = std::bit_cast<float>(generation);
		} else {
			a_out.tree.windTimers[2] = a_out.tree.windTimers[3] = 0.0f;
		}
		// Skinning: the palette block's rows in the frame's palettes (FrameValues, PaletteRowsOf).
		const bool skinned = (object.flags & kObjectSkinned) && a_objectIndex < a_tables.boneOffset.size();
		const auto palette = skinned ? PaletteRowsOf(a_tables.boneOffset[a_objectIndex], a_tables.boneRows[a_objectIndex]) : PaletteRows{};
		a_out.boneOffset = palette.current;
		a_out.previousBoneOffset = palette.previous;
		a_out.boneRows = skinned ? a_tables.boneRows[a_objectIndex] : 0u;
		const bool extras = a_objectIndex < a_tables.extraOffset.size() && a_tables.extraOffset[a_objectIndex] != kNoExtraRows;
		a_out.extraOffset = extras ? a_tables.extraOffset[a_objectIndex] : 0u;
		// The LOD fades the pass draws with (MakeShading's rule: MaterialData.x for the Envmap technique, .y and SSRParams.w
		// with Specular), faded by the draw while the object's fade node has them apply (its placement row's).
		std::uint32_t fades = 0;
		if (!(object.flags & kObjectNoBindings) && object.pipelineIndex < a_tables.pipelines.size()) {
			const std::uint32_t pass = a_tables.pipelines[object.pipelineIndex].passDescriptor;
			fades = ((pass & 0x200u) ? kLodFadeSpecular | ((a_renderFlags & 2) ? 0u : kLodFadeSsr) : 0u) | (((pass >> 24) & 0x3f) == 1 ? kLodFadeEnvmap : 0u);
		}
		a_out.lodFades = fades;
	}

	void SampleShading(const RE::BSLightingShaderProperty& a_property, std::uint32_t a_pass, bool a_member, BindlessShading& a_out)
	{
		// BSLightingShader::SetupGeometry (engine notes): which components it writes depends on the pass.
		const bool specular = (a_pass & 0x200u) != 0;              // the pass descriptor's Specular
		// MaterialData.x (property +0x104, envmapLODFade): SetupGeometry's cases 1, 0xb and 0x10 (Envmap, MultilayerParallax, Eye;
		// 1414dd040), which Lighting.hlsl's environment map reads under ENVMAP, MULTI_LAYER_PARALLAX and EYE.
		const std::uint32_t technique = (a_pass >> 24) & 0x3f;
		const bool envmap = technique == 1 || technique == 0xb || technique == 0x10;
		auto& shading = a_out.shading;
		shading.materialData[0] = envmap ? a_property.envmapLODFade : 0.0f;
		shading.materialData[1] = specular ? a_property.specularLODFade : 0.0f;
		// GetRenderPasses leaves materialAlpha * the fade node's currentFade on the property, for whichever camera called it last. A
		// member's is the material's alone: the draw multiplies its root's fade in (T1c, Lighting.hlsl DCLFAlphaFade).
		const auto* material = static_cast<const RE::BSLightingShaderMaterialBase*>(a_property.material);
		shading.materialData[2] = a_member && material ? material->materialAlpha : a_property.alpha;
		shading.materialData[3] = 0.0f;
		a_out.written = (envmap ? 1u : 0u) | (specular ? 2u : 0u) | 4u | (a_property.emissiveColor ? 0x70u : 0u) | 0x80u;
		// The same sample the emissive colour folds in: the shader divides it out again, so the two must never come from different
		// reads of an animated value.
		const float mult = a_property.emissiveMult;
		a_out.emissiveMult = mult;
		const auto* emissive = a_property.emissiveColor;
		shading.emitColor[0] = emissive ? emissive->red * mult : 0.0f;
		shading.emitColor[1] = emissive ? emissive->green * mult : 0.0f;
		shading.emitColor[2] = emissive ? emissive->blue * mult : 0.0f;
		shading.ssrSpecular = ((SceneStore::kMainPassRenderFlags & 2) ? 0.0f : 1.0f) * (specular ? a_property.specularLODFade : 0.0f);
		a_out.padding[0] = a_out.padding[1] = 0u;
		// The tree's wind, as SetupGeometry's case 0xc makes it now (T6b1a: was the accumulate phase's, from the node).
		ObjectTreeAnim tree{};
		if (a_property.flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kTreeAnim))
			DeriveTreeAnim(a_property, tree);
		std::memcpy(a_out.treeParams, tree.treeParams, sizeof(a_out.treeParams));
		a_out.windTimers[0] = tree.windTimers[0];
		a_out.windTimers[1] = tree.windTimers[1];
		a_out.windTimers[2] = a_out.windTimers[3] = 0.0f;
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
