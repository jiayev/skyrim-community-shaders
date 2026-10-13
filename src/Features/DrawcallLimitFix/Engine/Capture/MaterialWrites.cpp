// MaterialWrites: the controller and material vtable hooks (on whichever thread runs the engine's update) and their lock-free queues.

#include "Features/DrawcallLimitFix/Engine/Capture/MaterialWrites.h"

#include "Features/DrawcallLimitFix/Runtime/EventQueue.h"
#include "Features/DrawcallLimitFix/Runtime/Toggles.h"
#include "Features/DrawcallLimitFix/Diagnostics/Observation.h"
#include "Features/DrawcallLimitFix/Engine/Access/EngineAccess.h"
#include "Features/DrawcallLimitFix/Scene/Ports/MaterialPort.h"

#include <array>
#include <cstddef>
#include <cstring>

namespace DCLF::MaterialWrites
{
	namespace
	{
		// ---- The write queue (EventQueue): CS_DCLF_CAPTURE_PARITY's diagnostics alone, drained by the render thread once a frame; a
		// producer never blocks. The records follow the writes' captures (MaterialPort::PushCapture), not this queue.
		EventQueue<const RE::BSShaderMaterial*, 8192>& GetQueue()
		{
			static EventQueue<const RE::BSShaderMaterial*, 8192> queue;
			return queue;
		}
		bool WritesQueued()
		{
			return Diagnostics::Observe().capture;
		}
		EventQueue<const void*, 8192>& ShadingQueue()
		{
			// The scene pass drains it (NameShadingEvents) for the next frame's shading sample: per-frame, so it wakes no pass (the next
			// frame input's takes it).
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
			return reinterpret_cast<const std::uint32_t*>(REL::Offset(Engine::Address::kFloatControllerTargets).address());
		}
		bool ControllerDestinationTablesKnown()
		{
			static const bool known = REL::Module::get().version() == REL::Version{ 1, 6, 1170, 0 };
			return known;
		}
		const std::uint32_t* ColourDestinations()
		{
			return reinterpret_cast<const std::uint32_t*>(REL::Offset(Engine::Address::kColorControllerTargets).address());
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
						// The buffers as written, for the scene work's records (its transform watch). The property's current material:
						// alive, the property holding it (a swap during the update may have let `material` go; its slots follow the swap's
						// event).
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

		// The Effect port: what an effect controller can write of its property's BSEffectShaderMaterial (0x14150f090, 0x14150f700): the
		// texture-transform buffers (+0x0c..+0x2c: UV scroll, types 6 and up), the falloff (+0x38..+0x44, types 0-4) and the base colour
		// (+0x48..+0x54: the colour, type 5's alpha through SetMaterialAlpha).
		constexpr std::size_t kEffectWrittenFirst = 0x0c, kEffectWrittenEnd = 0x58;
		using EffectWritten = std::array<std::byte, kEffectWrittenEnd - kEffectWrittenFirst>;
		EffectWritten EffectWrittenOf(const RE::BSShaderMaterial* a_material)
		{
			EffectWritten out{};
			if (a_material)
				std::memcpy(out.data(), reinterpret_cast<const std::byte*>(a_material) + kEffectWrittenFirst, out.size());
			return out;
		}
		// An effect controller's update: the material captured when it moved (its records, the transform watch), and the property's shading
		// resampled (an effect member's PropertyColor: the material's alpha, the emittance colour). A colour controller of any other type
		// writes a property colour (its table 0x1435ef2e8): its shading is resampled.
		void EffectControllerUpdate(RE::NiTimeController* a_this, bool a_propertyColour, void (*a_func)(RE::NiTimeController*, void*), void* a_data)
		{
			// Nothing is captured while the toggle is off: no effect record exists then (the scene work asks for a material it lacks:
			// FrameView::ServeMaterialRequests).
			if (!ActiveToggles().effects) {
				a_func(a_this, a_data);
				return;
			}
			auto* property = static_cast<RE::BSShaderProperty*>(a_this->target);
			auto* material = property ? property->material : nullptr;
			const auto before = EffectWrittenOf(material);
			a_func(a_this, a_data);
			if (!property)
				return;
			if (property->material != material || (material && EffectWrittenOf(material) != before)) {
				if (property->material)
					NoteWritten(property->material);
				ShadingQueue().Push(property);
			} else if (a_propertyColour) {
				ShadingQueue().Push(property);
			}
		}
		struct EffectFloatControllerUpdate
		{
			static void thunk(RE::NiTimeController* a_this, void* a_data)
			{
				EffectControllerUpdate(a_this, false, [](RE::NiTimeController* a_c, void* a_d) { func(a_c, a_d); }, a_data);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
		struct EffectColorControllerUpdate
		{
			static void thunk(RE::NiTimeController* a_this, void* a_data)
			{
				EffectControllerUpdate(a_this, ControllerType(a_this) != 0, [](RE::NiTimeController* a_c, void* a_d) { func(a_c, a_d); }, a_data);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
		// BSEffectShaderMaterial::CopyMembers (02, 0x1414e9cf0): its in-place rewrite.
		struct EffectCopyMembers
		{
			static void thunk(RE::BSShaderMaterial* a_this, RE::BSShaderMaterial* a_that)
			{
				func(a_this, a_that);
				NoteWritten(a_this);
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
	}

	void Install()
	{
		stl::write_vfunc<0x27, FloatControllerUpdate>(RE::VTABLE_BSLightingShaderPropertyFloatController[0]);
		stl::write_vfunc<0x27, ColorControllerUpdate>(RE::VTABLE_BSLightingShaderPropertyColorController[0]);
		InstallMaterialHooks(Engine::LightingMaterialVtables(), std::make_index_sequence<14>{});
		// The Effect port: the effect controllers and the effect material's rewrite (installed whatever the toggle; the controllers capture
		// nothing while it is off).
		stl::write_vfunc<0x27, EffectFloatControllerUpdate>(RE::VTABLE_BSEffectShaderPropertyFloatController[0]);
		stl::write_vfunc<0x27, EffectColorControllerUpdate>(RE::VTABLE_BSEffectShaderPropertyColorController[0]);
		stl::write_vfunc<0x02, EffectCopyMembers>(RE::VTABLE_BSEffectShaderMaterial[0]);
		logger::info("[DCLF] material write hooks installed: float and colour controllers, 14 material vtables, the effect controllers and material");
	}

	void NoteWritten(const RE::BSShaderMaterial* a_material)
	{
		if (!a_material)
			return;
		if (WritesQueued())
			GetQueue().Push(a_material);
		// The material as written, for the scene work's records.
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
}
