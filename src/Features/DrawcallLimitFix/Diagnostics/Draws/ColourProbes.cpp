// The colour epoch's probe: the objects whose bound covers a pixel (CS_DCLF_TARGET_PROBE).

#include "DrawsState.h"
#include "Events.h"
#include "Observers.h"

namespace DCLF::Diagnostics
{
	void ProbeTarget(const MainCullViewProj& a_cull)
	{
		const auto& latch = a_cull.latch;
		const auto& tables = a_cull.tables;
		const bool depthOnly = a_cull.depthOnly;
		const std::uint32_t frameNumber = a_cull.frame;
		struct
		{
			std::uint32_t width, height;
		} shape{ a_cull.width, a_cull.height };
		// The objects whose bound covers the probed pixel, nearest first, with their pipelines' descriptors.
		if (!depthOnly && shape.width && shape.height && (frameNumber % 240) == 0) {
			if (const auto& pixel = Diagnostics::Observe().targetProbe; !pixel.empty()) {
				const auto sep = pixel.find_first_of(",x");
				const float px = std::strtof(pixel.substr(0, sep).c_str(), nullptr) + 32.0f, py = std::strtof(pixel.substr(sep + 1).c_str(), nullptr) + 32.0f;
				const float nx = px / shape.width * 2.0f - 1.0f, ny = 1.0f - py / shape.height * 2.0f;
				struct Hit { float w; std::uint32_t object; };
				std::vector<Hit> hits;
				for (std::uint32_t o = 0; o < tables.objects.size() && o < tables.objectGeometry.size(); ++o) {
					// The bound now, from the engine (render thread).
					const auto* geometry = tables.objectGeometry[o];
					if (!geometry || geometry->worldBound.radius <= 0.0f)
						continue;
					const auto& bound = geometry->worldBound;
					float clip[4];
					for (std::uint32_t row = 0; row < 4; ++row)
						clip[row] = latch.viewProj[row * 4] * bound.center.x + latch.viewProj[row * 4 + 1] * bound.center.y + latch.viewProj[row * 4 + 2] * bound.center.z +
						            latch.viewProj[row * 4 + 3];
					if (clip[3] <= 1.0f)
						continue;
					// The sphere's screen radius, roughly (the projection's x scale over the distance).
					const float scale = std::sqrt(latch.viewProj[0] * latch.viewProj[0] + latch.viewProj[1] * latch.viewProj[1] + latch.viewProj[2] * latch.viewProj[2]);
					const float radius = bound.radius * scale / clip[3];
					const float dx = clip[0] / clip[3] - nx, dy = clip[1] / clip[3] - ny;
					if (dx * dx + dy * dy <= radius * radius)
						hits.push_back({ clip[3], o });
				}
				std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.w < b.w; });
				std::string text;
				const auto& keys = tables.pipelines;
				for (std::size_t i = 0; i < hits.size() && i < 8; ++i) {
					const auto o = hits[i].object;
					const auto* geometry = o < tables.objectGeometry.size() ? tables.objectGeometry[o] : nullptr;
					const auto pipeline = tables.objects[o].pipelineIndex;
					text += fmt::format("; '{}' (object {}, w {:.0f}, flags {:08X}, pipeline {} pixel {:08X})", geometry && geometry->name.c_str() ? geometry->name.c_str() : "?", o,
						hits[i].w, tables.objects[o].flags, pipeline, pipeline < keys.size() ? keys[pipeline].pixelDescriptor : 0u);
				}
				logger::info("[DCLF] target probe: {} objects cover the pixel{}", hits.size(), text);
			}
		}
	}
}
