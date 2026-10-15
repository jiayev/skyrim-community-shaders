#pragma once

// The publication (CoordinatorPublication): the published table snapshots and their replay, the publication's sequence,
// commit and claims, and the deltas' imports. SceneSet.cpp (PublishScene, PublishTables).
//
// A private base of Scene::Coordinator, included by Coordinator.h: names, types and initialisers are the coordinator's own,
// and the functions over them are declared there.

namespace DCLF::Scene
{
	struct CoordinatorPublication
	{
		std::vector<std::shared_ptr<const void>> retiredImports;  // into the next publication's deltas (TakeDeltas)
		// The published table snapshots (PublishTables), pooled: one no publication holds any more is written again.
		std::vector<std::shared_ptr<SceneTables>> tablesPool;
		// Per pool entry: whether it equals the tables at its change log's end (brought up to date by replay); a new snapshot is copied
		// whole once.
		std::vector<std::uint8_t> tablesPoolReplayable;
		// A replay's scratch (PublishTables), collected once and read by its column jobs: the object slots the logs name (sorted) with the
		// causes each gathered (0: not named), the material slots the material log names (sorted), and the geometry slots' marks.
		std::vector<std::uint32_t> replaySlots;
		std::vector<std::uint32_t> replaySlotCauses;
		std::vector<std::uint32_t> replayMaterialSlots;
		std::vector<std::uint8_t> replayMaterialMarks, replayGeometryMarks;
		// The replay's object slots and material records by pool chunks from these counts (FanoutMode; smaller replays run on the lane).
		static constexpr std::size_t kReplayMinSlots = 96, kReplayMinMaterials = 24;
		// Material records a replay chunk copies (2.4 KB each).
		static constexpr std::size_t kReplayMaterialGrain = 32;
		// The last published snapshot.
		std::shared_ptr<SceneTables> publishedTables;
		using PublicationDeltas = DCLF::PublicationDeltas;
		std::uint64_t publicationSequence = 0;
		std::uint32_t publicationCommitFrame = 0;    // the commit the next publication applies
		std::uint32_t publicationCommitToggles = 0;  // the toggles generation that commit was made under
		std::shared_ptr<const SetSnapshot> publicationClaims;
		std::vector<const RE::BSGeometry*> publicationJoined, publicationLeft;
	};
}
