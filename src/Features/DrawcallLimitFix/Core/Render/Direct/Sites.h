#pragma once

// The CPU-driven pipeline's draw sites (Direct.h; dclf-architecture.md, "The CPU-driven pipeline"): at each draw epoch's trigger, while
// the graph is built for the CPU pipeline (GraphConditions::cpuPipeline), the epoch's commits run on the render thread as in the GPU
// path (they latch the frame's values), its submission is the site's instead: the site's views culled on DCLF's pool, and one callback
// queued into DXVK's command stream (RenderGraphRuntime::EmitExternalCommands) that records, on DXVK's CS thread into DXVK's own command
// buffer, its epoch's ORG execution (the commits' uploads, its CPU pipeline passes), the views' direct draws (Recorder.h) inside one rendering per
// target, and the exit barrier. Nothing waits on the render thread but a frame slot's reuse (FrameStart).
//
// The sites: shadow (the shadow views and occlusion maps, ShadowSite.cpp), depth (main depth, the families' depth and the hosted views:
// the reflection's faces and first person's depth, ViewSites.cpp), colour (main colour, decals, the families' colour), forward (forward
// and refraction) and first person's colour. A view type at the per-snapshot cadence is drawn from a recording made once per batch set
// (SiteFrame.h, PrepareSnapshotView).
//
// Lifetime: everything a frame's sites made (the culled batches' arenas, the secondaries and their allocators, the adopted list's
// wrapper, the executions' owners, the pre-work, the batch set and the cull frame) is its frame slot's, kept until the frame's signal on
// DCLF's CPU-frame timeline (signalled after the frame's last callback, EndFrame) completes: the slot's next frame waits for it at its
// start (one host wait, the slots' depth ahead) before it resets them. The ORG latch regions the commits write are the same slot's
// (PersistentGraphHost::BeginExternalFrameSlot), so they are not rewritten while the GPU reads them either.

#include "Features/DrawcallLimitFix/Api/Passes.h"
#include "Features/DrawcallLimitFix/Core/Render/Direct/Direct.h"
#include "RenderGraph/RenderGraphRuntime.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>

namespace org::services
{
	class NamedVersions;
}

namespace DCLF::Draws
{
	struct BuildDrawsLatch;
	struct GpuCascade;
	struct GpuShadowVolume;
	struct PassFrame;
	struct Resources;
}

namespace DCLF::Direct
{
	/**
	 * @brief The draw sites' late passes (SitePasses.cpp), registered with Core's: one last in each site's epoch, in the CPU pipeline's
	 * graph only (Api::PassDef::recordAtExecution), which records the site's draws.
	 */
	std::span<const Api::PassDef> SitePassDefs();

	/** @brief Any thread: whether the frame draws through the direct sites (the graph as built is the CPU pipeline's). */
	bool CpuFrame();

	/** @brief Render thread: the frame's mode (CpuFrame's pipeline, the frame's toggles' record mode and cadences). */
	Mode FrameMode();

	/**
	 * @brief Render thread, at the frame's start once its snapshot is adopted, before its coverage (IndirectDraws::DecideCoverage) and
	 * before FrameValues::Kick: a_set (the adopted snapshot's view batches; null: none) is the frame's. In CPU mode: the frame's slot is
	 * taken (waiting for its last frame's signal when the GPU has not passed it yet), its last frame's state released, the ORG latch
	 * regions the commits write set to it (BeginExternalFrameSlot), and the frame's CPU state kicked (KickCpuFrameState). Frames after
	 * CPU frames that are not ones release the CPU frames' slots as their signals complete, never waiting for them.
	 */
	void FrameStart(std::shared_ptr<const ViewBatchSet> a_set);

	/**
	 * @brief Render thread, while it waits for GPU work at a frame's start: what was queued into DXVK's stream since its last flush
	 * submitted - the last CPU frame's signal (EndFrame queues it after Present, into DXVK's next command list) or a host submission made
	 * since (a graph build's, the wait's own). The frame's own work would submit them; anything waiting for them meanwhile (a slot's
	 * release, a payload version built into a backing they retire) would wait for the waiting frame. DXVK's list is flushed only when
	 * something is queued.
	 */
	void SubmitQueued();

	/**
	 * @brief Render thread, outside a CPU frame (after CPU frames, and while a frame's start waits): the slots whose frames' GPU work is
	 * done released, never waited for. What they hold (the batch sets, and through them their payloads' versions) leases backings a new
	 * payload version may need; a slot is otherwise released only at its reuse (FrameStart). The last CPU frame's signal reaches the GPU
	 * with the next submission (SubmitQueued).
	 */
	void ReleaseCompletedSlots();

	/** @brief Render thread: the frame's view batches (FrameStart's), null before the first or in GPU mode. */
	const ViewBatchSet* FrameBatches();

	/**
	 * @brief Render thread, at Present after the frame's last site: the frame's signal queued behind its callbacks (a callback of its
	 * own, which records nothing), the external latch slot closed, the report's counters taken.
	 */
	void EndFrame();

	/**
	 * @brief Render thread, at a draw epoch's trigger: the GPU path's RenderGraphRuntime::ExecuteEpoch(a_segment, a_body, a_owner); in CPU
	 * mode a_body (the epoch's commits) runs here, outside any ORG epoch, a_owner is held by the frame's slot, and the site its commit
	 * opened (DepthCommitted, ColourCommitted, ForwardReached) queues its callback after it, with the uploads the commits staged. False:
	 * the epoch failed (GPU), or no callback could be queued (CPU; logged once).
	 */
	bool ExecuteEpoch(RenderGraphRuntime::Segment a_segment, const std::function<void()>& a_body, std::shared_ptr<const void> a_owner);

	/** @brief A main commit's inputs to its site (Payload/Uploads.cpp's CommitMainPayload, where the GPU path submits its recording). */
	struct MainCommitInputs
	{
		std::shared_ptr<const Draws::PassFrame> shape;  // the revision's shape the commit wrote against
		std::shared_ptr<Draws::Resources> resources;     // the main resources (the targets, the frame constants, the pool)
		const org::services::NamedVersions* adopted = nullptr;  // the frame's adopted set (held by the execution's owner)
		const Draws::BuildDrawsLatch* latch = nullptr;  // the segment's BuildDraws latch as written (the views' cull inputs)
		std::span<const Draws::GpuCascade> cascades;     // the colour commit's: the sun's cascades it latched
		std::span<const Draws::GpuShadowVolume> volumes;  // ... and the local shadow volumes (clamped)
		std::uint32_t latchSlot = 0;   // the latch regions it wrote (the external frame slot)
		std::uint32_t poolCopies = 0;  // the depth commit's index pool copies (0: its pre-work skips the dispatch)
	};

	/** @brief Render thread, inside the depth epoch's commit: the depth site (main depth; its families' depth). */
	void DepthCommitted(const MainCommitInputs& a_inputs);

	/**
	 * @brief Render thread, inside the colour epoch's commit: the colour site (main colour, decals, the families' colour), and the forward
	 * site's culls (forward, refraction: their view is this commit's), drawn at ForwardReached.
	 */
	void ColourCommitted(const MainCommitInputs& a_inputs);

	/**
	 * @brief Render thread, at the forward epoch's trigger (IndirectDraws::SubmitSharedEpoch's body, in ExecuteEpoch): the forward site, from
	 * this frame's colour commit (ColourCommitted); a_refraction: its refraction pass (the epoch's optional part, prepared). False: no
	 * colour commit this frame (nothing to draw, as the GPU path's noSharedPayload).
	 */
	bool ForwardReached(bool a_refraction);

	/**
	 * @brief Render thread (coverage, IndirectDraws::Impl::DecideEpochCoverage): whether the shadow site (ShadowSite.cpp) draws the shadow
	 * epoch's views and maps in CPU mode, and whether the view sites (ViewSites.cpp) draw view a_view's epochs (a ViewKind's index): an
	 * epoch whose site is not live is not covered, and the engine draws it.
	 */
	bool ShadowSiteLive();
	bool ViewSiteLive(std::uint32_t a_view);

	/** @brief One report window's totals (Reports::CpuPipelineLine): FrameCounters summed, the frames, and the sites' drawn > 0 checks. */
	struct WindowCounters
	{
		FrameCounters sum;
		std::uint32_t frames = 0;
		std::array<std::uint32_t, static_cast<std::size_t>(Site::kCount)> siteRuns{}, siteEmpty{};  // callbacks, and those that drew nothing
		std::uint32_t notQueued = 0, slotWaits = 0, rowWaits = 0;
		double slotWaitMs = 0.0;
		// The CPU frames' render thread and DXVK CS thread CPU time (FrameStart to FrameStart), and their wall time.
		double renderBusyUs = 0.0, csBusyUs = 0.0, wallUs = 0.0;
	};

	/** @brief Render thread (the reports): the window's counters since the last take. */
	WindowCounters TakeWindowCounters();
}
