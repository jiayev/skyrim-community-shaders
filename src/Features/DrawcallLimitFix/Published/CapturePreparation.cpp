#include "CapturePreparation.h"

#include <limits>

namespace DCLF::Published
{
	CapturePreparation::CapturePreparation(std::size_t byteBudget, std::size_t eventLimit) :
		budget(byteBudget), admission(byteBudget, eventLimit) {}

	CaptureAdmission::Result CapturePreparation::Post(const CapturedSceneEvent& event)
	{
		using Result = CaptureAdmission::Result;
		if (event.kind == SceneEventKind::Reset ? event.lifecycle <= lifecycle : event.lifecycle != lifecycle)
			return Result::Invalid;
		const auto result = admission.TryPush(event, chargedBytes);
		if ((result == Result::Accepted || result == Result::Coalesced) && event.kind == SceneEventKind::Reset) {
			lifecycle = event.lifecycle;
			// A worker still owns its old inputs until completion. Never free its
			// reservation early just because its result can no longer be accepted.
			if (ready) { ready.reset(); chargedBytes = 0; }
		}
		return result;
	}

	std::optional<CaptureBuildRequest> CapturePreparation::Begin(std::size_t reservedOutputBytes)
	{
		if (building || ready || (!retry && !admission.PendingEvents()) || nextTicket == 0) return {};
		if (admission.PendingEvents()) {
			admission.ApplyPending(reducer);
			retry = true;
		}
		const auto scene = reducer.Snapshot();
		std::size_t bytes = 0;
		for (const auto& [id, group] : scene->groups) {
			const auto size = group->OwnedBytes();
			if (size > std::numeric_limits<std::size_t>::max() - bytes) return {};
			bytes += size;
		}
		if (reservedOutputBytes > std::numeric_limits<std::size_t>::max() - bytes) return {};
		bytes += reservedOutputBytes;
		// Full-root charging is deliberately conservative until active-page accounting
		// exists. Never silently exempt an arbitrary multi-group root from the budget.
		if (bytes > budget && scene->groups.size() != 1) return {};
		building = CaptureBuildRequest{ nextTicket++, scene, reservedOutputBytes };
		chargedBytes = bytes;
		retry = false;
		return building;
	}

	bool CapturePreparation::Complete(std::uint64_t ticket, std::shared_ptr<const void> artifact, std::size_t outputBytes)
	{
		if (!building || building->ticket != ticket) return false;
		if (!artifact || outputBytes > building->reservedOutputBytes || building->scene->lifecycle != lifecycle)
			return Fail(ticket);
		// Allocate before changing state: callers can retry completion on allocation failure.
		auto candidate = std::make_shared<const PreparedCapture>(PreparedCapture{ *building, std::move(artifact) });
		ready = std::move(candidate);
		building.reset();
		return true;
	}

	bool CapturePreparation::Fail(std::uint64_t ticket)
	{
		if (!building || building->ticket != ticket) return false;
		building.reset();
		chargedBytes = 0;
		retry = true;
		return true;
	}

	bool CapturePreparation::Acknowledge(std::uint64_t ticket, bool selected)
	{
		if (!ready || ready->request.ticket != ticket) return false;
		ready.reset();
		chargedBytes = 0;
		retry = !selected;
		return true;
	}
}
