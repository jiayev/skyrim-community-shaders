#pragma once

// Object LOD's segment events (dclf-architecture.md, "Object LOD: segments"): the writers' hooks and the channel's queue (LodSegmentHooks.cpp), the
// engine channel, its coordinator component and report line (LodSegmentChannel.cpp), and the parity (Diagnostics/LodSegmentParity.cpp).
// The drawn ranges themselves are Core's (Api::CoordinatorComponentContext::EntryRanges: the attach's capture, the walk's range slots);
// the component keeps them up to date as the writers name the shapes. Included by the feature's sources only.

#include "Features/DrawcallLimitFix/Api/Feature.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace DCLF::Features::Lod
{
	/** @brief The component's state (scene lane): the events' counters and the parity's, since the last report. */
	struct SegmentState
	{
		struct Stats
		{
			std::uint64_t events = 0, changed = 0;
		} stats;
		// CS_DCLF_PERSISTENT_PARITY: the shapes' kept ranges against their live state.
		struct Parity
		{
			std::uint64_t checks = 0, shapes = 0, differ = 0;
			std::string first;
			std::uint32_t pass = 0;  // the pass (PassStamp) that ran the check: once a pass
		} parity;
	};

	// LodSegmentHooks.cpp.
	/** @brief The feature's early hooks (the shell's Extensions::InstallEarlyHooks): every segment writer's call patched, or none. */
	void InstallSegmentHooks();
	/** @brief Whether InstallSegmentHooks patched the writers (the report's "events not installed" otherwise). */
	bool SegmentHooksInstalled();
	/** @brief The channel's drain (Api::EngineChannel::drain): the scene lane, the queue's one consumer. */
	std::size_t DrainSegmentEvents(Api::KeySink& a_sink);

	// LodSegmentChannel.cpp.
	/** @brief Registers the channel, its component and its report line (the feature's install). */
	void RegisterSegmentChannel(Api::InstallContext& a_context);

	/**
	 * @brief The hook point (Diagnostics::Notify; the scene lane): a pass's segment events applied (the shapes' drawn ranges sampled again),
	 * every pass that applies a batch.
	 */
	struct SegmentsApplied
	{
		const Api::CoordinatorComponentContext& context;
	};

	// Diagnostics/LodSegmentParity.cpp.
	/** @brief CS_DCLF_PERSISTENT_PARITY (Diagnostics::Observe().scene): the parity's observer. */
	void SubscribeSegmentParity();
}
