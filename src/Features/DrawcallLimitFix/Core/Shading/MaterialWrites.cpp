// MaterialWrites: the controller and material writes (SceneWriterFramework's call hooks, on whichever thread runs the engine's update) and their lock-free queues, which
// the features' hooks reach through the material-write sink (Api/MaterialWrites.h, defined here).

#include "Features/DrawcallLimitFix/Core/Shading/MaterialWrites.h"

#include "Features/DrawcallLimitFix/Core/Engine/Capture/SceneWriter.h"

#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Extensions/MaterialWrites.h"
#include "Features/DrawcallLimitFix/Core/Runtime/EventQueue.h"
#include "Features/DrawcallLimitFix/Core/Shading/MaterialPort.h"
#include "Features/DrawcallLimitFix/Diagnostics/Observation.h"

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
		// The transform moves (Extensions::DrainTransformMoves): the kept shadow build's (ShadowKept), whatever patches the records'.
		EventQueue<const RE::BSShaderMaterial*, 8192>& TransformMoves()
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

		// The controllers' and materials' writes are SceneWriterFramework's call hooks (SWF_EVENT_CALL): each controller handler records
		// on PRE which of its cases the update is and the values it compares (scratch[0] the case, [6] the type, [7] the property), and
		// finishes that case on POST.
		enum ControllerCase : std::uintptr_t
		{
			kEmissive = 1,  // the property's emissive multiplier or colour (per-object shading)
			kTransforms,    // the texture transforms (float controller types above 0x13)
			kDestination,   // a material member the AE destination tables name
			kOther,         // anything else: the conservative notification
		};

		// BSLightingShaderPropertyFloatController::Update (AE 14150dde0, vtable slot 0x27). Type 0xb writes the
		// property's emissive multiplier (per-object shading, resampled every frame at Prepass) and types above
		// 0x13 the texture-transform buffers; every other type a material field.
		// While a feature patches the frame VS variable (InstallContext::RegisterFrameVSVariablePatch), the texture transforms are no
		// record's: the GPU patches every material row's TexcoordOffset from the buffer the frame reads, so a write that moved only them
		// captures nothing; the kept shadow build still hears of it (TransformMoves: its rows read the transform themselves).
		void OnFloatController(const SWF_EventHeader* a_event, std::uintptr_t* a_scratch, void*)
		{
			auto* controller = SceneWriter::Arg<RE::NiTimeController*>(a_event, 0);
			if (SceneWriter::IsPre(a_event)) {
				const auto type = ControllerType(controller);
				auto* property = static_cast<RE::BSLightingShaderProperty*>(controller->target);
				a_scratch[6] = type;
				a_scratch[7] = reinterpret_cast<std::uintptr_t>(property);
				if (type == 0xb && property) {
					a_scratch[0] = kEmissive;
					std::memcpy(&a_scratch[1], &property->emissiveMult, sizeof(property->emissiveMult));
				} else if (type > 0x13 && property && property->material) {
					auto* material = static_cast<RE::BSLightingShaderMaterialBase*>(property->material);
					a_scratch[0] = kTransforms;
					a_scratch[1] = reinterpret_cast<std::uintptr_t>(material);
					static_assert(sizeof(material->texCoordOffset) + sizeof(material->texCoordScale) <= 4 * sizeof(std::uintptr_t));
					std::memcpy(&a_scratch[2], material->texCoordOffset, sizeof(material->texCoordOffset));
					std::memcpy(&a_scratch[4], material->texCoordScale, sizeof(material->texCoordScale));
				} else if (type <= 0x13 && type != 0xb && property && property->material && ControllerDestinationTablesKnown()) {
					auto* material = property->material;
					const auto offset = std::size_t(FloatDestinations()[type]) * sizeof(float);
					a_scratch[0] = kDestination;
					a_scratch[1] = reinterpret_cast<std::uintptr_t>(material);
					std::memcpy(&a_scratch[2], reinterpret_cast<const std::byte*>(material) + offset, sizeof(std::uint32_t));
				} else
					a_scratch[0] = kOther;
				return;
			}
			const auto type = static_cast<std::uint32_t>(a_scratch[6]);
			auto* property = reinterpret_cast<RE::BSLightingShaderProperty*>(a_scratch[7]);
			switch (a_scratch[0]) {
			case kEmissive:
				if (std::memcmp(&a_scratch[1], &property->emissiveMult, sizeof(property->emissiveMult)) != 0)
					ShadingQueue().Push(property);
				break;
			case kTransforms:
				{
					auto* material = reinterpret_cast<RE::BSLightingShaderMaterialBase*>(a_scratch[1]);
					// A swap during the update is the property's: its current material (alive, the property holding it) is captured.
					if (property->material != material && property->material)
						MaterialPort::PushCapture(property->material);
					const bool moved = property->material != material ||
					                   std::memcmp(&a_scratch[2], material->texCoordOffset, sizeof(material->texCoordOffset)) != 0 ||
					                   std::memcmp(&a_scratch[4], material->texCoordScale, sizeof(material->texCoordScale)) != 0;
					// Unless a feature patches the frame's (InstallContext::RegisterFrameVSVariablePatch), the transforms are the records': captured.
					if (moved && property->material == material && !Extensions::FrameVSVariablePatched())
						NoteWritten(material);
					if (moved)
						TransformMoves().Push(material);
					break;
				}
			case kDestination:
				{
					auto* material = reinterpret_cast<RE::BSShaderMaterial*>(a_scratch[1]);
					const auto offset = std::size_t(FloatDestinations()[type]) * sizeof(float);
					// A member's shading takes the material's alpha (MakeShading): its dependents are resampled too.
					if (property->material != material ||
						std::memcmp(&a_scratch[2], reinterpret_cast<const std::byte*>(material) + offset, sizeof(std::uint32_t)) != 0) {
						NoteWritten(material);
						ShadingQueue().Push(property);
					}
					break;
				}
			default:
				if (type != 0xb && type <= 0x13) {
					NoteTarget(controller);
					if (property)
						ShadingQueue().Push(property);
				}
				break;
			}
		}

		// BSLightingShaderPropertyColorController::Update (AE 14150ea50, slot 0x27). Type 1 writes the property's
		// emissive colour (per-object shading); every other type a material colour.
		void OnColorController(const SWF_EventHeader* a_event, std::uintptr_t* a_scratch, void*)
		{
			constexpr std::size_t kColour = 12;
			auto* controller = SceneWriter::Arg<RE::NiTimeController*>(a_event, 0);
			if (SceneWriter::IsPre(a_event)) {
				const auto type = ControllerType(controller);
				auto* property = static_cast<RE::BSLightingShaderProperty*>(controller->target);
				a_scratch[6] = type;
				a_scratch[7] = reinterpret_cast<std::uintptr_t>(property);
				if (type == 1 && property && property->emissiveColor) {
					a_scratch[0] = kEmissive;
					std::memcpy(&a_scratch[1], property->emissiveColor, kColour);
				} else if (type != 1 && property && property->material && ControllerDestinationTablesKnown()) {
					auto* material = property->material;
					a_scratch[0] = kDestination;
					a_scratch[1] = reinterpret_cast<std::uintptr_t>(material);
					std::memcpy(&a_scratch[2], reinterpret_cast<const std::byte*>(material) + ColourDestinations()[type], kColour);
				} else
					a_scratch[0] = kOther;
				return;
			}
			const auto type = static_cast<std::uint32_t>(a_scratch[6]);
			auto* property = reinterpret_cast<RE::BSLightingShaderProperty*>(a_scratch[7]);
			switch (a_scratch[0]) {
			case kEmissive:
				if (!property->emissiveColor || std::memcmp(&a_scratch[1], property->emissiveColor, kColour) != 0)
					ShadingQueue().Push(property);
				break;
			case kDestination:
				{
					auto* material = reinterpret_cast<RE::BSShaderMaterial*>(a_scratch[1]);
					if (property->material != material ||
						std::memcmp(&a_scratch[2], reinterpret_cast<const std::byte*>(material) + ColourDestinations()[type], kColour) != 0)
						NoteWritten(material);
					break;
				}
			default:
				if (type != 1)
					NoteTarget(controller);
				break;
			}
		}

		// The material's in-place rewrites (BSShaderMaterial 02, BSLightingShaderMaterialBase 08-0A) on the 14 Lighting material classes.
		void OnMaterialRewrite(const SWF_EventHeader* a_event, std::uintptr_t*, void*)
		{
			NoteWritten(SceneWriter::Arg<RE::BSShaderMaterial*>(a_event, 0));
		}
	}

	void Install()
	{
		// SceneWriterFramework's material call family, which DrawcallLimitFix::PostPostLoad required (SceneWriter::kRequiredCaps).
		bool subscribed = SceneWriter::SubscribeCall(SWF_HOOK_FLOAT_CONTROLLER_UPDATE, &OnFloatController) &&
		                  SceneWriter::SubscribeCall(SWF_HOOK_COLOR_CONTROLLER_UPDATE, &OnColorController);
		for (const std::uint32_t hook : { SWF_HOOK_MATERIAL_COPY_MEMBERS, SWF_HOOK_MATERIAL_LOAD_TEXTURE_SET, SWF_HOOK_MATERIAL_CLEAR_TEXTURES,
				 SWF_HOOK_MATERIAL_RECEIVE_VALUES })
			subscribed = subscribed && SceneWriter::SubscribeCall(hook, &OnMaterialRewrite, SWF_PHASE_POST);
		if (!subscribed)
			stl::report_and_fail("Drawcall Limit Fix: the scene writer framework refused the material write hooks after reporting them installed");
		// The Effect port's controllers and material rewrite are Features/Effects' (EffectWrites.cpp, at its install), through Api/MaterialWrites.h.
		logger::info("[DCLF] material write hooks subscribed: float and colour controllers, 14 material classes");
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
}

namespace DCLF::Extensions
{
	// The transform moves' consumer side (Core/Extensions/MaterialWrites.h); the queue is the hooks'.
	void DrainTransformMoves(std::vector<const RE::BSShaderMaterial*>& a_out)
	{
		MaterialWrites::TransformMoves().Drain([&](const RE::BSShaderMaterial* a_material) { a_out.push_back(a_material); });
	}
}

namespace DCLF::Api
{
	// The material-write sink (Api/MaterialWrites.h): a feature's hooks reach the write queues through it.
	void NoteMaterialWritten(const RE::BSShaderMaterial* a_material)
	{
		MaterialWrites::NoteWritten(a_material);
	}

	void NotePropertyShading(const RE::BSShaderProperty* a_property)
	{
		MaterialWrites::ShadingQueue().Push(a_property);
	}
}
