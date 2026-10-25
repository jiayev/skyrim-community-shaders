#pragma once

// The CPU-driven pipeline's cull (Direct.h; dclf-architecture.md, "The CPU-driven pipeline"): BuildDrawsCS's per-input decisions on the
// CPU over a ViewBatch's SoA lanes (CullBatch), the frame's CPU state they read (CullFrame: the CPU fade stage's roots, the portal
// programs, the placements' sun entry and fade node rows), the views as the GPU latches carry them (the *CullView builders), and the
// main camera's verdicts kept for kAccumulated (diffed into Visibility.cpp's decoder once the frame's main-camera lists are culled).
//
// No HZB: CPU mode has no occlusion. The frustum is a sphere against the view-projection's world planes, where BuildDrawsCS projects the
// sphere's world AABB corner by corner: the sphere test rejects at least what the corners do (the AABB holds the sphere), so the CPU keeps
// a subset of the GPU's set, never a draw the GPU would not make. Accepted difference.
//
// Threads: CullBatch on any pool thread (a batch's lanes are its job's; the frustum stamps are per object, written by the lane's owner with
// relaxed atomics); the CullFrame is immutable once published; KickCpuFrameState on the render thread.

#include "Features/DrawcallLimitFix/Core/Render/Direct/Direct.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Records.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <vector>

namespace DCLF
{
	struct BindlessPlacement;
	struct EntrySpace;
	struct SceneTables;
}

namespace DCLF::Draws
{
	struct BuildDrawsLatch;
	struct SceneBuffers;
}

namespace DCLF::Direct
{
	/**
	 * @brief A cull job's output memory for one frame slot: bump allocation in chunks (grown by a chunk when one is full, never moved, so
	 * a CulledBatch handed out stays put), Reset at the slot's retirement. One thread at a time (its owner: a worker's, per frame slot).
	 */
	class CullArena
	{
	public:
		CullArena() = default;
		CullArena(const CullArena&) = delete;
		CullArena& operator=(const CullArena&) = delete;
		CullArena(CullArena&&) = default;
		CullArena& operator=(CullArena&&) = default;

		/** @brief a_count T, uninitialised (trivial types only), aligned for T. */
		template <class T>
		T* Make(std::size_t a_count)
		{
			return static_cast<T*>(Allocate(a_count * sizeof(T), alignof(T)));
		}
		void* Allocate(std::size_t a_bytes, std::size_t a_align);
		/** @brief Every allocation let go (the frame slot retired); the chunks are kept for the next frame of the slot. */
		void Reset();
		std::size_t BytesUsed() const;

	private:
		static constexpr std::size_t kChunkBytes = 256 * 1024;
		struct Chunk
		{
			std::unique_ptr<std::byte[]> bytes;
			std::size_t size = 0;
		};
		std::vector<Chunk> chunks;
		std::size_t current = 0, used = 0;
	};

	/**
	 * @brief A portal-strict point light's gates on the CPU (PortalPrograms.hlsli's StrictGateRejects, over the words its GPU buffers hold:
	 * Portals/StrictGates.h's verdicts, the frame's light context atlas and the object gates' column). rejects is the evaluator's
	 * (Features/Portals' PortalProgramsCpu: Portals::StrictGatesOf fills it); hold keeps the words alive for the cull.
	 */
	struct StrictGates
	{
		std::span<const std::uint32_t> gates, atlas, objectGates;
		bool (*rejects)(const StrictGates& a_gates, std::uint32_t a_context, std::uint32_t a_object, const float a_centre[3], float a_radius) = nullptr;
		std::shared_ptr<const void> hold;
	};

	/**
	 * @brief The frame's strict gates (Features/LocalLights, the GPU's StrictGates, LightContexts and ObjectGates bindings' words): a_scene's
	 * for scene frame a_frame, render thread, at a site's commit; rejects null when no portal-strict context is published. Registered once
	 * at install (SetStrictGatesSource); null: none, nothing gated.
	 */
	using StrictGatesSource = StrictGates (*)(const Draws::SceneBuffers& a_scene, std::uint32_t a_frame);
	void SetStrictGatesSource(StrictGatesSource a_source);
	/** @brief Render thread: the frame's strict gates from the registered source (rejects null: none), held by the caller for its culls. */
	std::shared_ptr<const StrictGates> FrameStrictGates(const Draws::SceneBuffers& a_scene, std::uint32_t a_frame);

	/** @brief PortalPrograms.hlsli's PortalTest on the CPU (Features/Portals' evaluator): 1 in view, 0 culled, -1 no program. */
	using PortalTestFn = int (*)(std::span<const std::uint32_t> a_programs, std::uint32_t a_program, const float a_centre[3], float a_radius, std::uint32_t a_nodeFlags);

	/** @brief CullFrame::rootWords: a root slot's verdicts as the frame's cull reads them, from the CPU fade stage. */
	enum RootWord : std::uint32_t
	{
		kRootListed = 1u << 0,               // a listed root of a seeded generation
		kRootFading = 1u << 1,               // BuildDrawsCS's RootFading: not fully faded in (shadow views drop its casters)
		kRootFadedOutOfOcclusion = 1u << 2,  // FadedOutOfOcclusion (the occlusion maps' kCullFadeOnVisible)
		kRootServiced = 1u << 3,             // this frame's OnVisible ran (kFadeVerdictServiced)
		kRootDrawn = 1u << 4,                // ... and went on into the children (kFadeVerdictDrawn)
		kRootInView = 1u << 5,               // the root's own cull test passed (kFadeVerdictInView): its kAccumulated
		kRootUpdated = 1u << 6,              // the stage updated its listing this frame (a verdict exists)
		kRootLodCopy = 1u << 7,              // crossing levels with int(blend * 31) > 0: main views draw row L+1 less row L as the copy
		kRootLodShift = 8,                   // bits 8-9: the LOD level (min(+0x152 & 0xF, 3)); 0 without state
	};

	/**
	 * @brief The frame's CPU state every cull of the frame reads (published once a frame by the CPU fade stage, Features/Fades' FadeCpuStage,
	 * from FrameValues' producer once the placements are sampled): immutable once published; jobs hold it (shared) for as long as they cull.
	 */
	struct CullFrame
	{
		std::uint32_t sceneFrame = 0;
		std::uint32_t stamp = 0;  // the frustum stamp: sceneFrame's low 28 bits
		std::uint64_t frameValuesSeq = 0;
		std::shared_ptr<const ViewBatchSet> set;  // the batches KickCpuFrameState armed the frame with
		// By object slot, the placements' rows as the frame's producer sampled them: the sun entry sphere (row 7) and the fade node (row 8).
		std::span<const std::array<float, 4>> sunEntry, fadeNode;
		// By root slot: the verdicts (RootWord), the states the readers take (each root's this-frame state, else its seed's, else its row's:
		// BuildDrawsCS's SeedState rule), and each root's portal program (kPortalNoProgram: the frustum alone).
		std::span<const std::uint32_t> rootWords;
		std::span<const FadeNodeState> rootStates;
		std::span<const std::uint32_t> rootGenerations;
		std::span<const std::uint32_t> rootPrograms;
		// The main camera's programs (PortalPrograms.h's encoding; empty: none this frame, the frustum alone) and their evaluator.
		std::span<const std::uint32_t> programs;
		PortalTestFn portalTest = nullptr;
		// The main camera as the fade test measures from it (FadeState::FadeEye: position, LOD factor) and the tree height test (base,
		// limit): the main camera's lists' stamps read these whichever view culls them (the depth latch's fadeEye and treeHeight).
		std::array<float, 4> fadeEye{};
		std::array<float, 2> treeHeight{};
		// The tables and plain entries the frame's visibility batch is judged against (Readback::Batch::tables, candidates).
		std::shared_ptr<const SceneTables> tables;
		std::shared_ptr<const EntrySpace> candidates;
		bool visibilityReset = false;  // CpuFrameArm::visibilityReset
		std::shared_ptr<const void> storage;  // owns every span above
		// The main camera's batches (Main, Decal, Forward, Refraction) still to cull this frame, plus the publication's one: the last one to
		// finish diffs the frame's verdicts into the visibility decoder (Cull.cpp).
		mutable std::atomic<std::uint32_t> pendingStamps{ 0 };
	};

	// ---- The frame state ----

	/**
	 * @brief Render thread, FrameDriver::BeginFrame, before FrameValues::Kick (never blocks): arms the frame's CPU state with the batches its
	 * culls read. FrameValues' producer then runs the CPU fade stage right after it samples the placements (the "fade-states-cpu" pool,
	 * Features/Fades), which publishes the frame's CullFrame (AfterCullFrame). A frame not armed has none (GPU mode).
	 */
	void KickCpuFrameState(std::shared_ptr<const ViewBatchSet> a_set);
	/** @brief What KickCpuFrameState armed, taken by the CPU fade stage's kick (render thread, in FrameValues::Kick); null set: not armed. */
	struct CpuFrameArm
	{
		std::shared_ptr<const ViewBatchSet> set;
		std::uint32_t sceneFrame = 0;
		// The frame before was not armed (the first CPU frame, or GPU frames between): the CPU's reported verdicts start again, and so does
		// the visibility decoder (CullFrame::visibilityReset, Readback::Batch::reset).
		bool visibilityReset = false;
	};
	CpuFrameArm TakeCpuFrameArm();
	/** @brief Any thread: whether the frame's root states are the CPU stage's (its bindings: the fade states the draws read). */
	bool CpuRootStatesFor(std::uint32_t a_sceneFrame);

	/**
	 * @brief FrameValues' producer, after the placements' sampling and the live-sphere stores, before the registered pools' sampling: the
	 * frame's rows, for the CPU fade stage (SampledRows). Producer thread only.
	 */
	void OnFrameValuesSampled(std::uint64_t a_seq, std::span<const BindlessPlacement> a_rows);
	/** @brief The producer's (a registered pool's sample): the rows OnFrameValuesSampled was given for a_seq (empty: none). */
	std::span<const BindlessPlacement> SampledRows(std::uint64_t a_seq);

	/** @brief The CPU fade stage (producer thread): the frame's CullFrame, published; continuations waiting for its frame run here. */
	void PublishCullFrame(std::shared_ptr<CullFrame> a_frame);
	/**
	 * @brief Any thread: a_then(frame) once the CullFrame of a_sceneFrame (or a later one) is published: at once if it is, else on the
	 * publishing thread. Lock-free (a stack the publication drains; each continuation runs once).
	 */
	void AfterCullFrame(std::uint32_t a_sceneFrame, std::function<void(const std::shared_ptr<const CullFrame>&)> a_then);
	/** @brief Any thread: the newest published CullFrame (null before the first). */
	std::shared_ptr<const CullFrame> LatestCullFrame();

	// ---- The cull ----

	/**
	 * @brief Any pool thread. Culls a_batch for a_view; writes the kept lanes and their object words into a_arena; returns the CulledBatch
	 * (in the arena). a_live: the per-frame cull, reading liveSpheres[a_parity]; else the snapshot's spheres (a per-snapshot cull). The
	 * caller sets `last` (and `commands`). A main-camera view (Main, Decal, Forward, Refraction) also stamps each lane in its frustum for
	 * kAccumulated; the frame's last such batch diffs the verdicts into the visibility decoder.
	 */
	const CulledBatch& CullBatch(const ViewBatch& a_batch, bool a_live, std::uint32_t a_parity, const CullView& a_view, const CullFrame& a_frame, CullArena& a_arena);
	/**
	 * @brief The colour pass's words for the main list's depth-culled batch (same lanes and draw masks): each object word with the sun miss
	 * (a_colour.sunTest, the cascades) and the local shadow mask (the volumes), which only exist at the colour commit.
	 */
	const CulledBatch& ColourObjectWords(const CulledBatch& a_depth, const CullView& a_colour, const CullFrame& a_frame, CullArena& a_arena);

	// ---- The views, from the latches the GPU path writes at the same trigger ----

	/** @brief Any view from its BuildDrawsLatch (cull flags, view-projection, cull planes, focus, fade eye and tree height, strict context). */
	CullView CullViewOfLatch(ViewType a_type, std::uint32_t a_viewIndex, const Draws::BuildDrawsLatch& a_latch);
	/** @brief The main camera's per-frame view: the depth segment's latch (FillCullLatch, depthOnly). */
	CullView MainCullView(const Draws::BuildDrawsLatch& a_depthLatch);
	/**
	 * @brief The colour segment's views (Decal, Forward, Refraction; Main's colour words, ColourObjectWords): its latch, the cascades and
	 * local shadow volumes it latched (IndirectDraws::Impl::CommitShadowInputs' sunCascades, shadowVolumes clamped), held by the caller.
	 */
	CullView ColourCullView(ViewType a_type, const Draws::BuildDrawsLatch& a_colourLatch, std::span<const Draws::GpuCascade> a_cascades,
		std::span<const Draws::GpuShadowVolume> a_volumes);
	/** @brief A shadow view (Sun, Spot, Point): its slot's latch (ShadowEpochs.cpp's LatchShadowViews) and, for a sun view, the processes. */
	CullView ShadowCullView(ViewType a_type, std::uint32_t a_slot, const Draws::BuildDrawsLatch& a_latch, std::span<const Draws::SunEntryProcess> a_sunEntry);
	/** @brief An occlusion map (SkyOccluder, PrecipOccluder): its latch (LatchOcclusionViews). */
	CullView OcclusionCullView(ViewType a_type, std::uint32_t a_view, const Draws::BuildDrawsLatch& a_latch);
	/** @brief A water reflection face (Features/Reflection's view latch, ViewEpoch.cpp). */
	CullView ReflectionCullView(std::uint32_t a_face, const Draws::BuildDrawsLatch& a_latch);
	/** @brief The first-person view (Features/FirstPerson's view latch). */
	CullView FirstPersonCullView(const Draws::BuildDrawsLatch& a_latch);
}
