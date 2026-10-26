#pragma once

// The CPU-driven pipeline (an experiment beside the GPU one; dclf-architecture.md, "The CPU-driven pipeline"): the scene's
// per-view lists as fixed-size SoA batches made with each snapshot, culled on the CPU per frame or once per snapshot, and drawn
// with direct draws recorded into DXVK's own command buffer. This header is the contract its parts build against: the batches
// (Snapshot/ViewBatches.cpp), the cull (Cull.cpp), the recorder (Recorder.cpp) and the sites (Sites.cpp).
//
// It suspends two standing policies while dclf:pipeline=cpu, and only then: "DCLF owns all culling, on the GPU" and "No per-view
// lists". The GPU path, the set, the claims and withholding are unchanged.

#include "Features/DrawcallLimitFix/Core/Runtime/EventQueue.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace rhi
{
	class CommandList;
}

namespace org::services
{
	struct PublishedBufferVersion;
}

namespace DCLF::Draws
{
	struct GpuCascade;
	struct GpuShadowVolume;
	struct SunEntryProcess;
}

namespace DCLF::Direct
{
	struct StrictGates;  // Cull.h

	/**
	 * @brief The lanes of a batch by default (the live setting: Draws::BatchLanes; always a multiple of 8, for the SIMD tests). 4096: in
	 * heavy scenes it halves Parallel mode's cost against 256 (fewer secondaries) at the same Serial cost; past it a list is too few
	 * jobs, and DXVK's CS thread waits for them (2026-10-25, 1.6.1170 heavy exterior).
	 */
	inline constexpr std::uint32_t kDefaultBatchLanes = 4096;

	/** @brief The view types with a list of their own. Main's list serves both main segments: with no HZB the colour pass draws exactly
	 * the depth pass's set (EQUAL). */
	enum class ViewType : std::uint8_t
	{
		Main,
		Decal,
		Forward,
		Refraction,
		Sun,
		Spot,
		Point,
		SkyOccluder,
		PrecipOccluder,
		Reflection,
		FirstPerson,
		kCount
	};
	inline constexpr std::size_t kViewTypes = static_cast<std::size_t>(ViewType::kCount);

	/** @brief When a view type is culled and recorded: every frame, or once per snapshot (exactly as last captured; pops accepted).
	 * Main is always PerFrame. */
	enum class Cadence : std::uint8_t
	{
		PerFrame,
		PerSnapshot
	};

	/**
	 * @brief The CPU pipeline's performance changes under A/B (test command dclf:cpu-exp=<bits>): each on by default; a bit cleared restores
	 * the behaviour before it, for a comparison within one launch.
	 */
	enum Experiment : std::uint32_t
	{
		kExpCullEarly = 1u << 0,  // CullBatch: a lane outside the frustum dropped before its other tests (gathering its inputs)
		kExpPortalEarly = 1u << 1,  // Features/Portals' CPU plane set test stopped at the plane that decides it
		kExpParabolicPass = 1u << 3,  // CullBatch: a paraboloid view's sphere test over the lanes, four at a time, before the lanes' own tests
		kExpPrefillLive = 1u << 4,  // MakeViewBatches fills both live columns (the producer's first fill a set and parity skipped)
		kExpPortalBatch = 1u << 2,  // CullBatch: the main camera lists' portal tests made over the batch's lanes, four at a time (portalTestBatch)
		kExpIncremental = 1u << 5,  // the view batch sets from the incremental scene lists (Snapshot/SceneLists.cpp), not a full build each
		// CullBatch's frustum pass in the widest kernel the CPU has (CullSimd.h: AVX2's 8 lanes, or AVX-512's 16 with kExpCullAvx512), a group
		// of lanes stopping at the plane all are outside of, with the tombstones folded in; off, the SSE pass of four.
		kExpCullWide = 1u << 6,
		kExpCullAvx512 = 1u << 7,  // kExpCullWide may take AVX-512 (else AVX2 at most)
		// CullBatch's loop over the lanes the pass kept alone (with kExpCullEarly), sixteen bytes of the pass at a time; off, every lane read.
		kExpCullPassedLanes = 1u << 8,
		// Features/Portals' batch test walks the program once a call, each operator over every 16-bound chunk routed to it (a group of any
		// size visits nearly every operator); off, a walk per group of 16 bounds.
		kExpPortalOpMajor = 1u << 9,
		// Not a change: kExpCullProfile's per-lane sections (gather, keep, LOD, word), a TSC read each; without it the loop is one span a
		// batch ("lanes"), the reads' own cost otherwise a large part of what they time. Off by default.
		kExpCullProfileLanes = 1u << 27,
		// Not a change: kExpCullWide's verdicts (the frustum's against the SSE pass, the paraboloid's against ParabolicRejected) checked
		// (mismatches on the [CPU] line). Off by default.
		kExpCullWideCheck = 1u << 28,
		// Not a change: every other incremental set checked against a full build of its snapshot (Snapshot/SceneLists.cpp, CompareSets).
		kExpIncrementalCheck = 1u << 29,
		// Not a change: kExpPortalBatch's verdicts checked against the per-lane test (mismatches on the [CPU] line). Off by default.
		kExpPortalCheck = 1u << 30,
		// Not a change: CullBatch's sections timed by view (TSC cycles), reported on the [CPU] line (Direct::TakeCullProfileLine). Off by default.
		kExpCullProfile = 1u << 31,
	};
	inline std::atomic<std::uint32_t> experimentBits{ ~(kExpCullProfile | kExpPortalCheck | kExpIncrementalCheck | kExpCullWideCheck | kExpCullProfileLanes) };  // constant-initialised: no guard on the hot paths that read it
	inline std::atomic<std::uint32_t>& ExperimentBits() { return experimentBits; }
	inline bool ExperimentOn(std::uint32_t a_bit) { return (experimentBits.load(std::memory_order_relaxed) & a_bit) != 0; }

	/** @brief How per-frame views are recorded (dclf:cpu-record). */
	enum class RecordMode : std::uint8_t
	{
		// Culled in parallel; DXVK's CS thread drains the published CulledBatches inside the site's one callback and records them
		// into DXVK's command buffer in list order.
		Serial,
		// Culled and recorded in parallel, a secondary command list per batch; the site's callback executes them in list order.
		Parallel
	};

	/** @brief Per-lane flags (ViewBatch::flags). */
	enum LaneFlags : std::uint32_t
	{
		kLaneFadeTest = 1u << 0,    // the fade distance test (kObjectFadeTest)
		kLaneHeightTest = 1u << 1,  // the tree height test (kObjectHeightTest)
		kLaneSunTest = 1u << 2,     // sun-miss bit computed per draw (kObjectSunTest)
		kLaneMover = 1u << 3,       // its sphere is resampled per frame (the live column)
		kLaneForward = 1u << 4,
		kLaneDecal = 1u << 5,
		kLaneVolumetricOnly = 1u << 6,  // a shadow caster drawn only by the volumetric copy (kObjectVolumetricOnly)
		kLaneLandscapeLights = 1u << 7,
		kLaneCrossfadeCopy = 1u << 8,  // a lane that draws the cross-fade copy's partitions
		// Main's terrain groups (ObjectTerrainGroup 1, 2: Terrain Blending's held groups): drawn in depth with the rest, in colour by their
		// own range after the decals (their main range slot's), not by the main colour pass.
		kLaneTerrainGroup1 = 1u << 9,
		kLaneTerrainGroup2 = 1u << 10,
		// Main's cull-only candidates (no bindings, or outside the set): culled for their verdicts (kAccumulated), no sequences (sequenceCount 0),
		// after every drawn lane of the list.
		kLaneCullOnly = 1u << 11,
	};

	/**
	 * @brief One fixed-size batch of a view type's list, SoA. Made by the snapshot builder (MakeViewBatches) and immutable after,
	 * except liveSpheres, which FrameValues' producer writes for movers (one column per frame parity).
	 *
	 * Lanes are ordered by pipeline bucket, then object, so a recorder binds a pipeline only when it changes; a decal list's by
	 * group then range ordinal. count <= lanes; lanes past count are never read.
	 */
	struct ViewBatch
	{
		std::uint32_t count = 0;
		std::uint32_t index = 0;  // in its list (the reorder window's key)
		// Per lane:
		std::uint32_t* object = nullptr;          // the object index (the object word's bits 0-25)
		std::uint32_t* sequence = nullptr;        // the first of its resident sequences (ViewBatchSet::sequences)
		std::uint16_t* sequenceCount = nullptr;   // its partitions' sequences, consecutive from sequence
		std::uint32_t* pipelineBucket = nullptr;  // the view's pipeline bucket (BucketsOf)
		std::uint32_t* flags = nullptr;           // LaneFlags
		std::uint32_t* fadeRoot = nullptr;        // 0 without one, else root + 1
		float* fadeDistance = nullptr;
		// The input's partition masks (DrawInput::partitions, lodPartitions: byte L = level L's mask, 0 not a LOD skin). A lane with
		// lodPartitions has one sequence per set bit of its four bytes OR'd, in bit order (sequence + k = the k-th set bit); the cull picks
		// the level's (CulledBatch::drawMasks). Without, every sequence of the lane is drawn.
		std::uint32_t* partitions = nullptr;
		std::uint32_t* lodPartitions = nullptr;
		// The snapshot's spheres (placement row 6 when the set was made): per-snapshot culls and static lanes read these.
		float *cx = nullptr, *cy = nullptr, *cz = nullptr, *r = nullptr;
		// The live spheres, by frame parity: static lanes copied once from the snapshot's, movers' written by FrameValues' producer
		// before it signals done. Per-frame culls read the column of the frame's parity.
		struct Spheres
		{
			float *cx = nullptr, *cy = nullptr, *cz = nullptr, *r = nullptr;
		};
		std::array<Spheres, 2> liveSpheres{};
		// The incremental lists' tombstones (Snapshot/SceneLists.cpp): a base batch shared by several sets has a bit per lane set where
		// this set's lane is gone (removed or replaced since the base); the cull drops those. Null: every lane is live.
		const std::uint64_t* dead = nullptr;
	};

	/** @brief Where a mover's lanes are, for FrameValues' producer (ViewBatchSet::moverLanes). */
	struct LaneRef
	{
		std::uint16_t view;   // ViewType
		std::uint32_t batch;  // in the view's list
		std::uint16_t lane;
	};

	/**
	 * @brief A view type's pipeline buckets: the pulled pipeline each lane's bucket binds, and its layout. By view type, a bucket's
	 * index is a main set index (DrawInput::pipelineIndex: Main, Decal, Forward, Reflection, FirstPerson; the recorder binds its
	 * IndirectState::zPipelines[zGroups[i]] in a depth pass, plainDepth[i] in the decal depth pass, plainColour[i] otherwise), a shadow
	 * key slot (Sun, Spot, Point, SkyOccluder, PrecipOccluder: DrawInput's shadow or occlusion key, Lookups::shadowSlots; the view's map
	 * row, Lookups::ShadowMapRow(mode, state), gives its ShadowIndirectState::pipelines index, as BuildDrawsCS's DrawPipeline), or unread
	 * (Refraction: one pipeline, bucket 0).
	 */
	struct ViewBuckets
	{
		std::vector<std::uint32_t> pipeline;  // per bucket: the pipeline handle's index in the frame's pipeline catalog
		// Per bucket, the main family's views (not the shadow views'): a pipeline slot of the tables its lanes draw (SceneTables::pipelines;
		// every slot of a set index has its key), for the views whose pipeline is the slot's own (Reflection's forward pipeline,
		// ViewForwardKey); ~0u for the shadow views'.
		std::vector<std::uint32_t> pipelineSlot;
	};

	/**
	 * @brief A resident sequence's draw arguments, the CPU copy the recorder draws from: its index count, its geometry's own first index
	 * and its geometry slot, whose index pool first (IndexPool::slotFirst, the frame's copy) the recorder adds at record time (the pool
	 * reassigns ranges at the frame's first drawing commit, after the set was made). A slot without a range (kNoRange) draws nothing.
	 */
	struct DrawArgs
	{
		std::uint32_t indexCount = 0;
		std::uint32_t firstIndex = 0;
		std::uint32_t geometrySlot = 0;
	};

	/**
	 * @brief The incremental lists' persistent sequences' arguments (Snapshot/SceneLists.cpp), shared by the sets of one base: fixed
	 * chunks in a fixed directory, appended past every posted set's count and never moved, so a set reads its prefix while the builder
	 * appends the next set's.
	 */
	struct ArgsChunks
	{
		static constexpr std::uint32_t kShift = 12, kChunk = 1u << kShift, kChunks = 4096;  // 16M sequences
		std::unique_ptr<std::unique_ptr<DrawArgs[]>[]> chunks = std::make_unique<std::unique_ptr<DrawArgs[]>[]>(kChunks);
		const DrawArgs& At(std::uint32_t a_sequence) const { return chunks[a_sequence >> kShift][a_sequence & (kChunk - 1)]; }
	};

	/**
	 * @brief One snapshot's per-view lists (MakeViewBatches): immutable once posted, retired with its snapshot. Every batch's
	 * columns live in `storage`, one allocation, laid out by the builder; nothing is copied after.
	 */
	struct ViewBatchSet
	{
		std::uint64_t serial = 0;  // the snapshot's
		std::uint32_t lanes = kDefaultBatchLanes;
		std::array<std::vector<ViewBatch>, kViewTypes> lists;
		std::array<ViewBuckets, kViewTypes> buckets;
		// The resident sequences: one fully resolved 136 B DrawSequence per drawn (object, partition, view class), in a pooled
		// version of their own (the payload's publication rules; append-only). The per-frame bits of the object word (sun miss,
		// local shadow mask, cross-fade copy) are not in them: the recorder pushes the word per draw.
		std::uint64_t sequencesAddress = 0;
		std::uint32_t sequenceCount = 0;
		// The sequences' rows are offsets from these (kDirectFlagRelativeRows): the payload's row parts' versions the set was made for
		// (held by `keep`), pushed once per list by the recorder (main lists: pipeline and material rows; shadow lists: the shadow rows).
		std::uint64_t pipelineRowsBase = 0, materialRowsBase = 0, shadowRowsBase = 0;
		// By resident sequence: its draw arguments (the recorder's DrawIndexed; the GPU copy's are the same, without the pool's first):
		// the first persistentSequences in persistentArgs (the incremental lists'), then sequenceArgs (the set's own: every sequence of a
		// full build). Read through Args.
		std::shared_ptr<const ArgsChunks> persistentArgs;
		std::uint32_t persistentSequences = 0;
		std::vector<DrawArgs> sequenceArgs;
		/** @brief A resident sequence's arguments (null: past the set's). */
		const DrawArgs* Args(std::uint32_t a_sequence) const
		{
			if (a_sequence < persistentSequences)
				return &persistentArgs->At(a_sequence);
			a_sequence -= persistentSequences;
			return a_sequence < sequenceArgs.size() ? &sequenceArgs[a_sequence] : nullptr;
		}
		// The decal list's second pass groups: group g (its main range slot, Extensions::MainRangeEntry::slot) is the decal list's lanes
		// [decalGroupFirst[g], decalGroupFirst[g + 1]), counted across its batches (batch * lanes + lane). Empty: no decals.
		std::vector<std::uint32_t> decalGroupFirst;
		// The index pool's serial its sequences' first indices were laid out against: a per-snapshot recording of another is stale.
		std::uint64_t indexPoolSerial = 0;
		// By geometry slot: the index buffer its sequences were resolved against (0: none). A frame draws a slot's sequences only while
		// the pool's range for it is of that buffer (TakePoolFirsts); held by `keep`, so no other buffer can take its address meanwhile.
		std::vector<std::uint64_t> geometryIndexAddress;
		// object -> [moverLaneFirst[object], moverLaneFirst[object + 1]) in moverLanes; only movers have lanes there.
		std::vector<std::uint32_t> moverLaneFirst;
		std::vector<LaneRef> moverLanes;
		std::shared_ptr<std::byte[]> storage;  // a recycled block (ViewBatches.cpp's storage pool): returned when the set goes
		// The resident sequences' version (sequencesAddress is its backing's): bound to the retirement point of every frame that draws
		// with the set (Draws::BindViewBatches, beside PayloadPools::BindInstalled), so its backing is not reused while one reads it.
		std::shared_ptr<const org::services::PublishedBufferVersion> sequencesVersion;
		// What the sequences' addresses are into: the snapshot's draws and payload (its row parts' versions), held for as long as the set.
		std::shared_ptr<const void> keep;
		// The incremental lists' shared parts the set's batches and sequences are in (their base's storage, the persistent arguments).
		std::shared_ptr<const void> shared;
		// The set's dead bitmaps (ViewBatch::dead), its own.
		std::vector<std::uint64_t> deadBits;
		// Mutable: the live columns' static lanes are filled once, by the first frame of each parity that uses the set (MakeViewBatches
		// fills both and sets both).
		mutable std::array<std::atomic<bool>, 2> liveFilled{};

		const std::vector<ViewBatch>& List(ViewType a_view) const { return lists[static_cast<std::size_t>(a_view)]; }
	};

	/** @brief The live sphere column a frame reads and FrameValues' producer of that frame writes: its producer's sequence's parity. */
	inline std::size_t LiveParity(std::uint64_t a_frameValuesSeq) { return static_cast<std::size_t>(a_frameValuesSeq & 1u); }

	/**
	 * @brief The placement bounds (row 6) as a FrameValues producer left them, for the snapshot builder (FrameValues::RequestSnapshotSpheres,
	 * TakeSnapshotSpheres): the builder never reads the producer's rows. By object slot: the sphere, the geometry the producer sampled the
	 * slot's row for (raw, never dereferenced: compared with SceneTables::objectGeometry), and whether it moves (a kept mover, a settling
	 * slot, or a sphere that changed in the last two producers). A slot whose owner is another geometry, or that moves, has no snapshot
	 * sphere: its lanes are movers (kLaneMover), their snapshot radius -1 (unknown: a per-snapshot cull keeps it).
	 */
	struct SnapshotSpheres
	{
		std::uint64_t seq = 0;  // the producer's
		std::vector<std::array<float, 4>> sphere;
		std::vector<const void*> owner;
		std::vector<std::uint8_t> moving;
	};

	/** @brief One view's culling inputs for a frame (or a snapshot): what BuildDrawsLatch carries, on the CPU. */
	struct CullView
	{
		ViewType type = ViewType::Main;
		std::uint32_t viewIndex = 0;  // the instance (a shadow slot, a reflection face)
		std::array<float, 16> viewProj{};
		std::array<std::array<float, 4>, 6> planes{};  // world space, inward
		std::uint32_t planeMask = 0;
		std::uint32_t cullFlags = 0;  // the kCull* bits (GpuLayouts.h)
		std::array<float, 3> eye{};
		float fadeEyeDistanceScale = 1.0f;
		float treeHeight = 0.0f;
		std::uint32_t focusRoot = 0;  // 0 none, else root + 1
		std::uint32_t frame = 0;      // the frustum stamp's
		bool frustumCull = true;      // false: first person draws every member
		// Added for Cull.cpp (BuildDrawsLatch's other words; Cull.h's builders fill them from the latch):
		// The view-projection's six clip planes in world space (normalised, inward: -x, +x, -y, +y, near, far), tested as spheres.
		std::array<std::array<float, 4>, 6> clipPlanes{};
		float treeHeightBase = 0.0f;  // BSTreeNode::OnVisible's height test: base (treeHeight is its limit; +infinity off)
		float minCasterRadius = 0.0f;  // kCullMinTexels
		std::uint32_t strictContext = 0;  // a portal-strict point light's atlas entry + 1 (0: not gated)
		bool sunTest = false;  // the colour pass's sun test (kSunTestOn)
		// The main camera's lists (Main, Decal, Forward, Refraction) culled only for their verdicts (kAccumulated): a per-snapshot list's
		// per-frame stamp pass. Nothing is kept.
		bool stampOnly = false;
		// Latch-block regions, as spans the caller holds for the cull's duration: the colour pass's cascades and local shadow volumes,
		// a sun view's full-frustum processes.
		const Draws::GpuCascade* cascades = nullptr;
		std::uint32_t cascadeCount = 0;
		const Draws::GpuShadowVolume* shadowVolumes = nullptr;
		std::uint32_t shadowVolumeCount = 0;
		const Draws::SunEntryProcess* sunEntry = nullptr;
		std::uint32_t sunEntryCount = 0;
		// A strict light's gates (Cull.h, StrictGates; null: nothing gated).
		const StrictGates* strict = nullptr;
	};

	/**
	 * @brief A batch's culled draws for one view (Cull.cpp): the lanes kept and each draw's pushed object word. Lives in a
	 * per-frame-slot arena owned by the job that culled it, never copied: consumers get a pointer (the site's queue) and the arena
	 * is reset at the frame slot's retirement. In Parallel mode `commands` is the secondary list it was recorded into.
	 */
	struct CulledBatch
	{
		const ViewBatch* batch = nullptr;
		std::uint32_t view = 0;  // CullView::viewIndex
		std::uint32_t count = 0;
		const std::uint16_t* lanes = nullptr;
		const std::uint32_t* objectWords = nullptr;  // the full word: index, local shadow mask, cross-fade copy, sun miss
		// Per kept lane (null: every sequence, none as the copy): bits 0-15 the lane's sequences drawn (bit s: its sequence s), bits 16-31
		// those of them drawn as the LOD cross-fade copy (their object word with kObjectCrossfadeCopy, bit 30).
		const std::uint32_t* drawMasks = nullptr;
		rhi::CommandList* commands = nullptr;        // Parallel mode's secondary (null in Serial mode)
		bool last = false;                           // the view's list ends with it
	};

	/** @brief The CS thread's queue of a site's culled batches (MPSC: the cull jobs push, the site's callback drains). */
	using CulledQueue = EventQueue<const CulledBatch*, 1024>;

	/** @brief The draw sites: one per draw epoch it replaces, at the same trigger. */
	enum class Site : std::uint8_t
	{
		Shadow,       // shadow-maps.after: every shadow view and the occlusion maps
		Depth,        // depth.committed: main depth, the reflection's faces, first person's depth
		Colour,       // opaque.after: main colour and decals
		Forward,      // blended.draw: forward and refraction
		FirstPerson,  // fp.colour-render
		kCount
	};

	/** @brief The live state (Core/Settings/Toggles: dclf:pipeline, dclf:cpu-record, dclf:cpu-cadence-<view>). */
	struct Mode
	{
		bool cpu = false;
		RecordMode record = RecordMode::Serial;
		std::array<Cadence, kViewTypes> cadence{};  // Main's is always PerFrame

		bool operator==(const Mode&) const = default;
	};

	/** @brief One frame's report counters (the [CPU] line, Diagnostics/Reports). */
	struct FrameCounters
	{
		std::array<std::uint32_t, kViewTypes> batches{}, drawn{};
		std::array<double, kViewTypes> cullUs{};
		std::array<double, static_cast<std::size_t>(Site::kCount)> recordUs{};  // on DXVK's CS thread
		std::uint32_t secondaries = 0;
		std::uint32_t waits = 0;  // a site's callback waiting for a batch not yet published
		// Per-snapshot views (Cadence::PerSnapshot): recordings made (a view's first frame of a set, or its index ranges moved), and
		// executed in place of a per-frame cull.
		std::uint32_t snapshotRecorded = 0, snapshotExecuted = 0;
		// Geometry slots the set drew whose pool range is another buffer's this frame (TakePoolFirsts): their draws skipped.
		std::uint32_t staleGeometry = 0;
		// The recordings' calls (RecordContext's): pipeline binds, those binding the pipeline already bound, per-draw pushes.
		std::uint32_t binds = 0, sameBinds = 0, pushes = 0;
	};
}
