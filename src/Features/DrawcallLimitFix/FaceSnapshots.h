#pragma once

#include <atomic>
#include <cstdint>
#include <memory>

namespace RE
{
	class BSDynamicTriShape;
	class BSFaceGenNiNode;
}

namespace DCLF
{
	/**
	 * @brief The positions of NPC face shapes (BSDynamicTriShape under a BSFaceGenNiNode: head, mouth, hair,
	 * brows), published by the engine's own writer so that DCLF never reads BSDynamicTriShape::dynamicData
	 * or takes its spin lock.
	 *
	 * The engine has one writer of a face's positions (docs/development/skyrim-engine-notes.md, "Face
	 * morphing"): the "Face morphing" job of the "Main post render" stage (AE 0x1406d36b0), which runs one job
	 * per BSFaceGenNiNode (FUN_1404334e0 -> FUN_140432550) that resets and morphs every shape of that head,
	 * then joins them (JobList::Finish). Two thunks publish from there:
	 * - after a head's job has morphed it (0x1404334f3), on that job's thread: a copy of every shape of the
	 *   head DCLF registered, published as one snapshot;
	 * - after the stage's join (0x1406d36fd), when no morph job can run: a first snapshot of the heads that
	 *   asked for one (new, rebuilt, or never animated, which the stage never queues).
	 *
	 * A head's snapshots are a triple buffer: the writer fills its slot and exchanges it into `latest`; the
	 * reader (the scene walk) exchanges its slot for `latest` when that holds a fresh one. Neither waits, and
	 * every shape of a head is read from one slot, so from one run of its job: a head is never drawn from two
	 * different updates. With the engine's schedule the walk reads what the post-render stage of the previous
	 * frame wrote, which is what the engine's own draws read.
	 *
	 * Records are created, rebuilt and retired by the scene walk alone (one thread at a time). The writers
	 * find them through a fixed open-addressed table keyed by the head, and a retired record is freed only
	 * once a morph stage has completed after its retirement - no job spans a stage's join.
	 */
	class FaceSnapshots
	{
	public:
		static FaceSnapshots& Get();

		/** @brief CS_DCLF_FACEGEN (default on), and the writer's thunks are installed (AE). */
		static bool Enabled();

		/** @brief Installs the two thunks in the engine's face morphing (AE only). */
		void Install();

		/** @brief A face shape's positions in the snapshot the walk holds: float4 per vertex. */
		struct ShapeView
		{
			const float* positions = nullptr;
			std::uint32_t vertexCount = 0;
			std::uint64_t generation = 0;  // unique across heads: a new snapshot of any head has a new one
		};

		// ---- The scene walk (one thread at a time).

		/** @brief Takes every live head's newest snapshot, and frees the records whose retirement a stage has passed. */
		void BeginWalk();
		/**
		 * @brief A face shape of the walk: registers its head (or rebuilds its record when the head's shapes
		 * changed) and returns the shape's positions in the head's snapshot. Empty while the head has none:
		 * the engine then draws every shape of it.
		 */
		ShapeView Shape(RE::BSDynamicTriShape& a_shape, RE::BSFaceGenNiNode& a_head);
		/** @brief Retires the records of heads the walk did not see. */
		void EndWalk();
		/** @brief Retires every record (the live toggle, teardown). */
		void RetireAll();

		struct Stats
		{
			std::uint32_t heads = 0;           // live records
			std::uint32_t acquired = 0;        // heads with a fresh snapshot this walk
			std::uint32_t withoutSnapshot = 0; // heads the walk saw with none yet
			std::uint32_t rebuilt = 0;         // records rebuilt this walk (a head's shapes changed)
			std::uint32_t retired = 0, freed = 0;
			// [TEMP] CS_DCLF_VOLUMETRIC_PROBE: shapes whose snapshot equals dynamicData at the walk, and ones that do not.
			std::uint32_t parityEqual = 0, parityDiffer = 0;
		};
		Stats TakeStats();

		/** @brief Writer-side counters since the last call (any thread): job captures, stage captures, layout mismatches. */
		struct WriterStats
		{
			std::uint32_t jobCaptures = 0, stageCaptures = 0, mismatches = 0, stages = 0;
		};
		WriterStats TakeWriterStats();

		~FaceSnapshots();

	private:
		FaceSnapshots();
		struct Impl;
		std::unique_ptr<Impl> impl;
		struct MorphHook;
		struct StageJoinHook;
	};
}
