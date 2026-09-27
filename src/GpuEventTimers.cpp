#include "GpuEventTimers.h"

#include "Globals.h"

#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

namespace GpuEventTimers
{
	namespace
	{
		// Frames in flight before a frame's queries are read back; a frame whose slot comes round again unread is dropped.
		// Nothing in the frame waits for the GPU (the upscaler's submissions are enqueued), so the render thread can run
		// several frames ahead: with 4, every frame was dropped with DCLF off.
		constexpr std::uint32_t kFrames = 8;
		// Queries per frame (two an event, two for the frame); events past it are not timed, and counted.
		constexpr std::uint32_t kMaxQueries = 4096;
		constexpr std::uint32_t kNone = ~0u;
		// Paths below this average inclusive time a frame are left out of the log.
		constexpr double kMinLoggedMs = 0.02;

		std::uint32_t ReadInterval()
		{
			char value[32] = {};
			const DWORD length = GetEnvironmentVariableA("CS_GPU_EVENT_TIMERS", value, sizeof(value));
			return length && length < sizeof(value) ? static_cast<std::uint32_t>(std::strtoul(value, nullptr, 10)) : 0u;
		}

		struct Event
		{
			std::uint32_t path;
			std::uint32_t parent;  // the enclosing event's index in the frame, or kNone
			std::uint32_t begin, end = kNone;  // query indices
		};

		struct Frame
		{
			winrt::com_ptr<ID3D11Query> disjoint;
			std::vector<winrt::com_ptr<ID3D11Query>> queries;
			std::uint32_t used = 0;
			std::uint32_t frameBegin = kNone, frameEnd = kNone;
			std::vector<Event> events;
			bool open = false, pending = false, overflow = false;
		};

		struct Accumulated
		{
			double inclusive = 0.0, self = 0.0;
			std::uint64_t count = 0;
		};

		struct State
		{
			std::uint32_t interval = 0;
			bool initialized = false, failed = false;
			DWORD thread = 0;
			Frame frames[kFrames];
			std::uint32_t write = 0;
			// The open events, innermost last: an index into the recording frame's events, or kNone for one begun while
			// nothing was recorded (before the first frame, on the far side of a Present), whose end is then ignored.
			std::vector<std::uint32_t> stack;
			std::vector<std::string> paths;  // in the order they were first seen: the frame's order, near enough
			std::vector<std::uint32_t> depth;
			std::unordered_map<std::string, std::uint32_t> pathIndex;
			std::vector<Accumulated> accumulated;
			double frameMs = 0.0;
			std::uint32_t collected = 0, dropped = 0, disjoint = 0, overflowed = 0;
		};

		State& Get()
		{
			static State state;
			return state;
		}

		bool Initialize(State& a_state)
		{
			auto* device = globals::d3d::device;
			if (!device || !globals::d3d::context)
				return false;
			D3D11_QUERY_DESC desc{ D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
			for (auto& frame : a_state.frames) {
				if (FAILED(device->CreateQuery(&desc, frame.disjoint.put())))
					return false;
			}
			a_state.initialized = true;
			logger::info("[GpuEvents] GPU event timers on: every perf event's GPU time, logged every {} frames", a_state.interval);
			return true;
		}

		/** @brief A timestamp in the recording frame, or kNone when the frame has used all its queries. */
		std::uint32_t Timestamp(Frame& a_frame)
		{
			if (a_frame.used >= kMaxQueries) {
				a_frame.overflow = true;
				return kNone;
			}
			if (a_frame.used == a_frame.queries.size()) {
				D3D11_QUERY_DESC desc{ D3D11_QUERY_TIMESTAMP, 0 };
				winrt::com_ptr<ID3D11Query> query;
				if (FAILED(globals::d3d::device->CreateQuery(&desc, query.put()))) {
					a_frame.overflow = true;
					return kNone;
				}
				a_frame.queries.push_back(std::move(query));
			}
			const std::uint32_t index = a_frame.used++;
			globals::d3d::context->End(a_frame.queries[index].get());
			return index;
		}

		std::uint32_t PathOf(State& a_state, std::uint32_t a_parentPath, std::string_view a_name)
		{
			std::string path = a_parentPath == kNone ? std::string(a_name) : a_state.paths[a_parentPath] + " / " + std::string(a_name);
			const auto [at, inserted] = a_state.pathIndex.try_emplace(path, static_cast<std::uint32_t>(a_state.paths.size()));
			if (inserted) {
				a_state.paths.push_back(std::move(path));
				a_state.depth.push_back(a_parentPath == kNone ? 0u : a_state.depth[a_parentPath] + 1);
				a_state.accumulated.emplace_back();
			}
			return at->second;
		}

		/** @brief Reads a finished frame's queries into the totals; false while the GPU has not reached its end. */
		bool Collect(State& a_state, Frame& a_frame, bool a_flush)
		{
			auto* context = globals::d3d::context;
			const UINT flags = a_flush ? 0u : UINT(D3D11_ASYNC_GETDATA_DONOTFLUSH);
			D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
			if (context->GetData(a_frame.disjoint.get(), &disjoint, sizeof(disjoint), flags) != S_OK)
				return false;
			std::vector<std::uint64_t> ticks(a_frame.used);
			for (std::uint32_t i = 0; i < a_frame.used; ++i)
				if (context->GetData(a_frame.queries[i].get(), &ticks[i], sizeof(std::uint64_t), flags) != S_OK)
					return false;
			a_frame.pending = false;
			if (disjoint.Disjoint || !disjoint.Frequency || a_frame.frameBegin == kNone || a_frame.frameEnd == kNone) {
				++a_state.disjoint;
				return true;
			}
			const double toMs = 1000.0 / static_cast<double>(disjoint.Frequency);
			auto span = [&](std::uint32_t a_begin, std::uint32_t a_end) {
				return ticks[a_end] >= ticks[a_begin] ? static_cast<double>(ticks[a_end] - ticks[a_begin]) * toMs : 0.0;
			};
			std::vector<double> inclusive(a_frame.events.size(), -1.0), children(a_frame.events.size(), 0.0);
			for (std::size_t e = 0; e < a_frame.events.size(); ++e) {
				const auto& event = a_frame.events[e];
				if (event.begin == kNone || event.end == kNone)
					continue;
				inclusive[e] = span(event.begin, event.end);
				if (event.parent != kNone)
					children[event.parent] += inclusive[e];
			}
			for (std::size_t e = 0; e < a_frame.events.size(); ++e) {
				if (inclusive[e] < 0.0)
					continue;
				auto& total = a_state.accumulated[a_frame.events[e].path];
				total.inclusive += inclusive[e];
				total.self += std::max(0.0, inclusive[e] - children[e]);
				++total.count;
			}
			a_state.frameMs += span(a_frame.frameBegin, a_frame.frameEnd);
			a_state.overflowed += a_frame.overflow ? 1u : 0u;
			++a_state.collected;
			return true;
		}

		void Log(State& a_state)
		{
			const double frames = a_state.collected;
			double annotated = 0.0;
			for (std::size_t p = 0; p < a_state.paths.size(); ++p)
				if (a_state.depth[p] == 0)
					annotated += a_state.accumulated[p].inclusive;
			std::string text = fmt::format("[GpuEvents] {} frames ({} dropped unread, {} disjoint, {} out of queries): GPU frame {:.2f} ms, {:.2f} ms outside any event; per frame, inclusive / self ms, calls:",
				a_state.collected, a_state.dropped, a_state.disjoint, a_state.overflowed, a_state.frameMs / frames, (a_state.frameMs - annotated) / frames);
			for (std::size_t p = 0; p < a_state.paths.size(); ++p) {
				const auto& total = a_state.accumulated[p];
				if (total.inclusive / frames < kMinLoggedMs)
					continue;
				const auto& path = a_state.paths[p];
				const auto leaf = path.rfind(" / ");
				text += fmt::format("\n    {:>6.2f} {:>6.2f} {:>5.1f}  {}{}", total.inclusive / frames, total.self / frames, static_cast<double>(total.count) / frames,
					std::string(std::size_t(a_state.depth[p]) * 2, ' '), leaf == std::string::npos ? path : path.substr(leaf + 3));
			}
			logger::info("{}", text);
			for (auto& total : a_state.accumulated)
				total = {};
			a_state.frameMs = 0.0;
			a_state.collected = a_state.dropped = a_state.disjoint = a_state.overflowed = 0;
		}
	}

	bool Requested()
	{
		static const bool requested = ReadInterval() != 0;
		return requested;
	}

	void BeginEvent(std::string_view a_name)
	{
		if (!Requested())
			return;
		auto& state = Get();
		auto& frame = state.frames[state.write];
		if (!frame.open || GetCurrentThreadId() != state.thread) {
			if (GetCurrentThreadId() == state.thread || !state.initialized)
				state.stack.push_back(kNone);
			return;
		}
		const std::uint32_t parent = state.stack.empty() ? kNone : state.stack.back();
		const std::uint32_t parentPath = parent == kNone ? kNone : frame.events[parent].path;
		// An event under one begun outside the frame has no place in the tree: it is not timed.
		if (!state.stack.empty() && parent == kNone) {
			state.stack.push_back(kNone);
			return;
		}
		const auto index = static_cast<std::uint32_t>(frame.events.size());
		const std::uint32_t path = PathOf(state, parentPath, a_name);
		frame.events.push_back({ path, parent, Timestamp(frame) });
		state.stack.push_back(index);
	}

	void EndEvent()
	{
		if (!Requested())
			return;
		auto& state = Get();
		if (state.initialized && GetCurrentThreadId() != state.thread)
			return;
		if (state.stack.empty())
			return;
		const std::uint32_t index = state.stack.back();
		state.stack.pop_back();
		auto& frame = state.frames[state.write];
		if (index != kNone && frame.open && index < frame.events.size() && frame.events[index].begin != kNone) {
			frame.events[index].end = Timestamp(frame);
		}
	}

	void OnPresent()
	{
		if (!Requested())
			return;
		auto& state = Get();
		if (state.failed)
			return;
		if (!state.initialized) {
			state.interval = ReadInterval();
			if (!Initialize(state)) {
				state.failed = true;
				return;
			}
		}
		state.thread = GetCurrentThreadId();
		auto* context = globals::d3d::context;

		// Close the recording frame. An event still open at Present is not timed, and its end is ignored.
		auto& current = state.frames[state.write];
		if (current.open) {
			for (auto& open : state.stack)
				open = kNone;
			current.frameEnd = Timestamp(current);
			context->End(current.disjoint.get());
			current.open = false;
			current.pending = true;
		}

		// Read back every finished frame, oldest first. The oldest is the next to be reused, so its read may flush: without
		// it, a frame whose query ends DXVK has not submitted yet (nothing else flushed the context) is never readable, and
		// with DCLF off every frame was dropped unread.
		for (std::uint32_t i = 1; i <= kFrames; ++i) {
			auto& frame = state.frames[(state.write + i) % kFrames];
			if (frame.pending && !Collect(state, frame, i == 1))
				break;
		}
		if (state.collected >= state.interval)
			Log(state);

		// Open the next frame in the ring; one the GPU has still not finished is given up.
		state.write = (state.write + 1) % kFrames;
		auto& next = state.frames[state.write];
		if (next.pending) {
			next.pending = false;
			++state.dropped;
		}
		next.used = 0;
		next.events.clear();
		next.overflow = false;
		next.frameEnd = kNone;
		context->Begin(next.disjoint.get());
		next.open = true;
		next.frameBegin = Timestamp(next);
	}
}
