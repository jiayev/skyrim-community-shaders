#pragma once

// The material records (CoordinatorMaterials): the held material captures, the rewrites' and the joins' evaluations, the
// signatures' applications and their counters. FrameConstants.cpp (RefreshMaterialRecords), AccumulatePhase.cpp (the joins'
// materials) and ApplyEvents.cpp (DrainMaterialCaptures).
//
// A private base of Scene::Coordinator, included by Coordinator.h: names, types and initialisers are the coordinator's own,
// and the functions over them are declared there.

namespace DCLF::Scene
{
	struct CoordinatorMaterials
	{
		/**
		 * @brief The scene work's material captures, newest per material, let go after kMaterialSnapshotFrames unused frames. A join makes
		 * a record from one (MaterialPort::Evaluate); a material with none is requested (Channels::materialRequests).
		 */
		struct HeldMaterial
		{
			std::unique_ptr<MaterialPort::HeldSnapshot> held;
			std::uint32_t frame = 0;  // taken or used
			bool listed = false;      // on materialsCaptured since the last RewriteCapturedMaterials (each material once)
		};
		static constexpr std::uint32_t kMaterialSnapshotFrames = 8;
		ankerl::unordered_dense::map<const RE::BSShaderMaterial*, HeldMaterial> materialSnapshots;
		std::uint32_t materialSnapshotsAgedFrame = ~0u;  // the frame the held snapshots were last aged (once a frame, not a pass)
		struct MaterialSnapshotStats
		{
			std::uint64_t captured = 0, replaced = 0, stale = 0, aged = 0, made = 0, uncovered = 0, requested = 0;
			// Captures equal to the held one (no rewrite), those of which only the transform buffers moved (the watch alone), and captures
			// of a material already listed since the last rewrite.
			std::uint64_t deduped = 0, transformsOnly = 0, relisted = 0;
			// Captures whose fields moved against the held one: by 4-byte word of the material, how often it differed (the report names the
			// words that move most: what a controller writes every frame), and the first such material's vtable.
			std::uint64_t fieldsMoved = 0;
			std::array<std::uint32_t, MaterialPort::kMaxMaterialBytes / 4> wordMoved{};
			const void* fieldsMovedVtable = nullptr;
			std::array<std::pair<const void*, std::uint32_t>, 8> fieldsMovedBy{};  // by vtable, the first eight seen
		} materialSnapshotStats;
		// The parity's (CS_DCLF_PERSISTENT_PARITY, its passes): materials whose captures DrainMaterialCaptures deduplicated, evaluated as
		// a rewrite would and compared with their slots' records (RewriteCapturedMaterials). Empty otherwise.
		std::vector<const RE::BSShaderMaterial*> materialsDeduped;
		static constexpr std::uint32_t kMaterialDedupParityOffset = 37;
		/** @brief A rewrite's or a join's port evaluation and what it gave. */
		struct MaterialEvaluation
		{
			enum class Outcome : std::uint8_t
			{
				Made,       // record holds it
				Retry,      // an input the frame lacks yet (Advanced Skin's textures): again later
				Uncovered,  // a class the port does not cover
			};
			MaterialRecord record;
			Outcome outcome = Outcome::Made;
			bool same = false;  // a rewrite's: equal to the slot's record
		};
		static constexpr std::uint32_t kNoMaterialSlot = ~0u;
		// RewriteCapturedMaterials' prepare: one item per slot to evaluate (its snapshot is the held capture, stable over the pass), its result.
		struct MaterialRewriteItem
		{
			const RE::BSShaderMaterial* material = nullptr;
			const MaterialPort::MaterialSnapshot* snapshot = nullptr;
			std::uint32_t slot = 0;
			bool parity = false;  // a deduplicated capture's, evaluated for the parity only
		};
		std::vector<MaterialRewriteItem> materialRewriteItems;
		std::vector<MaterialEvaluation> materialRewriteResults;
		// The joins' prepare (BuildAccumulatePhase): each (material, pass) a join keys that no slot holds, evaluated once before the merges,
		// which store the records.
		struct JoinMaterialItem
		{
			const RE::BSShaderMaterial* material = nullptr;
			const MaterialPort::MaterialSnapshot* snapshot = nullptr;
			std::uint32_t pass = 0;
		};
		std::vector<JoinMaterialItem> joinMaterialItems;
		std::vector<MaterialEvaluation> joinMaterialResults;
		ankerl::unordered_dense::map<std::pair<const RE::BSShaderMaterial*, std::uint32_t>, std::uint32_t> joinMaterialPrepared;  // -> item
		// The signatures' applications, written in place by chunks (disjoint slots) and versioned after, in order.
		struct MaterialApplyItem
		{
			std::uint32_t slot = 0;
			std::uint32_t signature = 0;  // the signature's (its applied record)
			std::uint8_t changed = 0;     // bit 0: the record (its textures), bit 1: frame floats
		};
		std::vector<MaterialApplyItem> materialApplyItems;
		// Materials DrainMaterialCaptures kept since the last RefreshMaterialRecords, and those a rewrite could not evaluate yet (an
		// Advanced Skin key not set up: retried while the capture is held).
		std::vector<const RE::BSShaderMaterial*> materialsCaptured;
		std::vector<const RE::BSShaderMaterial*> materialRewritesPending;
		struct MaterialSignature
		{
			std::vector<std::uint32_t> slots;      // listed when keyed (materialSignatureListed), dropped as the list is walked
			MaterialPort::MaterialSnapshot probe;  // a capture of one of its materials (pure data: no pointer in it is followed)
			std::uint32_t probePass = 0;
			bool probeValid = false;
			bool probeFailed = false;  // its last evaluation failed (an Advanced Skin key not set up): the next slot noted replaces it
			MaterialRecord applied;  // the frame components last written into its slots
			bool appliedValid = false;
		};
		ankerl::unordered_dense::map<std::uint32_t, MaterialSignature> materialSignatures;
		std::vector<std::uint32_t> materialSignatureListed;  // per material slot: its signature + 1, 0 when unlisted
		struct MaterialRecordStats
		{
			// transformsWatched: captures of a material with dependents whose texture transforms alone moved while a feature patches them
			// (Extensions::FrameVSVariablePatched), and the joins' records made (nothing followed: the frame values'),
			// transformsWritten: records the scene lane wrote for them (none).
			std::uint64_t passes = 0, captured = 0, rewritten = 0, retried = 0, uncovered = 0, samples = 0, applications = 0, slotsApplied = 0,
						  transformsWatched = 0, transformsWritten = 0;
			// Evaluations on the pool (rewrites, the joins' materials) and the rounds that fanned out, the joins' material evaluations left to
			// the merge, applications written by chunks. watchesSkipped / watchesDropped: no longer moved (the transform watch is gone).
			std::uint64_t evaluationsPooled = 0, poolRounds = 0, joinPrepared = 0, joinInline = 0, watchesSkipped = 0, watchesDropped = 0,
						  appliedPooled = 0;
		} materialRecordStats;
		std::uint32_t materialReportWindow = ~0u;  // the report window (sceneFrame / Reports::Interval()) the material lines last printed in
		ankerl::unordered_dense::set<const RE::BSShaderMaterial*> materialRequested;  // asked, not answered yet
	};
}
