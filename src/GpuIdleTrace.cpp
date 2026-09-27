#include "GpuIdleTrace.h"

#include "Features/Upscaling/DXVKInterop.h"
#include "Globals.h"
#include "RenderGraph/DxvkOrgInterop.h"

#include <algorithm>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace GpuIdleTrace
{
	namespace
	{
		// Gaps shorter than this are queue scheduling noise.
		constexpr double kMinGapMs = 0.05;
		// Gaps logged per analysed frame, longest first.
		constexpr std::size_t kMaxGapsLogged = 8;
		// Innermost CPU markers listed per gap.
		constexpr std::size_t kMaxMarkersPerGap = 6;
		// CPU events and interop spans older than this are dropped.
		constexpr double kHistoryMs = 500.0;

		struct Event
		{
			enum class Type : std::uint8_t
			{
				Begin,
				End,
				Mark,
			};
			std::int64_t qpc = 0;
			Type type = Type::Begin;
			std::string name;
		};

		struct Span
		{
			enum class Kind : std::uint8_t
			{
				CommandList,
				External,
				Interop,
			};
			Kind kind = Kind::CommandList;
			std::uint32_t flushType = ~0u;
			std::uint64_t submissionId = 0;
			std::int64_t appQpc = 0;
			std::int64_t csQpc = 0;
			std::int64_t queueQpc = 0;
			std::uint64_t gpuBegin = 0;
			std::uint64_t gpuEnd = 0;
			std::string label;
		};

		struct State
		{
			std::uint32_t interval = 0;
			bool active = false;
			PFN_dxvkReadSubmissionTrace readTrace = nullptr;
			double tickNs = 1.0;
			double qpcToMs = 0.0;
			DWORD renderThread = 0;

			std::mutex mutex;  // events and interop spans
			std::deque<Event> events;
			std::deque<Span> interopSpans;

			std::vector<Span> frameSpans;  // DXVK spans since the last present record
			std::uint64_t presents = 0;
			bool analysePending = false;
			std::uint64_t framesSinceReport = 0;

			// The summary window: idle time of every analysed frame since the last summary, attributed to the
			// render-thread markers open during the gaps (self time, as the per-gap report computes it).
			std::uint32_t summaryFrames = 0;
			std::uint64_t summaryFirstPresent = 0;
			double summaryIdleMs = 0.0;
			double summaryUnattributedMs = 0.0;
			std::map<std::string, double> summaryByMarker;
		};

		// Analysed frames per summary line; at CS_GPU_IDLE_TRACE=30 that is one line per 300 frames.
		constexpr std::uint32_t kSummaryFrames = 10;

		void Summarise(State& a_state)
		{
			if (!a_state.summaryFrames)
				return;
			const double frames = static_cast<double>(a_state.summaryFrames);
			// Group the markers by their prefix up to the first ':' ("CS DCLF: main opaque (CPU)" -> "CS DCLF"), so
			// one number per subsystem heads each group and the members follow.
			struct Group
			{
				double ms = 0.0;
				std::vector<std::pair<std::string, double>> members;
			};
			std::map<std::string, Group> groups;
			for (const auto& [name, ms] : a_state.summaryByMarker) {
				const auto colon = name.find(':');
				const std::string prefix = colon == std::string::npos ? name : name.substr(0, colon);
				auto& group = groups[prefix];
				group.ms += ms;
				if (colon != std::string::npos) {
					auto member = name.substr(colon + 1);
					if (!member.empty() && member.front() == ' ')
						member.erase(0, 1);
					group.members.emplace_back(std::move(member), ms);
				}
			}
			std::vector<std::pair<std::string, Group>> ranked(groups.begin(), groups.end());
			std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) { return a.second.ms > b.second.ms; });
			std::string byMarker;
			for (auto& [prefix, group] : ranked) {
				if (group.ms / frames < 0.005)
					break;
				byMarker += fmt::format("{}{} {:.2f}", byMarker.empty() ? "" : ", ", prefix, group.ms / frames);
				std::sort(group.members.begin(), group.members.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
				std::string members;
				for (std::size_t i = 0; i < group.members.size() && i < 6; ++i) {
					if (group.members[i].second / frames < 0.005)
						break;
					members += fmt::format("{}{} {:.2f}", members.empty() ? "" : ", ", group.members[i].first, group.members[i].second / frames);
				}
				if (!members.empty())
					byMarker += " (" + members + ")";
			}
			logger::info("[GpuIdle] summary over {} analysed frames (presents {}-{}): idle {:.2f} ms/frame; by marker: {}{}unattributed {:.2f}",
				a_state.summaryFrames, a_state.summaryFirstPresent, a_state.presents, a_state.summaryIdleMs / frames,
				byMarker, byMarker.empty() ? "" : ", ", a_state.summaryUnattributedMs / frames);
			a_state.summaryFrames = 0;
			a_state.summaryIdleMs = 0.0;
			a_state.summaryUnattributedMs = 0.0;
			a_state.summaryByMarker.clear();
		}

		State& Get()
		{
			static State state;
			return state;
		}

		std::uint32_t ReadInterval()
		{
			char value[32] = {};
			const DWORD length = GetEnvironmentVariableA("CS_GPU_IDLE_TRACE", value, sizeof(value));
			return length && length < sizeof(value) ? static_cast<std::uint32_t>(std::strtoul(value, nullptr, 10)) : 0u;
		}

		std::int64_t Now()
		{
			LARGE_INTEGER counter;
			QueryPerformanceCounter(&counter);
			return counter.QuadPart;
		}

		const char* FlushTypeName(std::uint32_t a_type)
		{
			switch (a_type) {
			case 0:
				return "explicit flush";
			case 1:
				return "implicit: synchronization";
			case 2:
				return "implicit: strong hint";
			case 3:
				return "implicit: weak hint";
			}
			return "no flush info";
		}

		std::string Describe(const Span& a_span)
		{
			switch (a_span.kind) {
			case Span::Kind::CommandList:
				if (!a_span.submissionId)
					return "DXVK list outside the immediate context (the presenter's blit)";
				return fmt::format("DXVK list #{} ({}{}{})", a_span.submissionId, FlushTypeName(a_span.flushType),
					a_span.label.empty() ? "" : ": ", a_span.label);
			case Span::Kind::External:
				return fmt::format("interop '{}'", a_span.label);
			case Span::Kind::Interop:
				return fmt::format("Streamline ring '{}'", a_span.label);
			}
			return "?";
		}

		struct Interval
		{
			std::string name;
			int depth = 0;
			std::int64_t begin = 0;
			std::int64_t end = 0;
		};

		// Rebuilds nested marker intervals from the event log. Unclosed events end at a_now.
		std::vector<Interval> BuildIntervals(const std::deque<Event>& a_events, std::int64_t a_now)
		{
			std::vector<Interval> out;
			std::vector<std::size_t> stack;
			for (const auto& e : a_events) {
				switch (e.type) {
				case Event::Type::Begin:
					stack.push_back(out.size());
					out.push_back({ e.name, static_cast<int>(stack.size()) - 1, e.qpc, a_now });
					break;
				case Event::Type::End:
					if (!stack.empty()) {
						out[stack.back()].end = e.qpc;
						stack.pop_back();
					}
					break;
				case Event::Type::Mark:
					out.push_back({ "(mark) " + e.name, static_cast<int>(stack.size()), e.qpc, e.qpc });
					break;
				}
			}
			return out;
		}

		std::string StackAt(const std::vector<Interval>& a_intervals, std::int64_t a_qpc)
		{
			std::vector<const Interval*> open;
			for (const auto& i : a_intervals) {
				if (i.begin <= a_qpc && i.end > a_qpc)
					open.push_back(&i);
			}
			std::sort(open.begin(), open.end(), [](const Interval* a, const Interval* b) { return a->depth < b->depth; });
			std::string s;
			for (const auto* i : open)
				s += (s.empty() ? "" : " > ") + i->name;
			return s.empty() ? "(no marker)" : s;
		}

		void Analyse(State& a_state)
		{
			std::vector<Span> spans = a_state.frameSpans;
			std::deque<Event> events;
			{
				std::lock_guard lock(a_state.mutex);
				events = a_state.events;
				if (!spans.empty()) {
					std::uint64_t lo = UINT64_MAX, hi = 0;
					for (const auto& s : spans) {
						lo = (std::min)(lo, s.gpuBegin);
						hi = (std::max)(hi, s.gpuEnd);
					}
					for (const auto& s : a_state.interopSpans) {
						if (s.gpuBegin >= lo && s.gpuEnd <= hi)
							spans.push_back(s);
					}
				}
			}
			if (spans.size() < 2)
				return;

			std::sort(spans.begin(), spans.end(), [](const Span& a, const Span& b) { return a.gpuBegin < b.gpuBegin; });
			const double tickMs = a_state.tickNs * 1e-6;
			const auto tickToMs = [&](std::uint64_t t) { return static_cast<double>(t) * tickMs; };
			const auto qpcToMs = [&](std::int64_t q) { return static_cast<double>(q) * a_state.qpcToMs; };

			// GPU clock -> CPU clock: the GPU cannot begin a submission before it is submitted.
			double offsetMs = 1e300;
			for (const auto& s : spans) {
				if (s.queueQpc)
					offsetMs = (std::min)(offsetMs, tickToMs(s.gpuBegin) - qpcToMs(s.queueQpc));
			}
			const auto gpuToQpc = [&](std::uint64_t t) {
				return static_cast<std::int64_t>((tickToMs(t) - offsetMs) / a_state.qpcToMs);
			};

			struct Gap
			{
				std::size_t prev;
				std::size_t next;
				std::uint64_t begin;
				std::uint64_t end;
			};
			std::vector<Gap> gaps;
			std::uint64_t busyEnd = spans.front().gpuEnd;
			std::size_t busyIdx = 0;
			double busyMs = tickToMs(spans.front().gpuEnd - spans.front().gpuBegin);
			for (std::size_t i = 1; i < spans.size(); ++i) {
				const auto& s = spans[i];
				if (s.gpuBegin > busyEnd && tickToMs(s.gpuBegin - busyEnd) >= kMinGapMs)
					gaps.push_back({ busyIdx, i, busyEnd, s.gpuBegin });
				if (s.gpuEnd > busyEnd) {
					busyMs += tickToMs(s.gpuEnd - (std::max)(busyEnd, s.gpuBegin));
					busyEnd = s.gpuEnd;
					busyIdx = i;
				}
			}

			const std::uint64_t frameBegin = spans.front().gpuBegin;
			const double frameMs = tickToMs(busyEnd - frameBegin);
			double idleMs = 0.0;
			for (const auto& g : gaps)
				idleMs += tickToMs(g.end - g.begin);
			logger::info("[GpuIdle] frame: {} submissions over {:.2f} ms of GPU time, {:.2f} ms busy, {} gaps >= {:.2f} ms totalling {:.2f} ms",
				spans.size(), frameMs, busyMs, gaps.size(), kMinGapMs, idleMs);

			// Chronological list of every submission, so the gaps can be matched against a capture.
			std::string order;
			for (std::size_t i = 0; i < spans.size(); ++i) {
				const auto& s = spans[i];
				order += fmt::format("\n    [{:2}] +{:6.2f} ms  {:5.2f} ms  {}", i, tickToMs(s.gpuBegin - frameBegin),
					tickToMs(s.gpuEnd - s.gpuBegin), Describe(s));
			}
			logger::info("[GpuIdle] submissions in GPU order:{}", order);

			const std::int64_t now = Now();
			const auto intervals = BuildIntervals(events, now);

			if (!a_state.summaryFrames)
				a_state.summaryFirstPresent = a_state.presents;
			++a_state.summaryFrames;
			a_state.summaryIdleMs += idleMs;

			std::sort(gaps.begin(), gaps.end(), [](const Gap& a, const Gap& b) { return (a.end - a.begin) > (b.end - b.begin); });
			// Every gap is attributed into the summary; only the largest few are described in full.
			for (std::size_t g = 0; g < gaps.size(); ++g) {
				const auto& gap = gaps[g];
				const auto& prev = spans[gap.prev];
				const auto& next = spans[gap.next];
				const std::int64_t cpuBegin = gpuToQpc(gap.begin);
				const std::int64_t cpuEnd = gpuToQpc(gap.end);
				const auto rel = [&](std::int64_t q) { return q ? fmt::format("{:+.2f}", qpcToMs(q - cpuBegin)) : std::string("n/a"); };

				// Self time of each marker inside the gap's CPU window.
				std::map<std::string, double> self;
				double topLevel = 0.0;
				for (std::size_t i = 0; i < intervals.size(); ++i) {
					const auto& iv = intervals[i];
					const std::int64_t lo = (std::max)(iv.begin, cpuBegin), hi = (std::min)(iv.end, cpuEnd);
					if (hi <= lo)
						continue;
					double ms = qpcToMs(hi - lo);
					for (std::size_t j = i + 1; j < intervals.size() && intervals[j].begin < iv.end; ++j) {
						const auto& child = intervals[j];
						if (child.depth != iv.depth + 1)
							continue;
						const std::int64_t clo = (std::max)(child.begin, lo), chi = (std::min)(child.end, hi);
						if (chi > clo)
							ms -= qpcToMs(chi - clo);
					}
					self[iv.name] += ms;
					if (iv.depth == 0)
						topLevel += qpcToMs(hi - lo);
				}
				const double windowMs = qpcToMs(cpuEnd - cpuBegin);
				for (const auto& [name, ms] : self)
					a_state.summaryByMarker[name] += ms;
				a_state.summaryUnattributedMs += (std::max)(0.0, windowMs - topLevel);
				if (g >= kMaxGapsLogged)
					continue;

				std::vector<std::pair<std::string, double>> ranked(self.begin(), self.end());
				std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
				std::string markers;
				for (std::size_t i = 0; i < ranked.size() && i < kMaxMarkersPerGap; ++i) {
					if (ranked[i].second < 0.005)
						break;
					markers += fmt::format("\n      {:.3f} ms  {}", ranked[i].second, ranked[i].first);
				}

				logger::info(
					"[GpuIdle] gap {:.3f} ms at +{:.2f} ms: after [{}] before [{}]"
					"\n    next submission, relative to the GPU going idle: app flush {} ms, DXVK CS thread {} ms, queue submit {} ms"
					"\n    render thread at gap start: {}"
					"\n    render thread at gap end:   {}"
					"\n    CPU self time inside the gap ({:.3f} ms window, {:.3f} ms without any marker):{}",
					tickToMs(gap.end - gap.begin), tickToMs(gap.begin - frameBegin), Describe(prev), Describe(next),
					rel(next.appQpc), rel(next.csQpc), rel(next.queueQpc),
					StackAt(intervals, cpuBegin), StackAt(intervals, cpuEnd),
					windowMs, (std::max)(0.0, windowMs - topLevel), markers.empty() ? std::string(" (none)") : markers);
			}

			if (a_state.summaryFrames >= kSummaryFrames)
				Summarise(a_state);
		}
	}

	void Shutdown()
	{
		auto& state = Get();
		if (state.active)
			Summarise(state);
	}

	bool Requested()
	{
		static const bool requested = ReadInterval() != 0;
		return requested;
	}

	void Initialize()
	{
		auto& state = Get();
		state.interval = ReadInterval();
		if (!state.interval || state.active)
			return;

		HMODULE dxvk = GetModuleHandleW(L"dxvk_d3d11.dll");
		auto setTrace = dxvk ? reinterpret_cast<PFN_dxvkSetSubmissionTrace>(reinterpret_cast<void*>(GetProcAddress(dxvk, "dxvkSetSubmissionTrace"))) : nullptr;
		state.readTrace = dxvk ? reinterpret_cast<PFN_dxvkReadSubmissionTrace>(reinterpret_cast<void*>(GetProcAddress(dxvk, "dxvkReadSubmissionTrace"))) : nullptr;
		auto* interop = DXVKInterop::GetSingleton();
		if (!setTrace || !state.readTrace || !interop || !interop->IsAvailable() || FAILED(setTrace(globals::d3d::device, TRUE))) {
			logger::warn("[GpuIdle] CS_GPU_IDLE_TRACE is set, but this DXVK build has no submission trace");
			return;
		}

		VkPhysicalDeviceProperties props{};
		vkGetPhysicalDeviceProperties(interop->GetPhysicalDevice(), &props);
		state.tickNs = props.limits.timestampPeriod;
		LARGE_INTEGER freq;
		QueryPerformanceFrequency(&freq);
		state.qpcToMs = 1000.0 / static_cast<double>(freq.QuadPart);
		state.active = true;
		logger::info("[GpuIdle] Tracing DXVK's queue; analysing one frame every {} frames", state.interval);
	}

	void BeginEvent(std::string_view a_name)
	{
		auto& state = Get();
		if (!state.active || ::GetCurrentThreadId() != state.renderThread)
			return;
		std::lock_guard lock(state.mutex);
		state.events.push_back({ Now(), Event::Type::Begin, std::string(a_name) });
	}

	void EndEvent()
	{
		auto& state = Get();
		if (!state.active || ::GetCurrentThreadId() != state.renderThread)
			return;
		std::lock_guard lock(state.mutex);
		state.events.push_back({ Now(), Event::Type::End, {} });
	}

	void Mark(std::string_view a_name)
	{
		auto& state = Get();
		if (!state.active || ::GetCurrentThreadId() != state.renderThread)
			return;
		std::lock_guard lock(state.mutex);
		state.events.push_back({ Now(), Event::Type::Mark, std::string(a_name) });
	}

	void AddInteropSpan(const char* a_label, std::uint64_t a_gpuBegin, std::uint64_t a_gpuEnd, std::int64_t a_submitQpc)
	{
		auto& state = Get();
		if (!state.active)
			return;
		Span span;
		span.kind = Span::Kind::Interop;
		span.label = a_label ? a_label : "";
		span.gpuBegin = a_gpuBegin;
		span.gpuEnd = a_gpuEnd;
		span.queueQpc = a_submitQpc;
		std::lock_guard lock(state.mutex);
		state.interopSpans.push_back(std::move(span));
	}

	void OnPresent()
	{
		if (!Requested())
			return;
		auto& state = Get();
		static bool initialized = false;
		if (!std::exchange(initialized, true))
			Initialize();
		if (!state.active)
			return;
		state.renderThread = ::GetCurrentThreadId();

		// Drop CPU history nobody can still need.
		{
			std::lock_guard lock(state.mutex);
			const std::int64_t horizon = Now() - static_cast<std::int64_t>(kHistoryMs / state.qpcToMs);
			while (!state.events.empty() && state.events.front().qpc < horizon)
				state.events.pop_front();
			while (!state.interopSpans.empty() && state.interopSpans.front().queueQpc < horizon)
				state.interopSpans.pop_front();
			// An event log that starts with orphan End events would unbalance the stack; they are harmless
			// (BuildIntervals ignores them), and so are Begin events whose End is still to come.
		}

		if (++state.framesSinceReport >= state.interval) {
			state.framesSinceReport = 0;
			state.analysePending = true;
		}

		DxvkOrgSubmissionTraceRecord records[256];
		for (;;) {
			std::uint32_t count = 0;
			if (FAILED(state.readTrace(globals::d3d::device, records, static_cast<std::uint32_t>(std::size(records)), &count)) || !count)
				break;
			for (std::uint32_t i = 0; i < count; ++i) {
				const auto& r = records[i];
				if (r.kind == DXVK_ORG_SUBMISSION_PRESENT) {
					// A frame's submissions end at its present; analyse only complete frames.
					if (state.analysePending && !state.frameSpans.empty() && state.presents > 0) {
						state.analysePending = false;
						Analyse(state);
					}
					state.frameSpans.clear();
					++state.presents;
					continue;
				}
				Span span;
				span.kind = r.kind == DXVK_ORG_SUBMISSION_EXTERNAL ? Span::Kind::External : Span::Kind::CommandList;
				span.flushType = r.flushType;
				span.submissionId = r.submissionId;
				span.appQpc = r.appQpc;
				span.csQpc = r.csQpc;
				span.queueQpc = r.queueQpc;
				span.gpuBegin = r.gpuBegin;
				span.gpuEnd = r.gpuEnd;
				span.label = r.label;
				state.frameSpans.push_back(std::move(span));
			}
			if (count < std::size(records))
				break;
		}
	}
}
