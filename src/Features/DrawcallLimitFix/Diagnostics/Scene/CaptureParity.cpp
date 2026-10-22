// CaptureParity's checks (CS_DCLF_CAPTURE_PARITY): each native Lighting draw of the main pass and the reflection faces against the
// installed tables, from the SetupGeometry, DrawIndexed and Map/Unmap detours. Render thread.

#include "CaptureParity.h"

#include "Features/DrawcallLimitFix/Core/Extensions/ObjectContributors.h"
#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"
#include "Features/DrawcallLimitFix/Core/Engine/SkinPartitions.h"
#include "Features/DrawcallLimitFix/Core/Gpu/FrameValues.h"
#include "Features/DrawcallLimitFix/Core/Shading/LightingConstants.h"
#include "Features/DrawcallLimitFix/Core/Handoff/PassCapture.h"

#include <algorithm>
#include <cstring>

#include "Deferred.h"
#include "Features/DrawcallLimitFix/Core/Extensions/FrameBindings.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Materials.h"
#include "Features/DrawcallLimitFix/Integrations/SubsurfaceScattering/SubsurfaceScattering.h"
#include "Features/DrawcallLimitFix/Core/Scene/Coordinator/Coordinator.h"
#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"
#include "Features/DrawcallLimitFix/Core/Shading/MaterialSources.h"
#include "Features/DrawcallLimitFix/Core/Shading/MaterialWrites.h"
#include "ShaderCache.h"
#include "State.h"

namespace DCLF
{
	namespace
	{
		constexpr std::size_t kMaxSamples = 24;

		// Constant buffers that change per draw by design, from b3 (Extensions::PerDrawMask): the permutation (b4, checked separately),
		// bone palettes (VS b9, b10; SKINNED) and the alpha-test reference (PS b11), Core's; and the integrations' declared per-draw
		// buffers: LLF's StrictLightData (PS b3, Integrations/LightLimitFix's parity), Skin's SkinPerGeometry (PS b7; SKIN) and Linear
		// Lighting's LLPerGeometry (PS b8).
		std::uint32_t PerDrawVSBuffers() { return Extensions::PerDrawMask(Api::ShaderStage::Vertex) & ~0x7u; }
		std::uint32_t PerDrawPSBuffers() { return Extensions::PerDrawMask(Api::ShaderStage::Pixel) & ~0x7u; }

		/**
		 * @brief Descriptor bits a DCLF draw decides on the GPU rather than in its pipeline (excluded from the comparison):
		 * - A sun-test record's shadow bits (ShadowDir, DefShadow, shadow light count), dropped by BuildDraws on a cascade miss;
		 *   checked by the culling readback's sun counters.
		 * - LOD fades: an object whose fades apply keeps Specular and the Envmap technique at any distance and the draw fades them
		 *   to zero, where GetRenderPasses drops them; checked by the shading parity.
		 */
		struct PerDrawBits
		{
			std::uint32_t pass = 0, vertex = 0, pixel = 0;
			bool envmap = false;  // Envmap technique (1) faded by the draw: the engine's pass is None (0) past the fade's end

			/** @brief The native descriptor with DCLF's technique where the engine dropped a faded Envmap. */
			std::uint32_t Native(std::uint32_t a_native, std::uint32_t a_dclf) const
			{
				return envmap && ((a_dclf >> 24) & 0x3f) == 1 && ((a_native >> 24) & 0x3f) == 0 ? a_native | (1u << 24) : a_native;
			}
		};

		PerDrawBits PerDrawBitsOf(const SceneTables& a_tables, std::uint32_t a_object, std::uint32_t a_pass)
		{
			PerDrawBits bits;
			if (a_tables.objects[a_object].flags & kObjectSunTest) {
				bits.pass |= kShadowBits;
				bits.pixel |= kShadowBits & ~0x1c0u;  // the pixel stage drops the count
				bits.vertex |= kShadowBits & 0x48007u;  // DefShadow (VertexDescriptorFromPass)
			}
			// The engine's current fade node (a layer's is its layer property's).
			const auto* geometry = a_object < a_tables.objectGeometry.size() ? a_tables.objectGeometry[a_object] : nullptr;
			const RE::BSShaderProperty* fadeProperty = !geometry ? nullptr : a_tables.IsLayer(a_object) ? LayerPropertyOf(*geometry) :
			                                                                                            geometry->GetGeometryRuntimeData().shaderProperty.get();
			if (geometry && LodFadesApply(LodFadeNodeOf(fadeProperty))) {
				if (a_pass & 0x200u) {
					bits.pass |= 0x200u;
					bits.pixel |= 0x200u;
					bits.vertex |= 0x200u;
				}
				bits.envmap = ((a_pass >> 24) & 0x3f) == 1;
			}
			return bits;
		}

		/**
		 * @brief A member whose feature block variables a feature's pass writes: its record carries a table family's reference
		 * (Api::TableFamily::recordWord; Features/TreeWind's: a member listed under its tree, whose wind TreeWindCS writes).
		 */
		bool GpuWrittenBlock(const SceneTables& a_tables, std::uint32_t a_object)
		{
			if (!Scene::FrameView::Get().IsMember(static_cast<std::int32_t>(a_object)))
				return false;
			for (const auto& family : a_tables.families.All())
				if (family.recordWord != ~0u && family.RowOf(a_object) != Extensions::kNoFamilyRow)
					return true;
			return false;
		}

		std::string Describe(const RE::BSGeometry* a_geometry)
		{
			const char* name = a_geometry->name.c_str();
			auto* ref = a_geometry->GetUserData();
			for (auto* node = a_geometry->parent; !ref && node; node = node->parent)
				ref = node->GetUserData();
			return fmt::format("'{}' ref {:08X}", name ? name : "", ref ? ref->GetFormID() : 0u);
		}

		// For values the engine updates while the frame renders (EmitColor: time-of-day emittance).
		bool WithinTolerance(const auto& a_native, std::uint32_t a_float, float a_expected)
		{
			float native = 0.0f;
			std::memcpy(&native, &a_native.bytes[a_float * 4], 4);
			return std::abs(native - a_expected) <= 1e-3f * std::max(std::abs(native), std::abs(a_expected));
		}

		// Which engine structure a node is (TES roots, a loaded cell's 3D or multibound node).
		std::string DescribeRole(const RE::NiAVObject* a_node)
		{
			auto* tes = RE::TES::GetSingleton();
			if (!tes)
				return {};
			if (a_node == tes->objRoot)
				return "[objRoot]";
			if (a_node == tes->lodLandRoot)
				return "[lodLandRoot]";
			std::string role;
			auto check = [&](RE::TESObjectCELL* a_cell) {
				auto* loaded = a_cell ? a_cell->GetRuntimeData().loadedData : nullptr;
				if (!loaded)
					return;
				if (a_node == loaded->cell3D.get())
					role += fmt::format("[cell3D {:08X}]", a_cell->GetFormID());
				if (a_node == loaded->multiBoundNode.get())
					role += fmt::format("[multiBoundNode {:08X}]", a_cell->GetFormID());
			};
			if (tes->interiorCell)
				check(tes->interiorCell);
			else if (auto* grid = tes->gridCells) {
				for (std::uint32_t i = 0; i < grid->length * grid->length; ++i)
					check(grid->cells[i]);
			}
			return role;
		}

		bool SameTransform(const float (&a_record)[12], const RE::NiTransform& a_transform)
		{
			const auto& r = a_transform.rotate.entry;
			const float s = a_transform.scale;
			for (int row = 0; row < 3; ++row) {
				if (a_record[row * 4 + 0] != r[row][0] * s || a_record[row * 4 + 1] != r[row][1] * s || a_record[row * 4 + 2] != r[row][2] * s)
					return false;
			}
			return a_record[3] == a_transform.translate.x && a_record[7] == a_transform.translate.y && a_record[11] == a_transform.translate.z;
		}
	}

	bool CaptureParity::Enabled()
	{
		return SwitchEnabled(Switch::CaptureParity);
	}

	CaptureParity& CaptureParity::Get()
	{
		static CaptureParity parity;
		return parity;
	}

	void CaptureParity::NoteMismatch(std::string a_message)
	{
		lastNote = a_message;
		if (samples.size() < kMaxSamples)
			samples.push_back(std::move(a_message));
	}

	void CaptureParity::OnMap(ID3D11Resource* a_resource, void* a_data)
	{
		lastMappedAny[a_resource] = a_data;
		if (baselineValid) {
			const std::uint32_t perDrawVS = PerDrawVSBuffers();
			const std::uint32_t perDrawPS = PerDrawPSBuffers();
			for (std::uint32_t slot = kFirstFeatureConstantBuffer; slot < kConstantBufferSlots; ++slot) {
				if (!((perDrawVS >> slot) & 1) && reinterpret_cast<ID3D11Resource*>(baseline.vsBuffers[slot]) == a_resource)
					pendingRewrites |= 1u << slot;
				if (!((perDrawPS >> slot) & 1) && reinterpret_cast<ID3D11Resource*>(baseline.psBuffers[slot]) == a_resource)
					pendingRewrites |= 1u << (16 + slot);
			}
		}
		// Only the current shaders' PerMaterial and PerGeometry buffers are of interest.
		auto* vs = *globals::game::currentVertexShader;
		auto* ps = *globals::game::currentPixelShader;
		for (std::uint32_t level : { kPerTechnique, kPerMaterial, kPerGeometry }) {
			if (vs && reinterpret_cast<ID3D11Resource*>(vs->constantBuffers[level].buffer) == a_resource) {
				Slot(0, level).buffer = a_resource;
				Slot(0, level).mapped = Slot(0, level).lastMapped = a_data;
			}
			if (ps && reinterpret_cast<ID3D11Resource*>(ps->constantBuffers[level].buffer) == a_resource) {
				Slot(1, level).buffer = a_resource;
				Slot(1, level).mapped = Slot(1, level).lastMapped = a_data;
			}
		}
	}

	void CaptureParity::Snapshot(ConstantSnapshot& a_snapshot, ID3D11Resource* a_resource)
	{
		if (a_snapshot.buffer != a_resource || !a_snapshot.mapped)
			return;
		D3D11_BUFFER_DESC desc{};
		static_cast<ID3D11Buffer*>(a_resource)->GetDesc(&desc);
		a_snapshot.size = std::min<std::uint32_t>(desc.ByteWidth, static_cast<std::uint32_t>(a_snapshot.bytes.size()));
		std::memcpy(a_snapshot.bytes.data(), a_snapshot.mapped, a_snapshot.size);
		a_snapshot.mapped = nullptr;
		a_snapshot.valid = true;
	}

	void CaptureParity::OnUnmap(ID3D11Resource* a_resource)
	{
		for (auto& stage : snapshots) {
			for (auto& snapshot : stage)
				Snapshot(snapshot, a_resource);
		}
	}

	bool CaptureParity::CompareBlock(const RE::BSGeometry* a_geometry, const char* a_what, const ConstantBlock& a_expected, const StageLayout& a_layout,
		const void* a_nativeTable, std::size_t a_nativeTableSize, const ConstantSnapshot& a_native, ID3D11Resource* a_boundBuffer,
		std::uint32_t a_firstVariable, std::uint64_t a_variables, std::uint64_t a_tolerant)
	{
		// Compared only when the bound buffer is the one the Map/Unmap detours snapshotted; otherwise it counts as OK. Coverage is
		// reported, since a block never compared is not a passing check.
		const std::string what = labelPrefix + a_what;
		auto& coverage = blockCoverage[what];
		if (!a_native.valid || a_boundBuffer != a_native.buffer) {
			++coverage.second;
			return true;
		}
		++coverage.first;

		bool ok = true;
		for (std::uint32_t i = 0; i < a_layout.count && i < a_nativeTableSize; ++i) {
			if (!((a_variables >> i) & 1))
				continue;
			// Reflection leaves offset 0 for variables a permutation lacks: only valid for the group's first variable.
			const std::uint32_t nativeOffset = static_cast<const std::uint8_t*>(a_nativeTable)[i];
			if (nativeOffset == 0 && i != a_firstVariable)
				continue;
			const std::uint32_t ourOffset = a_layout.offset[i];
			bool anyWritten = false;
			for (std::uint32_t c = 0; c < a_layout.size[i]; ++c)
				anyWritten |= a_expected.Written(ourOffset + c);
			if (!anyWritten) {
				++unevaluated[fmt::format("{} {}", what, i)];
				continue;
			}
			bool differs = false;
			std::uint32_t firstComponent = 0;
			for (std::uint32_t c = 0; c < a_layout.size[i]; ++c) {
				// Only components the engine writes; the rest of a discard-mapped buffer is undefined.
				if (!a_expected.Written(ourOffset + c) || (nativeOffset + c + 1) * 4 > a_native.size)
					continue;
				if (std::memcmp(&a_native.bytes[(nativeOffset + c) * 4], &a_expected.floats[ourOffset + c], 4) != 0 &&
					!(((a_tolerant >> i) & 1) && WithinTolerance(a_native, nativeOffset + c, a_expected.floats[ourOffset + c]))) {
					if (!differs)
						firstComponent = c;
					differs = true;
				}
			}
			if (differs) {
				ok = false;
				// A histogram plus at most two samples per variable, so one standing difference cannot fill the capped sample list.
				if (++mismatchByVariable[fmt::format("{} {}", what, i)] <= 2) {
					float native = 0;
					std::memcpy(&native, &a_native.bytes[(nativeOffset + firstComponent) * 4], 4);
					NoteMismatch(fmt::format("{} {} variable {} component {}: DCLF {}, native {}{}", Describe(a_geometry), what, i, firstComponent,
						a_expected.floats[ourOffset + firstComponent], native, compareContext));
				}
			}
		}
		return ok;
	}

	bool CaptureParity::CompareMaterial(const RE::BSGeometry* a_geometry, std::uint32_t a_materialIndex)
	{
		const auto& tables = Scene::FrameView::Get().GetTables();
		const auto& record = tables.materials[a_materialIndex];
		const bool frameCharacterLight = MaterialSources::FrameCharacterLight(tables.materialSlotKey[a_materialIndex].second);
		auto* vs = *globals::game::currentVertexShader;
		auto* ps = *globals::game::currentPixelShader;
		bool ok = true;
		// Not compared: TexcoordOffset, which the GPU patches into the rows (frame values' texture transforms; the record keeps its
		// capture's), and IBLParams, which no Lighting stage reads (the record keeps its first: KeepUnreadFloats).
		if (vs)
			ok &= CompareBlock(a_geometry, "VS PerMaterial", record.vs, LightingVSLayout(), vs->constantTable.data(), vs->constantTable.size(), Slot(0, kPerMaterial),
				reinterpret_cast<ID3D11Resource*>(vs->constantBuffers[kPerMaterial].buffer), kVSLeftEyeCenter, kVSGroups[kPerMaterial] & ~(1ull << kVSTexcoordOffset), 0);
		if (ps)
			ok &= CompareBlock(a_geometry, "PS PerMaterial", record.ps, LightingPSLayout(), ps->constantTable.data(), ps->constantTable.size(), Slot(1, kPerMaterial),
				reinterpret_cast<ID3D11Resource*>(ps->constantBuffers[kPerMaterial].buffer), kPSLODTexParams, kPSGroups[kPerMaterial] & ~(1ull << kPSIBLParams), 0);

		const auto& state = globals::game::shadowState->GetRuntimeData();
		for (std::uint32_t slot = 0; slot < kPixelTextureSlots; ++slot) {
			if (!((record.textureWritten >> slot) & 1))
				continue;
			const auto* native = reinterpret_cast<const ID3D11ShaderResourceView*>(state.PSTexture[slot]);
			const auto nativeMode = static_cast<std::uint32_t>(state.PSTextureAddressMode[slot].underlying());
			// A character-light pass's t11 is the frame's view (kCharacterLightRegister).
			const auto* view = frameCharacterLight && slot == 11 ? Scene::FrameView::Get().GetFrameCapture().characterLightView : record.textures[slot];
			if (native != view || nativeMode != record.addressModes[slot]) {
				ok = false;
				NoteMismatch(fmt::format("{} texture slot {}: DCLF {} mode {}, native {} mode {}", Describe(a_geometry), slot,
					fmt::ptr(view), record.addressModes[slot], fmt::ptr(native), nativeMode));
			}
		}
		return ok;
	}

	bool CaptureParity::CompareGeometry(const RE::BSGeometry* a_geometry, std::uint32_t a_objectIndex, std::uint32_t a_renderFlags)
	{
		const auto& tables = Scene::FrameView::Get().GetTables();
		const auto& object = tables.objects[a_objectIndex];
		if ((object.flags & kObjectNoBindings) || !Scene::FrameView::Get().GetFrameTables().geometryConstantsValid[object.pipelineIndex])
			return true;

		// Expected: the per-frame block for this pass descriptor plus per-object values.
		auto& shadowState = globals::game::shadowState->GetRuntimeData();
		// The engine's current placement (render flag 0x10: the previous transform is the current one).
		BindlessPlacement placement{};
		if (!FrameValues::SampleSlot(tables, a_objectIndex, placement))
			return true;
		if (a_renderFlags & 0x10)
			std::memcpy(placement.previousWorld, placement.world, sizeof(placement.world));
		// The frame's shading (sampled at frame start); if the property changed since, the engine's current one (counted).
		const auto* shadingRows = FrameValues::Get().ShadingIfDone(true);
		const auto* property = Scene::Coordinator::Get().SlotPropertyOf(a_objectIndex);
		if (!shadingRows || a_objectIndex >= shadingRows->size() || !property)
			return true;
		BindlessShading shading = (*shadingRows)[a_objectIndex];
		BindlessShading now = shading;
		SampleShading(*property, tables.pipelines[object.pipelineIndex].passDescriptor, Scene::FrameView::Get().IsMember(static_cast<std::int32_t>(a_objectIndex)), now);
		if (std::memcmp(&now, &shading, sizeof(now)) != 0) {
			++shadingSinceStart;
			shading = now;
		}
		GeometryConstants expected = ObjectGeometryConstants(tables, Scene::FrameView::Get().GetFrameTables(), a_objectIndex, placement, shading, SampleExtrasFrame(), shadowState.posAdjust.getEye(),
			shadowState.previousPosAdjust.getEye());
		const auto& vsLayout = LightingVSLayout();
		const auto& psLayout = LightingPSLayout();
		// A block variable's floats past its compared ones are not compared (Api::ShadingVariable::compared): Features/TreeWind's
		// WindTimers.y (previous timer), which SetupGeometry (AE 1414dd040, case 0xc) copies from the tree node's current timer
		// (+0x168 = +0x164) after every native draw, so it depends on native draw order, and DCLF does not write engine nodes.
		// Lighting.hlsl reads it only into the unused previousInputPosition.
		const auto& block = Extensions::ShadingBlockRule();
		for (const auto& variable : block.variables)
			for (std::uint32_t c = variable.compared; c < variable.floats && c < vsLayout.size[variable.variable]; ++c)
				expected.vs.floats[vsLayout.offset[variable.variable] + c] = std::bit_cast<float>(kUnwrittenBits);
		// A multi-index shape's ProjectedUVParams2.zw: FUN_1414e00c0 writes only xy (normalDampener, materialScale); zw is leftover.
		if ((object.flags & kObjectProjectedUV) && const_cast<RE::BSGeometry*>(a_geometry)->GetType().get() == RE::BSGeometry::Type::kMultiIndexTriShape &&
			psLayout.size[kPSProjectedUVParams + 1] > 3) {
			expected.ps.floats[psLayout.offset[kPSProjectedUVParams + 1] + 2] = std::bit_cast<float>(kUnwrittenBits);
			expected.ps.floats[psLayout.offset[kPSProjectedUVParams + 1] + 3] = std::bit_cast<float>(kUnwrittenBits);
		}

		auto* vs = *globals::game::currentVertexShader;
		auto* ps = *globals::game::currentPixelShader;
		bool ok = true;
		compareContext = fmt::format(" (pipeline descriptor {:08X}, skinned object {})", tables.pipelines[object.pipelineIndex].passDescriptor,
			(object.flags & kObjectPaletteBlock) != 0);
		// The block's variables (TreeParams and WindTimers): SetupGeometry writes them for its objects only (technique 12; otherwise
		// leftover, unread), and a member tree's come from a feature's pass (TreeWindCS: its own parity checks those). World and
		// PreviousWorld: SetupGeometry skips them for skinned objects (engine notes); the SKINNED vertex shader positions from the palette.
		const std::uint64_t vsMask = kVSGroups[kPerGeometry] &
		                             ((object.flags & block.objectFlags) && !GpuWrittenBlock(tables, a_objectIndex) ? ~0ull : ~block.vsMask) &
		                             ((object.flags & kObjectPaletteBlock) ? ~((1ull << kVSWorld) | (1ull << kVSPreviousWorld)) : ~0ull);
		if (vs)
			ok &= CompareBlock(a_geometry, "VS PerGeometry", expected.vs, vsLayout, vs->constantTable.data(),
				vs->constantTable.size(), Slot(0, kPerGeometry), reinterpret_cast<ID3D11Resource*>(vs->constantBuffers[kPerGeometry].buffer), kVSWorld, vsMask, 0);
		if (ps)
			ok &= CompareBlock(a_geometry, "PS PerGeometry", expected.ps, psLayout, ps->constantTable.data(), ps->constantTable.size(), Slot(1, kPerGeometry),
				reinterpret_cast<ID3D11Resource*>(ps->constantBuffers[kPerGeometry].buffer), kPSDirLightDirection,
				kPSGroups[kPerGeometry] & ~kPSLightAssignment, 1ull << kPSEmitColor);
		return ok;
	}

	bool CaptureParity::CompareTechnique(const RE::BSGeometry* a_geometry, std::uint32_t a_objectIndex)
	{
		const auto& tables = Scene::FrameView::Get().GetTables();
		const auto& object = tables.objects[a_objectIndex];
		if (!tables.TechniqueConstantsValid(object.pipelineIndex))
			return true;
		const auto& technique = tables.TechniqueOf(object.pipelineIndex).value;
		// SetupTechnique writes VPOSOffset after unmapping (engine notes); under DXVK the memory stays mapped and the draw sees the
		// late write, so compare the current contents.
		for (auto& stage : snapshots) {
			auto& snapshot = stage[kPerTechnique];
			if (snapshot.valid && snapshot.lastMapped)
				std::memcpy(snapshot.bytes.data(), snapshot.lastMapped, snapshot.size);
		}
		auto* vs = *globals::game::currentVertexShader;
		auto* ps = *globals::game::currentPixelShader;
		bool ok = true;
		// Terrain LOD's HighDetailRange: DCLF's centre is absolute; the engine's is relative to posAdjust.
		ConstantBlock techniqueVS = technique.vs;
		if (const std::uint32_t t = (tables.pipelines[object.pipelineIndex].passDescriptor >> 24) & 0x3f; t == 9 || t == 18) {
			const auto eye = globals::game::shadowState->GetRuntimeData().posAdjust.getEye();
			float* range = &techniqueVS.floats[LightingVSLayout().offset[kVSHighDetailRange]];
			range[0] -= eye.x;
			range[1] -= eye.y;
		}
		// Not compared: the fog, which a technique row keeps from its making (KeepTechniqueFog); the vertex stage reads the frame's
		// (DCLFFrameFog) and no Lighting stage reads FogColor.
		std::uint64_t vsFog = 0;
		for (const std::uint32_t variable : kVSFrameFog)
			vsFog |= 1ull << variable;
		if (vs)
			ok &= CompareBlock(a_geometry, "VS PerTechnique", techniqueVS, LightingVSLayout(), vs->constantTable.data(), vs->constantTable.size(),
				Slot(0, kPerTechnique), reinterpret_cast<ID3D11Resource*>(vs->constantBuffers[kPerTechnique].buffer), kVSHighDetailRange, kVSGroups[kPerTechnique] & ~vsFog, 0);
		if (ps)
			ok &= CompareBlock(a_geometry, "PS PerTechnique", technique.ps, LightingPSLayout(), ps->constantTable.data(), ps->constantTable.size(),
				Slot(1, kPerTechnique), reinterpret_cast<ID3D11Resource*>(ps->constantBuffers[kPerTechnique].buffer), kPSFogColor, kPSGroups[kPerTechnique] & ~(1ull << kPSFogColor), 0);

		// Filter modes: SetupMaterial's, else the technique's.
		const auto& material = tables.materials[object.materialIndex];
		const auto& state = globals::game::shadowState->GetRuntimeData();
		if (technique.shadowMask) {
			const auto* native = reinterpret_cast<const ID3D11ShaderResourceView*>(state.PSTexture[kShadowMaskSlot]);
			const auto nativeAddress = static_cast<std::uint32_t>(state.PSTextureAddressMode[kShadowMaskSlot].underlying());
			const auto nativeFilter = static_cast<std::uint32_t>(state.PSTextureFilterMode[kShadowMaskSlot].underlying());
			if (native != technique.shadowMaskTexture || nativeAddress != 0 || nativeFilter != technique.filterModes[kShadowMaskSlot]) {
				ok = false;
				NoteMismatch(fmt::format("{} shadow mask: DCLF {} filter {}, native {} address {} filter {}", Describe(a_geometry),
					fmt::ptr(technique.shadowMaskTexture), technique.filterModes[kShadowMaskSlot], fmt::ptr(native), nativeAddress, nativeFilter));
			}
		}
		for (std::uint32_t slot = 0; slot < kPixelTextureSlots; ++slot) {
			if (!((material.textureWritten >> slot) & 1))
				continue;
			std::uint32_t expected = material.filterModes[slot];
			if (expected == kUnwrittenFilterMode)
				expected = technique.filterModes[slot];
			if (expected == kUnwrittenFilterMode) {
				++inheritedFilters;
				continue;
			}
			const auto native = static_cast<std::uint32_t>(state.PSTextureFilterMode[slot].underlying());
			if (native != expected) {
				ok = false;
				NoteMismatch(fmt::format("{} filter mode slot {}: DCLF {}, native {}", Describe(a_geometry), slot, expected, native));
			}
		}
		return ok;
	}

	void CaptureParity::ComparePermutation(const RE::BSGeometry* a_geometry, std::uint32_t a_objectIndex)
	{
		const auto& tables = Scene::FrameView::Get().GetTables();
		const auto& object = tables.objects[a_objectIndex];
		const auto& expected = tables.permutations[object.pipelineIndex];
		const auto& native = globals::state->permutationData;
		using Extra = State::ExtraShaderDescriptors;
		// Extra bits the Lighting shader or DCLF's model covers; the rest (IsSun, GrassSphereNormal) are other shaders' leftovers.
		// IsBeastRace only for faces: Subsurface Scattering writes it at a face's SetupGeometry and Lighting.hlsl reads it under
		// SKIN alone, so other draws carry the last face's.
		const std::uint32_t technique = (tables.pipelines[object.pipelineIndex].passDescriptor >> 24) & 0x3f;
		const bool face = technique == 4 || technique == 5;
		const std::uint32_t lightingExtraBits = static_cast<std::uint32_t>(Extra::InWorld) | static_cast<std::uint32_t>(Extra::IsReflections) |
		                                        (face ? static_cast<std::uint32_t>(Extra::IsBeastRace) : 0u) |
		                                        static_cast<std::uint32_t>(Extra::SuppressExternalEmittance) | static_cast<std::uint32_t>(Extra::AdditiveLighting);
		const std::uint32_t extra = (expected.extraShaderDescriptor & lightingExtraBits) |
		                            ((object.flags & kObjectSuppressExternalEmittance) ? static_cast<std::uint32_t>(Extra::SuppressExternalEmittance) : 0u) |
		                            (face && (object.flags & Integrations::SubsurfaceScattering::kObjectBeastRace) ? static_cast<std::uint32_t>(Extra::IsBeastRace) : 0u);
		// Excluded: hair's ProjectedUV bit (DCLF drops it: HairProjection) and PerDrawBits.
		const std::uint32_t hairProjection = HairProjection(native.VertexShaderDescriptor) ? kPassProjectedUV : 0u;
		const PerDrawBits perDraw = PerDrawBitsOf(tables, a_objectIndex, tables.pipelines[object.pipelineIndex].passDescriptor);
		const std::array<std::pair<std::uint32_t, std::uint32_t>, 4> fields{ {
			{ expected.vertexShaderDescriptor & ~perDraw.vertex,
				perDraw.Native(native.VertexShaderDescriptor, expected.vertexShaderDescriptor) & ~(hairProjection | perDraw.vertex) },
			{ expected.pixelShaderDescriptor & ~perDraw.pixel,
				perDraw.Native(native.PixelShaderDescriptor, expected.pixelShaderDescriptor) & ~(hairProjection | perDraw.pixel) },
			{ extra, native.ExtraShaderDescriptor & lightingExtraBits },
			{ expected.extraFeatureDescriptor, native.ExtraFeatureDescriptor },
		} };
		++permutationChecks;
		bool differs = false;
		for (std::uint32_t i = 0; i < fields.size(); ++i) {
			const std::uint32_t bits = fields[i].first ^ fields[i].second;
			if (bits) {
				differs = true;
				++permutationDiffs[(static_cast<std::uint64_t>(i) << 32) | bits];
			}
		}
		if (differs) {
			++permutationMismatches;
			NoteMismatch(fmt::format("{} permutation buffer: DCLF {:08X} {:08X} {:08X} {:08X}, native {:08X} {:08X} {:08X} {:08X}", Describe(a_geometry),
				fields[0].first, fields[1].first, fields[2].first, fields[3].first, fields[0].second, fields[1].second, fields[2].second, fields[3].second));
		}
	}

	void CaptureParity::CompareSkin(ID3D11DeviceContext* a_context, const RE::BSGeometry* a_geometry, std::uint32_t a_objectIndex)
	{
		if (!Integrations::AdvancedSkin::Loaded())
			return;
		const auto& tables = Scene::FrameView::Get().GetTables();
		auto note = [](void* a_owner, const RE::BSGeometry* a_note, std::string a_message) {
			static_cast<CaptureParity*>(a_owner)->NoteMismatch(fmt::format("{} {}", Describe(a_note), a_message));
		};

		// t71 and t74, bound by State::Draw from the SetupMaterial hook's pending binding: Integrations/AdvancedSkin's.
		skinTextures.Compare(a_context, a_geometry, a_objectIndex, tables, note, this);

		// b7 and the actor ownership: Integrations/AdvancedSkin's.
		skinWetness.Compare(a_geometry, a_objectIndex, tables, FrameValues::Get().ShadingIfDone(true), note, this);
	}

	void CaptureParity::CompareFeatureBindings(ID3D11DeviceContext* a_context)
	{
		FeatureBindings current;
		a_context->VSGetConstantBuffers(0, kConstantBufferSlots, current.vsBuffers.data());
		a_context->PSGetConstantBuffers(0, kConstantBufferSlots, current.psBuffers.data());
		a_context->VSGetShaderResources(0, kResourceSlots, current.vsResources.data());
		a_context->PSGetShaderResources(0, kResourceSlots, current.psResources.data());

		// Skipped: per-material feature textures (the material contributors' registers, Extensions::FeatureTextureRegister; the
		// integrations' texture parities check them) and Skin's per-geometry t75 (SKIN only). By register: t0-t63, t64-t127. Resolved at
		// install, so built once at the first check.
		static const std::array<std::uint64_t, 2> kSkinResources = [] {
			std::array<std::uint64_t, 2> bits{ 0, 1ull << (75 - 64) };
			for (std::uint32_t f = 0; f < Extensions::FeatureTextureCount(); ++f) {
				const std::uint32_t t = Extensions::FeatureTextureRegister(f);
				bits[t / 64] |= 1ull << (t % 64);
			}
			return bits;
		}();
		auto compare = [&](const auto& a_current, const auto& a_baseline, std::uint32_t a_first, std::uint64_t a_skip, const char* a_what,
						   std::uint64_t a_skipHigh = 0) {
			for (std::uint32_t slot = a_first; slot < a_current.size(); ++slot) {
				if (slot < 64 ? ((a_skip >> slot) & 1) : ((a_skipHigh >> (slot - 64)) & 1))
					continue;
				if (a_current[slot] != a_baseline[slot])
					++bindingChanges[fmt::format("{} {}", a_what, slot)];
			}
		};
		for (std::uint32_t bits = pendingRewrites; bits; bits &= bits - 1) {
			const std::uint32_t bit = std::countr_zero(bits);
			++bindingChanges[fmt::format("{} b{} rewritten", bit < 16 ? "VS" : "PS", bit % 16)];
		}
		pendingRewrites = 0;
		if (!baselineValid) {
			baseline = current;
			baselineValid = true;
		} else {
			// PS t0-t15 are the engine's (covered by the material and technique checks; unsampled slots keep leftovers).
			compare(current.vsBuffers, baseline.vsBuffers, kFirstFeatureConstantBuffer, PerDrawVSBuffers(), "VS b");
			compare(current.psBuffers, baseline.psBuffers, kFirstFeatureConstantBuffer, PerDrawPSBuffers(), "PS b");
			compare(current.vsResources, baseline.vsResources, 0, 0, "VS t");
			compare(current.psResources, baseline.psResources, 0, 0xffffull | kSkinResources[0], "PS t", kSkinResources[1]);
		}

		auto release = [](auto& a_array) {
			for (auto* object : a_array) {
				if (object)
					object->Release();
			}
		};
		release(current.vsBuffers);
		release(current.psBuffers);
		release(current.vsResources);
		release(current.psResources);
	}

	namespace
	{
		struct DrawIndexedHook
		{
			static void thunk(ID3D11DeviceContext* a_this, UINT a_indexCount, UINT a_startIndex, INT a_baseVertex)
			{
				CaptureParity::Get().OnDrawIndexed(a_this, a_indexCount, a_startIndex, a_baseVertex);
				func(a_this, a_indexCount, a_startIndex, a_baseVertex);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	void CaptureParity::InstallDrawHook()
	{
		if (drawHookInstalled || !globals::d3d::context)
			return;
		stl::detour_vfunc<12, DrawIndexedHook>(globals::d3d::context);
		drawHookInstalled = true;
	}

	void CaptureParity::OnDrawIndexed(ID3D11DeviceContext* a_context, UINT a_indexCount, UINT a_startIndex, INT a_baseVertex)
	{
		const std::int32_t index = pendingObject;
		const std::uint32_t partition = pendingSlotCursor++;
		if (pendingSlotCursor >= pendingSlotCount)
			pendingObject = -1;
		if (index < 0 || partition >= pendingSlotCount)
			return;

		const auto& tables = Scene::FrameView::Get().GetTables();
		if (static_cast<std::size_t>(index) >= tables.objects.size() || pendingSlots[partition] >= tables.geometries.size())
			return;
		const auto& object = tables.objects[index];
		const auto& geometryRecord = tables.geometries[pendingSlots[partition]];
		const auto* geometry = tables.objectGeometry[index];
		++drawsChecked;

		ID3D11Buffer* indexBuffer = nullptr;
		DXGI_FORMAT indexFormat = DXGI_FORMAT_UNKNOWN;
		UINT indexOffset = 0;
		a_context->IAGetIndexBuffer(&indexBuffer, &indexFormat, &indexOffset);
		ID3D11Buffer* vertexBuffer = nullptr;
		UINT stride = 0, vertexOffset = 0;
		a_context->IAGetVertexBuffers(0, 1, &vertexBuffer, &stride, &vertexOffset);
		const bool argsOk = a_indexCount == geometryRecord.indexCount && a_startIndex == geometryRecord.firstIndex && a_baseVertex == 0;
		const bool buffersOk = indexBuffer == geometryRecord.indexBuffer && indexFormat == DXGI_FORMAT_R16_UINT && indexOffset == 0 &&
		                       vertexBuffer == geometryRecord.vertexBuffer && stride == geometryRecord.vertexStride && vertexOffset == 0;
		if (!argsOk || !buffersOk) {
			++drawMismatches;
			NoteMismatch(fmt::format("{} draw: DCLF {} indices from {} (IB {} VB {} stride {}), native {} from {} base {} (IB {} fmt {} +{}, VB {} stride {} +{})",
				Describe(geometry), geometryRecord.indexCount, geometryRecord.firstIndex, fmt::ptr(geometryRecord.indexBuffer), fmt::ptr(geometryRecord.vertexBuffer),
				geometryRecord.vertexStride, a_indexCount, a_startIndex, a_baseVertex, fmt::ptr(indexBuffer), static_cast<int>(indexFormat), indexOffset,
				fmt::ptr(vertexBuffer), stride, vertexOffset));
		}
		if (indexBuffer)
			indexBuffer->Release();
		if (vertexBuffer)
			vertexBuffer->Release();

		// State::Draw has uploaded the permutation buffer for this draw by now.
		if (partition == 0)
			ComparePermutation(geometry, static_cast<std::uint32_t>(index));

		// Skinned: the bone setter's b10 (current) and b9 (previous) against the frame's palettes (sampled after the same update);
		// skipped while the producer runs.
		const auto* framePalettes = FrameValues::Get().PalettesIfDone();
		if ((object.flags & kObjectPaletteBlock) && static_cast<std::size_t>(index) < tables.boneOffset.size() && tables.boneRows[index] && framePalettes) {
			ID3D11Buffer* boneBuffers[2] = {};
			a_context->VSGetConstantBuffers(9, 2, boneBuffers);
			const std::uint32_t rows = tables.boneRows[index];
			const auto palette = PaletteRowsOf(tables.boneOffset[index], rows);
			auto check = [&](ID3D11Buffer* a_buffer, std::uint32_t a_first, const char* a_what) {
				++boneChecks;
				const auto mapped = a_buffer ? lastMappedAny.find(a_buffer) : lastMappedAny.end();
				if (mapped == lastMappedAny.end() || !mapped->second || std::size_t(a_first) + rows > framePalettes->size()) {
					++boneMismatches;
					NoteMismatch(fmt::format("{} {}: no mapped palette to compare (buffer {}, {} rows)", Describe(geometry), a_what, fmt::ptr(a_buffer), rows));
					return;
				}
				const auto* ours = (*framePalettes)[a_first].v;
				if (std::memcmp(mapped->second, ours, std::size_t(rows) * 16) != 0) {
					++boneMismatches;
					const auto* native = static_cast<const float*>(mapped->second);
					NoteMismatch(fmt::format("{} {}: row 0 DCLF ({} {} {} {}), native ({} {} {} {}), {} rows", Describe(geometry), a_what,
						ours[0], ours[1], ours[2], ours[3], native[0], native[1], native[2], native[3], rows));
				}
			};
			check(boneBuffers[1], palette.current, "Bones (b10)");
			check(boneBuffers[0], palette.previous, "PreviousBones (b9)");
			for (auto* buffer : boneBuffers) {
				if (buffer)
					buffer->Release();
			}
		}

		// PS b3: Integrations/LightLimitFix's.
		lightData.Compare(geometry, static_cast<std::uint32_t>(index), tables,
			[](void* a_owner, const RE::BSGeometry* a_note, std::string a_message) {
				static_cast<CaptureParity*>(a_owner)->NoteMismatch(fmt::format("{} {}", Describe(a_note), a_message));
			},
			this);

		CompareSkin(a_context, geometry, static_cast<std::uint32_t>(index));
		// t80-t91: Integrations/TruePBR's.
		truePBRTextures.Compare(a_context, geometry, static_cast<std::uint32_t>(index), tables,
			[](void* a_owner, const RE::BSGeometry* a_note, std::string a_message) {
				static_cast<CaptureParity*>(a_owner)->NoteMismatch(fmt::format("{} {}", Describe(a_note), a_message));
			},
			this);

		CompareFeatureBindings(a_context);

		// Render state, learned per property-derived key.
		auto& data = const_cast<RE::BSGeometry*>(geometry)->GetGeometryRuntimeData();
		const auto* property = data.shaderProperty.get();
		const auto* alpha = data.alphaProperty.get();
		RenderStateKey key{};
		key.twoSided = property && property->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kTwoSided);
		key.zTest = property && property->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kZBufferTest);
		key.zWrite = property && property->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kZBufferWrite);
		key.alphaTest = alpha && alpha->GetAlphaTesting();
		key.alphaThreshold = alpha ? alpha->alphaThreshold : 0;
		const auto& state = globals::game::shadowState->GetRuntimeData();
		RenderStateValue value{ state.rasterStateCullMode, static_cast<std::uint32_t>(state.depthStencilDepthMode), state.depthStencilStencilMode,
			state.alphaBlendMode, state.rasterStateDepthBiasMode, state.alphaTestEnabled, state.alphaTestRef };
		auto& seen = renderStates[key];
		if (std::find(seen.begin(), seen.end(), value) == seen.end())
			seen.push_back(value);
	}

	void CaptureParity::OnFaceLightingDraw(const RE::BSRenderPass* a_pass, std::uint32_t a_renderFlags)
	{
		pendingObject = -1;
		if (!a_pass || !a_pass->geometry)
			return;
		++faceDraws;
		auto& store = Scene::Coordinator::Get();
		const auto* geometry = a_pass->geometry;
		const std::int32_t index = store.FindObject(geometry);
		if (index < 0) {
			++faceUntracked;
			return;
		}
		const auto& tables = Scene::FrameView::Get().GetTables();
		// The lookup and the tables are not one snapshot: skip an index past the tables.
		if (static_cast<std::size_t>(index) >= tables.objects.size())
			return;
		const auto& object = tables.objects[index];
		if (object.flags & kObjectNoBindings)
			return;
		++faceChecked;
		labelPrefix = "face ";
		compareContext = fmt::format(" (face pass {:08X}, DCLF's main {:08X})", PassDescriptorOf(a_pass->passEnum), tables.pipelines[object.pipelineIndex].passDescriptor);
		bool ok = CompareMaterial(geometry, object.materialIndex);
		ok &= CompareTechnique(geometry, static_cast<std::uint32_t>(index));
		ok &= CompareGeometry(geometry, static_cast<std::uint32_t>(index), a_renderFlags);
		labelPrefix.clear();
		compareContext.clear();
		if (!ok)
			++faceMismatched;
	}

	void CaptureParity::OnNativeLightingDraw(const RE::BSRenderPass* a_pass, std::uint32_t a_renderFlags)
	{
		pendingObject = -1;
		lastNote.clear();
		compareContext.clear();
		if (!globals::deferred->deferredPass || !a_pass || !a_pass->geometry)
			return;
		InstallDrawHook();
		++nativeDraws;
		renderFlagsSeen.insert(a_renderFlags);

		auto& store = Scene::Coordinator::Get();
		auto* geometry = a_pass->geometry;
		// A multi-index shape's layer pass (accumulation hint 12, the second index list) is its layer object's.
		const bool layerPass = a_pass->accumulationHint == kLayerHint;
		const std::int32_t index = layerPass ? store.FindLayerObject(geometry) : store.FindObject(geometry);
		if (const_cast<RE::BSGeometry*>(geometry)->GetType().get() == RE::BSGeometry::Type::kMultiIndexTriShape) {
			auto& entry = multiIndexDraws[fmt::format("hint {} technique {}", a_pass->accumulationHint, (PassDescriptorOf(a_pass->passEnum) >> 24) & 0x3f)];
			if (entry.first++ == 0)
				entry.second = fmt::format("{} property {}", Describe(geometry), fmt::ptr(a_pass->shaderProperty));
		}
		if (layerPass && layerDraws++ == 0)
			firstLayerDraw = fmt::format("{} pass {:08X}, object {}", Describe(geometry), a_pass->passEnum, index);

		if (index < 0) {
			if (store.IsTracked(geometry)) {
				// Tracked but left native this frame. Hidden would mean the visibility check disagrees with the game (fading
				// objects are native by design).
				const Ineligible reason = store.Classify(geometry);
				if (reason == Ineligible::Hidden) {
					++mismatchedDraws;
					NoteMismatch(fmt::format("{} drawn natively but excluded as {}", Describe(geometry), kIneligibleNames[static_cast<std::size_t>(reason)]));
				}
				++notInTables;
				if (!notInTablesBy[static_cast<std::uint8_t>(reason)]++)
					notInTablesFirst[static_cast<std::uint8_t>(reason)] = Describe(geometry);
			} else if (Eligibility::Static(SceneCapture::LiveLeaf(*geometry).view, nullptr) == Ineligible::None) {
				if (auto* category = store.CategoryNodeOf(geometry)) {
					++untrackedEligible;  // followed below and reported with its parents and what tracked it (ResolveUntracked)
					const std::uint32_t frame = Scene::FrameView::Get().GetFrame();
					auto [it, inserted] = untracked.try_emplace(geometry);
					if (inserted) {
						it->second.firstFrame = frame;
						std::string chain;
						for (const RE::NiAVObject* node = geometry->parent; node; node = node->parent) {
							const auto* rtti = node->GetRTTI();
							const char* name = node->name.c_str();
							chain += fmt::format(" < {}'{}'", rtti && rtti->name ? rtti->name : "?", name ? name : "");
							if (node == category) {
								std::uint32_t found = 0;
								std::uint8_t cause = 0;
								if (store.GetCategoryInfo(category, found, cause))
									chain += fmt::format("[CATEGORY found frame {} ({} before), cause {}]", found, frame - found, cause);
								else
									chain += "[CATEGORY]";
							}
							chain += DescribeRole(node);
						}
						it->second.chain = std::move(chain);
					}
					if (inserted || it->second.lastFrame != frame)
						++it->second.frames;
					it->second.lastFrame = frame;
				} else {
					// Statically eligible content outside the tracked category nodes: where does it live?
					++outsideCategories;
					std::string chain;
					for (const RE::NiAVObject* node = geometry->parent; node; node = node->parent) {
						const auto* rtti = node->GetRTTI();
						const char* name = node->name.c_str();
						const auto* ref = node->GetUserData();
						const auto* base = ref ? ref->GetBaseObject() : nullptr;
						chain += fmt::format(" < {}'{}'", rtti && rtti->name ? rtti->name : "?", name ? name : "");
						if (ref)
							chain += fmt::format("#{:08X}/{}", ref->GetFormID(), base ? static_cast<int>(base->GetFormType()) : -1);
						chain += DescribeRole(node);
					}
					// Which portal graph list holds the root (BSPortalGraph +0x58 alwaysRenderChildren, +0x78, +0x90, +0xA8).
					const RE::NiAVObject* root = geometry;
					while (root->parent)
						root = root->parent;
					std::string where;
					const auto& shaderState = RE::BSShaderManager::State::GetSingleton();
					for (std::uint32_t sg = 0; sg < std::size(shaderState.shadowSceneNode); ++sg) {
						const auto* sceneNode = shaderState.shadowSceneNode[sg];
						const auto* graph = sceneNode ? sceneNode->GetRuntimeData().portalGraph : nullptr;
						if (!graph)
							continue;
						auto in = [&](const auto& a_list) {
							for (const auto& entry : a_list)
								if (entry.get() == root)
									return true;
							return false;
						};
						if (in(graph->alwaysRenderChildren))
							where += fmt::format(" graph{}.alwaysRender", sg);
						if (in(graph->unk78))
							where += fmt::format(" graph{}.78", sg);
						if (in(graph->unk90))
							where += fmt::format(" graph{}.90", sg);
						if (in(graph->unkA8))
							where += fmt::format(" graph{}.A8", sg);
					}
					++outsideRoots[fmt::format("{}{}", root->parent ? "parented" : "parentless", where.empty() ? " in no graph list" : where)];
					if (outsideChains.size() < 8)
						outsideChains.insert(chain);
				}
			} else {
				// Untracked and ineligible: counted by verdict, first of each named.
				const auto reason = static_cast<std::uint8_t>(Eligibility::Static(SceneCapture::LiveLeaf(*geometry).view, nullptr));
				if (!untrackedBy[reason]++)
					untrackedFirst[reason] = fmt::format("{} hint {}", Describe(geometry), a_pass->accumulationHint);
			}
			return;
		}

		const auto& tables = Scene::FrameView::Get().GetTables();
		if (static_cast<std::size_t>(index) >= tables.objects.size())
			return;  // the lookup is past the tables (not one snapshot with them)
		++checkedDraws;
		const auto& object = tables.objects[index];
		if (object.flags & kObjectNoBindings)
			return;  // a culling candidate only: no material or pipeline entry to compare against
		const auto& key = tables.pipelines[object.pipelineIndex];
		const auto* state = globals::state;

		bool mismatch = false;
		const auto* accumulated = layerPass ? store.FindAccumulatedLayerPass(geometry) : store.FindAccumulatedPass(geometry);
		// DCLF draws an alpha-test list's pass with DoAlphaTest regardless (DrawnPassDescriptor): normalise the native side.
		const std::uint32_t list = accumulated ? accumulated->subPass : 0u;
		const std::uint32_t nativeDescriptor = DrawnPassDescriptor(PassDescriptorOf(a_pass->passEnum), list);
		const PerDrawBits drawBits = PerDrawBitsOf(tables, static_cast<std::uint32_t>(index), key.passDescriptor);
		const std::uint32_t passDiffers = key.passDescriptor ^ drawBits.Native(nativeDescriptor, key.passDescriptor);
		// A native pass differing only in PerDrawBits is another technique's (no shadow mask, LOD-faded features dropped): only
		// descriptors are compared; the object is compared whole on frames where the bits match.
		const bool otherTechnique = passDiffers && !(passDiffers & ~drawBits.pass);
		if (otherTechnique)
			++perDrawNormalised;
		if (passDiffers & ~drawBits.pass) {
			mismatch = true;
			NoteMismatch(fmt::format("{} pass descriptor: DCLF {:08X}, native {:08X} (flags {:016X}; accumulated technique {:08X} list {} passEnum then {:08X})",
				Describe(geometry), key.passDescriptor, PassDescriptorOf(a_pass->passEnum), a_pass->shaderProperty ? a_pass->shaderProperty->flags.underlying() : 0ull,
				accumulated ? accumulated->technique : 0u, accumulated ? accumulated->subPass : 0u, accumulated ? PassDescriptorOf(accumulated->passEnum) : 0u));
		}
		// Shader descriptors likewise, ignoring the alpha-test bit and hair's ProjectedUV bit (HairProjection).
		const std::uint32_t ignored = DrawnPassDescriptor(0, list) | (HairProjection(PassDescriptorOf(a_pass->passEnum)) ? kPassProjectedUV : 0u);
		const PerDrawBits& perDraw = drawBits;
		if (((key.vertexDescriptor ^ perDraw.Native(state->modifiedVertexDescriptor, key.vertexDescriptor)) & ~(ignored | perDraw.vertex)) != 0 ||
			((key.pixelDescriptor ^ perDraw.Native(state->modifiedPixelDescriptor, key.pixelDescriptor)) & ~(ignored | perDraw.pixel)) != 0) {
			mismatch = true;
			NoteMismatch(fmt::format("{} descriptors: DCLF VS {:08X} PS {:08X}, native VS {:08X} PS {:08X} (pass {:08X}, flags {:016X})",
				Describe(geometry), key.vertexDescriptor, key.pixelDescriptor, state->modifiedVertexDescriptor, state->modifiedPixelDescriptor,
				a_pass->passEnum, a_pass->shaderProperty ? a_pass->shaderProperty->flags.underlying() : 0ull));
		}
		if (const auto* frameRows = FrameValues::Get().RowsIfDone(); frameRows && static_cast<std::size_t>(index) < frameRows->size() &&
																	 !SameTransform((*frameRows)[index].world, geometry->world)) {
			mismatch = true;
			NoteMismatch(fmt::format("{} world transform differs from the frame's placement row", Describe(geometry)));
		}
		if (!mismatch && !otherTechnique && !CompareMaterial(geometry, object.materialIndex)) {
			mismatch = true;
			++materialMismatches;
			const auto* drawn = a_pass->shaderProperty ? a_pass->shaderProperty->material : nullptr;
			const auto* recorded = object.materialIndex < tables.materialSlotKey.size() ? tables.materialSlotKey[object.materialIndex].first : nullptr;
			const std::uint32_t frame = Scene::FrameView::Get().GetFrame();
			auto& follow = materialMismatchFollow[drawn];
			if (follow.frames == 0)
				follow.firstFrame = frame;
			if (follow.frames == 0 || follow.lastFrame != frame)
				++follow.frames;
			follow.lastFrame = frame;
			follow.keyDiffers |= drawn != recorded;
			follow.writtenBefore |= writtenMaterials.contains(drawn);
		}
		if (!mismatch && !otherTechnique && !CompareGeometry(geometry, static_cast<std::uint32_t>(index), a_renderFlags)) {
			mismatch = true;
			++geometryMismatches;
		}
		if (!mismatch && !otherTechnique && !CompareTechnique(geometry, static_cast<std::uint32_t>(index))) {
			mismatch = true;
			++techniqueMismatches;
		}
		if (mismatch)
			++mismatchedDraws;
		++checkedByTechnique[(key.passDescriptor >> 24) & 0x3f];
		mismatchedByTechnique[(key.passDescriptor >> 24) & 0x3f] += mismatch ? 1 : 0;
		if (mismatch && firstByTechnique[(key.passDescriptor >> 24) & 0x3f].empty())
			firstByTechnique[(key.passDescriptor >> 24) & 0x3f] = lastNote.empty() ? fmt::format("{} (no note)", Describe(geometry)) : lastNote;
		lastNote.clear();
		pendingObject = index;
		// What its DrawIndexed calls should bind: the geometry, or each partition the pass's LODMode draws.
		pendingSlotCount = pendingSlotCursor = 0;
		const auto* skin = geometry->GetGeometryRuntimeData().skinInstance.get();
		const std::uint32_t partitions = skin && skin->skinPartition && skin->skinPartition->numPartitions > 1 ?
		                                     SkinPartitions::Mask(*skin, SceneCapture::LodRowOf(*a_pass)) :
		                                     0u;
		if (skin && skin->skinPartition && skin->skinPartition->numPartitions > 1 && !partitions) {
			pendingObject = -1;  // the engine draws no partition of it with this pass
			return;
		}
		std::uint32_t slot = tables.objects[index].geometryIndex;
		// Object LOD's chain (kPartitionChain): every link.
		if (partitions & kPartitionChain) {
			for (std::uint32_t i = 0; i < (partitions & kPartitionChainCount) && i < pendingSlots.size() && slot < tables.geometries.size(); ++i) {
				pendingSlots[pendingSlotCount++] = slot;
				slot = tables.geometries[slot].nextPartition;
			}
			if (!pendingSlotCount)
				pendingObject = -1;
			return;
		}
		for (std::uint32_t i = 0; i < pendingSlots.size() && slot < tables.geometries.size(); ++i) {
			if (partitions == 0 || ((partitions >> i) & 1))
				pendingSlots[pendingSlotCount++] = slot;
			if ((partitions >> (i + 1)) == 0)
				break;
			slot = tables.geometries[slot].nextPartition;
		}
		if (!pendingSlotCount)
			pendingObject = -1;
	}

	void CaptureParity::ResolveUntracked(std::uint32_t a_frame)
	{
		constexpr std::uint32_t kStuckFrames = 60;
		static constexpr const char* kSources[] = { "attach event", "category appeared", "rescan" };
		auto& store = Scene::Coordinator::Get();
		std::erase_if(untracked, [&](auto& a_entry) {
			auto& [geometry, entry] = a_entry;
			std::uint32_t frame = 0;
			Scene::Coordinator::TrackSource source{};
			if (store.GetTrackInfo(geometry, frame, source)) {
				++untrackedResolved[fmt::format("{}, tracked {} frames after first drawn untracked, drawn untracked on {} frames",
					kSources[static_cast<std::size_t>(source)], static_cast<std::int32_t>(frame - entry.firstFrame), entry.frames)];
				return true;
			}
			if (a_frame - entry.lastFrame > kStuckFrames) {
				if (untrackedStuck.size() < 16)
					untrackedStuck.push_back(fmt::format("drawn untracked on {} frames ({}..{}), never tracked:{}", entry.frames, entry.firstFrame,
						entry.lastFrame, entry.chain));
				return true;
			}
			return false;
		});
	}

	void CaptureParity::Report(std::uint32_t a_frame, std::uint32_t a_interval)
	{
		// Called at the start of every main pass: a new baseline.
		baselineValid = false;
		pendingRewrites = 0;
		ResolveUntracked(Scene::Coordinator::Get().GetFrame());
		{
			// Resolve mismatched materials a frame on (this frame's drain ran in the accumulate phase).
			const std::uint32_t frame = Scene::FrameView::Get().GetFrame();
			std::erase_if(materialMismatchFollow, [&](const auto& a_entry) {
				const auto& [material, follow] = a_entry;
				if (follow.lastFrame >= frame)
					return false;
				const bool written = writtenMaterials.contains(material);
				if (!written && frame - follow.lastFrame < 4)
					return false;
				++materialMismatchResolved[fmt::format("{} {}mismatched on {} frames (first {}), {}{}", fmt::ptr(material), follow.keyDiffers ? "record keyed by another material, " : "",
					follow.frames, follow.firstFrame, written ? fmt::format("written after its last mismatch (drained {} frames later)", frame - follow.lastFrame) : "no write event within 3 frames",
					follow.writtenBefore ? ", also written before a mismatching draw" : "")];
				return true;
			});
		}
		if (a_interval == 0 || (a_frame % a_interval) != 0)
			return;

		{
			std::size_t pendingStuck = 0;
			for (const auto& [geometry, entry] : untracked)
				pendingStuck += entry.frames > 1 ? 1 : 0;
			logger::info("[DCLF] untracked eligible geometry: {} still followed ({} drawn untracked on more than one frame)", untracked.size(), pendingStuck);
			for (const auto& [what, count] : untrackedResolved)
				logger::info("[DCLF]   {} geometries: {}", count, what);
			for (const auto& chain : untrackedStuck)
				logger::info("[DCLF]   {}", chain);
			std::size_t shown = 0;
			for (const auto& [geometry, entry] : untracked) {
				if (entry.frames > 1 && shown++ < 4)
					logger::info("[DCLF]   pending, drawn untracked on {} frames ({}..{}):{}", entry.frames, entry.firstFrame, entry.lastFrame, entry.chain);
			}
			untrackedResolved.clear();
			untrackedStuck.clear();
			for (const auto& [what, count] : materialMismatchResolved)
				logger::info("[DCLF] mismatched materials: {} {}", count, what);
			materialMismatchResolved.clear();
		}

		const auto& stats = Scene::Coordinator::Get().GetStats();
		const bool ok = mismatchedDraws == 0 && untrackedEligible == 0 && drawMismatches == 0 && permutationMismatches == 0 && lightData.Ok() &&
		                skinTextures.Ok() && skinWetness.Ok() && truePBRTextures.Ok();
		std::string flags;
		for (auto value : renderFlagsSeen)
			flags += fmt::format(" {:X}", value);
		const auto* player = RE::PlayerCharacter::GetSingleton();
		const auto* cell = player ? player->GetParentCell() : nullptr;
		const char* cellName = cell ? cell->GetName() : nullptr;
		logger::info("[DCLF] location: cell {:08X} '{}' ({})", cell ? cell->GetFormID() : 0u, cellName ? cellName : "",
			cell && cell->IsInteriorCell() ? "interior" : "exterior");
		logger::info("[DCLF] capture parity {}: {} native main-pass lighting draws, {} checked against the tables, {} mismatched ({} material, {} per-geometry, {} technique), {} untracked eligible, {} tracked but excluded, {} native-only passes of DCLF objects, {} whose pass differs only in the bits the draw decides (descriptors compared alone); tables hold {} objects / {} geometries / {} pipelines ({} with shadow mask) / {} materials from {} tracked; render flags seen:{}",
			ok ? "OK" : "MISMATCH", nativeDraws, checkedDraws, mismatchedDraws, materialMismatches, geometryMismatches, techniqueMismatches, untrackedEligible,
			notInTables, nativeOnlyPasses, perDrawNormalised, stats.objects, stats.geometries, stats.pipelines, stats.shadowMaskPipelines, stats.materials, stats.tracked, flags);
		{
			std::string byTechnique;
			for (std::uint32_t t = 0; t < checkedByTechnique.size(); ++t)
				if (checkedByTechnique[t])
					byTechnique += fmt::format(" {} {}/{}", LightingTechniqueName(t), mismatchedByTechnique[t], checkedByTechnique[t]);
			logger::info("[DCLF] capture parity by technique (mismatched/checked):{}", byTechnique.empty() ? std::string(" none") : byTechnique);
			for (std::uint32_t t = 0; t < firstByTechnique.size(); ++t)
				if (!firstByTechnique[t].empty())
					logger::info("[DCLF]   first {} mismatch: {}", LightingTechniqueName(t), std::exchange(firstByTechnique[t], {}));
			checkedByTechnique.fill(0);
			mismatchedByTechnique.fill(0);
		}
		if (notInTables) {
			std::string byReason;
			for (const auto& [reason, count] : notInTablesBy)
				byReason += fmt::format("{}{} {} (first {})", byReason.empty() ? "" : ", ", kIneligibleNames[reason], count, notInTablesFirst[reason]);
			logger::info("[DCLF] capture parity: tracked but excluded, by the verdict now: {}", byReason);
		}
		notInTablesBy.clear();
		notInTablesFirst.clear();
		if (!untrackedBy.empty()) {
			std::string byReason;
			for (const auto& [reason, count] : untrackedBy)
				byReason += fmt::format("{}{} {} (first {})", byReason.empty() ? "" : ", ", kIneligibleNames[reason], count, untrackedFirst[reason]);
			logger::info("[DCLF] capture parity: untracked and ineligible, by the verdict: {}", byReason);
		}
		untrackedBy.clear();
		untrackedFirst.clear();
		if (layerDraws)
			logger::info("[DCLF] capture parity: {} native layer draws (hint 12), first {}", layerDraws, firstLayerDraw);
		layerDraws = 0;
		firstLayerDraw.clear();
		for (const auto& [what, entry] : multiIndexDraws)
			logger::info("[DCLF] capture parity: native multi-index draws, {}: {} (first {})", what, entry.first, entry.second);
		multiIndexDraws.clear();
		if (boneChecks)
			logger::info("[DCLF] bone palette parity {}: {} palettes checked, {} differ", boneMismatches == 0 ? "OK" : "MISMATCH", boneChecks, boneMismatches);
		boneChecks = boneMismatches = 0;
		logger::info("[DCLF] draw parity {}: {} draws checked, {} with different arguments or bound buffers", drawMismatches == 0 ? "OK" : "MISMATCH",
			drawsChecked, drawMismatches);
		lightData.Report();
		logger::info("[DCLF] skin parity {}: {} draws' t71/t74 checked, {} differ; {} draws' wetness checked ({} wet), {} differ, {} with a different actor ownership",
			skinTextures.Ok() && skinWetness.Ok() ? "OK" : "MISMATCH", skinTextures.checks, skinTextures.mismatches, skinWetness.checks,
			skinWetness.wetDraws, skinWetness.mismatches, skinWetness.ownerMismatches);
		truePBRTextures.Report();
		std::string diffs;
		static constexpr const char* kFieldNames[] = { "vertex", "pixel", "extra", "feature" };
		for (const auto& [key, count] : permutationDiffs)
			diffs += fmt::format(" {} {:X} x{}", kFieldNames[key >> 32], static_cast<std::uint32_t>(key), count);
		logger::info("[DCLF] permutation parity {}: {} draws checked, {} differ; material textures with inherited filter modes: {};{}",
			permutationMismatches == 0 ? "OK" : "MISMATCH", permutationChecks, permutationMismatches, inheritedFilters, diffs.empty() ? " no differing bits" : diffs);
		if (faceDraws)
			logger::info("[DCLF] face capture parity: {} reflection face Lighting draws, {} of objects in the tables checked against their main rows, {} differ, {} untracked",
				faceDraws, faceChecked, faceMismatched, faceUntracked);
		faceDraws = faceChecked = faceMismatched = faceUntracked = 0;
		std::string blocks;
		for (const auto& [what, counts] : blockCoverage)
			blocks += fmt::format("{}{}: {} compared, {} not", blocks.empty() ? "" : ", ", what, counts.first, counts.second);
		logger::info("[DCLF] constant block coverage: {}", blocks.empty() ? "nothing compared" : blocks);
		for (const auto& [key, values] : renderStates) {
			std::string text;
			for (const auto& v : values)
				text += fmt::format(" [cull {} depth {} stencil {} blend {} bias {} alphaTest {} ref {}]", v.cull, v.depth, v.stencil, v.blend, v.depthBias,
					v.alphaTestEnabled, v.alphaTestRef);
			logger::info("[DCLF] render state{} twoSided {} zTest {} zWrite {} alphaTest {} threshold {} ->{}", values.size() > 1 ? " (VARIES)" : "",
				key.twoSided, key.zTest, key.zWrite, key.alphaTest, key.alphaThreshold, text);
		}
		std::string changes;
		for (const auto& [name, count] : bindingChanges)
			changes += fmt::format(" [{} x{}]", name, count);
		logger::info("[DCLF] feature binding parity {}: bindings that differ within a frame's eligible draws:{}", bindingChanges.empty() ? "OK" : "MISMATCH",
			changes.empty() ? " none" : changes);
		logger::info("[DCLF] statically eligible geometry drawn outside the tracked category nodes: {}", outsideCategories);
		for (const auto& chain : outsideChains)
			logger::info("[DCLF]   parents:{}", chain);
		for (const auto& [where, count] : outsideRoots)
			logger::info("[DCLF]   outside roots: {}: {} draws", where, count);
		outsideRoots.clear();
		std::string missing;
		for (const auto& [name, count] : unevaluated)
			missing += fmt::format(" [{} x{}]", name, count);
		{
			std::string byVariable;
			std::vector<std::pair<std::string, std::uint32_t>> sorted(mismatchByVariable.begin(), mismatchByVariable.end());
			std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
			for (const auto& [name, count] : sorted)
				byVariable += fmt::format(" [{} x{}]", name, count);
			if (!byVariable.empty())
				logger::info("[DCLF] parity mismatches by variable:{}", byVariable);
			mismatchByVariable.clear();
		}
		logger::info("[DCLF] variables the native shaders have that DCLF leaves unwritten:{}", missing.empty() ? " none" : missing);
		for (const auto& sample : samples)
			logger::info("[DCLF]   {}", sample);

		if (shadingSinceStart)
			logger::info("[DCLF] capture parity: {} draws' shading changed since the frame's start sampled it (accepted: DCLF draws it a frame later); "
						 "compared with the engine's now", shadingSinceStart);
		shadingSinceStart = 0;
		nativeDraws = checkedDraws = mismatchedDraws = untrackedEligible = notInTables = nativeOnlyPasses = perDrawNormalised = materialMismatches = geometryMismatches = 0;
		drawMismatches = drawsChecked = 0;
		techniqueMismatches = inheritedFilters = permutationChecks = permutationMismatches = 0;
		lightData.Reset();
		skinTextures.Reset();
		skinWetness.Reset();
		truePBRTextures.Reset();
		permutationDiffs.clear();
		unevaluated.clear();
		bindingChanges.clear();
		outsideCategories = 0;
		outsideChains.clear();
		renderStates.clear();
		blockCoverage.clear();
		renderFlagsSeen.clear();
		samples.clear();
	}

	void CaptureParity::DrainWrittenMaterials()
	{
		writtenMaterials.clear();
		MaterialWrites::Drain(writtenMaterials);
	}
}
