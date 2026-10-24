#include "Draws.h"

#include "Features/DrawcallLimitFix/Diagnostics/Draws/DrawsState.h"
#include "Features/DrawcallLimitFix/Core/Gpu/FrameData.h"
#include "Features/DrawcallLimitFix/Core/Gpu/FrameValues.h"

// The frame values' and the frame data's report lines.

namespace DCLF::Reports
{
	std::string FrameValuesLine()
	{
		const auto c = FrameValues::Get().TakeCounters();
		if (!c.frames)
			return {};
		const double n = static_cast<double>(c.frames);
		auto line = fmt::format("[DCLF] frame values: {} frames; a frame {:.0f} items sampled ({:.1f} written slots kept for their transforms to settle), {:.1f} fade roots seeded, {:.1f} rows changed, {:.0f} skins ({:.1f} palette rows changed), "
								"{:.1f} shading items sampled ({:.1f} rows changed, {:.1f} wetness changed), "
								"{:.1f} runs ({:.1f} KB) sent; {:.0f} us sampling, {:.0f} us sending, at most {} us; "
								"{} refused leases, {} palette size defects, {} failures",
			c.frames, c.sampled / n, c.settled / n, c.seeded / n, c.rowsChanged / n, c.skins / n, c.paletteRowsChanged / n, c.shadingSampled / n, c.shadingChanged / n,
			c.wetnessChanged / n, c.runs / n, c.bytes / n / 1024.0, c.sampleUs / n, c.uploadUs / n, c.maxTotalUs, c.refused, c.defects,
			c.failures);
		// The placement plans (deltas) the producers applied onto their kept set.
		if (c.plans)
			line += fmt::format("; {} placement plans applied ({} resets, {:.1f} items a plan)", c.plans, c.planResets, static_cast<double>(c.planItems) / static_cast<double>(c.plans));
		if (auto& s = Diagnostics::DrawsState::Get().frameValues; s.checks) {
			line += fmt::format("; parity against the engine: {} rows checked, {} differ{}{} ({} written this frame, the next frame's); {} palettes checked, {} differ{}{}",
				s.checks, s.differs, s.differs ? " <- DIFFER; first: " : " <- OK", s.differs ? s.first : std::string(), s.fresh, s.paletteChecks, s.paletteDiffers,
				s.paletteDiffers ? " <- DIFFER; first: " : " <- OK", s.paletteDiffers ? s.paletteFirst : std::string());
			if (!s.causes.empty()) {
				std::vector<std::pair<std::string, std::pair<std::uint64_t, std::string>>> causes(s.causes.begin(), s.causes.end());
				std::ranges::sort(causes, [](const auto& a, const auto& b) { return a.second.first > b.second.first; });
				line += fmt::format("\n[DCLF] frame values' parity against the engine: differing rows by owner ({} owners):", causes.size());
				for (std::size_t i = 0; i < std::min<std::size_t>(causes.size(), 8); ++i)
					line += fmt::format("{} {} x {} (e.g. {})", i ? ";" : "", causes[i].second.first, causes[i].first, causes[i].second.second);
				s.causes.clear();
			}
			s.checks = s.differs = s.fresh = s.paletteChecks = s.paletteDiffers = 0;
			s.first.clear();
			s.paletteFirst.clear();
		}
		// CS_DCLF_SET_PARITY (CheckShadingSampled).
		if (auto& s = Diagnostics::DrawsState::Get().frameValues; s.shadingChecks) {
			line += fmt::format("; {} resident shading rows checked, {} never sampled{}{}", s.shadingChecks, s.shadingUnsampled,
				s.shadingUnsampled ? " <- UNSAMPLED; first: " : " <- OK", s.shadingUnsampled ? s.shadingFirst : std::string());
			s.shadingChecks = s.shadingUnsampled = 0;
			s.shadingFirst.clear();
		}
		// The row sets' pools (D6; the built-in parts', then the features' registered pools): a frame's acquires and catch-up, the backings
		// made, the census and the journal floor; under CS_DCLF_PERSISTENT_PARITY, the leased count against the lease-leak bound.
		const bool watch = Diagnostics::Observe().scene;
		auto& watches = Diagnostics::DrawsState::Get().frameValuesPools;
		line += "\n[DCLF] frame values' pools (D6):";
		const auto pools = FrameValues::Get().TakePoolStats();
		if (watches.size() < pools.size())
			watches.resize(pools.size());
		for (std::size_t p = 0; p < pools.size(); ++p) {
			const auto& pool = pools[p];
			line += fmt::format("{} {} {:.2f} acquires, {:.1f} runs ({:.1f} KB) caught up a frame, {} made; {} live ({} leased, {} free, {:.1f} MB), floor {}{}", p ? ";" : "",
				pool.name, pool.acquires / n, pool.runs / n, pool.bytes / n / 1024.0, pool.made, pool.live, pool.leased, pool.free, pool.liveBytes / (1024.0 * 1024.0),
				pool.floor, watch ? watches[p].Window(pool.leased) : "");
		}
		return line;
	}

	std::string FrameDataLine()
	{
		auto taken = FrameData::Take();
		if (!taken.frames)
			return {};
		const double n = static_cast<double>(taken.frames);
		std::vector<std::pair<const std::string*, FrameData::Counts>> sorted;
		std::uint64_t total = 0, writes = 0;
		for (const auto& [where, c] : taken.counts) {
			sorted.emplace_back(&where, c);
			total += c.bytes;
			writes += c.writes;
		}
		std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second.bytes > b.second.bytes; });
		auto line = fmt::format("[DCLF] frame data through the render thread: {:.1f} KB a frame in {:.0f} writes over {} frames; by where:", total / n / 1024.0,
			writes / n, taken.frames);
		for (const auto& [where, c] : sorted)
			line += fmt::format(" {} {:.2f} KB ({:.1f} writes);", *where, c.bytes / n / 1024.0, c.writes / n);
		line.pop_back();
		return line;
	}
}
