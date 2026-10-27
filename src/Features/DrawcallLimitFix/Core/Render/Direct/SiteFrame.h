#pragma once

// The direct sites' shared internals (Sites.h's implementation, split by site): the frame slots and what a site's frame is made of, the
// culls' dispatch, the callback's recording helpers, and the hooks each site's own file defines. Sites.cpp owns the frame (FrameStart,
// EndFrame, ExecuteEpoch, the slots' retirement) and the main depth, colour and forward sites; the others are in their own files:
// - ShadowSite.cpp: the shadow site (the shadow views and the occlusion maps);
// - ViewSites.cpp: the depth site's hosted views (the reflection's faces, first person's depth) and the first-person colour site;
// - Families.cpp: the draw families' draws at the depth and colour sites.
// Included by those files only (volk first: include Main/Resources.h before it).

#include "Features/DrawcallLimitFix/Core/Render/Direct/Cull.h"
#include "Features/DrawcallLimitFix/Core/Render/Direct/Recorder.h"
#include "Features/DrawcallLimitFix/Core/Render/Direct/Sites.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/GpuLayouts.h"

#include <rhi.h>

namespace org
{
	class ExternalEpochExecution;
}

#include <array>
#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <vector>

namespace org
{
	class ExternalTextureResource;
}

namespace DCLF::Direct::detail
{
	using Clock = std::chrono::steady_clock;
	inline constexpr std::size_t kSites = static_cast<std::size_t>(Site::kCount);

	inline double MicrosecondsSince(Clock::time_point a_start)
	{
		return std::chrono::duration<double, std::micro>(Clock::now() - a_start).count();
	}

	template <class T>
	void Add(T& a_counter, T a_value)
	{
		std::atomic_ref<T>(a_counter).fetch_add(a_value, std::memory_order_relaxed);
	}

	/** @brief One thread's secondaries for one frame slot: its own command pool, reset at the slot's retirement. */
	struct ListPool
	{
		rhi::CommandAllocatorPtr allocator;
		std::deque<rhi::CommandListPtr> lists;  // never moved: a CulledBatch names one by address
		std::size_t used = 0;

		/** @brief A secondary recycled for recording (begun by its recorder), or null when one could not be made. */
		rhi::CommandList* Next(rhi::Device& a_device);
		void Reset();
	};

	/**
	 * @brief Any thread: a_pool reset on DCLF's pool (none of its lists in use: its frame's signal completed, or its recording released), then
	 * ready for TakeResetPool. Off the render thread: a reset is a driver call per pool.
	 */
	void ResetLater(ListPool&& a_pool);
	/** @brief Render thread: a pool ResetLater made ready (or a new one when none is): nothing recorded in it. */
	ListPool TakeResetPool();

	/** @brief A pool worker's state for one frame slot: its cull arena and its secondaries. */
	struct Worker
	{
		CullArena arena;
		ListPool lists;
	};

	/** @brief The main resources' targets as the direct sites attach them: views of DCLF's own, made once per resources. */
	struct TargetViews
	{
		// The images the views are of, held as long as the views (and keyed by: a Resources' address can be reused by the next one, and
		// its targets change with new main resources; views of a released image drew into freed memory DXVK had reused).
		std::array<std::shared_ptr<org::ExternalTextureResource>, Draws::kColorTargets> native{};
		std::shared_ptr<org::ExternalTextureResource> nativeDepth, refractionNormals;
		rhi::DescriptorHeapPtr rtvHeap, dsvHeap;
		std::uint32_t count = 0;
		std::array<rhi::DescriptorSlot, Draws::kColorTargets> colours{};
		std::array<rhi::ResourceHandle, Draws::kColorTargets> colourResources{};
		std::array<rhi::Format, Draws::kColorTargets> formats{};
		rhi::DescriptorSlot depth{};
		rhi::Format depthFormat = rhi::Format::Unknown;
		rhi::DescriptorSlot refraction{};
		rhi::ResourceHandle refractionResource{};
		rhi::Format refractionFormat = rhi::Format::Unknown;
		bool valid = false;
	};

	struct SnapshotRecording;  // Sites.cpp

	/** @brief One view of a site in a frame: its cull's inputs and its culled batches as the site's callback receives them. */
	struct SiteView
	{
		ViewType type = ViewType::Main;
		CullView view;
		bool depthPass = false;
		// Main colour: its object words from the depth site's batches (ColourObjectWords); mainView culls them when the depth site did
		// not run this frame.
		bool colourWords = false;
		CullView mainView;
		const std::vector<ViewBatch>* list = nullptr;
		CulledQueue* queue = nullptr;
		std::vector<const CulledBatch*> arrived;  // the callback's: by batch index
		std::uint32_t received = 0;
		// Parallel mode: the rendering its batches' secondaries are recorded inside (CullJob; formats, extent). Main's is the main
		// targets' (CullJob fills it); another view's site sets it before its culls are dispatched (secondary: record them).
		bool secondary = false;
		rhi::SecondaryInheritance inheritance{};
		// The bindings its recordings use (null: its run's bindings).
		const RecordBindings* bindings = nullptr;
		// A per-snapshot view (PrepareSnapshotView): its recording, drawn from this frame (fromSnapshot: no culls, RecordViewPass executes
		// it), or made this frame by a job of its own beside its per-frame culls.
		std::shared_ptr<SnapshotRecording> snapshot;
		bool fromSnapshot = false;
	};

	struct FrameSlot;

	/** @brief One site's frame: its inputs, its views and what its callback records. */
	struct SiteRun
	{
		Site site = Site::Depth;
		RecordMode record = RecordMode::Serial;
		FrameSlot* slot = nullptr;
		rhi::Device device;
		std::shared_ptr<const Draws::PassFrame> shape;
		std::shared_ptr<Draws::Resources> resources;
		std::shared_ptr<const TargetViews> targets;  // the main sites' (null: the site attaches its own)
		// Its epoch's ORG execution (an external epoch's: the commits' uploads, then its CPU pipeline passes with ORG's barriers),
		// executed by its callback ahead of its draws in place of uploads; its waits and signals join DXVK's submission.
		std::shared_ptr<org::ExternalEpochExecution> execution;
		std::shared_ptr<const CullFrame> cullFrame;  // its views' culls' (written by their dispatch alone)
		rhi::CommandList* uploads = nullptr;          // the commits' staged uploads, recorded on the render thread
		RecordBindings bindings, refractionBindings;
		std::vector<SiteView*> views;
		SiteView *main = nullptr, *decal = nullptr, *forward = nullptr, *refractionView = nullptr;
		bool refraction = false;  // the forward site's refraction pass (SubmitSharedEpoch's optional part)
		// Whether its callback records its passes (false: the run's inputs were not complete; its queues are still drained).
		bool valid = true;
		// Its callback executed batches' secondaries (Parallel mode: their draws were counted when the pool recorded them, before it ran).
		bool executedSecondaries = false;
		// The site file's own state for this run (ShadowSite.cpp's views and maps, ViewSites.cpp's hosted views, Families.cpp's draws), by
		// file; released with the run at the slot's retirement.
		std::shared_ptr<void> shadowState, viewState, familyState;
		rhi::CommandListPtr adopted;  // DXVK's buffer, wrapped: kept to the slot's retirement (what its recording allocated)
	};

	/** @brief A frame slot: everything a CPU frame's sites made, kept until the frame's signal completes. */
	struct FrameSlot
	{
		std::uint32_t index = 0;
		// The host frame slot the colour commit writes its latch regions at; the frame's other commits write at index. The depth and colour
		// commits share the main latch block, whose region the GPU reads when the site's work runs, after both commits: one region would
		// hand the depth site's readers the colour latch (the GPU path's epochs take a slot each).
		std::uint32_t colourSlot = 0;
		std::uint64_t signal = 0;  // the CPU-frame timeline's value of the frame that used it last (0: none)
		bool used = false;         // a frame used it since its last Release
		std::unique_ptr<Worker[]> workers;
		std::uint32_t workerCount = 0;
		Worker render, cs;  // the render thread's (the uploads) and DXVK's CS thread's
		std::vector<std::unique_ptr<CulledQueue>> queues;  // reused: an EventQueue's ring is never freed
		std::size_t queuesUsed = 0;
		std::deque<SiteView> views;
		std::deque<SiteRun> runs;
		std::vector<std::shared_ptr<const void>> holds;
		std::shared_ptr<const ViewBatchSet> set;
		std::uint32_t sceneFrame = 0;
		// The index pool's first indices as the frame's first drawing commit left them (the recorder adds them per draw).
		std::vector<std::uint32_t> poolFirsts;
		bool poolFirstsTaken = false;
		// The forward and refraction lanes' placement rows (by object; the others unwritten), for their sort on the CS thread.
		std::vector<BindlessPlacement> forwardRows;
		// The main list's depth batches, by batch index, which the colour site's object words are made from; and the gate that
		// dispatches the main colour culls once both the depth site's culls and the colour site's cull frame are in.
		std::unique_ptr<std::atomic<const CulledBatch*>[]> mainDepth;
		std::size_t mainDepthCount = 0;
		std::atomic<std::uint32_t> mainDepthPending{ 0 };
		std::atomic<int> colourGate{ 0 };
		std::function<void()> colourDispatch;
		bool depthRan = false;
		SiteRun* forwardRun = nullptr;
		// The site files' own state for this frame (as SiteRun's), released at the slot's retirement.
		std::shared_ptr<void> shadowState, viewState, familyState;
		// The slot's frame's work still running on the pool: its cull jobs, and its cull-frame continuations once they started (one
		// that starts after the slot moved to another frame, generation, does nothing). Release waits for none to be left.
		std::atomic<std::uint32_t> outstanding{ 0 };
		std::atomic<std::uint64_t> generation{ 0 };
		FrameCounters counters;
		std::array<std::uint32_t, kSites> siteRuns{}, siteEmpty{};
		std::uint32_t waits = 0;
	};

	struct State
	{
		rhi::Device device;
		bool deviceSet = false;
		std::shared_ptr<rhi::TimelinePtr> timeline;
		VkSemaphore semaphore = VK_NULL_HANDLE;
		std::uint64_t signalled = 0;  // the timeline's last value queued
		std::uint64_t submitted = 0;  // the last value known submitted (SubmitQueued's flush, or a frame's own work after it)
		std::uint64_t hostSubmitted = 0;  // the host's submissions made at SubmitQueued's last flush
		std::vector<std::unique_ptr<FrameSlot>> slots;
		std::uint64_t frames = 0;
		FrameSlot* current = nullptr;  // the frame's (FrameStart to EndFrame)
		SiteRun* pending = nullptr;    // the site the epoch body being run opened (ExecuteEpoch)
		// The per-snapshot views' recordings of the newest set (PrepareSnapshotView; a released one's pool goes to ResetLater).
		std::vector<std::shared_ptr<SnapshotRecording>> snapshots;
		bool wasCpu = false;
		std::shared_ptr<const TargetViews> targets;
		std::atomic<std::uint32_t> nextWorker{ 0 };
		WindowCounters window;
		bool loggedNoCallback = false, loggedRecordedLists = false, loggedNotAdopted = false, loggedExternal = false;
		// Emit's scratch (render thread): a site's waits and signals for DXVK's submission.
		std::vector<RenderGraphRuntime::ExternalCommandsWait> emitWaits, emitSignals;
	};

	State& S();

	// ---- The frame's runs and views (render thread) ----

	/** @brief A run of a_site in a_slot, with only the frame's parts (no bindings, targets or shape: the site fills what it binds). */
	SiteRun& NewBareRun(FrameSlot& a_slot, Site a_site);
	/** @brief A main site's run (Depth, Colour, Forward): NewBareRun with the commit's shape, resources, main targets and bindings. */
	SiteRun& NewRun(FrameSlot& a_slot, Site a_site, const MainCommitInputs& a_inputs);
	/** @brief A view of a_run: a_type's list of the frame's set, its queue, its batch counters. */
	SiteView& NewView(FrameSlot& a_slot, SiteRun& a_run, ViewType a_type, const CullView& a_view, bool a_depthPass);
	/**
	 * @brief Render thread, after a_view's bindings and inheritance are set and before its culls are dispatched (the slot's index pool
	 * firsts taken): a view of a type at the PerSnapshot cadence (not the main camera's lists, nor first person, whose colour site draws
	 * its depth view's batches) draws its set's recording when one is ready for the same bindings and pass, and its index ranges have
	 * not moved; else it is culled per frame this frame, and a recording is made beside it for the frames after (culled once, with the
	 * snapshot's spheres, against the view as this frame captured it: pops at its edges are accepted).
	 */
	void PrepareSnapshotView(FrameSlot& a_slot, SiteRun& a_run, SiteView& a_view);
	/**
	 * @brief Render thread: the slot's copy of the index pool's first indices, once a frame (the first site's, after its commit), without
	 * the slots whose geometry is no longer the set's (ViewBatchSet::geometryIndexAddress).
	 */
	void TakePoolFirsts(FrameSlot& a_slot, const Draws::IndexPool* a_pool);
	/**
	 * @brief The run its epoch body opened: ExecuteEpoch queues its callback after the body (Emit). A site that runs outside
	 * Direct::ExecuteEpoch queues it with Emit itself.
	 */
	void SetPending(SiteRun& a_run);
	/** @brief Render thread: a_run's callback queued (with the uploads its commits staged and the frame values' wait). False: not queued. */
	bool Emit(SiteRun& a_run);

	// ---- The culls (DCLF's pool) ----

	/** @brief Any thread: a_view's culls dispatched on DCLF's pool (never run on the caller: it may be the render thread). */
	void DispatchCulls(FrameSlot& a_slot, SiteRun& a_run, SiteView& a_view, const std::shared_ptr<const CullFrame>& a_frame);

	/**
	 * @brief AfterCullFrame for a_slot's frame: a_then(frame) on the cull frame's publication, unless the slot moved to another frame
	 * first (it then does nothing: Release waits only for continuations that started).
	 */
	template <class Then>
	void OnCullFrame(FrameSlot& a_slot, Then a_then)
	{
		const std::uint64_t generation = a_slot.generation.load(std::memory_order_acquire);
		AfterCullFrame(a_slot.sceneFrame, [slot = &a_slot, generation, then = std::move(a_then)](const std::shared_ptr<const CullFrame>& a_frame) {
			slot->outstanding.fetch_add(1, std::memory_order_seq_cst);
			if (slot->generation.load(std::memory_order_seq_cst) == generation)
				then(a_frame);
			slot->outstanding.fetch_sub(1, std::memory_order_acq_rel);
		});
	}

	// ---- The callback (DXVK's CS thread) ----

	/** @brief a_view's batch a_index, once its job pushed it (the reorder window: later ones are kept as they come). */
	const CulledBatch& Await(SiteRun& a_run, SiteView& a_view, std::size_t a_index);
	/** @brief Every batch of a_view, in list order. */
	std::vector<const CulledBatch*> AwaitAll(SiteRun& a_run, SiteView& a_view);

	/**
	 * @brief Where a site's callback records (DXVK's CS thread). DXVK's own buffer takes only barriers, the passes whose contents are
	 * executed secondaries, and the executions themselves; everything that binds state goes into secondaries of the CS thread's
	 * (Inline), each executed in DXVK's buffer once the next primary command or the callback's end needs it (Flush). A secondary begins
	 * with no state bound, so nothing of DXVK's leaks into the RHI's recording or back.
	 *
	 * This does not make the descriptor models mixable: with the RHI on descriptor heaps and DXVK on descriptor buffers, NVIDIA fetched
	 * garbage descriptors in these secondaries too (faults with no owning resource, by where in DXVK's stream the callback landed). So the
	 * two always share one model, heaps or descriptor buffers: DXVK negotiates it with the render graph at device creation and grants the
	 * graph only its own (RenderGraphRuntime's NegotiateBindingModel; dxvk_device_info.cpp, negotiateBindingModel).
	 */
	struct SiteOut
	{
		rhi::CommandList& primary;
		ListPool& pool;
		rhi::Device& device;
		// The run's descriptor heaps (its bindings'), bound in DXVK's buffer before each execution of the site's secondaries (BindHeaps).
		rhi::DescriptorHeapHandle resourceHeap{}, samplerHeap{};
		rhi::CommandList* open = nullptr;

		rhi::CommandList& Inline();
		rhi::CommandList& Primary();
		void Flush();
		/**
		 * @brief The run's heaps bound in DXVK's buffer, as well as in the secondaries it is about to execute (SetDescriptorHeaps: descriptor
		 * heaps, or descriptor buffers and their offsets, whichever model the device runs on). On NVIDIA, on heaps, a secondary's own heap
		 * binding did not hold while the executing buffer had DXVK's heap bound (the draws fetched DXVK's descriptors at the graph's
		 * indices: MMU faults with no owning resource, for a callback recorded mid command list; one at a command list's start, before
		 * DXVK bound a heap, ran clean). Descriptor buffers are bound the same way, as the safe assumption. Every execution leaves the
		 * buffer's bindings undefined, so before each one. DXVK binds its own heaps or buffers again after the callback
		 * (DXVK_ORG_EXTERNAL_COMMANDS_BINDS_STATE).
		 */
		void BindHeaps();
	};

	/** @brief The stencil format of a depth format with stencil (Unknown: none). */
	rhi::Format StencilOf(rhi::Format a_depth);

	/**
	 * @brief A view's pass over its batches: Serial (or a view without secondaries), each batch recorded inline as it arrives, in list
	 * order, inside a_begin's rendering; Parallel with a_view.secondary, the batches' secondaries executed in list order in DXVK's buffer
	 * once all arrived, in a_begin's rendering with secondary contents. a_context's list is replaced by the one recorded to. Returns the
	 * batches, in list order.
	 */
	std::vector<const CulledBatch*> RecordViewPass(SiteOut& a_out, SiteRun& a_run, SiteView& a_view, rhi::PassBeginInfo a_begin, RecordBindings const& a_bindings);

	// ---- The site files' hooks ----

	// ShadowSite.cpp: the shadow site's callback body (Site::Shadow); its slot state released.
	void RecordShadowSite(SiteOut& a_out, SiteRun& a_run);
	// ViewSites.cpp: the depth site's hosted views, after main depth (their runs' views, rendered into their own targets); the first-person
	// colour site's callback body (Site::FirstPerson).
	void RecordHostedDepthViews(SiteOut& a_out, SiteRun& a_run);
	void RecordFirstPersonSite(SiteOut& a_out, SiteRun& a_run);
	// Families.cpp: the depth and colour sites' families (render thread, from NewRun's main commit: what their draws bind); their draws in
	// the callback, after the site's main pass (a pass of their own: a_depthPass, the depth site's).
	void PrepareFamilies(FrameSlot& a_slot, SiteRun& a_run, const MainCommitInputs& a_inputs);
	void RecordFamilies(SiteOut& a_out, SiteRun& a_run, bool a_depthPass);
}
