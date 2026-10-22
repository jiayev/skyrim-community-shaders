// MaterialSources.h's rules: the frame-sourced float positions, signatures and the copy / apply helpers.

#include "Features/DrawcallLimitFix/Core/Shading/MaterialSources.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Extensions/MaterialPorts.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Materials.h"
#include "Features/DrawcallLimitFix/Core/Extensions/MaterialWrites.h"
#include "Features/DrawcallLimitFix/Core/Shading/MaterialPort.h"
#include "Features/DrawcallLimitFix/Core/Shading/LightingConstants.h"

#include "Features/DrawcallLimitFix/Core/Runtime/EventQueue.h"
#include "Features/DrawcallLimitFix/Core/Scene/SceneWake.h"
#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"

#include <bit>

#include "Features/DrawcallLimitFix/Core/Shading/ConstantEvaluator.h"
#include "Features/DrawcallLimitFix/Core/Shading/LightingDescriptors.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <memory>

#include "Globals.h"
#include "Features/DrawcallLimitFix/Diagnostics/Observation.h"

namespace DCLF::MaterialSources
{
	namespace
	{
		// Pass descriptor flags (ShaderCache.h LightingShaderFlags) that decide which frame-sourced groups
		// SetupMaterial writes (with the material contributors' own: Extensions::MaterialSignatureBits, TruePBR's TruePbr).
		constexpr std::uint32_t kAmbientSpecular = 1u << 17;
		constexpr std::uint32_t kSnow = 1u << 21;
		constexpr std::uint32_t kCharacterLight = 1u << 22;

		// PS PerMaterial variables whose values come from the shader object or engine globals
		// (BSLightingShader::SetupMaterial, AE 1414dc310), with the components that do. Not IBLParams (this+0xcc, then
		// this+0xd0.. or this+0xe0.. by this+0xf0): no Lighting stage reads it, and its colour drifts with the time of day
		// almost every frame, which would re-version every record of every signature.
		struct FrameVariable
		{
			std::uint32_t variable;
			std::uint32_t components;  // bit c: component c
		};
		constexpr std::array<FrameVariable, 5> kFrameVariables{ {
			{ 6, 0xF },   // AmbientSpecularTintAndFresnelPower (AmbientSpecular): 0x14203315c..
			{ 34, 0xF },  // SnowRimLightParameters (Snow): 0x142035590, 0x1420355a8, 0x1420355c0, 0x1420355d8
			{ 35, 0xF },  // CharacterLightParams (CharacterLight): 0x14203316c.., or smState (TruePBR)
			{ 24, 0x4 },  // LODTexParams.z (MTLand, LODLand): 0x142032fda
			{ 31, 0xC },  // LandscapeTexture5to6IsSnow.zw (MTLand with Snow): 0x142035548, 1 / 0x1420355f0
		} };
		constexpr std::uint32_t kCharacterLightSlot = 11;

		std::vector<std::uint32_t> BuildPSPositions()
		{
			const auto& layout = LightingPSLayout();
			std::vector<std::uint32_t> out;
			for (const auto& v : kFrameVariables)
				for (std::uint32_t c = 0; c < layout.size[v.variable] && c < 4; ++c)
					if ((v.components >> c) & 1)
						out.push_back(layout.offset[v.variable] + c);
			return out;
		}

		std::vector<std::uint32_t> BuildVSPositions()
		{
			const auto& layout = LightingVSLayout();
			std::vector<std::uint32_t> out;
			for (std::uint32_t c = 0; c < layout.size[kVSTexcoordOffset] && c < 4; ++c)
				out.push_back(layout.offset[kVSTexcoordOffset] + c);
			return out;
		}

		// A port's record's TexcoordOffset (its rules' frame VS variable: BSEffectShader's VS variable 9).
		const std::vector<std::uint32_t>& PortVSPositions(std::uint8_t a_kind)
		{
			return Extensions::MaterialSpaceOf(a_kind).frameVSFloats;
		}
	}

	std::uint32_t Signature(std::uint32_t a_passDescriptor)
	{
		std::uint32_t signature = a_passDescriptor & (kAmbientSpecular | kSnow | kCharacterLight | Extensions::MaterialSignatureBits());
		const std::uint32_t technique = (a_passDescriptor >> 24) & 0x3f;
		if (technique == 8 || technique == 0x13)
			signature |= 1u << 24;  // MTLand, MTLandLODBlend: LODTexParams.z, the snow flags
		else if (technique == 9 || technique == 0x12)
			signature |= 2u << 24;  // LODLand, LODLandNoise: LODTexParams.z
		return signature;
	}

	const std::vector<std::uint32_t>& FramePSFloats()
	{
		static const std::vector<std::uint32_t> positions = BuildPSPositions();
		return positions;
	}

	const std::vector<std::uint32_t>& FrameVSFloats()
	{
		static const std::vector<std::uint32_t> positions = BuildVSPositions();
		return positions;
	}

	void CopyFrameComponents(const MaterialRecord& a_from, MaterialRecord& a_to, std::uint32_t a_passDescriptor)
	{
		if (a_to.kind != kMaterialKindLighting || a_from.kind != kMaterialKindLighting) {
			// A port's record's frame parts are TexcoordOffset (the transform watch's) and, while a feature patches them, its declared frame
			// variables (Api::MaterialFrameVariable: the Effect port's FalloffData and BaseColor; the frame values write their rows); its pass
			// descriptor's bits are not Lighting's.
			if (a_to.kind == a_from.kind) {
				const auto copy = [](const std::vector<std::uint32_t>& a_positions, const ConstantBlock& a_source, ConstantBlock& a_target) {
					for (const auto position : a_positions)
						if (a_target.Written(position) && a_source.Written(position))
							a_target.floats[position] = a_source.floats[position];
				};
				const auto& space = Extensions::MaterialSpaceOf(a_to.kind);
				copy(PortVSPositions(a_to.kind), a_from.vs, a_to.vs);
				if (Extensions::FrameVSVariablePatched()) {
					copy(space.declaredVSFloats, a_from.vs, a_to.vs);
					copy(space.declaredPSFloats, a_from.ps, a_to.ps);
				}
			}
			return;
		}
		for (const auto position : FramePSFloats())
			if (a_to.ps.Written(position) && a_from.ps.Written(position))
				a_to.ps.floats[position] = a_from.ps.floats[position];
		for (const auto position : FrameVSFloats())
			if (a_to.vs.Written(position) && a_from.vs.Written(position))
				a_to.vs.floats[position] = a_from.vs.floats[position];
		// Whether t11 is written at all is the frame's too: SetupMaterial binds it only while the character light has a
		// render target (its index, 0x142033db0, is -1 while a cell loads).
		if (a_passDescriptor & kCharacterLight) {
			a_to.textures[kCharacterLightSlot] = a_from.textures[kCharacterLightSlot];
			a_to.addressModes[kCharacterLightSlot] = a_from.addressModes[kCharacterLightSlot];
			a_to.filterModes[kCharacterLightSlot] = a_from.filterModes[kCharacterLightSlot];
			a_to.textureWritten = (a_to.textureWritten & ~(1u << kCharacterLightSlot)) | (a_from.textureWritten & (1u << kCharacterLightSlot));
		}
	}

	bool ApplyFrameComponents(const MaterialRecord& a_live, MaterialRecord& a_record, std::uint32_t a_passDescriptor, bool* a_floatsChanged)
	{
		// A port's record without signature components (the Effect port's) has none to apply (NoteMaterialRecord lists none).
		if (!Extensions::PortRules(a_live.kind).signatureComponents || !Extensions::PortRules(a_record.kind).signatureComponents)
			return false;
		for (const auto position : FramePSFloats())
			if (a_record.ps.Written(position) && a_live.ps.Written(position)) {
				if (a_floatsChanged && std::bit_cast<std::uint32_t>(a_record.ps.floats[position]) != std::bit_cast<std::uint32_t>(a_live.ps.floats[position]))
					*a_floatsChanged = true;
				a_record.ps.floats[position] = a_live.ps.floats[position];
			}
		if (!(a_passDescriptor & kCharacterLight) || !((a_live.textureWritten >> kCharacterLightSlot) & 1))
			return false;
		// The view is the frame's (CharacterLightView, the frame record's kCharacterLightRegister): the record holds none, so the
		// render target alternating every frame changes no record.
		const bool changed = a_record.textures[kCharacterLightSlot] != nullptr || a_record.addressModes[kCharacterLightSlot] != a_live.addressModes[kCharacterLightSlot] ||
		                     a_record.filterModes[kCharacterLightSlot] != a_live.filterModes[kCharacterLightSlot] ||
		                     !((a_record.textureWritten >> kCharacterLightSlot) & 1);
		a_record.textures[kCharacterLightSlot] = nullptr;
		a_record.addressModes[kCharacterLightSlot] = a_live.addressModes[kCharacterLightSlot];
		a_record.filterModes[kCharacterLightSlot] = a_live.filterModes[kCharacterLightSlot];
		a_record.textureWritten |= 1u << kCharacterLightSlot;
		return changed;
	}

	void StripFrameViews(MaterialRecord& a_record, std::uint32_t a_passDescriptor)
	{
		if ((a_passDescriptor & kCharacterLight) && a_record.kind == kMaterialKindLighting)
			a_record.textures[kCharacterLightSlot] = nullptr;
	}

	bool FrameCharacterLight(std::uint32_t a_passDescriptor)
	{
		return (a_passDescriptor & kCharacterLight) != 0;
	}

	ID3D11ShaderResourceView* CharacterLightView(const MaterialRecord& a_live, std::uint32_t a_passDescriptor)
	{
		return (a_passDescriptor & kCharacterLight) && ((a_live.textureWritten >> kCharacterLightSlot) & 1) ? a_live.textures[kCharacterLightSlot] : nullptr;
	}

	void KeepUnreadFloats(const MaterialRecord& a_from, MaterialRecord& a_to)
	{
		if (a_from.kind != kMaterialKindLighting || a_to.kind != kMaterialKindLighting)
			return;  // IBLParams is Lighting's
		const auto& layout = LightingPSLayout();
		for (std::uint32_t c = 0; c < layout.size[kPSIBLParams]; ++c)
			a_to.ps.floats[layout.offset[kPSIBLParams] + c] = a_from.ps.floats[layout.offset[kPSIBLParams] + c];
	}

	ID3D11ShaderResourceView* FrameCharacterLightView(const MaterialPort::MaterialFrame& a_frame)
	{
		// What SetupMaterial binds at t11 (MaterialPortVanilla: the render target at 0x142033db0 while not negative; TruePBR's hook:
		// the character light image space texture's), from the frame's sample: no engine read.
		const auto& vanilla = a_frame.vanilla;
		if (vanilla.characterLightTarget >= 0 && static_cast<std::uint32_t>(vanilla.characterLightTarget) < MaterialPort::kVanillaRenderTargets)
			if (auto* view = vanilla.renderTargetViews[static_cast<std::size_t>(vanilla.characterLightTarget)])
				return view;
		const auto& hook = a_frame.hook;
		if (hook.characterLightTarget >= 0 && static_cast<std::uint32_t>(hook.characterLightTarget) < MaterialPort::kHookRenderTargets)
			return hook.renderTargetViews[static_cast<std::size_t>(hook.characterLightTarget)];
		return nullptr;
	}
}
