// The Effect port's material writes (SceneWriterFramework's effect calls): the effect controllers' updates and the effect material's
// in-place rewrite (on whichever thread runs the engine's update), each write named to Core through the material-write sink (Api/MaterialWrites.h): the material captured for
// its records, the property's shading resampled.

#include "Features/DrawcallLimitFix/Api/MaterialWrites.h"
#include "Features/DrawcallLimitFix/Core/Engine/Capture/SceneWriter.h"
#include "Features/DrawcallLimitFix/Core/Extensions/MaterialPorts.h"
#include "Features/DrawcallLimitFix/Core/Extensions/MaterialWrites.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ObjectContributors.h"
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"
#include "Features/DrawcallLimitFix/Features/Effects/EffectConstants.h"
#include "Features/DrawcallLimitFix/Features/Effects/Effects.h"

#include <array>
#include <cstddef>
#include <cstring>
#include <vector>

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
		constexpr std::size_t kEffectBaseAlpha = 0x54;  // baseColor.alpha: what an effect member's shading reads of them (SampleEffectShading)
		using EffectWritten = std::array<std::byte, kEffectWrittenEnd - kEffectWrittenFirst>;
		EffectWritten EffectWrittenOf(const RE::BSShaderMaterial* a_material)
		{
			EffectWritten out{};
			if (a_material)
				std::memcpy(out.data(), reinterpret_cast<const std::byte*>(a_material) + kEffectWrittenFirst, out.size());
			return out;
		}
		// An effect controller's update, from PRE to POST: what its property and material were, and the bytes the update can write.
		// Kept on a per-thread stack (an update inside another stacks), since the bytes outrun the event's scratch.
		struct Update
		{
			RE::BSShaderProperty* property;
			RE::BSShaderMaterial* material;
			EffectWritten before;
			bool propertyColour;
			bool captured;  // the Effect class was on at PRE
		};
		thread_local std::vector<Update> updates;

		// The material captured when what its records read moved, and the property's shading resampled when what that reads did (an
		// effect member's PropertyColor: the material's alpha, the emittance colour). A colour controller of any other type writes a
		// property colour (its table 0x1435ef2e8): its shading is resampled.
		void EffectControllerUpdate(const SWF_EventHeader* a_event, bool a_propertyColour)
		{
			if (SceneWriter::IsPre(a_event)) {
				// Nothing is captured while the toggle is off: no effect record exists then (the scene work asks for a material it lacks:
				// FrameView::ServeMaterialRequests).
				auto* controller = SceneWriter::Arg<RE::NiTimeController*>(a_event, 0);
				auto* property = static_cast<RE::BSShaderProperty*>(controller->target);
				auto* material = property ? property->material : nullptr;
				const bool captured = Extensions::ObjectClassOn(SceneToggles(), Api::ObjectClass::Effect);
				updates.push_back({ property, material, captured ? EffectWrittenOf(material) : EffectWritten{}, a_propertyColour, captured });
				return;
			}
			const Update update = updates.back();
			updates.pop_back();
			auto* property = update.property;
			auto* material = update.material;
			if (!update.captured || !property)
				return;
			// The bytes the frame owns while a feature patches them (Features/TextureTransforms, InstallContext::RegisterFrameVSVariablePatch:
			// the texture transforms, a UV scroll, and the port's frame variables, the falloff and the base colour: Extensions' frame spans)
			// are the frame values': no record reads them, so a write confined to them captures nothing. Unpatched, they are the record's:
			// compared with the rest. A swapped material is captured whatever moved.
			const auto& before = update.before;
			const auto after = material ? EffectWrittenOf(material) : EffectWritten{};
			const bool swapped = property->material != material;
			const bool moved = material && (Extensions::FrameVSVariablePatched() ?
												   Extensions::DifferOutsideFrameSpans(kMaterialKindEffect, before.data(), after.data(), kEffectWrittenFirst, kEffectWrittenEnd) :
												   std::memcmp(after.data(), before.data(), after.size()) != 0);
			if ((swapped || moved) && property->material)
				Api::NoteMaterialWritten(property->material);
			// The shading reads the base colour's alpha of them (a falloff or colour write leaves it alone).
			const auto alpha = kEffectBaseAlpha - kEffectWrittenFirst;
			if (swapped || update.propertyColour || (material && std::memcmp(after.data() + alpha, before.data() + alpha, sizeof(float)) != 0))
				Api::NotePropertyShading(property);
		}

		void OnEffectFloatController(const SWF_EventHeader* a_event, std::uintptr_t*, void*)
		{
			EffectControllerUpdate(a_event, false);
		}

		void OnEffectColorController(const SWF_EventHeader* a_event, std::uintptr_t*, void*)
		{
			EffectControllerUpdate(a_event, SceneWriter::IsPre(a_event) && ControllerType(SceneWriter::Arg<RE::NiTimeController*>(a_event, 0)) != 0);
		}

		// BSEffectShaderMaterial::CopyMembers (02, 0x1414e9cf0): its in-place rewrite.
		void OnEffectCopyMembers(const SWF_EventHeader* a_event, std::uintptr_t*, void*)
		{
			Api::NoteMaterialWritten(SceneWriter::Arg<RE::BSShaderMaterial*>(a_event, 0));
		}
	}

	void InstallWrites()
	{
		// The effect controllers and the effect material's rewrite (installed whatever the toggle; the controllers capture nothing while it
		// is off).
		// SceneWriterFramework's effect calls, which DrawcallLimitFix::PostPostLoad required (SceneWriter::kRequiredCaps).
		const bool subscribed = SceneWriter::SubscribeCall(SWF_HOOK_EFFECT_FLOAT_CONTROLLER_UPDATE, &OnEffectFloatController) &&
		                        SceneWriter::SubscribeCall(SWF_HOOK_EFFECT_COLOR_CONTROLLER_UPDATE, &OnEffectColorController) &&
		                        SceneWriter::SubscribeCall(SWF_HOOK_EFFECT_MATERIAL_COPY_MEMBERS, &OnEffectCopyMembers, SWF_PHASE_POST);
		if (!subscribed)
			stl::report_and_fail("Drawcall Limit Fix: the scene writer framework refused the effect write hooks after reporting them installed");
		logger::info("[DCLF] effect write hooks subscribed: the effect controllers and material");
	}
}
