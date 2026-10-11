#include "RenderThreadBudget.h"

#include "RenderGraph/RenderGraphRuntime.h"

#include <fmt/format.h>

namespace DCLF
{
	namespace
	{
		constexpr const char* kBucketNames[] = { "scene exchange (snapshot taken and adopted)", "scene captures (globals, categories, mirror, switch catch-ups)",
			"capture", "submit", "ORG overhead", "engine boundary", "other" };
		constexpr const char* kSiteNames[] = { "Present", "scene frame", "BeforeShadowMaps", "AfterShadowMaps (shadow epoch)", "shadow view capture",
			"occlusion", "EarlyPrepass", "Prepass", "Z-prepass", "BeforeOpaquePass", "AfterOpaquePass (colour epoch)", "reflection face" };
		static_assert(std::size(kBucketNames) == static_cast<std::size_t>(RenderThreadBudget::Bucket::Count));
		static_assert(std::size(kSiteNames) == static_cast<std::size_t>(RenderThreadBudget::Site::Count));

		double Microseconds(std::chrono::steady_clock::duration a_duration)
		{
			return std::chrono::duration<double, std::micro>(a_duration).count();
		}
	}

	RenderThreadBudget& RenderThreadBudget::Get()
	{
		static RenderThreadBudget budget;
		return budget;
	}

	bool RenderThreadBudget::OnRenderThread() const
	{
		return renderThread == ::GetCurrentThreadId();
	}

	RenderThreadBudget::Hook::Hook(Site a_site)
	{
		auto& b = Get();
		// The render thread is Main::Draw's: a load screen's Present runs on another thread, which is not the frame's.
		if (a_site == Site::SceneFrame)
			b.renderThread = ::GetCurrentThreadId();
		if (!b.OnRenderThread())
			return;
		counted = true;
		if (b.depth++)
			return;
		b.site = a_site;
		b.hookPartsUs = 0.0;
		const auto& epochs = RenderGraphRuntime::Get().RenderThreadEpochTotals();
		b.epochSubmitStart = epochs.submitUs;
		b.epochOverheadStart = epochs.overheadUs;
		b.hookStart = std::chrono::steady_clock::now();
	}

	RenderThreadBudget::Hook::~Hook()
	{
		auto& b = Get();
		if (!counted || --b.depth)
			return;
		const double us = Microseconds(std::chrono::steady_clock::now() - b.hookStart);
		const auto& epochs = RenderGraphRuntime::Get().RenderThreadEpochTotals();
		const double submit = epochs.submitUs - b.epochSubmitStart;
		const double overhead = epochs.overheadUs - b.epochOverheadStart;
		b.bucketUs[static_cast<std::size_t>(Bucket::Submit)] += submit;
		b.bucketUs[static_cast<std::size_t>(Bucket::OrgOverhead)] += overhead;
		b.bucketUs[static_cast<std::size_t>(Bucket::Other)] += (std::max)(0.0, us - b.hookPartsUs - submit - overhead);
		b.siteUs[static_cast<std::size_t>(b.site)] += us;
		++b.siteCalls[static_cast<std::size_t>(b.site)];
		b.frameUs += us;
		b.totalUs += us;
	}

	RenderThreadBudget::Part::Part(Bucket a_bucket) :
		bucket(a_bucket)
	{
		if (!Get().OnRenderThread())
			return;
		counted = true;
		start = std::chrono::steady_clock::now();
	}

	RenderThreadBudget::Part::~Part()
	{
		if (!counted)
			return;
		auto& b = Get();
		const double us = Microseconds(std::chrono::steady_clock::now() - start);
		b.bucketUs[static_cast<std::size_t>(bucket)] += us;
		if (b.depth) {
			b.hookPartsUs += us;
			return;
		}
		// Outside any hook: its own time, not a part of one.
		b.frameUs += us;
		b.totalUs += us;
	}

	void RenderThreadBudget::EndFrame()
	{
		if (!OnRenderThread())
			return;
		frameMaxUs = (std::max)(frameMaxUs, frameUs);
		frameUs = 0.0;
		++frames;
	}

	std::string RenderThreadBudget::Report()
	{
		if (!frames)
			return {};
		const double n = frames;
		std::string buckets, sites;
		for (std::size_t i = 0; i < bucketUs.size(); ++i)
			buckets += fmt::format("{}{} {:.3f}", i ? ", " : "", kBucketNames[i], bucketUs[i] / n / 1000.0);
		for (std::size_t i = 0; i < siteUs.size(); ++i)
			if (siteCalls[i])
				sites += fmt::format("{}{} {:.3f} ({:.1f} calls)", sites.empty() ? "" : ", ", kSiteNames[i], siteUs[i] / n / 1000.0, siteCalls[i] / n);
		auto line = fmt::format("[DCLF] render thread, ms per frame over {} frames: {:.3f} (max {:.3f}); by bucket: {}; by hook: {}", frames, totalUs / n / 1000.0,
			frameMaxUs / 1000.0, buckets, sites);
		bucketUs = {};
		siteUs = {};
		siteCalls = {};
		frameMaxUs = 0.0;
		totalUs = 0.0;
		frames = 0;
		return line;
	}
}
