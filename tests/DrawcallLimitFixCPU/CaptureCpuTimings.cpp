#include "Features/DrawcallLimitFix/CaptureGraph.h"

#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <numeric>
#include <span>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace DCLF::Published;
using Clock = std::chrono::steady_clock;

namespace
{
	constexpr std::size_t kGroups = 512;
	constexpr std::size_t kTrials = 5;

	struct Sample
	{
		std::vector<double> postUs;
		double completionMs = 0;
		std::uint32_t builds = 0;
	};

	std::shared_ptr<const ConsistencyGroupUpdate> MakeGroup(std::size_t index)
	{
		const Identity group{ index + 1, 1 };
		std::array members{ MemberInput{ { index * 2 + 1, 1 } }, MemberInput{ { index * 2 + 2, 1 } } };
		std::array geometry{ std::byte{ 1 }, std::byte{ 2 }, std::byte{ 3 }, std::byte{ 4 } };
		std::array material{ std::byte{ 9 }, std::byte{ 8 }, std::byte{ 7 }, std::byte{ 6 } };
		std::array parts{
			ComponentInput{ members[0].identity, ComponentKind::Geometry, geometry },
			ComponentInput{ members[0].identity, ComponentKind::Material, material },
			ComponentInput{ members[1].identity, ComponentKind::Geometry, geometry },
			ComponentInput{ members[1].identity, ComponentKind::Material, material }
		};
		return ConsistencyGroupUpdate::Capture(group, 1, 1, members, parts, {});
	}

	Sample Measure(bool graph, const std::vector<std::shared_ptr<const ConsistencyGroupUpdate>>& groups)
	{
		Sample sample;
		sample.postUs.reserve(groups.size() + 1);
		std::atomic<std::uint32_t> builds{ 0 };
		CaptureService::Builder builder = [&](const auto& request) {
			++builds;
			std::uint64_t checksum = 0;
			for (const auto& [id, group] : request.scene->groups)
				checksum += id + group->Members().size();
			return CaptureService::Output{ std::make_shared<const std::uint64_t>(checksum), sizeof(checksum) };
		};
		std::unique_ptr<CaptureService> service;
		if (graph) service = std::make_unique<CaptureService>(MakeGraphCaptureBackend(builder), sizeof(std::uint64_t));
		else service = std::make_unique<CaptureService>(builder, sizeof(std::uint64_t));
		const auto start = Clock::now();
		auto post = [&](const CapturedSceneEvent& event) {
			const auto begin = Clock::now();
			while (!service->TryPost(event)) {
				if (service->Faulted() || Clock::now() - start > std::chrono::seconds(5)) throw std::runtime_error("capture admission stalled");
				std::this_thread::yield();
			}
			sample.postUs.push_back(std::chrono::duration<double, std::micro>(Clock::now() - begin).count());
		};
		post({ SceneEventKind::Reset, 1, 1, {}, {} });
		for (std::size_t i = 0; i < groups.size(); ++i)
			post({ SceneEventKind::ReplaceGroup, i + 2, 1, groups[i]->Group(), groups[i] });
		std::shared_ptr<const PreparedCapture> ready;
		for (;;) {
			if (service->Faulted() || Clock::now() - start > std::chrono::seconds(5)) throw std::runtime_error("capture preparation stalled");
			if (!service->TryTakeReady(ready)) { std::this_thread::yield(); continue; }
			const bool complete = ready->request.scene->groups.size() == groups.size();
			if (complete) {
				const auto checksum = std::static_pointer_cast<const std::uint64_t>(ready->artifact);
				const auto expected = static_cast<std::uint64_t>(groups.size() * (groups.size() + 1) / 2 + groups.size() * 2);
				if (!checksum || *checksum != expected) throw std::runtime_error("prepared scene checksum mismatch");
			}
			if (!service->TryAcknowledge(ready->request.ticket, true)) throw std::runtime_error("acknowledgement failed");
			ready.reset();
			if (complete) break;
		}
		sample.completionMs = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
		sample.builds = builds.load();
		service->Shutdown();
		return sample;
	}

	double Percentile(std::vector<double> values, double fraction)
	{
		std::sort(values.begin(), values.end());
		return values[static_cast<std::size_t>((values.size() - 1) * fraction)];
	}
}

int main()
{
	std::vector<std::shared_ptr<const ConsistencyGroupUpdate>> groups;
	groups.reserve(kGroups);
	for (std::size_t i = 0; i < kGroups; ++i) groups.push_back(MakeGroup(i));
	std::cout << "backend,trial,groups,builds,post_p50_us,post_p95_us,post_p99_us,completion_ms\n";
	for (std::size_t trial = 0; trial < kTrials; ++trial)
		for (const bool graph : { false, true }) {
			const auto sample = Measure(graph, groups);
			std::cout << (graph ? "graph" : "direct") << ',' << trial << ',' << kGroups << ',' << sample.builds << ','
				<< Percentile(sample.postUs, 0.5) << ',' << Percentile(sample.postUs, 0.95) << ','
				<< Percentile(sample.postUs, 0.99) << ',' << sample.completionMs << '\n';
		}
}
