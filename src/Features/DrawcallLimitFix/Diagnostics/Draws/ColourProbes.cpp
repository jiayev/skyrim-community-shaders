#if defined(CS_HAS_RENDER_GRAPH) && defined(CS_HAS_ORG_MODULE_SERVICES)
#	include "DrawsState.h"
#	include "Events.h"
#	include "Observers.h"

// The colour epoch's probes: its ViewProj's jitter (CS_DCLF_STATS) and the objects whose bound covers a pixel (CS_DCLF_TARGET_PROBE).

namespace DCLF::Diagnostics
{
	void MeasureJitter(const ColourFrameBlock& a_block)
	{
		// The colour epoch's ViewProj (c8-c11 of b12) against its unjittered copy (c12-c15), the main pass's b12 now, the cached frame
		// buffer and the previous epoch's.
		const auto& a_capture = a_block.capture;
		const auto packed = a_block.packed;
		auto& mirror = ConstantMirror::Get();
		if (packed.size() >= 16 * 4 * sizeof(float)) {
			const auto* p = reinterpret_cast<const float*>(packed.data());
			auto maxDiff = [&](const float* a_other) {
				float d = 0.0f;
				for (int i = 0; i < 16; ++i)
					d = std::max(d, std::abs(p[32 + i] - a_other[i]));
				return d;
			};
			auto& j = DrawsState::Get().jitter;
			j.vsUnjittered += maxDiff(p + 48);
			j.vsUnjitteredMax = std::max(j.vsUnjitteredMax, maxDiff(p + 48));
			if (auto* buffer = a_capture.vsBuffers[kPerFrameVertexRegister]) {
				const auto main = mirror.Contents(buffer);
				if (main.size() >= 48 * sizeof(float)) {
					const float d = maxDiff(reinterpret_cast<const float*>(main.data()) + 32);
					j.vsMain += d;
					j.vsMainMax = std::max(j.vsMainMax, d);
				}
			}
			const float cached = maxDiff(reinterpret_cast<const float*>(&globals::game::frameBufferCached.data) + 32);
			j.vsCached += cached;
			j.vsCachedMax = std::max(j.vsCachedMax, cached);
			if (j.havePrevious) {
				const float d = maxDiff(j.previous.data());
				j.vsPrevious += d;
				j.vsPreviousMax = std::max(j.vsPreviousMax, d);
			}
			std::copy(p + 32, p + 48, j.previous.begin());
			j.havePrevious = true;
			if (++j.epochs == 300) {
				logger::info("[DCLF] colour epoch ViewProj over {} epochs (mean / max of the largest element difference): against its unjittered copy {:.2e} / {:.2e}, "
							 "the main pass's b12 now {:.2e} / {:.2e}, the cached frame buffer {:.2e} / {:.2e}, the previous epoch's {:.2e} / {:.2e}; jitter {:.6f} {:.6f}",
					j.epochs, j.vsUnjittered / j.epochs, j.vsUnjitteredMax, j.vsMain / j.epochs, j.vsMainMax, j.vsCached / j.epochs, j.vsCachedMax,
					j.vsPrevious / j.epochs, j.vsPreviousMax, p[32 + 2] - p[48 + 2], p[32 + 6] - p[48 + 6]);
				j = { .previous = j.previous, .havePrevious = true };
			}
		}
	}

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

#endif
