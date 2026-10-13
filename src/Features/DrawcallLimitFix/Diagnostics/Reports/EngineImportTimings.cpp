#include "Engine.h"

#include "Features/DrawcallLimitFix/Diagnostics/Engine/ImportTimings.h"

namespace DCLF::Reports
{
	std::string ImportTimingsLine(std::uint32_t a_frames)
	{
		if (!ImportTimings::Installed())
			return {};
		const auto totals = ImportTimings::Take();
		LARGE_INTEGER frequency;
		QueryPerformanceFrequency(&frequency);
		const double frames = std::max(1u, a_frames);
		const double msPerTick = 1000.0 / static_cast<double>(frequency.QuadPart);
		std::string text = fmt::format("[DCLF] engine import work by thread (T6b0, inclusive, over {} frames): per frame, main / other", a_frames);
		for (std::size_t e = 0; e < ImportTimings::kEntries; ++e) {
			const auto& calls = totals.calls[e];
			const auto& ticks = totals.ticks[e];
			text += fmt::format("; {}: {:.2f} calls {:.3f} ms / {:.2f} calls {:.3f} ms", ImportTimings::kNames[e], calls[1] / frames, ticks[1] * msPerTick / frames, calls[0] / frames,
				ticks[0] * msPerTick / frames);
		}
		return text;
	}
}
