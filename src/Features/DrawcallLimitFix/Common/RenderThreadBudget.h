#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <string>

namespace DCLF
{
	/**
	 * @brief DCLF's render-thread time a frame, by what the asynchronous scene leaves on it (dclf-async-publication.md,
	 * "The render thread").
	 *
	 * Every DCLF hook the render thread enters is a Hook: its whole time is counted, by site. Parts nested in it are counted to
	 * their bucket; the epochs it ran are split by RenderGraphRuntime::RenderThreadEpochTotals (the submission proper, and ORG's
	 * other render-thread work); the rest is Other, which the migration takes to zero. The per-draw hooks (NoteNativePass,
	 * OnNativeLightingDraw) and the engine's own hooks DCLF edits (the sun's and point lights' culls, the scene lists, the pass
	 * capture on the job threads) are not counted here: they report their own times.
	 *
	 * Render thread only: a scope on any other thread counts nothing.
	 */
	class RenderThreadBudget
	{
	public:
		enum class Bucket : std::uint8_t
		{
			Exchange,        // T6b3d: the frame's start's exchange with the scene: the snapshot taken and adopted, its publication installed
			SceneCaptures,   // T6b3d: the frame's start's engine captures and requests for the scene (globals, categories, mirror, switches)
			Capture,         // copying state only the render thread can read: D3D11 bindings, constant mirrors, view state
			Submit,          // ORG's submission proper: the ticket, the queue submission, the stream's close
			OrgOverhead,     // ORG's other render-thread work in an epoch: releases, the ticket's check, upload recording, flushes
			EngineBoundary,  // engine code DCLF calls (ConstantEvaluator's stand-ins) and engine writes it applies
			Other,           // everything else: what moves to the workers
			Count
		};

		enum class Site : std::uint8_t
		{
			Present,
			SceneFrame,
			BeforeShadowMaps,
			AfterShadowMaps,
			ShadowViewCapture,
			Occlusion,
			EarlyPrepass,
			Prepass,
			ZPrepass,
			BeforeOpaque,
			AfterOpaque,
			ReflectionFace,
			Count
		};

		static RenderThreadBudget& Get();

		/** @brief A DCLF hook's whole call. Nested hooks count once, to the outermost's site. */
		class Hook
		{
		public:
			explicit Hook(Site a_site);
			~Hook();
			Hook(const Hook&) = delete;
			Hook& operator=(const Hook&) = delete;

		private:
			bool counted = false;
		};

		/** @brief A part of a hook (or of nothing) counted to a_bucket rather than Other. */
		class Part
		{
		public:
			explicit Part(Bucket a_bucket);
			~Part();
			Part(const Part&) = delete;
			Part& operator=(const Part&) = delete;

		private:
			Bucket bucket;
			bool counted = false;
			std::chrono::steady_clock::time_point start;
		};

		/** @brief The frame's end (Present, after its hook): the frame's total joins the report's maximum. */
		void EndFrame();

		/** @brief The report line since the last one, or empty when no frame ended. */
		std::string Report();

	private:
		RenderThreadBudget() = default;
		bool OnRenderThread() const;

		std::uint32_t renderThread = 0;
		std::uint32_t depth = 0;
		Site site = Site::Present;
		std::chrono::steady_clock::time_point hookStart;
		double hookPartsUs = 0.0;  // the open hook's parts
		double epochSubmitStart = 0.0, epochOverheadStart = 0.0;

		std::array<double, static_cast<std::size_t>(Bucket::Count)> bucketUs{};
		std::array<double, static_cast<std::size_t>(Site::Count)> siteUs{};
		std::array<std::uint32_t, static_cast<std::size_t>(Site::Count)> siteCalls{};
		double frameUs = 0.0;
		double frameMaxUs = 0.0;
		double totalUs = 0.0;
		std::uint32_t frames = 0;
	};
}
