// The main commits' warnings and setup log (always on, render thread): frame buffers the mirror cannot fill, the fade updates' varied
// inputs, a draw past its buffers, and the culling setup (once) with each epoch's viewport and ViewProj z row (periodic).

#include "DrawsState.h"
#include "Events.h"
#include "Observers.h"

namespace DCLF::Diagnostics
{
	void LogUnfilledFrameBuffer(const FrameBufferUnfilled& a_unfilled)
	{
		auto& unfilled = DrawsState::Get().unfilledFrameBuffers[a_unfilled.textureRegister];
		if ((unfilled++ % 600) == 0)
			logger::warn("[DCLF] frame buffer t{} ({} elements of {} bytes) not filled: the mirror holds {} bytes of it ({} times)", a_unfilled.textureRegister,
				a_unfilled.elements, a_unfilled.stride, a_unfilled.mirrorBytes, unfilled);
	}

	void LogFadeInputsVaried(const FadeInputsVaried& a_varied)
	{
		if (DrawsState::Get().fadeInputsVaried++ < 5)
			logger::info("[DCLF] the animation job's fade updates of frame {} had differing inputs; the first's stand for all", a_varied.frame);
	}

	void LogShortBuffers(const ShortBuffersDrawn& a_drawn)
	{
		const auto& tables = a_drawn.tables;
		const auto& shortBuffer = a_drawn.payload.body->shortBuffer;
		if (shortBuffer.object >= tables.objects.size())
			return;
		const auto& geometry = tables.geometries[tables.objects[shortBuffer.object].geometryIndex];
		logger::warn("[DCLF] '{}' draws past its buffers: {} vertices x {} bytes needs {}, the slice holds {}; indices to {} need {}, the slice holds {}",
			tables.objectGeometry[shortBuffer.object] ? tables.objectGeometry[shortBuffer.object]->name.c_str() : "?", geometry.vertexCount, geometry.vertexStride,
			shortBuffer.vertexNeeded, geometry.vertexBytes, geometry.firstIndex + geometry.indexCount, shortBuffer.indexNeeded, geometry.indexBytes);
	}

	void LogCullingSetup(const MainCullingSetup& a_setup)
	{
		auto& state = DrawsState::Get();
		const auto& shape = a_setup.shape;
		const auto& capture = a_setup.capture;
		auto* perFrameBuffer = capture.vsBuffers[kPerFrameVertexRegister];
		if (!std::exchange(state.cullingSetupLogged, true))
			logger::info("[DCLF] culling setup: mode {}, ViewProj {}, VS_PerFrame b{} {}", shape.cullMode, a_setup.hasViewProj ? "yes" : "no", kPerFrameVertexRegister,
				perFrameBuffer ? "bound" : "not bound");
		// Logged periodically: both epochs must rasterise identically (viewport, depth range) for the colour EQUAL test.
		if (state.epochLines[a_setup.depthOnly ? 1 : 0]++ % 480 != 0)
			return;
		// The packed ViewProj's z row, which produces the compared depth.
		std::string viewProjZ = "(none)";
		std::span<const std::byte> perFrame;
		if (a_setup.replay)
			perFrame = a_setup.replayPerFrame;
		else if (perFrameBuffer)
			perFrame = ConstantMirror::Get().Contents(perFrameBuffer);
		if (perFrame.size() >= 48 * sizeof(float)) {
			const auto* floats = reinterpret_cast<const float*>(perFrame.data());
			viewProjZ = fmt::format("({:.6f} {:.6f} {:.6f} {:.6f})", floats[40], floats[41], floats[42], floats[43]);
		}
		logger::info("[DCLF] {} epoch: tables frame {} holding {} objects; render area {}x{}, depth range [{}, {}] (captured [{}, {}]), replay {}, eye ({:.2f} {:.2f} {:.2f}), ViewProj z row {}",
			a_setup.depthOnly ? "z-prepass" : "colour", a_setup.frame, a_setup.liveObjects, shape.width, shape.height, shape.minDepth, shape.maxDepth, capture.minDepth,
			capture.maxDepth, a_setup.replay ? "on" : "off", capture.eye.x, capture.eye.y, capture.eye.z, viewProjZ);
	}
}
