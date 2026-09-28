#pragma once

#include "CapturedScene.h"

namespace DCLF::Published
{
	// Coordinator-owned pending target, not an engine-thread mailbox. On pressure
	// the caller retains/retries the event; lifecycle events must NEVER be dropped.
	// Only adjacent, structurally equivalent whole-group RefreshGroup events merge.
	// Byte accounting covers group-owned vector capacities, not allocator overhead
	// or leased resource storage. Event metadata has its own fixed entry bound.
	// The coordinator must supply all other build/staging bytes on each admission;
	// this class alone does not implement the total preparation-memory budget.
	class CaptureAdmission
	{
	public:
		enum class Result { Accepted, Coalesced, Pressure, Invalid };
		explicit CaptureAdmission(std::size_t byteBudget = 128u * 1024u * 1024u, std::size_t eventLimit = 4096);
		Result TryPush(const CapturedSceneEvent&, std::size_t otherPreparationBytes = 0);
		CapturedSceneUpdate TakePending();
		CapturedSceneReducer::Result ApplyPending(CapturedSceneReducer&);
		std::size_t PendingBytes() const { return pendingBytes; }
		std::size_t PendingEvents() const { return pending.events.size(); }
	private:
		std::size_t budget, limit, pendingBytes = 0;
		std::uint64_t lastSequence = 0;
		CapturedSceneUpdate pending;
	};
}
