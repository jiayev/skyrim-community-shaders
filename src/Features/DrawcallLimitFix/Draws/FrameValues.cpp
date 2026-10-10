#include "FrameValues.h"

#include "Features/DrawcallLimitFix/Common/KeptState.h"
#include "Features/DrawcallLimitFix/Engine/ShadowViews.h"
#include "Features/DrawcallLimitFix/Scene/LightingConstants.h"
#include "Features/DrawcallLimitFix/Scene/FadeState.h"
#include "Features/DrawcallLimitFix/Scene/FrameGlobals.h"
#include "Features/DrawcallLimitFix/Scene/LightingDescriptors.h"

#include <cstring>
#include <limits>

#if defined(CS_HAS_RENDER_GRAPH) && defined(CS_HAS_ORG_MODULE_SERVICES)
#	include "Features/DrawcallLimitFix/Common/SceneScheduler.h"
#	include "Features/DrawcallLimitFix/Common/Switches.h"
#	include "Features/DrawcallLimitFix/Engine/EngineReadWindow.h"
#	include "RenderGraph/RenderGraphRuntime.h"
#	include <OpenRenderGraph/PersistentGraphHost.h>
#	include <Render/Runtime/IUploadService.h>
#	include <Resources/Buffers/Buffer.h>
#	include <rhi.h>

#	include <algorithm>
#	include <chrono>
#endif

namespace DCLF
{
	namespace
	{
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

		// A sun entry: the node's bound, or unbounded (radius +max, inside every process) without one.
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
	}

	void FrameValues::SampleRow(const RE::BSGeometry& a_geometry, const RE::NiAVObject* a_sunEntryNode, const RE::BSShaderProperty* a_fadeProperty,
		BindlessPlacement& a_out)
	{
		StoreTransform(a_geometry.world, a_out.world);
		// Render flag 0x10: the previous transform is the current one (engine notes: SetupGeometry).
		if (SceneStore::kMainPassRenderFlags & 0x10)
			std::memcpy(a_out.previousWorld, a_out.world, sizeof(a_out.world));
		else
			StoreTransform(DrawnPreviousWorld(a_geometry), a_out.previousWorld);
		a_out.bound[0] = a_geometry.worldBound.center.x;
		a_out.bound[1] = a_geometry.worldBound.center.y;
		a_out.bound[2] = a_geometry.worldBound.center.z;
		a_out.bound[3] = a_geometry.worldBound.radius;
		StoreSunEntry(a_sunEntryNode, a_out.sunEntry);
		StoreFadeNode(a_fadeProperty, a_out.lodFadeNode);
	}

	bool FrameValues::SampleSlot(const SceneStore::Tables& a_tables, std::uint32_t a_slot, BindlessPlacement& a_out)
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
		using Plan = SceneStore::PlacementPlan;
		using PaletteRow = FrameValues::PaletteRow;

		/**
		 * @brief The engine's per-frame palette update (AE FUN_140e4ff90): what the bone setter runs from the native draw an owned
		 * skin no longer gets. Idempotent within a frame (frameID) and under the skin's own critical section, so any thread may run
		 * it; it copies the current palette to the previous one first and writes three float4 rows a bone in absolute world space.
		 */
		void UpdateSkin(RE::NiSkinInstance* a_skin, const RE::NiTransform& a_world)
		{
			using UpdateSkinInstance = void (*)(RE::NiSkinInstance*, const RE::NiTransform*);
			static const REL::Relocation<UpdateSkinInstance> updateSkinInstance{ REL::Offset(0xe4ff90) };
			updateSkinInstance(a_skin, &a_world);
		}

		// An item's row, and its layer's.
		void SampleItem(const Plan::Item& a_item, BindlessPlacement& a_row, BindlessPlacement& a_layer)
		{
			const auto& geometry = *a_item.geometry;
			FrameValues::SampleRow(geometry, a_item.sunEntryNode.get(), geometry.GetGeometryRuntimeData().shaderProperty.get(), a_row);
			if (a_item.layerSlot != kNoObjectSlot) {
				a_layer = a_row;
				StoreFadeNode(LayerPropertyOf(geometry), a_layer.lodFadeNode);
			}
		}

		// The skin's palettes after this frame's update, or null when its size is not its block's (the walk writes it again: KeepSkin).
		bool SkinPalettes(const RE::BSGeometry& a_geometry, std::uint32_t a_rows, const float*& a_current, const float*& a_previous)
		{
			auto* skin = a_geometry.GetGeometryRuntimeData().skinInstance.get();
			if (!skin)
				return false;
			a_current = static_cast<const float*>(skin->boneMatrices);
			a_previous = static_cast<const float*>(skin->prevBoneMatrices);
			return a_current && a_previous && skin->numMatrices * 3 == a_rows;
		}

		std::uint32_t Doubled(std::uint32_t a_capacity, std::uint32_t a_needed)
		{
			std::uint32_t capacity = std::max(a_capacity, 1024u);
			while (capacity < a_needed)
				capacity *= 2;
			return capacity;
		}
	}

	struct FrameValues::Impl
	{
		// Render thread.
		struct RingBuffer
		{
			std::shared_ptr<org::Buffer> buffer;
			std::uint32_t capacity = 0, srvIndex = 0;
		};
		std::array<RingBuffer, kRing> ring, paletteRing, shadingRing, seedRing, treeSeedRing;
		std::array<org::PersistentGraphHost::GpuPoint, kRing> points;  // per ring entry: the last frame that read it
		std::shared_ptr<const Plan> plan;                              // the newest
		std::vector<std::shared_ptr<const Plan>> unsampled;           // plans whose written slots no producer has sampled yet
		std::vector<SceneStore::ShadingItem> unsampledShading;        // shading items and wetness no producer has taken yet
		std::vector<SceneStore::WetnessValue> unsampledWetness;
		std::vector<std::pair<std::uint32_t, FadeRootStatic>> unsentSeeds;  // T6b1a: seed rows (FadeSeedRow) no producer has written yet
		std::uint32_t seedRows = 0;                                     // the seed rows the requests have named (FadeSeedRow + 1, grown)
		std::vector<std::pair<std::uint32_t, TreeStatic>> unsentTreeSeeds;
		std::uint32_t treeSeedRows = 0;
		std::deque<std::pair<std::uint64_t, std::shared_ptr<const Plan>>> plans;  // the plans each kicked producer reads, by its sequence
		std::uint64_t seq = 0;                                          // the last kicked
		bool kicked = false;                                            // this frame
		bool unavailableLogged = false;

		// The producers', one at a time in sequence order.
		KeptArray<BindlessPlacement> rows;
		KeptArray<PaletteRow> palettes;
		KeptArray<BindlessShading> shading;
		KeptArray<FadeRootStatic> seeds;  // T6b1a: by FadeSeedRow
		KeptArray<TreeStatic> treeSeeds;  // T6b1a: by 2 * tree slot + seedOdd
		std::array<std::uint64_t, kRing> held{}, paletteHeld{}, shadingHeld{}, seedsHeld{}, treeSeedsHeld{};  // per ring entry: the versions it holds
		std::vector<std::uint8_t> listed;                       // per slot, while a producer lists its items
		std::vector<std::uint8_t> blockListed;                  // per palette row, likewise (a block an older plan's item still names)
		std::vector<std::uint8_t> shadingListed;                // per slot, while a producer lists its shading items
		// T6b1a: the written slots whose previous transform was not their current one yet (a static's first update), sampled again by
		// the next producers until it is; a newer item for the slot (a write, a mover) takes its place. The scene work no longer reads
		// the transforms to write them again.
		std::vector<Plan::Item> settling;
		std::atomic<std::uint64_t> done{ 0 };

		// Since the last report.
		std::atomic<std::uint64_t> frames{ 0 }, sampled{ 0 }, rowsChanged{ 0 }, skins{ 0 }, paletteRowsChanged{ 0 }, shadingSampled{ 0 }, shadingChanged{ 0 },
			wetnessChanged{ 0 }, defects{ 0 }, runs{ 0 }, bytes{ 0 }, refused{ 0 }, failures{ 0 }, pointWaits{ 0 }, settled{ 0 }, seeded{ 0 };
		std::atomic<std::uint64_t> sampleUs{ 0 }, pointWaitUs{ 0 }, uploadUs{ 0 }, maxTotalUs{ 0 };
		std::uint64_t parityChecks = 0, parityDiffers = 0, parityFresh = 0, paletteChecks = 0, paletteDiffers = 0;
		std::string parityFirst, paletteFirst;

		struct Job
		{
			std::uint64_t seq = 0;
			const Plan* plan = nullptr;
			std::vector<const Plan*> written;  // the plans whose written slots it samples too (once each, oldest first)
			std::vector<SceneStore::ShadingItem> shadingItems;  // the slots named for their shading, the newest last
			std::vector<SceneStore::WetnessValue> wetness;      // the wetness captured, in capture order
			std::vector<std::pair<std::uint32_t, FadeRootStatic>> seedValues;  // T6b1a: the seed rows taken at the frame's start, in request order
			std::uint32_t seedRows = 0;
			std::vector<std::pair<std::uint32_t, TreeStatic>> treeSeedValues;
			std::uint32_t treeSeedRows = 0;
			std::shared_ptr<const FrameGlobals> globals;  // the frame's engine globals (the shading rows' tree wind)
			std::uint32_t ringIndex = 0;
			std::shared_ptr<org::Buffer> target, paletteTarget, shadingTarget, seedTarget, treeSeedTarget;
			std::uint32_t capacity = 0, paletteCapacity = 0, shadingCapacity = 0, seedCapacity = 0, treeSeedCapacity = 0;  // their rows
			bool fresh = false, paletteFresh = false, shadingFresh = false, seedFresh = false, treeSeedFresh = false;      // a new buffer: it holds nothing
			org::PersistentGraphHost::GpuPoint reuse;
			std::shared_ptr<rhi::TimelinePtr> timeline;
			std::shared_ptr<org::runtime::IUploadService> uploads;
			FrameUploads more;  // the frame's other uploads (Kick's a_uploads)
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
		// One producer at a time, in order: each builds on the rows the one before left, and the signals rise.
		for (auto finished = done.load(std::memory_order_acquire); finished + 1 < a_job.seq; finished = done.load(std::memory_order_acquire))
			done.wait(finished, std::memory_order_acquire);
		const auto start = std::chrono::steady_clock::now();
		try {
			Sample(a_job);
			const auto sampledAt = std::chrono::steady_clock::now();
			sampleUs += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(sampledAt - start).count());
			// The buffers' last readers, F frames back, done on the GPU (almost always already).
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
		// Always: every batch of the frame waits for it.
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
		// Every block's current and previous palette (PaletteRowsOf).
		auto& paletteOut = palettes.Mutable();
		if (const std::size_t paletteRows = 2ull * work.boneCapacity; paletteOut.size() < paletteRows) {
			const std::size_t first = paletteOut.size();
			paletteOut.resize(paletteRows, PaletteRow{});
			palettes.MarkRange(first, paletteRows - first);
		}
		// The items, once each: the movers, and the slots the plans not sampled yet wrote in full, the newest walk's first (a slot an
		// older walk wrote and a newer one wrote again is the newer one's). A palette block likewise: the newest item naming it.
		struct Listed
		{
			const Plan::Item* item;
			bool palette;
			bool settles = false;  // a written slot's (not a mover's): kept while its transforms differ
		};
		std::vector<Listed> items;
		items.reserve(work.movers.size() + (a_job.written.empty() ? 0 : a_job.written.back()->written.size()));
		listed.assign(out.size(), 0);
		blockListed.assign(paletteOut.size() / 2, 0);
		auto list = [&](const Plan::Item& a_item, bool a_settles) {
			if (a_item.slot >= out.size() || !a_item.geometry || std::exchange(listed[a_item.slot], 1))
				return;
			const bool palette = a_item.boneRows && std::size_t(a_item.boneOffset) + a_item.boneRows <= blockListed.size() && !std::exchange(blockListed[a_item.boneOffset], 1);
			items.push_back({ &a_item, palette, a_settles });
		};
		for (const auto& item : work.movers)
			list(item, false);
		for (auto it = a_job.written.rbegin(); it != a_job.written.rend(); ++it)
			for (const auto& item : (*it)->written)
				list(item, true);
		// Last: a slot listed above is a newer item's.
		const auto carried = std::move(settling);
		settling.clear();
		for (const auto& item : carried)
			list(item, true);
		auto& executor = SceneScheduler::Executor();
		constexpr std::size_t kGrain = 64;
		// Each chunk's changed slots and palette runs, marked after (the journals are one thread's).
		struct Changed
		{
			std::vector<std::uint32_t> slots;
			std::vector<std::pair<std::uint32_t, std::uint32_t>> paletteRuns;  // (first row, rows)
			std::vector<const Plan::Item*> settling;
			std::uint64_t skins = 0, defects = 0;
		};
		std::vector<Changed> changed((items.size() + kGrain - 1) / kGrain);
		std::atomic<std::uint64_t> refusedHere{ 0 };
		executor.ParallelFor("CS.DCLF.FrameValues.Items", items.size(), kGrain, [&](std::size_t a_begin, std::size_t a_end) {
			ZoneScopedN("CS.DCLF.FrameValues.Items");
			auto& mine = changed[a_begin / kGrain];
			BindlessPlacement row, layer;
			for (std::size_t i = a_begin; i < a_end; ++i) {
				const auto& [itemPointer, palette, settles] = items[i];
				const auto& item = *itemPointer;
				EngineReadWindow::Lease lease;
				if (!lease) {
					++refusedHere;
					if (settles)
						mine.settling.push_back(&item);  // not sampled: the next producer takes it
					continue;
				}
				SampleItem(item, row, layer);
				if (settles && std::memcmp(&item.geometry->world, &DrawnPreviousWorld(*item.geometry), sizeof(RE::NiTransform)) != 0)
					mine.settling.push_back(&item);
				if (std::memcmp(&out[item.slot], &row, sizeof(row)) != 0) {
					out[item.slot] = row;
					mine.slots.push_back(item.slot);
				}
				if (item.layerSlot != kNoObjectSlot && item.layerSlot < out.size() && std::memcmp(&out[item.layerSlot], &layer, sizeof(layer)) != 0) {
					out[item.layerSlot] = layer;
					mine.slots.push_back(item.layerSlot);
				}
				if (!palette)
					continue;
				// The skin's palette: the engine's update for the frame, then its rows into the block's, noted only when they differ.
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
		// The roots: their dependents' sun entries (and fade nodes), after the items wrote theirs.
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
					if (dependent.slot >= out.size() || !dependent.geometry)
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
		sampled += static_cast<std::uint64_t>(items.size());
		rowsChanged += marked;
		paletteRowsChanged += paletteMarked;
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
		// Taken by the render thread at the frame's start (Kick), in request order: a later one for the same row wins.
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
		// Each slot once, by the newest item naming it (a slot freed and taken again since an older one was named).
		std::vector<const SceneStore::ShadingItem*> items;
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
				SampleShading(*static_cast<const RE::BSLightingShaderProperty*>(item.property.get()), item.pass, item.member, row);
				// An actor's wetness is the capture's (below); anything else has none.
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

	bool FrameValues::Kick(std::shared_ptr<const SceneStore::PlacementPlan> a_plan, std::vector<SceneStore::ShadingItem> a_shading,
		std::vector<SceneStore::WetnessValue> a_wetness, std::vector<SceneStore::FadeSeedItem> a_seeds, std::vector<SceneStore::TreeSeedItem> a_treeSeeds,
		std::shared_ptr<const FrameGlobals> a_globals, FrameUploads a_uploads)
	{
		auto& s = *impl;
		s.kicked = false;
		// The fade seeds (T6b1a): the nodes as the frame's start has them, before this frame's culls update them (FadeStateCS's first update
		// of the generation is this frame's), with the frame's globals (StaticOf's LOD scale). The render thread's own read.
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
			// The trees' likewise: the node's clock and values (TreeStaticOfNode runs the engine's FUN_14147d640: the render thread's).
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
		// The shading items and the wetness, after any no producer took yet (each in order: the newest last).
		s.unsampledShading.insert(s.unsampledShading.end(), std::make_move_iterator(a_shading.begin()), std::make_move_iterator(a_shading.end()));
		s.unsampledWetness.insert(s.unsampledWetness.end(), a_wetness.begin(), a_wetness.end());
		auto* host = RenderGraphRuntime::Get().Host();
		auto timeline = RenderGraphRuntime::Get().FrameWaitTimeline();
		auto uploads = host ? host->RetainUploads() : nullptr;
		if (!host || !timeline || !uploads || !uploads->HasDedicatedStreamingQueue()) {
			// The plan waits for the first producer (its written slots are sampled once, by one): a startup's first frames precede the
			// upload queue (the graph's first build makes it).
			if (a_plan) {
				s.unsampled.push_back(a_plan);
				s.plan = std::move(a_plan);
			}
			if (host)
				host->SetFrameWaitValue(0);
			if (!s.unavailableLogged && s.seq) {
				s.unavailableLogged = true;
				logger::error("[DCLF] frame values unavailable ({}): DCLF's draws have no placements", !host ? "no render graph" : "no dedicated upload queue");
			}
			return false;
		}
		if (a_plan) {
			s.unsampled.push_back(a_plan);
			s.plan = std::move(a_plan);
		}
		// The plans done producers read, released here (the newest stays with s.plan): their references are the engine's to drop.
		const auto finished = s.done.load(std::memory_order_acquire);
		while (!s.plans.empty() && s.plans.front().first <= finished)
			s.plans.pop_front();
		static const Plan kEmpty{};
		const std::uint64_t seq = ++s.seq;
		const std::uint32_t r = static_cast<std::uint32_t>(seq % kRing);
		Impl::Job job;
		job.seq = seq;
		job.plan = s.plan ? s.plan.get() : &kEmpty;
		// Every plan the producer reads, held until it is done.
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
		// Each buffer holds every row the plan's tables do; a new one when it grows (the old one goes when the frames reading it retire).
		const auto ensure = [&](Impl::RingBuffer& a_entry, std::uint32_t a_needed, std::uint32_t a_stride, const char* a_name, bool& a_fresh) {
			if (a_entry.buffer && a_entry.capacity >= a_needed)
				return;
			const std::uint32_t capacity = Doubled(a_entry.capacity, a_needed);
			auto buffer = org::Buffer::CreateUnmaterializedStructuredBuffer(capacity, a_stride, false);
			buffer->SetName(fmt::format("cs.dclf.frame-values.{}.{}", a_name, r).c_str());
			buffer->Materialize();
			if (a_entry.buffer)
				SceneStore::Get().RetireImport(std::move(a_entry.buffer));
			a_entry.buffer = std::move(buffer);
			a_entry.capacity = capacity;
			a_entry.srvIndex = a_entry.buffer->GetSRVInfo(0).slot.index;
			a_fresh = true;
		};
		ensure(s.ring[r], s.plan ? s.plan->slots : 0u, sizeof(BindlessPlacement), "placements", job.fresh);
		ensure(s.paletteRing[r], s.plan ? 2 * s.plan->boneCapacity : 0u, sizeof(PaletteRow), "palettes", job.paletteFresh);
		ensure(s.shadingRing[r], s.plan ? s.plan->slots : 0u, sizeof(BindlessShading), "shading", job.shadingFresh);
		ensure(s.seedRing[r], s.seedRows, sizeof(FadeRootStatic), "fade-seeds", job.seedFresh);
		ensure(s.treeSeedRing[r], s.treeSeedRows, sizeof(TreeStatic), "tree-seeds", job.treeSeedFresh);
		job.target = s.ring[r].buffer;
		job.capacity = s.ring[r].capacity;
		job.paletteTarget = s.paletteRing[r].buffer;
		job.paletteCapacity = s.paletteRing[r].capacity;
		job.shadingTarget = s.shadingRing[r].buffer;
		job.shadingCapacity = s.shadingRing[r].capacity;
		job.seedTarget = s.seedRing[r].buffer;
		job.seedCapacity = s.seedRing[r].capacity;
		job.treeSeedTarget = s.treeSeedRing[r].buffer;
		job.treeSeedCapacity = s.treeSeedRing[r].capacity;
		job.reuse = s.points[r];
		job.timeline = std::move(timeline);
		job.uploads = std::move(uploads);
		job.more = std::move(a_uploads);
		if (s.plan)
			s.plans.emplace_back(seq, s.plan);
		frameIndex = s.ring[r].srvIndex;
		paletteIndex = s.paletteRing[r].srvIndex;
		shadingIndex = s.shadingRing[r].srvIndex;
		seedsIndex = s.seedRing[r].srvIndex;
		treeSeedsIndex = s.treeSeedRing[r].srvIndex;
		// Every batch submitted from here waits for the frame's values; the producer always signals them.
		host->SetFrameWaitValue(seq);
		s.kicked = true;
		if (!SceneScheduler::Executor().Dispatch(SceneScheduler::Scope(), PublishedSceneExecutor::Preparation, org::async::TaskDispatch::Cpu,
				"frame values", [&s, job = std::move(job)](const auto&) { s.Run(job); }))
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

	const std::vector<BindlessPlacement>* FrameValues::RowsIfDone() const
	{
		auto& s = *impl;
		if (!s.seq || s.done.load(std::memory_order_acquire) < s.seq)
			return nullptr;
		return &s.rows.Get();
	}

	const std::vector<FrameValues::PaletteRow>* FrameValues::PalettesIfDone() const
	{
		auto& s = *impl;
		if (!s.seq || s.done.load(std::memory_order_acquire) < s.seq)
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

	void FrameValues::CheckParity(const SceneStore::Tables& a_tables, const SceneStore::PlacementPlan* a_fresh)
	{
		auto& s = *impl;
		if (!s.seq)
			return;
		// A diagnostics frame: the render thread waits for the frame's producer.
		for (auto finished = s.done.load(std::memory_order_acquire); finished < s.seq; finished = s.done.load(std::memory_order_acquire))
			s.done.wait(finished, std::memory_order_acquire);
		const auto& rows = s.rows.Get();
		const auto& palettes = s.palettes.Get();
		std::vector<std::uint8_t> fresh(rows.size(), 0);
		if (a_fresh)
			for (const auto& item : a_fresh->written) {
				if (item.slot < fresh.size())
					fresh[item.slot] = 1;
				if (item.layerSlot < fresh.size())
					fresh[item.layerSlot] = 1;
			}
		const std::size_t count = std::min(rows.size(), a_tables.objects.size());
		BindlessPlacement expected;
		for (std::uint32_t slot = 0; slot < count; ++slot) {
			if (!SampleSlot(a_tables, slot, expected))
				continue;
			++s.parityChecks;
			if (fresh[slot]) {
				++s.parityFresh;
				continue;
			}
			if (std::memcmp(&expected, &rows[slot], sizeof(expected)) != 0 && s.parityDiffers++ == 0) {
				const auto& got = rows[slot];
				const char* what = std::memcmp(expected.world, got.world, sizeof(expected.world)) ? "world" :
				                   std::memcmp(expected.previousWorld, got.previousWorld, sizeof(expected.previousWorld)) ? "previous world" :
				                   std::memcmp(expected.bound, got.bound, sizeof(expected.bound)) ? "bound" :
				                   std::memcmp(expected.sunEntry, got.sunEntry, sizeof(expected.sunEntry)) ? "sun entry" :
				                                                                                               "fade node";
				const auto* geometry = a_tables.objectGeometry[slot];
				s.parityFirst = fmt::format("slot {} '{}': {} (engine x {:.2f} r {:.2f}, rows x {:.2f} r {:.2f})", slot, geometry->name.c_str() ? geometry->name.c_str() : "?",
					what, expected.world[3], expected.bound[3], got.world[3], got.bound[3]);
			}
			// A skinned record's palettes, as this frame's update left them.
			if (!(a_tables.objects[slot].flags & kObjectSkinned) || slot >= a_tables.boneRows.size() || !a_tables.boneRows[slot])
				continue;
			const std::uint32_t boneRows = a_tables.boneRows[slot];
			const auto at = PaletteRowsOf(a_tables.boneOffset[slot], boneRows);
			const float* current = nullptr;
			const float* previous = nullptr;
			if (std::size_t(at.previous) + boneRows > palettes.size() || !SkinPalettes(*a_tables.objectGeometry[slot], boneRows, current, previous))
				continue;
			++s.paletteChecks;
			const std::size_t bytes = std::size_t(boneRows) * sizeof(PaletteRow);
			if ((std::memcmp(&palettes[at.current], current, bytes) != 0 || std::memcmp(&palettes[at.previous], previous, bytes) != 0) && s.paletteDiffers++ == 0) {
				const auto* geometry = a_tables.objectGeometry[slot];
				s.paletteFirst = fmt::format("slot {} '{}' ({} rows)", slot, geometry->name.c_str() ? geometry->name.c_str() : "?", boneRows);
			}
		}
	}

	std::string FrameValues::Report()
	{
		auto& s = *impl;
		const auto frames = s.frames.exchange(0);
		if (!frames)
			return {};
		const double n = static_cast<double>(frames);
		auto line = fmt::format("[DCLF] frame values: {} frames; a frame {:.0f} items sampled ({:.1f} written slots kept for their transforms to settle), {:.1f} fade roots seeded, {:.1f} rows changed, {:.0f} skins ({:.1f} palette rows changed), "
								"{:.1f} shading items sampled ({:.1f} rows changed, {:.1f} wetness changed), "
								"{:.1f} runs ({:.1f} KB) sent; {:.0f} us sampling, {:.0f} us waiting for a buffer ({} waits), {:.0f} us sending, at most {} us; "
								"{} refused leases, {} palette size defects, {} failures",
			frames, s.sampled.exchange(0) / n, s.settled.exchange(0) / n, s.seeded.exchange(0) / n, s.rowsChanged.exchange(0) / n, s.skins.exchange(0) / n, s.paletteRowsChanged.exchange(0) / n,
			s.shadingSampled.exchange(0) / n, s.shadingChanged.exchange(0) / n, s.wetnessChanged.exchange(0) / n, s.runs.exchange(0) / n,
			s.bytes.exchange(0) / n / 1024.0, s.sampleUs.exchange(0) / n, s.pointWaitUs.exchange(0) / n, s.pointWaits.exchange(0), s.uploadUs.exchange(0) / n,
			s.maxTotalUs.exchange(0), s.refused.exchange(0), s.defects.exchange(0), s.failures.exchange(0));
		if (s.parityChecks) {
			line += fmt::format("; parity against the engine: {} rows checked, {} differ{}{} ({} written this frame, the next frame's); {} palettes checked, {} differ{}{}",
				s.parityChecks, s.parityDiffers, s.parityDiffers ? " <- DIFFER; first: " : " <- OK", s.parityDiffers ? s.parityFirst : std::string(), s.parityFresh,
				s.paletteChecks, s.paletteDiffers, s.paletteDiffers ? " <- DIFFER; first: " : " <- OK", s.paletteDiffers ? s.paletteFirst : std::string());
			s.parityChecks = s.parityDiffers = s.parityFresh = s.paletteChecks = s.paletteDiffers = 0;
			s.parityFirst.clear();
			s.paletteFirst.clear();
		}
		return line;
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
	bool FrameValues::Kick(std::shared_ptr<const SceneStore::PlacementPlan>, std::vector<SceneStore::ShadingItem>, std::vector<SceneStore::WetnessValue>,
		std::vector<SceneStore::FadeSeedItem>, std::vector<SceneStore::TreeSeedItem>, std::shared_ptr<const FrameGlobals>, FrameUploads) { return false; }
	const std::vector<TreeStatic>* FrameValues::TreeSeedsIfDone(bool) const { return nullptr; }
	const std::vector<FadeRootStatic>* FrameValues::FadeSeedsIfDone(bool) const { return nullptr; }
	void FrameValues::Skip() {}
	void FrameValues::EndFrame() {}
	const std::vector<BindlessPlacement>* FrameValues::RowsIfDone() const { return nullptr; }
	const std::vector<FrameValues::PaletteRow>* FrameValues::PalettesIfDone() const { return nullptr; }
	const std::vector<BindlessShading>* FrameValues::ShadingIfDone(bool) const { return nullptr; }
	void FrameValues::CheckParity(const SceneStore::Tables&, const SceneStore::PlacementPlan*) {}
	std::string FrameValues::Report() { return {}; }
#endif
}
