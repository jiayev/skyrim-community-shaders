// The actors' animation stamp from DCLF's shadow views (main thread, Main::Update's entry, after the accumulated and fade writes: the
// stand-in observer's afterWrites) and its report line among the accumulated writes' (ReportPoint::AccumulatedWrites).
//
// The actors' animation rate (skyrim-engine-notes.md, "the fade nodes' OnVisible stores and the actors' animation stamp"). A high actor
// whose 3D the actor pre-cull left kNotVisible (bit 20; the race's flag bit 7 clear) animates at full rate in a frame only when its skin
// property (MiddleHighProcessData +0x178) carries that frame's gFrameCounter at +0x80 (Actor::ModifyAnimationUpdateData, 0x1406a37ef: the
// difference must be under fMaxFrameCounterDifferenceToConsiderVisible, so 0). Only the depth and shadow registrations stamp it
// (FUN_1414b2a60 and its siblings, before the batch renderer's RegisterPass), so it is lost once the culls stop, and for an actor the sun's
// entry exclusion keeps out of the cascade culls.
//
// Main thread at Main::Update's entry (no job runs; gFrameCounter is this frame's until Renderer::End): DCLF's verdict per high actor is
// its 3D's world bound inside any of the last scene frame's shadow views (the Sun board's cascade culls, the Lights board's local views:
// Features/Shadows/Sun's and Features/Shadows/Local's, DEPENDS) - the views DCLF draws, from the cameras the engine set up, no cull result
// read. The property of every actor with that verdict is stamped.

#include "Features/DrawcallLimitFix/Features/Actors/Actors.h"

#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Render/Shadow/ShadowCameras.h"
#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"
#include "Features/DrawcallLimitFix/Features/Shadows/Local/LightViews.h"
#include "Features/DrawcallLimitFix/Features/Shadows/Sun/SunViews.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

namespace DCLF::Features::Actors
{
	namespace
	{
		// AE 1.6.1170 and SE 1.5.97: the actors' animation rate (UpdateActorRate). The field offsets are the same on both.
		constexpr Engine::Offset kFrameCounter{ { 411489 }, { 525008 } };  // gFrameCounter: Renderer::End (0x140e4482b) increments it
		constexpr std::size_t kMiddleHighSkinProperty = 0x178;  // MiddleHighProcessData: the property Actor::ModifyAnimationUpdateData reads
		constexpr std::size_t kPropertyStamp = 0x80;            // BSShaderProperty::lastAccumulatedFrameCount

		/** @brief The actor rate's counts since the last report (main thread; the report reads them on the render thread between frames). */
		struct ActorRate
		{
			std::uint64_t entries = 0, actors = 0, noProperty = 0, stamped = 0, inSun = 0, inLocal = 0;
		};
		ActorRate actorRate;

		bool SphereInside(const ShadowCameras::Planes& a_planes, const RE::NiPoint3& a_centre, float a_radius)
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
			const auto& sun = Shadows::Sun::SunBoard();
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
				// The sun: the cascade culls walk what the full-frustum cull kept (Shadows::Sun::SunFrame::fullFrusta), then test their own planes.
				bool inFull = sun.fullFrusta.empty();
				for (const auto& planes : sun.fullFrusta)
					inFull = inFull || SphereInside(planes, bound.center, bound.radius);
				if (sunViews && inFull)
					for (std::uint32_t i = 0; i < sun.count && i < Shadows::Sun::SunFrame::kMaxCascades && !inSun; ++i)
						inSun = sun.cascades[i].valid && SphereInside(sun.cascades[i].cull, bound.center, bound.radius);
				if (!inSun)
					Shadows::Local::LightsBoard().ForEachView(sceneFrame, [&](const ShadowCameras::Camera& a_view, const std::array<float, 4>& a_sphere) {
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

		// The actors' animation stamp (newline-terminated); nothing when it did not run.
		void ActorRateText(std::string& a_text)
		{
			const auto c = std::exchange(actorRate, {});
			if (!c.entries)
				return;
			a_text += fmt::format(
				"[DCLF] actor rate (T7f checklist item 6): {} entries, {:.1f} high actors a frame ({:.1f} without a skin property or 3D), {:.1f} in the "
				"sun's cascade culls and {:.1f} in a local shadow view a frame, {:.1f} stamped by DCLF a frame\n",
				c.entries, double(c.actors) / double(c.entries), double(c.noProperty) / double(c.entries),
				double(c.inSun) / double(c.entries), double(c.inLocal) / double(c.entries), double(c.stamped) / double(c.entries));
		}
	}

	void RegisterActorRate(Api::InstallContext& a_context)
	{
		// Where PrimaryCull::AtUpdateEntry called it: last at Main::Update's entry, after the fade writes. Without the primary cull nothing
		// calls it (and it did not run before either).
		a_context.RegisterStandInObserver({ .afterWrites = &UpdateActorRate });
		// Inside the accumulated writes' text, after its every-listed-root lines, as before.
		a_context.RegisterReportLines({ .name = "actor rate", .point = Api::ReportPoint::AccumulatedWrites, .text = &ActorRateText });
	}
}
