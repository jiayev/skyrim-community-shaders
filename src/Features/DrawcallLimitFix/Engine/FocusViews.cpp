#include "FocusViews.h"

#include "EngineAccess.h"

#include "Features/DrawcallLimitFix/Scene/SceneStore.h"

#include "State.h"

namespace DCLF
{
	namespace
	{
		using Engine::At;
		using Engine::Global;

		constexpr std::uintptr_t kFocusCameras = 0x14f0480;  // BSShadowDirectionalLight::sub: the focus cameras, any host
		constexpr std::uintptr_t kTargets = 0x332a488;       // the focus targets: {float distance; NiAVObject* node} x count
		constexpr std::uintptr_t kTargetCount = 0x332a498;
	}

	FocusViews& FocusViews::Get()
	{
		static FocusViews instance;
		return instance;
	}

	std::uint32_t FocusViews::EngineTargetCount()
	{
		return Global<std::uint32_t>(kTargetCount);
	}

	const RE::NiAVObject* FocusViews::EngineTarget(std::uint32_t a_index)
	{
		const auto* targets = Global<const std::byte*>(kTargets);
		if (!targets || a_index >= EngineTargetCount())
			return nullptr;
		return At<const RE::NiAVObject*>(targets, std::size_t(a_index) * 16 + 8);
	}

	const SunViews::Cascade* FocusViews::CascadeOf(const RE::BSShadowLight* a_light, std::uint32_t a_descriptor, std::uint32_t a_sceneFrame) const
	{
		if (a_sceneFrame != sceneFrame)
			return nullptr;
		for (const auto& host : hosts)
			if (host.light == a_light)
				return a_descriptor < host.views.size() && host.views[a_descriptor].valid ? &host.views[a_descriptor] : nullptr;
		return nullptr;
	}

	const RE::NiAVObject* FocusViews::TargetOf(std::uint32_t a_descriptor, std::uint32_t a_sceneFrame) const
	{
		return a_sceneFrame == sceneFrame && a_descriptor < targetCount ? targets[a_descriptor] : nullptr;
	}

	void FocusViews::NoteRegistration(std::uint32_t a_descriptor, const void* a_geometry)
	{
		// DCLF's casters for focus view i are the members whose fade root (their property's fadeNode) is target i: a geometry the
		// engine registers there under another root is one DCLF would withhold and not draw.
		registrations.fetch_add(1, std::memory_order_relaxed);
		const auto* geometry = static_cast<const RE::BSGeometry*>(a_geometry);
		const auto* property = geometry ? geometry->GetGeometryRuntimeData().shaderProperty.get() : nullptr;
		const RE::NiAVObject* root = property ? property->fadeNode : nullptr;
		const auto* target = EngineTarget(a_descriptor);
		if (root != target && outside.fetch_add(1, std::memory_order_relaxed) == 0) {
			std::scoped_lock lock(firstMutex);
			firstOutside = fmt::format("'{}' in focus view {} under fade root '{}', the target '{}'", geometry && geometry->name.c_str() ? geometry->name.c_str() : "",
				a_descriptor, root && root->name.c_str() ? root->name.c_str() : "none", target && target->name.c_str() ? target->name.c_str() : "none");
		}
	}

	void FocusViews::Report()
	{
		auto& p = parity;
		const std::uint64_t differ = p.viewProj + p.eye + p.viewport + p.casters + p.missing;
		logger::info("[DCLF] focus views (T3c: DCLF's, from the cameras BSShadowDirectionalLight::sub aimed, against the engine's): {} views ({} matrices, {} eye, "
					 "{} viewport, {} caster volume differ), {} without DCLF's{}{}",
			p.views, p.viewProj, p.eye, p.viewport, p.casters, p.missing, differ ? " <- FOCUS VIEW" : " <- OK", p.first.empty() ? "" : "; first: " + p.first);
		p = {};
		// The membership check.
		const std::uint64_t seen = registrations.exchange(0, std::memory_order_relaxed);
		const std::uint64_t out = outside.exchange(0, std::memory_order_relaxed);
		std::string named;
		{
			std::scoped_lock lock(firstMutex);
			if (out && !firstOutside.empty())
				named = "; first: " + firstOutside;
			firstOutside.clear();
		}
		logger::info("[DCLF] focus members (T3c: the engine's registrations into DCLF's focus views against their targets' fade roots): {} registrations, {} under "
					 "another root{}{}",
			seen, out, out ? " <- FOCUS MEMBERS" : " <- OK", named);
	}

	void FocusViews::Update(const RE::BSShadowLight* a_light)
	{
		const std::uint32_t frame = SceneStore::Get().GetFrame();
		if (frame != sceneFrame) {
			if (!hosts.empty() && SunViews::ParityEnabled() && ++reportFrames >= 300) {
				reportFrames = 0;
				Report();
			}
			hosts.clear();
			sceneFrame = frame;
		}
		// The targets as sub read them.
		targetCount = std::min(EngineTargetCount(), kMaxFocus);
		for (std::uint32_t i = 0; i < kMaxFocus; ++i)
			targets[i] = i < targetCount ? EngineTarget(i) : nullptr;
		Host* host = nullptr;
		for (auto& existing : hosts)
			if (existing.light == a_light)
				host = &existing;
		if (!host) {
			host = &hosts.emplace_back();
			host->light = a_light;
		}
		auto& data = const_cast<RE::BSShadowLight*>(a_light)->GetRuntimeData();
		host->views.assign(targetCount, SunViews::Cascade{});
		for (std::uint32_t d = 0; d < targetCount; ++d) {
			const auto* camera = data.focusShadowmapDescriptors[d].camera.get();
			if (!camera)
				continue;
			auto& view = host->views[d];
			SunViews::CameraMatrices(*camera, view);
			SunViews::FrustumPlanes(const_cast<RE::NiCamera*>(camera)->GetRuntimeData2().viewFrustum, camera->world, view.frustum);
			// No caster volume and no cull: the focus accumulation registers the target's subtree whole.
			view.valid = true;
		}
	}

	struct FocusViews::Hooks
	{
		struct Cameras
		{
			static void thunk(RE::BSShadowLight* a_light, RE::NiCamera* a_camera)
			{
				func(a_light, a_camera);
				FocusViews::Get().Update(a_light);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	};

	void FocusViews::Install()
	{
		if (installed)
			return;
		stl::detour_thunk<Hooks::Cameras>(REL::Offset(kFocusCameras).address());
		installed = true;
		logger::info("[DCLF] focus views: BSShadowDirectionalLight::sub detoured (T3c)");
	}
}
