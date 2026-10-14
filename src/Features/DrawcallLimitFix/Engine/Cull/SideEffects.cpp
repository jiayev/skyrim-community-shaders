// SideEffects: the actors' animation stamp from DCLF's shadow views (main thread, Main::Update's entry). SideEffects.h describes it.
#include "Features/DrawcallLimitFix/Engine/Cull/SideEffects.h"

#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Engine/Views/LightViews.h"
#include "Features/DrawcallLimitFix/Engine/Views/SunViews.h"
#include "Features/DrawcallLimitFix/Scene/FrameView/FrameView.h"

#include <array>
#include <atomic>
#include <cstddef>

namespace DCLF::SideEffects
{
	namespace
	{
		// AE 1.6.1170: the actors' animation rate (UpdateActorRate).
		constexpr std::uintptr_t kFrameCounter = 0x328cc6c;     // gFrameCounter: Renderer::End (0x140e4482b) increments it
		constexpr std::size_t kMiddleHighSkinProperty = 0x178;  // MiddleHighProcessData: the property Actor::ModifyAnimationUpdateData reads
		constexpr std::size_t kPropertyStamp = 0x80;            // BSShaderProperty::lastAccumulatedFrameCount

		ActorRate actorRate;

		bool SphereInside(const SunViews::Planes& a_planes, const RE::NiPoint3& a_centre, float a_radius)
		{
			for (std::uint32_t p = 0; p < 6; ++p) {
				if (!((a_planes.mask >> p) & 1))
					continue;
				const auto& plane = a_planes.plane[p];
				if (!(plane[0] * a_centre.x + plane[1] * a_centre.y + plane[2] * a_centre.z - plane[3] > -a_radius))
					return false;
			}
			return true;
		}
	}

	void UpdateActorRate(bool a_running)
	{
		if (!a_running)
			return;
		auto* lists = RE::ProcessLists::GetSingleton();
		if (!lists)
			return;
		auto& c = actorRate;
		++c.entries;
		const std::uint32_t counter = Engine::Global<std::uint32_t>(kFrameCounter);
		// The last scene frame's shadow views (the frame whose stamps the engine left): the sun's cascade culls, the local lights' views.
		const std::uint32_t sceneFrame = Scene::FrameView::Get().GetFrame();
		const auto& sun = SunViews::Get().Current();
		const bool sunViews = sun.valid && sun.sceneFrame == sceneFrame;
		for (const auto& handle : lists->highActorHandles) {
			const auto actor = handle.get();
			if (!actor)
				continue;
			const auto* process = actor->GetActorRuntimeData().currentProcess;
			const auto* middleHigh = process ? process->middleHigh : nullptr;
			auto* property = middleHigh ? Engine::At<std::byte*>(middleHigh, kMiddleHighSkinProperty) : nullptr;
			auto* root = actor->Get3D2();
			++c.actors;
			if (!property || !root) {
				++c.noProperty;
				continue;
			}
			// DCLF's verdict: the 3D's bound inside a shadow view of the last scene frame.
			const auto& bound = root->worldBound;
			bool inSun = false, inLocal = false;
			// The sun: the cascade culls walk what the full-frustum cull kept (SunViews::Frame::fullFrusta), then test their own planes.
			bool inFull = sun.fullFrusta.empty();
			for (const auto& planes : sun.fullFrusta)
				inFull = inFull || SphereInside(planes, bound.center, bound.radius);
			if (sunViews && inFull)
				for (std::uint32_t i = 0; i < sun.count && i < SunViews::kMaxCascades && !inSun; ++i)
					inSun = sun.cascades[i].valid && SphereInside(sun.cascades[i].cull, bound.center, bound.radius);
			if (!inSun)
				LightViews::Get().ForEachView(sceneFrame, [&](const SunViews::Cascade& a_view, const std::array<float, 4>& a_sphere) {
					// A spot light's view: its cull planes; a point light's (no planes): the bound within the light's radius.
					if (inLocal)
						return;
					if (a_view.cull.mask) {
						inLocal = SphereInside(a_view.cull, bound.center, bound.radius);
					} else if (a_sphere[3] > 0.0f) {
						const float dx = bound.center.x - a_sphere[0], dy = bound.center.y - a_sphere[1], dz = bound.center.z - a_sphere[2];
						const float reach = a_sphere[3] + bound.radius;
						inLocal = dx * dx + dy * dy + dz * dz <= reach * reach;
					}
				});
			const bool dclf = inSun || inLocal;
			c.inSun += inSun ? 1 : 0;
			c.inLocal += inLocal ? 1 : 0;
			// This frame's stamp, for every actor DCLF's shadow views hold (the engine reads it only for a kNotVisible one).
			if (dclf) {
				std::atomic_ref<std::uint32_t>(Engine::At<std::uint32_t>(property, kPropertyStamp)).store(counter, std::memory_order_relaxed);
				++c.stamped;
			}
		}
	}

	ActorRate TakeActorRate()
	{
		return std::exchange(actorRate, {});
	}
}
