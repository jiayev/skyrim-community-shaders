// CS_DCLF_CAPTURE_POINT_PARITY: at the frame's first deferred lighting draw the engine makes, what is bound there against the colour
// epoch's capture, in everything the colour epoch takes from it.

#include "DrawsState.h"
#include "DrawsView.h"
#include "Observers.h"

namespace DCLF::Diagnostics
{
	void CheckCapturePoint()
	{
		const auto& pending = DrawsView::Get().pending;
		if (!pending)
			return;
		const auto& captured = *pending;
		auto now = CaptureBindings();
		// Only what the colour epoch takes from a capture: the frame registers, the frame textures, the targets, the
		// viewport and the eye (the per-draw registers and textures are the draw's own).
		auto& p = DrawsState::Get().capturePoint;
		bool any = false;
		auto note = [&](std::string a_what) {
			++p.differ[std::move(a_what)];
			any = true;
		};
		for (std::uint32_t slot = 0; slot < kConstantBufferRegisters; ++slot) {
			if (!((kPerDrawVS >> slot) & 1) && captured.vsBuffers[slot] != now.vsBuffers[slot])
				note(fmt::format("VS b{}", slot));
			if (!((kPerDrawPS >> slot) & 1) && captured.psBuffers[slot] != now.psBuffers[slot])
				note(fmt::format("PS b{}", slot));
		}
		for (std::uint32_t t = kPixelTextureSlots; t < kTextureRegisters; ++t)
			if (captured.psViews[t] != now.psViews[t])
				note(fmt::format("t{}", t));
		for (std::uint32_t i = 0; i < kColorTargets; ++i)
			if (captured.targets[i] != now.targets[i])
				note(fmt::format("rt{}", i));
		if (captured.depth != now.depth)
			note("depth");
		if (captured.viewportWidth != now.viewportWidth || captured.viewportHeight != now.viewportHeight || captured.minDepth != now.minDepth ||
			captured.maxDepth != now.maxDepth)
			note("viewport");
		if (std::memcmp(&captured.eye, &now.eye, sizeof(captured.eye)) != 0 || std::memcmp(&captured.previousEye, &now.previousEye, sizeof(captured.previousEye)) != 0)
			note("eye");
		now.Release();
		p.frames += any ? 1u : 0u;
		if (p.window.Tick()) {
			std::string text;
			for (const auto& [what, count] : p.differ)
				text += fmt::format(" {}={}", what, count);
			logger::info("[DCLF] capture point parity: {} frames' captures against the first lighting draw's bindings, {} differ{}", Reports::kInterval, p.frames,
				p.frames ? " <- DIFFER:" + text : std::string(" <- OK"));
			p = {};
		}
	}
}
