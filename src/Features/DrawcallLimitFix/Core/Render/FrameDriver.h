#pragma once

// The frame's DCLF work at each engine point, render thread: the scene frame's start (the publication installed, its claims, the
// coordinator's requests served, the frame values kicked), the shadow views, the depth commit, EarlyPrepass and Prepass, the occlusion
// maps, the main pass's captures and the colour epoch, the views' work at their points (Api::ViewPoint), and Present's end of frame. The feature file's hooks call it; it reads the scene through
// Scene::FrameView and drives IndirectDraws.

#include "Features/DrawcallLimitFix/Api/Views.h"

#include <array>
#include <cstdint>

namespace RE
{
	class BSRenderPass;
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
				double sceneMs = 0.0;  // BeforeShadowMaps' views (the scene passes are the coordinator's, timed by its pump)
				double sceneMaxMs = 0.0;
				std::uint32_t frames = 0;
			} timing;
		};
		Counters& ReportCounters() { return counters; }

		/** @brief The occlusion maps' frames since the last take (Features/OcclusionMaps' report line), with what the line reads beside them now. */
		struct OcclusionFrames
		{
			// Per occlusion view (kOcclusionSky, kOcclusionPrecipitation): the maps left to the engine (DCLF not ready), and the frames the
			// engine registered the map's other occluders.
			std::array<std::uint32_t, 2> native{};
			std::array<std::uint32_t, 2> engine{};
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
		void BeforeShadowMaps();
		void AfterShadowMaps();
		/**
		 * @brief Main::RenderDepth's world-drawn point, while DCLF runs: the depth commit (IndirectDraws::CaptureDepthPass), then the views'
		 * work there (ViewPoint::DepthCommitted: the frame's reflection faces).
		 */
		void DepthCommitted();
		void EarlyPrepass();
		void Prepass();
		bool OcclusionReady(std::uint32_t a_map, bool a_running);
		bool OcclusionNeedsEngine(std::uint32_t a_map, bool a_running);
		void DrawOcclusion();
		void BeforeOpaquePass();
		void AfterOpaquePass();
		/** @brief A native lighting draw in the main pass: SetupGeometry's projected textures noted, and the observers told. */
		void NativeLightingDraw(RE::BSRenderPass* a_pass, std::uint32_t a_renderFlags);

	private:
		/** @brief The main pass's targets and bindings for this frame's colour epoch; false when its targets are not bound. */
		bool CaptureMainPass();
		/** @brief Every view's work at a_point (Extensions::ViewEntry::work), the views in slot order. */
		void RunViewWork(Api::ViewPoint a_point);

		Counters counters;
		OcclusionFrames occlusionFrames;  // native and engine only: the rest is read at the take
		std::uint32_t toggleGeneration = 0;
		bool frameRan = false;            // a scene frame ran since the last Present
		std::uint32_t occlusionWanted = 0;  // the maps OcclusionReady said DCLF draws this frame
		std::uint32_t captureFrame = ~0u;   // the frame whose main-pass bindings have been captured
		bool loggedCaptureFailure = false;
	};
}
