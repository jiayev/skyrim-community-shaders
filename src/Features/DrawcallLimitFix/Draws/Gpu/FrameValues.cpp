// FrameValues (see the header): the render thread's Kick and the producers it dispatches onto the preparation pool, one at a time.

#include "FrameValues.h"
#include "Features/DrawcallLimitFix/Scene/FrameView/FrameView.h"
#include "Features/DrawcallLimitFix/Engine/Access/EngineAccess.h"

#include "Features/DrawcallLimitFix/Runtime/EngineReleases.h"

#include "Features/DrawcallLimitFix/Runtime/KeptState.h"
#include "Features/DrawcallLimitFix/Engine/Capture/Billboards.h"
#include "Features/DrawcallLimitFix/Engine/Capture/ParticleSnapshots.h"
#include "Features/DrawcallLimitFix/Engine/Views/ShadowViews.h"
#include "Features/DrawcallLimitFix/Scene/Ports/LightingConstants.h"
#include "Features/DrawcallLimitFix/Scene/Ports/FadeState.h"
#include "Features/DrawcallLimitFix/Scene/Ports/FrameGlobals.h"
#include "Features/DrawcallLimitFix/Scene/Ports/LightingDescriptors.h"

#include <bit>
#include <cmath>
#include <optional>
#include <cstring>
#include <limits>

#if defined(CS_HAS_RENDER_GRAPH) && defined(CS_HAS_ORG_MODULE_SERVICES)
#	include "Features/DrawcallLimitFix/Runtime/SceneScheduler.h"
#	include "Features/DrawcallLimitFix/Runtime/Switches.h"
#	include "Features/DrawcallLimitFix/Engine/Access/EngineReadWindow.h"
#	include "RenderGraph/RenderGraphRuntime.h"
#	include <OpenRenderGraph/PersistentGraphHost.h>
#	include <Render/Runtime/IUploadService.h>
#	include <Resources/Buffers/Buffer.h>
#	include <ORGModuleServices/GrowthPolicy.h>
#	include <ORGModuleServices/TransportBuffer.h>
#	include <rhi.h>

#	include <algorithm>
#	include <chrono>
#endif

namespace DCLF
{
	namespace
	{
		// First-person particle expansion counters, for the first-person report.
		std::atomic<std::uint64_t> firstPersonParticleExpansions{ 0 }, firstPersonParticleCount{ 0 };
		std::atomic<std::uint32_t> firstPersonParticleFarthest{ 0 };  // non-negative float bits (ordered as the float)

		// Row-major 3x4, as the shaders read a transform.
		void StoreTransform(const RE::NiTransform& a_transform, float (&a_out)[12])
		{
			const auto& r = a_transform.rotate.entry;
			const float s = a_transform.scale;
			for (int row = 0; row < 3; ++row) {
				a_out[row * 4 + 0] = r[row][0] * s;
				a_out[row * 4 + 1] = r[row][1] * s;
				a_out[row * 4 + 2] = r[row][2] * s;
			}
			a_out[3] = a_transform.translate.x;
			a_out[7] = a_transform.translate.y;
			a_out[11] = a_transform.translate.z;
		}

		// The node's bound, or unbounded (radius +max, inside every process).
		void StoreSunEntry(const RE::NiAVObject* a_node, float (&a_out)[4])
		{
			if (a_node && a_node->worldBound.radius >= 0.0f) {
				const auto& bound = a_node->worldBound;
				a_out[0] = bound.center.x;
				a_out[1] = bound.center.y;
				a_out[2] = bound.center.z;
				a_out[3] = bound.radius;
			} else {
				a_out[0] = a_out[1] = a_out[2] = 0.0f;
				a_out[3] = std::numeric_limits<float>::max();
			}
		}

		void StoreFadeNode(const RE::BSShaderProperty* a_property, float (&a_out)[4])
		{
			const auto node = LodFadeNodeOf(a_property);
			std::memcpy(a_out, node.data(), sizeof(a_out));
		}

		/** @brief a_geometry's depth-first index among a_node's geometries (null slots skipped); false if not found. */
		bool OrdinalUnder(const RE::NiNode& a_node, const RE::BSGeometry& a_geometry, std::uint32_t& a_index)
		{
			for (const auto& child : a_node.GetChildren()) {
				auto* object = child.get();
				if (!object)
					continue;
				if (object == &a_geometry)
					return true;
				if (object->AsGeometry())
					++a_index;
				else if (const auto* node = object->AsNode(); node && OrdinalUnder(*node, a_geometry, a_index))
					return true;
			}
			return false;
		}

		// The ordered-node interleave (BindlessPlacement::forwardSort; engine walk in drawcall-limit-fix.md): a blended leaf's nearest
		// BSOrderedNode (exact type, FUN_1415039f0) opens a group whose children draw in child order; the groups are sorted by the
		// node's world-bound centre depth (comparator 0x1414f55e0) and interleaved with the blended list. Only blended leaves join a
		// group (FUN_1414b2330), so an opaque leaf's row stays 0. The bound is live unless kFixedBound (0x141503a60). Read inside the
		// caller's engine-read lease.
		void StoreForwardSort(const RE::BSGeometry& a_geometry, float (&a_out)[4])
		{
			a_out[0] = a_out[1] = a_out[2] = a_out[3] = 0.0f;
			const auto& data = a_geometry.GetGeometryRuntimeData();
			const auto* alpha = data.alphaProperty.get();
			bool blended = alpha && (alpha->alphaFlags & 1);
			if (!blended)
				if (const auto* lighting = netimmerse_cast<RE::BSLightingShaderProperty*>(data.shaderProperty.get()); lighting && lighting->material)
					blended = static_cast<const RE::BSLightingShaderMaterialBase*>(lighting->material)->materialAlpha < 1.0f;
			if (!blended)
				return;
			static const REL::Relocation<const RE::NiRTTI*> orderedRtti{ RE::BSOrderedNode::Ni_RTTI };
			const RE::NiNode* ordered = nullptr;
			for (auto* node = a_geometry.parent; node; node = node->parent)
				if (node->GetRTTI() == orderedRtti.get()) {
					ordered = node;
					break;
				}
			std::uint32_t index = 0;
			if (!ordered || !OrdinalUnder(*ordered, a_geometry, index))
				return;
			const auto& bound = ordered->worldBound;
			a_out[0] = bound.center.x;
			a_out[1] = bound.center.y;
			a_out[2] = bound.center.z;
			a_out[3] = std::bit_cast<float>(index + 1);
		}
	}

	FrameValues::FirstPersonParticleStats FrameValues::TakeFirstPersonParticleStats()
	{
		FirstPersonParticleStats out;
		out.expansions = firstPersonParticleExpansions.exchange(0, std::memory_order_relaxed);
		out.particles = firstPersonParticleCount.exchange(0, std::memory_order_relaxed);
		out.farthest = std::bit_cast<float>(firstPersonParticleFarthest.exchange(0, std::memory_order_relaxed));
		return out;
	}

	void FrameValues::SampleRow(const RE::BSGeometry& a_geometry, const RE::NiAVObject* a_sunEntryNode, const RE::BSShaderProperty* a_fadeProperty,
		BindlessPlacement& a_out)
	{
		StoreTransform(a_geometry.world, a_out.world);
		// Render flag 0x10: the previous transform is the current one (engine notes: SetupGeometry).
		if (GeometryPort::kMainPassRenderFlags & 0x10)
			std::memcpy(a_out.previousWorld, a_out.world, sizeof(a_out.world));
		else
			StoreTransform(DrawnPreviousWorld(a_geometry), a_out.previousWorld);
		a_out.bound[0] = a_geometry.worldBound.center.x;
		a_out.bound[1] = a_geometry.worldBound.center.y;
		a_out.bound[2] = a_geometry.worldBound.center.z;
		a_out.bound[3] = a_geometry.worldBound.radius;
		StoreSunEntry(a_sunEntryNode, a_out.sunEntry);
		StoreFadeNode(a_fadeProperty, a_out.lodFadeNode);
		StoreForwardSort(a_geometry, a_out.forwardSort);
		// A billboard's unturned world, base and pivot (Billboards.h), sampled regardless of the toggle so rows never depend on it.
		Billboards::SampleRow(a_geometry, a_out);
	}

	bool FrameValues::SampleSlot(const SceneTables& a_tables, std::uint32_t a_slot, BindlessPlacement& a_out)
	{
		const auto* geometry = a_slot < a_tables.objectGeometry.size() ? a_tables.objectGeometry[a_slot] : nullptr;
		if (!geometry)
			return false;
		// A layer's row is its base's placement with its layer property's fade node (WriteLayer).
		const RE::BSShaderProperty* fadeProperty = a_tables.IsLayer(a_slot) ? LayerPropertyOf(*geometry) : geometry->GetGeometryRuntimeData().shaderProperty.get();
		SampleRow(*geometry, a_slot < a_tables.sunEntryNode.size() ? a_tables.sunEntryNode[a_slot] : nullptr, fadeProperty, a_out);
		return true;
	}

#if defined(CS_HAS_RENDER_GRAPH) && defined(CS_HAS_ORG_MODULE_SERVICES)
	namespace
	{
		using Plan = PlacementPlan;
		using PaletteRow = FrameValues::PaletteRow;

		/**
		 * @brief The engine's per-frame palette update (AE FUN_140e4ff90), which the native draw's bone setter runs and an owned skin
		 * misses. Idempotent per frame (frameID) under the skin's critical section, so any thread may run it; copies current to
		 * previous, then writes three float4 rows per bone in absolute world space.
		 */
		void UpdateSkin(RE::NiSkinInstance* a_skin, const RE::NiTransform& a_world)
		{
			using UpdateSkinInstance = void (*)(RE::NiSkinInstance*, const RE::NiTransform*);
			static const REL::Relocation<UpdateSkinInstance> updateSkinInstance{ REL::Offset(Engine::Address::kUpdateSkinInstance) };
			updateSkinInstance(a_skin, &a_world);
		}

		void SampleItem(const Plan::Item& a_item, BindlessPlacement& a_row, BindlessPlacement& a_layer)
		{
			const auto& geometry = *a_item.geometry;
			FrameValues::SampleRow(geometry, a_item.sunEntryNode.get(), geometry.GetGeometryRuntimeData().shaderProperty.get(), a_row);
			if (a_item.layerSlot != kNoObjectSlot) {
				a_layer = a_row;
				StoreFadeNode(LayerPropertyOf(geometry), a_layer.lodFadeNode);
			}
		}
	}

	// The skin's palettes after this frame's update; false when its size mismatches its block (the walk rewrites it: KeepSkin).
	bool FrameValues::SkinPalettes(const RE::BSGeometry& a_geometry, std::uint32_t a_rows, const float*& a_current, const float*& a_previous)
	{
		auto* skin = a_geometry.GetGeometryRuntimeData().skinInstance.get();
		if (!skin)
			return false;
		a_current = static_cast<const float*>(skin->boneMatrices);
		a_previous = static_cast<const float*>(skin->prevBoneMatrices);
		return a_current && a_previous && skin->numMatrices * 3 == a_rows;
	}

	struct FrameValues::Impl
	{
		// Render thread.
		using RingBuffer = org::services::TransportBuffer;
		std::array<RingBuffer, kRing> ring, paletteRing, shadingRing, seedRing, treeSeedRing;
		std::array<org::PersistentGraphHost::GpuPoint, kRing> points;  // per ring entry: the last frame that read it
		std::shared_ptr<const Plan> plan;                              // the newest
		std::vector<std::shared_ptr<const Plan>> unsampled;           // plans whose written slots no producer has sampled yet
		std::vector<ShadingItem> unsampledShading;        // not yet taken by a producer
		std::vector<WetnessValue> unsampledWetness;
		std::vector<std::pair<std::uint32_t, FadeRootStatic>> unsentSeeds;  // by FadeSeedRow, not yet written
		std::uint32_t seedRows = 0;                                     // highest named FadeSeedRow + 1
		std::vector<std::pair<std::uint32_t, TreeStatic>> unsentTreeSeeds;
		std::uint32_t treeSeedRows = 0;
		std::deque<std::pair<std::uint64_t, std::shared_ptr<const Plan>>> plans;  // held per kicked producer's sequence
		std::uint64_t seq = 0;                                          // the last kicked
		bool kicked = false;                                            // this frame
		bool unavailableLogged = false;

		// The producers', one at a time in sequence order.
		KeptArray<BindlessPlacement> rows;
		KeptArray<PaletteRow> palettes;
		KeptArray<BindlessShading> shading;
		KeptArray<FadeRootStatic> seeds;  // by FadeSeedRow
		KeptArray<TreeStatic> treeSeeds;  // by 2 * tree slot + seedOdd
		std::array<std::uint64_t, kRing> held{}, paletteHeld{}, shadingHeld{}, seedsHeld{}, treeSeedsHeld{};  // per ring entry: versions held
		std::vector<std::uint8_t> listed;                       // dedup marks while listing items: per slot
		std::vector<std::uint8_t> blockListed;                  // per palette block
		std::vector<std::uint8_t> shadingListed;                // per slot, shading items
		// Written slots whose previous transform still differs from the current (a static's first update): resampled until equal.
		std::vector<Plan::Item> settling;
		std::atomic<std::uint64_t> done{ 0 };

		// Since the last report.
		std::atomic<std::uint64_t> frames{ 0 }, sampled{ 0 }, rowsChanged{ 0 }, skins{ 0 }, paletteRowsChanged{ 0 }, shadingSampled{ 0 }, shadingChanged{ 0 },
			wetnessChanged{ 0 }, defects{ 0 }, runs{ 0 }, bytes{ 0 }, refused{ 0 }, failures{ 0 }, pointWaits{ 0 }, settled{ 0 }, seeded{ 0 };
		std::atomic<std::uint64_t> sampleUs{ 0 }, pointWaitUs{ 0 }, uploadUs{ 0 }, maxTotalUs{ 0 };

		/**
		 * @brief A first-person member's rows and palettes, sampled by the render thread at Kick, before Main::Draw moves the
		 * skeleton into its root's space.
		 */
		struct FirstPersonSample
		{
			std::uint32_t slot = kNoObjectSlot, layerSlot = kNoObjectSlot, boneOffset = 0, boneRows = 0;
			BindlessPlacement row{}, layer{};
			std::vector<PaletteRow> current, previous;
		};

		struct Job
		{
			std::uint64_t seq = 0;
			const Plan* plan = nullptr;
			std::vector<const Plan*> written;  // plans whose written slots it also samples (oldest first)
			std::vector<ShadingItem> shadingItems;  // newest last
			std::vector<WetnessValue> wetness;      // in capture order
			std::vector<std::pair<std::uint32_t, FadeRootStatic>> seedValues;  // taken at the frame's start, in request order
			std::uint32_t seedRows = 0;
			std::vector<std::pair<std::uint32_t, TreeStatic>> treeSeedValues;
			std::uint32_t treeSeedRows = 0;
			std::shared_ptr<const FrameGlobals> globals;  // the frame's engine globals (shading rows' tree wind)
			std::uint32_t ringIndex = 0;
			std::shared_ptr<org::Buffer> target, paletteTarget, shadingTarget, seedTarget, treeSeedTarget;
			std::uint32_t capacity = 0, paletteCapacity = 0, shadingCapacity = 0, seedCapacity = 0, treeSeedCapacity = 0;  // in rows
			bool fresh = false, paletteFresh = false, shadingFresh = false, seedFresh = false, treeSeedFresh = false;      // new buffer, holds nothing
			org::PersistentGraphHost::GpuPoint reuse;
			std::shared_ptr<rhi::TimelinePtr> timeline;
			std::shared_ptr<org::runtime::IUploadService> uploads;
			FrameUploads more;  // the frame's other uploads (Kick's a_uploads)
			std::vector<FirstPersonSample> firstPerson;  // the render thread's samples (Kick)
			ParticleSnapshots::Camera particleCamera;    // the main camera particle quads face
			// The camera first-person particle quads face: the main camera's rotation (the first-person camera's, FUN_1414cb020)
			// at the first-person eye (DrawWorld +0x00).
			ParticleSnapshots::Camera firstPersonParticleCamera;
		};
		void Run(const Job& a_job);
		void Sample(const Job& a_job);
		void SampleShadingRows(const Job& a_job, std::size_t a_slots);
		void SampleFadeSeeds(const Job& a_job);
		void Upload(const Job& a_job);
		template <class T>
		void Send(const Job& a_job, const KeptArray<T>& a_rows, std::uint64_t& a_holds, bool a_fresh, const std::shared_ptr<org::Buffer>& a_target, std::uint32_t a_capacity);
	};

	void FrameValues::Impl::Run(const Job& a_job)
	{
		ZoneScopedN("CS.DCLF.FrameValues");
		// One producer at a time, in order: each builds on the previous one's rows, and signals rise monotonically.
		for (auto finished = done.load(std::memory_order_acquire); finished + 1 < a_job.seq; finished = done.load(std::memory_order_acquire))
			done.wait(finished, std::memory_order_acquire);
		const auto start = std::chrono::steady_clock::now();
		try {
			Sample(a_job);
			const auto sampledAt = std::chrono::steady_clock::now();
			sampleUs += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(sampledAt - start).count());
			// Wait for the buffers' last readers, kRing frames back (almost always done).
			if (!a_job.reuse.Reached()) {
				ZoneScopedN("CS.DCLF.FrameValues.WaitBuffer");
				++pointWaits;
				if (!a_job.reuse.Wait(10000))
					throw std::runtime_error("the frame that last read its buffers did not complete on the GPU");
			}
			const auto reusable = std::chrono::steady_clock::now();
			pointWaitUs += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(reusable - sampledAt).count());
			Upload(a_job);
			if (a_job.more) {
				try {
					a_job.more(*a_job.uploads);
				} catch (const std::exception& e) {
					if (failures++ == 0)
						logger::error("[DCLF] frame values {}: the frame's other uploads failed: {}", a_job.seq, e.what());
				}
			}
			uploadUs += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - reusable).count());
		} catch (const std::exception& e) {
			if (failures++ == 0)
				logger::error("[DCLF] frame values {} failed: {}; the frame draws the rows its buffers held", a_job.seq, e.what());
		}
		// Always signal: every batch of the frame waits for it.
		if (!a_job.uploads || !a_job.uploads->QueueStreamingSignal(a_job.timeline, a_job.seq))
			stl::report_and_fail(fmt::format("Drawcall Limit Fix: frame values {} could not signal the frame's wait; the GPU would wait for it forever", a_job.seq));
		const auto total = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
		for (auto max = maxTotalUs.load(std::memory_order_relaxed); total > max && !maxTotalUs.compare_exchange_weak(max, total, std::memory_order_relaxed);) {}
		++frames;
		done.store(a_job.seq, std::memory_order_release);
		done.notify_all();
	}

	void FrameValues::Impl::Sample(const Job& a_job)
	{
		ZoneScopedN("CS.DCLF.FrameValues.Sample");
		const auto& work = *a_job.plan;
		const auto oldestOf = [](const std::array<std::uint64_t, kRing>& a_held) { return *std::min_element(a_held.begin(), a_held.end()); };
		rows.BeginBuild(oldestOf(held));
		palettes.BeginBuild(oldestOf(paletteHeld));
		auto& out = rows.Mutable();
		if (out.size() < work.slots) {
			const std::size_t first = out.size();
			out.resize(work.slots, BindlessPlacement{});
			rows.MarkRange(first, work.slots - first);
		}
		// Two row sets per block: current and previous (PaletteRowsOf).
		auto& paletteOut = palettes.Mutable();
		if (const std::size_t paletteRows = 2ull * work.boneCapacity; paletteOut.size() < paletteRows) {
			const std::size_t first = paletteOut.size();
			paletteOut.resize(paletteRows, PaletteRow{});
			palettes.MarkRange(first, paletteRows - first);
		}
		// Each slot and palette block once, by its newest item.
		struct Listed
		{
			const Plan::Item* item;
			bool palette;
			bool settles = false;  // a written slot (not a mover): kept while its transforms differ
		};
		std::vector<Listed> items;
		items.reserve(work.movers.size() + (a_job.written.empty() ? 0 : a_job.written.back()->written.size()));
		listed.assign(out.size(), 0);
		blockListed.assign(paletteOut.size() / 2, 0);
		// First-person items are the render thread's (FirstPersonSample), except particle members (expanded from snapshots).
		auto list = [&](const Plan::Item& a_item, bool a_settles) {
			if ((a_item.firstPerson && !a_item.particles) || a_item.slot >= out.size() || !a_item.geometry || std::exchange(listed[a_item.slot], 1))
				return;
			const bool palette = a_item.boneRows && std::size_t(a_item.boneOffset) + a_item.boneRows <= blockListed.size() && !std::exchange(blockListed[a_item.boneOffset], 1);
			items.push_back({ &a_item, palette, a_settles });
		};
		for (const auto& item : work.movers)
			list(item, false);
		for (auto it = a_job.written.rbegin(); it != a_job.written.rend(); ++it)
			for (const auto& item : (*it)->written)
				list(item, true);
		// Carried settling items last (a slot listed above is newer). They may hold the last engine references, so they go to the
		// render thread when done (EngineReleases, at Present), never dropped here.
		auto carried = std::move(settling);
		settling.clear();
		struct HandOver
		{
			std::vector<Plan::Item>& items;
			~HandOver()
			{
				for (auto& item : items) {
					EngineReleases::Push(RE::NiPointer<RE::NiRefObject>(std::move(item.geometry)));
					EngineReleases::Push(RE::NiPointer<RE::NiRefObject>(std::move(item.sunEntryNode)));
				}
			}
		} handOver{ carried };
		for (const auto& item : carried)
			list(item, true);
		std::uint64_t firstPersonMarked = 0, firstPersonPaletteMarked = 0;
		for (const auto& sample : a_job.firstPerson) {
			if (sample.slot >= out.size())
				continue;
			if (std::memcmp(&out[sample.slot], &sample.row, sizeof(sample.row)) != 0) {
				out[sample.slot] = sample.row;
				rows.Mark(sample.slot);
				++firstPersonMarked;
			}
			if (sample.layerSlot != kNoObjectSlot && sample.layerSlot < out.size() && std::memcmp(&out[sample.layerSlot], &sample.layer, sizeof(sample.layer)) != 0) {
				out[sample.layerSlot] = sample.layer;
				rows.Mark(sample.layerSlot);
				++firstPersonMarked;
			}
			if (!sample.boneRows || sample.current.size() != sample.boneRows || sample.previous.size() != sample.boneRows)
				continue;
			const auto at = PaletteRowsOf(sample.boneOffset, sample.boneRows);
			if (std::size_t(at.previous) + sample.boneRows > paletteOut.size() || std::size_t(at.current) + sample.boneRows > paletteOut.size())
				continue;
			const std::size_t paletteBytes = std::size_t(sample.boneRows) * sizeof(PaletteRow);
			if (std::memcmp(&paletteOut[at.current], sample.current.data(), paletteBytes) != 0 || std::memcmp(&paletteOut[at.previous], sample.previous.data(), paletteBytes) != 0) {
				std::memcpy(&paletteOut[at.current], sample.current.data(), paletteBytes);
				std::memcpy(&paletteOut[at.previous], sample.previous.data(), paletteBytes);
				palettes.MarkRange(at.current, 2 * sample.boneRows);
				firstPersonPaletteMarked += 2 * sample.boneRows;
			}
		}
		auto& executor = SceneScheduler::Executor();
		constexpr std::size_t kGrain = 64;
		// Per chunk, marked afterwards (the journals are single-threaded).
		struct Changed
		{
			std::vector<std::uint32_t> slots;
			std::vector<std::pair<std::uint32_t, std::uint32_t>> paletteRuns;  // (first row, rows)
			std::vector<const Plan::Item*> settling;
			std::uint64_t skins = 0, defects = 0;
		};
		std::vector<Changed> changed((items.size() + kGrain - 1) / kGrain);
		std::atomic<std::uint64_t> refusedHere{ 0 };
		// Particle records, before the parallel loop (ParticleSnapshots::Follow is producer-only).
		std::vector<ParticleSnapshots::Record*> particleRecords(items.size(), nullptr);
		for (std::size_t i = 0; i < items.size(); ++i)
			if (items[i].palette && items[i].item->particles)
				particleRecords[i] = ParticleSnapshots::Get().Follow(items[i].item->geometry.get());
		executor.ParallelFor("CS.DCLF.FrameValues.Items", items.size(), kGrain, [&](std::size_t a_begin, std::size_t a_end) {
			ZoneScopedN("CS.DCLF.FrameValues.Items");
			auto& mine = changed[a_begin / kGrain];
			BindlessPlacement row, layer;
			std::vector<PaletteRow> particleRows;
			for (std::size_t i = a_begin; i < a_end; ++i) {
				const auto& [itemPointer, palette, settles] = items[i];
				const auto& item = *itemPointer;
				// A first-person particle member reads no engine memory here: no lease, never settling.
				const bool firstPerson = item.firstPerson;
				std::optional<EngineReadWindow::Lease> lease;
				if (!firstPerson)
					lease.emplace();
				if (lease && !*lease) {
					++refusedHere;
					if (settles)
						mine.settling.push_back(&item);  // not sampled: the next producer takes it
					continue;
				}
				if (firstPerson)
					row = out[item.slot];
				else
					SampleItem(item, row, layer);
				// Particle quads, transforms and bound from the newest snapshot (particle data is never read here).
				auto* particles = particleRecords[i];
				if (particles) {
					particleRows.resize(item.boneRows);
					ParticleSnapshots::Expanded expanded;
					ParticleSnapshots::Get().Expand(*particles, firstPerson ? a_job.firstPersonParticleCamera : a_job.particleCamera,
						item.boneRows / ParticleSnapshots::kRowsPerParticle, particleRows.data()->v, expanded);
					if (expanded.snapshot) {
						std::memcpy(row.world, expanded.world, sizeof(row.world));
						std::memcpy(row.previousWorld, expanded.world, sizeof(row.previousWorld));
						std::memcpy(row.bound, expanded.bound, sizeof(row.bound));
						if (firstPerson) {
							const auto& eye = a_job.firstPersonParticleCamera.eye;
							const float dx = expanded.bound[0] - eye[0], dy = expanded.bound[1] - eye[1], dz = expanded.bound[2] - eye[2];
							const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
							firstPersonParticleExpansions.fetch_add(1, std::memory_order_relaxed);
							firstPersonParticleCount.fetch_add(expanded.particles, std::memory_order_relaxed);
							const std::uint32_t bits = std::bit_cast<std::uint32_t>(distance);
							for (std::uint32_t seen = firstPersonParticleFarthest.load(std::memory_order_relaxed);
								bits > seen && !firstPersonParticleFarthest.compare_exchange_weak(seen, bits, std::memory_order_relaxed);) {}
						}
					}
				}
				if (settles && !firstPerson && std::memcmp(&item.geometry->world, &DrawnPreviousWorld(*item.geometry), sizeof(RE::NiTransform)) != 0)
					mine.settling.push_back(&item);
				if (std::memcmp(&out[item.slot], &row, sizeof(row)) != 0) {
					out[item.slot] = row;
					mine.slots.push_back(item.slot);
				}
				if (!firstPerson && item.layerSlot != kNoObjectSlot && item.layerSlot < out.size() && std::memcmp(&out[item.layerSlot], &layer, sizeof(layer)) != 0) {
					out[item.layerSlot] = layer;
					mine.slots.push_back(item.layerSlot);
				}
				if (!palette)
					continue;
				if (particles) {
					const auto at = PaletteRowsOf(item.boneOffset, item.boneRows);
					const std::size_t bytes = std::size_t(item.boneRows) * sizeof(PaletteRow);
					if (std::memcmp(&paletteOut[at.current], particleRows.data(), bytes) != 0) {
						std::memcpy(&paletteOut[at.current], particleRows.data(), bytes);
						mine.paletteRuns.emplace_back(at.current, item.boneRows);
					}
					continue;
				}
				// Run the engine's palette update, then copy rows that differ.
				auto& geometry = *item.geometry;
				if (auto* skin = geometry.GetGeometryRuntimeData().skinInstance.get())
					UpdateSkin(skin, geometry.world);
				const float* current = nullptr;
				const float* previous = nullptr;
				if (!SkinPalettes(geometry, item.boneRows, current, previous)) {
					++mine.defects;
					continue;
				}
				++mine.skins;
				const auto at = PaletteRowsOf(item.boneOffset, item.boneRows);
				const std::size_t bytes = std::size_t(item.boneRows) * sizeof(PaletteRow);
				if (std::memcmp(&paletteOut[at.current], current, bytes) != 0 || std::memcmp(&paletteOut[at.previous], previous, bytes) != 0) {
					std::memcpy(&paletteOut[at.current], current, bytes);
					std::memcpy(&paletteOut[at.previous], previous, bytes);
					mine.paletteRuns.emplace_back(at.current, 2 * item.boneRows);
				}
			}
		});
		ParticleSnapshots::Get().EndRun();
		// Roots: their dependents' sun entries and fade nodes, after the items wrote theirs.
		std::vector<std::vector<std::uint32_t>> rootChanged(work.roots.size());
		executor.ParallelFor("CS.DCLF.FrameValues.Roots", work.roots.size(), 16, [&](std::size_t a_begin, std::size_t a_end) {
			ZoneScopedN("CS.DCLF.FrameValues.Roots");
			for (std::size_t r = a_begin; r < a_end; ++r) {
				const auto& root = work.roots[r];
				EngineReadWindow::Lease lease;
				if (!lease) {
					++refusedHere;
					continue;
				}
				float entry[4];
				StoreSunEntry(root.root.get(), entry);
				for (const auto& dependent : root.dependents) {
					if (dependent.firstPerson || dependent.slot >= out.size() || !dependent.geometry)
						continue;
					const auto write = [&](std::uint32_t a_slot, const RE::BSShaderProperty* a_property) {
						auto& row = out[a_slot];
						float fade[4];
						StoreFadeNode(a_property, fade);
						if (std::memcmp(row.sunEntry, entry, sizeof(entry)) == 0 && std::memcmp(row.lodFadeNode, fade, sizeof(fade)) == 0)
							return;
						std::memcpy(row.sunEntry, entry, sizeof(entry));
						std::memcpy(row.lodFadeNode, fade, sizeof(fade));
						rootChanged[r].push_back(a_slot);
					};
					write(dependent.slot, dependent.geometry->GetGeometryRuntimeData().shaderProperty.get());
					if (dependent.layerSlot != kNoObjectSlot && dependent.layerSlot < out.size())
						write(dependent.layerSlot, LayerPropertyOf(*dependent.geometry));
				}
			}
		});
		std::uint64_t marked = 0, paletteMarked = 0;
		for (const auto& chunk : changed)
			for (const auto* item : chunk.settling)
				settling.push_back(*item);
		settled += settling.size();
		for (const auto& chunk : changed) {
			for (const auto slot : chunk.slots)
				rows.Mark(slot);
			marked += chunk.slots.size();
			for (const auto& [first, count] : chunk.paletteRuns) {
				palettes.MarkRange(first, count);
				paletteMarked += count;
			}
			skins += chunk.skins;
			defects += chunk.defects;
		}
		for (const auto& slots : rootChanged) {
			for (const auto slot : slots)
				rows.Mark(slot);
			marked += slots.size();
		}
		sampled += static_cast<std::uint64_t>(items.size() + a_job.firstPerson.size());
		rowsChanged += marked + firstPersonMarked;
		paletteRowsChanged += paletteMarked + firstPersonPaletteMarked;
		if (const auto refusals = refusedHere.load())
			refused += refusals;
		SampleShadingRows(a_job, work.slots);
		SampleFadeSeeds(a_job);
	}

	void FrameValues::Impl::SampleFadeSeeds(const Job& a_job)
	{
		ZoneScopedN("CS.DCLF.FrameValues.FadeSeeds");
		seeds.BeginBuild(*std::min_element(seedsHeld.begin(), seedsHeld.end()));
		auto& out = seeds.Mutable();
		if (out.size() < a_job.seedRows) {
			const std::size_t first = out.size();
			out.resize(a_job.seedRows, FadeRootStatic{});
			seeds.MarkRange(first, a_job.seedRows - first);
		}
		// In request order: a later value for the same row wins.
		for (const auto& [at, row] : a_job.seedValues) {
			if (at >= out.size())
				continue;
			out[at] = row;
			seeds.Mark(at);
			++seeded;
		}
		treeSeeds.BeginBuild(*std::min_element(treeSeedsHeld.begin(), treeSeedsHeld.end()));
		auto& trees = treeSeeds.Mutable();
		if (trees.size() < a_job.treeSeedRows) {
			const std::size_t first = trees.size();
			trees.resize(a_job.treeSeedRows, TreeStatic{});
			treeSeeds.MarkRange(first, a_job.treeSeedRows - first);
		}
		for (const auto& [at, row] : a_job.treeSeedValues) {
			if (at >= trees.size())
				continue;
			trees[at] = row;
			treeSeeds.Mark(at);
			++seeded;
		}
	}

	void FrameValues::Impl::SampleShadingRows(const Job& a_job, std::size_t a_slots)
	{
		ZoneScopedN("CS.DCLF.FrameValues.Shading");
		shading.BeginBuild(*std::min_element(shadingHeld.begin(), shadingHeld.end()));
		auto& out = shading.Mutable();
		if (out.size() < a_slots) {
			const std::size_t first = out.size();
			out.resize(a_slots, BindlessShading{});
			shading.MarkRange(first, a_slots - first);
		}
		// Each slot once, by its newest item (the slot may have been reused since).
		std::vector<const ShadingItem*> items;
		items.reserve(a_job.shadingItems.size());
		shadingListed.assign(out.size(), 0);
		for (auto it = a_job.shadingItems.rbegin(); it != a_job.shadingItems.rend(); ++it)
			if (it->slot < out.size() && it->property && !std::exchange(shadingListed[it->slot], 1))
				items.push_back(&*it);
		constexpr std::size_t kGrain = 128;
		std::vector<std::vector<std::uint32_t>> changed((items.size() + kGrain - 1) / kGrain);
		std::atomic<std::uint64_t> refusedHere{ 0 };
		SceneScheduler::Executor().ParallelFor("CS.DCLF.FrameValues.Shading", items.size(), kGrain, [&](std::size_t a_begin, std::size_t a_end) {
			ZoneScopedN("CS.DCLF.FrameValues.Shading");
			FrameGlobals::Scope scope(a_job.globals);  // the tree wind's (SampleShading: DeriveTreeAnim)
			auto& mine = changed[a_begin / kGrain];
			for (std::size_t i = a_begin; i < a_end; ++i) {
				const auto& item = *items[i];
				EngineReadWindow::Lease lease;
				if (!lease) {
					++refusedHere;
					continue;
				}
				BindlessShading row = out[item.slot];
				SampleShading(*item.property.get(), item.pass, item.member, row);
				// Only actors have wetness (from the capture, below).
				if (!item.actor)
					std::fill(std::begin(row.skinPerGeometry), std::end(row.skinPerGeometry), 0.0f);
				if (std::memcmp(&row, &out[item.slot], sizeof(row)) != 0) {
					out[item.slot] = row;
					mine.push_back(item.slot);
				}
			}
		});
		std::uint64_t marked = 0, wetMarked = 0;
		for (const auto& slots : changed) {
			for (const auto slot : slots)
				shading.Mark(slot);
			marked += slots.size();
		}
		for (const auto& [slot, value] : a_job.wetness) {
			if (slot >= out.size() || std::memcmp(out[slot].skinPerGeometry, value.data(), sizeof(value)) == 0)
				continue;
			std::memcpy(out[slot].skinPerGeometry, value.data(), sizeof(value));
			shading.Mark(slot);
			++wetMarked;
		}
		shadingSampled += items.size();
		shadingChanged += marked;
		wetnessChanged += wetMarked;
		if (const auto refusals = refusedHere.load())
			refused += refusals;
	}

	template <class T>
	void FrameValues::Impl::Send(const Job& a_job, const KeptArray<T>& a_rows, std::uint64_t& a_holds, bool a_fresh, const std::shared_ptr<org::Buffer>& a_target,
		std::uint32_t a_capacity)
	{
		auto view = a_rows.View();
		const std::size_t count = std::min<std::size_t>(view.Count(), a_capacity);
		if (view.Count() > count)
			throw std::runtime_error(fmt::format("its buffer holds {} rows of {}", count, view.Count()));
		if (a_fresh)
			a_holds = 0;
		const auto* data = view.elements ? view.elements->data() : nullptr;
		std::uint64_t sent = 0, sentBytes = 0;
		view.changes.ForEachRun(a_holds, count, [&](std::uint64_t a_first, std::uint64_t a_count) {
			const org::StreamingUploadSegment segment{ data + a_first, static_cast<std::size_t>(a_count * sizeof(T)) };
			auto ticket = a_job.uploads->QueueTrackedStreamingUploadSegments({ &segment, 1 }, segment.size,
				org::WorkerOwnedDestination{ a_target, org::WorkerOwnedDestination::Ownership::RetiredFrameRegion }, static_cast<std::size_t>(a_first * sizeof(T)));
			if (!ticket || ticket->state.load(std::memory_order_acquire) == org::TrackedUploadTicketState::Cancelled)
				throw std::runtime_error("the dedicated uploader refused its rows");
			++sent;
			sentBytes += segment.size;
		});
		a_holds = view.Version();
		runs += sent;
		bytes += sentBytes;
	}

	void FrameValues::Impl::Upload(const Job& a_job)
	{
		ZoneScopedN("CS.DCLF.FrameValues.Upload");
		Send(a_job, rows, held[a_job.ringIndex], a_job.fresh, a_job.target, a_job.capacity);
		Send(a_job, palettes, paletteHeld[a_job.ringIndex], a_job.paletteFresh, a_job.paletteTarget, a_job.paletteCapacity);
		Send(a_job, shading, shadingHeld[a_job.ringIndex], a_job.shadingFresh, a_job.shadingTarget, a_job.shadingCapacity);
		Send(a_job, seeds, seedsHeld[a_job.ringIndex], a_job.seedFresh, a_job.seedTarget, a_job.seedCapacity);
		Send(a_job, treeSeeds, treeSeedsHeld[a_job.ringIndex], a_job.treeSeedFresh, a_job.treeSeedTarget, a_job.treeSeedCapacity);
	}

	FrameValues::FrameValues() :
		impl(std::make_unique<Impl>())
	{}

	FrameValues::~FrameValues() = default;

	FrameValues& FrameValues::Get()
	{
		static FrameValues values;
		return values;
	}

	bool FrameValues::Available() const
	{
		auto& runtime = RenderGraphRuntime::Get();
		return runtime.Host() && runtime.FrameWaitTimeline();
	}

	bool FrameValues::Kick(std::vector<std::shared_ptr<const PlacementPlan>> a_plans, std::vector<ShadingItem> a_shading,
		std::vector<WetnessValue> a_wetness, std::vector<FadeSeedItem> a_seeds, std::vector<TreeSeedItem> a_treeSeeds,
		std::shared_ptr<const FrameGlobals> a_globals, FrameUploads a_uploads)
	{
		auto& s = *impl;
		s.kicked = false;
		// Fade seeds: read at the frame's start, before this frame's culls update the nodes (FadeStateCS's first update is this
		// frame's), with the frame's globals (StaticOf's LOD scale).
		{
			FrameGlobals::Scope scope(a_globals);
			for (const auto& seed : a_seeds) {
				if (!seed.node)
					continue;
				FadeRootStatic row = FadeState::StaticOf(*seed.node);
				row.generation = seed.generation;
				s.unsentSeeds.emplace_back(seed.row, row);
				s.seedRows = std::max(s.seedRows, seed.row + 1);
			}
			// Tree seeds likewise (TreeStaticOfNode runs the engine's FUN_14147d640: render thread only).
			for (const auto& seed : a_treeSeeds) {
				if (!seed.node)
					continue;
				TreeStatic row;
				TreeStaticOfNode(seed.node.get(), row);
				row.generation = seed.generation;
				row.seedOdd = seed.row & 1u;
				s.unsentTreeSeeds.emplace_back(seed.row, row);
				s.treeSeedRows = std::max(s.treeSeedRows, seed.row + 1);
			}
		}
		// Appended after any not yet taken (newest last).
		s.unsampledShading.insert(s.unsampledShading.end(), std::make_move_iterator(a_shading.begin()), std::make_move_iterator(a_shading.end()));
		s.unsampledWetness.insert(s.unsampledWetness.end(), a_wetness.begin(), a_wetness.end());
		auto* host = RenderGraphRuntime::Get().Host();
		auto timeline = RenderGraphRuntime::Get().FrameWaitTimeline();
		auto uploads = host ? host->RetainUploads() : nullptr;
		if (!host || !timeline || !uploads || !uploads->HasDedicatedStreamingQueue()) {
			// Plans wait for the first producer; startup frames precede the upload queue (made by the graph's first build).
			for (auto& plan : a_plans)
				if (plan) {
					s.unsampled.push_back(plan);
					s.plan = std::move(plan);
				}
			if (host)
				host->SetFrameWaitValue(0);
			if (!s.unavailableLogged && s.seq) {
				s.unavailableLogged = true;
				logger::error("[DCLF] frame values unavailable ({}): DCLF's draws have no placements", !host ? "no render graph" : "no dedicated upload queue");
			}
			return false;
		}
		// Every plan since the last frame: all written slots are sampled, the newest is drawn with.
		for (auto& plan : a_plans)
			if (plan) {
				s.unsampled.push_back(plan);
				s.plan = std::move(plan);
			}
		// Release finished producers' plans here on the render thread (they hold engine references).
		const auto finished = s.done.load(std::memory_order_acquire);
		while (!s.plans.empty() && s.plans.front().first <= finished)
			s.plans.pop_front();
		static const Plan kEmpty{};
		const std::uint64_t seq = ++s.seq;
		const std::uint32_t r = static_cast<std::uint32_t>(seq % kRing);
		Impl::Job job;
		job.seq = seq;
		job.plan = s.plan ? s.plan.get() : &kEmpty;
		// Held until the producer is done.
		for (auto& unsampled : s.unsampled) {
			job.written.push_back(unsampled.get());
			s.plans.emplace_back(seq, std::move(unsampled));
		}
		s.unsampled.clear();
		job.shadingItems = std::exchange(s.unsampledShading, {});
		job.wetness = std::exchange(s.unsampledWetness, {});
		job.seedValues = std::exchange(s.unsentSeeds, {});
		job.seedRows = s.seedRows;
		job.treeSeedValues = std::exchange(s.unsentTreeSeeds, {});
		job.treeSeedRows = s.treeSeedRows;
		job.globals = a_globals;
		job.ringIndex = r;
		// Grow by replacing the buffer; the old one retires with the frames reading it.
		const auto retire = [](std::shared_ptr<org::Buffer>&& a_old) { Scene::FrameView::Get().RetireImport(std::move(a_old)); };
		const auto ensure = [&](Impl::RingBuffer& a_entry, std::uint32_t a_needed, std::uint32_t a_stride, const char* a_name, bool& a_fresh) {
			if (org::services::EnsureTransport(a_entry, a_needed, a_stride, org::services::GrowthPolicy::Doubling(1024),
					[&] { return fmt::format("cs.dclf.frame-values.{}.{}", a_name, r); }, retire))
				a_fresh = true;
		};
		ensure(s.ring[r], s.plan ? s.plan->slots : 0u, sizeof(BindlessPlacement), "placements", job.fresh);
		ensure(s.paletteRing[r], s.plan ? 2 * s.plan->boneCapacity : 0u, sizeof(PaletteRow), "palettes", job.paletteFresh);
		ensure(s.shadingRing[r], s.plan ? s.plan->slots : 0u, sizeof(BindlessShading), "shading", job.shadingFresh);
		ensure(s.seedRing[r], s.seedRows, sizeof(FadeRootStatic), "fade-seeds", job.seedFresh);
		ensure(s.treeSeedRing[r], s.treeSeedRows, sizeof(TreeStatic), "tree-seeds", job.treeSeedFresh);
		job.target = s.ring[r].buffer;
		job.capacity = static_cast<std::uint32_t>(s.ring[r].capacity);
		job.paletteTarget = s.paletteRing[r].buffer;
		job.paletteCapacity = static_cast<std::uint32_t>(s.paletteRing[r].capacity);
		job.shadingTarget = s.shadingRing[r].buffer;
		job.shadingCapacity = static_cast<std::uint32_t>(s.shadingRing[r].capacity);
		job.seedTarget = s.seedRing[r].buffer;
		job.seedCapacity = static_cast<std::uint32_t>(s.seedRing[r].capacity);
		job.treeSeedTarget = s.treeSeedRing[r].buffer;
		job.treeSeedCapacity = static_cast<std::uint32_t>(s.treeSeedRing[r].capacity);
		job.reuse = s.points[r];
		job.timeline = std::move(timeline);
		job.uploads = std::move(uploads);
		job.more = std::move(a_uploads);
		job.particleCamera = ParticleSnapshots::Get().FrameCamera();
		// The first-person eye (DrawWorld +0x00, filled by FUN_1406452c0 before FrameDriver::BeginFrame): where FUN_1414cb020 places
		// the first-person camera, with the world camera's rotation.
		job.firstPersonParticleCamera = job.particleCamera;
		if (const auto* eye = REL::Relocation<const float*>{ REL::Offset(Engine::Address::kDrawWorldEye) }.get(); eye && job.particleCamera.valid)
			std::memcpy(job.firstPersonParticleCamera.eye, eye, sizeof(job.firstPersonParticleCamera.eye));
		// Claimed first-person members, sampled after the engine's update and before Main::Draw's first-person shift
		// (FUN_1414cb020), palettes included: the engine draws none of them, so none of its reads see a world-space palette.
		if (const auto claims = std::exchange(firstPersonClaims, nullptr)) {
			std::vector<std::uint8_t> taken;
			auto take = [&](const Plan::Item& a_item) {
				if (!a_item.firstPerson || !a_item.geometry || a_item.slot == kNoObjectSlot || !(claims->PhasesOf(a_item.geometry.get()) & kSetFirstPerson))
					return;
				if (taken.size() <= a_item.slot)
					taken.resize(a_item.slot + 1, 0);
				if (std::exchange(taken[a_item.slot], 1))
					return;
				auto& sample = job.firstPerson.emplace_back();
				sample.slot = a_item.slot;
				sample.layerSlot = a_item.layerSlot;
				SampleItem(a_item, sample.row, sample.layer);
				if (!a_item.boneRows)
					return;
				auto& geometry = *a_item.geometry;
				if (auto* skin = geometry.GetGeometryRuntimeData().skinInstance.get())
					UpdateSkin(skin, geometry.world);
				const float* current = nullptr;
				const float* previous = nullptr;
				if (!SkinPalettes(geometry, a_item.boneRows, current, previous))
					return;
				sample.boneOffset = a_item.boneOffset;
				sample.boneRows = a_item.boneRows;
				const auto* currentRows = reinterpret_cast<const PaletteRow*>(current);
				const auto* previousRows = reinterpret_cast<const PaletteRow*>(previous);
				sample.current.assign(currentRows, currentRows + a_item.boneRows);
				sample.previous.assign(previousRows, previousRows + a_item.boneRows);
			};
			for (const auto& item : job.plan->movers)
				take(item);
			for (auto it = job.written.rbegin(); it != job.written.rend(); ++it)
				for (const auto& item : (*it)->written)
					take(item);
		}
		if (s.plan)
			s.plans.emplace_back(seq, s.plan);
		frameIndex = s.ring[r].srvIndex;
		paletteIndex = s.paletteRing[r].srvIndex;
		shadingIndex = s.shadingRing[r].srvIndex;
		seedsIndex = s.seedRing[r].srvIndex;
		treeSeedsIndex = s.treeSeedRing[r].srvIndex;
		// Batches submitted from here wait for the frame's values; the producer always signals.
		host->SetFrameWaitValue(seq);
		s.kicked = true;
		if (!SceneScheduler::Executor().Dispatch(SceneScheduler::Scope(), SceneExecutor::Preparation, org::async::TaskDispatch::Cpu,
				"frame values", [&s, job = std::move(job)](const auto&) mutable {
					s.Run(job);
					// Engine references go to the render thread (EngineReleases, at Present), never dropped on this worker.
					for (auto& item : job.shadingItems)
						EngineReleases::Push(RE::NiPointer<RE::NiRefObject>(std::move(item.property)));
				}))
			stl::report_and_fail("Drawcall Limit Fix: the frame values' producer was refused by DCLF's executor");
		return true;
	}

	void FrameValues::Skip()
	{
		impl->kicked = false;
		if (auto* host = RenderGraphRuntime::Get().Host())
			host->SetFrameWaitValue(0);
	}

	void FrameValues::EndFrame()
	{
		auto& s = *impl;
		if (!s.kicked)
			return;
		s.kicked = false;
		if (auto* host = RenderGraphRuntime::Get().Host())
			s.points[s.seq % kRing] = host->SubmittedPoint();
	}

	const std::vector<TreeStatic>* FrameValues::TreeSeedsIfDone(bool a_wait) const
	{
		auto& s = *impl;
		if (!FadeSeedsIfDone(a_wait))
			return nullptr;
		return &s.treeSeeds.Get();
	}

	const std::vector<FadeRootStatic>* FrameValues::FadeSeedsIfDone(bool a_wait) const
	{
		auto& s = *impl;
		if (!s.seq)
			return nullptr;
		if (a_wait)
			for (auto finished = s.done.load(std::memory_order_acquire); finished < s.seq; finished = s.done.load(std::memory_order_acquire))
				s.done.wait(finished, std::memory_order_acquire);
		if (s.done.load(std::memory_order_acquire) < s.seq)
			return nullptr;
		return &s.seeds.Get();
	}

	const std::vector<BindlessPlacement>* FrameValues::RowsIfDone(bool a_wait) const
	{
		auto& s = *impl;
		if (!s.seq)
			return nullptr;
		if (a_wait)
			for (auto finished = s.done.load(std::memory_order_acquire); finished < s.seq; finished = s.done.load(std::memory_order_acquire))
				s.done.wait(finished, std::memory_order_acquire);
		if (s.done.load(std::memory_order_acquire) < s.seq)
			return nullptr;
		return &s.rows.Get();
	}

	const std::vector<FrameValues::PaletteRow>* FrameValues::PalettesIfDone(bool a_wait) const
	{
		auto& s = *impl;
		if (!s.seq)
			return nullptr;
		if (a_wait)
			for (auto finished = s.done.load(std::memory_order_acquire); finished < s.seq; finished = s.done.load(std::memory_order_acquire))
				s.done.wait(finished, std::memory_order_acquire);
		if (s.done.load(std::memory_order_acquire) < s.seq)
			return nullptr;
		return &s.palettes.Get();
	}

	const std::vector<BindlessShading>* FrameValues::ShadingIfDone(bool a_wait) const
	{
		auto& s = *impl;
		if (!s.seq)
			return nullptr;
		if (a_wait)
			for (auto finished = s.done.load(std::memory_order_acquire); finished < s.seq; finished = s.done.load(std::memory_order_acquire))
				s.done.wait(finished, std::memory_order_acquire);
		if (s.done.load(std::memory_order_acquire) < s.seq)
			return nullptr;
		return &s.shading.Get();
	}

	FrameValues::Counters FrameValues::TakeCounters()
	{
		auto& s = *impl;
		Counters c;
		c.frames = s.frames.exchange(0);
		if (!c.frames)
			return c;
		c.sampled = s.sampled.exchange(0);
		c.settled = s.settled.exchange(0);
		c.seeded = s.seeded.exchange(0);
		c.rowsChanged = s.rowsChanged.exchange(0);
		c.skins = s.skins.exchange(0);
		c.paletteRowsChanged = s.paletteRowsChanged.exchange(0);
		c.shadingSampled = s.shadingSampled.exchange(0);
		c.shadingChanged = s.shadingChanged.exchange(0);
		c.wetnessChanged = s.wetnessChanged.exchange(0);
		c.runs = s.runs.exchange(0);
		c.bytes = s.bytes.exchange(0);
		c.sampleUs = s.sampleUs.exchange(0);
		c.pointWaitUs = s.pointWaitUs.exchange(0);
		c.pointWaits = s.pointWaits.exchange(0);
		c.uploadUs = s.uploadUs.exchange(0);
		c.maxTotalUs = s.maxTotalUs.exchange(0);
		c.refused = s.refused.exchange(0);
		c.defects = s.defects.exchange(0);
		c.failures = s.failures.exchange(0);
		return c;
	}
#else
	struct FrameValues::Impl
	{};
	FrameValues::FrameValues() = default;
	FrameValues::~FrameValues() = default;
	FrameValues& FrameValues::Get()
	{
		static FrameValues values;
		return values;
	}
	bool FrameValues::Available() const { return false; }
	bool FrameValues::Kick(std::vector<std::shared_ptr<const PlacementPlan>>, std::vector<ShadingItem>, std::vector<WetnessValue>,
		std::vector<FadeSeedItem>, std::vector<TreeSeedItem>, std::shared_ptr<const FrameGlobals>, FrameUploads) { return false; }
	const std::vector<TreeStatic>* FrameValues::TreeSeedsIfDone(bool) const { return nullptr; }
	const std::vector<FadeRootStatic>* FrameValues::FadeSeedsIfDone(bool) const { return nullptr; }
	void FrameValues::Skip() {}
	void FrameValues::EndFrame() {}
	const std::vector<BindlessPlacement>* FrameValues::RowsIfDone(bool) const { return nullptr; }
	const std::vector<FrameValues::PaletteRow>* FrameValues::PalettesIfDone(bool) const { return nullptr; }
	const std::vector<BindlessShading>* FrameValues::ShadingIfDone(bool) const { return nullptr; }
	FrameValues::Counters FrameValues::TakeCounters() { return {}; }
	bool FrameValues::SkinPalettes(const RE::BSGeometry&, std::uint32_t, const float*&, const float*&) { return false; }
#endif
}
