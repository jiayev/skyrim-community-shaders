#include "CaptureParity.h"

#include "Switches.h"
#include "LightingConstants.h"
#include "PassCapture.h"

#include <cstring>

#include "Deferred.h"
#include "Features/LightLimitFix.h"
#include "Features/Skin.h"
#include "SceneStore.h"
#include "State.h"

namespace DCLF
{
	namespace
	{
		constexpr std::size_t kMaxSamples = 24;

		// Constant buffers that change between eligible draws by design: Light Limit Fix's StrictLightData
		// (PS b3; per object, ObjectLights, checked on its own), the permutation (b4, checked on its own),
		// the bone palettes (VS b9, b10; SKINNED permutations only), Skin's SkinPerGeometry (PS b7; SKIN
		// only), Linear Lighting's LLPerGeometry (PS b8; per object while it is enabled) and the alpha-test
		// reference (PS b11; per object, ObjectRecord flags).
		constexpr std::uint32_t kPerDrawVSBuffers = (1u << 4) | (1u << 9) | (1u << 10);
		constexpr std::uint32_t kPerDrawPSBuffers = (1u << 3) | (1u << 4) | (1u << 7) | (1u << 8) | (1u << 11);

		std::string Describe(const RE::BSGeometry* a_geometry)
		{
			const char* name = a_geometry->name.c_str();
			auto* ref = a_geometry->GetUserData();
			for (auto* node = a_geometry->parent; !ref && node; node = node->parent)
				ref = node->GetUserData();
			return fmt::format("'{}' ref {:08X}", name ? name : "", ref ? ref->GetFormID() : 0u);
		}

		// Diagnostics: which engine structure a node is (TES roots, a loaded cell's 3D or multibound node).
		// For values the engine updates while the frame renders (EmitColor: time-of-day emittance).
		bool WithinTolerance(const auto& a_native, std::uint32_t a_float, float a_expected)
		{
			float native = 0.0f;
			std::memcpy(&native, &a_native.bytes[a_float * 4], 4);
			return std::abs(native - a_expected) <= 1e-3f * std::max(std::abs(native), std::abs(a_expected));
		}

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

		// World matrix as the PerGeometry constant holds it: translation relative to the camera (posAdjust).
	}

	bool CaptureParity::Enabled()
	{
		static const bool enabled = [] {
			return SwitchEnabled("CS_DCLF_CAPTURE_PARITY");
		}();
		return enabled;
	}

	CaptureParity& CaptureParity::Get()
	{
		static CaptureParity parity;
		return parity;
	}

	void CaptureParity::NoteMismatch(std::string a_message)
	{
		if (samples.size() < kMaxSamples)
			samples.push_back(std::move(a_message));
	}

	void CaptureParity::OnMap(ID3D11Resource* a_resource, void* a_data)
	{
		if (ConstantEvaluator::Evaluating())
			return;
		lastMappedAny[a_resource] = a_data;
		if (baselineValid) {
			// The permutation buffer (b4) changes per draw by design and is checked separately.
			for (std::uint32_t slot = kFirstFeatureConstantBuffer; slot < kConstantBufferSlots; ++slot) {
				if (!((kPerDrawVSBuffers >> slot) & 1) && reinterpret_cast<ID3D11Resource*>(baseline.vsBuffers[slot]) == a_resource)
					pendingRewrites |= 1u << slot;
				if (!((kPerDrawPSBuffers >> slot) & 1) && reinterpret_cast<ID3D11Resource*>(baseline.psBuffers[slot]) == a_resource)
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
		const std::uint8_t* a_nativeTable, std::size_t a_nativeTableSize, const ConstantSnapshot& a_native, ID3D11Resource* a_boundBuffer,
		std::uint32_t a_firstVariable, std::uint64_t a_variables, std::uint64_t a_tolerant)
	{
		// A block is only compared when the buffer the draw binds is the one the Map/Unmap detours
		// snapshotted; otherwise there is nothing to compare against and the draw silently counts as OK.
		// The counts are reported, because a block that is never compared is not a passing check.
		auto& coverage = blockCoverage[a_what];
		if (!a_native.valid || a_boundBuffer != a_native.buffer) {
			++coverage.second;
			return true;
		}
		++coverage.first;

		bool ok = true;
		for (std::uint32_t i = 0; i < a_layout.count && i < a_nativeTableSize; ++i) {
			if (!((a_variables >> i) & 1))
				continue;
			// Offset 0 is also what reflection leaves for variables a permutation lacks, so it only
			// counts for the group's first variable.
			const std::uint32_t nativeOffset = a_nativeTable[i];
			if (nativeOffset == 0 && i != a_firstVariable)
				continue;
			const std::uint32_t ourOffset = a_layout.offset[i];
			bool anyWritten = false;
			for (std::uint32_t c = 0; c < a_layout.size[i]; ++c)
				anyWritten |= a_expected.Written(ourOffset + c);
			if (!anyWritten) {
				++unevaluated[fmt::format("{} {}", a_what, i)];
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
				// A histogram as well as a sample. The sample list is capped, so once a known standing
				// difference fills it nothing else is ever seen - which is exactly what happened when trees
				// were brought in: 83,396 per-geometry mismatches and not one of them in the samples.
				// Two samples per variable: a standing difference in one variable (PS PerMaterial 29) used
				// to fill the capped list on its own, and every other variable's mismatches went unsampled.
				if (++mismatchByVariable[fmt::format("{} {}", a_what, i)] <= 2) {
					float native = 0;
					std::memcpy(&native, &a_native.bytes[(nativeOffset + firstComponent) * 4], 4);
					NoteMismatch(fmt::format("{} {} variable {} component {}: DCLF {}, native {}", Describe(a_geometry), a_what, i, firstComponent,
						a_expected.floats[ourOffset + firstComponent], native));
				}
			}
		}
		return ok;
	}

	bool CaptureParity::CompareMaterial(const RE::BSGeometry* a_geometry, std::uint32_t a_materialIndex)
	{
		const auto& record = SceneStore::Get().GetTables().materials[a_materialIndex];
		auto* vs = *globals::game::currentVertexShader;
		auto* ps = *globals::game::currentPixelShader;
		bool ok = true;
		if (vs)
			ok &= CompareBlock(a_geometry, "VS PerMaterial", record.vs, LightingVSLayout(), vs->constantTable.data(),
				vs->constantTable.size(), Slot(0, kPerMaterial), reinterpret_cast<ID3D11Resource*>(vs->constantBuffers[kPerMaterial].buffer), kVSLeftEyeCenter, kVSGroups[kPerMaterial], 0);
		if (ps)
			ok &= CompareBlock(a_geometry, "PS PerMaterial", record.ps, LightingPSLayout(), ps->constantTable.data(), ps->constantTable.size(), Slot(1, kPerMaterial),
				reinterpret_cast<ID3D11Resource*>(ps->constantBuffers[kPerMaterial].buffer), kPSLODTexParams, kPSGroups[kPerMaterial], 0);

		// Textures and address modes the material binds.
		const auto& state = globals::game::shadowState->GetRuntimeData();
		for (std::uint32_t slot = 0; slot < kPixelTextureSlots; ++slot) {
			if (!((record.textureWritten >> slot) & 1))
				continue;
			const auto* native = reinterpret_cast<const ID3D11ShaderResourceView*>(state.PSTexture[slot]);
			const auto nativeMode = static_cast<std::uint32_t>(state.PSTextureAddressMode[slot].underlying());
			if (native != record.textures[slot] || nativeMode != record.addressModes[slot]) {
				ok = false;
				NoteMismatch(fmt::format("{} texture slot {}: DCLF {} mode {}, native {} mode {}", Describe(a_geometry), slot,
					fmt::ptr(record.textures[slot]), record.addressModes[slot], fmt::ptr(native), nativeMode));
			}
		}
		return ok;
	}

	bool CaptureParity::CompareGeometry(const RE::BSGeometry* a_geometry, std::uint32_t a_objectIndex, std::uint32_t a_renderFlags)
	{
		const auto& tables = SceneStore::Get().GetTables();
		const auto& object = tables.objects[a_objectIndex];
		if ((object.flags & kObjectNoBindings) || !tables.geometryConstantsValid[object.pipelineIndex])
			return true;

		// Expected values: the per-frame block for this pass descriptor with the per-object values on top.
		auto& shadowState = globals::game::shadowState->GetRuntimeData();
		GeometryConstants expected = ObjectGeometryConstants(tables, a_objectIndex, a_renderFlags, shadowState.posAdjust.getEye(), shadowState.previousPosAdjust.getEye());
		const auto& vsLayout = LightingVSLayout();
		const auto& psLayout = LightingPSLayout();
		// WindTimers.y, the previous wind timer, is not compared. SetupGeometry (AE 1414dd040, case 0xc) copies
		// the tree node's current timer over its previous one (+0x168 = +0x164) after every native draw, so
		// the value a native draw sees depends on whether another geometry of the same tree was drawn natively
		// before it that frame - and DCLF does not write the engine's nodes. Lighting.hlsl reads it only into
		// previousInputPosition, which nothing uses: it reaches no pixel, native or DCLF's.
		if (vsLayout.size[kVSWindTimers] > 1)
			expected.vs.floats[vsLayout.offset[kVSWindTimers] + 1] = std::bit_cast<float>(kUnwrittenBits);

		auto* vs = *globals::game::currentVertexShader;
		auto* ps = *globals::game::currentPixelShader;
		bool ok = true;
		// TreeParams and WindTimers are written by SetupGeometry for technique 12 only; for anything else
		// the native buffer holds whatever the last tree left there, and the shader never reads them. They
		// used to compare clean by accident - no eligible object was drawn after a tree in the main range -
		// until decals, which the engine draws after everything, made the leftover visible: 382 mismatches
		// an interval on values nothing consumes.
		// Likewise World and PreviousWorld for a skinned object: SetupGeometry writes them for everything
		// that is not skinned (engine notes), and the SKINNED vertex shader positions from the palette.
		const std::uint64_t vsMask = kVSGroups[kPerGeometry] &
		                             ((object.flags & kObjectTreeAnim) ? ~0ull : ~((1ull << kVSTreeParams) | (1ull << kVSWindTimers))) &
		                             ((object.flags & kObjectSkinned) ? ~((1ull << kVSWorld) | (1ull << kVSPreviousWorld)) : ~0ull);
		if (vs)
			ok &= CompareBlock(a_geometry, "VS PerGeometry", expected.vs, vsLayout, vs->constantTable.data(),
				vs->constantTable.size(), Slot(0, kPerGeometry), reinterpret_cast<ID3D11Resource*>(vs->constantBuffers[kPerGeometry].buffer), kVSWorld, vsMask, 0);
		// A tree whose amplitude differs: say what the derivation saw against what the node holds now.
		if (vs && !ok && (object.flags & kObjectTreeAnim) && a_objectIndex < tables.treeAnim.size() && treeSamples < 4) {
			const auto& tree = tables.treeAnim[a_objectIndex];
			const auto* fadeNode = a_geometry->GetGeometryRuntimeData().shaderProperty ? a_geometry->GetGeometryRuntimeData().shaderProperty->fadeNode : nullptr;
			float liveDistance = -2.0f, liveAmplitude = -2.0f;
			if (fadeNode) {
				const auto* vtable = *reinterpret_cast<const std::uintptr_t* const*>(fadeNode);
				using Fn = const void* (*)(const RE::BSFadeNode*);
				if (const void* node = reinterpret_cast<Fn>(vtable[0x1f8 / 8])(fadeNode)) {
					liveDistance = *reinterpret_cast<const float*>(static_cast<const std::byte*>(node) + 0x158);
					liveAmplitude = *reinterpret_cast<const float*>(static_cast<const std::byte*>(node) + 0x15c);
				}
			}
			++treeSamples;
			logger::warn("[DCLF] tree parity: '{}' derived amplitude {} from distance^2 {} / max {} at BuildFrame; the node now holds {} / {} (fade node {})",
				a_geometry->name.c_str() ? a_geometry->name.c_str() : "?", tree.treeParams[2], tree.windTimers[2], tree.windTimers[3], liveDistance, liveAmplitude,
				fmt::ptr(fadeNode));
		}
		if (ps)
			ok &= CompareBlock(a_geometry, "PS PerGeometry", expected.ps, psLayout, ps->constantTable.data(), ps->constantTable.size(), Slot(1, kPerGeometry),
				reinterpret_cast<ID3D11Resource*>(ps->constantBuffers[kPerGeometry].buffer), kPSDirLightDirection,
				kPSGroups[kPerGeometry] & ~kPSLightAssignment, 1ull << kPSEmitColor);
		return ok;
	}

	bool CaptureParity::CompareTechnique(const RE::BSGeometry* a_geometry, std::uint32_t a_objectIndex)
	{
		const auto& tables = SceneStore::Get().GetTables();
		const auto& object = tables.objects[a_objectIndex];
		const auto& technique = tables.TechniqueOf(object.pipelineIndex);
		// SetupTechnique writes VPOSOffset after it unmaps the buffer (engine notes, SetupTechnique); with
		// DXVK the memory stays mapped and the draw sees the late write, so compare what is there now.
		for (auto& stage : snapshots) {
			auto& snapshot = stage[kPerTechnique];
			if (snapshot.valid && snapshot.lastMapped)
				std::memcpy(snapshot.bytes.data(), snapshot.lastMapped, snapshot.size);
		}
		auto* vs = *globals::game::currentVertexShader;
		auto* ps = *globals::game::currentPixelShader;
		bool ok = true;
		if (vs)
			ok &= CompareBlock(a_geometry, "VS PerTechnique", technique.vs, LightingVSLayout(), vs->constantTable.data(), vs->constantTable.size(),
				Slot(0, kPerTechnique), reinterpret_cast<ID3D11Resource*>(vs->constantBuffers[kPerTechnique].buffer), kVSHighDetailRange, kVSGroups[kPerTechnique], 0);
		if (ps)
			ok &= CompareBlock(a_geometry, "PS PerTechnique", technique.ps, LightingPSLayout(), ps->constantTable.data(), ps->constantTable.size(),
				Slot(1, kPerTechnique), reinterpret_cast<ID3D11Resource*>(ps->constantBuffers[kPerTechnique].buffer), kPSFogColor, kPSGroups[kPerTechnique], 0);

		// Filter modes of the slots the material binds: the one SetupMaterial sets, else the technique's.
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
		const auto& tables = SceneStore::Get().GetTables();
		const auto& object = tables.objects[a_objectIndex];
		const auto& expected = tables.permutations[object.pipelineIndex];
		const auto& native = globals::state->permutationData;
		using Extra = State::ExtraShaderDescriptors;
		// Extra bits the Lighting shader or DCLF's model covers; the rest (IsSun, GrassSphereNormal) are other
		// shaders' and stay set from their last draw.
		constexpr std::uint32_t kLightingExtraBits = static_cast<std::uint32_t>(Extra::InWorld) | static_cast<std::uint32_t>(Extra::IsReflections) |
		                                             static_cast<std::uint32_t>(Extra::IsBeastRace) | static_cast<std::uint32_t>(Extra::SuppressExternalEmittance) |
		                                             static_cast<std::uint32_t>(Extra::AdditiveLighting);
		const std::uint32_t extra = expected.extraShaderDescriptor |
		                            ((object.flags & kObjectSuppressExternalEmittance) ? static_cast<std::uint32_t>(Extra::SuppressExternalEmittance) : 0u);
		// The ProjectedUV bit of hair, which DCLF drops (HairProjection), is left out of the descriptors' comparison.
		const std::uint32_t hairProjection = HairProjection(native.VertexShaderDescriptor) ? kPassProjectedUV : 0u;
		const std::array<std::pair<std::uint32_t, std::uint32_t>, 4> fields{ {
			{ expected.vertexShaderDescriptor, native.VertexShaderDescriptor & ~hairProjection },
			{ expected.pixelShaderDescriptor, native.PixelShaderDescriptor & ~hairProjection },
			{ extra, native.ExtraShaderDescriptor & kLightingExtraBits },
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
		auto& skin = globals::features::skin;
		if (!skin.loaded)
			return;
		const auto& tables = SceneStore::Get().GetTables();
		const auto& object = tables.objects[a_objectIndex];

		// t71 and t74 as State::Draw bound them from the pending binding the SetupMaterial hook left.
		if (!(object.flags & kObjectNoBindings) && object.materialIndex < tables.materials.size()) {
			const auto& expected = tables.materials[object.materialIndex].featureTextures;
			if (expected[0] || expected[1]) {
				std::array<ID3D11ShaderResourceView*, kFeatureMaterialTextures> native{};
				for (std::uint32_t f = 0; f < kFeatureMaterialTextures; ++f)
					a_context->PSGetShaderResources(kFeatureMaterialRegisters[f], 1, &native[f]);
				++skinTextureChecks;
				if (native != expected) {
					++skinTextureMismatches;
					NoteMismatch(fmt::format("{} skin textures: DCLF t71 {} t74 {}, native t71 {} t74 {}", Describe(a_geometry), fmt::ptr(expected[0]),
						fmt::ptr(expected[1]), fmt::ptr(native[0]), fmt::ptr(native[1])));
				}
				for (auto* bound : native) {
					if (bound)
						bound->Release();
				}
			}
		}

		// b7: what the SetupGeometry hook uploaded for this draw (it binds its buffer only while enabled).
		if (skin.settings.EnableSkin && a_objectIndex < tables.skinWetness.size()) {
			const auto& expected = tables.skinWetness[a_objectIndex];
			const auto& native = skin.currentWetness;
			++skinWetnessChecks;
			if (native.x != 0.0f || native.y != 0.0f)
				++skinWetDraws;
			if (native.x != expected[0] || native.y != expected[1] || native.z != expected[2] || native.w != expected[3]) {
				++skinWetnessMismatches;
				NoteMismatch(fmt::format("{} skin wetness: DCLF ({} {} {} {}), native ({} {} {} {})", Describe(a_geometry), expected[0], expected[1], expected[2],
					expected[3], native.x, native.y, native.z, native.w));
			}
			const auto* owner = a_geometry->GetUserData();
			const bool actor = owner && owner->GetFormType() == RE::FormType::ActorCharacter;
			if (actor != std::binary_search(tables.actorObjects.begin(), tables.actorObjects.end(), a_objectIndex)) {
				++skinOwnerMismatches;
				NoteMismatch(fmt::format("{} skin wetness: owned by an actor {}, DCLF resolved {}", Describe(a_geometry), actor, !actor));
			}
		}
	}

	void CaptureParity::CompareFeatureBindings(ID3D11DeviceContext* a_context)
	{
		FeatureBindings current;
		a_context->VSGetConstantBuffers(0, kConstantBufferSlots, current.vsBuffers.data());
		a_context->PSGetConstantBuffers(0, kConstantBufferSlots, current.psBuffers.data());
		a_context->VSGetShaderResources(0, kResourceSlots, current.vsResources.data());
		a_context->PSGetShaderResources(0, kResourceSlots, current.psResources.data());

		// Skin's per-geometry textures (t71, t74, t75), bound for SKIN permutations only.
		constexpr std::uint64_t kSkinResources = (1ull << (71 - 64)) | (1ull << (74 - 64)) | (1ull << (75 - 64));
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
			// PS t0-t15 are the engine's (the material and technique checks cover them, and slots a
			// permutation does not sample keep the previous draw's texture).
			compare(current.vsBuffers, baseline.vsBuffers, kFirstFeatureConstantBuffer, kPerDrawVSBuffers, "VS b");
			compare(current.psBuffers, baseline.psBuffers, kFirstFeatureConstantBuffer, kPerDrawPSBuffers, "PS b");
			compare(current.vsResources, baseline.vsResources, 0, 0, "VS t");
			compare(current.psResources, baseline.psResources, 0, 0xffffull, "PS t", kSkinResources);
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
		if (index < 0 || ConstantEvaluator::Evaluating() || partition >= pendingSlotCount)
			return;

		const auto& tables = SceneStore::Get().GetTables();
		if (static_cast<std::size_t>(index) >= tables.objects.size() || pendingSlots[partition] >= tables.geometries.size())
			return;
		const auto& object = tables.objects[index];
		const auto& geometryRecord = tables.geometries[pendingSlots[partition]];
		const auto* geometry = tables.objectGeometry[index];
		++drawsChecked;

		// Draw arguments and input assembler bindings.
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

		// Skinned: the palettes the bone setter bound at b10 (current) and b9 (previous), against the rows
		// BuildFrame copied out of the skin instance after running the same update the setter runs.
		if ((object.flags & kObjectSkinned) && static_cast<std::size_t>(index) < tables.boneOffset.size() && tables.boneRows[index]) {
			ID3D11Buffer* boneBuffers[2] = {};
			a_context->VSGetConstantBuffers(9, 2, boneBuffers);
			const std::uint32_t rows = tables.boneRows[index];
			const std::size_t offset = std::size_t(tables.boneOffset[index]) * 4;
			auto check = [&](ID3D11Buffer* a_buffer, const std::vector<float>& a_rows, const char* a_what) {
				++boneChecks;
				const auto mapped = a_buffer ? lastMappedAny.find(a_buffer) : lastMappedAny.end();
				if (mapped == lastMappedAny.end() || !mapped->second || offset + std::size_t(rows) * 4 > a_rows.size()) {
					++boneMismatches;
					NoteMismatch(fmt::format("{} {}: no mapped palette to compare (buffer {}, {} rows)", Describe(geometry), a_what, fmt::ptr(a_buffer), rows));
					return;
				}
				if (std::memcmp(mapped->second, &a_rows[offset], std::size_t(rows) * 16) != 0) {
					++boneMismatches;
					const auto* native = static_cast<const float*>(mapped->second);
					NoteMismatch(fmt::format("{} {}: row 0 DCLF ({} {} {} {}), native ({} {} {} {}), {} rows", Describe(geometry), a_what,
						a_rows[offset], a_rows[offset + 1], a_rows[offset + 2], a_rows[offset + 3], native[0], native[1], native[2], native[3], rows));
				}
			};
			check(boneBuffers[1], tables.bones, "Bones (b10)");
			check(boneBuffers[0], tables.previousBones, "PreviousBones (b9)");
			for (auto* buffer : boneBuffers) {
				if (buffer)
					buffer->Release();
			}
		}

		// Light Limit Fix's StrictLightData as its SetupGeometry hook left it (it uploads when these change).
		if (globals::features::lightLimitFix.loaded) {
			const auto& native = globals::features::lightLimitFix.strictLightDataTemp;
			const auto& expected = tables.lights[index];
			++lightChecks;
			if (native.NumStrictLights != 0 || native.RoomIndex != expected.roomIndex || native.ShadowBitMask != expected.shadowBitMask) {
				++lightMismatches;
				NoteMismatch(fmt::format("{} strict light data: DCLF room {} shadow mask {:X}, native {} strict lights, room {} shadow mask {:X}", Describe(geometry),
					expected.roomIndex, expected.shadowBitMask, native.NumStrictLights, native.RoomIndex, native.ShadowBitMask));
			}
		}

		CompareSkin(a_context, geometry, static_cast<std::uint32_t>(index));

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

	void CaptureParity::OnNativeLightingDraw(const RE::BSRenderPass* a_pass, std::uint32_t a_renderFlags)
	{
		pendingObject = -1;
		if (ConstantEvaluator::Evaluating() || !globals::deferred->deferredPass || !a_pass || !a_pass->geometry)
			return;
		InstallDrawHook();
		++nativeDraws;
		renderFlagsSeen.insert(a_renderFlags);

		auto& store = SceneStore::Get();
		auto* geometry = a_pass->geometry;
		const std::int32_t index = store.FindObject(geometry);

		if (index < 0) {
			if (store.IsTracked(geometry)) {
				// Tracked, drawn natively, but DCLF left it out this frame. Hidden here would mean the
				// visibility check disagrees with the game (fading objects are drawn natively by design).
				const Ineligible reason = store.Classify(geometry);
				if (reason == Ineligible::Hidden) {
					++mismatchedDraws;
					NoteMismatch(fmt::format("{} drawn natively but excluded as {}", Describe(geometry), kIneligibleNames[static_cast<std::size_t>(reason)]));
				}
				++notInTables;
			} else if (SceneStore::ClassifyStatic(*geometry, nullptr) == Ineligible::None) {
				if (auto* category = store.CategoryNodeOf(geometry)) {
					++untrackedEligible;  // followed below and reported with its parents and what tracked it (ResolveUntracked)
					const std::uint32_t frame = store.GetFrame();
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
					if (outsideChains.size() < 8)
						outsideChains.insert(chain);
				}
			}
			return;
		}

		++checkedDraws;
		const auto& tables = store.GetTables();
		const auto& object = tables.objects[index];
		if (object.flags & kObjectNoBindings)
			return;  // a culling candidate only: no material or pipeline entry to compare against
		if (PassCapture::FadingAtRegistration(a_pass)) {
			// The native loop's own pass of an object DCLF draws (a LOD cross-fade's hint-10 copy): nothing of
			// DCLF's stands for it, so it is neither compared nor counted against the object's draws.
			++nativeOnlyPasses;
			return;
		}
		const auto& key = tables.pipelines[object.pipelineIndex];
		const auto* state = globals::state;

		bool mismatch = false;
		const auto* accumulated = store.FindAccumulatedPass(geometry);
		if (!accumulated || accumulated->pass != a_pass) {
			mismatch = true;
			NoteMismatch(fmt::format("{} drawn pass {} is not the accumulated pass {}", Describe(geometry), fmt::ptr(a_pass),
				fmt::ptr(accumulated ? accumulated->pass : nullptr)));
		}
		// A pass in an alpha-test list is drawn by DCLF with DoAlphaTest whatever the native draw's technique
		// carries this frame (DrawnPassDescriptor), so the native side is compared as DCLF normalises it.
		const std::uint32_t list = accumulated ? accumulated->subPass : 0u;
		const std::uint32_t nativeDescriptor = DrawnPassDescriptor(PassDescriptorOf(a_pass->passEnum), list);
		if (key.passDescriptor != nativeDescriptor) {
			mismatch = true;
			NoteMismatch(fmt::format("{} pass descriptor: DCLF {:08X}, native {:08X} (flags {:016X}; accumulated technique {:08X} list {} passEnum then {:08X})",
				Describe(geometry), key.passDescriptor, PassDescriptorOf(a_pass->passEnum), a_pass->shaderProperty ? a_pass->shaderProperty->flags.underlying() : 0ull,
				accumulated ? accumulated->technique : 0u, accumulated ? accumulated->subPass : 0u, accumulated ? PassDescriptorOf(accumulated->passEnum) : 0u));
		}
		// The shader descriptors likewise, with that one bit left out of the comparison in an alpha-test list, and the
		// ProjectedUV bit of hair, which DCLF drops (HairProjection).
		const std::uint32_t ignored = DrawnPassDescriptor(0, list) | (HairProjection(PassDescriptorOf(a_pass->passEnum)) ? kPassProjectedUV : 0u);
		if (((key.vertexDescriptor ^ state->modifiedVertexDescriptor) & ~ignored) != 0 || ((key.pixelDescriptor ^ state->modifiedPixelDescriptor) & ~ignored) != 0) {
			mismatch = true;
			NoteMismatch(fmt::format("{} descriptors: DCLF VS {:08X} PS {:08X}, native VS {:08X} PS {:08X} (pass {:08X}, flags {:016X})",
				Describe(geometry), key.vertexDescriptor, key.pixelDescriptor, state->modifiedVertexDescriptor, state->modifiedPixelDescriptor,
				a_pass->passEnum, a_pass->shaderProperty ? a_pass->shaderProperty->flags.underlying() : 0ull));
		}
		if (!SameTransform(object.world, geometry->world)) {
			mismatch = true;
			NoteMismatch(fmt::format("{} world transform changed after the tables were built", Describe(geometry)));
		}
		if (!mismatch && !CompareMaterial(geometry, object.materialIndex)) {
			mismatch = true;
			++materialMismatches;
			const auto* drawn = a_pass->shaderProperty ? a_pass->shaderProperty->material : nullptr;
			const auto* recorded = object.materialIndex < tables.materialSlotKey.size() ? tables.materialSlotKey[object.materialIndex].first : nullptr;
			const std::uint32_t frame = store.GetFrame();
			auto& follow = materialMismatchFollow[drawn];
			if (follow.frames == 0)
				follow.firstFrame = frame;
			if (follow.frames == 0 || follow.lastFrame != frame)
				++follow.frames;
			follow.lastFrame = frame;
			follow.keyDiffers |= drawn != recorded;
			follow.writtenBefore |= store.GetWrittenMaterials().contains(drawn);
		}
		if (!mismatch && !CompareGeometry(geometry, static_cast<std::uint32_t>(index), a_renderFlags)) {
			mismatch = true;
			++geometryMismatches;
		}
		if (!mismatch && !CompareTechnique(geometry, static_cast<std::uint32_t>(index))) {
			mismatch = true;
			++techniqueMismatches;
		}
		if (mismatch)
			++mismatchedDraws;
		pendingObject = index;
		// What its DrawIndexed calls should bind: the object's geometry, or each partition this pass draws, by
		// the pass's own LODMode.
		pendingSlotCount = pendingSlotCursor = 0;
		const auto* skin = geometry->GetGeometryRuntimeData().skinInstance.get();
		const std::uint32_t partitions = skin && skin->skinPartition && skin->skinPartition->numPartitions > 1 ?
		                                     SceneStore::SkinPartitionMask(*skin, SceneStore::LodRowOf(*a_pass)) :
		                                     0u;
		if (skin && skin->skinPartition && skin->skinPartition->numPartitions > 1 && !partitions) {
			pendingObject = -1;  // the engine draws no partition of it with this pass
			return;
		}
		std::uint32_t slot = tables.objects[index].geometryIndex;
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
		auto& store = SceneStore::Get();
		std::erase_if(untracked, [&](auto& a_entry) {
			auto& [geometry, entry] = a_entry;
			std::uint32_t frame = 0;
			SceneStore::TrackSource source{};
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
		// Called at the start of every main pass: a new frame, a new baseline.
		baselineValid = false;
		pendingRewrites = 0;
		ResolveUntracked(SceneStore::Get().GetFrame());
		{
			// A mismatched material, one frame on: this frame's drain has run (the accumulate phase is before
			// the main pass).
			auto& store = SceneStore::Get();
			const std::uint32_t frame = store.GetFrame();
			std::erase_if(materialMismatchFollow, [&](const auto& a_entry) {
				const auto& [material, follow] = a_entry;
				if (follow.lastFrame >= frame)
					return false;
				const bool written = store.GetWrittenMaterials().contains(material);
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

		const auto& stats = SceneStore::Get().GetStats();
		const bool ok = mismatchedDraws == 0 && untrackedEligible == 0 && drawMismatches == 0 && permutationMismatches == 0 && lightMismatches == 0 &&
		                skinTextureMismatches == 0 && skinWetnessMismatches == 0 && skinOwnerMismatches == 0;
		std::string flags;
		for (auto value : renderFlagsSeen)
			flags += fmt::format(" {:X}", value);
		const auto* player = RE::PlayerCharacter::GetSingleton();
		const auto* cell = player ? player->GetParentCell() : nullptr;
		const char* cellName = cell ? cell->GetName() : nullptr;
		logger::info("[DCLF] location: cell {:08X} '{}' ({})", cell ? cell->GetFormID() : 0u, cellName ? cellName : "",
			cell && cell->IsInteriorCell() ? "interior" : "exterior");
		logger::info("[DCLF] capture parity {}: {} native main-pass lighting draws, {} checked against the tables, {} mismatched ({} material, {} per-geometry, {} technique), {} untracked eligible, {} tracked but excluded, {} native-only passes of DCLF objects; tables hold {} objects / {} geometries / {} pipelines ({} with shadow mask) / {} materials from {} tracked; render flags seen:{}",
			ok ? "OK" : "MISMATCH", nativeDraws, checkedDraws, mismatchedDraws, materialMismatches, geometryMismatches, techniqueMismatches, untrackedEligible,
			notInTables, nativeOnlyPasses, stats.objects, stats.geometries, stats.pipelines, stats.shadowMaskPipelines, stats.materials, stats.tracked, flags);
		if (boneChecks)
			logger::info("[DCLF] bone palette parity {}: {} palettes checked, {} differ", boneMismatches == 0 ? "OK" : "MISMATCH", boneChecks, boneMismatches);
		boneChecks = boneMismatches = 0;
		logger::info("[DCLF] draw parity {}: {} draws checked, {} with different arguments or bound buffers", drawMismatches == 0 ? "OK" : "MISMATCH",
			drawsChecked, drawMismatches);
		logger::info("[DCLF] light data parity {}: {} draws checked, {} with different StrictLightData", lightMismatches == 0 ? "OK" : "MISMATCH", lightChecks,
			lightMismatches);
		logger::info("[DCLF] skin parity {}: {} draws' t71/t74 checked, {} differ; {} draws' wetness checked ({} wet), {} differ, {} with a different actor ownership",
			skinTextureMismatches == 0 && skinWetnessMismatches == 0 && skinOwnerMismatches == 0 ? "OK" : "MISMATCH", skinTextureChecks, skinTextureMismatches,
			skinWetnessChecks, skinWetDraws, skinWetnessMismatches, skinOwnerMismatches);
		std::string diffs;
		static constexpr const char* kFieldNames[] = { "vertex", "pixel", "extra", "feature" };
		for (const auto& [key, count] : permutationDiffs)
			diffs += fmt::format(" {} {:X} x{}", kFieldNames[key >> 32], static_cast<std::uint32_t>(key), count);
		logger::info("[DCLF] permutation parity {}: {} draws checked, {} differ; material textures with inherited filter modes: {};{}",
			permutationMismatches == 0 ? "OK" : "MISMATCH", permutationChecks, permutationMismatches, inheritedFilters, diffs.empty() ? " no differing bits" : diffs);
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

		nativeDraws = checkedDraws = mismatchedDraws = untrackedEligible = notInTables = nativeOnlyPasses = materialMismatches = geometryMismatches = 0;
		drawMismatches = drawsChecked = 0;
		techniqueMismatches = inheritedFilters = permutationChecks = permutationMismatches = lightChecks = lightMismatches = 0;
		skinTextureChecks = skinTextureMismatches = skinWetnessChecks = skinWetnessMismatches = skinWetDraws = skinOwnerMismatches = 0;
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
}
