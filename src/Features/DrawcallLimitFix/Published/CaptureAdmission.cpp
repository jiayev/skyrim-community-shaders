#include "CaptureAdmission.h"

#include <limits>
#include <stdexcept>
#include <utility>

namespace DCLF::Published
{
	CaptureAdmission::CaptureAdmission(std::size_t byteBudget, std::size_t eventLimit) : budget(byteBudget), limit(eventLimit)
	{
		if (!budget || !limit) throw std::invalid_argument("capture admission limits must be positive");
		pending.events.reserve(limit);
	}

	CaptureAdmission::Result CaptureAdmission::TryPush(const CapturedSceneEvent& event, std::size_t otherPreparationBytes)
	{
		if (!event.sequence || event.sequence <= lastSequence || !event.lifecycle) return Result::Invalid;
		const bool group = event.kind == SceneEventKind::ReplaceGroup || event.kind == SceneEventKind::RefreshGroup;
		if (group) {
			if (!event.update || event.group != event.update->Group() || event.lifecycle != event.update->Lifecycle()) return Result::Invalid;
		} else if (event.kind == SceneEventKind::Reset) {
			if (event.update || event.group != Identity{}) return Result::Invalid;
		} else if (event.kind == SceneEventKind::DetachGroup) {
			if (event.update || !event.group.id || !event.group.incarnation) return Result::Invalid;
		} else return Result::Invalid;
		bool merge = false;
		if (!pending.events.empty() && event.kind == SceneEventKind::RefreshGroup) {
			const auto& tail = pending.events.back();
			merge = tail.kind == SceneEventKind::RefreshGroup && event.update->CanRefresh(*tail.update);
		}
		if (!merge && pending.events.size() == limit) return Result::Pressure;
		const auto removed = merge ? pending.events.back().update->OwnedBytes() : 0;
		const auto added = group ? event.update->OwnedBytes() : 0;
		const auto kept = pendingBytes - removed;
		if (added > std::numeric_limits<std::size_t>::max() - kept) return Result::Pressure;
		const auto nextBytes = kept + added;
		const bool fits = otherPreparationBytes <= budget && nextBytes <= budget - otherPreparationBytes;
		// A single oversized group is permitted only after other preparation drains.
		const bool oversized = group && pending.events.empty() && otherPreparationBytes == 0;
		if (!fits && !oversized) return Result::Pressure;
		if (merge) pending.events.back() = event;
		else pending.events.push_back(event);
		pendingBytes = nextBytes;
		lastSequence = event.sequence;
		return merge ? Result::Coalesced : Result::Accepted;
	}

	CapturedSceneUpdate CaptureAdmission::TakePending()
	{
		// Allocate the next bounded envelope before transferring ownership, so an
		// allocation failure cannot consume pending lifecycle records.
		CapturedSceneUpdate replacement;
		replacement.events.reserve(limit);
		std::swap(replacement, pending);
		pendingBytes = 0;
		return replacement;
	}

	CapturedSceneReducer::Result CaptureAdmission::ApplyPending(CapturedSceneReducer& reducer)
	{
		// Copy before applying: allocation failure preserves the ordered envelope.
		const auto result = reducer.Apply(pending);
		pending.events.clear();
		pendingBytes = 0;
		return result;
	}
}
