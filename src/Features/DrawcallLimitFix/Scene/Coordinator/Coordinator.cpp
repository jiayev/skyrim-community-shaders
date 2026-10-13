// Coordinator's singleton and its per-geometry lookups (the tracked map, read on the coordinator thread or by the render thread's
// diagnostics).

#include "Internal.h"
#include "Features/DrawcallLimitFix/Scene/FrameView/FrameView.h"

namespace DCLF::Scene
{
	Coordinator& Coordinator::Get()
	{
		static Coordinator store;
		return store;
	}

	std::uint32_t Coordinator::GetFrame() const
	{
		return sceneWorkThread ? sceneFrame : FrameView::Get().GetFrame();
	}

	std::int32_t Coordinator::FindObject(const RE::BSGeometry* a_geometry) const
	{
		const auto it = tracked.find(const_cast<RE::BSGeometry*>(a_geometry));
		return it == tracked.end() || it->second.objectStamp != objectStamp ? -1 : static_cast<std::int32_t>(it->second.objectId);
	}

	std::int32_t Coordinator::FindLayerObject(const RE::BSGeometry* a_geometry) const
	{
		const auto it = tracked.find(const_cast<RE::BSGeometry*>(a_geometry));
		if (it == tracked.end() || it->second.objectStamp != objectStamp || it->second.layerSlot == kNoObjectSlot || !tables.IsLayer(it->second.layerSlot))
			return -1;
		return static_cast<std::int32_t>(it->second.layerSlot);
	}

	Ineligible Coordinator::Classify(RE::BSGeometry* a_geometry) const
	{
		auto it = tracked.find(a_geometry);
		if (it == tracked.end())
			return Ineligible::NotTriShape;
		// The render thread's diagnostics (the capture check): the live records.
		const Ineligible reason = Eligibility::Static(SceneCapture::LiveLeaf(*a_geometry).view, nullptr);
		return reason != Ineligible::None ? reason : ClassifyFrame(it->second);
	}

	bool Coordinator::GetTrackInfo(const RE::BSGeometry* a_geometry, std::uint32_t& a_frame, TrackSource& a_source) const
	{
		const auto it = tracked.find(const_cast<RE::BSGeometry*>(a_geometry));
		if (it == tracked.end())
			return false;
		a_frame = it->second.trackedFrame;
		a_source = it->second.trackedBy;
		return true;
	}

	bool Coordinator::GetCategoryInfo(const RE::NiNode* a_node, std::uint32_t& a_frame, std::uint8_t& a_cause) const
	{
		const auto it = categoryFound.find(a_node);
		if (it == categoryFound.end())
			return false;
		a_frame = it->second.first;
		a_cause = it->second.second;
		return true;
	}

	bool Coordinator::IsTracked(const RE::BSGeometry* a_geometry) const
	{
		return tracked.contains(const_cast<RE::BSGeometry*>(a_geometry));
	}

}
