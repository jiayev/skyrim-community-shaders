#include "SceneObservers.h"
#include "SceneState.h"

#include "Features/DrawcallLimitFix/Scene/Coordinator/Internal.h"

// CS_DCLF_CENSUS: the coordinator's verdicts on the geometries the registration census saw the engine draw (Diagnostics/Engine/
// RegistrationCensus), answered after the joins of the pass that takes the request.

namespace DCLF::Diagnostics
{
	namespace
	{
		using V = SceneView;

		void OnJoinsEnd(const JoinsEnd& a_event)
		{
			// The geometries the census saw the engine draw, each with the coordinator's verdict - what the render thread cannot read while
			// the passes run beside the frame. The pins go back with the reply: the render thread lets go of them (engine references are
			// never released on DCLF's threads). One request in flight (the census posts the next once it took this reply), so the reply
			// slot is empty here.
			auto& state = SceneDiagnostics();
			auto request = state.censusRequest.exchange(nullptr, std::memory_order_acq_rel);
			if (!request)
				return;
			auto reply = std::make_shared<CensusReply>();
			reply->sceneFrame = V::sceneFrame(a_event.store);
			reply->verdicts.reserve(request->geometries.size());
			for (const auto& geometry : request->geometries)
				reply->verdicts.push_back(CensusVerdictOf(a_event.store, geometry.get()));
			reply->geometries = std::move(request->geometries);
			state.censusReply.store(std::move(reply), std::memory_order_release);
		}
	}

	CensusVerdict CensusVerdictOf(const Scene::Coordinator& a_store, const RE::BSGeometry* a_geometry)
	{
		// The tracked entry by pointer: its verdict this pass - the joins' if they made one this pass, else the walk's (ReasonThisFrame, by
		// the coordinator's frame) - then its record, residency and phases. Nothing of the geometry is read.
		CensusVerdict out;
		const auto& tracked = V::tracked(a_store);
		const auto it = tracked.find(const_cast<RE::BSGeometry*>(a_geometry));
		if (it == tracked.end())
			return out;
		out.tracked = true;
		const auto& entry = it->second;
		out.reason = entry.accumulateReasonFrame == V::sceneFrame(a_store) && entry.accumulateReason != Ineligible::None ? entry.accumulateReason : entry.candidateReason;
		if (const std::int32_t slot = a_store.FindObject(a_geometry); slot >= 0) {
			out.record = true;
			out.resident = V::ResidentObject(a_store, slot);
			out.phases = Scene::PhasesIn(V::tables(a_store), static_cast<std::uint32_t>(slot));
		}
		return out;
	}

	void PostCensusRequest(std::shared_ptr<CensusRequest> a_request)
	{
		SceneDiagnostics().censusRequest.store(std::move(a_request), std::memory_order_release);
	}

	std::shared_ptr<CensusReply> TakeCensusReply()
	{
		return SceneDiagnostics().censusReply.exchange(nullptr, std::memory_order_acq_rel);
	}

	void InstallCensus()
	{
		Subscribe<JoinsEnd>(OnJoinsEnd);
	}
}
