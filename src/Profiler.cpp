#include "Profiler.h"

#include "RenderGraph/DxvkOrgInterop.h"

#include <algorithm>
#include <unordered_map>

float Profiler::RollingHistory::GetAverage() const
{
	if (count == 0)
		return lastMs;
	float sum = 0.0f;
	for (uint32_t i = 0; i < count; i++)
		sum += history[i];
	return sum / static_cast<float>(count);
}

float Profiler::RollingHistory::GetPercentile(float p) const
{
	if (count == 0)
		return lastMs;

	if (sortedDirty) {
		std::copy_n(history, count, sorted.begin());
		std::sort(sorted.begin(), sorted.begin() + count);
		sortedDirty = false;
	}

	float idx = (p / 100.0f) * static_cast<float>(count - 1);
	uint32_t lo = static_cast<uint32_t>(idx);
	uint32_t hi = std::min(lo + 1, count - 1);
	float frac = idx - static_cast<float>(lo);
	return sorted[lo] * (1.0f - frac) + sorted[hi] * frac;
}

void Profiler::Initialize(ID3D11Device* device, ID3D11DeviceContext* a_context)
{
	Release();

	context = a_context;

	for (auto& frame : frames) {
		D3D11_QUERY_DESC disjointDesc{};
		disjointDesc.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
		if (FAILED(device->CreateQuery(&disjointDesc, frame.disjoint.put()))) {
			Release();
			logger::warn("Profiler disabled: failed to create timestamp disjoint query");
			return;
		}

		frame.timers.resize(kMaxTimers);
		for (auto& timer : frame.timers) {
			D3D11_QUERY_DESC tsDesc{};
			tsDesc.Query = D3D11_QUERY_TIMESTAMP;
			if (FAILED(device->CreateQuery(&tsDesc, timer.begin.put())) ||
				FAILED(device->CreateQuery(&tsDesc, timer.end.put()))) {
				Release();
				logger::warn("Profiler disabled: failed to create timestamp query");
				return;
			}
		}
		frame.activeCount = 0;
		frame.inFlight = false;
	}

	// DXVK's timestamp queries are written at ALL_COMMANDS, so a pair inside one submission measures just
	// the work between them. A pair across a submission boundary (an implicit flush, the flush ahead of an
	// interop or render-graph submission) also counts the time the queue waited for the later submission,
	// so those samples are flagged.
	submissionCounter = nullptr;
	if (HMODULE dxvk = GetModuleHandleW(L"dxvk_d3d11.dll")) {
		if (auto getCounter = reinterpret_cast<PFN_dxvkGetSubmissionCounter>(
				reinterpret_cast<void*>(GetProcAddress(dxvk, "dxvkGetSubmissionCounter")));
			getCounter && FAILED(getCounter(device, &submissionCounter)))
			submissionCounter = nullptr;
	}
	logger::info("[Profiler] DXVK submission counter {}", submissionCounter ? "found: split timers are flagged" : "unavailable");

	writeFrame = 0;
	readFrame = 0;
	frameSkipped = false;
	lastLoggedFrame = 0;
	initialized = true;
}

void Profiler::Release()
{
	for (auto& frame : frames) {
		frame.disjoint = nullptr;
		frame.timers.clear();
		frame.activeCount = 0;
		frame.inFlight = false;
	}
	results.clear();
	knownTimers.clear();
	knownTimerIndex.clear();
	collectedFrames = 0;
	totalTimeMs = 0.0f;
	cpuTotalTimeMs = 0.0f;
	initialized = false;
	frameActive = false;
	context = nullptr;
}

void Profiler::BeginFrame()
{
	if (!initialized || !context || frameActive || !IsActive())
		return;

	CollectResults();

	frameActive = true;
	passOpen = false;
	++framesSinceExternalMerge;
	auto& frame = frames[writeFrame];
	// Every slot still holds a frame the GPU has not finished: leave this one untimed rather than restart a
	// pending disjoint query, which would never let that slot complete.
	frameSkipped = frame.inFlight;
	if (frameSkipped)
		return;
	frame.activeCount = 0;
	frame.inFlight = true;
	context->Begin(frame.disjoint.get());
}

void Profiler::BeginPass(std::string_view name)
{
	if (!initialized || !context)
		return;

	if (!frameActive)
		BeginFrame();

	if (frameActive && !frameSkipped) {
		auto& frame = frames[writeFrame];
		if (frame.activeCount < kMaxTimers) {
			auto& timer = frame.timers[frame.activeCount];
			timer.name.assign(name);
			context->End(timer.begin.get());
			QueryPerformanceCounter(&timer.cpuBegin);
			timer.submissionAtBegin = submissionCounter ? *submissionCounter : 0;
			passOpen = true;
		}
	}

	if (beginPerfEvent)
		beginPerfEvent(name);
}

void Profiler::EndPass()
{
	if (!initialized || !context)
		return;

	if (frameActive && !frameSkipped) {
		auto& frame = frames[writeFrame];
		if (frame.activeCount < kMaxTimers && std::exchange(passOpen, false)) {
			auto& timer = frame.timers[frame.activeCount];

			LARGE_INTEGER cpuEnd;
			QueryPerformanceCounter(&cpuEnd);
			timer.cpuMs = static_cast<float>(static_cast<double>(cpuEnd.QuadPart - timer.cpuBegin.QuadPart) * cpuTicksToMs);

			context->End(timer.end.get());
			timer.split = submissionCounter && *submissionCounter != timer.submissionAtBegin;
			frame.activeCount++;
		}
	}

	if (endPerfEvent)
		endPerfEvent({});
}

void Profiler::AddExternalSample(std::string_view a_name, float a_gpuMs)
{
	// Nothing averages or clears the samples while timing is off.
	if (!initialized || !IsActive())
		return;
	pendingExternal[std::string(a_name)] += a_gpuMs;
}

void Profiler::EndFrame()
{
	// Before the early returns so it ticks while GPU queries are off.
	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);
	if (lastFrameTicks != 0)
		frameHistory.PushSample(static_cast<float>(static_cast<double>(now.QuadPart - lastFrameTicks) * cpuTicksToMs));
	lastFrameTicks = now.QuadPart;

	if (!initialized || !context)
		return;

	frameCounter.fetch_add(1, std::memory_order_relaxed);

	if (!frameActive)
		return;

	frameActive = false;
	passOpen = false;
	if (std::exchange(frameSkipped, false))
		return;
	context->End(frames[writeFrame].disjoint.get());
	writeFrame = (writeFrame + 1) % kFrameRing;
}

bool Profiler::CollectFrame(FrameQueries& frame, std::unordered_map<std::string, std::pair<float, float>>& activeTimers)
{
	D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjointData{};
	if (context->GetData(frame.disjoint.get(), &disjointData, sizeof(disjointData), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
		return false;

	frame.inFlight = false;
	collectedFrames++;
	activeTimers.clear();
	if (disjointData.Disjoint)
		return true;

	const double ticksToMs = 1000.0 / static_cast<double>(disjointData.Frequency);
	for (uint32_t i = 0; i < frame.activeCount; i++) {
		auto& timer = frame.timers[i];
		UINT64 tsBegin = 0, tsEnd = 0;

		if (context->GetData(timer.begin.get(), &tsBegin, sizeof(tsBegin), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
			continue;
		if (context->GetData(timer.end.get(), &tsEnd, sizeof(tsEnd), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
			continue;

		float ms = static_cast<float>(static_cast<double>(tsEnd - tsBegin) * ticksToMs);
		auto& entry = activeTimers[timer.name];
		entry.first += ms;
		entry.second += timer.cpuMs;

		auto [it, inserted] = knownTimerIndex.try_emplace(timer.name, knownTimers.size());
		if (inserted) {
			KnownTimer kt;
			kt.name = timer.name;
			knownTimers.push_back(std::move(kt));
		}
		knownTimers[it->second].split.PushSample(timer.split ? 1.0f : 0.0f);
	}

	// A name timed several times in one frame is one sample: its frame total.
	for (const auto& [name, times] : activeTimers) {
		auto& known = knownTimers[knownTimerIndex.at(name)];
		known.gpu.PushSample(times.first);
		known.cpu.PushSample(times.second);
		known.lastSampleFrame = collectedFrames;
	}
	return true;
}

void Profiler::CollectResults()
{
	// The last collected frame's timers (GPU, CPU), for the results' current values.
	std::unordered_map<std::string, std::pair<float, float>> frameTimers;
	bool collected = false;
	while (frames[readFrame].inFlight && CollectFrame(frames[readFrame], frameTimers)) {
		collected = true;
		readFrame = (readFrame + 1) % kFrameRing;
	}
	if (!collected)
		return;

	struct ActiveTimerData
	{
		float gpuMs = 0.0f;
		float cpuMs = 0.0f;
	};
	std::unordered_map<std::string, ActiveTimerData> activeTimers;
	float activeTotalMs = 0.0f;
	float activeCpuTotalMs = 0.0f;
	for (const auto& [name, times] : frameTimers) {
		activeTimers[name] = { times.first, times.second };
		activeTotalMs += times.first;
		activeCpuTotalMs += times.second;
	}

	// The render graph's passes: GPU time from its own timestamps, per frame over the frames since the last
	// merge. Independent of the D3D11 disjoint flag, which says nothing about the graph's queries.
	const float externalFrames = static_cast<float>(std::max(1u, framesSinceExternalMerge));
	for (const auto& [name, gpuMs] : pendingExternal) {
		const float ms = gpuMs / externalFrames;
		activeTimers[name].gpuMs += ms;
		activeTotalMs += ms;
		auto [it, inserted] = knownTimerIndex.try_emplace(name, knownTimers.size());
		if (inserted) {
			KnownTimer kt;
			kt.name = name;
			knownTimers.push_back(std::move(kt));
		}
		auto& known = knownTimers[it->second];
		known.gpu.PushSample(ms);
		known.cpu.PushSample(0.0f);
		known.lastSampleFrame = collectedFrames;
	}
	pendingExternal.clear();
	framesSinceExternalMerge = 0;

	RetireStaleTimers();

	totalTimeMs = activeTotalMs;
	cpuTotalTimeMs = activeCpuTotalMs;

	results.clear();
	results.reserve(knownTimers.size());
	for (const auto& known : knownTimers) {
		TimerResult result;
		result.name = known.name;
		auto it = activeTimers.find(known.name);
		if (it != activeTimers.end()) {
			result.gpuTimeMs = it->second.gpuMs;
			result.cpuTimeMs = it->second.cpuMs;
		} else {
			result.gpuTimeMs = known.gpu.lastMs;
			result.cpuTimeMs = known.cpu.lastMs;
		}
		result.valid = true;
		result.splitFraction = known.split.count ? known.split.GetAverage() : 0.0f;
		result.historyBuffer = known.gpu.history;
		result.historyHead = known.gpu.head;
		result.historyCount = known.gpu.count;
		results.push_back(std::move(result));
	}

	LogResultsIfRequested();
}

void Profiler::LogResultsIfRequested()
{
	// CS_PROFILER_LOG=<frames>: log the rolling averages every that many collected frames, for runs where
	// the menu cannot be read (automated validation, comparing two configurations from their logs).
	static const uint64_t interval = [] {
		char value[32] = {};
		const DWORD length = GetEnvironmentVariableA("CS_PROFILER_LOG", value, sizeof(value));
		return length && length < sizeof(value) ? std::strtoull(value, nullptr, 10) : 0ull;
	}();
	if (interval == 0 || collectedFrames - lastLoggedFrame < interval)
		return;
	lastLoggedFrame = collectedFrames;

	std::vector<const TimerResult*> sorted;
	for (const auto& r : results)
		sorted.push_back(&r);
	std::sort(sorted.begin(), sorted.end(), [](const auto* a, const auto* b) { return a->avgMs > b->avgMs; });
	std::string line;
	for (const auto* r : sorted) {
		if (r->avgMs < 0.01f)
			continue;
		line += fmt::format(" | {} {:.2f}/p95 {:.2f}", r->name, r->avgMs, r->p95Ms);
		if (r->splitFraction > 0.0f)
			line += fmt::format(" [split {:.0f}%]", r->splitFraction * 100.0f);
	}
	logger::info("[Profiler] frame {} total {:.2f} ms{}", collectedFrames, totalTimeMs, line);
}

double Profiler::QpcTicksToMs()
{
	LARGE_INTEGER freq;
	QueryPerformanceFrequency(&freq);
	return 1000.0 / static_cast<double>(freq.QuadPart);
}

Profiler::FrameStats Profiler::GetFrameStats() const
{
	return {
		.lastMs = frameHistory.lastMs,
		.avgMs = frameHistory.GetAverage(),
		.p95Ms = frameHistory.GetPercentile(95.0f),
		.p99Ms = frameHistory.GetPercentile(99.0f),
		.count = frameHistory.count
	};
}

std::vector<Profiler::TimerResult> Profiler::GetStatistics(std::string_view timerPrefix) const
{
	std::vector<TimerResult> snapshot;
	snapshot.reserve(results.size());
	for (const auto& live : results) {
		if (!live.name.starts_with(timerPrefix))
			continue;
		auto it = knownTimerIndex.find(live.name);
		if (it == knownTimerIndex.end())
			continue;
		const auto& known = knownTimers[it->second];
		auto result = live;
		result.avgMs = known.gpu.GetAverage();
		result.p95Ms = known.gpu.GetPercentile(95.0f);
		result.p99Ms = known.gpu.GetPercentile(99.0f);
		result.cpuAvgMs = known.cpu.GetAverage();
		result.cpuP95Ms = known.cpu.GetPercentile(95.0f);
		result.cpuP99Ms = known.cpu.GetPercentile(99.0f);
		snapshot.push_back(std::move(result));
	}
	return snapshot;
}

void Profiler::RetireStaleTimers()
{
	const size_t before = knownTimers.size();
	std::erase_if(knownTimers, [this](const KnownTimer& known) {
		return collectedFrames - known.lastSampleFrame >= kTimerRetireFrames;
	});
	if (knownTimers.size() != before)
		RebuildTimerIndex();
}

void Profiler::RebuildTimerIndex()
{
	knownTimerIndex.clear();
	for (size_t i = 0; i < knownTimers.size(); i++)
		knownTimerIndex[knownTimers[i].name] = i;
}
