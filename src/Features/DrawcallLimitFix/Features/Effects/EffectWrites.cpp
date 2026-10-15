// The Effect port's material-write hooks: the effect controllers' updates and the effect material's in-place rewrite (on whichever thread
// runs the engine's update), each write named to Core through the material-write sink (Api/MaterialWrites.h): the material captured for
// its records, the property's shading resampled.

#include "Features/DrawcallLimitFix/Features/Effects/Effects.h"
#include "Features/DrawcallLimitFix/Api/MaterialWrites.h"
#include "Features/DrawcallLimitFix/Core/Extensions/MaterialWrites.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ObjectContributors.h"
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"

#include <array>
#include <cstddef>
#include <cstring>

namespace DCLF::Features::Effects
{
	namespace
	{
		// A controller's own type field (+0x50): which member of the property or material it writes.
		std::uint32_t ControllerType(const RE::NiTimeController* a_controller)
		{
			return *reinterpret_cast<const std::uint32_t*>(reinterpret_cast<const std::byte*>(a_controller) + 0x50);
		}

		// The Effect port: what an effect controller can write of its property's BSEffectShaderMaterial (0x14150f090, 0x14150f700): the
		// texture-transform buffers (+0x0c..+0x2c: UV scroll, types 6 and up), the falloff (+0x38..+0x44, types 0-4) and the base colour
		// (+0x48..+0x54: the colour, type 5's alpha through SetMaterialAlpha).
		constexpr std::size_t kEffectWrittenFirst = 0x0c, kEffectWrittenEnd = 0x58;
		constexpr std::size_t kEffectTransformsEnd = 0x2c;  // the texture-transform buffers lead the written span
		using EffectWritten = std::array<std::byte, kEffectWrittenEnd - kEffectWrittenFirst>;
		EffectWritten EffectWrittenOf(const RE::BSShaderMaterial* a_material)
		{
			EffectWritten out{};
			if (a_material)
				std::memcpy(out.data(), reinterpret_cast<const std::byte*>(a_material) + kEffectWrittenFirst, out.size());
			return out;
		}
		// An effect controller's update: the material captured when it moved (its records), and the property's shading resampled (an effect
		// member's PropertyColor: the material's alpha, the emittance colour). A colour controller of any other type writes a property colour
		// (its table 0x1435ef2e8): its shading is resampled.
		void EffectControllerUpdate(RE::NiTimeController* a_this, bool a_propertyColour, void (*a_func)(RE::NiTimeController*, void*), void* a_data)
		{
			// Nothing is captured while the toggle is off: no effect record exists then (the scene work asks for a material it lacks:
			// FrameView::ServeMaterialRequests).
			if (!Extensions::ObjectClassOn(ActiveToggles(), Api::ObjectClass::Effect)) {
				a_func(a_this, a_data);
				return;
			}
			auto* property = static_cast<RE::BSShaderProperty*>(a_this->target);
			auto* material = property ? property->material : nullptr;
			const auto before = EffectWrittenOf(material);
			a_func(a_this, a_data);
			if (!property)
				return;
			// The texture transforms alone (a UV scroll) are the frame values' while a feature patches the frame VS variable
			// (Features/TextureTransforms, InstallContext::RegisterFrameVSVariablePatch): neither the record nor the shading reads them, so such a
			// write captures nothing. Unpatched, they are the record's: compared with the rest.
			const auto after = material ? EffectWrittenOf(material) : EffectWritten{};
			const std::size_t transforms = Extensions::FrameVSVariablePatched() ? kEffectTransformsEnd - kEffectWrittenFirst : 0;
			const bool fields = property->material != material ||
			                    (material && std::memcmp(after.data() + transforms, before.data() + transforms, after.size() - transforms) != 0);
			if (fields) {
				if (property->material)
					Api::NoteMaterialWritten(property->material);
				Api::NotePropertyShading(property);
			} else if (a_propertyColour) {
				Api::NotePropertyShading(property);
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
				Api::NoteMaterialWritten(a_this);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	void InstallWrites()
	{
		// The effect controllers and the effect material's rewrite (installed whatever the toggle; the controllers capture nothing while it
		// is off).
		stl::write_vfunc<0x27, EffectFloatControllerUpdate>(RE::VTABLE_BSEffectShaderPropertyFloatController[0]);
		stl::write_vfunc<0x27, EffectColorControllerUpdate>(RE::VTABLE_BSEffectShaderPropertyColorController[0]);
		stl::write_vfunc<0x02, EffectCopyMembers>(RE::VTABLE_BSEffectShaderMaterial[0]);
		logger::info("[DCLF] effect write hooks installed: the effect controllers and material");
	}
}
