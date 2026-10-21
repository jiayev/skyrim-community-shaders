#pragma once

#include <cstdint>
#include <string>
#include <vector>

/**
 * @brief Per-frame GPU times of the render graph's ranges from OpenRenderGraph's pass timestamps: the samples of NvPerf's
 * timestamp traces (NvPerfTrace.h, "source": "timestamps"). Cheap and continuous: every frame is a sample, on every queue.
 *
 * Collecting turns on the graph host's recording timestamps (PersistentGraphHost::SetRecordingTimestamps), which rebuilds the
 * graph so that the recordings kept across frames (Drawcall Limit Fix's revision epochs) are recorded again with timestamps
 * of their own; Ready() tells when that build has happened. Stopping turns them off (another rebuild).
 *
 * RenderGraphRuntime reports each epoch it submits (Submitted, with the frame it ran in) and each epoch read back, as its frame
 * slot completes (Completed: every pass and barrier batch it timed). A frame is complete once every epoch it submitted has
 * come back; TakeFrames hands complete frames over in frame order, and drops a frame whose epochs have not all come back
 * kMaxLagFrames frames later.
 *
 * Render thread only, apart from Collecting() and CurrentFrame() (any thread). Needs the render graph (CS_HAS_RENDER_GRAPH):
 * without it Start fails.
 */
namespace NvPerf::Timestamps
{
	enum class Queue : std::uint8_t
	{
		Graphics,
		Compute,
		Copy,
	};

	struct Range
	{
		std::string name;
		Queue queue = Queue::Graphics;
		bool barrier = false;   // a barrier batch around a pass (cs.org.barrier.<where>.<pass>)
		bool nested = false;    // a part of a pass, inside its range (NvPerf::PushCommandBufferRange: a shadow view, a draw group)
		bool replayed = false;  // from a recording kept across frames (its own queries)
		double us = 0.0;        // end - begin
		// The queue's ticks (comparable within one queue, across its epochs) and their length: a trace's spans (NvPerfTrace.h).
		std::uint64_t beginTick = 0, endTick = 0;
		double usPerTick = 0.0;
	};

	struct Frame
	{
		std::uint64_t frame = 0;  // CurrentFrame() + 1 when its epochs were submitted
		std::uint32_t epochs = 0;
		std::uint32_t timedEpochs = 0;  // epochs that came back with timestamps
		std::vector<Range> ranges;      // every epoch's, in completion order
	};

	inline constexpr std::uint64_t kMaxLagFrames = 64;

	/// Whether a trace collects. Cheap; RenderGraphRuntime gathers ranges only while it does.
	bool Collecting() noexcept;
	/// Frames presented so far (NvPerf's present count).
	std::uint64_t CurrentFrame() noexcept;
	/// Render thread, from NvPerf::OnPresentEnd.
	void OnPresentEnd(std::uint64_t a_presents);

	/// An epoch submitted in a_frame (non-zero), whose completion follows.
	void Submitted(std::uint64_t a_frame);
	/// An epoch of a_frame read back; a_ranges is empty when it carried no timestamps.
	void Completed(std::uint64_t a_frame, std::vector<Range> a_ranges);

	/// Starts collecting: recording timestamps on (a graph rebuild). False without an active render graph (a_error says why).
	bool Start(std::string& a_error);
	/// Stops collecting: recording timestamps off (a graph rebuild); the frames not taken are dropped.
	void Stop();
	/// Whether the graph has been built with recording timestamps since Start.
	bool Ready();
	/// The frames completed since the last call, in frame order; a_dropped counts the incomplete ones given up.
	std::vector<Frame> TakeFrames(std::uint32_t& a_dropped);
}
