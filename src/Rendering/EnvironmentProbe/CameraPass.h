#pragma once

#include "Buffer.h"
#include "Rendering/EnvironmentProbe/Anchor.h"
#include "Rendering/EnvironmentProbe/Capture.h"
#include <atomic>

namespace EnvironmentProbe
{
	/** @brief Shared per-frame capture, controlled by the Environment Probe feature. */
	class CameraPass
	{
	public:
		enum class Mode
		{
			AllFaces,
			OneFacePerFrame
		};
		static constexpr uint32_t Resolution = 512;
		static constexpr float FarDistance = 16384.0f;
		bool enabled = true;
		bool supplementalSunShadows = true;
		Mode mode = Mode::OneFacePerFrame;
		float anchorDistance = 64.0f;
		Result result = Result::Unavailable;

		/** @brief Captures the world camera once per frame, after main world rendering. */
		void Render();
		/** @brief Returns the latest successful radiance capture, or null when unavailable. */
		ID3D11ShaderResourceView* Radiance() const { return valid && !resetPending.load() ? radiance->srv.get() : nullptr; }
		/** @brief Returns the latest successful radial depth capture in game units. */
		ID3D11ShaderResourceView* Depth() const { return valid && !resetPending.load() ? depth->srv.get() : nullptr; }
		/** @brief Encoded world-space source normals for probe GI visibility. */
		ID3D11ShaderResourceView* Normals() const { return valid && !resetPending.load() ? normals->srv.get() : nullptr; }
		/** @brief Valid octants and their individual capture origins for rotating updates. */
		uint32_t ValidFaces() const { return validFaces; }
		const std::array<RE::NiPoint3, 8>& Origins() const { return origins; }
		/** @brief Frame stamp of each successful octant capture, for incremental consumers. */
		const std::array<uint32_t, 8>& CaptureFrames() const { return captureFrames; }
		/** @brief Invalidates old-cell captures without touching render-thread resources. */
		void Reset() { resetPending.store(true); }

	private:
		Anchor anchor;
		std::unique_ptr<Texture2D> radiance;
		std::unique_ptr<Texture2D> depth;
		std::unique_ptr<Texture2D> normals;
		uint32_t lastFrame = UINT32_MAX;
		bool valid = false;
		uint32_t validFaces = 0;
		std::array<RE::NiPoint3, 8> origins{};
		std::array<uint32_t, 8> captureFrames{};
		std::atomic_bool resetPending{ false };
		winrt::com_ptr<ID3D11Query> statistics;
		bool statisticsPending = false;
		uint32_t statisticsFrame = 0;
	};
	/** @brief Shared world capture used by reflection and indirect-light consumers. */
	CameraPass& GetCameraPass();
}
