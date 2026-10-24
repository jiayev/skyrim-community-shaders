// FocusViews: the BSShadowDirectionalLight::sub detour (render thread) and the focus views it records (FocusBoard), the Focus source's
// camera over them, and the engine's focus targets.

#include "Features/DrawcallLimitFix/Features/Shadows/Local/FocusViews.h"

#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ShadowViews.h"
#include "Features/DrawcallLimitFix/Core/Render/Shadow/ShadowCameras.h"

#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"

#include "State.h"
#include "Features/DrawcallLimitFix/Diagnostics/Engine/Events.h"
#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"

namespace DCLF::Features::Shadows::Local
{
	namespace
	{
		// Engine::Offsets (Address Library ids).
		constexpr DCLF::Engine::Offset kFocusCameras{ { 107601 }, { 100817 } };  // BSShadowDirectionalLight::sub: the focus cameras, any host
		constexpr DCLF::Engine::Offset kFocusTargets{ { 414623 }, { 527701 } };  // the focus targets: {float distance; NiAVObject* node} x count
	}

	std::uint32_t EngineFocusTargetCount()
	{
		return Engine::Global<std::uint32_t>(Engine::Address::kFocusTargetCount);
	}

	const RE::NiAVObject* EngineFocusTarget(std::uint32_t a_index)
	{
		const auto* targets = Engine::Global<const std::byte*>(kFocusTargets);
		if (!targets || a_index >= EngineFocusTargetCount())
			return nullptr;
		return Engine::At<const RE::NiAVObject*>(targets, std::size_t(a_index) * 16 + 8);
	}

	FocusFrame& FocusBoard()
	{
		static FocusFrame frame;
		return frame;
	}

	void FocusCamera(Api::ShadowSourceCamera& a_camera)
	{
		const auto& board = FocusBoard();
		a_camera.camera = board.CascadeOf(a_camera.light, a_camera.descriptor, a_camera.sceneFrame);
		a_camera.focusTarget = board.TargetOf(a_camera.descriptor, a_camera.sceneFrame);
	}

	FocusViews& FocusViews::Get()
	{
		static FocusViews instance;
		return instance;
	}

	void FocusViews::Update(const RE::BSShadowLight* a_light)
	{
		auto& board = FocusBoard();
		auto& hosts = board.hosts;
		const std::uint32_t frame = Scene::FrameView::Get().GetFrame();
		if (frame != board.sceneFrame) {
			if (!hosts.empty())
				Diagnostics::Notify<Diagnostics::EnginePoint::FocusViewsFrame>();
			hosts.clear();
			board.sceneFrame = frame;
		}
		// The targets as sub read them.
		constexpr std::uint32_t kMaxFocus = FocusFrame::kMaxFocus;
		const std::uint32_t targetCount = board.targetCount = std::min(EngineFocusTargetCount(), kMaxFocus);
		for (std::uint32_t i = 0; i < kMaxFocus; ++i)
			board.targets[i] = i < targetCount ? EngineFocusTarget(i) : nullptr;
		FocusFrame::Host* host = nullptr;
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
		if (!kFocusCameras.Known()) {
			logger::warn("[DCLF] focus views: BSShadowDirectionalLight::sub is not known for this runtime; no focus views");
			return;
		}
		stl::detour_thunk<Hooks::Cameras>(REL::Offset(kFocusCameras.Get()).address());
		installed = true;
		logger::info("[DCLF] focus views: BSShadowDirectionalLight::sub detoured (T3c)");
	}
}
