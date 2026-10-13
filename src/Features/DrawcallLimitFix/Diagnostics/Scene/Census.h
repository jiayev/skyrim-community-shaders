#pragma once

// The registration census's question to the coordinator and its answer: Diagnostics/Engine/RegistrationCensus asks on the render
// thread, SceneCensus.cpp answers in the next scene pass.

#include "Features/DrawcallLimitFix/Scene/Ports/LightingDescriptors.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace RE
{
	class BSGeometry;
}

namespace DCLF
{
	namespace Scene
	{
		class Coordinator;
	}
}

namespace DCLF::Diagnostics
{
	/**
	 * @brief The coordinator's verdict on a geometry the census (CS_DCLF_CENSUS) saw the engine draw: tracked, and if so its verdict this
	 * frame (hidden, switch, actor, fading and the static ones: the joins' verdict of this frame, else the walk's), its record, its
	 * residency and its phases in the coordinator's tables. By pointer: nothing of the geometry is read (the request's pins keep the
	 * address its own).
	 */
	struct CensusVerdict
	{
		bool tracked = false;
		bool record = false;    // a record (FindObject)
		bool resident = false;  // bound (ResidentObject)
		Ineligible reason = Ineligible::None;
		std::uint8_t phases = 0;  // PhasesIn of its record, the coordinator's tables
	};
	/**
	 * @brief The census's question (render thread, Collect -> the next scene pass, SceneCensus.cpp) and its answer (the pass -> the render
	 * thread's next Collect). One in flight at a time: the census posts the next only once it took the reply, carrying every geometry seen
	 * since. The pins (taken on the render thread) travel back with the reply, so they are let go of on the render thread.
	 */
	struct CensusRequest
	{
		std::vector<RE::NiPointer<RE::BSGeometry>> geometries;
	};
	struct CensusReply
	{
		std::vector<RE::NiPointer<RE::BSGeometry>> geometries;
		std::vector<CensusVerdict> verdicts;  // parallel to geometries
		std::uint32_t sceneFrame = 0;         // the pass's
	};
	/** @brief The coordinator's thread, or the render thread while the passes run inline (the parities): a_geometry's verdict. */
	CensusVerdict CensusVerdictOf(const Scene::Coordinator& a_store, const RE::BSGeometry* a_geometry);
	/** @brief Render thread (the census's Collect): the request posted for the next scene pass (no wake: the frame's pass takes it). */
	void PostCensusRequest(std::shared_ptr<CensusRequest> a_request);
	/** @brief Render thread (the census's Collect): the reply posted since the last call, or null. */
	std::shared_ptr<CensusReply> TakeCensusReply();
}
