#include "MaterialSources.h"
#include "MaterialPort.h"

#include "Features/DrawcallLimitFix/Common/EventQueue.h"
#include "Features/DrawcallLimitFix/Common/Switches.h"

#include <bit>

#include "ConstantEvaluator.h"
#include "LightingDescriptors.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>
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
		// (BSLightingShader::SetupMaterial, AE 1414dc310), with the components that do. Not IBLParams (this+0xcc, then
		// this+0xd0.. or this+0xe0.. by this+0xf0): no Lighting stage reads it, and its colour drifts with the time of day
		// almost every frame, which re-versioned every record of every signature (the motion run m1: ~1,030 a frame).
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
		constexpr std::uint32_t kVSTexcoordOffset = 11;
		constexpr std::uint32_t kCharacterLightSlot = 11;

		// ---- The write queue (EventQueue): CS_DCLF_CAPTURE_PARITY's diagnostics alone, drained by the render thread once a frame; a
		// producer never blocks. The records follow the writes' captures (MaterialPort::PushCapture), not this queue.
		EventQueue<const RE::BSShaderMaterial*, 8192>& GetQueue()
		{
			static EventQueue<const RE::BSShaderMaterial*, 8192> queue;
			return queue;
		}
		bool WritesQueued()
		{
			static const bool queued = SwitchEnabled(Switch::CaptureParity);
			return queued;
		}
		EventQueue<const void*, 8192>& ShadingQueue()
		{
			static EventQueue<const void*, 8192> queue;
			return queue;
		}
		// The same events for the kept shadow build (ShadowKept), which runs before the main pass drains its own.
		EventQueue<const RE::BSShaderMaterial*, 8192>& ShadowTransformQueue()
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

		// AE 1.6.1170 controller destination tables (Ghidra: float Update 0x14150DDE0,
		// colour Update 0x14150EA50). The float table contains float indices; the colour
		// table contains byte offsets. Other runtimes retain the conservative notification.
		const std::uint32_t* FloatDestinations()
		{
			return reinterpret_cast<const std::uint32_t*>(REL::Offset(0x35ef210).address());
		}
		bool ControllerDestinationTablesKnown()
		{
			static const bool known = REL::Module::get().version() == REL::Version{ 1, 6, 1170, 0 };
			return known;
		}
		const std::uint32_t* ColourDestinations()
		{
			return reinterpret_cast<const std::uint32_t*>(REL::Offset(0x35ef298).address());
		}

		// BSLightingShaderPropertyFloatController::Update (AE 14150dde0, vtable slot 0x27). Type 0xb writes the
		// property's emissive multiplier (per-object shading, resampled every frame at Prepass) and types above
		// 0x13 the texture-transform buffers (captured for the scene work's transform watch); every other type a material field.
		struct FloatControllerUpdate
		{
			static void thunk(RE::NiTimeController* a_this, void* a_data)
			{
				const auto type = ControllerType(a_this);
				auto* property = static_cast<RE::BSLightingShaderProperty*>(a_this->target);
				if (type == 0xb && property) {
					const auto before = property->emissiveMult;
					func(a_this, a_data);
					if (std::memcmp(&before, &property->emissiveMult, sizeof(before)) != 0)
						ShadingQueue().Push(property);
				} else if (type > 0x13 && property && property->material) {
					auto* material = static_cast<RE::BSLightingShaderMaterialBase*>(property->material);
					const auto offset0 = material->texCoordOffset[0], offset1 = material->texCoordOffset[1];
					const auto scale0 = material->texCoordScale[0], scale1 = material->texCoordScale[1];
					func(a_this, a_data);
					if (property->material != material || offset0 != material->texCoordOffset[0] || offset1 != material->texCoordOffset[1] ||
						scale0 != material->texCoordScale[0] || scale1 != material->texCoordScale[1]) {
						// T6b2c step 7: the buffers as written, for the scene work's records (its transform watch). The property's material
						// now: alive, the property holding it (a swap during the update may have let `material` go; its slots follow the
						// swap's event).
						MaterialPort::PushCapture(property->material);
						ShadowTransformQueue().Push(material);
					}
				} else if (type <= 0x13 && type != 0xb && property && property->material && ControllerDestinationTablesKnown()) {
					auto* material = property->material;
					const auto offset = std::size_t(FloatDestinations()[type]) * sizeof(float);
					std::uint32_t before = 0;
					std::memcpy(&before, reinterpret_cast<const std::byte*>(material) + offset, sizeof(before));
					func(a_this, a_data);
					// A member's shading takes the material's alpha (MakeShading): its dependents are resampled too.
					if (property->material != material ||
						std::memcmp(&before, reinterpret_cast<const std::byte*>(material) + offset, sizeof(before)) != 0) {
						NoteWritten(material);
						ShadingQueue().Push(property);
					}
				} else {
					func(a_this, a_data);
					if (type != 0xb && type <= 0x13) {
						NoteTarget(a_this);
						if (property)
							ShadingQueue().Push(property);
					}
				}
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		// BSLightingShaderPropertyColorController::Update (AE 14150ea50, slot 0x27). Type 1 writes the property's
		// emissive colour (per-object shading); every other type a material colour.
		struct ColorControllerUpdate
		{
			static void thunk(RE::NiTimeController* a_this, void* a_data)
			{
				const auto type = ControllerType(a_this);
				auto* property = static_cast<RE::BSLightingShaderProperty*>(a_this->target);
				if (type == 1 && property && property->emissiveColor) {
					const auto before = *property->emissiveColor;
					func(a_this, a_data);
					if (!property->emissiveColor || std::memcmp(&before, property->emissiveColor, sizeof(before)) != 0)
						ShadingQueue().Push(property);
				} else if (type != 1 && property && property->material && ControllerDestinationTablesKnown()) {
					auto* material = property->material;
					const auto offset = std::size_t(ColourDestinations()[type]);
					std::array<std::byte, 12> before{};
					std::memcpy(before.data(), reinterpret_cast<const std::byte*>(material) + offset, before.size());
					func(a_this, a_data);
					if (property->material != material ||
						std::memcmp(before.data(), reinterpret_cast<const std::byte*>(material) + offset, before.size()) != 0)
						NoteWritten(material);
				} else {
					func(a_this, a_data);
					if (type != 1)
						NoteTarget(a_this);
				}
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
		if (WritesQueued())
			GetQueue().Push(a_material);
		// T6b2a: the material as written, for the scene work's records.
		MaterialPort::PushCapture(a_material);
	}

	bool Drain(ankerl::unordered_dense::set<const RE::BSShaderMaterial*>& a_out)
	{
		// The queue loses nothing (it spills when full), so a drain is always complete.
		GetQueue().Drain([&](const RE::BSShaderMaterial* a_material) { a_out.insert(a_material); });
		return true;
	}

	void DrainShadingChanges(std::vector<const void*>& a_out)
	{
		ShadingQueue().Drain([&](const void* a_property) { a_out.push_back(a_property); });
	}

	void DrainShadowTransformChanges(std::vector<const RE::BSShaderMaterial*>& a_out)
	{
		ShadowTransformQueue().Drain([&](const RE::BSShaderMaterial* a_material) { a_out.push_back(a_material); });
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
		if (a_passDescriptor & kCharacterLight)
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

	void KeepUnreadFloats(const MaterialRecord& a_from, MaterialRecord& a_to)
	{
		constexpr std::uint32_t kPSIBLParams = 29;
		const auto& layout = LightingPSLayout();
		for (std::uint32_t c = 0; c < layout.size[kPSIBLParams]; ++c)
			a_to.ps.floats[layout.offset[kPSIBLParams] + c] = a_from.ps.floats[layout.offset[kPSIBLParams] + c];
	}

	TextureTransforms TextureTransformsOf(const MaterialPort::MaterialSnapshot& a_snapshot)
	{
		// BSShaderMaterial's texCoordOffset (+0xc) and texCoordScale (+0x1c), 8 bytes a buffer (MaterialPortVanilla's and
		// MaterialPortFeature's TexcoordOffset read the same fields).
		constexpr std::size_t kOffset = 0xc, kScale = 0x1c;
		TextureTransforms out;
		for (std::size_t b = 0; b < out.buffers.size(); ++b)
			out.buffers[b] = { a_snapshot.At<float>(kOffset + b * 8), a_snapshot.At<float>(kOffset + b * 8 + 4), a_snapshot.At<float>(kScale + b * 8),
				a_snapshot.At<float>(kScale + b * 8 + 4) };
		return out;
	}

	bool ApplyTextureTransform(const TextureTransforms& a_transforms, std::uint32_t a_buffer, MaterialRecord& a_record)
	{
		const auto& values = a_transforms.buffers[a_buffer & 1];
		const auto& positions = FrameVSFloats();
		bool changed = false;
		for (std::size_t c = 0; c < positions.size() && c < values.size(); ++c)
			if (a_record.vs.Written(positions[c])) {
				changed |= std::bit_cast<std::uint32_t>(a_record.vs.floats[positions[c]]) != std::bit_cast<std::uint32_t>(values[c]);
				a_record.vs.floats[positions[c]] = values[c];
			}
		return changed;
	}

	ID3D11ShaderResourceView* FrameCharacterLightView(const MaterialPort::MaterialFrame& a_frame)
	{
		// What SetupMaterial binds at t11 (MaterialPortVanilla: the render target at 0x142033db0 while not negative; TruePBR's hook:
		// the character light image space texture's), from the frame's sample: no engine read.
		const auto& vanilla = a_frame.vanilla;
		if (vanilla.characterLightTarget >= 0 && static_cast<std::uint32_t>(vanilla.characterLightTarget) < MaterialPort::kVanillaRenderTargets)
			if (auto* view = vanilla.renderTargetViews[static_cast<std::size_t>(vanilla.characterLightTarget)])
				return view;
		const auto& feature = a_frame.feature;
		if (feature.characterLightTarget >= 0 && static_cast<std::uint32_t>(feature.characterLightTarget) < MaterialPort::kFeatureRenderTargets)
			return feature.renderTargetViews[static_cast<std::size_t>(feature.characterLightTarget)];
		return nullptr;
	}
}
