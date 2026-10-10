#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace RE
{
	class NiRefObject;
	class BSDynamicTriShape;
	class BSFaceGenNiNode;
	class BSGeometry;
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
	 * Shape views retain immutable backing storage for the entire head. Returning a read slot no longer
	 * invalidates older payloads: a writer replaces the backing when that version still has a lease.
	 *
	 * Records are created and rebuilt by the scene walk alone (one thread at a time), when it writes a face shape's
	 * record, and retired when the scene store releases the head: its last face shape left the scene (membership,
	 * not "seen this walk"). The writers find them through a fixed open-addressed table keyed by the head, and a
	 * retired record is freed only once a morph stage has completed after its retirement - no job spans a stage's
	 * join.
	 *
	 * A publication is the event a head's shapes follow: BeginWalk names the heads it took a fresh snapshot of, and the
	 * heads whose writer found their shapes changed (stale), so that the scene store updates their face streams in place
	 * or writes their shapes again, without visiting them every frame.
	 */
	class FaceSnapshots
	{
	public:
		static FaceSnapshots& Get();

		/** @brief Whether the writer's thunks are installed (Install checked the engine's code first). */
		static bool Enabled();

		/** @brief Installs the two thunks in the engine's face morphing (AE only). */
		void Install();

		/** @brief A face shape's positions in the snapshot the walk holds: float4 per vertex. */
		struct ShapeView
		{
			const float* positions = nullptr;
			std::uint32_t vertexCount = 0;
			std::uint64_t generation = 0;  // unique across heads: a new snapshot of any head has a new one
			// Pins the complete head update, including every sibling shape. The writer
			// replaces a recycled slot's backing while any view still owns this version.
			std::shared_ptr<const std::vector<float>> owner;
		};
		struct HeadView
		{
			struct ShapeRange
			{
				std::uint32_t ordinal = 0;
				std::uint32_t offsetFloats = 0;
				std::uint32_t vertexCount = 0;
			};
			// A new registration/rebuild gets a fresh ID; an engine head pointer is
			// only a lookup key and cannot be used as a durable group identity.
			std::uint64_t recordId = 0;
			std::uint64_t generation = 0;
			std::vector<ShapeRange> shapes;
			std::shared_ptr<const std::vector<float>> owner;
		};

		// ---- The scene walk (one thread at a time).

		/**
		 * @brief Takes every live head's newest snapshot, and frees the records whose retirement a stage has passed. The
		 * heads it took a fresh snapshot of go to a_published, the ones a writer found stale (their shapes changed) to
		 * a_stale, each once.
		 */
		void BeginWalk(std::vector<const RE::BSFaceGenNiNode*>& a_published, std::vector<const RE::BSFaceGenNiNode*>& a_stale);
		/**
		 * @brief A face shape being written: registers its head (or rebuilds its record when the head's shapes
		 * changed) and returns the shape's positions in the head's snapshot. Empty while the head has none:
		 * the engine then draws every shape of it.
		 */
		/**
		 * @brief One of a head's shapes as the caller lists them (T6b1b: the scene work's: the mirror's children of the head, each its
		 * tracked entry's reference and its record's vertex count).
		 */
		struct ShapeInput
		{
			RE::BSDynamicTriShape* shape = nullptr;  // the caller holds a reference (the record's is a copy)
			std::uint32_t vertexCount = 0;
		};
		/** a_head: held by the caller (the record's reference is a copy). a_shapes: the head's dynamic shapes, asked for only when the record
		 * is checked (its first shape this walk). */
		ShapeView Shape(const RE::BSGeometry* a_shape, RE::BSFaceGenNiNode* a_head, const std::function<void(std::vector<ShapeInput>&)>& a_shapes);
		/**
		 * @brief The references the records freed since the last call held (their heads and shapes), one count each now the caller's: it
		 * lets them go where an engine object's last release may run (T6b1b: not the scene work).
		 */
		void TakeReleased(std::vector<RE::NiRefObject*>& a_out);
		/**
		 * @brief A registered shape's positions in its head's current snapshot, without registering anything: a publication's
		 * update of a kept face stream. Empty when the head has no record or no snapshot, or the record does not hold the shape.
		 */
		ShapeView View(const RE::BSGeometry* a_shape, const RE::BSFaceGenNiNode* a_head);
		/** @brief Complete immutable head capture of the head's current snapshot. */
		HeadView HeadSnapshot(const RE::BSFaceGenNiNode& a_head);
		/** @brief The head's last face shape left the scene: its record is retired. */
		void Release(const RE::BSFaceGenNiNode* a_head);
		/** @brief Every record is retired (the scene store forgot its entries). */
		void ReleaseAll();
		/** @brief The walk's periodic statistics. */
		void EndWalk();

		struct Stats
		{
			std::uint32_t heads = 0;           // live records
			std::uint32_t acquired = 0;        // heads with a fresh snapshot this walk
			std::uint32_t withoutSnapshot = 0; // heads the walk saw with none yet
			std::uint32_t rebuilt = 0;         // records rebuilt this walk (a head's shapes changed)
			std::uint32_t retired = 0, freed = 0;
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
