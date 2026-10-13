// IndirectDraws' GPU resources: growth (Adopt, Growths, GrowableRows), the main, scene, shadow and view resources created on the
// render thread (Setup, EnsureSceneBuffers, SetupShadow, SetupView), and the reserves the revision code runs before a make.
#if defined(CS_HAS_RENDER_GRAPH) && defined(CS_HAS_ORG_MODULE_SERVICES)
#	include "Features/DrawcallLimitFix/Draws/IndirectDraws/Internal.h"
#include "Features/DrawcallLimitFix/Scene/FrameView/FrameView.h"
#	include "Features/TerrainBlending.h"
#	include "Features/DrawcallLimitFix/Diagnostics/Draws/Events.h"

namespace DCLF::Draws
{
	namespace
	{
		/** @brief A device-local buffer the shaders read by device address. */
		std::shared_ptr<org::Buffer> DeviceBuffer(std::uint64_t a_bytes, const char* a_name)
		{
			auto created = org::Buffer::CreateShared(rhi::HeapType::DeviceLocal, a_bytes, false);
			created->SetName(a_name);
			return created;
		}

		/** @brief A structured buffer the shaders read through its SRV; a_index is the SRV's descriptor slot. */
		std::shared_ptr<org::Buffer> StructuredBuffer(std::uint32_t a_elements, std::uint32_t a_stride, const char* a_name, std::uint32_t& a_index, bool a_unorderedAccess = false)
		{
			auto created = org::Buffer::CreateUnmaterializedStructuredBuffer(a_elements, a_stride, a_unorderedAccess);
			created->SetName(a_name);
			created->Materialize();
			a_index = created->GetSRVInfo(0).slot.index;
			return created;
		}

		/** @brief A D3D11 view of a word buffer's first a_bytes, for immediate-context readbacks; words past it read as zero. */
		auto WrapWords(org::Buffer& a_buffer, std::uint64_t a_bytes)
		{
			D3D11_BUFFER_DESC desc{};
			desc.ByteWidth = static_cast<UINT>(a_bytes);
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
			desc.StructureByteStride = sizeof(std::uint32_t);
			return RenderGraphRuntime::Get().WrapBuffer(a_buffer, desc);
		}

		std::uint64_t AddressOf(rhi::Device a_device, org::Buffer& a_buffer)
		{
			return a_device.GetBufferDeviceAddress({ a_buffer.GetAPIResource().GetHandle(), 0 });
		}

		// Tree LOD's table sizes in words (TreeLod.h): per shape slot its row, records and visible/retest list entries; per mesh slot
		// its row; plus the list header.
		std::uint32_t TreeLodShapeWords(std::uint32_t a_slots) { return a_slots * static_cast<std::uint32_t>(sizeof(TreeLod::ShapeRow) / 4); }
		std::uint32_t TreeLodInstanceWords(std::uint32_t a_slots) { return a_slots * TreeLod::kMaxGroupInstances * static_cast<std::uint32_t>(sizeof(TreeLod::Instance) / 4); }
		std::uint32_t TreeLodMeshWords(std::uint32_t a_slots) { return a_slots * static_cast<std::uint32_t>(sizeof(TreeLod::MeshRow) / 4); }
		std::uint32_t TreeLodVisibleWords(std::uint32_t a_slots) { return TreeLod::kVisibleHeaderWords + 2 * a_slots * TreeLod::kMaxGroupInstances; }
	}
}

namespace DCLF::Draws
{
	void Adopt(Growth&& a_growth, const std::function<void()>& a_adopted)
	{
		// In revision mode an immediate adoption moves versions past what the revisions name: counted per buffer (Growths::atOnce).
		if (Growths::Deferred())
			for (const auto& [buffer, version] : a_growth.versions)
				++Growths::Get().atOnce[buffer->Get()->GetName()];
		for (const auto& [buffer, version] : a_growth.versions)
			buffer->Adopt(version);
		if (!a_growth.versions.empty())
			VersionRegistry::Get().Changed();
		if (a_adopted)
			a_adopted();
		if (auto* host = RenderGraphRuntime::Get().Host())
			host->NoteNewVersions();
	}

	ShadowLatchLayout InitialShadowLatchLayout()
	{
		const bool smallSlots = SwitchValue(Switch::TableStart) == "small";
		return { smallSlots ? 2u : kInitialShadowViewSlots, smallSlots ? 16u : kInitialShadowKeySlots, smallSlots ? 1u : kInitialShadowRasterStates,
			smallSlots ? 1u : kInitialSunProcesses };
	}

	bool GrowableRows::Create(std::uint32_t a_stride, std::uint32_t a_rows, const char* a_name)
	{
		auto* host = RenderGraphRuntime::Get().Host();
		if (!host)
			return false;
		stride = a_stride;
		capacity = std::max(a_rows, 1u);
		name = a_name;
		buffer = MakeVersioned(DeviceBuffer(std::uint64_t(capacity) * stride, a_name));
		address = AddressOf(host->GetDesc().device, *buffer->Get());
		++generation;
		return address != 0;
	}

	bool GrowableRows::Reserve(std::uint32_t a_rows, const std::function<void(std::uint64_t)>& a_adopted, const RowsCapture* a_capture)
	{
		if (!buffer)
			return false;
		auto* host = RenderGraphRuntime::Get().Host();
		if (!host)
			return false;
		const auto device = host->GetDesc().device;
		auto& graphGrowths = Growths::Get();
		const bool deferred = a_capture && Growths::Deferred();
		// The rows the newest growth holds or asks for (in flight counts as held), as the reserving side knows them; never the
		// capacity the frame's adoption writes.
		const std::uint64_t held = deferred ? graphGrowths.Held(buffer) : 0;
		const std::uint32_t current = held ? static_cast<std::uint32_t>(held / stride) : capacity;
		if (a_rows <= current)
			return false;
		// Doubling: a filling table grows a handful of times, not every frame.
		std::uint32_t rows = std::max(current, 1u);
		while (rows < a_rows)
			rows *= 2;
		logger::info("[DCLF] {}: {} rows grown to {} ({} KB){}", name, current, rows, std::uint64_t(rows) * stride / 1024, deferred ? ", as graph work" : "");
		++growths;
		if (!deferred) {
			// A new version; the old one lives for its holders and is released through ORG's deletion queue.
			capacity = rows;
			Adopt(std::move(Growth{}.Bytes(buffer, std::uint64_t(rows) * stride)), [this, device, a_adopted] {
				address = AddressOf(device, *buffer->Get());
				++generation;
				if (a_adopted)
					a_adopted(0);
			});
			return true;
		}
		// Graph work: the new version is made and filled with the captured rows (laid out for its own address) off-thread; capacity,
		// address and generation move when a revision naming it is selected.
		Growths::Part part{ buffer, 0, std::uint64_t(rows) * stride, {} };
		if (a_capture->bytes)
			part.contents = [device, bytesOf = a_capture->bytes](const org::BufferVersion& a_version, std::vector<org::services::VersionFill>& a_fills,
								std::shared_ptr<const void>& a_contents) {
				auto bytes = std::make_shared<std::vector<std::byte>>(bytesOf(AddressOf(device, *a_version.buffer)));
				if (bytes->empty())
					return;
				a_fills.push_back({ 0, { { bytes->data(), bytes->size() } } });
				a_contents = std::move(bytes);
			};
		graphGrowths.Post(nullptr, std::weak_ptr<const void>(life), nullptr, { std::move(part) },
			[this, device, rows, held = a_capture->held, life = std::weak_ptr<int>(life), a_adopted] {
				if (life.expired())
					return;
				capacity = rows;
				address = AddressOf(device, *buffer->Get());
				++generation;
				if (a_adopted)
					a_adopted(held);
			});
		return true;
	}

	std::uint64_t GrowableRows::RevisionAddress() const
	{
		// The version a revision made now names: a ready growth's, adopted or not, else the current one.
		if (const auto named = buffer ? Growths::Get().Named(buffer) : nullptr)
			if (auto* host = RenderGraphRuntime::Get().Host())
				return AddressOf(host->GetDesc().device, *named->buffer);
		return address;
	}

	Growths& Growths::Get()
	{
		static Growths growths;
		return growths;
	}

	std::uint64_t Growths::Asked(const Versioned& a_buffer) const
	{
		const std::uint64_t through = adoptedPublished.load(std::memory_order_acquire);
		for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
			if ((*it)->adoptedSeen || (*it)->serial <= through)
				continue;
			for (const auto& part : (*it)->parts)
				if (part.buffer == a_buffer)
					return part.Bytes();
		}
		return 0;
	}

	std::uint64_t Growths::Held(const Versioned& a_buffer) const
	{
		for (auto it = entries.rbegin(); it != entries.rend(); ++it)
			for (const auto& part : (*it)->parts)
				if (part.buffer == a_buffer)
					return part.Bytes();
		return 0;
	}

	void Growths::Post(const void* a_owner, std::weak_ptr<const void> a_life, std::shared_ptr<const void> a_sizing, std::vector<Part> a_parts, std::function<void()> a_adopted)
	{
		std::erase_if(a_parts, [](const Part& a_part) { return !a_part.buffer; });
		if (!Deferred()) {
			// Without revisions: made and adopted now; a new version holds nothing, its users send the contents.
			Growth growth;
			for (const auto& part : a_parts)
				if (part.elements)
					growth.Structured(part.buffer, part.elements);
				else
					growth.Bytes(part.buffer, part.bytes);
			Adopt(std::move(growth), a_adopted);
			return;
		}
		auto entry = std::make_shared<Entry>();
		entry->serial = ++serial;
		entry->owner = a_owner;
		entry->life = std::move(a_life);
		entry->sizing = std::move(a_sizing);
		entry->parts = std::move(a_parts);
		entry->adopted = std::move(a_adopted);
		++requested;
		// Graph work: the preparation pool makes the versions and queues their fills; ready once the copies complete. No queue uses
		// a version before adoption; preparations for a revision naming it resolve it. The uploader is the frame's posted one (never
		// the host's, which a build replaces on the owner's thread).
		const auto uploader = uploads.lock();
		std::vector<org::services::VersionGrowthRequest> requests;
		for (const auto& part : entry->parts) {
			auto& request = requests.emplace_back();
			request.buffer = part.buffer;
			request.elements = part.elements;
			request.bytes = part.bytes;
			request.contentsOf = part.contents;
			request.uploads = part.contents ? uploader : nullptr;
		}
		Runtime::GrowthWork work;
		work.produce = [requests = std::move(requests)] {
			ZoneScopedN("CS.DCLF.Growth");
			auto grown = org::services::GrowVersions(requests);
			return Runtime::GrowthWork::Produced{ std::make_shared<const std::vector<std::shared_ptr<const org::BufferVersion>>>(std::move(grown.versions)),
				std::move(grown.ready) };
		};
		// On the coordinator lane: versions first, then the state that publishes them (Settle reads the state first).
		work.done = [entry](std::shared_ptr<const void> a_value, std::string a_error) {
			const auto* versions = static_cast<const std::vector<std::shared_ptr<const org::BufferVersion>>*>(a_value.get());
			if (versions && versions->size() == entry->parts.size()) {
				entry->versions = *versions;
				entry->state.store(Entry::kReady, std::memory_order_release);
			} else {
				entry->error = a_error.empty() ? std::string("a growth made the wrong versions") : std::move(a_error);
				entry->state.store(Entry::kFailed, std::memory_order_release);
			}
			// No revision is sealed while a change is pending.
			WakeSnapshotBuilder();
		};
		if (!SceneScheduler::Graph().PostGrowth(std::move(work))) {
			entry->error = "the scene graph refused it";
			entry->state.store(Entry::kFailed, std::memory_order_release);
		}
		entries.push_back(std::move(entry));
	}

	void Growths::Prune()
	{
		// An adopted entry stays the producer's record of its owner's sizing and buffers' versions until a newer adopted change covers
		// its owner and every buffer, or its owner is gone.
		const std::uint64_t through = adoptedPublished.load(std::memory_order_acquire);
		bool any = false;
		for (const auto& entry : entries)
			if (!entry->adoptedSeen && entry->serial <= through) {
				entry->adoptedSeen = true;
				any = true;
			}
		if (any)
			++stamp;
		auto covered = [&](std::size_t a_at) {
			const auto& entry = *entries[a_at];
			auto coveredLater = [&](auto&& a_matches) {
				for (std::size_t later = a_at + 1; later < entries.size(); ++later)
					if (entries[later]->adoptedSeen && entries[later]->Alive() && a_matches(*entries[later]))
						return true;
				return false;
			};
			if (entry.owner && !coveredLater([&](const Entry& a_later) { return a_later.owner == entry.owner; }))
				return false;
			for (const auto& part : entry.parts)
				if (!coveredLater([&](const Entry& a_later) {
						return std::any_of(a_later.parts.begin(), a_later.parts.end(), [&](const Part& a_part) { return a_part.buffer == part.buffer; });
					}))
					return false;
			return true;
		};
		std::vector<bool> drop(entries.size(), false);
		for (std::size_t e = 0; e < entries.size(); ++e)
			drop[e] = !entries[e]->Alive() || (entries[e]->adoptedSeen && covered(e));
		std::vector<std::shared_ptr<Entry>> kept;
		kept.reserve(entries.size());
		for (std::size_t e = 0; e < entries.size(); ++e)
			if (!drop[e])
				kept.push_back(std::move(entries[e]));
		entries = std::move(kept);
	}

	bool Growths::Settle()
	{
		bool pending = false;
		// An owner's later changes build on a failed one (LatestSizing), so they go with it.
		std::vector<const void*> failedOwners;
		std::erase_if(entries, [&](const auto& a_entry) {
			if (a_entry->owner && std::find(failedOwners.begin(), failedOwners.end(), a_entry->owner) != failedOwners.end()) {
				++failed;
				return true;
			}
			const auto state = a_entry->state.load(std::memory_order_acquire);
			if (state == Entry::kFailed && a_entry->owner)
				failedOwners.push_back(a_entry->owner);
			switch (state) {
			case Entry::kPending:
				pending = true;
				return false;
			case Entry::kReady:
				if (!std::exchange(a_entry->ready, true))
					++stamp;
				return false;
			default:
				// The next reserve that needs it asks again.
				++failed;
				if (failuresLogged++ < 8) {
					std::uint64_t bytes = 0;
					for (const auto& part : a_entry->parts)
						bytes += part.Bytes();
					logger::error("[DCLF] a growth of {} buffers ({} KB) failed: {}", a_entry->parts.size(), bytes / 1024, a_entry->error);
				}
				return true;
			}
		});
		return pending;
	}

	std::shared_ptr<const org::BufferVersion> Growths::Ready(const Versioned& a_buffer) const
	{
		const std::uint64_t through = adoptedPublished.load(std::memory_order_acquire);
		for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
			const auto& entry = **it;
			if (!entry.ready || entry.adoptedSeen || entry.serial <= through)
				continue;
			for (std::size_t i = 0; i < entry.parts.size() && i < entry.versions.size(); ++i)
				if (entry.parts[i].buffer == a_buffer)
					return entry.versions[i];
		}
		return nullptr;
	}

	std::shared_ptr<const org::BufferVersion> Growths::Named(const Versioned& a_buffer) const
	{
		for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
			const auto& entry = **it;
			if (!entry.ready)
				continue;
			for (std::size_t i = 0; i < entry.parts.size() && i < entry.versions.size(); ++i)
				if (entry.parts[i].buffer == a_buffer)
					return entry.versions[i];
		}
		return nullptr;
	}

	std::shared_ptr<const Growths::NamedChanges> Growths::Naming() const
	{
		// Every ready change not yet adopted, including those an earlier revision named, so a revision selected after others were
		// passed over adopts theirs too.
		auto named = std::make_shared<NamedChanges>();
		const std::uint64_t through = adoptedPublished.load(std::memory_order_acquire);
		for (const auto& entry : entries)
			if (entry->ready && !entry->adoptedSeen && entry->serial > through && entry->Alive())
				named->entries.push_back(entry);
		return named;
	}

	bool Growths::AdoptNamed(const NamedChanges& a_named)
	{
		// In request order: later changes build on earlier ones.
		bool any = false;
		for (const auto& entry : a_named.entries) {
			if (!entry || entry->serial <= adoptedThrough)
				continue;
			adoptedThrough = entry->serial;
			// An owner that is gone adopts nothing.
			if (!entry->Alive())
				continue;
			for (std::size_t i = 0; i < entry->parts.size() && i < entry->versions.size(); ++i)
				entry->parts[i].buffer->Adopt(entry->versions[i]);
			if (entry->adopted)
				entry->adopted();
			adopted.fetch_add(1, std::memory_order_relaxed);
			any = true;
		}
		// The producer takes it in at its next make (Prune).
		adoptedPublished.store(adoptedThrough, std::memory_order_release);
		if (!any)
			return false;
		VersionRegistry::Get().Changed();
		if (auto* host = RenderGraphRuntime::Get().Host())
			host->NoteNewVersions();
		return true;
	}

	namespace
	{
		/** @brief The largest max count one indirect draw may have on this device (IndirectCommandsFeatureInfo::maxSequenceCount). */
		std::uint32_t DeviceMaxSequences()
		{
			static const std::uint32_t max = [] {
				::IndirectCommandsFeatureInfo indirect{};
				auto* host = RenderGraphRuntime::Get().Host();
				if (!host || host->GetDesc().device.QueryFeatureInfo(&indirect.header) != rhi::Result::Ok)
					return 0u;
				logger::info("[DCLF] indirect draws: up to {} sequences per call", indirect.maxSequenceCount);
				return indirect.maxSequenceCount;
			}();
			return max;
		}

		/**
		 * @brief One object slot's share of the scene draw bound (one per object, or per skin partition; any epoch's draws fit in
		 * it): its draws and the pipeline slot whose Z-prepass bucket they count toward. Decals count toward none: the depth
		 * segment gets no decals.
		 */
		std::pair<std::uint32_t, std::uint32_t> DrawShareOf(const SceneTables& a_tables, std::size_t a_slot, bool a_partitioned)
		{
			const auto& object = a_tables.objects[a_slot];
			if (object.flags & kObjectFree)
				return { 0u, DrawBoundStore::kNoPipeline };
			const std::uint32_t produced = PartitionDraws(a_partitioned ? a_tables.skinPartitions[a_slot] : 0u);
			return { produced, (object.flags & kObjectDecal) ? DrawBoundStore::kNoPipeline : object.pipelineIndex };
		}

		/** @brief What a_slot casts with (DrawBoundStore::ShadowShare), given its draws. */
		DrawBoundStore::ShadowShare ShadowShareOf(const SceneTables& a_tables, std::size_t a_slot, std::uint32_t a_produced)
		{
			DrawBoundStore::ShadowShare out;
			const auto& object = a_tables.objects[a_slot];
			if ((object.flags & kObjectFree) || !a_produced)
				return out;
			out.produced = a_produced;
			out.caster = !(object.flags & kObjectNoShadow);
			out.technique = a_slot < a_tables.shadowTechnique.size() ? a_tables.shadowTechnique[a_slot] : 0u;
			for (std::uint32_t v = 0; v < kOcclusionViews; ++v)
				out.occlusion[v] = a_slot < a_tables.occlusionTechnique[v].size() ? a_tables.occlusionTechnique[v][a_slot] : 0u;
			out.rasterFlags = (object.flags & kObjectTwoSided) ? kRasterTwoSided : 0u;
			out.vertexLayout = object.geometryIndex < a_tables.geometries.size() ? VertexLayoutOf(a_tables.geometries[object.geometryIndex].vertexDesc) : 0ull;
			return out;
		}

		/** @brief Adds a_share's draws to its key in each mode it draws in, or takes them out. */
		void CountShadowShare(ModeKeyDraws& a_out, const DrawBoundStore::ShadowShare& a_share, bool a_add)
		{
			if (!a_share.produced)
				return;
			for (std::uint32_t m = 0; m < kShadowModeCount; ++m) {
				ShadowPipelineKey key;
				if (!a_share.KeyOf(m, key))
					continue;
				if (a_add) {
					a_out[m][key] += a_share.produced;
				} else if (const auto it = a_out[m].find(key); it != a_out[m].end() && (it->second -= a_share.produced) == 0) {
					a_out[m].erase(it);
				}
			}
		}

		/**
		 * @brief Adds shadow view slot buffers up to a_slots at the revision code's newest sizing. Render thread: the passes declare
		 * them (a graph build) and the counters' readback views are D3D11's.
		 */
		void AddShadowViewSlots(ShadowResources& a_state, std::uint32_t a_slots, std::uint32_t a_draws, std::uint32_t a_words)
		{
			const std::uint32_t draws = a_draws;
			for (auto s = static_cast<std::uint32_t>(a_state.sequences.size()); s < a_slots; ++s) {
				a_state.sequences.push_back(MakeVersioned(CreateWords(kShadowClasses * std::uint64_t(draws) * sizeof(DrawSequence) / 4, true,
					fmt::format("cs.dclf.shadow.sequences{}", s).c_str())));
				a_state.count.push_back(CreateWords(kCountWords, true, fmt::format("cs.dclf.shadow.draw-count{}", s).c_str()));
				a_state.countD3D11.push_back(WrapWords(*a_state.count.back(), kCountWords * sizeof(std::uint32_t)));
				a_state.bucketCounts.push_back(MakeVersioned(CreateWords(std::max(a_words, 64u), true,
					fmt::format("cs.dclf.shadow.bucket-counts{}", s).c_str())));
			}
		}

		/** @brief A draw range one call cannot execute or the sort cannot rank: a hard failure until draws split across calls. */
		[[noreturn]] void SequenceLimitFailure(const char* a_what, std::uint32_t a_draws, std::uint32_t a_limit)
		{
			const auto message = fmt::format("Drawcall Limit Fix: {} needs {} draws in one indirect draw, over the limit of {}", a_what, a_draws, a_limit);
			logger::critical("[DCLF] {}", message);
			spdlog::default_logger()->flush();
			stl::report_and_fail(message);
		}

		void CheckSequenceLimits(const char* a_what, std::uint32_t a_draws, bool a_sorted)
		{
			const std::uint32_t device = DeviceMaxSequences();
			if (device && a_draws > device)
				SequenceLimitFailure(a_what, a_draws, device);
			if (a_sorted && a_draws > kSortRankLimit)
				SequenceLimitFailure(a_what, a_draws, kSortRankLimit);
		}
	}
}

namespace DCLF
{
	bool IndirectDraws::Impl::Setup(const Capture& a_capture, bool a_depthOnly)
	{
		auto* host = RenderGraphRuntime::Get().Host();
		if (!host)
			return NotReady(0, "no render graph");
		if (a_depthOnly) {
			// The native depth pass binds no colour targets: the Z-prepass reuses the main pass's resources (none on the first frame).
			return resources != nullptr;
		}
		if (a_capture.targetCount == 0)
			return NotReady(1, "no render targets bound");
		D3D11_TEXTURE2D_DESC target{};
		a_capture.targets[0]->GetDesc(&target);
		const auto& targets = DrawPipelines::Get().Targets();
		std::uint32_t lights, list, grid;
		const bool lightLimitFix = ORGLightCulling::Get().GetShaderResourceIndices(lights, list, grid);
		auto frameBuffers = FrameBuffersOf(a_capture, lightLimitFix);
		const bool sameFrameBuffers = resources && resources->frameBuffers.size() == frameBuffers.size() &&
		                              std::equal(frameBuffers.begin(), frameBuffers.end(), resources->frameBuffers.begin(), [](const auto& a, const auto& b) { return a.SameShape(b); });
		if (resources && targets == formats && target.Width == width && target.Height == height && sameFrameBuffers &&
			resources->lightLimitFix == lightLimitFix)
			return true;

		auto device = host->GetDesc().device;
		auto state = std::make_shared<Resources>();
		// The main rows, grown before each epoch to the scene's material and pipeline slots.
		const bool smallRows = SwitchValue(Switch::TableStart) == "small";
		if (!state->materialRows.Create(kMaterialRowBytes, smallRows ? 4u : 1024u, "cs.dclf.material-rows") ||
			!state->pipelineRows.Create(kPipelineRowBytes, smallRows ? 4u : 256u, "cs.dclf.pipeline-rows")) {
			logger::error("[DCLF] The main rows have no device address");
			return false;
		}
		if (!EnsureSceneBuffers(device)) {
			logger::error("[DCLF] The face positions buffer has no device address");
			return false;
		}
		state->scene = scene;
		// The sequence ranges (SequenceSlots), grown before each epoch to every draw the scene can produce (ReserveMainSequences).
		const bool smallTables = SwitchValue(Switch::TableStart) == "small";
		state->sequenceDraws = smallTables ? 64u : kInitialSequenceDraws;
		state->sequenceDecals = smallTables ? 16u : kInitialDecalDraws;
		state->sequences = MakeVersioned(CreateWords(SequenceSlots(state->sequenceDraws, state->sequenceDecals) * sizeof(DrawSequence) / 4, true, "cs.dclf.sequences"));
		state->askedSequenceDraws.store(state->sequenceDraws, std::memory_order_relaxed);
		state->count = CreateWords(kCountWords, true, "cs.dclf.draw-count");
		// One word per object: the depth segment's culling verdict, so the colour segment draws exactly the same set.
		state->objectCapacity = scene->objectCapacity;
		state->visibility = MakeVersioned(CreateWords(state->objectCapacity, true, "cs.dclf.visibility"));
		state->frustum = MakeVersioned(CreateWords(state->objectCapacity, true, "cs.dclf.frustum"));
		if (PassStats::Enabled()) {
			auto passStats = std::make_shared<PassStats>();
			const std::uint32_t slots = host->FrameSlots();
			rhi::QueryPoolDesc desc{};
			desc.type = rhi::QueryType::PipelineStatistics;
			desc.count = slots * PassStats::kKinds;
			desc.statsMask = rhi::PipelineStatBits::PS_IAVertices | rhi::PipelineStatBits::PS_IAPrimitives | rhi::PipelineStatBits::PS_VSInvocations |
			                 rhi::PipelineStatBits::PS_PSInvocations;
			desc.requireAllStats = true;
			if (device.CreateQueryPool(desc, passStats->pool) != rhi::Result::Ok || !passStats->pool) {
				logger::warn("[DCLF] pass statistics: no pipeline statistics query pool");
			} else {
				for (std::uint32_t i = 0; i < slots; ++i)
					passStats->readback.push_back(org::Buffer::CreateShared(rhi::HeapType::Readback, std::uint64_t(PassStats::kKinds) * PassStats::kFields * sizeof(std::uint64_t)));
				passStats->written.resize(slots);
				state->passStats = std::move(passStats);
				logger::info("[DCLF] pass statistics on: {} frame slots", slots);
			}
		}
		state->preprocessMain = PreprocessStates::Create(device, host->FrameSlots(), "the main segment");
		// The forward epoch records in its own epoch, so it needs its own state list.
		state->preprocessForward = PreprocessStates::Create(device, host->FrameSlots(), "the forward epoch");
		state->inputs = MakeVersioned(CreateWords(std::uint64_t(state->objectCapacity) * sizeof(DrawInput) / 4, false, "cs.dclf.draw-inputs"));
		state->inputsDepth = MakeVersioned(CreateWords(std::uint64_t(state->objectCapacity) * sizeof(DrawInput) / 4, false, "cs.dclf.draw-inputs-depth"));
		state->buildDraws = ComputeProgram::Load(device, { .source = kBuildDrawsShader, .constantWords = kBuildDrawsConstantWords });
		if (!state->buildDraws)
			return NotReady(7, "the BuildDraws program could not be created");
		// Without the forward sort the forward epoch draws nothing.
		state->forwardSort = ComputeProgram::Load(device, { .source = kForwardSortShader, .constantWords = kForwardSortConstantWords });
		if (!state->forwardSort)
			logger::warn("[DCLF] the forward sort program could not be created: the forward epoch draws nothing");
		state->dispatchSignature = CreateDispatchSignature(device, state->buildDraws->layout->GetHandle());
		if (!state->dispatchSignature)
			return NotReady(7, "the BuildDraws dispatch signature could not be created");
		// The latch is the revision producer's. Sorting by pipeline: BuildDraws appends in thread order, so unsorted nearly every
		// sequence would switch pipeline.
		state->sort = DrawSort::Create(device, state->sequenceDraws);
		// The Z-prepass's bucket count words (grown with the pipeline slots) and the scene's index pool.
		state->zBucketCountWords = 64;
		state->zBucketCounts[0] = MakeVersioned(CreateWords(state->zBucketCountWords, true, "cs.dclf.z.bucket-counts"));
		state->zBucketCounts[1] = MakeVersioned(CreateWords(state->zBucketCountWords, true, "cs.dclf.z.bucket-counts2"));
		state->pool = EnsureIndexPool(*scene, device, smallTables);
		if (!state->pool)
			return NotReady(7, "the index pool's copy program could not be created");
		if (Diagnostics::Observe().builds)
			state->sequencesD3D11 = WrapWords(*state->sequences->Get(), SequenceSlots(state->sequenceDraws, state->sequenceDecals) * sizeof(DrawSequence));
		// The draw-count buffer is read back for the culling counters, parity or not; the view covers every word (a short view
		// would read zeros that look clean).
		state->countD3D11 = WrapWords(*state->count, kCountWords * sizeof(std::uint32_t));
		if (Diagnostics::Observe().claims) {
			state->visibilityD3D11 = WrapWords(*state->visibility->Get(), std::uint64_t(state->objectCapacity) * sizeof(std::uint32_t));
			state->frustumD3D11 = WrapWords(*state->frustum->Get(), std::uint64_t(state->objectCapacity) * sizeof(std::uint32_t));
		}
		state->frameConstants = DeviceBuffer(kFrameConstantBytes, "cs.dclf.frame-constants");
		state->frameConstantsAddress = AddressOf(device, *state->frameConstants);

		if (!state->frameConstantsAddress) {
			logger::error("[DCLF] Draw data buffers have no device address");
			return false;
		}

		state->width = target.Width;
		state->height = target.Height;
		if (Policy::RecordFoliagePixels()) {
			auto foliage = std::make_shared<Resources::FoliageRecord>();
			foliage->width = target.Width;
			foliage->height = target.Height;
			const std::uint64_t pixels = std::uint64_t(target.Width) * target.Height;
			for (std::uint32_t h = 0; h < 2; ++h) {
				foliage->ids[h] = CreateWords(pixels, true, fmt::format("cs.dclf.foliage-ids{}", h).c_str());
				foliage->colours[h] = CreateWords(pixels * kFoliageColourWords, true, fmt::format("cs.dclf.foliage-colours{}", h).c_str());
				foliage->idsAddress[h] = AddressOf(device, *foliage->ids[h]);
				foliage->coloursAddress[h] = AddressOf(device, *foliage->colours[h]);
			}
			foliage->results = CreateWords(kFoliageResultWords, true, "cs.dclf.foliage-results");
			foliage->owners = CreateWords(pixels * 2, true, "cs.dclf.foliage-owners");
			foliage->ownersIndex = foliage->owners->GetUAVShaderVisibleInfo(0).slot.index;
			foliage->resultsAddress = AddressOf(device, *foliage->results);
			// The record stands only with the observers' compare pass.
			bool ready = false;
			Diagnostics::Notify(Diagnostics::FoliageRecordMade{ *foliage, device, host->FrameSlots(), ready });
			if (ready)
				state->foliage = std::move(foliage);
		}

		// The main pass's targets and depth, imported.
		state->targetCount = targets.colorCount;
		for (std::uint32_t i = 0; i < targets.colorCount; ++i) {
			DxvkOrgInteropResourceInfo info{};
			if (!a_capture.targets[i] || !RenderGraphRuntime::Get().DescribeResource(a_capture.targets[i].get(), info) || info.kind != DXVK_ORG_INTEROP_RESOURCE_IMAGE)
				return NotReady(5, "a main-pass target cannot be described");
			org::TextureDescription desc{};
			desc.format = rhi::helpers::ToRHI(targets.colors[i]);
			desc.channels = 4;
			desc.hasRTV = true;
			desc.rtvFormat = desc.format;
			state->native[i] = ImportImage(device, info.image, desc, "DCLF native target");
			if (!state->native[i])
				return NotReady(6, "a main-pass target could not be imported");
		}
		{
			DxvkOrgInteropResourceInfo info{};
			if (!a_capture.depth || !RenderGraphRuntime::Get().DescribeResource(a_capture.depth.get(), info) || info.kind != DXVK_ORG_INTEROP_RESOURCE_IMAGE)
				return NotReady(10, "the main-pass depth cannot be described");
			// Attachable and readable from one import: two imports would be unrelated resources to the graph, with no barrier
			// between the depth draws and the HZB read (which would then reduce the cleared far plane).
			org::TextureDescription depthDesc{};
			depthDesc.imageDimensions.push_back({ target.Width, target.Height, 0, 0 });
			depthDesc.format = rhi::helpers::ToRHI(targets.depth);
			depthDesc.channels = 1;
			depthDesc.hasDSV = true;
			depthDesc.dsvFormat = depthDesc.format;
			depthDesc.hasSRV = true;
			depthDesc.srvFormat = DepthReadFormat(depthDesc.format);
			state->nativeDepth = ImportImage(device, info.image, depthDesc, "DCLF native depth");
			if (!state->nativeDepth)
				return NotReady(11, "the main-pass depth could not be imported");
			// Terrain Blending's terrain depth shares the main depth's description; the Z-prepass draws near terrain blocks into it
			// as that feature's depth hook does.
			if (const auto& terrainBlending = globals::features::terrainBlending; terrainBlending.loaded && terrainBlending.terrainDepth.texture) {
				DxvkOrgInteropResourceInfo terrainInfo{};
				if (!RenderGraphRuntime::Get().DescribeResource(terrainBlending.terrainDepth.texture, terrainInfo) || terrainInfo.kind != DXVK_ORG_INTEROP_RESOURCE_IMAGE)
					return NotReady(12, "Terrain Blending's terrain depth cannot be described");
				state->terrainDepth = ImportImage(device, terrainInfo.image, depthDesc, "DCLF terrain blending depth");
				if (!state->terrainDepth)
					return NotReady(12, "Terrain Blending's terrain depth could not be imported");
			}
		}
		// The engine's refraction normals (kREFRACTION_NORMALS): FUN_1414b4860 binds them as RT0 for group 5, ISRefraction samples
		// them (Src1Tex). Imported in the engine RTV's format; without them DCLF draws no refraction.
		if (auto* renderer = globals::game::renderer) {
			const auto& normals = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kREFRACTION_NORMALS];
			DxvkOrgInteropResourceInfo normalsInfo{};
			if (normals.texture && normals.RTV && RenderGraphRuntime::Get().DescribeResource(normals.texture, normalsInfo) && normalsInfo.kind == DXVK_ORG_INTEROP_RESOURCE_IMAGE) {
				D3D11_RENDER_TARGET_VIEW_DESC rtvDesc{};
				normals.RTV->GetDesc(&rtvDesc);
				D3D11_TEXTURE2D_DESC textureDesc{};
				normals.texture->GetDesc(&textureDesc);
				org::TextureDescription normalsDesc{};
				normalsDesc.imageDimensions.push_back({ textureDesc.Width, textureDesc.Height, 0, 0 });
				normalsDesc.format = rhi::helpers::ToRHI(rtvDesc.Format);
				normalsDesc.channels = 4;
				normalsDesc.hasRTV = true;
				normalsDesc.rtvFormat = normalsDesc.format;
				state->refractionNormals = ImportImage(device, normalsInfo.image, normalsDesc, "DCLF refraction normals");
				state->refractionFormat = state->refractionNormals ? rtvDesc.Format : DXGI_FORMAT_UNKNOWN;
			}
			if (!state->refractionNormals)
				logger::warn("[DCLF] the refraction normals (kREFRACTION_NORMALS) could not be imported; DCLF draws no refraction");
		}

		// The HZB: mip 0 is half the depth's next power of two, so a mip is chosen from a screen extent by log2 alone. Padded
		// texels are filled with the far plane, which suppresses culling rather than causing it.
		auto nextPowerOfTwo = [](std::uint32_t a_value) {
			std::uint32_t result = 1;
			while (result < a_value)
				result <<= 1;
			return result;
		};
		state->hzbWidth = std::max(1u, nextPowerOfTwo(state->width) / 2);
		state->hzbHeight = std::max(1u, nextPowerOfTwo(state->height) / 2);
		state->hzbMips = 1;
		for (std::uint32_t size = std::max(state->hzbWidth, state->hzbHeight); size > 1; size >>= 1)
			++state->hzbMips;

		org::TextureDescription hzbDesc{};
		for (std::uint32_t mip = 0; mip < state->hzbMips; ++mip)
			hzbDesc.imageDimensions.push_back({ std::max(1u, state->hzbWidth >> mip), std::max(1u, state->hzbHeight >> mip), 0, 0 });
		hzbDesc.format = rhi::Format::R32_Float;
		hzbDesc.channels = 1;
		hzbDesc.hasUAV = true;
		hzbDesc.uavFormat = hzbDesc.format;
		hzbDesc.hasSRV = true;
		hzbDesc.srvFormat = hzbDesc.format;
		state->hzb = org::PixelBuffer::CreateSharedUnmaterialized(hzbDesc);
		state->hzb->SetName("cs.dclf.hzb");
		state->hzbCounter = CreateWords(1, true, "cs.dclf.hzb-counter");

		state->hzbProgram = ComputeProgram::Load(device, { .source = kHzbShader, .constantWords = kHzbConstantWords });
		if (!state->hzbProgram) {
			logger::warn("[DCLF] The HZB compute program could not be created; occlusion culling stays off");
			state->hzb.reset();
		}

		state->lightLimitFix = lightLimitFix;
		for (auto& frameBuffer : frameBuffers) {
			frameBuffer.copy = org::Buffer::CreateUnmaterializedStructuredBuffer(frameBuffer.elements, frameBuffer.stride, false);
			frameBuffer.copy->SetName(fmt::format("cs.dclf.frame-buffer.t{}", frameBuffer.textureRegister));
			frameBuffer.copy->Materialize();
		}
		state->frameBuffers = std::move(frameBuffers);

		resources = state;
		formats = targets;
		width = target.Width;
		height = target.Height;
		// The views draw from the main resources: rebuilt at their next import, before the graph is built (on the render thread).
		for (const auto kind : { ViewKind::Reflection, ViewKind::FirstPerson })
			if (const auto& view = View(kind).resources) {
				view->main = state;
				view->rebuildPending = true;
			}
		PostRevisionInputs();
		// The graph is rebuilt with the new resources on its next epoch.
		host->AddExtension(kExtensionId, [state] { return MakeMainOpaqueExtension(state); });
		logger::info("[DCLF] Main-pass graph resources: {} targets {}x{}, depth format {}", targets.colorCount, width, height, static_cast<int>(targets.depth));
		return true;
	}

	ScannedDrawBound Draws::ScanDrawBound(const SceneTables& a_tables)
	{
		ScannedDrawBound out;
		std::uint64_t draws = 0;
		const std::size_t objects = a_tables.objects.size();
		const bool partitioned = a_tables.skinPartitions.size() >= objects;
		out.perPipeline.assign(a_tables.pipelines.size(), 0u);
		for (std::size_t o = 0; o < objects; ++o) {
			const auto [produced, pipeline] = DrawShareOf(a_tables, o, partitioned);
			draws += produced;
			if (pipeline < out.perPipeline.size())
				out.perPipeline[pipeline] += produced;
			CountShadowShare(out.modeKeys, ShadowShareOf(a_tables, o, produced), true);
		}
		out.draws = static_cast<std::uint32_t>(std::min<std::uint64_t>(draws, UINT32_MAX));
		return out;
	}

	void IndirectDraws::Impl::UpdateDrawBound(const SceneTables& a_tables, std::uint32_t a_generation)
	{
		auto& s = producer.drawBound;
		const std::size_t count = a_tables.objects.size();
		const bool partitioned = a_tables.skinPartitions.size() >= count;
		const std::uint32_t generation = a_generation;  // the tables' own
		constexpr auto kNoPipeline = DrawBoundStore::kNoPipeline;
		auto set = [&](std::size_t a_slot) {
			const auto [produced, pipeline] = DrawShareOf(a_tables, a_slot, partitioned);
			// The casting share is unchanged by most logged changes (placement, shading).
			if (auto share = ShadowShareOf(a_tables, a_slot, produced); !(share == s.shadow[a_slot])) {
				CountShadowShare(s.modeKeyDraws, s.shadow[a_slot], false);
				CountShadowShare(s.modeKeyDraws, share, true);
				s.shadow[a_slot] = share;
				++s.shadowVersion;
			}
			s.draws -= s.produced[a_slot];
			if (s.pipeline[a_slot] != kNoPipeline)
				s.perPipeline[s.pipeline[a_slot]] -= s.produced[a_slot];
			s.produced[a_slot] = produced;
			s.pipeline[a_slot] = pipeline;
			s.draws += produced;
			if (pipeline != kNoPipeline) {
				if (pipeline >= s.perPipeline.size())
					s.perPipeline.resize(std::size_t(pipeline) + 1, 0u);
				s.perPipeline[pipeline] += produced;
			}
		};
		++s.updates;
		// Every slot again on new tables, a lost log, fewer slots, or the partition column coming or going (unlogged).
		if (!s.cursor.Continues(a_tables.changeLog, generation) || s.produced.size() > count || s.partitioned != partitioned) {
			s.produced.assign(count, 0u);
			s.pipeline.assign(count, kNoPipeline);
			s.perPipeline.clear();
			s.shadow.assign(count, {});
			for (auto& keys : s.modeKeyDraws)
				keys.clear();
			s.draws = 0;
			s.partitioned = partitioned;
			for (std::size_t o = 0; o < count; ++o)
				set(o);
			s.cursor.Restart(generation);
			++s.resyncs;
			++s.shadowVersion;
		} else {
			// Slots the tables grew by.
			if (const std::size_t first = s.produced.size(); first < count) {
				s.produced.resize(count, 0u);
				s.pipeline.resize(count, kNoPipeline);
				s.shadow.resize(count);
				for (std::size_t o = first; o < count; ++o)
					set(o);
			}
			// Any change kind: re-counting an unchanged share costs nothing.
			for (const auto& change : s.cursor.Unread(a_tables.changeLog)) {
				if (change.slot >= count)
					continue;
				set(change.slot);
				++s.changes;
			}
		}
		s.cursor.Advance(a_tables.changeLog);
		Diagnostics::Notify(Diagnostics::StoreUpdated<DrawBoundStore>{ s, a_tables, producer.frame });
	}

	void IndirectDraws::Impl::ReserveMainSequences(Resources& a_resources, const SceneTables& a_tables, std::uint32_t a_generation)
	{
		auto& r = a_resources;
		UpdateDrawBound(a_tables, a_generation);
		const auto& drawBound = producer.drawBound;
		std::uint32_t draws = drawBound.Draws();
		// Builds on the latest sizing (in flight or current). Sequences, bucket layout and counts move together, when a revision
		// naming them is selected.
		auto& growths = Growths::Get();
		const MainSizing& base = growths.LatestSizing<MainSizing>(r);
		MainSizing next = base;
		std::vector<Growths::Part> parts;
		// Z-prepass buckets: each pipeline slot's draws, capacity kept while they fit and grown to a power of two past them, so the
		// recorded plain draws change only then.
		{
			const auto slots = static_cast<std::uint32_t>(a_tables.pipelines.size());
			bool moved = next.zBucketCapacity.size() < slots;
			if (moved)
				next.zBucketCapacity.resize(slots, 0u);
			for (std::uint32_t p = 0; p < slots; ++p) {
				// Effect keys draw only in the forward range and have no Z-prepass pipeline.
				if (RasterEffect(a_tables.pipelines[p].rasterFlags))
					continue;
				if (const std::uint32_t pipelineDraws = drawBound.PipelineDraws(p); pipelineDraws > next.zBucketCapacity[p]) {
					next.zBucketCapacity[p] = std::bit_ceil(pipelineDraws);
					moved = true;
				}
			}
			if (moved) {
				next.zBucketFirst.resize(next.zBucketCapacity.size());
				std::uint64_t first = 0;
				for (std::size_t p = 0; p < next.zBucketCapacity.size(); ++p) {
					next.zBucketFirst[p] = static_cast<std::uint32_t>(first);
					first += next.zBucketCapacity[p];
				}
				draws = static_cast<std::uint32_t>(std::max<std::uint64_t>(draws, std::min<std::uint64_t>(first, UINT32_MAX)));
				++next.zBucketsLayout;
			} else if (!next.zBucketFirst.empty()) {
				draws = std::max(draws, next.zBucketFirst.back() + next.zBucketCapacity.back());
			}
			// A terrain group 1 bucket has an equal twin for its near blocks (PlanZBuckets). The colour segment's terrain groups take
			// ranges of phase 2's (kTerrainGroups), sized like buckets.
			std::uint64_t terrainTwins = 0, terrainSlots = 0;
			std::array<std::uint64_t, kTerrainGroups> terrainDraws{};
			for (std::uint32_t p = 0; p < slots && p < a_tables.pipelines.size(); ++p) {
				const std::uint32_t group = RasterTerrainGroup(a_tables.pipelines[p].rasterFlags);
				if (!group)
					continue;
				terrainDraws[group - 1] += drawBound.PipelineDraws(p);
				if (group == 1) {
					terrainTwins += next.zBucketCapacity[p];
					++terrainSlots;
				}
			}
			for (std::uint32_t g = 0; g < kTerrainGroups; ++g)
				if (terrainDraws[g] > next.terrainCapacity[g])
					next.terrainCapacity[g] = static_cast<std::uint32_t>(std::bit_ceil(std::min<std::uint64_t>(terrainDraws[g], 1u << 31)));
			// The forward range, sized like the terrain ranges, twice over (staging and sorted halves), after them in phase 2's.
			// Refraction slots' draws go to the refraction range after the forward halves, sized likewise.
			std::uint64_t forwardDraws = 0, refractionDraws = 0;
			for (std::uint32_t p = 0; p < slots && p < a_tables.pipelines.size(); ++p)
				if (RasterForward(a_tables.pipelines[p].rasterFlags))
					(RasterRefraction(a_tables.pipelines[p].rasterFlags) ? refractionDraws : forwardDraws) += drawBound.PipelineDraws(p);
			if (forwardDraws > next.forwardCapacity)
				next.forwardCapacity = static_cast<std::uint32_t>(std::bit_ceil(std::min<std::uint64_t>(forwardDraws, 1u << 30)));
			if (refractionDraws > next.refractionCapacity)
				next.refractionCapacity = static_cast<std::uint32_t>(std::bit_ceil(std::min<std::uint64_t>(refractionDraws, 1u << 30)));
			const std::uint64_t phaseDraws = (next.zBucketFirst.empty() ? 0ull : std::uint64_t(next.zBucketFirst.back()) + next.zBucketCapacity.back()) + terrainTwins;
			const std::uint64_t terrainRange = std::uint64_t(next.terrainCapacity[0]) + next.terrainCapacity[1] + 2ull * next.forwardCapacity + next.refractionCapacity;
			draws = static_cast<std::uint32_t>(std::min<std::uint64_t>(std::max<std::uint64_t>({ draws, phaseDraws, terrainRange }), UINT32_MAX));
			const auto buckets = static_cast<std::uint32_t>(next.zBucketCapacity.size() + terrainSlots);
			if (buckets > next.zBucketCountWords) {
				next.zBucketCountWords = Doubled(std::max(next.zBucketCountWords, 64u), buckets);
				for (auto& counts : r.zBucketCounts)
					parts.push_back({ counts, next.zBucketCountWords });
			}
			// The latch is not versioned: grown now.
			ReserveMainLatch(producer.latches.mainLayout.cascades, producer.latches.mainLayout.shadowVolumes, buckets);
		}
		std::uint32_t decals = 0;
		for (std::uint32_t group = 0; group < kDecalGroups; ++group)
			decals = std::max(decals, a_tables.decalCount[group]);
		CheckSequenceLimits("the main pass", draws, true);
		CheckSequenceLimits("a decal group", decals, false);
		// One row per material and pipeline slot, plus a quarter, so the tables grow ahead of the scene.
		const auto materialSlots = static_cast<std::uint32_t>(a_tables.materials.size()), pipelineSlots = static_cast<std::uint32_t>(a_tables.pipelines.size());
		// A deferred growth's version is filled with the rows the last commit posted (taken at the make's start, never the commit's
		// live state), laid out for its address.
		RowsCapture materials, pipelines;
		const bool deferred = Growths::Deferred();
		const auto& committed = producer.committedRows;
		if (deferred && committed.materials.elements) {
			materials.held = committed.materials.Version();
			materials.bytes = [rows = committed.materials](std::uint64_t a_address) { return CapturedMainRows(rows, a_address); };
		}
		if (deferred && committed.pipelines.elements) {
			pipelines.held = committed.pipelines.Version();
			pipelines.bytes = [rows = committed.pipelines](std::uint64_t a_address) { return CapturedMainRows(rows, a_address); };
		}
		r.materialRows.Reserve(materialSlots + materialSlots / 4, [&r](std::uint64_t a_held) { r.materialRowsHeld = a_held; }, &materials);
		// CS_DCLF_TEST_ROWS_GROWTH: forces both to grow at that frame, to test a mid-run growth's fill under claims.
		std::uint32_t forced = 0;
		if (const auto& at = SwitchValue(Switch::TestRowsGrowth); !at.empty() && producer.frame == static_cast<std::uint32_t>(std::strtoul(at.c_str(), nullptr, 10)))
			forced = 1;
		// The rows held or asked for, as the revision code knows them.
		const auto heldRows = [](const GrowableRows& a_rows) {
			const std::uint64_t held = Growths::Get().Held(a_rows.buffer);
			return held ? static_cast<std::uint32_t>(held / a_rows.stride) : a_rows.capacity;
		};
		r.materialRows.Reserve(forced ? heldRows(r.materialRows) + 1 : 0u, [&r](std::uint64_t a_held) { r.materialRowsHeld = a_held; }, &materials);
		r.pipelineRows.Reserve(std::max(pipelineSlots + pipelineSlots / 4, forced ? heldRows(r.pipelineRows) + 1 : 0u), [&r](std::uint64_t a_held) { r.pipelineRowsHeld = a_held; }, &pipelines);
		if (draws > next.sequenceDraws || decals > next.sequenceDecals) {
			const std::uint32_t newDraws = Doubled(next.sequenceDraws, draws), newDecals = Doubled(next.sequenceDecals, decals);
			const std::uint64_t slots = SequenceSlots(newDraws, newDecals);
			// New backings, rewritten whole by the epochs; ORG's deletion queue releases the old ones.
			parts.push_back({ r.sequences, static_cast<std::uint32_t>(slots * sizeof(DrawSequence) / 4) });
			if (r.sort) {
				parts.push_back({ r.sort->staging, static_cast<std::uint32_t>(std::uint64_t(newDraws) * sizeof(DrawSequence) / 4) });
				parts.push_back({ r.sort->ranks, newDraws });
			}
			logger::info("[DCLF] main sequences: {} draws and {} per decal group grown to {} and {} ({} KB){}", next.sequenceDraws, next.sequenceDecals, newDraws,
				newDecals, slots * sizeof(DrawSequence) / 1024, Growths::Deferred() ? ", as graph work" : "");
			next.sequenceDraws = newDraws;
			next.sequenceDecals = newDecals;
		}
		if (next == base)
			return;
		// A commit past the adopted ranges is checked against this (draws waiting for the change, or a defect of the bound).
		r.askedSequenceDraws.store(next.sequenceDraws, std::memory_order_release);
		growths.Change<MainSizing>(r, producer.inputs.main, std::move(next), std::move(parts), [&r] {
			if (r.sequencesD3D11)
				r.sequencesD3D11 = WrapWords(*r.sequences->Get(), SequenceSlots(r.sequenceDraws, r.sequenceDecals) * sizeof(DrawSequence));
		});
	}

	void IndirectDraws::Impl::ReserveShadowSequences(ShadowResources& a_shadow, const SceneTables& a_tables, std::uint32_t a_generation, std::uint32_t a_slots,
		std::uint32_t a_first)
	{
		(void)a_slots;
		(void)a_first;
		UpdateDrawBound(a_tables, a_generation);
		const std::uint32_t draws = producer.drawBound.Draws();
		CheckSequenceLimits("a shadow view", draws, false);
		// Every slot's sequences in one change, adopted with the revision that names it. Until then a view draws its claims, which
		// fit the sizing they were claimed under; the bound only sizes the shapes ahead.
		auto& growths = Growths::Get();
		const ShadowSizing& base = growths.LatestSizing<ShadowSizing>(a_shadow);
		if (draws <= base.sequenceDraws)
			return;
		// Doubled: buckets grow by powers of two (SizeShadowBuckets).
		ShadowSizing next = base;
		next.sequenceDraws = Doubled(base.sequenceDraws, draws);
		const std::uint64_t bytes = std::uint64_t(kShadowClasses) * next.sequenceDraws * sizeof(DrawSequence);
		std::vector<Growths::Part> parts;
		// Every slot the frame made, as posted.
		if (const auto& slotBuffers = producer.inputs.shadowSlots)
			for (const auto& sequences : slotBuffers->sequences)
				parts.push_back({ sequences, static_cast<std::uint32_t>(bytes / 4) });
		logger::info("[DCLF] shadow sequences: {} draws a slot grown to {} ({} slots, {} KB each){}", base.sequenceDraws, next.sequenceDraws, parts.size(), bytes / 1024,
			Growths::Deferred() ? ", as graph work" : "");
		growths.Change<ShadowSizing>(a_shadow, producer.inputs.shadow, std::move(next), std::move(parts));
	}

	bool IndirectDraws::Impl::EnsureSceneBuffers(rhi::Device a_device)
	{
		if (scene)
			return true;
		auto buffers = std::make_shared<SceneBuffers>();
		const bool smallStart = SwitchValue(Switch::TableStart) == "small";
		buffers->objectCapacity = smallStart ? 64u : kInitialObjects;
		buffers->geometryRows = smallStart ? 64u : kInitialGeometries;
		buffers->extraRows = smallStart ? 256u : kInitialExtraRows;
		buffers->faceVertices = smallStart ? 1024u : kInitialFaceVertices;
		// Objects and extras are structured (SRVs t127, t126); geometries are raw words; face positions are a vertex buffer.
		buffers->objects = MakeVersioned(StructuredBuffer(buffers->objectCapacity, sizeof(BindlessObject), "cs.dclf.objects", buffers->objectsIndex));
		buffers->extras = MakeVersioned(StructuredBuffer(buffers->extraRows, 16, "cs.dclf.extras", buffers->extrasIndex));
		buffers->geometries = MakeVersioned(CreateWords(std::uint64_t(buffers->geometryRows) * sizeof(GeometryDraw) / 4, false, "cs.dclf.geometries"));
		buffers->facePositions = MakeVersioned(DeviceBuffer(std::uint64_t(buffers->faceVertices) * 16, "cs.dclf.face-positions"));
		buffers->facePositionsAddress = AddressOf(a_device, *buffers->facePositions->Get());
		if (!buffers->facePositionsAddress)
			return false;
		// Without the tree wind program, trees keep the wind they joined with (the wind buffers, t124, exist and stay zero).
		buffers->treeWind = ComputeProgram::Load(a_device, { .source = kTreeWindShader, .constantWords = kTreeWindConstantWords });
		if (buffers->treeWind) {
			std::uint32_t unused = 0;
			buffers->treeCapacity = smallStart ? 4u : kInitialTrees;
			buffers->trees = MakeVersioned(StructuredBuffer(buffers->treeCapacity, sizeof(TreeStatic), "cs.dclf.trees", unused));
			buffers->treeClocks = MakeVersioned(StructuredBuffer(buffers->treeCapacity, sizeof(TreeClock), "cs.dclf.tree-clocks", unused, true));
			buffers->treeFrameBuffer = StructuredBuffer(1, sizeof(TreeWindFrameRow), "cs.dclf.tree-frame", unused);
		} else {
			logger::warn("[DCLF] The tree wind program could not be created; trees keep the wind they joined with");
		}
		for (std::uint32_t h = 0; h < 2; ++h)
			buffers->treeWindRows[h] = MakeVersioned(StructuredBuffer((buffers->treeCapacity + 1) * kTreeWindEntryRows, 16, h ? "cs.dclf.tree-wind1" : "cs.dclf.tree-wind0",
				buffers->treeWindIndex[h], true));
		// Without the fade state program, fades stay the CPU's.
		buffers->fadeState = ComputeProgram::Load(a_device, { .source = kFadeStateShader, .constantWords = kFadeStateConstantWords });
		if (buffers->fadeState) {
			std::uint32_t unused = 0;
			buffers->fadeRootCapacity = smallStart ? 4u : kInitialFadeRoots;
			buffers->fadeRoots = MakeVersioned(StructuredBuffer(buffers->fadeRootCapacity, sizeof(FadeRootStatic), "cs.dclf.fade-roots", buffers->fadeRootsIndex));
			buffers->fadeStates = MakeVersioned(StructuredBuffer(buffers->fadeRootCapacity, sizeof(FadeNodeState), "cs.dclf.fade-states", unused, true));
			for (std::uint32_t h = 0; h < 2; ++h)
				buffers->fadeStatesOut[h] = MakeVersioned(StructuredBuffer(buffers->fadeRootCapacity, sizeof(FadeNodeState), h ? "cs.dclf.fade-states-out1" : "cs.dclf.fade-states-out0",
					buffers->fadeStatesOutIndex[h], true));
			buffers->fadeFrameBuffer = StructuredBuffer(1, sizeof(FadeFrame), "cs.dclf.fade-frame", unused);
			buffers->portalWords = smallStart ? kPortalHeaderBytes / 4 : 4096u;
			buffers->portalPrograms = MakeVersioned(CreateWords(buffers->portalWords, false, "cs.dclf.portal-programs"));
			buffers->fadeRootPrograms = MakeVersioned(StructuredBuffer(buffers->fadeRootCapacity, sizeof(std::uint32_t), "cs.dclf.fade-root-programs", unused));
			buffers->fadeAnimated = MakeVersioned(StructuredBuffer(buffers->fadeRootCapacity, sizeof(std::uint32_t), "cs.dclf.fade-animated", unused));
			buffers->fadeLog = StructuredBuffer(kFadeLogEntries, sizeof(FadeLogEntry), "cs.dclf.fade-log", unused, true);
			// The fade write-back: events, the roots' reported milestones, and a staging set (a host buffer per frame slot).
			if (auto* host = RenderGraphRuntime::Get().Host()) {
				buffers->fadeEventCapacity = smallStart ? 4u : kInitialFadeEvents;
				buffers->fadeEvents = MakeVersioned(CreateWords(FadeWriteBack::BytesFor(buffers->fadeEventCapacity) / 4, true, "cs.dclf.fade-events"));
				buffers->fadeReported = MakeVersioned(StructuredBuffer(buffers->fadeRootCapacity, 2 * sizeof(std::uint32_t), "cs.dclf.fade-reported", unused, true));
				auto writeBack = std::make_shared<FadeWriteBack>();
				writeBack->frameSlots = host->FrameSlots();
				writeBack->armed.resize(writeBack->frameSlots);
				writeBack->AddSet(buffers->fadeEventCapacity);
				buffers->fadeWriteBack = std::move(writeBack);
			}
			// The visibility readback (the normal path writes the listed roots' kAccumulated from it): events, each root's and object's
			// reported verdict, and a staging set.
			if (auto* host = RenderGraphRuntime::Get().Host()) {
				buffers->visibilityEventsProgram = ComputeProgram::Load(a_device, { .source = kVisibilityEventsShader, .constantWords = kVisibilityEventsConstantWords });
				if (buffers->visibilityEventsProgram) {
					buffers->visibilityEventCapacity = smallStart ? 4u : kInitialVisibilityEvents;
					buffers->visibilityEvents = MakeVersioned(CreateWords(VisibilityEventBytes(buffers->visibilityEventCapacity) / 4, true, "cs.dclf.visibility-events"));
					buffers->visibilityRootReported = MakeVersioned(
						StructuredBuffer(buffers->fadeRootCapacity, 2 * sizeof(std::uint32_t), "cs.dclf.visibility-root-reported", unused, true));
					buffers->visibilityObjectReported = MakeVersioned(
						StructuredBuffer(buffers->objectCapacity, sizeof(std::uint32_t), "cs.dclf.visibility-object-reported", unused, true));
					auto readback = std::make_shared<VisibilityReadback>();
					readback->frameSlots = host->FrameSlots();
					readback->armed.resize(readback->frameSlots);
					readback->AddSet(buffers->visibilityEventCapacity);
					buffers->visibilityReadback = std::move(readback);
				} else {
					logger::warn("[DCLF] The visibility events program could not be created; the stood-in roots' kAccumulated is not written (T7b2)");
				}
			}
		} else {
			logger::warn("[DCLF] The fade state program could not be created; fades stay the CPU's");
		}
		// Without the tree LOD cull program, tree LOD stays the engine's.
		buffers->treeLodCull = ComputeProgram::Load(a_device, { .source = kTreeLodCullShader, .constantWords = kTreeLodCullConstantWords });
		if (buffers->treeLodCull) {
			buffers->treeLodShapeCapacity = smallStart ? 4u : kInitialTreeLodShapes;
			buffers->treeLodMeshCapacity = smallStart ? 4u : kInitialTreeLodMeshes;
			buffers->treeLodShapes = MakeVersioned(CreateWords(TreeLodShapeWords(buffers->treeLodShapeCapacity), false, "cs.dclf.tree-lod-shapes"));
			buffers->treeLodInstances = MakeVersioned(CreateWords(TreeLodInstanceWords(buffers->treeLodShapeCapacity), false, "cs.dclf.tree-lod-instances"));
			buffers->treeLodMeshes = MakeVersioned(CreateWords(TreeLodMeshWords(buffers->treeLodMeshCapacity), false, "cs.dclf.tree-lod-meshes"));
			buffers->treeLodDraw = CreateWords(sizeof(TreeLod::DrawRow) / 4, false, "cs.dclf.tree-lod-draw");
			buffers->treeLodVisible = MakeVersioned(CreateWords(TreeLodVisibleWords(buffers->treeLodShapeCapacity), true, "cs.dclf.tree-lod-visible"));
			buffers->treeLodShapesAddress = AddressOf(a_device, *buffers->treeLodShapes->Get());
			buffers->treeLodInstancesAddress = AddressOf(a_device, *buffers->treeLodInstances->Get());
			buffers->treeLodMeshesAddress = AddressOf(a_device, *buffers->treeLodMeshes->Get());
			buffers->treeLodDrawAddress = AddressOf(a_device, *buffers->treeLodDraw);
			buffers->treeLodVisibleAddress = AddressOf(a_device, *buffers->treeLodVisible->Get());
			if (!buffers->treeLodShapesAddress || !buffers->treeLodInstancesAddress || !buffers->treeLodMeshesAddress || !buffers->treeLodDrawAddress ||
				!buffers->treeLodVisibleAddress) {
				logger::warn("[DCLF] Tree LOD's tables have no device address; tree LOD stays native");
				buffers->treeLodCull = nullptr;
			}
			if (auto* host = RenderGraphRuntime::Get().Host(); host && buffers->treeLodCull) {
				auto counts = std::make_shared<SceneBuffers::TreeLodCounts>();
				for (std::uint32_t i = 0; i < host->FrameSlots(); ++i)
					counts->readback.push_back(org::Buffer::CreateShared(rhi::HeapType::Readback, sizeof(TreeLod::VisibleHeader)));
				counts->filled.assign(host->FrameSlots(), 0u);
				buffers->treeLodCounts = std::move(counts);
			}
			// New tables hold nothing: send every slot and mesh again.
			Scene::FrameView::Get().TreeLodMirror().MarkAllChanged();
		} else {
			logger::warn("[DCLF] The tree LOD cull program could not be created; tree LOD stays native");
		}
		scene = std::move(buffers);
		return true;
	}

	void IndirectDraws::Impl::ReserveShadowLatch(ShadowResources& a_shadow, std::uint32_t a_views, std::uint32_t a_keys, std::uint32_t a_rasterStates,
		std::uint32_t a_sunProcesses)
	{
		auto* host = RenderGraphRuntime::Get().Host();
		auto& latches = producer.latches;
		if (!host || !latches.shadow)
			return;
		auto& r = a_shadow;
		ShadowLatchLayout layout = latches.shadowLayout;
		const std::uint32_t slots = kFirstShadowViewSlot + a_views;
		const bool newSlots = slots > layout.viewSlots;
		if (newSlots) {
			// The slots' buffers are the render thread's to make (ShadowSlotsRequest below: a graph build declares them).
			layout.viewSlots = Doubled(layout.viewSlots, slots);
			logger::info("[DCLF] shadow view slots: {} grown to {}", latches.shadowLayout.viewSlots, layout.viewSlots);
		}
		// The view blocks (rewritten whole by every epoch, nothing to fill): one change adopted with its revision; until then views
		// past the slots stay the engine's.
		if (auto& growths = Growths::Get(); layout.viewSlots > growths.LatestSizing<ShadowSizing>(r).viewSlots) {
			ShadowSizing next = growths.LatestSizing<ShadowSizing>(r);
			next.viewSlots = layout.viewSlots;
			const std::uint64_t bytes = std::uint64_t(next.viewSlots) * kShadowViewSlotBytes;
			logger::info("[DCLF] shadow view blocks: {} slots grown to {}{}", growths.LatestSizing<ShadowSizing>(r).viewSlots, next.viewSlots,
				Growths::Deferred() ? ", as graph work" : "");
			growths.Change<ShadowSizing>(r, producer.inputs.shadow, std::move(next), { { r.viewBlocks.buffer, 0, bytes } }, [&r, host, slots = layout.viewSlots] {
				r.viewBlocks.capacity = slots;
				r.viewBlocks.address = AddressOf(host->GetDesc().device, *r.viewBlocks.buffer->Get());
				++r.viewBlocks.generation;
			});
		}
		if (a_keys > layout.keySlots) {
			layout.keySlots = Doubled(layout.keySlots, a_keys);
			logger::info("[DCLF] shadow key slots: {} grown to {}", latches.shadowLayout.keySlots, layout.keySlots);
		}
		// A count word per key slot (a view's bucket bound), every slot in one change; until adopted, shapes trim to the words there are.
		auto& growths = Growths::Get();
		if (const ShadowSizing& base = growths.LatestSizing<ShadowSizing>(r); layout.keySlots > base.bucketCountWords) {
			ShadowSizing next = base;
			next.bucketCountWords = layout.keySlots;
			std::vector<Growths::Part> parts;
			// Every slot the frame made, as posted; a slot made for the request below is made at this size.
			if (const auto& slotBuffers = producer.inputs.shadowSlots)
				for (const auto& counts : slotBuffers->bucketCounts)
					parts.push_back({ counts, next.bucketCountWords });
			logger::info("[DCLF] shadow bucket counts: {} words a slot grown to {}{}", base.bucketCountWords, next.bucketCountWords,
				Growths::Deferred() ? ", as graph work" : "");
			growths.Change<ShadowSizing>(r, producer.inputs.shadow, std::move(next), std::move(parts));
		}
		// The latched copies' zeros: a view's draw count and bucket counts.
		if (const std::uint32_t zeroBytes = std::max<std::uint32_t>(growths.LatestSizing<ShadowSizing>(r).bucketCountWords * 4, sizeof(kZeroCounts));
			!latches.shadowZeros || latches.shadowZeros->Stride() < zeroBytes)
			latches.shadowZeros = org::LatchBlock::Create("cs.dclf.shadow.zeros", zeroBytes, 1);
		if (a_rasterStates > layout.rasterStates) {
			layout.rasterStates = Doubled(layout.rasterStates, a_rasterStates);
			logger::info("[DCLF] shadow view rasterizer state rows: {} grown to {}", latches.shadowLayout.rasterStates, layout.rasterStates);
		}
		if (a_sunProcesses > layout.sunProcesses) {
			layout.sunProcesses = Doubled(layout.sunProcesses, a_sunProcesses);
			logger::info("[DCLF] sun full-frustum processes: {} grown to {}", latches.shadowLayout.sunProcesses, layout.sunProcesses);
		}
		if (layout == latches.shadowLayout)
			return;
		latches.shadowLayout = layout;
		// Every execution writes its frame slot's region whole; frames in flight keep the old block alive through their shapes.
		latches.shadow = org::LatchBlock::Create("cs.dclf.shadow.latch", layout.Bytes(), host->FrameSlots());
		// The render thread makes the slots' buffers at the newest sizing and builds the graph that declares them
		// (ServeRevisionRequests). No revision is sealed until that build.
		if (newSlots) {
			const ShadowSizing& latest = growths.LatestSizing<ShadowSizing>(r);
			shadowSlotsSlot.Post(std::make_unique<ShadowSlotsRequest>(ShadowSlotsRequest{ layout.viewSlots, latest.sequenceDraws, std::max(latest.bucketCountWords, 64u) }));
			producer.buildWanted = producer.inputs.buildGeneration + 1;
		}
	}

	void IndirectDraws::Impl::ReserveMainLatch(std::uint32_t a_cascades, std::uint32_t a_shadowVolumes, std::uint32_t a_buckets)
	{
		auto* host = RenderGraphRuntime::Get().Host();
		auto& latches = producer.latches;
		auto& layout = latches.mainLayout;
		if (!host || !latches.main || (a_cascades <= layout.cascades && a_shadowVolumes <= layout.shadowVolumes && a_buckets <= layout.buckets))
			return;
		if (a_buckets > layout.buckets) {
			const std::uint32_t grown = Doubled(std::max(layout.buckets, 64u), a_buckets);
			logger::info("[DCLF] Z-prepass buckets in the main latch: {} grown to {}", layout.buckets, grown);
			layout.buckets = grown;
		}
		if (a_cascades > layout.cascades) {
			const std::uint32_t grown = Doubled(layout.cascades, a_cascades);
			logger::info("[DCLF] sun cascades in the main latch: {} grown to {}", layout.cascades, grown);
			layout.cascades = grown;
		}
		if (a_shadowVolumes > layout.shadowVolumes) {
			const std::uint32_t grown = Doubled(layout.shadowVolumes, a_shadowVolumes);
			logger::info("[DCLF] local shadow light volumes in the main latch: {} grown to {}", layout.shadowVolumes, grown);
			layout.shadowVolumes = grown;
		}
		// Every execution writes its frame slot's region whole; frames in flight keep the old block.
		latches.main = org::LatchBlock::Create("cs.dclf.latch", layout.Bytes(), host->FrameSlots());
	}

	void IndirectDraws::Impl::ReserveSceneTables(SceneBuffers& a_scene, const SceneTables& a_tables)
	{
		auto* host = RenderGraphRuntime::Get().Host();
		if (!host)
			return;
		auto& s = a_scene;
		const auto device = host->GetDesc().device;
		// One change of the scene's sizing (adopted with its revision, at once without), built on any in flight, by doubling. A new
		// version holds nothing: on adoption the held marks reset and the next commit sends tables whole. a_make names the parts;
		// a_adopted is what adoption changes. Batches staged against old versions are dropped (generation); passes see a new layout.
		auto& growths = Growths::Get();
		const SceneSizing& base = growths.LatestSizing<SceneSizing>(s);
		SceneSizing next = base;
		std::vector<Growths::Part> parts;
		std::vector<std::function<void()>> consequences;
		auto grow = [&](const char* a_name, std::uint32_t& a_capacity, std::uint64_t a_needed, std::uint32_t a_rowBytes, auto&& a_make, std::function<void()> a_adopted) {
			if (a_needed <= a_capacity)
				return;
			const std::uint32_t rows = Doubled(a_capacity, static_cast<std::uint32_t>(std::min<std::uint64_t>(a_needed, UINT32_MAX)));
			a_make(rows, parts);
			logger::info("[DCLF] scene {}: {} grown to {} ({} KB){}", a_name, a_capacity, rows, std::uint64_t(rows) * a_rowBytes / 1024,
				Growths::Deferred() ? ", as graph work" : "");
			a_capacity = rows;
			++s.growths;
			if (a_adopted)
				consequences.push_back(std::move(a_adopted));
		};
		grow("object records", next.objectCapacity, a_tables.objects.size(), sizeof(BindlessObject),
			[&](std::uint32_t a_rows, auto& a_parts) {
				a_parts.push_back({ s.objects, a_rows });
				if (s.visibilityObjectReported)
					a_parts.push_back({ s.visibilityObjectReported, a_rows });
			},
			[&s] {
				s.objectsIndex = s.objects->Get()->GetSRVInfo(0).slot.index;
				s.held.objects = 0;
				s.visibilityReportedZeroed = false;
			});
		// The slots, then a row per face stream.
		grow("geometry rows", next.geometryRows, a_tables.geometries.size() + a_tables.faceStreams.size(), sizeof(GeometryDraw),
			[&](std::uint32_t a_rows, auto& a_parts) { a_parts.push_back({ s.geometries, static_cast<std::uint32_t>(std::uint64_t(a_rows) * sizeof(GeometryDraw) / 4) }); },
			[&s] { s.held.geometries = 0; });
		grow("extras rows", next.extraRows, a_tables.extraRows.size() / 4, 16,
			[&](std::uint32_t a_rows, auto& a_parts) { a_parts.push_back({ s.extras, a_rows }); },
			[&s] {
				s.extrasIndex = s.extras->Get()->GetSRVInfo(0).slot.index;
				s.held.extras = 0;
			});
		std::uint64_t faceVertices = 0;
		for (const auto& stream : a_tables.faceStreams)
			if (stream.object != SceneTables::kNoFaceObject)
				faceVertices = std::max<std::uint64_t>(faceVertices, std::uint64_t(stream.region) + stream.vertexCount);
		// Tree rows, clocks and wind by tree slot. New clocks restart from each tree's static row; new wind is empty until next frame.
		if (s.trees) {
			grow("tree rows", next.treeCapacity, a_tables.trees.size(), sizeof(TreeStatic) + sizeof(TreeClock) + 2 * kTreeWindEntryRows * 16,
				[&](std::uint32_t a_rows, auto& a_parts) {
					a_parts.push_back({ s.trees, a_rows });
					a_parts.push_back({ s.treeClocks, a_rows });
					for (std::uint32_t h = 0; h < 2; ++h)
						a_parts.push_back({ s.treeWindRows[h], (a_rows + 1) * kTreeWindEntryRows });
				},
				[&s] {
					for (std::uint32_t h = 0; h < 2; ++h)
						s.treeWindIndex[h] = s.treeWindRows[h]->Get()->GetSRVInfo(0).slot.index;
					s.treesHeld = ~0ull;
					s.treeWindZeroed = false;
				});
		}
		// Fade roots by root slot. New state is reseeded from the static rows (a zero generation is never a listing's).
		if (s.fadeRoots) {
			grow("fade roots", next.fadeRootCapacity, a_tables.fadeRoots.size(), sizeof(FadeRootStatic) + sizeof(FadeNodeState),
				[&](std::uint32_t a_rows, auto& a_parts) {
					for (const auto* buffer : { &s.fadeRoots, &s.fadeRootPrograms, &s.fadeAnimated, &s.fadeStates })
						a_parts.push_back({ *buffer, a_rows });
					if (s.fadeReported)
						a_parts.push_back({ s.fadeReported, a_rows });
					if (s.visibilityRootReported)
						a_parts.push_back({ s.visibilityRootReported, a_rows });
					for (std::uint32_t h = 0; h < 2; ++h)
						a_parts.push_back({ s.fadeStatesOut[h], a_rows });
				},
				[&s] {
					for (std::uint32_t h = 0; h < 2; ++h)
						s.fadeStatesOutIndex[h] = s.fadeStatesOut[h]->Get()->GetSRVInfo(0).slot.index;
					s.fadeRootsIndex = s.fadeRoots->Get()->GetSRVInfo(0).slot.index;
					s.fadeRootsHeld = ~0ull;
					s.fadeRootProgramsHeld = ~0ull;
					s.fadeStatesOutZeroed = false;
					s.fadeReportedZeroed = false;
					s.visibilityReportedZeroed = false;
				});
		}
		// Portal programs, to the most a frame's encoding needs.
		if (s.portalPrograms) {
			grow("portal programs", next.portalWords, producer.inputs.portalWords, sizeof(std::uint32_t),
				[&](std::uint32_t a_words, auto& a_parts) { a_parts.push_back({ s.portalPrograms, a_words }); }, [&s] { s.portalProgramsSent.clear(); });
		}
		// The fade event list, to the most a frame wanted (dropped roots append again), with its staging set made before any revision
		// names the capacity.
		if (s.fadeEvents && s.fadeWriteBack) {
			grow("fade events", next.fadeEventCapacity, s.fadeWriteBack->wanted.load(std::memory_order_acquire), sizeof(FadeEvent),
				[&](std::uint32_t a_rows, auto& a_parts) {
					a_parts.push_back({ s.fadeEvents, static_cast<std::uint32_t>(FadeWriteBack::BytesFor(a_rows) / 4) });
					s.fadeWriteBack->AddSet(a_rows);
				},
				nullptr);
		}
		// The visibility event list, likewise.
		if (s.visibilityEvents && s.visibilityReadback) {
			grow("visibility events", next.visibilityEventCapacity, s.visibilityReadback->wanted.load(std::memory_order_acquire), sizeof(VisibilityEvent),
				[&](std::uint32_t a_rows, auto& a_parts) {
					a_parts.push_back({ s.visibilityEvents, static_cast<std::uint32_t>(VisibilityEventBytes(a_rows) / 4) });
					s.visibilityReadback->AddSet(a_rows);
				},
				nullptr);
		}
		// Tree LOD's tables by the mirror's posted shape and mesh slots; adoption marks every slot changed.
		if (s.treeLodCull) {
			auto& mirror = Scene::FrameView::Get().TreeLodMirror();
			grow("tree LOD shape slots", next.treeLodShapeCapacity, producer.inputs.treeLodShapeSlots, sizeof(TreeLod::ShapeRow) + TreeLod::kMaxGroupInstances * (sizeof(TreeLod::Instance) + 4),
				[&](std::uint32_t a_rows, auto& a_parts) {
					a_parts.push_back({ s.treeLodShapes, TreeLodShapeWords(a_rows) });
					a_parts.push_back({ s.treeLodInstances, TreeLodInstanceWords(a_rows) });
					a_parts.push_back({ s.treeLodVisible, TreeLodVisibleWords(a_rows) });
				},
				[&s, &mirror, device] {
					s.treeLodShapesAddress = AddressOf(device, *s.treeLodShapes->Get());
					s.treeLodInstancesAddress = AddressOf(device, *s.treeLodInstances->Get());
					s.treeLodVisibleAddress = AddressOf(device, *s.treeLodVisible->Get());
					mirror.MarkAllChanged();
				});
			grow("tree LOD mesh slots", next.treeLodMeshCapacity, producer.inputs.treeLodMeshSlots, sizeof(TreeLod::MeshRow),
				[&](std::uint32_t a_rows, auto& a_parts) { a_parts.push_back({ s.treeLodMeshes, TreeLodMeshWords(a_rows) }); },
				[&s, &mirror, device] {
					s.treeLodMeshesAddress = AddressOf(device, *s.treeLodMeshes->Get());
					mirror.MarkAllChanged();
				});
		}
		grow("face position vertices", next.faceVertices, faceVertices, 16,
			[&](std::uint32_t a_rows, auto& a_parts) { a_parts.push_back({ s.facePositions, 0, std::uint64_t(a_rows) * 16 }); },
			[&s, device] {
				s.facePositionsAddress = AddressOf(device, *s.facePositions->Get());
				s.faceUploaded.clear();  // every region again
			});
		if (!(next == base))
			growths.Change<SceneSizing>(s, producer.inputs.scene, next, std::move(parts), [&s, consequences = std::move(consequences)] {
				for (const auto& consequence : consequences)
					consequence();
				++s.generation;
				s.layout.fetch_add(1, std::memory_order_release);
			});
		ReserveObjectBuffers();
	}

	void IndirectDraws::Impl::ReserveObjectBuffers()
	{
		// Revision code: the per-object buffers follow the scene's newest object capacity.
		const auto& inputs = producer.inputs;
		if (!inputs.scene)
			return;
		auto& growths = Growths::Get();
		const std::uint32_t objects = growths.LatestSizing<SceneSizing>(*inputs.scene).objectCapacity;
		const auto inputWords = [&] { return static_cast<std::uint32_t>(std::uint64_t(objects) * sizeof(DrawInput) / 4); };
		if (inputs.main) {
			auto& r = *inputs.main;
			const MainSizing& base = growths.LatestSizing<MainSizing>(r);
			if (base.objectCapacity < objects) {
				MainSizing next = base;
				next.objectCapacity = objects;
				std::vector<Growths::Part> parts{ { r.visibility, objects }, { r.inputs, inputWords() } };
				if (r.frustum)
					parts.push_back({ r.frustum, objects });
				if (r.inputsDepth)
					parts.push_back({ r.inputsDepth, inputWords() });
				growths.Change<MainSizing>(r, inputs.main, std::move(next), std::move(parts), [&r] {
					r.residentUploaded = {};  // the new input buffers hold no region
					if (r.visibilityD3D11)
						r.visibilityD3D11 = WrapWords(*r.visibility->Get(), std::uint64_t(r.objectCapacity) * sizeof(std::uint32_t));
					if (r.frustumD3D11)
						r.frustumD3D11 = WrapWords(*r.frustum->Get(), std::uint64_t(r.objectCapacity) * sizeof(std::uint32_t));
				});
			}
		}
		if (inputs.shadow) {
			auto& r = *inputs.shadow;
			const ShadowSizing& base = growths.LatestSizing<ShadowSizing>(r);
			if (base.objectCapacity < objects) {
				ShadowSizing next = base;
				next.objectCapacity = objects;
				std::vector<Growths::Part> parts{ { r.visibility, objects } };
				parts.push_back({ r.inputs, inputWords() });
				// The new input buffers hold none of the kept state's inputs.
				growths.Change<ShadowSizing>(r, inputs.shadow, std::move(next), std::move(parts), [&r] { r.inputsUploaded = {}; });
			}
		}
	}

	bool IndirectDraws::Impl::SetupShadow()
	{
		if (shadow)
			return true;
		if (shadowSetupFailed)
			return false;
		auto* host = RenderGraphRuntime::Get().Host();
		if (!host)
			return ShadowNotReady(0, "no render graph");
		auto device = host->GetDesc().device;
		auto state = std::make_shared<ShadowResources>();
		state->constants = DeviceBuffer(kShadowConstantBytes, "cs.dclf.shadow.constants");
		// The shadow commit latches into the constants alone, known before any graph declares the pass.
		state->latchedTargets.store(std::make_shared<const std::vector<const void*>>(1, state->constants.get()), std::memory_order_release);
		// One material row table for every view, grown as the kept state needs.
		const std::uint32_t initialRows = SwitchValue(Switch::TableStart) == "small" ? 4u : kShadowMaterialRowsInitial;
		if (!state->materialRows.Create(sizeof(ShadowMaterialRow), initialRows, "cs.dclf.shadow.material-rows")) {
			shadowSetupFailed = true;
			return ShadowNotReady(2, "no device address for the shadow material rows");
		}
		if (!EnsureSceneBuffers(device)) {
			shadowSetupFailed = true;
			return ShadowNotReady(2, "no device address for the face positions");
		}
		state->scene = scene;
		// The first view slots, as many as the initial latch layout (the latch is the producer's).
		const bool smallSlots = SwitchValue(Switch::TableStart) == "small";
		const auto firstLayout = InitialShadowLatchLayout();
		state->sequenceDraws = 64u;
		state->bucketCountWords = 64u;
		AddShadowViewSlots(*state, firstLayout.viewSlots, state->sequenceDraws, state->bucketCountWords);
		state->viewSlots = firstLayout.viewSlots;
		if (!state->viewBlocks.Create(static_cast<std::uint32_t>(kShadowViewSlotBytes), firstLayout.viewSlots, "cs.dclf.shadow.view-blocks")) {
			shadowSetupFailed = true;
			return ShadowNotReady(2, "no device address for the shadow view blocks");
		}
		state->objectCapacity = scene->objectCapacity;
		state->visibility = MakeVersioned(CreateWords(state->objectCapacity, true, "cs.dclf.shadow.visibility"));
		state->inputs = MakeVersioned(CreateWords(std::uint64_t(state->objectCapacity) * sizeof(DrawInput) / 4, false, "cs.dclf.shadow.draw-inputs"));
		state->buildDraws = ComputeProgram::Load(device, { .source = kBuildDrawsShader, .constantWords = kBuildDrawsConstantWords });
		if (!state->buildDraws) {
			shadowSetupFailed = true;
			return ShadowNotReady(1, "the BuildDraws compute program could not be created");
		}
		state->dispatchSignature = CreateDispatchSignature(device, state->buildDraws->layout->GetHandle());
		if (!state->dispatchSignature) {
			shadowSetupFailed = true;
			return ShadowNotReady(1, "the BuildDraws dispatch signature could not be created");
		}
		state->pool = EnsureIndexPool(*state->scene, device, smallSlots);
		if (!state->pool) {
			shadowSetupFailed = true;
			return ShadowNotReady(1, "the index pool's copy program could not be created");
		}
		state->constantsAddress = AddressOf(device, *state->constants);
		if (!state->constantsAddress) {
			shadowSetupFailed = true;
			return ShadowNotReady(2, "no device address for the shadow buffers");
		}
		shadow = state;
		host->AddExtension(kShadowExtensionId, [state] { return MakeShadowExtension(state); });
		logger::info("[DCLF] shadow view graph resources created");
		PostRevisionInputs();
		return true;
	}

	void IndirectDraws::Impl::ServeRevisionRequests()
	{
		// Makes the shadow view slots the revision code asked for (render thread) and rebuilds the graph; the revision code waits.
		auto* host = RenderGraphRuntime::Get().Host();
		if (const auto request = shadowSlotsSlot.Take(); request && host && shadow && request->slots > shadow->sequences.size()) {
			AddShadowViewSlots(*shadow, request->slots, request->sequenceDraws, request->bucketCountWords);
			host->AddExtension(kShadowExtensionId, [state = shadow] { return MakeShadowExtension(state); });
			PostRevisionInputs();
		}
	}

	bool IndirectDraws::Impl::SetupView(ViewKind a_kind)
	{
		// A view's buffers, once the main resources and index pool exist; its latch, zeros and growth are the revision code's.
		auto& v = View(a_kind);
		if (v.resources)
			return true;
		if (v.setupFailed || !resources || !scene || !scene->pool)
			return false;
		auto* host = RenderGraphRuntime::Get().Host();
		if (!host)
			return false;
		const auto& traits = TraitsOf(a_kind);
		auto device = host->GetDesc().device;
		const auto name = [&](std::string_view a_name) { return fmt::format("{}.{}", traits.prefix, a_name); };
		auto fail = [&](const char* a_what) {
			v.setupFailed = true;
			logger::warn("[DCLF] the {} view's {}; it stays native", traits.name, a_what);
			return false;
		};
		auto state = std::make_shared<ViewResources>();
		state->traits = &traits;
		state->main = resources;
		state->buildDraws = ComputeProgram::Load(device, { .source = kBuildDrawsShader, .constantWords = kBuildDrawsConstantWords });
		if (state->buildDraws)
			state->dispatchSignature = CreateDispatchSignature(device, state->buildDraws->layout->GetHandle());
		if (!state->dispatchSignature)
			return fail("BuildDraws program could not be created");
		state->count = CreateWords(kCountWords, true, name("draw-count").c_str());
		// Sized for the adopted main sizing (ReserveView grows for one in flight). A depth-pass view has no buckets without lookups.
		ViewPlan plan;
		if (!traits.depthPass)
			PlanViewBuckets(traits, *resources, nullptr, {}, v.slotPipelines, {}, plan);
		state->bucketCountWords = Doubled(16u, static_cast<std::uint32_t>(plan.buckets.size() + (traits.forwardPart ? 1 : 0)));
		for (std::uint32_t f = 0; f < traits.faces; ++f)
			state->bucketCounts[f] = MakeVersioned(CreateWords(state->bucketCountWords, true, name(traits.faces > 1 ? fmt::format("bucket-counts{}", f) : "bucket-counts").c_str()));
		state->sequenceDraws = Doubled(64u, plan.draws);
		state->sequences = MakeVersioned(CreateWords(std::uint64_t(traits.faces) * state->sequenceDraws * sizeof(DrawSequence) / 4, true, name("sequences").c_str()));
		// The faces' tree LOD rows and lists for the adopted shape slots; ReserveView grows them.
		for (std::uint32_t f = 0; f < traits.faces && traits.trees; ++f) {
			state->treeRows[f] = CreateWords(sizeof(TreeLod::DrawRow) / 4, false, name(fmt::format("tree-row{}", f)).c_str());
			state->treeRowsAddress[f] = AddressOf(device, *state->treeRows[f]);
			if (!state->treeRowsAddress[f])
				return fail("tree LOD rows have no device address");
			if (scene->treeLodCull) {
				state->treeShapeCapacity = scene->treeLodShapeCapacity;
				const std::uint32_t words = TreeLod::kVisibleHeaderWords + state->treeShapeCapacity * TreeLod::kMaxGroupInstances;
				state->treeVisible[f] = MakeVersioned(CreateWords(words, true, name(fmt::format("tree-visible{}", f)).c_str()));
				state->treeVisibleAddress[f] = AddressOf(device, *state->treeVisible[f]->Get());
			}
		}
		state->blocks = DeviceBuffer(std::uint64_t(traits.faces) * kViewBlockBytes, name(traits.blocks).c_str());
		state->blocksAddress = AddressOf(device, *state->blocks);
		if (!state->blocksAddress)
			return fail("blocks have no device address");
		// The forward part draws through the main colour signature with an explicit preprocess, as ForwardDrawPass.
		if (traits.forwardPart)
			state->preprocessForward = PreprocessStates::Create(device, host->FrameSlots(), "the first-person forward part");
		v.resources = std::move(state);
		logger::info("[DCLF] {} graph resources created", traits.name);
		PostRevisionInputs();
		return true;
	}

	void IndirectDraws::Impl::ReserveView(ViewResources& a_view, SceneBuffers* a_scene, std::uint32_t a_slots, std::uint32_t a_buckets, std::uint32_t a_draws)
	{
		auto* host = RenderGraphRuntime::Get().Host();
		const auto& traits = *a_view.traits;
		auto& latches = producer.latches.views[ViewIndex(traits.kind)];
		if (!host || !latches.latch)
			return;
		auto& r = a_view;
		auto device = host->GetDesc().device;
		ViewLatchLayout layout = latches.layout;
		if (a_slots > layout.slots)
			layout.slots = Doubled(layout.slots, a_slots);
		if (a_buckets > layout.buckets)
			layout.buckets = Doubled(layout.buckets, a_buckets);
		if (!(layout == latches.layout)) {
			// A new block; frames in flight keep their own.
			latches.layout = layout;
			latches.latch = org::LatchBlock::Create(fmt::format("{}.latch", traits.prefix), layout.Bytes(), host->FrameSlots());
			logger::info("[DCLF] {} latch: {} pipeline slots, {} buckets", traits.name, layout.slots, layout.buckets);
		}
		// One change of the view's sizing, adopted with its revision, together with or after the main and scene sizings it follows.
		auto& growths = Growths::Get();
		const ViewSizing& base = growths.LatestSizing<ViewSizing>(r);
		ViewSizing next = base;
		std::vector<Growths::Part> parts;
		bool newTrees = false;
		if (layout.buckets > next.bucketCountWords) {
			next.bucketCountWords = layout.buckets;
			for (std::uint32_t f = 0; f < traits.faces; ++f)
				parts.push_back({ r.bucketCounts[f], next.bucketCountWords });
		}
		// The latched copies' zeros: the draw count and a face's bucket counts.
		if (const std::uint32_t zeroBytes = std::max<std::uint32_t>(next.bucketCountWords * 4, sizeof(kZeroCounts)); !latches.zeros || latches.zeros->Stride() < zeroBytes)
			latches.zeros = org::LatchBlock::Create(fmt::format("{}.zeros", traits.prefix), zeroBytes, 1);
		if (a_draws > next.sequenceDraws) {
			next.sequenceDraws = Doubled(next.sequenceDraws, a_draws);
			parts.push_back({ r.sequences, static_cast<std::uint32_t>(std::uint64_t(traits.faces) * next.sequenceDraws * sizeof(DrawSequence) / 4) });
			logger::info("[DCLF] {} sequences: {} draws a face{}", traits.name, next.sequenceDraws, Growths::Deferred() ? ", as graph work" : "");
		}
		// The faces' tree LOD lists hold every record of the newest shape slots (phase 1 only: no retest list).
		if (a_scene && a_scene->treeLodCull && r.treeVisible[0]) {
			if (const std::uint32_t shapes = growths.LatestSizing<SceneSizing>(*a_scene).treeLodShapeCapacity; next.treeShapeCapacity != shapes) {
				const std::uint32_t words = TreeLod::kVisibleHeaderWords + shapes * TreeLod::kMaxGroupInstances;
				for (std::uint32_t f = 0; f < traits.faces; ++f)
					if (r.treeVisible[f])
						parts.push_back({ r.treeVisible[f], words });
				next.treeShapeCapacity = shapes;
				newTrees = true;
			}
		}
		if (!(next == base))
			growths.Change<ViewSizing>(r, producer.inputs.views[ViewIndex(traits.kind)].resources, std::move(next), std::move(parts), [&r, device, newTrees] {
				for (std::uint32_t f = 0; f < r.traits->faces; ++f)
					if (r.treeVisible[f])
						r.treeVisibleAddress[f] = AddressOf(device, *r.treeVisible[f]->Get());
				// The tree passes declare these lists: rebuild.
				r.rebuildPending = r.rebuildPending || newTrees;
			});
	}

	bool IndirectDraws::Impl::ImportReflectionCube(ID3D11Texture2D* a_texture)
	{
		auto* host = RenderGraphRuntime::Get().Host();
		if (!reflection.resources || !host || !a_texture)
			return false;
		auto& r = *reflection.resources;
		if (r.target && r.targetTexture == a_texture)
			return true;
		DxvkOrgInteropResourceInfo info{};
		if (!RenderGraphRuntime::Get().DescribeResource(a_texture, info) || info.kind != DXVK_ORG_INTEROP_RESOURCE_IMAGE)
			return false;
		org::TextureDescription desc{};
		desc.format = rhi::helpers::ToRHI(reflection.targets.colour);
		desc.channels = 4;
		desc.hasRTV = true;
		desc.rtvFormat = desc.format;
		auto cube = ImportImage(host->GetDesc().device, info.image, desc, "DCLF reflection cube");
		if (!cube) {
			static std::uint32_t logged = 0;
			if (logged++ < 4)
				logger::warn("[DCLF] the reflection cube could not be imported (not in the general layout?); the faces stay native");
			return false;
		}
		// DCLF's own depth for the faces, at the engine's precision (its depth target 6 is R24G8).
		org::TextureDescription depthDesc{};
		depthDesc.imageDimensions.push_back({ info.image.extent.width, info.image.extent.height, 0, 0 });
		depthDesc.format = rhi::helpers::ToRHI(reflection.targets.depth);
		depthDesc.channels = 1;
		depthDesc.hasDSV = true;
		depthDesc.dsvFormat = depthDesc.format;
		r.depth = org::PixelBuffer::CreateSharedUnmaterialized(depthDesc);
		r.depth->SetName("cs.dclf.reflection.depth");
		r.target = std::move(cube);
		r.targetTexture = a_texture;
		r.width = info.image.extent.width;
		r.height = info.image.extent.height;
		host->AddExtension(kReflectionExtensionId, [state = reflection.resources] { return MakeViewExtension(state); });
		logger::info("[DCLF] reflection cube imported: {}x{}, {} faces, format {}", r.width, r.height, info.image.arrayLayers, static_cast<int>(reflection.targets.colour));
		PostRevisionInputs();
		return true;
	}

	bool IndirectDraws::Impl::ImportFirstPersonDepth()
	{
		// kPOST_ZPREPASS_COPY, the first-person colour pass's depth (FUN_1414cb6d0: FUN_140e4fe00(manager, 7, ...)), imported in its
		// DSV's format; the extension is added with it and again when the main resources change.
		auto* host = RenderGraphRuntime::Get().Host();
		auto* renderer = globals::game::renderer;
		if (!firstPerson.resources || !host || !renderer)
			return false;
		auto& r = *firstPerson.resources;
		const auto& copy = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];
		if (!copy.texture || !copy.views[0])
			return false;
		if (r.target && r.targetTexture == copy.texture && !r.rebuildPending)
			return true;
		if (!r.target || r.targetTexture != copy.texture) {
			DxvkOrgInteropResourceInfo info{};
			if (!RenderGraphRuntime::Get().DescribeResource(copy.texture, info) || info.kind != DXVK_ORG_INTEROP_RESOURCE_IMAGE)
				return false;
			D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
			copy.views[0]->GetDesc(&dsvDesc);
			org::TextureDescription desc{};
			desc.imageDimensions.push_back({ info.image.extent.width, info.image.extent.height, 0, 0 });
			desc.format = rhi::helpers::ToRHI(dsvDesc.Format);
			desc.channels = 1;
			desc.hasDSV = true;
			desc.dsvFormat = desc.format;
			auto imported = ImportImage(host->GetDesc().device, info.image, desc, "DCLF first-person depth (kPOST_ZPREPASS_COPY)");
			if (!imported) {
				static std::uint32_t logged = 0;
				if (logged++ < 4)
					logger::warn("[DCLF] kPOST_ZPREPASS_COPY could not be imported (not in the general layout?); first person stays native");
				return false;
			}
			r.target = std::move(imported);
			r.targetTexture = copy.texture;
			logger::info("[DCLF] first-person depth imported: {}x{}, format {}", info.image.extent.width, info.image.extent.height, static_cast<int>(dsvDesc.Format));
		}
		r.rebuildPending = false;
		host->AddExtension(kFirstPersonExtensionId, [state = firstPerson.resources] { return MakeViewExtension(state); });
		PostRevisionInputs();
		return true;
	}

	bool IndirectDraws::Impl::ImportShadowDepth(std::uint32_t a_index, std::uint32_t a_target)
	{
		auto* renderer = globals::game::renderer;
		if (!renderer)
			return false;
		const auto& data = renderer->GetDepthStencilData().depthStencils[a_target];
		return ImportDepthTexture(a_index, data.texture, data.views[0], a_target);
	}

	bool IndirectDraws::Impl::ImportDepthTexture(std::uint32_t a_index, ID3D11Texture2D* a_texture, ID3D11DepthStencilView* a_view, std::uint32_t a_label)
	{
		auto* host = RenderGraphRuntime::Get().Host();
		if (!host || !shadow || a_index >= kShadowDepthTargets)
			return false;
		if (!a_texture || !a_view)
			return ShadowNotReady(3, "the shadow map has no texture");
		if (shadow->depth[a_index] && shadow->depthTexture[a_index] == a_texture)
			return true;
		DxvkOrgInteropResourceInfo info{};
		if (!RenderGraphRuntime::Get().DescribeResource(a_texture, info) || info.kind != DXVK_ORG_INTEROP_RESOURCE_IMAGE)
			return ShadowNotReady(4, "the shadow map cannot be described");
		D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
		a_view->GetDesc(&dsvDesc);
		org::TextureDescription desc{};
		desc.format = rhi::helpers::ToRHI(dsvDesc.Format);
		desc.channels = 1;
		desc.hasDSV = true;
		desc.dsvFormat = desc.format;
		static constexpr const char* kNames[kShadowDepthTargets] = { "DCLF shadow maps (ESRAM)", "DCLF shadow maps", "DCLF volumetric shadow maps (ESRAM)",
			"DCLF Skylighting occlusion map", "DCLF precipitation occlusion mask" };
		auto imported = ImportImage(host->GetDesc().device, info.image, desc, kNames[a_index]);
		if (!imported)
			return ShadowNotReady(5, "the shadow map could not be imported (not in the general layout?)");
		shadow->depth[a_index] = std::move(imported);
		shadow->depthTexture[a_index] = a_texture;
		shadow->depthLayers[a_index] = info.image.arrayLayers;
		// A new resource for the passes to bind: the graph is rebuilt on the next epoch.
		host->AddExtension(kShadowExtensionId, [state = shadow] { return MakeShadowExtension(state); });
		logger::info("[DCLF] shadow map {} imported: {}x{}, {} slices, format {}", a_label, info.image.extent.width, info.image.extent.height,
			info.image.arrayLayers, static_cast<int>(dsvDesc.Format));
		return true;
	}
}

#endif
