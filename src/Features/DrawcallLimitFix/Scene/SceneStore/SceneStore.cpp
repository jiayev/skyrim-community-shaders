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
		placementPlan.reset();
		placementPlanReady.reset();
		tracked.clear();
		ClearFaceShapes();
		++trackedLayout;
		sceneIdentity.Reset();
		categoryNodes.clear();
		categoryAppliedGeneration = 0;
		alwaysRenderRoots.clear();
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
		memberDecals.clear();
		frameTables.materialEvaluationsPending.clear();
		NoteDecalsCleared();
		fadeChanged.clear();
		fadeDependents.clear();
		fadeRootOwned.clear();
		propertyChanged.clear();
		nodeChanged.clear();
		dirtyRoots.clear();
		propertyDependents.clear();
		rootDependents.clear();
		lightDependents.clear();
		ReleaseRootOwners();
		rootReference.clear();
		referenceRoot.clear();
		rootMotion.clear();
		movingRoots.clear();
		hiddenDependents.clear();
		DropSunCandidates();
		buckets = {};
	}

	std::int32_t SceneStore::FindObject(const RE::BSGeometry* a_geometry) const
	{
		GuardFrameAccess("FindObject");
		const auto it = tracked.find(const_cast<RE::BSGeometry*>(a_geometry));
		return it == tracked.end() || it->second.objectStamp != objectStamp ? -1 : static_cast<std::int32_t>(it->second.objectId);
	}

	std::int32_t SceneStore::FindLayerObject(const RE::BSGeometry* a_geometry) const
	{
		GuardFrameAccess("FindLayerObject");
		const auto it = tracked.find(const_cast<RE::BSGeometry*>(a_geometry));
		if (it == tracked.end() || it->second.objectStamp != objectStamp || it->second.layerSlot == kNoObjectSlot || !tables.IsLayer(it->second.layerSlot))
			return -1;
		return static_cast<std::int32_t>(it->second.layerSlot);
	}

	Ineligible SceneStore::Classify(RE::BSGeometry* a_geometry) const
	{
		auto it = tracked.find(a_geometry);
		if (it == tracked.end())
			return Ineligible::NotTriShape;
		// The render thread's diagnostics (CaptureParity): the live records.
		const Ineligible reason = ClassifyStatic(SceneCapture::LiveLeaf(*a_geometry).view, nullptr);
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

	namespace
	{
		std::string DescribeUser(const RE::BSGeometry* a_geometry)
		{
			if (!a_geometry)
				return "no geometry";
			const char* name = a_geometry->name.c_str();
			auto* ref = a_geometry->GetUserData();
			const RE::NiAVObject* root = a_geometry;
			for (auto* node = a_geometry->parent; node; node = node->parent) {
				root = node;
				if (!ref)
					ref = node->GetUserData();
			}
			const auto* base = ref ? ref->GetBaseObject() : nullptr;
			return fmt::format("'{}' ref {:08X} ({}) under '{}'", name ? name : "", ref ? ref->GetFormID() : 0u, base ? RE::FormTypeToString(base->GetFormType()) : "no base",
				root->name.c_str() ? root->name.c_str() : "");
		}
	}

	std::string SceneStore::DescribePipelineUsers(std::uint32_t a_pipeline) const
	{
		std::uint32_t users = 0;
		std::int64_t first = -1;
		for (std::size_t o = 0; o < tables.objects.size() && o < tables.objectGeometry.size(); ++o) {
			if ((tables.objects[o].flags & kObjectFree) || tables.objects[o].pipelineIndex != a_pipeline || !tables.objectGeometry[o])
				continue;
			if (users++ == 0)
				first = static_cast<std::int64_t>(o);
		}
		if (first < 0)
			return "no object yet";
		return fmt::format("{} objects, first {} {}{}", users, first, DescribeUser(tables.objectGeometry[first]), tables.IsLayer(static_cast<std::uint32_t>(first)) ? " (its layer)" : "");
	}

	std::string SceneStore::DescribeShadowKeyUsers(const ShadowPipelineKey& a_key, std::uint32_t a_occlusion) const
	{
		const auto& index = a_occlusion < kOcclusionViews ? occlusionKeyMembers[a_occlusion] : shadowKeyMembers;
		const auto it = index.find(a_key);
		if (it == index.end() || it->second.empty())
			return "no caster";
		const std::uint32_t first = *it->second.begin();
		const auto* geometry = first < tables.objectGeometry.size() ? tables.objectGeometry[first] : nullptr;
		return fmt::format("{} casters, first {} {}", it->second.size(), first, DescribeUser(geometry));
	}
}
