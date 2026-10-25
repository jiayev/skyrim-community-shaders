// The CPU-driven pipeline's draw sites (see the header): the frame slots, the culls dispatched at each trigger, and the one callback per
// site that records the site's uploads, pre-work and direct draws into DXVK's command buffer on its CS thread.

// Main/Resources.h first: volk must precede every Vulkan header of the translation unit.
#include "Features/DrawcallLimitFix/Core/Render/Main/Resources.h"
#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Core/Render/Direct/SiteFrame.h"
#include "Features/DrawcallLimitFix/Core/Render/Direct/ViewSites.h"
#include "Features/DrawcallLimitFix/Core/Extensions/MainRanges.h"
#include "Features/DrawcallLimitFix/Core/Gpu/FrameValues.h"
#include "Features/DrawcallLimitFix/Core/Render/Epochs/GraphConditions.h"
#include "Features/DrawcallLimitFix/Core/Render/Passes/DrawHelpers.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/GpuLayouts.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/IndexPool.h"
#include "Features/DrawcallLimitFix/Core/Render/Revision/Shapes.h"
#include "Features/DrawcallLimitFix/Core/Render/Snapshot/ViewBatches.h"
#include "Features/DrawcallLimitFix/Core/Runtime/FrameRetirement.h"
#include "Features/DrawcallLimitFix/Core/Runtime/SceneScheduler.h"
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"

#include <Render/Runtime/ScopedActiveGraphServices.h>
#include <Resources/Buffers/Buffer.h>
#include <rhi.h>
#include <rhi_interop.h>
#include <rhi_interop_vulkan.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <thread>
#include <vector>

namespace DCLF::Direct::detail
{
	namespace
	{
		// The batches a cull job culls in turn (a job each would be a dispatch per 256 lanes).
		constexpr std::size_t kJobBatches = 8;
	}

	rhi::CommandList* ListPool::Next(rhi::Device& a_device)
	{
		if (!allocator && a_device.CreateCommandAllocator(rhi::QueueKind::Graphics, allocator) != rhi::Result::Ok)
			return nullptr;
		if (used == lists.size()) {
			rhi::CommandListPtr list;
			if (a_device.CreateSecondaryCommandList(rhi::QueueKind::Graphics, allocator.Get(), list) != rhi::Result::Ok)
				return nullptr;
			lists.push_back(std::move(list));
		}
		auto& list = lists[used++].Get();
		list.Recycle(allocator.Get());
		return &list;
	}

	void ListPool::Reset()
	{
		if (allocator && used)
			allocator->Recycle();
		used = 0;
	}

	State& S()
	{
		static State state;
		return state;
	}

	/** @brief The calling pool worker's index (assigned at its first cull; the pool's threads are fixed). */
	std::uint32_t WorkerIndex()
	{
		thread_local std::uint32_t index = ~0u;
		if (index == ~0u)
			index = S().nextWorker.fetch_add(1, std::memory_order_relaxed);
		return index;
	}

	Worker& WorkerOf(FrameSlot& a_slot)
	{
		const std::uint32_t index = WorkerIndex();
		if (index >= a_slot.workerCount)
			stl::report_and_fail(fmt::format("Drawcall Limit Fix: a CPU-pipeline cull ran on thread {} of a pool of {} (not a preparation worker)", index,
				a_slot.workerCount));
		return a_slot.workers[index];
	}

	rhi::Format StencilOf(rhi::Format a_depth)
	{
		return a_depth == rhi::Format::D24_UNorm_S8_UInt || a_depth == rhi::Format::D32_Float_S8X24_UInt ? a_depth : rhi::Format::Unknown;
	}

	/**
	 * @brief Render thread: the main resources' targets (made once per resources: RTVs and a DSV in heaps of DCLF's own; the ones before
	 * are kept by the slot of the frame that replaced them, as frames in flight attach them).
	 */
	std::shared_ptr<const TargetViews> TargetsOf(FrameSlot& a_slot, const Draws::Resources& a_resources)
	{
		auto& s = S();
		if (s.targets && s.targets->count == a_resources.targetCount && s.targets->native == a_resources.native &&
			s.targets->nativeDepth == a_resources.nativeDepth && s.targets->refractionNormals == a_resources.refractionNormals)
			return s.targets;
		if (s.targets)
			a_slot.holds.push_back(std::move(s.targets));
		auto views = std::make_shared<TargetViews>();
		views->native = a_resources.native;
		views->nativeDepth = a_resources.nativeDepth;
		views->refractionNormals = a_resources.refractionNormals;
		views->count = a_resources.targetCount;
		const std::uint32_t rtvs = a_resources.targetCount + 1;
		bool ok = s.device.CreateDescriptorHeap({ rhi::DescriptorHeapType::RTV, rtvs, false, "cs.dclf.direct.rtv" }, views->rtvHeap) == rhi::Result::Ok &&
		          s.device.CreateDescriptorHeap({ rhi::DescriptorHeapType::DSV, 1, false, "cs.dclf.direct.dsv" }, views->dsvHeap) == rhi::Result::Ok;
		for (std::uint32_t i = 0; ok && i < a_resources.targetCount; ++i) {
			const auto& target = a_resources.native[i];
			ok = target != nullptr;
			if (!ok)
				break;
			views->colourResources[i] = target->GetAPIResource().GetHandle();
			views->formats[i] = target->GetFormat();
			views->colours[i] = rhi::DescriptorSlot(views->rtvHeap->GetHandle(), i);
			ok = s.device.CreateRenderTargetView(views->colours[i], views->colourResources[i], rhi::RtvDesc{}) == rhi::Result::Ok;
		}
		if (ok && a_resources.nativeDepth) {
			views->depthFormat = a_resources.nativeDepth->GetFormat();
			views->depth = rhi::DescriptorSlot(views->dsvHeap->GetHandle(), 0);
			ok = s.device.CreateDepthStencilView(views->depth, a_resources.nativeDepth->GetAPIResource().GetHandle(), rhi::DsvDesc{}) == rhi::Result::Ok;
		} else {
			ok = false;
		}
		if (ok && a_resources.refractionNormals) {
			views->refractionResource = a_resources.refractionNormals->GetAPIResource().GetHandle();
			views->refractionFormat = a_resources.refractionNormals->GetFormat();
			views->refraction = rhi::DescriptorSlot(views->rtvHeap->GetHandle(), a_resources.targetCount);
			if (s.device.CreateRenderTargetView(views->refraction, views->refractionResource, rhi::RtvDesc{}) != rhi::Result::Ok)
				views->refractionResource = {};
		}
		views->valid = ok;
		if (!ok)
			logger::error("[DCLF] CPU pipeline: the main targets' views could not be made; its sites draw nothing");
		s.targets = views;
		return s.targets;
	}

	CulledQueue* QueueOf(FrameSlot& a_slot)
	{
		if (a_slot.queuesUsed == a_slot.queues.size())
			a_slot.queues.push_back(std::make_unique<CulledQueue>());
		return a_slot.queues[a_slot.queuesUsed++].get();
	}

	SiteView& NewView(FrameSlot& a_slot, SiteRun& a_run, ViewType a_type, const CullView& a_view, bool a_depthPass)
	{
		auto& view = a_slot.views.emplace_back();
		view.type = a_type;
		view.view = a_view;
		view.depthPass = a_depthPass;
		view.list = &a_slot.set->List(a_type);
		view.queue = QueueOf(a_slot);
		view.arrived.assign(view.list->size(), nullptr);
		a_run.views.push_back(&view);
		Add(a_slot.counters.batches[static_cast<std::size_t>(a_type)], static_cast<std::uint32_t>(view.list->size()));
		return view;
	}

	/** @brief Render thread: the slot's copy of the index pool's first indices, once a frame (the first site's, after its commit). */
	void TakePoolFirsts(FrameSlot& a_slot, const Draws::IndexPool* a_pool)
	{
		if (a_slot.poolFirstsTaken || !a_pool || !a_slot.set)
			return;
		// A slot's first only while its range is of the index buffer the set's sequences were resolved against: one the frame's tables
		// gave other geometry since the set (released, reassigned, re-pointed: a cell's streaming) holds that geometry's indices, which
		// the set's sequence would pull its own vertex buffer with, unbounded (a fault past it). Its draws wait for a newer set.
		const auto& pool = *a_pool;
		const auto& resolved = a_slot.set->geometryIndexAddress;
		a_slot.poolFirsts.assign(pool.slotFirst.size(), Draws::IndexPool::kNoRange);
		for (std::size_t g = 0; g < pool.slotFirst.size(); ++g) {
			if (pool.slotFirst[g] == Draws::IndexPool::kNoRange || g >= resolved.size() || !resolved[g])
				continue;
			if (g < pool.slots.size() && pool.slots[g].address == resolved[g])
				a_slot.poolFirsts[g] = pool.slotFirst[g];
			else
				++a_slot.counters.staleGeometry;
		}
		a_slot.poolFirstsTaken = true;
	}

	/** @brief Render thread: what a main site's recordings bind (the frame's adopted state; immutable while they read it). */
	void MainBindings(FrameSlot& a_slot, SiteRun& a_run, const MainCommitInputs& a_inputs)
	{
		auto& b = a_run.bindings;
		const auto& shape = *a_inputs.shape;
		b.resourceHeap = shape.resourceHeap;
		b.samplerHeap = shape.samplerHeap;
		b.main = &shape.indirect;
		if (a_inputs.adopted && a_inputs.resources->pool)
			if (auto version = a_inputs.adopted->Find(*a_inputs.resources->pool->indices); version && version->buffer) {
				b.indexPool = version->buffer->GetAPIResource().GetHandle();
				a_slot.holds.push_back(std::move(version));
			}
		b.poolFirsts = a_slot.poolFirsts;
		b.framePush = Draws::MainFramePushWords(a_inputs.resources->frameConstantsAddress);
		a_run.refractionBindings = b;
		std::uint32_t push[kShadowPushWords]{};
		Draws::MainShadowLayoutPush(a_inputs.resources->frameConstantsAddress, push);
		std::copy(std::begin(push), std::end(push), a_run.refractionBindings.shadowPush.begin());
		a_run.refractionBindings.lodFadeFrame = Draws::LodFadeFrameAddress(a_inputs.resources->frameConstantsAddress);
	}

	SiteRun& NewBareRun(FrameSlot& a_slot, Site a_site)
	{
		auto& run = a_slot.runs.emplace_back();
		run.site = a_site;
		run.record = FrameMode().record;
		run.slot = &a_slot;
		run.device = S().device;
		return run;
	}

	SiteRun& NewRun(FrameSlot& a_slot, Site a_site, const MainCommitInputs& a_inputs)
	{
		auto& run = NewBareRun(a_slot, a_site);
		run.shape = a_inputs.shape;
		run.resources = a_inputs.resources;
		run.targets = TargetsOf(a_slot, *a_inputs.resources);
		run.valid = run.targets && run.targets->valid;
		MainBindings(a_slot, run, a_inputs);
		if (a_site == Site::Depth || a_site == Site::Colour)
			PrepareFamilies(a_slot, run, a_inputs);
		return run;
	}

	void SetPending(SiteRun& a_run)
	{
		S().pending = &a_run;
	}

	// ---- The culls (DCLF's pool) ----

	void ArriveColour(FrameSlot& a_slot);

	/**
	 * @brief A pool job: a_view's batches [a_first, a_end) culled (and, in Parallel mode, each recorded into a secondary of the worker's),
	 * each pushed to the view's queue; the main list's depth batches noted for the colour words.
	 */
	void CullJob(FrameSlot& a_slot, SiteRun& a_run, SiteView& a_view, const CullFrame& a_frame, std::size_t a_first, std::size_t a_end)
	{
		const auto start = Clock::now();
		auto& worker = WorkerOf(a_slot);
		const auto& list = *a_view.list;
		const auto& set = *a_slot.set;
		const std::uint32_t parity = static_cast<std::uint32_t>(LiveParity(a_frame.frameValuesSeq));
		const bool depthMain = a_run.site == Site::Depth && a_view.type == ViewType::Main;
		// Parallel mode records the batches of the views with secondaries (the plain lists: main depth and colour, the shadow views...)
		// into secondaries; the decals, forward and refraction keep one ordered stream on the CS thread.
		const bool secondary = a_run.record == RecordMode::Parallel && a_view.secondary;
		const auto& bindings = a_view.bindings ? *a_view.bindings : a_run.bindings;
		for (std::size_t i = a_first; i < a_end; ++i) {
			const auto& batch = list[i];
			const CulledBatch* culled = nullptr;
			if (a_view.colourWords) {
				const CulledBatch* depth = i < a_slot.mainDepthCount ? a_slot.mainDepth[i].load(std::memory_order_acquire) : nullptr;
				if (!depth)
					depth = &CullBatch(batch, true, parity, a_view.mainView, a_frame, worker.arena);
				culled = &ColourObjectWords(*depth, a_view.view, a_frame, worker.arena);
			} else {
				culled = &CullBatch(batch, true, parity, a_view.view, a_frame, worker.arena);
			}
			auto* out = worker.arena.Make<CulledBatch>(1);
			*out = *culled;
			out->last = i + 1 == list.size();
			out->commands = nullptr;
			if (depthMain && i < a_slot.mainDepthCount)
				a_slot.mainDepth[i].store(out, std::memory_order_release);
			if (secondary && out->count)
				if (auto* commands = worker.lists.Next(a_run.device)) {
					RecordContext context{ *commands, set, a_view.type, a_view.view.viewIndex, a_run.site, a_view.depthPass, bindings, &a_slot.counters };
					if (RecordSecondary(context, a_view.inheritance, *out, *commands))
						out->commands = commands;
				}
			if (!a_view.view.stampOnly)
				a_view.queue->Push(out);
		}
		Add(a_slot.counters.cullUs[static_cast<std::size_t>(a_view.type)], MicrosecondsSince(start));
		if (depthMain && a_slot.mainDepthPending.fetch_sub(static_cast<std::uint32_t>(a_end - a_first), std::memory_order_acq_rel) == a_end - a_first)
			ArriveColour(a_slot);
	}

	// ---- Per-snapshot views ----

	/**
	 * @brief A per-snapshot view's recording (PrepareSnapshotView): its batches culled once with the set's snapshot spheres, against the
	 * view as the frame that made it captured it, into one simultaneous-use secondary of its own pool, which the set's later frames with
	 * the same bindings and pass execute in the view's rendering. Released with the set (the frames that executed it hold it too).
	 */
	struct SnapshotRecording
	{
		ViewType type = ViewType::Main;
		std::uint32_t viewIndex = 0;
		std::uint64_t identity = 0;  // IdentityOf: what the secondary bakes
		std::shared_ptr<const ViewBatchSet> set;
		std::shared_ptr<const void> version;  // the pipelines' set version (IndirectState / ShadowIndirectState::version)
		std::unique_ptr<ListPool> lists;
		std::vector<std::unique_ptr<ListPool>>* spares = nullptr;  // State::spareLists, which takes lists back
		CullArena arena;
		rhi::CommandList* commands = nullptr;  // null: nothing drawn
		// The index pool firsts its draws were recorded with: (geometry slot, first index), by slot. A frame whose pool moved one draws it per frame.
		std::vector<std::array<std::uint32_t, 2>> poolFirsts;
		std::uint32_t drawn = 0;
		std::atomic<int> state{ 0 };  // 0 being recorded, 1 ready, -1 failed (not tried again for the set)

		~SnapshotRecording()
		{
			if (lists && spares)
				spares->push_back(std::move(lists));
		}
	};

	namespace
	{
		/** @brief FNV-1a over trivially copyable values: a per-snapshot recording's identity. */
		struct Identity
		{
			std::uint64_t hash = 1469598103934665603ull;

			template <class T>
			Identity& Add(const T& a_value)
			{
				static_assert(std::is_trivially_copyable_v<T>);
				const auto* bytes = reinterpret_cast<const unsigned char*>(&a_value);
				for (std::size_t i = 0; i < sizeof(T); ++i)
					hash = (hash ^ bytes[i]) * 1099511628211ull;
				return *this;
			}
			template <class Range>
			Identity& AddAll(const Range& a_values)
			{
				Add(a_values.size());
				for (const auto& value : a_values)
					Add(value);
				return *this;
			}
		};

		/**
		 * @brief What a view's recording bakes: the view and its pass (the inheritance: formats, extent, depth range), the heaps, the index
		 * pool, the push words, the shadow map row and the pipelines (their set version, held by the recording, and the main set's bucket
		 * mapping, which a view site may derive per frame).
		 */
		std::uint64_t IdentityOf(const SiteView& a_view, const RecordBindings& a_bindings)
		{
			Identity id;
			const auto& inheritance = a_view.inheritance;
			id.Add(a_view.type).Add(a_view.view.viewIndex).Add(a_view.depthPass);
			id.AddAll(std::span<const rhi::Format>(inheritance.colorFormats.data, inheritance.colorFormats.size)).Add(inheritance.depthFormat).Add(inheritance.stencilFormat).Add(inheritance.x).Add(inheritance.y);
			id.Add(inheritance.width).Add(inheritance.height).Add(inheritance.minDepth).Add(inheritance.maxDepth);
			id.Add(a_bindings.resourceHeap).Add(a_bindings.samplerHeap).Add(a_bindings.indexPool).Add(a_bindings.framePush).Add(a_bindings.shadowPush);
			id.AddAll(a_bindings.shadowMapRow).Add(a_bindings.lodFadeFrame).Add(a_bindings.offsetDepth);
			if (const auto* main = a_bindings.main) {
				id.Add(main->version.get()).Add(main->zLayout).Add(main->refraction);
				id.AddAll(main->zGroups).AddAll(main->zPipelines);
				id.AddAll(main->plainColour).AddAll(main->plainDepth);
			}
			if (const auto* shadow = a_bindings.shadow)
				id.Add(shadow->version.get()).Add(shadow->layout);
			return id.hash;
		}

		/** @brief Whether a recording's draws' index ranges are where the frame's index pool has them. */
		bool PoolFirstsHold(const SnapshotRecording& a_recording, std::span<const std::uint32_t> a_firsts)
		{
			for (const auto& [geometry, first] : a_recording.poolFirsts)
				if (geometry >= a_firsts.size() || a_firsts[geometry] != first)
					return false;
			return true;
		}

		/** @brief A pool job: a_view's recording made (every batch culled with the snapshot's spheres, recorded in list order). */
		void RecordSnapshot(FrameSlot& a_slot, SiteRun& a_run, SiteView& a_view, const CullFrame& a_frame)
		{
			const auto start = Clock::now();
			auto& recording = *a_view.snapshot;
			const auto& set = *a_slot.set;
			const auto& bindings = a_view.bindings ? *a_view.bindings : a_run.bindings;
			auto* commands = recording.lists->Next(a_run.device);
			rhi::SecondaryInheritance inheritance = a_view.inheritance;
			inheritance.simultaneousUse = true;
			inheritance.debugName = "DCLF direct per-snapshot view";
			if (!commands || commands->BeginSecondary(inheritance) != rhi::Result::Ok) {
				recording.state.store(-1, std::memory_order_release);
				return;
			}
			RecordContext context{ *commands, set, a_view.type, a_view.view.viewIndex, a_run.site, a_view.depthPass, bindings, nullptr };
			const bool valid = BeginList(context);
			std::vector<std::uint32_t> geometries;
			for (const auto& batch : *a_view.list) {
				const auto& culled = CullBatch(batch, false, 0, a_view.view, a_frame, recording.arena);
				if (!valid || !culled.count)
					continue;
				RecordBatch(context, culled);
				for (std::uint32_t k = 0; k < culled.count; ++k) {
					const std::uint32_t lane = culled.lanes[k];
					const std::uint32_t sequences = batch.sequenceCount ? batch.sequenceCount[lane] : 1u;
					for (std::uint32_t q = 0; q < sequences; ++q)
						if (const std::uint32_t sequence = batch.sequence[lane] + q; sequence < set.sequenceArgs.size())
							geometries.push_back(set.sequenceArgs[sequence].geometrySlot);
				}
			}
			commands->End();
			std::sort(geometries.begin(), geometries.end());
			geometries.erase(std::unique(geometries.begin(), geometries.end()), geometries.end());
			recording.poolFirsts.reserve(geometries.size());
			for (const std::uint32_t geometry : geometries)
				recording.poolFirsts.push_back({ geometry, geometry < a_slot.poolFirsts.size() ? a_slot.poolFirsts[geometry] : ~0u });
			recording.drawn = context.drawn;
			recording.commands = valid && context.drawn ? commands : nullptr;
			Add(a_slot.counters.snapshotRecorded, 1u);
			Add(a_slot.counters.cullUs[static_cast<std::size_t>(a_view.type)], MicrosecondsSince(start));
			recording.state.store(valid ? 1 : -1, std::memory_order_release);
		}
	}

	void PrepareSnapshotView(FrameSlot& a_slot, SiteRun& a_run, SiteView& a_view)
	{
		switch (a_view.type) {
		case ViewType::Main:
		case ViewType::Decal:
		case ViewType::Forward:
		case ViewType::Refraction:
		case ViewType::FirstPerson:
			return;
		default:
			break;
		}
		if (FrameMode().cadence[static_cast<std::size_t>(a_view.type)] != Cadence::PerSnapshot || !a_slot.set || !a_view.inheritance.width)
			return;
		auto& s = S();
		const auto& bindings = a_view.bindings ? *a_view.bindings : a_run.bindings;
		const std::uint64_t identity = IdentityOf(a_view, bindings);
		// Only the newest set's recordings are kept (the frames that drew an older one's hold it).
		std::erase_if(s.snapshots, [&](const auto& a_recording) { return a_recording->set != a_slot.set; });
		const auto found = std::find_if(s.snapshots.begin(), s.snapshots.end(), [&](const auto& a_recording) {
			return a_recording->type == a_view.type && a_recording->viewIndex == a_view.view.viewIndex && a_recording->identity == identity;
		});
		if (found != s.snapshots.end()) {
			const int state = (*found)->state.load(std::memory_order_acquire);
			// Still being made, or failed for the set: per frame.
			if (state != 1)
				return;
			if (PoolFirstsHold(**found, a_slot.poolFirsts)) {
				a_view.snapshot = *found;
				a_view.fromSnapshot = true;
				a_slot.holds.push_back(*found);
				Add(a_slot.counters.snapshotExecuted, 1u);
				return;
			}
			// Its index ranges moved: made again from this frame.
			s.snapshots.erase(found);
		}
		auto recording = std::make_shared<SnapshotRecording>();
		recording->type = a_view.type;
		recording->viewIndex = a_view.view.viewIndex;
		recording->identity = identity;
		recording->set = a_slot.set;
		recording->version = bindings.main ? bindings.main->version : bindings.shadow ? bindings.shadow->version : nullptr;
		recording->spares = &s.spareLists;
		if (!s.spareLists.empty()) {
			recording->lists = std::move(s.spareLists.back());
			s.spareLists.pop_back();
			recording->lists->Reset();
		} else {
			recording->lists = std::make_unique<ListPool>();
		}
		s.snapshots.push_back(recording);
		a_view.snapshot = std::move(recording);
	}

	/**
	 * @brief Any thread: a_view's culls dispatched on DCLF's pool, kJobBatches a job (never run here: the caller may be the render thread);
	 * none for a view drawn from its recording, and its recording's job beside them for a view that makes one.
	 */
	void DispatchCulls(FrameSlot& a_slot, SiteRun& a_run, SiteView& a_view, const std::shared_ptr<const CullFrame>& a_frame)
	{
		if (a_view.fromSnapshot)
			return;
		if (a_view.snapshot) {
			a_slot.outstanding.fetch_add(1, std::memory_order_acq_rel);
			if (!SceneScheduler::Executor().Dispatch(SceneScheduler::Scope(), SceneExecutor::Preparation, org::async::TaskDispatch::Cpu, "dclf cpu snapshot view",
					[slot = &a_slot, run = &a_run, view = &a_view, frame = a_frame](const auto&) {
						RecordSnapshot(*slot, *run, *view, *frame);
						slot->outstanding.fetch_sub(1, std::memory_order_acq_rel);
					}))
				stl::report_and_fail("Drawcall Limit Fix: the preparation pool refused a CPU-pipeline per-snapshot view");
		}
		const std::size_t count = a_view.list->size();
		for (std::size_t first = 0; first < count; first += kJobBatches) {
			const std::size_t end = std::min(count, first + kJobBatches);
			a_slot.outstanding.fetch_add(1, std::memory_order_acq_rel);
			if (!SceneScheduler::Executor().Dispatch(SceneScheduler::Scope(), SceneExecutor::Preparation, org::async::TaskDispatch::Cpu, "dclf cpu cull",
					[slot = &a_slot, run = &a_run, view = &a_view, frame = a_frame, first, end](const auto&) {
						CullJob(*slot, *run, *view, *frame, first, end);
						slot->outstanding.fetch_sub(1, std::memory_order_acq_rel);
					}))
				stl::report_and_fail("Drawcall Limit Fix: the preparation pool refused a CPU-pipeline cull");
		}
	}

	/** @brief The colour gate's arrival: its last (the depth site's culls done, the colour site's cull frame in) dispatches main colour. */
	void ArriveColour(FrameSlot& a_slot)
	{
		if (a_slot.colourGate.fetch_sub(1, std::memory_order_acq_rel) == 1 && a_slot.colourDispatch)
			a_slot.colourDispatch();
	}

	// ---- The callback (DXVK's CS thread) ----

	/** @brief Takes what the view's queue holds into its arrival slots; how many arrived. */
	std::size_t DrainView(SiteView& a_view)
	{
		return a_view.queue->Drain([&](const CulledBatch*&& a_batch) {
			a_view.arrived[a_batch->batch->index] = a_batch;
			++a_view.received;
		});
	}

	/** @brief a_view's batch a_index, once its job pushed it (the reorder window: later ones are kept as they come). */
	const CulledBatch& Await(SiteRun& a_run, SiteView& a_view, std::size_t a_index)
	{
		bool waited = false;
		while (!a_view.arrived[a_index])
			if (!DrainView(a_view)) {
				if (!std::exchange(waited, true))
					Add(a_run.slot->waits, 1u);
				std::this_thread::yield();
			}
		return *a_view.arrived[a_index];
	}

	/** @brief Every batch of a_view, in list order. */
	std::vector<const CulledBatch*> AwaitAll(SiteRun& a_run, SiteView& a_view)
	{
		std::vector<const CulledBatch*> all;
		all.reserve(a_view.list->size());
		for (std::size_t i = 0; i < a_view.list->size(); ++i)
			all.push_back(&Await(a_run, a_view, i));
		return all;
	}

	/** @brief A rendering of the main targets: a_colours (none: depth only), the main depth loaded and stored. Not copyable: begin points into it. */
	struct MainPass
	{
		std::array<rhi::ColorAttachment, Draws::kColorTargets> colours{};
		rhi::DepthAttachment depth{};
		rhi::PassBeginInfo begin{};

		MainPass(const SiteRun& a_run, bool a_colours, const char* a_name)
		{
			const auto& targets = *a_run.targets;
			const auto& shape = *a_run.shape;
			for (std::uint32_t i = 0; a_colours && i < targets.count; ++i) {
				colours[i].rtv = targets.colours[i];
				colours[i].loadOp = rhi::LoadOp::Load;
				colours[i].storeOp = rhi::StoreOp::Store;
				colours[i].resource = targets.colourResources[i];
			}
			depth.dsv = targets.depth;
			depth.depthLoad = rhi::LoadOp::Load;
			depth.depthStore = rhi::StoreOp::Store;
			depth.stencilLoad = rhi::LoadOp::Load;
			depth.stencilStore = rhi::StoreOp::Store;
			begin.colors = { colours.data(), a_colours ? targets.count : 0u };
			begin.depth = &depth;
			begin.width = shape.width;
			begin.height = shape.height;
			begin.minDepth = shape.minDepth;
			begin.maxDepth = shape.maxDepth;
			begin.debugName = a_name;
		}
		MainPass(const MainPass&) = delete;
		MainPass& operator=(const MainPass&) = delete;
	};

	void BeginMain(rhi::CommandList& a_list, const SiteRun& a_run, bool a_colours, const char* a_name)
	{
		MainPass pass(a_run, a_colours, a_name);
		a_list.BeginPass(pass.begin);
	}

	/** @brief A main view's secondaries (Parallel mode): recorded inside the main targets' rendering, its colours or depth only. */
	void SetMainSecondary(const SiteRun& a_run, SiteView& a_view)
	{
		const auto& targets = *a_run.targets;
		const auto& shape = *a_run.shape;
		auto& inheritance = a_view.inheritance;
		a_view.secondary = true;
		if (!a_view.depthPass)
			inheritance.colorFormats = { targets.formats.data(), targets.count };
		inheritance.depthFormat = targets.depthFormat;
		inheritance.stencilFormat = StencilOf(targets.depthFormat);
		inheritance.width = shape.width;
		inheritance.height = shape.height;
		inheritance.minDepth = shape.minDepth;
		inheritance.maxDepth = shape.maxDepth;
		inheritance.debugName = a_view.depthPass ? "DCLF direct depth" : "DCLF direct colour";
	}

	rhi::CommandList& SiteOut::Inline()
	{
		if (open)
			return *open;
		open = pool.Next(device);
		rhi::SecondaryInheritance outside{};
		outside.insideRendering = false;
		outside.debugName = "DCLF direct site";
		if (!open || open->BeginSecondary(outside) != rhi::Result::Ok)
			stl::report_and_fail("Drawcall Limit Fix: the CPU pipeline could not begin a secondary list on DXVK's CS thread");
		return *open;
	}

	rhi::CommandList& SiteOut::Primary()
	{
		Flush();
		return primary;
	}

	void SiteOut::Flush()
	{
		if (!open)
			return;
		open->End();
		rhi::CommandList* list = std::exchange(open, nullptr);
		BindHeaps();
		primary.ExecuteCommands({ &list, 1u });
	}

	void SiteOut::BindHeaps()
	{
		if (resourceHeap.valid())
			primary.SetDescriptorHeaps(resourceHeap, samplerHeap);
	}

	std::vector<const CulledBatch*> RecordViewPass(SiteOut& a_out, SiteRun& a_run, SiteView& a_view, rhi::PassBeginInfo a_begin, const RecordBindings& a_bindings)
	{
		auto& slot = *a_run.slot;
		std::vector<const CulledBatch*> batches;
		batches.reserve(a_view.list->size());
		// A per-snapshot view drawn from its recording: executed in DXVK's buffer, inside the view's rendering (no batches this frame).
		if (a_view.fromSnapshot) {
			const auto& recording = *a_view.snapshot;
			if (!recording.commands)
				return batches;
			rhi::CommandList* commands = recording.commands;
			auto& primary = a_out.Primary();
			a_begin.contents = rhi::PassContents::SecondaryLists;
			primary.BeginPass(a_begin);
			a_out.BindHeaps();
			primary.ExecuteCommands({ &commands, 1u });
			primary.EndPass();
			a_run.executedSecondaries = true;
			Add(slot.counters.drawn[static_cast<std::size_t>(a_view.type)], recording.drawn);
			return batches;
		}
		if (a_run.record == RecordMode::Parallel && a_view.secondary) {
			batches = AwaitAll(a_run, a_view);
			std::vector<rhi::CommandList*> commands;
			for (const auto* batch : batches)
				if (batch->commands)
					commands.push_back(batch->commands);
			if (commands.empty())
				return batches;
			// The batches' secondaries in DXVK's buffer (a secondary cannot execute secondaries): the pass is the primary's.
			auto& primary = a_out.Primary();
			a_begin.contents = rhi::PassContents::SecondaryLists;
			primary.BeginPass(a_begin);
			a_out.BindHeaps();
			primary.ExecuteCommands({ commands.data(), static_cast<std::uint32_t>(commands.size()) });
			primary.EndPass();
			a_run.executedSecondaries = true;
			return batches;
		}
		auto& list = a_out.Inline();
		a_begin.contents = rhi::PassContents::Inline;
		list.BeginPass(a_begin);
		RecordContext context{ list, *slot.set, a_view.type, a_view.view.viewIndex, a_run.site, a_view.depthPass, a_bindings, &slot.counters };
		const bool valid = BeginList(context);
		for (std::size_t i = 0; i < a_view.list->size(); ++i) {
			const auto& batch = Await(a_run, a_view, i);
			batches.push_back(&batch);
			if (valid && batch.count)
				RecordBatch(context, batch);
		}
		list.EndPass();
		return batches;
	}

	/** @brief A plain list's pass (main depth, main colour) in the main targets' rendering (RecordViewPass). */
	std::vector<const CulledBatch*> RecordPlainList(SiteOut& a_out, SiteRun& a_run, SiteView& a_view, bool a_colours, const char* a_name)
	{
		MainPass pass(a_run, a_colours, a_name);
		return RecordViewPass(a_out, a_run, a_view, pass.begin, a_run.bindings);
	}

	void RecordDepthSite(SiteOut& a_out, SiteRun& a_run)
	{
		if (a_run.main)
			RecordPlainList(a_out, a_run, *a_run.main, false, "DCLF direct depth");
		RecordFamilies(a_out, a_run, true);
		RecordHostedDepthViews(a_out, a_run);
	}

	void RecordColourSite(SiteOut& a_out, SiteRun& a_run)
	{
		auto& slot = *a_run.slot;
		const auto& ranges = Extensions::MainRanges().drawn;
		// The decals: every batch, for the decal depth pass (first) and the second pass's groups.
		std::vector<const CulledBatch*> decals;
		if (a_run.decal)
			decals = AwaitAll(a_run, *a_run.decal);
		if (!decals.empty() && Extensions::MainRanges().anyDepthPrepass) {
			auto& list = a_out.Inline();
			BeginMain(list, a_run, false, "DCLF direct decal depth");
			RecordContext context{ list, *slot.set, ViewType::Decal, 0, Site::Colour, true, a_run.bindings, &slot.counters };
			if (BeginList(context))
				for (const auto& range : ranges)
					if (range.depthPrepass && range.slot < Draws::kDecalGroups)
						RecordDecalGroup(context, range.slot, decals);
			list.EndPass();
		}
		// The main list's colour, depth EQUAL against the depth site's.
		std::vector<const CulledBatch*> main;
		if (a_run.main)
			main = RecordPlainList(a_out, a_run, *a_run.main, true, "DCLF direct colour");
		RecordFamilies(a_out, a_run, false);
		// The second pass, in the main ranges' order: the decal groups and the terrain groups.
		if (ranges.empty() || (decals.empty() && main.empty()))
			return;
		auto& list = a_out.Inline();
		BeginMain(list, a_run, true, "DCLF direct second pass");
		RecordContext decal{ list, *slot.set, ViewType::Decal, 0, Site::Colour, false, a_run.bindings, &slot.counters };
		RecordContext terrain{ list, *slot.set, ViewType::Main, 0, Site::Colour, false, a_run.bindings, &slot.counters };
		for (const auto& range : ranges) {
			if (range.slot < Draws::kDecalGroups) {
				if (!decals.empty() && BeginList(decal))
					RecordDecalGroup(decal, range.slot, decals);
			} else if (!main.empty() && BeginList(terrain)) {
				RecordTerrainGroup(terrain, range.slot - Draws::kDecalGroups + 1, main);
			}
		}
		list.EndPass();
	}

	void RecordForwardSite(SiteOut& a_out, SiteRun& a_run)
	{
		auto& slot = *a_run.slot;
		if (a_run.forward) {
			const auto batches = AwaitAll(a_run, *a_run.forward);
			auto& list = a_out.Inline();
			BeginMain(list, a_run, true, "DCLF direct forward");
			RecordContext context{ list, *slot.set, ViewType::Forward, 0, Site::Forward, false, a_run.bindings, &slot.counters };
			if (BeginList(context))
				RecordForward(context, batches);
			list.EndPass();
		}
		if (a_run.refractionView) {
			const auto batches = AwaitAll(a_run, *a_run.refractionView);
			const auto& targets = *a_run.targets;
			if (!a_run.refraction || !targets.refractionResource.valid())
				return;
			const auto& shape = *a_run.shape;
			// The refraction normals cleared as the engine's RENDER_NORMAL_CLEAR (as Features/Forward's refraction pass), depth loaded.
			rhi::ColorAttachment colour{};
			colour.rtv = targets.refraction;
			colour.loadOp = rhi::LoadOp::Clear;
			colour.storeOp = rhi::StoreOp::Store;
			colour.clear.type = rhi::ClearValueType::Color;
			colour.clear.rgba[0] = colour.clear.rgba[1] = 0.5f;
			colour.clear.rgba[2] = colour.clear.rgba[3] = 0.0f;
			colour.resource = targets.refractionResource;
			rhi::DepthAttachment depth{};
			depth.dsv = targets.depth;
			depth.depthLoad = rhi::LoadOp::Load;
			depth.depthStore = rhi::StoreOp::Store;
			depth.stencilLoad = rhi::LoadOp::Load;
			depth.stencilStore = rhi::StoreOp::Store;
			rhi::PassBeginInfo begin{};
			begin.colors = { &colour, 1 };
			begin.depth = &depth;
			begin.width = shape.width;
			begin.height = shape.height;
			begin.minDepth = shape.minDepth;
			begin.maxDepth = shape.maxDepth;
			begin.debugName = "DCLF direct refraction normals";
			auto& list = a_out.Inline();
			list.BeginPass(begin);
			RecordContext context{ list, *slot.set, ViewType::Refraction, 0, Site::Forward, false, a_run.refractionBindings, &slot.counters };
			if (BeginList(context))
				RecordForward(context, batches);
			list.EndPass();
		}
	}

	/** @brief DXVK's CS thread: a site's commands into DXVK's command buffer (EmitExternalCommands' callback). */
	void SiteCallback(void* a_user, VkCommandBuffer a_commandBuffer)
	{
		auto& run = *static_cast<SiteRun*>(a_user);
		auto& slot = *run.slot;
		const auto start = Clock::now();
		const std::size_t site = static_cast<std::size_t>(run.site);
		std::uint32_t drawnBefore = 0;
		for (const auto* view : run.views)
			drawnBefore += std::atomic_ref<std::uint32_t>(slot.counters.drawn[static_cast<std::size_t>(view->type)]).load(std::memory_order_relaxed);
		run.adopted = rhi::vulkan::AdoptCommandBuffer(run.device, a_commandBuffer, rhi::QueueKind::Graphics);
		if (run.adopted && run.valid) {
			auto& list = run.adopted.Get();
			auto full = rhi::FullMemoryBarrier();
			rhi::BarrierBatch barrier{};
			barrier.globals = { &full, 1u };
			// The commits' staged uploads, then a barrier against everything after (their destinations are the draws' and pre-work's).
			if (run.uploads) {
				rhi::CommandList* uploads = run.uploads;
				list.ExecuteCommands({ &uploads, 1u });
				list.Barriers(barrier);
			}
			// The site's pre-work (its own barriers around it, PreWork.h), then its passes: all of it in secondaries but the parallel lists'
			// executions (SiteOut).
			SiteOut out{ list, slot.cs.lists, run.device, run.bindings.resourceHeap, run.bindings.samplerHeap };
			if (run.prework)
				RecordPreWork(*run.prework, out.Inline());
			switch (run.site) {
			case Site::Shadow:
				RecordShadowSite(out, run);
				break;
			case Site::Depth:
				RecordDepthSite(out, run);
				break;
			case Site::Colour:
				RecordColourSite(out, run);
				break;
			case Site::Forward:
				RecordForwardSite(out, run);
				break;
			case Site::FirstPerson:
				RecordFirstPersonSite(out, run);
				break;
			default:
				break;
			}
			out.Flush();
		} else if (!std::exchange(S().loggedNotAdopted, true)) {
			logger::error("[DCLF] CPU pipeline: DXVK's command buffer could not be adopted, or the site's targets have no views: the {} site draws nothing", site);
		}
		// Every view's queue empty for the slot's next frame, whatever was recorded.
		for (auto* view : run.views)
			if (!view->view.stampOnly && !view->fromSnapshot)
				while (view->received < view->list->size())
					if (!DrainView(*view))
						std::this_thread::yield();
		std::uint32_t drawnAfter = 0;
		for (const auto* view : run.views)
			drawnAfter += std::atomic_ref<std::uint32_t>(slot.counters.drawn[static_cast<std::size_t>(view->type)]).load(std::memory_order_relaxed);
		Add(slot.siteRuns[site], 1u);
		if (drawnAfter == drawnBefore && !run.executedSecondaries)
			Add(slot.siteEmpty[site], 1u);
		Add(slot.counters.recordUs[site], MicrosecondsSince(start));
	}

	/** @brief DXVK's CS thread: the frame's signal's callback, which records nothing (its signal joins the submission). */
	void SignalCallback(void*, VkCommandBuffer) {}

	/**
	 * @brief Render thread, after the epoch body: the site queued: the uploads the commits staged recorded into a secondary of the render
	 * thread's (executed first in the callback, so DXVK's list is not ended), and the callback with the frame values' wait.
	 */
	bool Emit(SiteRun& a_run)
	{
		auto& s = S();
		auto& slot = *a_run.slot;
		auto* host = RenderGraphRuntime::Get().Host();
		if (auto* graph = host ? host->Graph() : nullptr)
			if (auto* uploads = slot.render.lists.Next(s.device)) {
				rhi::SecondaryInheritance outside{};
				outside.insideRendering = false;
				outside.debugName = "DCLF direct uploads";
				if (uploads->BeginSecondary(outside) == rhi::Result::Ok) {
					std::shared_ptr<void> keepAlive;
					bool recorded = false;
					std::vector<rhi::CommandList> recordedLists;
					(void)graph->RecordPendingUploads(host->Device(), *uploads, static_cast<std::uint8_t>(slot.index), keepAlive, recorded, recordedLists);
					uploads->End();
					if (recorded)
						a_run.uploads = uploads;
					if (keepAlive)
						slot.holds.push_back(std::shared_ptr<const void>(std::move(keepAlive)));
					// Lists a producer recorded itself are primaries: no secondary can carry them, and DCLF's producers record none.
					if (!recordedLists.empty()) {
						s.window.recordedUploadLists += recordedLists.size();
						if (!std::exchange(s.loggedRecordedLists, true))
							logger::error("[DCLF] CPU pipeline: {} upload lists a producer recorded reached a CPU frame's site, which cannot submit them <- UPLOADS NOT TAKEN",
								recordedLists.size());
					}
				}
			}
		const auto wait = FrameValues::Get().FrameWaitPoint();
		const RenderGraphRuntime::ExternalCommandsWait waits[1]{ { wait.semaphore, wait.value, 0 } };
		const std::span<const RenderGraphRuntime::ExternalCommandsWait> waitSpan(waits, wait.semaphore ? 1u : 0u);
		static constexpr const char* kLabels[kSites] = { "DCLF direct shadow", "DCLF direct depth", "DCLF direct colour", "DCLF direct forward",
			"DCLF direct first person" };
		const bool queued = RenderGraphRuntime::Get().EmitExternalCommands(&SiteCallback, &a_run, waitSpan, {}, true, kLabels[static_cast<std::size_t>(a_run.site)], true);
		if (!queued) {
			++s.window.notQueued;
			if (!std::exchange(s.loggedNoCallback, true))
				logger::error("[DCLF] CPU pipeline: DXVK took no external commands (dxvkEmitExternalCommands missing): its sites draw nothing");
		}
		return queued;
	}

	/** @brief Render thread: a slot's last frame's state released (its signal completed), its counters into the window. */
	void Release(FrameSlot& a_slot)
	{
		// Nothing of its frame still runs on the pool (frames ago: in practice never waited for), and a continuation not started yet
		// will do nothing.
		a_slot.generation.fetch_add(1, std::memory_order_seq_cst);
		while (a_slot.outstanding.load(std::memory_order_seq_cst))
			std::this_thread::yield();
		// What no callback drained (a site whose trigger was not reached, a callback not queued).
		for (std::size_t q = 0; q < a_slot.queuesUsed; ++q)
			a_slot.queues[q]->Discard();
		auto& window = S().window;
		const auto& c = a_slot.counters;
		for (std::size_t v = 0; v < kViewTypes; ++v) {
			window.sum.batches[v] += c.batches[v];
			window.sum.drawn[v] += c.drawn[v];
			window.sum.cullUs[v] += c.cullUs[v];
		}
		for (std::size_t site = 0; site < kSites; ++site) {
			window.sum.recordUs[site] += c.recordUs[site];
			window.siteRuns[site] += a_slot.siteRuns[site];
			window.siteEmpty[site] += a_slot.siteEmpty[site];
		}
		window.sum.secondaries += c.secondaries;
		window.sum.waits += a_slot.waits;
		window.sum.snapshotRecorded += c.snapshotRecorded;
		window.sum.snapshotExecuted += c.snapshotExecuted;
		window.sum.staleGeometry += c.staleGeometry;
		for (std::uint32_t w = 0; w < a_slot.workerCount; ++w) {
			a_slot.workers[w].arena.Reset();
			a_slot.workers[w].lists.Reset();
		}
		a_slot.render.lists.Reset();
		a_slot.cs.lists.Reset();
		a_slot.queuesUsed = 0;
		a_slot.runs.clear();
		a_slot.views.clear();
		a_slot.holds.clear();
		a_slot.set.reset();
		a_slot.poolFirstsTaken = false;
		a_slot.mainDepth.reset();
		a_slot.mainDepthCount = 0;
		a_slot.colourDispatch = nullptr;
		a_slot.depthRan = false;
		a_slot.forwardRun = nullptr;
		a_slot.shadowState.reset();
		a_slot.viewState.reset();
		a_slot.familyState.reset();
		a_slot.counters = {};
		a_slot.siteRuns = {};
		a_slot.siteEmpty = {};
		a_slot.waits = 0;
		a_slot.signal = 0;
	}

	/** @brief Render thread: waits for a slot's last frame on the GPU (counted), then releases it. */
	void Retire(FrameSlot& a_slot)
	{
		auto& s = S();
		if (a_slot.signal && s.timeline && *s.timeline && (*s.timeline)->GetCompletedValue() < a_slot.signal) {
			const auto start = Clock::now();
			(void)(*s.timeline)->HostWait(a_slot.signal);
			++s.window.slotWaits;
			s.window.slotWaitMs += MicrosecondsSince(start) / 1000.0;
		}
		Release(a_slot);
	}

	bool EnsureDevice()
	{
		auto& s = S();
		if (s.deviceSet)
			return s.timeline != nullptr;
		auto* host = RenderGraphRuntime::Get().Host();
		if (!host)
			return false;
		s.deviceSet = true;
		s.device = host->Device();
		auto timeline = std::make_shared<rhi::TimelinePtr>();
		rhi::VulkanTimelineInfo info{};
		if (s.device.CreateTimeline(*timeline, 0, "cs.dclf.cpu-frames") != rhi::Result::Ok ||
			!rhi::QueryNativeTimeline(timeline->Get(), rhi::RHI_IID_VK_TIMELINE, &info, sizeof(info)) || !info.timeline) {
			logger::error("[DCLF] CPU pipeline: its frame timeline could not be made; CPU frames draw nothing");
			return false;
		}
		s.semaphore = static_cast<VkSemaphore>(info.timeline);
		s.timeline = timeline;
		SetCpuFrameTimeline(std::move(timeline));
		return true;
	}
}

namespace DCLF::Direct
{
	using namespace detail;

	bool CpuFrame()
	{
		return Draws::GraphBuiltForCpu();
	}

	Mode FrameMode()
	{
		Mode mode = ModeOf(FrameToggles());
		mode.cpu = CpuFrame();
		return mode;
	}

	void FrameStart(std::shared_ptr<const ViewBatchSet> a_set)
	{
		auto& s = S();
		PollRetirement();
		auto* host = RenderGraphRuntime::Get().Host();
		const bool cpu = CpuFrame() && host && EnsureDevice();
		if (!cpu) {
			// The first frame after CPU frames (a rebuild for the GPU pipeline): every CPU frame's GPU work done, the slots released (a
			// one-off wait at a rare event; the latch regions are the host's own slots' again).
			if (std::exchange(s.wasCpu, false)) {
				for (auto& slot : s.slots)
					Retire(*slot);
				// No CPU frame open: the points are the host's submissions and the frames signalled.
				NoteCpuFrameOpened(s.signalled);
			}
			FrameValues::Get().SetViewBatches(nullptr);
			return;
		}
		s.wasCpu = true;
		// What this frame binds from here on (its commits, FrameValues' versions, the payload's) retires after its own signal, the one
		// EndFrame queues behind its sites; a frame whose signal was not queued leaves the value to the next.
		NoteCpuFrameOpened(s.signalled + 1);
		// The slots: one per host frame slot (the commits' latch regions), each with a worker state per pool thread.
		const std::uint32_t slots = std::max(1u, host->FrameSlots());
		while (s.slots.size() < slots) {
			auto slot = std::make_unique<FrameSlot>();
			slot->index = static_cast<std::uint32_t>(s.slots.size());
			slot->workerCount = SceneScheduler::PreparationWorkers();
			slot->workers = std::make_unique<Worker[]>(slot->workerCount);
			s.slots.push_back(std::move(slot));
		}
		auto& slot = *s.slots[s.frames++ % slots];
		Retire(slot);
		slot.set = a_set;
		slot.sceneFrame = Scene::FrameView::Get().GetFrame();
		slot.colourGate.store(2, std::memory_order_relaxed);
		s.current = &slot;
		// The commits write their latch regions at this slot: no GPU work reads it now (Retire).
		host->BeginExternalFrameSlot(slot.index);
		// The host's own executions this frame (Light Limit Fix's epoch between the depth and colour sites) wait for their submissions
		// alone: what this frame staged and retired is released after its own signal.
		host->NoteExternalSlotUse(slot.index, s.timeline, s.signalled + 1);
		// Then, in this order before FrameValues::Kick: the movers' live spheres into this set, and the CPU frame state armed with it.
		FrameValues::Get().SetViewBatches(a_set);
		KickCpuFrameState(std::move(a_set));
	}

	const ViewBatchSet* FrameBatches()
	{
		const auto* slot = S().current;
		return slot ? slot->set.get() : nullptr;
	}

	void EndFrame()
	{
		auto& s = S();
		auto* slot = std::exchange(s.current, nullptr);
		if (!slot)
			return;
		// The frame's signal behind its sites' callbacks in DXVK's stream: the slot's (and the clock's) retirement point.
		const std::uint64_t value = s.signalled + 1;
		const RenderGraphRuntime::ExternalCommandsWait signals[1]{ { s.semaphore, value, 0 } };
		if (RenderGraphRuntime::Get().EmitExternalCommands(&SignalCallback, nullptr, {}, signals, false, "DCLF CPU frame")) {
			s.signalled = value;
			slot->signal = value;
			NoteCpuFrameSignalled(value);
		} else {
			// Without DXVK's external commands no site queued anything either: nothing of the slot is in flight.
			++s.window.notQueued;
		}
		// The view batches' sequences, bound with the payload's (the point covers this frame's signal).
		if (slot->set)
			Draws::BindViewBatches(*slot->set, SubmittedRetirementPoint());
		if (auto* host = RenderGraphRuntime::Get().Host())
			host->EndExternalFrameSlot();
		++s.window.frames;
	}

	bool ExecuteEpoch(RenderGraphRuntime::Segment a_segment, const std::function<void()>& a_body, std::shared_ptr<const void> a_owner)
	{
		auto& s = S();
		if (!CpuFrame() || !s.current)
			return RenderGraphRuntime::Get().ExecuteEpoch(a_segment, [&](org::RenderGraph&) { a_body(); }, std::move(a_owner));
		s.pending = nullptr;
		{
			// The body runs with the graph's upload and descriptor services active, as ORG's epochs run theirs: the commits stage their
			// uploads through them (CommitUploads), and Emit records the ones pending.
			auto* host = RenderGraphRuntime::Get().Host();
			auto* graph = host ? host->Graph() : nullptr;
			if (!graph)
				return false;
			org::runtime::ScopedActiveGraphServices services(graph->GetUploadService(), graph->GetDescriptorService());
			a_body();
		}
		// What the execution reads, held as ORG would hold it until it retires.
		s.current->holds.push_back(std::move(a_owner));
		if (auto* run = std::exchange(s.pending, nullptr))
			return Emit(*run);
		return true;
	}

	void DepthCommitted(const MainCommitInputs& a_inputs)
	{
		auto& s = S();
		auto* slot = s.current;
		if (!slot || !slot->set || !a_inputs.shape || !a_inputs.resources || !a_inputs.latch)
			return;
		TakePoolFirsts(*slot, a_inputs.resources->pool.get());
		auto& run = NewRun(*slot, Site::Depth, a_inputs);
		Draws::RevisionShapes shapes;
		shapes.main[Draws::kDepthShape] = a_inputs.shape;
		shapes.depthWithoutIndexPool = a_inputs.poolCopies == 0;
		// The hosted views committed in this epoch (ViewSites.cpp): their shapes for the pre-work, their views culled with main's.
		AttachHostedViews(*slot, run, shapes);
		const PreWorkInputs prework{ a_inputs.adopted, a_inputs.resources, nullptr, &shapes, a_inputs.latchSlot };
		run.prework = PrepareDepthPreWork(prework);
		auto& main = NewView(*slot, run, ViewType::Main, MainCullView(*a_inputs.latch), true);
		if (run.valid)
			SetMainSecondary(run, main);
		run.main = &main;
		// The main list's depth batches, for the colour site's words; the colour gate's depth arrival once they are all culled.
		slot->mainDepthCount = main.list->size();
		slot->mainDepth = std::make_unique<std::atomic<const CulledBatch*>[]>(slot->mainDepthCount);
		slot->mainDepthPending.store(static_cast<std::uint32_t>(slot->mainDepthCount), std::memory_order_relaxed);
		slot->depthRan = true;
		if (!slot->mainDepthCount)
			ArriveColour(*slot);
		OnCullFrame(*slot, [slot, run = &run](const std::shared_ptr<const CullFrame>& a_frame) {
			run->cullFrame = a_frame;
			for (auto* view : run->views)
				DispatchCulls(*slot, *run, *view, a_frame);
		});
		SetPending(run);
	}

	void ColourCommitted(const MainCommitInputs& a_inputs)
	{
		auto& s = S();
		auto* slot = s.current;
		if (!slot || !slot->set || !a_inputs.shape || !a_inputs.resources || !a_inputs.latch)
			return;
		TakePoolFirsts(*slot, a_inputs.resources->pool.get());
		auto& run = NewRun(*slot, Site::Colour, a_inputs);
		Draws::RevisionShapes shapes;
		shapes.main[Draws::kColourShape] = a_inputs.shape;
		const PreWorkInputs prework{ a_inputs.adopted, a_inputs.resources, nullptr, &shapes, a_inputs.latchSlot };
		run.prework = PrepareColourPreWork(prework);
		const auto& latch = *a_inputs.latch;
		// The colour views read the cascades and volumes the commit latched: copied, held by the slot for the culls' duration.
		auto cascades = std::make_shared<const std::vector<Draws::GpuCascade>>(a_inputs.cascades.begin(), a_inputs.cascades.end());
		auto volumes = std::make_shared<const std::vector<Draws::GpuShadowVolume>>(a_inputs.volumes.begin(), a_inputs.volumes.end());
		// The local shadow volumes' strict gates (a portal-strict point light's volume), held with them.
		auto strict = a_inputs.resources->scene ? FrameStrictGates(*a_inputs.resources->scene, slot->sceneFrame) : nullptr;
		const auto colourView = [&](ViewType a_type) {
			auto view = ColourCullView(a_type, latch, *cascades, *volumes);
			view.strict = strict.get();
			return view;
		};
		if (strict)
			slot->holds.push_back(strict);
		slot->holds.push_back(cascades);
		slot->holds.push_back(volumes);
		auto& main = NewView(*slot, run, ViewType::Main, colourView(ViewType::Main), false);
		main.colourWords = true;
		main.mainView = MainCullView(latch);
		if (run.valid)
			SetMainSecondary(run, main);
		run.main = &main;
		if (!slot->set->List(ViewType::Decal).empty())
			run.decal = &NewView(*slot, run, ViewType::Decal, colourView(ViewType::Decal), false);
		// The forward site's views are this commit's (Features/Forward's epoch shares the colour shape); its callback is queued at the
		// forward trigger (ForwardReached).
		auto& forward = NewRun(*slot, Site::Forward, a_inputs);
		forward.forward = &NewView(*slot, forward, ViewType::Forward, colourView(ViewType::Forward), false);
		forward.refractionView = &NewView(*slot, forward, ViewType::Refraction, colourView(ViewType::Refraction), false);
		slot->forwardRun = &forward;
		// The main colour culls wait for the depth site's batches (their lanes) and this frame's cull frame: the colour gate.
		slot->colourDispatch = [slot, run = &run] { DispatchCulls(*slot, *run, *run->main, run->cullFrame); };
		OnCullFrame(*slot, [slot, run = &run, forward = &forward](const std::shared_ptr<const CullFrame>& a_frame) {
			run->cullFrame = a_frame;
			forward->cullFrame = a_frame;
			if (run->decal)
				DispatchCulls(*slot, *run, *run->decal, a_frame);
			DispatchCulls(*slot, *forward, *forward->forward, a_frame);
			DispatchCulls(*slot, *forward, *forward->refractionView, a_frame);
			ArriveColour(*slot);
		});
		// No depth site this frame: nothing to wait for (the colour culls cull the main list themselves).
		if (!slot->depthRan)
			ArriveColour(*slot);
		s.pending = &run;
	}

	bool ForwardReached(bool a_refraction)
	{
		auto& s = S();
		auto* slot = s.current;
		auto* run = slot ? std::exchange(slot->forwardRun, nullptr) : nullptr;
		if (!run)
			return false;
		// The forward and refraction lanes' rows, for their sort on the CS thread (the producer's rows change at the next Kick). The
		// producer is done this late in the frame; a frame whose is not waits for it here (counted).
		const auto* rows = FrameValues::Get().RowsIfDone();
		if (!rows) {
			++s.window.rowWaits;
			rows = FrameValues::Get().RowsIfDone(true);
		}
		if (rows) {
			if (slot->forwardRows.size() < rows->size())
				slot->forwardRows.resize(rows->size());
			for (const ViewType type : { ViewType::Forward, ViewType::Refraction })
				for (const auto& batch : slot->set->List(type))
					for (std::uint32_t lane = 0; lane < batch.count; ++lane)
						if (const std::uint32_t object = batch.object[lane]; object < rows->size())
							slot->forwardRows[object] = (*rows)[object];
			run->bindings.placements = slot->forwardRows;
			run->refractionBindings.placements = slot->forwardRows;
		}
		run->bindings.viewProj = run->forward->view.viewProj;
		run->refractionBindings.viewProj = run->refractionView->view.viewProj;
		run->refraction = a_refraction;
		s.pending = run;
		return true;
	}

	WindowCounters TakeWindowCounters()
	{
		return std::exchange(S().window, {});
	}
}
