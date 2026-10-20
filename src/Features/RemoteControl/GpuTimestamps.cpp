#include "Features/RemoteControl/GpuTimestamps.h"

#if defined(CS_HAS_RENDER_GRAPH)
#	include "RenderGraph/RenderGraphRuntime.h"

#	include <OpenRenderGraph/PersistentGraphHost.h>
#endif

#include <atomic>
#include <iterator>
#include <map>
#include <mutex>

// NvPerf's timestamp samples (GpuTimestamps.h): the frames RenderGraphRuntime's epochs make up, assembled as they come back.
namespace NvPerf::Timestamps
{
	namespace
	{
		struct Pending
		{
			std::uint32_t submitted = 0, completed = 0, timed = 0;
			std::vector<Range> ranges;
		};

		struct State
		{
			std::atomic<bool> collecting{ false };
			std::atomic<std::uint64_t> presents{ 0 };
			std::mutex mutex;  // frames (all on the render thread today; the lock keeps it so if completions move)
			std::map<std::uint64_t, Pending> frames;
		};

		State& Get()
		{
			static State state;
			return state;
		}

#if defined(CS_HAS_RENDER_GRAPH)
		org::PersistentGraphHost* Host()
		{
			return RenderGraphRuntime::Get().Host();
		}
#endif
	}

	bool Collecting() noexcept
	{
		return Get().collecting.load(std::memory_order_acquire);
	}

	std::uint64_t CurrentFrame() noexcept
	{
		return Get().presents.load(std::memory_order_acquire);
	}

	void OnPresentEnd(std::uint64_t a_presents)
	{
		Get().presents.store(a_presents, std::memory_order_release);
	}

	void Submitted(std::uint64_t a_frame)
	{
		auto& state = Get();
		if (!a_frame || !Collecting())
			return;
		std::lock_guard lock(state.mutex);
		++state.frames[a_frame].submitted;
	}

	void Completed(std::uint64_t a_frame, std::vector<Range> a_ranges)
	{
		auto& state = Get();
		if (!a_frame)
			return;
		std::lock_guard lock(state.mutex);
		const auto found = state.frames.find(a_frame);
		if (found == state.frames.end())
			return;  // submitted before collecting started, or already given up
		auto& pending = found->second;
		++pending.completed;
		if (!a_ranges.empty()) {
			++pending.timed;
			pending.ranges.insert(pending.ranges.end(), std::make_move_iterator(a_ranges.begin()), std::make_move_iterator(a_ranges.end()));
		}
	}

	bool Start(std::string& a_error)
	{
#if defined(CS_HAS_RENDER_GRAPH)
		auto* host = Host();
		if (!host) {
			a_error = std::format("the render graph is not active ({})", RenderGraphRuntime::Get().GetDisabledReason());
			return false;
		}
		auto& state = Get();
		{
			std::lock_guard lock(state.mutex);
			state.frames.clear();
		}
		host->SetRecordingTimestamps(true);
		state.collecting.store(true, std::memory_order_release);
		logger::info("[NvPerf] timestamps: recording timestamps on (graph rebuild requested: {})", host->RebuildRequested());
		return true;
#else
		a_error = "built without the render graph: no pass timestamps";
		return false;
#endif
	}

	void Stop()
	{
		auto& state = Get();
		if (!state.collecting.exchange(false, std::memory_order_acq_rel))
			return;
		{
			std::lock_guard lock(state.mutex);
			state.frames.clear();
		}
#if defined(CS_HAS_RENDER_GRAPH)
		if (auto* host = Host())
			host->SetRecordingTimestamps(false);
		logger::info("[NvPerf] timestamps: recording timestamps off");
#endif
	}

	bool Ready()
	{
#if defined(CS_HAS_RENDER_GRAPH)
		const auto* host = Host();
		return Collecting() && host && host->RecordingTimestamps() && !host->RebuildRequested();
#else
		return false;
#endif
	}

	std::vector<Frame> TakeFrames(std::uint32_t& a_dropped)
	{
		auto& state = Get();
		std::vector<Frame> out;
		const auto now = CurrentFrame();
		std::lock_guard lock(state.mutex);
		// In frame order: a frame waits for the ones before it (completions arrive in slot order, not submission order).
		for (auto it = state.frames.begin(); it != state.frames.end();) {
			auto& [frame, pending] = *it;
			const bool closed = frame <= now;  // its present has passed: no epoch submits into it any more
			if (closed && pending.completed >= pending.submitted) {
				out.push_back({ frame, pending.submitted, pending.timed, std::move(pending.ranges) });
				it = state.frames.erase(it);
				continue;
			}
			if (closed && now > frame + kMaxLagFrames) {
				++a_dropped;
				it = state.frames.erase(it);
				continue;
			}
			break;
		}
		return out;
	}
}
