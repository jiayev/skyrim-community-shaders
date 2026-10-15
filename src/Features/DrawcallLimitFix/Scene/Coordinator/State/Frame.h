#pragma once

// The frame inputs (CoordinatorFrame): the newest frame inputs taken, the coordinator's frame and pass count, the room map, the
// pipeline frame, the channels and the technique rows' witness. Taken at each pass's start (ScenePump.cpp), read by every phase.
//
// A private base of Scene::Coordinator, included by Coordinator.h: names, types and initialisers are the coordinator's own,
// and the functions over them are declared there.

namespace DCLF::Scene
{
	struct CoordinatorFrame
	{
		std::shared_ptr<const GeometryPort::PipelineFrame> scenePipelineFrame;  // the scene lane's: the newest taken
		FrameInputs sceneInputs;              // the newest taken
		std::uint32_t verdictsApplied = 0;    // the verdicts generation last applied (InvalidateVerdicts)
		// The coordinator's frame (from its last frame inputs; GetFrame on a scene work thread) and pass count (PassStamp).
		std::uint32_t sceneFrame = 0;
		std::uint64_t passSerial = 0;
		std::shared_ptr<const ankerl::unordered_dense::map<const RE::NiNode*, int>> roomMap;
		std::uint64_t roomMapGeneration = ~0ull;
		Channels& channels = Channels::Get();
		// The frame inputs the technique rows were last evaluated against; fog left out (a row keeps its fog: KeepTechniqueFog).
		struct TechniqueWitness
		{
			TechniqueInputs inputs{};
			std::uint8_t byte12 = 0, byte7 = 0;
			bool valid = false;
		} techniqueWitness;
	};
}
