// FocusViews: the BSShadowDirectionalLight::sub detour (render thread) and the focus views it records (ShadowCameras::Focus).

#include "Features/DrawcallLimitFix/Engine/Views/FocusViews.h"

#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Render/Shadow/ShadowCameras.h"

#include "Features/DrawcallLimitFix/Scene/FrameView/FrameView.h"

#include "State.h"
#include "Features/DrawcallLimitFix/Diagnostics/Engine/Events.h"
#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"

namespace DCLF
{
	namespace
	{
		using Engine::At;
		using Engine::Global;

		constexpr std::uintptr_t kFocusCameras = 0x14f0480;  // BSShadowDirectionalLight::sub: the focus cameras, any host
		constexpr std::uintptr_t kTargets = 0x332a488;       // the focus targets: {float distance; NiAVObject* node} x count
	}

	FocusViews& FocusViews::Get()
	{
		static FocusViews instance;
		return instance;
	}

	std::uint32_t FocusViews::EngineTargetCount()
	{
		return Global<std::uint32_t>(Engine::Address::kFocusTargetCount);
	}

	const RE::NiAVObject* FocusViews::EngineTarget(std::uint32_t a_index)
	{
		const auto* targets = Global<const std::byte*>(kTargets);
		if (!targets || a_index >= EngineTargetCount())
			return nullptr;
		return At<const RE::NiAVObject*>(targets, std::size_t(a_index) * 16 + 8);
	}

	void FocusViews::Update(const RE::BSShadowLight* a_light)
	{
		auto& board = ShadowCameras::Focus();
		auto& hosts = board.hosts;
		const std::uint32_t frame = Scene::FrameView::Get().GetFrame();
		if (frame != board.sceneFrame) {
			if (!hosts.empty())
				Diagnostics::Notify<Diagnostics::EnginePoint::FocusViewsFrame>();
			hosts.clear();
			board.sceneFrame = frame;
		}
		// The targets as sub read them.
		constexpr std::uint32_t kMaxFocus = ShadowCameras::FocusFrame::kMaxFocus;
		const std::uint32_t targetCount = board.targetCount = std::min(EngineTargetCount(), kMaxFocus);
		for (std::uint32_t i = 0; i < kMaxFocus; ++i)
			board.targets[i] = i < targetCount ? EngineTarget(i) : nullptr;
		ShadowCameras::FocusFrame::Host* host = nullptr;
		for (auto& existing : hosts)
			if (existing.light == a_light)
				host = &existing;
		if (!host) {
			host = &hosts.emplace_back();
			host->light = a_light;
		}
		auto& data = const_cast<RE::BSShadowLight*>(a_light)->GetRuntimeData();
		host->views.assign(targetCount, ShadowCameras::Camera{});
		for (std::uint32_t d = 0; d < targetCount; ++d) {
			const auto* camera = data.focusShadowmapDescriptors[d].camera.get();
			if (!camera)
				continue;
			auto& view = host->views[d];
			ShadowCameras::CameraMatrices(*camera, view);
			ShadowCameras::FrustumPlanes(const_cast<RE::NiCamera*>(camera)->GetRuntimeData2().viewFrustum, camera->world, view.frustum);
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
