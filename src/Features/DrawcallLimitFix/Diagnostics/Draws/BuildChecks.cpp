#if defined(CS_HAS_RENDER_GRAPH) && defined(CS_HAS_ORG_MODULE_SERVICES)
#	include "DrawsState.h"
#	include "Events.h"
#	include "Observers.h"

// The main builds' checks, counted into the payload (PayloadChecks) and summed at its commits: CS_DCLF_BINDLESS_PARITY (each object's
// record against the PerGeometry group it replaces) and CS_DCLF_RESIDENT_DRAW_PARITY (the resident region against its entries written
// again from the tables).

namespace DCLF::Diagnostics
{
	namespace
	{
		// The per-object record against the packed constant group, variable by variable. Both are produced from tables.objects and the
		// shading row by the same rules, so the comparison is exact rather than tolerant - a tolerance here would only hide a layout
		// mistake.
		void CheckBindlessRecord(const SceneTables& a_tables, std::uint32_t a_objectIndex, const BindlessObject& a_record,
			const BindlessPlacement& a_placement, const BindlessShading& a_shading, const RE::NiPoint3& a_eye,
			const RE::NiPoint3& a_previousEye, const GeometryPatchOffsets& a_offsets, std::span<const std::byte> a_vs, std::span<const std::byte> a_ps,
			PayloadChecks& a_checks)
		{
			auto compare = [&](std::span<const std::byte> a_group, std::uint32_t a_offset, std::uint32_t a_count, const float* a_expected, const char* a_name) {
				if (a_offset == ~0u)
					return;  // the pipeline's shaders do not declare it, so neither form carries a value
				for (std::uint32_t c = 0; c < a_count; ++c) {
					const std::size_t at = (std::size_t(a_offset) + c) * 4;
					if (at + 4 > a_group.size())
						return;
					float packed = 0.0f;
					std::memcpy(&packed, a_group.data() + at, 4);
					++a_checks.bindless;
					if (std::bit_cast<std::uint32_t>(packed) == std::bit_cast<std::uint32_t>(a_expected[c]))
						continue;
					if (a_checks.bindlessDiffer++ == 0)
						logger::warn("[DCLF] bindless record parity: {}[{}] is {} in the record and {} in the constant group", a_name, c, a_expected[c], packed);
				}
			};
			// The placement is absolute; the shader makes it relative with the same single subtraction as this.
			float relative[16] = {};
			StoreRelativeTo(relative, a_placement.world, a_eye);
			compare(a_vs, a_offsets.vsWorld, std::min(a_offsets.vsWorldSize, 12u), relative, "World");
			StoreRelativeTo(relative, a_placement.previousWorld, a_previousEye);
			compare(a_vs, a_offsets.vsPreviousWorld, std::min(a_offsets.vsPreviousWorldSize, 12u), relative, "PreviousWorld");
			compare(a_ps, a_offsets.psMaterialData, std::min(a_offsets.psMaterialDataSize, 4u), a_shading.shading.materialData, "MaterialData");
			compare(a_ps, a_offsets.psEmitColor, std::min(a_offsets.psEmitColorSize, 3u), a_shading.shading.emitColor, "EmitColor");
			if (a_offsets.psSSRParams != ~0u && a_offsets.psSSRParamsSize > 3)
				compare(a_ps, a_offsets.psSSRParams + 3, 1, &a_shading.shading.ssrSpecular, "SSRParams.w");

			// The tail has no constant group of its own to compare against (it is the record's alone), so it is checked
			// against the values the engine's buffers would hold, derived from the tables.
			const auto& object = a_tables.objects[a_objectIndex];
			const auto& lights = a_tables.lights[a_objectIndex];
			auto expect = [&](bool a_equal, const char* a_name) {
				++a_checks.bindless;
				if (!a_equal && a_checks.bindlessDiffer++ == 0)
					logger::warn("[DCLF] bindless record parity: {} differs for object {}", a_name, a_objectIndex);
			};
			expect(a_record.roomIndex == lights.roomIndex, "RoomIndex");
			const std::uint32_t refraction =
				!(object.flags & kObjectNoBindings) && object.pipelineIndex < a_tables.pipelines.size() ?
					RefractionRecordFlagsOf(a_tables.pipelines[object.pipelineIndex]) | EffectRecordFlagsOf(a_tables.pipelines[object.pipelineIndex], object.flags) :
					0u;
			expect(a_record.recordFlags == (RecordFlagsOf(object.flags, a_objectIndex < a_tables.objectFadeRoot.size() ? a_tables.objectFadeRoot[a_objectIndex] : kNoFadeRoot) | refraction),
				"RecordFlags");
			const float threshold = AlphaTestRefOf(object.flags, object.pipelineIndex < a_tables.pipelines.size() ? &a_tables.pipelines[object.pipelineIndex] : nullptr);
			expect(std::bit_cast<std::uint32_t>(a_record.alphaTestRef) == std::bit_cast<std::uint32_t>(threshold), "AlphaTestRef");
			const bool skinned = (object.flags & kObjectSkinned) && a_objectIndex < a_tables.boneOffset.size();
			const auto palette = skinned ? PaletteRowsOf(a_tables.boneOffset[a_objectIndex], a_tables.boneRows[a_objectIndex]) : PaletteRows{};
			expect(a_record.boneOffset == palette.current, "BoneOffset");
			expect(a_record.boneRows == (skinned ? a_tables.boneRows[a_objectIndex] : 0u), "BoneRows");
			expect(a_record.previousBoneOffset == palette.previous, "PreviousBoneOffset");
			const bool extras = a_objectIndex < a_tables.extraOffset.size() && a_tables.extraOffset[a_objectIndex] != kNoExtraRows;
			expect(a_record.extraOffset == (extras ? a_tables.extraOffset[a_objectIndex] : 0u), "ExtraOffset");
		}
	}

	void BeginBindlessChecks(const MainBuildBegun&)
	{
		DrawsState::Get().bindless.templates.clear();
	}

	void CheckBindless(const MainRecordAssembled& a_record)
	{
		// The record the shaders read against the PerGeometry group the engine's constant buffer holds for the same object
		// (PatchObjectGeometry). The two derive from the same inputs through the same unwritten-component rule, so anything but bit
		// equality is a defect in the record's layout or in the way it is filled, caught on the CPU with no readback.
		const auto& tables = a_record.tables;
		const std::uint32_t o = a_record.object;
		if (o >= tables.objects.size())
			return;
		constexpr auto renderFlags = GeometryPort::kMainPassRenderFlags;
		auto& state = DrawsState::Get().bindless;
		const std::uint32_t pipeline = tables.objects[o].pipelineIndex;
		auto [templateIt, newTemplate] = state.templates.try_emplace(pipeline);
		auto& geometryTemplate = templateIt->second;
		if (newTemplate)
			PackGeometryTemplate(tables.pipelineConstants[pipeline].constants, a_record.vsTable, a_record.psTable, true, geometryTemplate);
		state.vs.assign(geometryTemplate.vs.begin(), geometryTemplate.vs.end());
		state.ps.assign(geometryTemplate.ps.begin(), geometryTemplate.ps.end());
		// The placement and the shading are FrameValues' (the frame's rows): the layout's check takes made-up ones, the same on both
		// sides, every component distinct.
		BindlessPlacement placement{};
		for (std::uint32_t c = 0; c < 12; ++c) {
			placement.world[c] = 1.0f + static_cast<float>(c) + static_cast<float>(o & 0xFF) * 0.125f;
			placement.previousWorld[c] = (renderFlags & 0x10) ? placement.world[c] : placement.world[c] + 0.5f;
		}
		BindlessShading shading{};
		float* const shadingFloats = reinterpret_cast<float*>(&shading.shading);
		for (std::uint32_t c = 0; c < sizeof(ObjectShading) / sizeof(float); ++c)
			shadingFloats[c] = 0.25f + static_cast<float>(c) + static_cast<float>(o & 0xFF) * 0.0625f;
		shading.written = 0xFFu;
		PatchObjectGeometry(tables, o, placement, shading, a_record.eye, a_record.previousEye, geometryTemplate.offsets, state.vs, state.ps);
		BindlessObject record;
		BuildObjectRecord(tables, o, GeometryPort::kMainPassRenderFlags, record);
		CheckBindlessRecord(tables, o, record, placement, shading, a_record.eye, a_record.previousEye, geometryTemplate.offsets, state.vs, state.ps,
			a_record.out.checks);
	}

	void CheckResidentRegion(const ResidentRegionUpdated& a_updated)
	{
		// Every entry written again from the tables and compared, and every resident the region should hold looked for.
		if (!ParityDue(a_updated.frame))
			return;
		const auto& r = a_updated.region;
		const auto& reference = a_updated.reference;
		const auto& tables = a_updated.tables;
		auto& out = a_updated.out.checks;
		const auto& inputs = r.inputs.Get();
		for (std::uint32_t i = 0; i < inputs.size(); ++i) {
			// The main part of the entry (the shadow build's words are its own check's: CheckKeptShadow).
			if (!(r.sideOf[i] & kSideMain))
				continue;
			DrawInput fresh{};
			const std::uint32_t o = inputs[i].objectIndex;
			const bool known = o < tables.objects.size() && reference.Eligible(o);
			const auto draws = known ? reference.Entry(o, fresh) : std::uint8_t{ 0 };
			DrawInput expected = inputs[i];
			SetMainPart(expected, fresh);
			++out.resident;
			if (!known || draws != r.drawsOf[i] || std::memcmp(&expected, &inputs[i], sizeof(DrawInput)) != 0 || r.indexOf[o] != i) {
				// The first few, with why.
				if (out.residentDiffer++ < 6)
					logger::info("[DCLF] main region parity: entry {} object {}: {} (resident {}, flags {:#x} vs {:#x}, pipeline {} vs {}, record {} vs {}, draws {} vs {}, indexOf {})",
						i, o, !known ? "not eligible" : "differs", o < tables.residentSlot.size() ? tables.residentSlot[o] : 9,
						inputs[i].flags, expected.flags, inputs[i].pipelineIndex, expected.pipelineIndex, inputs[i].recordIndex, expected.recordIndex, r.drawsOf[i], draws,
						o < r.indexOf.size() ? r.indexOf[o] : ~0u);
			}
		}
		for (std::uint32_t o = 0; o < tables.objects.size(); ++o)
			if (reference.Eligible(o) && !r.MainHolds(o))
				++out.residentMissing;
		// Every pair's witness as UpdateRegionPairs makes it, against the one it holds: a difference is a change no event named.
		for (const auto& [key, pair] : r.pairs) {
			std::uint64_t witness = 0;
			if (!reference.PairWitness(key, witness))
				continue;
			++out.pairs;
			out.pairsStale += witness != pair.witness ? 1 : 0;
		}
	}

	void SumBuildChecks(const MainPayloadCommitted& a_committed)
	{
		const auto& c = a_committed.payload.checks;
		auto& totals = DrawsState::Get().buildChecks;
		totals.resident += c.resident;
		totals.residentDiffer += c.residentDiffer;
		totals.residentMissing += c.residentMissing;
		totals.pairs += c.pairs;
		totals.pairsStale += c.pairsStale;
		totals.bindless += c.bindless;
		totals.bindlessDiffer += c.bindlessDiffer;
	}
}

#endif
