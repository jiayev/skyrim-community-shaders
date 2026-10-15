#pragma once

// The water reflection's own frame state (ReflectionFrame): this update's captured faces, the cube they render into and the report's
// counters, beside what Core keeps of the view's slot (IndirectDraws::ViewState: its targets, resources, coverage, slot pipelines and the
// faces' tree LOD); and the faces' camera parity's counts. Render thread, and the report. Included by the feature's sources.

// Internal.h first: volk must precede every Vulkan header of the translation unit.
#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Features/Reflection/Reflection.h"

namespace DCLF::Features::Reflection
{
	inline constexpr Api::EngineView kView = Api::EngineView::Reflection;

	/** @brief The view's frame state: the LOD forward programs and pipelines found, this update's captured faces, the report counters. */
	struct ReflectionFrame
	{
		std::uint32_t lodSlots = 0, programsReady = 0, pipelinesReady = 0;
		// This update's faces (FaceOriented), until Execute takes them.
		struct Face
		{
			bool captured = false;
			std::array<std::byte, kViewBlockBytes> perFrame{};
			std::uint32_t perFrameBytes = 0;
			std::array<float, 16> viewProj{};
			RE::NiPoint3 eye;
		};
		std::array<Face, kFaces> faces;
		// The face the engine last oriented the camera to (the parity).
		std::uint32_t orientedFace = ~0u;
		winrt::com_ptr<ID3D11Texture2D> cube;  // the cube target the faces render into
		std::uint32_t width = 0, height = 0;
		// Since the last report.
		std::uint32_t facesCaptured = 0, updates = 0, epochs = 0, facesDrawn = 0;
		std::array<std::uint32_t, 6> skipped{};  // not drawable, not covered (DecideCoverage), no faces, stale inputs, no resources, the epoch failed
		// The last Execute's frame, and faces captured after it that frame (expected none).
		std::uint32_t executedFrame = ~0u, lateFaces = 0;
	};

	/** @brief The view's frame state (render thread; the report reads and resets its counters). */
	inline ReflectionFrame& FrameState()
	{
		static ReflectionFrame frame;
		return frame;
	}

	/** @brief The faces' camera parity (Diagnostics/FaceCameraParity.cpp): faces compared, differing (block, slice); the report takes them. */
	struct FaceCameras
	{
		std::uint32_t faces = 0, blocks = 0, slices = 0;
		std::uint64_t registers = 0;  // differing registers (c0-c44), by bit
		float largest = 0.0f;
		std::string first;
	};

	/** @brief The parity's counts (render thread). */
	inline FaceCameras& FaceCameraCounts()
	{
		static FaceCameras counts;
		return counts;
	}
}
