#pragma once

// The frame's DCLF work at each engine point, render thread: the scene frame's start (the publication installed, its claims, the
// coordinator's requests served, the frame values kicked), the shadow views, the depth commit, EarlyPrepass and Prepass, the occlusion
// maps (the shadow views listed through Core's Shadow/ShadowPoints.h, from the engine's shadow-light selection, which Features/Shadows/Engine's
// hooks reach; the views' coverage, the maps' readiness and the shadow epoch with the maps taken through the shadow epochs' handlers there:
// Api/ShadowEpochs.h, Features/Shadows/Engine's), the main pass's points (their bookkeeping, and the main view's handlers there: Api/PrimaryView.h, Features/MainView's capture and
// colour epoch), the views' work at their points (Api::ViewPoint), and Present's end of frame. The feature file's hooks call it; it reads the scene through
// Scene::FrameView and drives IndirectDraws.

#include "Features/DrawcallLimitFix/Api/Views.h"

#include <array>
#include <chrono>
#include <cstdint>

namespace RE
{
	class BSRenderPass;
	class BSShadowLight;
}

namespace DCLF
{
	class FrameDriver
	{
	public:
		static FrameDriver& Get();

		/** @brief The render thread's time in the frame's DCLF points since the last report. */
		struct Counters
		{
			struct Timing
			{
				double eventsMs = 0.0;  // Present's, and the frame start's requests and captures (ServeFrameRequests)
				double buildMs = 0.0;   // EarlyPrepass's posts and material serving
				double buildMaxMs = 0.0;
				double sceneMs = 0.0;  // the shadow views' points, a frame's counted at BeforeShadowMaps (the scene passes are the coordinator's, timed by its pump)
				double sceneMaxMs = 0.0;
				std::uint32_t frames = 0;
			} timing;
		};
		Counters& ReportCounters() { return counters; }

		/** @brief The occlusion maps' frames since the last take (Features/OcclusionMaps' report line), with what the line reads beside them now. */
		struct OcclusionFrames
		{
			// Per occlusion view (kOcclusionSky, kOcclusionPrecipitation): the maps left to the engine (DCLF not ready), and the frames the
			// engine culled and registered the map's scene, and the frames DCLF drew it while some occluder was not yet a member.
			std::array<std::uint32_t, 2> native{};
			std::array<std::uint32_t, 2> engine{};
			std::array<std::uint32_t, 2> joining{};
			// Per occlusion view, now: the map's phase's members the frame's set lacks (FrameView::SetLacking).
			std::array<std::uint32_t, 2> lacking{};
			std::uint32_t withheld = 0;  // the maps' member passes withheld (PassCapture's, last frame)
		};
		/** @brief Render thread, at the report: the occlusion maps' frames since the last take (reset), and the set's lacking and withheld counts now. */
		OcclusionFrames TakeOcclusionFrames();

		/**
		 * @brief Main::Draw's early point, every frame. a_running false (DCLF not loaded or switched off): the coordinator's passes apply
		 * the events alone and the switch catch-ups they ask for are served (a_installed). True when DCLF runs this frame.
		 */
		bool BeginFrame(bool a_running, bool a_installed);
		/** @brief Present (in menus too, once installed): the frame's ends, its report, a load screen noted, the engine references released. */
		void EndFrame(bool a_running);
		/**
		 * @brief The engine's shadow-light selection (NiCamera::CalculateAndDrawShadowCasterLights, Features/Shadows/Engine's hooks), while
		 * DCLF runs: before it keeps its first light, the frame's shadow views begun and their coverage decided (the epochs'
		 * beforeShadowCasters); as it keeps each light, before its Accumulate, the light's views; once it returns, the views listed from
		 * DCLF's selection. Every shadow view's registration is thus withheld by this frame's coverage.
		 */
		void BeforeShadowCasters();
		void ShadowCasterKept(const RE::BSShadowLight* a_light, std::uint32_t a_firstSlot, std::uint32_t a_slots);
		void ShadowCastersSelected();
		void BeforeShadowMaps();
		void AfterShadowMaps();
		/**
		 * @brief Main::RenderDepth's world-drawn point, while DCLF runs: the depth commit (IndirectDraws::CaptureDepthPass), running the
		 * views' work there inside its epoch's execution (ViewPoint::DepthCommitted: the frame's reflection faces and first-person depth,
		 * which the depth epoch hosts); after it when the depth epoch did not commit.
		 */
		void DepthCommitted();
		void EarlyPrepass();
		void Prepass();
		/**
		 * @brief The occlusion maps (DrawcallLimitFix::OcclusionReady and OcclusionNeedsEngine): at the shadow maps, before the engine draws
		 * them, whether DCLF draws a map in the frame's shadow epoch (taken: AfterShadowMaps hands the shadow epoch the maps taken); at the
		 * engine's occlusion point, whether the engine renders it whole (the shadow epoch did not draw it).
		 */
		bool OcclusionReady(std::uint32_t a_map, bool a_running);
		bool OcclusionNeedsEngine(std::uint32_t a_map, bool a_running);
		/** @brief Where the main pass's opaque batches start: the engine's pending state applied, then the main view's capture (Api::PrimaryViewContribution). */
		void BeforeOpaquePass();
		/** @brief Where they end: the main view's colour epoch (Api::PrimaryViewContribution::afterOpaque). */
		void AfterOpaquePass();
		/** @brief A native lighting draw in the main pass: SetupGeometry's projected textures noted, and the observers told. */
		void NativeLightingDraw(RE::BSRenderPass* a_pass, std::uint32_t a_renderFlags);

	private:
		/** @brief Every view's work at a_point (Extensions::ViewEntry::work), the views in slot order. */
		void RunViewWork(Api::ViewPoint a_point);

		/** @brief The shadow views' time this frame so far (from BeforeShadowCasters), counted whole at BeforeShadowMaps. */
		void AddShadowViewsTime(std::chrono::steady_clock::time_point a_start);

		Counters counters;
		double shadowViewsMs = 0.0;       // the shadow views' points this frame, until BeforeShadowMaps counts them
		OcclusionFrames occlusionFrames;  // native and engine only: the rest is read at the take
		bool frameRan = false;            // a scene frame ran since the last Present
		std::uint32_t occlusionWanted = 0;  // the maps OcclusionReady took for the frame's shadow epoch, until AfterShadowMaps
		std::uint32_t occlusionDrawn = 0;   // the maps the frame's shadow epoch drew (OcclusionNeedsEngine's answer)
		std::uint32_t captureFrame = ~0u;   // the frame whose main-pass bindings have been captured (Api::PrimaryViewContribution::beforeOpaque)
	};
}
