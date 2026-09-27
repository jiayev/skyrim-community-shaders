#include "MaterialSources.h"

#include "EventQueue.h"

#include <bit>

#include "ConstantEvaluator.h"
#include "LightingDescriptors.h"

#include <array>
#include <atomic>
#include <memory>

#include "Globals.h"

namespace DCLF::MaterialSources
{
	namespace
	{
		// Pass descriptor flags (ShaderCache.h LightingShaderFlags) that decide which frame-sourced groups
		// SetupMaterial writes.
		constexpr std::uint32_t kTruePbr = 1u << 3;
		constexpr std::uint32_t kAmbientSpecular = 1u << 17;
		constexpr std::uint32_t kSnow = 1u << 21;
		constexpr std::uint32_t kCharacterLight = 1u << 22;

		// PS PerMaterial variables whose values come from the shader object or engine globals
		// (BSLightingShader::SetupMaterial, AE 1414dc310), with the components that do.
		struct FrameVariable
		{
			std::uint32_t variable;
			std::uint32_t components;  // bit c: component c
		};
		constexpr std::array<FrameVariable, 6> kFrameVariables{ {
			{ 29, 0xF },  // IBLParams: this+0xcc, then this+0xd0.. or this+0xe0.. by this+0xf0
			{ 6, 0xF },   // AmbientSpecularTintAndFresnelPower (AmbientSpecular): 0x14203315c..
			{ 34, 0xF },  // SnowRimLightParameters (Snow): 0x142035590, 0x1420355a8, 0x1420355c0, 0x1420355d8
			{ 35, 0xF },  // CharacterLightParams (CharacterLight): 0x14203316c.., or smState (TruePBR)
			{ 24, 0x4 },  // LODTexParams.z (MTLand, LODLand): 0x142032fda
			{ 31, 0xC },  // LandscapeTexture5to6IsSnow.zw (MTLand with Snow): 0x142035548, 1 / 0x1420355f0
		} };
		constexpr std::uint32_t kVSTexcoordOffset = 11;
		constexpr std::uint32_t kCharacterLightSlot = 11;

		// ---- The write queue (EventQueue): drained by the render thread once a frame; a producer never blocks.
		EventQueue<const RE::BSShaderMaterial*, 8192>& GetQueue()
		{
			static EventQueue<const RE::BSShaderMaterial*, 8192> queue;
			return queue;
		}

		// ---- Hooks.
		// A controller's own type field (+0x50): which member of the property or material it writes.
		std::uint32_t ControllerType(const RE::NiTimeController* a_controller)
		{
			return *reinterpret_cast<const std::uint32_t*>(reinterpret_cast<const std::byte*>(a_controller) + 0x50);
		}

		void NoteTarget(const RE::NiTimeController* a_controller)
		{
			// NiTimeController::target; these controllers attach to a BSLightingShaderProperty only (Func60), and
			// write through BSShaderProperty::material (+0x78).
			auto* property = static_cast<const RE::BSShaderProperty*>(a_controller->target);
			if (property && property->material)
				NoteWritten(property->material);
		}

		// BSLightingShaderPropertyFloatController::Update (AE 14150dde0, vtable slot 0x27). Type 0xb writes the
		// property's emissive multiplier (per-object shading, resampled every frame at Prepass) and types above
		// 0x13 the texture-transform buffers (ApplyTextureTransform, every frame); every other type a material field.
		struct FloatControllerUpdate
		{
			static void thunk(RE::NiTimeController* a_this, void* a_data)
			{
				func(a_this, a_data);
				const auto type = ControllerType(a_this);
				if (type != 0xb && type <= 0x13)
					NoteTarget(a_this);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		// BSLightingShaderPropertyColorController::Update (AE 14150ea50, slot 0x27). Type 1 writes the property's
		// emissive colour (per-object shading); every other type a material colour.
		struct ColorControllerUpdate
		{
			static void thunk(RE::NiTimeController* a_this, void* a_data)
			{
				func(a_this, a_data);
				if (ControllerType(a_this) != 1)
					NoteTarget(a_this);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		// The material's in-place rewrites (BSShaderMaterial 02, BSLightingShaderMaterialBase 08-0A), per vtable.
		template <std::size_t V>
		struct MaterialHooks
		{
			struct CopyMembers
			{
				static void thunk(RE::BSShaderMaterial* a_this, RE::BSShaderMaterial* a_that)
				{
					func(a_this, a_that);
					NoteWritten(a_this);
				}
				static inline REL::Relocation<decltype(thunk)> func;
			};
			struct OnLoadTextureSet
			{
				static void thunk(RE::BSShaderMaterial* a_this, std::uint64_t a_arg, RE::BSTextureSet* a_set)
				{
					func(a_this, a_arg, a_set);
					NoteWritten(a_this);
				}
				static inline REL::Relocation<decltype(thunk)> func;
			};
			struct ClearTextures
			{
				static void thunk(RE::BSShaderMaterial* a_this)
				{
					func(a_this);
					NoteWritten(a_this);
				}
				static inline REL::Relocation<decltype(thunk)> func;
			};
			struct ReceiveValues
			{
				static void thunk(RE::BSShaderMaterial* a_this, bool a_1, bool a_2, bool a_3, bool a_4, bool a_5)
				{
					func(a_this, a_1, a_2, a_3, a_4, a_5);
					NoteWritten(a_this);
				}
				static inline REL::Relocation<decltype(thunk)> func;
			};
			static void Install(const REL::VariantID& a_vtable)
			{
				stl::write_vfunc<0x02, CopyMembers>(a_vtable);
				stl::write_vfunc<0x08, OnLoadTextureSet>(a_vtable);
				stl::write_vfunc<0x09, ClearTextures>(a_vtable);
				stl::write_vfunc<0x0A, ReceiveValues>(a_vtable);
			}
		};

		template <std::size_t... I>
		void InstallMaterialHooks(const std::array<REL::VariantID, sizeof...(I)>& a_vtables, std::index_sequence<I...>)
		{
			(MaterialHooks<I>::Install(a_vtables[I]), ...);
		}

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
	}

	void Install()
	{
		stl::write_vfunc<0x27, FloatControllerUpdate>(RE::VTABLE_BSLightingShaderPropertyFloatController[0]);
		stl::write_vfunc<0x27, ColorControllerUpdate>(RE::VTABLE_BSLightingShaderPropertyColorController[0]);
		const std::array<REL::VariantID, 14> vtables{ RE::VTABLE_BSLightingShaderMaterial[0], RE::VTABLE_BSLightingShaderMaterialBase[0],
			RE::VTABLE_BSLightingShaderMaterialEnvmap[0], RE::VTABLE_BSLightingShaderMaterialEye[0], RE::VTABLE_BSLightingShaderMaterialGlowmap[0],
			RE::VTABLE_BSLightingShaderMaterialParallax[0], RE::VTABLE_BSLightingShaderMaterialParallaxOcc[0], RE::VTABLE_BSLightingShaderMaterialFacegen[0],
			RE::VTABLE_BSLightingShaderMaterialFacegenTint[0], RE::VTABLE_BSLightingShaderMaterialHairTint[0], RE::VTABLE_BSLightingShaderMaterialLandscape[0],
			RE::VTABLE_BSLightingShaderMaterialLODLandscape[0], RE::VTABLE_BSLightingShaderMaterialSnow[0], RE::VTABLE_BSLightingShaderMaterialMultiLayerParallax[0] };
		InstallMaterialHooks(vtables, std::make_index_sequence<14>{});
		// Build the fixed position lists now, off the render thread's first frame.
		(void)FramePSFloats();
		(void)FrameVSFloats();
		logger::info("[DCLF] material write hooks installed: float and colour controllers, 14 material vtables");
	}

	void NoteWritten(const RE::BSShaderMaterial* a_material)
	{
		if (!a_material)
			return;
		GetQueue().Push(a_material);
	}

	bool Drain(ankerl::unordered_dense::set<const RE::BSShaderMaterial*>& a_out)
	{
		// The queue loses nothing (it spills when full), so a drain is always complete.
		GetQueue().Drain([&](const RE::BSShaderMaterial* a_material) { a_out.insert(a_material); });
		return true;
	}

	std::uint32_t Signature(std::uint32_t a_passDescriptor)
	{
		std::uint32_t signature = a_passDescriptor & (kTruePbr | kAmbientSpecular | kSnow | kCharacterLight);
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
		for (const auto position : FramePSFloats())
			if (a_to.ps.Written(position) && a_from.ps.Written(position))
				a_to.ps.floats[position] = a_from.ps.floats[position];
		for (const auto position : FrameVSFloats())
			if (a_to.vs.Written(position) && a_from.vs.Written(position))
				a_to.vs.floats[position] = a_from.vs.floats[position];
		if ((a_passDescriptor & kCharacterLight) && ((a_from.textureWritten >> kCharacterLightSlot) & 1)) {
			a_to.textures[kCharacterLightSlot] = a_from.textures[kCharacterLightSlot];
			a_to.addressModes[kCharacterLightSlot] = a_from.addressModes[kCharacterLightSlot];
			a_to.filterModes[kCharacterLightSlot] = a_from.filterModes[kCharacterLightSlot];
			a_to.textureWritten |= 1u << kCharacterLightSlot;
		}
	}

	bool ApplyFrameComponents(const MaterialRecord& a_live, MaterialRecord& a_record, std::uint32_t a_passDescriptor, bool* a_floatsChanged)
	{
		for (const auto position : FramePSFloats())
			if (a_record.ps.Written(position) && a_live.ps.Written(position)) {
				if (a_floatsChanged && std::bit_cast<std::uint32_t>(a_record.ps.floats[position]) != std::bit_cast<std::uint32_t>(a_live.ps.floats[position]))
					*a_floatsChanged = true;
				a_record.ps.floats[position] = a_live.ps.floats[position];
			}
		if (!(a_passDescriptor & kCharacterLight) || !((a_live.textureWritten >> kCharacterLightSlot) & 1))
			return false;
		const bool changed = a_record.textures[kCharacterLightSlot] != a_live.textures[kCharacterLightSlot] ||
		                     a_record.addressModes[kCharacterLightSlot] != a_live.addressModes[kCharacterLightSlot] ||
		                     a_record.filterModes[kCharacterLightSlot] != a_live.filterModes[kCharacterLightSlot];
		a_record.textures[kCharacterLightSlot] = a_live.textures[kCharacterLightSlot];
		a_record.addressModes[kCharacterLightSlot] = a_live.addressModes[kCharacterLightSlot];
		a_record.filterModes[kCharacterLightSlot] = a_live.filterModes[kCharacterLightSlot];
		a_record.textureWritten |= 1u << kCharacterLightSlot;
		return changed;
	}

	bool ApplyTextureTransform(const RE::BSShaderMaterial* a_material, MaterialRecord& a_record)
	{
		const auto* smState = globals::game::smState;
		if (!a_material || !smState)
			return false;
		// SetupMaterial (vanilla and TruePBR alike): offset and scale of the buffer the frame reads.
		const auto* material = static_cast<const RE::BSLightingShaderMaterialBase*>(a_material);
		const std::uint32_t buffer = smState->textureTransformCurrentBuffer & 1;
		const float values[4]{ material->texCoordOffset[buffer].x, material->texCoordOffset[buffer].y, material->texCoordScale[buffer].x,
			material->texCoordScale[buffer].y };
		const auto& positions = FrameVSFloats();
		bool changed = false;
		for (std::size_t c = 0; c < positions.size() && c < 4; ++c)
			if (a_record.vs.Written(positions[c])) {
				changed |= std::bit_cast<std::uint32_t>(a_record.vs.floats[positions[c]]) != std::bit_cast<std::uint32_t>(values[c]);
				a_record.vs.floats[positions[c]] = values[c];
			}
		return changed;
	}
}
