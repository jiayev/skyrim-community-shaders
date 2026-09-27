#pragma once

#include "CaptureAdmission.h"

#include <optional>

namespace DCLF::Published
{
	struct CaptureBuildRequest
	{
		std::uint64_t ticket = 0;
		std::shared_ptr<const CapturedSceneState> scene;
		std::size_t reservedOutputBytes = 0;
	};
	// Offline CPU result, NOT an executable GPU publication or ownership claim.
	struct PreparedCapture
	{
		CaptureBuildRequest request;
		std::shared_ptr<const void> artifact;
	};

	// All methods are coordinator-only. Workers receive owning immutable requests
	// and post completion back; they never access this mutable state. No waits.
	// Conservative initial policy: do not start another build until ready is acked.
	class CapturePreparation
	{
	public:
		explicit CapturePreparation(std::size_t byteBudget = 128u * 1024u * 1024u, std::size_t eventLimit = 4096);
		CaptureAdmission::Result Post(const CapturedSceneEvent&);
		std::optional<CaptureBuildRequest> Begin(std::size_t reservedOutputBytes);
		// True means the matching completion was consumed, not necessarily ready:
		// invalidated, null or over-reservation results schedule a retry instead.
		bool Complete(std::uint64_t ticket, std::shared_ptr<const void> artifact, std::size_t outputBytes);
		bool Fail(std::uint64_t ticket);
		std::shared_ptr<const PreparedCapture> Ready() const { return ready; }
		bool Acknowledge(std::uint64_t ticket, bool selected);
		bool Building() const { return building.has_value(); }
		std::size_t PendingEvents() const { return admission.PendingEvents(); }
		std::uint64_t Lifecycle() const { return lifecycle; }
	private:
		std::size_t budget, chargedBytes = 0;
		std::uint64_t lifecycle = 0, nextTicket = 1;
		bool retry = false;
		CaptureAdmission admission;
		CapturedSceneReducer reducer;
		std::optional<CaptureBuildRequest> building;
		std::shared_ptr<const PreparedCapture> ready;
	};
}
