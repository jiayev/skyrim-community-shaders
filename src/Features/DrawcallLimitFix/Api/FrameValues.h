#pragma once

// The frame-value pool contract (Api::InstallContext::RegisterFrameValuePool): rows a feature samples every frame on DCLF's pool, sent
// into a pooled backing the frame's GPU work reads by descriptor. FrameValues (Core/Gpu) runs it beside its built-in parts.
//
// The palette rows contract (Api::InstallContext::RegisterPaletteRows): a class of FrameValues' items whose palette block a feature
// fills every frame in FrameValues' built-in palette part (the part the skins' bone palettes are in), and whose placement row it may
// override.

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

namespace DCLF::Api
{
	// Defined by Core (Core/Extensions/FrameValuePools.h), which the features inside DCLF include for them.
	class FrameValuePoolFrameStart;
	class FrameValuePoolKick;
	class FrameValuePoolProducer;

	/** @brief A registered pool (RegisterFrameValuePool's result): FrameValues::PoolIndex names its frame's backing by it. */
	struct FrameValuePoolId
	{
		std::uint32_t index = ~0u;
		explicit operator bool() const { return index != ~0u; }
	};

	/** @brief What a pool's kick hands its frame's producer. */
	struct FrameValueInput
	{
		// Held by the frame's job until its producer is done, then let go on DCLF's pool (no engine reference may be in it). Kick runs
		// every frame: hand over a pointer the feature already holds, not a new allocation.
		std::shared_ptr<const void> input;
		// The rows the producer may leave: the frame's backing holds at least these (its class grows by doubling, never shrinks).
		std::uint64_t rows = 0;
	};

	/**
	 * @brief A frame-value pool: its rows, journalled by the feature (KeptWholeArray<Row>), each frame sent the runs its backing lacks.
	 *
	 * - frameStart (optional): render thread, at every FrameValues::Kick (the frame's start), in registration order, with the built-in
	 *   parts' own frame-start reads, before it is known whether the frame has a producer: reads what must be the frame start's (the
	 *   engine as this frame's culls have not changed it yet) into the feature's own queue, which a later kick hands over. The feature
	 *   drains its inputs here: the frame's queued requests (the publications' deltas the frame installed) are its to take.
	 * - kick: render thread, in FrameValues::Kick (the frame's start, after the engine's update), in registration order, after the
	 *   built-in parts' inputs. Not called on a frame without a producer.
	 * - sample: the frame's producer, on DCLF's preparation pool, after the built-in parts' sampling. The producers run one at a time in
	 *   frame order, so state only they touch needs no synchronisation. Opens the rows' build at the job's floor (BeginBuild), writes and
	 *   marks them; may fan out on SceneScheduler::Executor(); reads the engine only under an EngineReadWindow::Lease while Newest().
	 * - send: the same producer, after the built-in parts' sends: hands the rows to FrameValuePoolProducer::Send.
	 *
	 * The frame's GPU work reads the backing by FrameValues::PoolIndex (render thread, from Kick until the next; 0 without a producer).
	 * The render thread's diagnostics read the rows as the frame's producer left them once FrameValues::ProducerDone says it is done
	 * (until the next Kick, which no producer passes).
	 * The backing is bound to the frame's last submission at EndFrame. A throw from sample or send fails the frame's values, as a built-in
	 * part's failure does (logged; the frame draws what its backings held).
	 */
	struct FrameValuePool
	{
		std::string_view name;    // the backing pool's name, cs.dclf.frame-values.<name>, and the report's
		std::uint32_t stride = 0;  // bytes a row
		void (*frameStart)(const FrameValuePoolFrameStart& a_start) = nullptr;
		FrameValueInput (*kick)(const FrameValuePoolKick& a_kick) = nullptr;
		void (*sample)(FrameValuePoolProducer& a_producer) = nullptr;
		void (*send)(FrameValuePoolProducer& a_producer) = nullptr;
	};

	// Defined by Core (Core/Extensions/PaletteRows.h), which the features inside DCLF include for them.
	struct PaletteGeometrySource;
	class PaletteRowsKick;
	class PaletteRowsRun;
	class PaletteRowsChunk;

	/**
	 * @brief A class of FrameValues' items (PlacementPlan::Item::itemClass), Core's to decide: a geometry type contribution names the class
	 * of its skinless objects (Api::GeometryTypeContribution::itemClass, read as data), the coordinator names an item's class when it plans
	 * it, the walk sizes its palette block by the class's rule, and the class's registration fills the block.
	 * - ParticleSystem: a particle system without a skin (BSGeometry type kParticles), its block its quads (Features/Particles).
	 */
	enum class FrameValueItemClass : std::uint8_t
	{
		None,
		ParticleSystem,
		Count,
	};
	inline constexpr std::size_t kPaletteRowsClasses = static_cast<std::size_t>(FrameValueItemClass::Count) - 1;  // None has none

	/** @brief The bytes of a palette rows class's frame input (PaletteRowsKick::input): trivially copyable, held in the frame's job. */
	inline constexpr std::size_t kPaletteRowsInputBytes = 128;

	/**
	 * @brief A palette rows class: the items of itemClass with a palette block (the walk's, sized by rule) are due every frame, their keys
	 * unread (FrameValues samples them whatever moved), and their block's rows are the feature's. Read as data where the walk sizes a block;
	 * its callbacks run at the frame's start, at FrameValues' kick, once a producer run and once a chunk of its items, never per object
	 * (geometrySource: per record write of the class's geometry).
	 *
	 * - name: the report's and the install's.
	 * - itemClass: the class (one registration's).
	 * - rule: a block's rows from the geometry's unit count (SceneCapture's record: a particle system's capacity): the units clamped to
	 *   [minUnits, maxUnits], rounded up to a power of two when pow2 (maxUnits then one), times rowsPerUnit. The walk reads it as data.
	 * - geometrySource: the scene lane (the walk's record writes): the geometry slot the class's draws read, for a_source.units units
	 *   (the rule's); false: none yet (the record waits, Tables::kSlotFree).
	 * - frameStart (optional): render thread, FrameDriver::BeginFrame, every frame (after the primary cull's frame camera): the engine's
	 *   frame-start state the class reads.
	 * - kick (optional): render thread, FrameValues::Kick (the frame's start, after the engine's update), on a frame with a producer: the
	 *   frame's input into PaletteRowsKick::input (kPaletteRowsInputBytes, held by the frame's job; all zero without a kick).
	 * - beginRun: the frame's producer (DCLF's pool, one producer at a time), before its items are sampled: every item of the class with a
	 *   block listed this run, in the run's order (PaletteRowsRun::items); sets each one's state.
	 * - sampleChunk: the producer's pool, once a chunk of the run's items (CS.DCLF.FrameValues.Items, concurrently with the other chunks;
	 *   a chunk's items are distinct): each listed item of the class whose sample was not refused, after Core sampled its placement row:
	 *   writes every row of its block, and may give its placement row's world (its previous too) and bound. Core then compares them with
	 *   the frame's rows (the block's current rows only) and marks what changed. No engine read (no lease is held).
	 * - endRun: the producer, after every chunk.
	 */
	struct PaletteRowsContribution
	{
		struct BlockRule
		{
			std::uint32_t rowsPerUnit = 0, minUnits = 0, maxUnits = 0;
			bool pow2 = false;

			/** @brief The units a geometry of a_count is given a block for. */
			constexpr std::uint32_t Units(std::uint32_t a_count) const
			{
				const std::uint32_t units = std::clamp(a_count, minUnits, maxUnits);
				return pow2 ? std::bit_ceil(units) : units;
			}
			/** @brief Its block's rows in the frame's palettes. */
			constexpr std::uint32_t Rows(std::uint32_t a_count) const { return Units(a_count) * rowsPerUnit; }
		};

		std::string_view name;
		FrameValueItemClass itemClass = FrameValueItemClass::Count;
		BlockRule rule;
		bool (*geometrySource)(PaletteGeometrySource& a_source) = nullptr;
		void (*frameStart)() = nullptr;
		void (*kick)(const PaletteRowsKick& a_kick) = nullptr;
		void (*beginRun)(const PaletteRowsRun& a_run) = nullptr;
		void (*sampleChunk)(const PaletteRowsChunk& a_chunk) = nullptr;
		void (*endRun)(const PaletteRowsRun& a_run) = nullptr;
	};
}
