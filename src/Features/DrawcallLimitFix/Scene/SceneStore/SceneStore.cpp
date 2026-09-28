#include "Internal.h"

namespace DCLF
{
	SceneStore& SceneStore::Get()
	{
		static SceneStore store;
		return store;
	}

	void SceneStore::Clear()
	{
		tracked.clear();
		sceneIdentity.Reset();
		categoryNodes.clear();
		ResetSlotTables();
		InvalidateObjectIndices();
		// They hold raw pointers into game allocations now that they outlive the frame, so the teardown
		// paths have to drop them rather than leave them to the next BuildFrame.
		geometryIndex.clear();
		pipelineIndex.clear();
		materialIndex.clear();
		validationCursor = 0;
		fullEvaluation = true;
		perFrameSet.clear();
		pendingEvaluation.clear();
		accumulatePatched.clear();
		lastPatched.clear();
		fadeChanged.clear();
		fadeDependents.clear();
		propertyChanged.clear();
		nodeChanged.clear();
		dirtyRoots.clear();
		propertyDependents.clear();
		rootDependents.clear();
		rootMotion.clear();
		DropSunCandidates();
		buckets = {};
	}

	std::int32_t SceneStore::FindObject(const RE::BSGeometry* a_geometry) const
	{
		const auto it = tracked.find(const_cast<RE::BSGeometry*>(a_geometry));
		return it == tracked.end() || it->second.objectStamp != objectStamp ? -1 : static_cast<std::int32_t>(it->second.objectId);
	}

	Ineligible SceneStore::Classify(RE::BSGeometry* a_geometry) const
	{
		auto it = tracked.find(a_geometry);
		if (it == tracked.end())
			return Ineligible::NotTriShape;
		const Ineligible reason = ClassifyStatic(*a_geometry, nullptr);
		return reason != Ineligible::None ? reason : ClassifyFrame(it->second);
	}

	bool SceneStore::GetTrackInfo(const RE::BSGeometry* a_geometry, std::uint32_t& a_frame, TrackSource& a_source) const
	{
		const auto it = tracked.find(const_cast<RE::BSGeometry*>(a_geometry));
		if (it == tracked.end())
			return false;
		a_frame = it->second.trackedFrame;
		a_source = it->second.trackedBy;
		return true;
	}

	bool SceneStore::GetCategoryInfo(const RE::NiNode* a_node, std::uint32_t& a_frame, std::uint8_t& a_cause) const
	{
		const auto it = categoryFound.find(a_node);
		if (it == categoryFound.end())
			return false;
		a_frame = it->second.first;
		a_cause = it->second.second;
		return true;
	}

	bool SceneStore::IsTracked(const RE::BSGeometry* a_geometry) const
	{
		return tracked.contains(const_cast<RE::BSGeometry*>(a_geometry));
	}

	Ineligible SceneStore::ReasonThisFrame(const RE::BSGeometry* a_geometry, bool* a_accumulate) const
	{
		if (a_accumulate)
			*a_accumulate = false;
		const auto it = tracked.find(const_cast<RE::BSGeometry*>(a_geometry));
		if (it == tracked.end())
			return Ineligible::None;
		if (it->second.accumulateReasonFrame == frame) {
			if (a_accumulate)
				*a_accumulate = true;
			return it->second.accumulateReason;
		}
		return it->second.candidateReason;
	}
}
