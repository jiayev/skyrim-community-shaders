#pragma once

// The parts a scene pass is timed by (CS_DCLF_PROFILE, CS_DCLF_STATS): BuildFrame's parts, the pass's sub-zones and the first
// round's kinds, with their report names. Named by Coordinator::Stats and the walk's counters (State/Walk.h); included by
// Coordinator.h, before the state.

namespace DCLF
{
	/** @brief The parts of Coordinator::BuildFrame, timed separately under CS_DCLF_PROFILE. */
	enum class BuildPart : std::uint32_t
	{
		Walk,            // the setup before the loop (the walk's prologue, the joins' BindByMembership)
		PassLookup,      // the loop's order built (the joins' passes looked up)
		ClassifyStatic,  // type, skin, property, material alpha, DeriveLightingDescriptors
		ClassifyFrame,   // the parent chain (hidden, actor) and the fade
		Resolve,         // GpuResources::Resolve for the vertex and index buffers
		Dedup,           // the geometry, pipeline and material map probes
		PipelineEval,    // the new-pipeline body: its technique row and constants
		MaterialEval,    // the new-material body: the material port
		Record,          // the record's writes, and the joins' patch bookkeeping
		CapturePatch,    // the joins' room index and tree wind
		ApplyPatch,      // the joins' patch into the tables (ApplyAccumulatePatch)
		DedupHit,   // the three map probes on the HIT path (Dedup measures the misses)
		LoopTail,   // per tracked object: a rejected one's continue path, and the iteration itself
		Skinning,   // skinned objects: their palette rows counted
		Count
	};

	inline constexpr std::array<const char*, static_cast<std::size_t>(BuildPart::Count)> kBuildPartNames{
		"walk", "pass-lookup", "classify-static", "classify-frame",
		"resolve", "dedup", "pipeline-eval", "material-eval", "record", "capture-patch", "apply-patch",
		"dedup-hit", "loop-tail", "skinning"
	};

	/** @brief The sub-zones of a scene pass, each a Tracy zone and a Stats::scenePartMs sum (CS_DCLF_STATS). */
	enum class ScenePart : std::uint32_t
	{
		CategoryNodes,     // the attach queue's drain and RefreshCategoryNodes
		AttachDetach,      // AddSubtree per attach, EraseTracked per detach
		Validate,          // ValidateSlice
		StructuralEvents,  // the fade, property, node and switch queues' drains
		Prologue,          // used sets, change log trim, LOD fade settings, GpuResources::BeginFrame
		SweepSlots,        // UpdateSlotReferences and the slot expiry
		CullHiddenBits,    // CaptureCullHiddenBits
		Schedule,          // BeginWalk and the order: the per-frame set and the events' dependents
		SwitchEvents,      // ApplySwitchEvents
		Evaluate,          // the first round: per-frame entries and those the events scheduled
		GeometryScan,      // FinishDeltaWalk's geometry slot touches and the stale-slot object scan
		LaterRounds,       // the rounds for stale and refreshed geometry slots
		ShadowSets,        // RefreshShadowSets and its parity
		ObjectSweep,       // SweepObjectSlots (a full walk only)
		SunCandidates,     // UpdateSunCandidates
		FaceWalk,          // the components' walk end (ComponentsWalkEnded: Features/Faces' regions)
		WalkReference,     // the walk's observers' dense reference (DenseReference)
		Shading,           // NameShadingEvents: the shading events' slots named for FrameValues
		Placements,        // PublishPlacementPlan: the roots taken, and the walk's plan for FrameValues
		Joins,             // BuildAccumulatePhase, new pipelines' blocks, bindings, lookups
		Commit,            // CommitSet, after the joins
		Publish,           // PublishScene: set applied, claims revoked, tables and lookups published
		// Nested in the parts above (ScenePartNested): the two halves of each fan-out.
		EvaluateFanout,    // EvaluateEntry over the round (the pool from 128 entries)
		EvaluateMerge,     // MergeEntry over the round, in order
		JoinsFanout,       // EvaluateJoinPre and EvaluateJoin
		JoinsMerge,        // their merges, in order
		SubtreesFanout,    // PrepareSubtrees (the pool from 16 roots)
		SubtreesMerge,     // MergeSubtree, in event order
		Count
	};

	inline constexpr std::array<const char*, static_cast<std::size_t>(ScenePart::Count)> kScenePartNames{
		"category nodes", "attach/detach", "validate", "structural events", "prologue", "sweep slots", "cull hidden bits",
		"schedule", "switch events", "evaluate", "geometry scan", "later rounds", "shadow sets",
		"object sweep", "sun candidates", "face walk", "walk parity", "shading", "placement plan", "joins", "commit", "publish",
		"evaluate (fan-out)", "evaluate merge", "joins (fan-out)", "joins merge", "subtrees (fan-out)", "subtrees merge"
	};

	/** @brief A part measured inside another (the fan-outs' halves), left out of the parts' sum. */
	inline constexpr bool ScenePartNested(std::size_t a_part)
	{
		return a_part >= static_cast<std::size_t>(ScenePart::EvaluateFanout);
	}

	/** @brief What the first evaluation round did with an entry, for its timing (CS_DCLF_PROFILE). */
	enum class EvaluateKind : std::uint32_t
	{
		Light,        // the light path kept or moved it
		LightMissed,  // the light path tried and fell through to the full write
		Actor,        // written in full: an actor's (kTraitActor) whose verdict lets it have a record
		ActorNoRecord,  // written in full: an actor's whose verdict gives it none (hidden, fading, ...)
		EntryClass,   // written in full: an entry class's (a face shape; the report's "face")
		PerFrame,     // written in full: any other per-frame entry
		Event,        // written in full: scheduled by an event, a pending evaluation or a full walk
		Count
	};

	inline constexpr std::array<const char*, static_cast<std::size_t>(EvaluateKind::Count)> kEvaluateKindNames{
		"light path", "light path missed", "actor", "actor without a record", "face", "other per-frame", "event"
	};
}
