#pragma once

#include "Features/DrawcallLimitFix/Api/FrameValues.h"
#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"
#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace DCLF
{
	struct FrameGlobals;
	struct BindlessPlacement;
	struct BindlessShading;

	/**
	 * @brief FrameValues(N) (dclf-architecture.md, "FrameValues"): the values a frame's draws need to be its own - the
	 * claimed members' placements (BindlessPlacement), the skins' palettes and the objects' shading (BindlessShading) - made on
	 * DCLF's pool while the frame runs, and waited for by the GPU, never by the render thread. The scene's tables hold none of
	 * them: these are the only ones drawn with.
	 *
	 * Each frame the render thread kicks one producer (Kick, at the scene's start) with the placement plans the walks made since the last
	 * (PlacementPlan: a delta of the placement set, made only when it changes), which the producer applies in order onto the set it keeps
	 * (its movers and roots at stable indices). The producers run one after another on the preparation pool:
	 * - they sample the kept movers and roots that moved, and the slots the walks wrote in full (once), across the pool, under
	 *   engine-read leases: a skin's palette after the engine's palette update (which the native draw's bone setter would have run),
	 *   into the rows kept by object slot and the palette rows kept by block (journalled KeptArrays); and the shading of the slots the
	 *   walk and the accumulate phase named (ShadingItem), with the shading fields' values the frame's start captured (the wetness);
	 * - what moved is what the move writers pushed (FrameValueMoves, the producer the queues' one consumer), looked up in an index from
	 *   move key to the kept movers and roots, patched as plans add, change and remove them: a mover or root is sampled when one of its
	 *   keys moved this frame or the last, when a plan added or changed it, and on the frame after a sample of it changed a row or palette
	 *   (so a skin's previous palette and a placement's previous world settle once its motion stops); a palette rows class's items every
	 *   frame, their blocks the class's registration's (Api::PaletteRowsContribution: particle quads face the camera);
	 * - send the frame's buffers the runs they lack on the dedicated upload queue, then signal the frame-wait timeline to the frame's
	 *   sequence number (always, also after a failure), which every batch of the frame waits for (PersistentGraphHost's frame wait).
	 *
	 * The features' registered pools (Api::FrameValuePool, Extensions::RegisteredPools) run beside the built-in row sets, after them: their
	 * frame-start reads after the built-in ones, their kicks after the built-in inputs, their samples after the built-in sampling, their
	 * sends after the built-in sends.
	 *
	 * Each of the row sets (the three built-in ones: placements, palettes, shading; then the registered pools', Features/Fades' seeds the
	 * last) is a pooled version (D6,
	 * org::services::BackingPool on DCLF's retirement clock, the render thread its one acquirer): Kick leases a backing holding every
	 * row, preferring the one that holds the most (its contentEpoch), and the producer sends it the journal's runs since then. The
	 * frame's draws read those backings (the frame record's t123, t122 and t121, and the culling's latches); EndFrame binds them to the
	 * frame's last submission, so no producer writes a backing while a frame reads it. No producer waits for the GPU.
	 */
	class FrameValues
	{
	public:
		/** @brief One float4 of a palette (three a bone): DCLFPalettes' element. */
		struct PaletteRow
		{
			float v[4];
		};

		static FrameValues& Get();

		/**
		 * @brief An object's placement row from the engine now: its geometry's transforms and bound, its listed root's bound
		 * (unbounded without one), its fade property's fade node (the layer property for a layer's row).
		 */
		static void SampleRow(const RE::BSGeometry& a_geometry, const RE::NiAVObject* a_listedRoot, const RE::BSShaderProperty* a_fadeProperty,
			BindlessPlacement& a_out);
		/** @brief Render thread (diagnostics): a slot's row from the engine now, by the tables' structure; false for a free slot. */
		static bool SampleSlot(const SceneTables& a_tables, std::uint32_t a_slot, BindlessPlacement& a_out);

		/**
		 * @brief Render thread, at the scene's start (the engine's update done, the window open): the frame's producer, from a_plans
		 * (the walks' plans the publication log brought since the last frame, oldest first: each applied onto the kept set in order and its
		 * written slots sampled; none: the set as it was), the slots named for their shading (a_shading, the newest last) and
		 * the shading fields' values captured (a_fieldValues: Extensions::CaptureShadingFields). Sets the host's frame wait to its sequence
		 * number. False when it cannot run (no render
		 * graph, no dedicated upload queue): the frame waits for nothing, and its draws have no rows; what it was handed waits for the
		 * first producer.
		 */
		bool Kick(std::vector<std::shared_ptr<const PlacementPlan>> a_plans, std::vector<ShadingItem> a_shading,
			std::vector<ShadingFieldValue> a_fieldValues, std::shared_ptr<const FrameGlobals> a_globals);
		/**
		 * @brief Render thread, before Kick: the claims whose CategoryRoot views' members the frame draws (null: none, those views'
		 * epochs do not draw this frame: the first person's). Kick samples those members' rows and skin palettes itself, before Main::Draw
		 * moves the first-person skeleton into its root's space (FUN_1414cb020); the producer never samples a root view's item.
		 */
		void SetRootViewClaims(std::shared_ptr<const SetSnapshot> a_claims) { rootViewClaims = std::move(a_claims); }
		/**
		 * @brief The first-person report: the palette rows classes' first-person writes since the last call that gave their placement
		 * (Api::PaletteRowsChunk::NoteFirstPerson; Features/Particles: the first-person particle members' expansions), their units (the
		 * particles), and the farthest a written placement's bound centre was from the first-person eye (a particle snapshot taken while
		 * Main::Draw had the skeleton in its root's space would be far from it).
		 */
		struct FirstPersonPaletteStats
		{
			std::uint64_t writes = 0, units = 0;
			float farthest = 0.0f;
		};
		static FirstPersonPaletteStats TakeFirstPersonPaletteStats();
		/** @brief Render thread: a frame with no producer (DCLF not running): no batch waits for one. */
		void Skip();
		/** @brief Render thread, after the frame's last submission: binds the frame's backings to it (SubmittedRetirementPoint). */
		void EndFrame();
		/** @brief Render thread: the descriptor of the placement buffer the current frame's draws read (0 before the first frame). */
		std::uint32_t PlacementsIndex() const { return frameIndex; }
		/** @brief Render thread: the descriptor of the palette buffer the current frame's draws read (0 before the first frame). */
		std::uint32_t PalettesIndex() const { return paletteIndex; }
		/** @brief Render thread: the descriptor of the shading buffer the current frame's draws read (0 before the first frame). */
		std::uint32_t ShadingIndex() const { return shadingIndex; }
		/**
		 * @brief Render thread: the descriptor of a registered pool's backing the current frame reads (StructuredBuffer of its rows; 0
		 * before the first frame, on a frame without a producer, and for an unknown pool).
		 */
		std::uint32_t PoolIndex(Api::FrameValuePoolId a_pool) const;
		/**
		 * @brief Render thread (diagnostics): whether the frame's producer is done (false before the first frame); a_wait: waits for it
		 * first. A registered pool's rows are then as it left them, until the next Kick.
		 */
		bool ProducerDone(bool a_wait = false) const;
		/**
		 * @brief Any thread (an engine writer inside the window: WindowWriters): returns once the newest kicked producer's sampling is done,
		 * its reads of engine memory (its sends may still run); at once without one.
		 */
		void WaitSampled() const;
		/** @brief The producer (a pool's sample): an engine-read lease refused, counted with the built-in parts' (Counters::refused). */
		void NoteRefused();
		bool Available() const;

		/**
		 * @brief Render thread: the rows as the frame's producer left them, once it is done (diagnostics); null while it runs (a_wait:
		 * waits for it first; null before the first frame). Valid until the next Kick.
		 */
		const std::vector<BindlessPlacement>* RowsIfDone(bool a_wait = false) const;
		/** @brief Render thread: the palette rows likewise (PaletteRowsOf places a block's). */
		const std::vector<PaletteRow>* PalettesIfDone(bool a_wait = false) const;
		/** @brief Render thread: the shading rows likewise; a_wait (diagnostics frames): waits for the frame's producer first. */
		const std::vector<BindlessShading>* ShadingIfDone(bool a_wait = false) const;
		/** @brief The producer's counters since the last take (frames 0: nothing ran, nothing else taken). */
		struct Counters
		{
			std::uint64_t frames = 0, sampled = 0, settled = 0, seeded = 0, rowsChanged = 0, skins = 0, paletteRowsChanged = 0, shadingSampled = 0, shadingChanged = 0,
						  wetnessChanged = 0, runs = 0, bytes = 0, sampleUs = 0, uploadUs = 0, maxTotalUs = 0, refused = 0, defects = 0,
						  failures = 0;
			// The moved-only sampling: the kept set's movers and roots, those due (sampled), the move keys and node events drained.
			std::uint64_t movers = 0, moversDue = 0, roots = 0, rootsDue = 0, moveKeys = 0, moveNodes = 0;
			// The placement plans applied onto the kept set, those that reset it, and their items (joined, changed and left).
			std::uint64_t plans = 0, planResets = 0, planItems = 0;
		};
		/** @brief The report's take (Reports/DrawsFrameValues.cpp): the counters, reset. */
		Counters TakeCounters();
		/** @brief One row set's pool (TakePoolStats): a built-in part's or a registered pool's. */
		struct PoolStats
		{
			const char* name = "";
			// The pool's census (BackingPool::Count): backings, leased, free, their bytes; allocated/reused/released since creation.
			std::uint64_t live = 0, leased = 0, free = 0, liveBytes = 0, allocated = 0, reused = 0, released = 0;
			// Since the last take: Kick's acquires, the backings it made new, the catch-up runs and bytes sent into the backings.
			std::uint64_t acquires = 0, made = 0, runs = 0, bytes = 0;
			// The journal's trim floor at the last Kick (the oldest content any backing holds; 0: none holds any).
			std::uint64_t floor = 0;
		};
		static constexpr std::size_t kBuiltInPools = 3;  // placements, palettes, shading
		/**
		 * @brief Any thread (the report): the pools' stats, the since-last-take counts reset: the built-in parts', then the registered
		 * pools' once the first Kick made them.
		 */
		std::vector<PoolStats> TakePoolStats();

	private:
		FrameValues();
		~FrameValues();
		struct Impl;
		std::unique_ptr<Impl> impl;
		std::uint32_t frameIndex = 0, paletteIndex = 0, shadingIndex = 0;
		std::shared_ptr<const SetSnapshot> rootViewClaims;  // SetRootViewClaims
	};
}
