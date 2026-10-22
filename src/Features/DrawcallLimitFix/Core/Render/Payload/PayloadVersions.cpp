// The payload's pooled versions (PayloadBuilder, D5): one JournaledBufferSet with a pool per part, published by the snapshot builder.
#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Core/Gpu/PooledBackings.h"
#include "Features/DrawcallLimitFix/Diagnostics/Draws/Events.h"

namespace DCLF::Draws
{
	namespace
	{
		constexpr std::size_t Part(PayloadPart a_part) { return static_cast<std::size_t>(a_part); }

		/** @brief The parts, in PayloadPart order. Inputs and geometries are 4-byte words (the shaders read them as such). */
		std::vector<org::services::JournaledPartConfig> PartConfigs()
		{
			const auto part = [](const char* a_name, std::uint32_t a_stride, bool a_address) {
				org::services::JournaledPartConfig config;
				config.name = fmt::format("cs.dclf.payload.{}", a_name);
				config.stride = a_stride;
				config.flags.deviceAddress = a_address;
				config.policy = org::services::GrowthPolicy::Doubling(64);
				return config;
			};
			std::vector<org::services::JournaledPartConfig> parts(kPayloadParts);
			parts[Part(PayloadPart::Objects)] = part("objects", sizeof(BindlessObject), false);
			parts[Part(PayloadPart::Extras)] = part("extras", 16, false);
			parts[Part(PayloadPart::Geometries)] = part("geometries", 4, false);
			parts[Part(PayloadPart::MaterialRows)] = part("material-rows", kMaterialRowBytes, true);
			parts[Part(PayloadPart::PipelineRows)] = part("pipeline-rows", kPipelineRowBytes, true);
			parts[Part(PayloadPart::Inputs)] = part("inputs", 4, false);
			parts[Part(PayloadPart::ShadowRows)] = part("shadow-rows", sizeof(ShadowMaterialRow), true);
			return parts;
		}

		/** @brief Each part's element (PayloadBuffer::units) in bytes, in PayloadPart order: the bytes a payload names of its backing. */
		constexpr std::array<std::uint64_t, kPayloadParts> kElementBytes{ sizeof(BindlessObject), 16, sizeof(GeometryDraw), kMaterialRowBytes, kPipelineRowBytes,
			sizeof(DrawInput), sizeof(ShadowMaterialRow) };

		template <class T>
		constexpr std::uint64_t Words(std::uint64_t a_elements) { return a_elements * sizeof(T) / 4; }

		/**
		 * @brief A part's runs, gathered as its emit makes them and sent in one batched upload (JournaledSink's ranges: one ticket, the
		 * staging page mapped once) instead of one upload each. The runs point into the views, which nothing writes while the
		 * publication is made; a_copy for the emits that patch each run into a scratch they reuse (the rows' addresses): those are copied
		 * into the builder's scratch, offsets until Send resolves them.
		 */
		struct RangeGather
		{
			std::vector<org::StreamingUploadRange>& ranges;
			std::vector<std::byte>& bytes;
			const bool copy;

			RangeGather(std::vector<org::StreamingUploadRange>& a_ranges, std::vector<std::byte>& a_bytes, bool a_copy) :
				ranges(a_ranges), bytes(a_bytes), copy(a_copy)
			{
				ranges.clear();
				bytes.clear();
			}
			void operator()(const void* a_data, std::size_t a_bytes, std::size_t a_offset)
			{
				if (!a_bytes)
					return;
				if (!copy) {
					ranges.push_back({ a_data, a_bytes, a_offset });
					return;
				}
				const std::size_t at = bytes.size();
				const auto* data = static_cast<const std::byte*>(a_data);
				bytes.insert(bytes.end(), data, data + a_bytes);
				ranges.push_back({ reinterpret_cast<const void*>(at), a_bytes, a_offset });
			}
			void Send(org::services::JournaledSink& a_sink)
			{
				if (copy)
					for (auto& range : ranges)
						range.data = bytes.data() + reinterpret_cast<std::uintptr_t>(range.data);
				a_sink.Write(std::span<const org::StreamingUploadRange>(ranges));
			}
		};

		/** @brief A kept whole tail against a publication's, bytewise (padding that differs only fails a share). */
		template <class T>
		bool SameRows(const std::vector<T>& a_kept, const T* a_data, std::size_t a_count)
		{
			return a_kept.size() == a_count && (!a_count || std::memcmp(a_kept.data(), a_data, a_count * sizeof(T)) == 0);
		}
		bool SameList(const std::vector<DrawInput>& a_kept, const std::vector<DrawInput>* a_list)
		{
			return a_list ? SameRows(a_kept, a_list->data(), a_list->size()) : a_kept.empty();
		}

		/** @brief CS_DCLF_PERSISTENT_PARITY: the extras backing a_lease, caught up from a_caughtUpFrom to a_rows, against its mirror. */
		void CheckExtras(const ExtrasOut& a_rows, const org::services::BackingLease& a_lease, std::uint64_t a_caughtUpFrom, const SceneTables* a_tables)
		{
			std::vector<float>* mirror = nullptr;
			Diagnostics::Notify(Diagnostics::ExtrasSending{ a_lease, mirror });
			if (mirror)
				EmitExtras(a_rows, a_caughtUpFrom, mirror, [](const void*, std::size_t, std::size_t) {});
			Diagnostics::Notify(Diagnostics::ExtrasSent{ a_lease, a_rows, a_tables });
		}
	}

	/**
	 * @brief The runs a holder of a part at a content epoch lacks, by bytes as its emit sends them (the journals' runs, then the
	 * whole tails): a_run(offset, bytes). Nothing merged or copied: for judging an append against its head (AppendClasses), not for
	 * sending.
	 */
	struct PayloadBuilder::PartRuns
	{
		const StreamViews& views;
		const GeometryDrawsOut& geometries;
		const MainPayload* main;
		const ShadowPayload* shadow;
		const KeptView<DrawInput>& region;
		const std::vector<DrawInput>* mainList;
		const std::vector<DrawInput>* shadowList;
		std::uint64_t mainInputsAt = 0, shadowInputsAt = 0;

		template <class Run>
		void operator()(PayloadPart a_part, std::uint64_t a_contentEpoch, Run&& a_run) const
		{
			const auto rows = [&](const auto& a_changes, std::uint64_t a_count, std::uint64_t a_bytes) {
				a_changes.ForEachRun(a_contentEpoch, a_count, [&](std::uint64_t a_first, std::uint64_t a_n) { a_run(a_first * a_bytes, a_n * a_bytes); });
			};
			const auto tail = [&](const auto* a_list, std::uint64_t a_at) {
				using Row = typename std::remove_cvref_t<decltype(*a_list)>::value_type;
				if (a_list && !a_list->empty())
					a_run(a_at * sizeof(Row), a_list->size() * sizeof(Row));
			};
			switch (a_part) {
			case PayloadPart::Objects:
				rows(views.objects.changes, views.objects.Count(), sizeof(BindlessObject));
				break;
			case PayloadPart::Extras:
				if (views.extras.extras)
					rows(views.extras.changes, views.extras.Rows(), 16);
				break;
			case PayloadPart::Geometries:
				rows(geometries.slots.changes, geometries.SlotCount(), sizeof(GeometryDraw));
				tail(&geometries.streamRows, geometries.SlotCount());
				break;
			case PayloadPart::MaterialRows:
				if (main)
					rows(main->body->materialRows.changes, main->body->materialRows.Count() * kMaterialRowParts, sizeof(MaterialRow) / kMaterialRowParts);
				break;
			case PayloadPart::PipelineRows:
				if (main)
					rows(main->body->pipelineRows.changes, main->body->pipelineRows.Count(), sizeof(PipelineRow));
				break;
			case PayloadPart::Inputs:
				rows(region.changes, region.Count(), sizeof(DrawInput));
				tail(mainList, mainInputsAt);
				tail(shadowList, shadowInputsAt);
				break;
			case PayloadPart::ShadowRows:
				if (shadow)
					rows(shadow->materialRows.changes, shadow->materialRows.Count(), sizeof(ShadowMaterialRow));
				break;
			default:
				break;
			}
		}
	};

	PayloadBuilder::PayloadBuilder(std::shared_ptr<org::services::RetirementClock> a_clock) :
		// DCLF's allocate (structured buffers with their SRV, device addresses for the row tables) and retire (released backings are
		// destroyed on the graph host's cleanup lane, never on the thread that let them go). No caps: a backing is made whenever none is
		// reusable, and the retirement clock (exact GPU completion) frees them.
		set(std::make_unique<org::services::JournaledBufferSet>(PartConfigs(), PooledBackings::Hooks(), std::move(a_clock),
			org::services::BackingPool::Config{ .name = "cs.dclf.payload", .maxPerClass = 0, .spareFree = PooledBackings::PerFrameSpares() })),
		appendInPlace(SwitchEnabled(Switch::PayloadAppend))
	{}

	PayloadBuilder::~PayloadBuilder() = default;

	std::shared_ptr<const PayloadVersions> PayloadBuilder::Publish(const StreamViews& a_views, const MainPayload* a_main, const ShadowPayload* a_shadow,
		org::runtime::IUploadService& a_uploads)
	{
		ZoneScopedN("CS.DCLF.Payload.Publish");
		if (!a_main && !a_shadow)
			return nullptr;
		using Target = org::services::JournaledTarget;
		using Sink = org::services::JournaledSink;
		std::array<org::services::JournaledPartRequest, kPayloadParts> requests{};
		auto out = std::make_shared<PayloadVersions>();
		out->main = a_main != nullptr;
		if (a_main) {
			out->products = a_main->body->products;
			Extensions::CarryPayloadProducts(out->products);
		}
		out->shadow = a_shadow != nullptr;
		std::array<std::uint64_t, kPayloadParts> units{};  // each part's elements (PayloadBuffer::units)

		// The objects' records and the extras rows: the publication's stream views.
		units[Part(PayloadPart::Objects)] = a_views.objects.Count();
		requests[Part(PayloadPart::Objects)] = { a_views.objects.Count(), a_views.objects.ContentEpoch(), [&](const Target& a_target, Sink& a_to) {
													 RangeGather gather{ uploadRanges, uploadBytes, false };
													 a_views.objects.Emit(a_target.contentEpoch, gather);
													 gather.Send(a_to);
													 return a_views.objects.ContentEpoch();
												 } };
		units[Part(PayloadPart::Extras)] = a_views.extras.Rows();
		requests[Part(PayloadPart::Extras)] = { a_views.extras.Rows(), a_views.extras.ContentEpoch(), [&](const Target& a_target, Sink& a_to) {
												   RangeGather gather{ uploadRanges, uploadBytes, false };
												   EmitExtras(a_views.extras, a_target.contentEpoch, nullptr, gather);
												   gather.Send(a_to);
												   return a_views.extras.ContentEpoch();
											   } };
		// The geometry table: the main payloads' and the shadow payload's are the same (the publication's stream views and face streams).
		const GeometryDrawsOut& geometries = a_main ? a_main->body->geometryDraws : a_shadow->geometries;
		units[Part(PayloadPart::Geometries)] = geometries.Count();
		requests[Part(PayloadPart::Geometries)] = { Words<GeometryDraw>(geometries.Count()), geometries.ContentEpoch(), [&](const Target& a_target, Sink& a_to) {
													   RangeGather gather{ uploadRanges, uploadBytes, false };
													   EmitGeometryDraws(geometries, a_target.contentEpoch, gather);
													   gather.Send(a_to);
													   return geometries.ContentEpoch();
												   } };
		// The main rows (the material rows by part: kMaterialRowParts), their headers made absolute for the backing they go to (a
		// backing at another address is written whole: a fresh one holds nothing).
		if (a_main) {
			units[Part(PayloadPart::MaterialRows)] = a_main->body->materialRows.Count();
			requests[Part(PayloadPart::MaterialRows)] = { a_main->body->materialRows.Count(), a_main->body->materialRows.ContentEpoch(), [&](const Target& a_target, Sink& a_to) {
															 RangeGather gather{ uploadRanges, uploadBytes, true };
															 EmitMainRowParts(a_main->body->materialRows, kMaterialRowParts, a_target.contentEpoch, a_target.address, a_target.capacityUnits,
																 [](MaterialRow& a_row, std::uint64_t a_address) { PatchRowAddresses(a_row, a_address); }, gather);
															 gather.Send(a_to);
															 return a_main->body->materialRows.ContentEpoch();
														 } };
			units[Part(PayloadPart::PipelineRows)] = a_main->body->pipelineRows.Count();
			requests[Part(PayloadPart::PipelineRows)] = { a_main->body->pipelineRows.Count(), a_main->body->pipelineRows.ContentEpoch(), [&](const Target& a_target, Sink& a_to) {
															 RangeGather gather{ uploadRanges, uploadBytes, true };
															 EmitMainRows(a_main->body->pipelineRows, a_target.contentEpoch, a_target.address, a_target.capacityUnits,
																 [](PipelineRow& a_row, std::uint64_t a_address) { PatchRowAddresses(a_row, a_address); }, gather);
															 gather.Send(a_to);
															 return a_main->body->pipelineRows.ContentEpoch();
														 } };
		}
		// The scene list: the region every payload of the publication holds at its head (what changed since), then the main payloads'
		// frame inputs, then the shadow payload's (both written whole: they are the frame's).
		const KeptView<DrawInput>& region = a_main ? a_main->resident : a_shadow->regionInputs;
		const std::vector<DrawInput>* mainList = a_main ? &a_main->body->inputList : nullptr;
		const std::vector<DrawInput>* shadowList = a_shadow ? &a_shadow->inputList : nullptr;
		out->regionCount = region.Count();
		out->mainInputsAt = out->regionCount;
		out->shadowInputsAt = out->mainInputsAt + (mainList ? mainList->size() : 0);
		units[Part(PayloadPart::Inputs)] = out->shadowInputsAt + (shadowList ? shadowList->size() : 0);
		requests[Part(PayloadPart::Inputs)] = { Words<DrawInput>(units[Part(PayloadPart::Inputs)]), region.ContentEpoch(), [&](const Target& a_target, Sink& a_to) {
												   RangeGather gather{ uploadRanges, uploadBytes, false };
												   region.Emit(a_target.contentEpoch, gather);
												   if (mainList && !mainList->empty())
													   gather(mainList->data(), mainList->size() * sizeof(DrawInput), out->mainInputsAt * sizeof(DrawInput));
												   if (shadowList && !shadowList->empty())
													   gather(shadowList->data(), shadowList->size() * sizeof(DrawInput), out->shadowInputsAt * sizeof(DrawInput));
												   gather.Send(a_to);
												   return region.ContentEpoch();
											   } };
		// The shadow payload's rows.
		if (a_shadow) {
			units[Part(PayloadPart::ShadowRows)] = a_shadow->materialRows.Count();
			requests[Part(PayloadPart::ShadowRows)] = { a_shadow->materialRows.Count(), a_shadow->materialRows.ContentEpoch(), [&](const Target& a_target, Sink& a_to) {
														   RangeGather gather{ uploadRanges, uploadBytes, false };
														   a_shadow->materialRows.Emit(a_target.contentEpoch, gather);
														   gather.Send(a_to);
														   return a_shadow->materialRows.ContentEpoch();
													   } };
		}

		// The parts unchanged since the last publication share its versions (ShareUnchanged): absent from the set's publication.
		const auto previous = last.lock();
		const auto shared = ShareUnchanged(requests, previous.get(), geometries.streamRows, mainList, shadowList);
		JudgeAppends(requests, shared, previous.get(), { a_views, geometries, a_main, a_shadow, region, mainList, shadowList, out->mainInputsAt, out->shadowInputsAt });
		auto publication = set->Publish(requests, a_uploads);
		CheckExtrasBacking(a_views.extras, a_views.tables.get(), publication, shared, previous.get());
		for (std::size_t k = 0; k < kPayloadParts; ++k) {
			if (shared[k]) {
				out->parts[k] = { previous->parts[k].srvIndex, previous->parts[k].address, units[k] };
				out->leases[k] = previous->leases[k];
				out->shared[k] = true;
				++stats.shared[k];
				continue;
			}
			auto& version = publication.parts[k];
			// What the next publication may share: this part's version (none when absent).
			kept[k] = version.lease ? KeptPart{ version.contentEpoch, version.units } : KeptPart{};
			if (!version.lease)
				continue;
			out->parts[k] = { version.srvIndex, version.address, units[k] };
			out->bytes[k] = version.bytes;
			out->runs[k] = version.runs;
			out->allocated[k] = version.allocated;
			out->appended[k] = version.appended;
			out->leases[k] = std::move(version.lease);
			stats.bytes[k] += version.bytes;
			stats.runs[k] += version.runs;
			stats.allocated[k] += version.allocated ? 1 : 0;
		}
		CheckAppends(*out, publication);
		KeepTails(shared, geometries.streamRows, mainList, shadowList);
		out->ready = std::move(publication.ready);
		out->serial = ++serial;
		++stats.publications;
		last = out;
		return out;
	}

	std::array<bool, kPayloadParts> PayloadBuilder::ShareUnchanged(std::span<org::services::JournaledPartRequest> a_requests, const PayloadVersions* a_previous,
		const std::vector<GeometryDraw>& a_streamRows, const std::vector<DrawInput>* a_mainList, const std::vector<DrawInput>* a_shadowList)
	{
		// A part shares the last publication's version when that version is what a catch-up would make: its journal at the position the
		// backing still holds (a discarded one holds 0), the same units and whole tails, its uploads done (a failed one's backings were
		// discarded). A catch-up into another backing would only send that backing's lag again.
		std::array<bool, kPayloadParts> shared{};
		if (!a_previous || !a_previous->Ready() || (a_previous->ready && a_previous->ready->Failed()))
			return shared;
		for (std::size_t k = 0; k < kPayloadParts; ++k) {
			auto& request = a_requests[k];
			const auto& held = kept[k];
			const auto& lease = a_previous->leases[k];
			if (!request.emit || !lease || !held.contentEpoch || held.contentEpoch != request.preferEpoch || held.units != request.units ||
				lease->contentEpoch.load(std::memory_order_acquire) != held.contentEpoch)
				continue;
			if (k == Part(PayloadPart::Geometries) && !SameRows(keptStreamRows, a_streamRows.data(), a_streamRows.size()))
				continue;
			if (k == Part(PayloadPart::Inputs) && !(SameList(keptMainList, a_mainList) && SameList(keptShadowList, a_shadowList)))
				continue;
			shared[k] = true;
			request = {};
		}
		return shared;
	}

	void PayloadBuilder::JudgeAppends(std::span<org::services::JournaledPartRequest> a_requests, const std::array<bool, kPayloadParts>& a_shared,
		const PayloadVersions* a_previous, const PartRuns& a_runs)
	{
		if (!appendInPlace && !Diagnostics::Observe().stats)
			return;
		ZoneScopedN("CS.DCLF.Payload.JudgeAppends");
		// The head is the last publication's backing of the part, once its uploads are done (an append's payload waits for its own
		// uploads only: what the head held before must have landed). A run straddling the mark counts on both sides.
		const bool headsReady = a_previous && a_previous->Ready() && !(a_previous->ready && a_previous->ready->Failed());
		for (std::size_t k = 0; k < kPayloadParts; ++k) {
			auto& request = a_requests[k];
			if (a_shared[k] || (!request.units && !request.emit))
				continue;
			auto& classes = stats.appends[k];
			const auto* head = headsReady && a_previous->leases[k] ? &a_previous->leases[k] : nullptr;
			if (!head) {
				++classes.headless;
				continue;
			}
			const std::uint64_t mark = (*head)->sealedBytes.load(std::memory_order_relaxed);
			std::uint64_t past = 0, below = 0;
			a_runs(static_cast<PayloadPart>(k), (*head)->contentEpoch.load(std::memory_order_acquire), [&](std::uint64_t a_offset, std::uint64_t a_bytes) {
				const std::uint64_t under = a_offset < mark ? std::min(a_bytes, mark - a_offset) : 0;
				below += under;
				past += a_bytes - under;
			});
			if (below && past) {
				++classes.mixed;
				classes.mixedBytes += below + past;
			} else if (below) {
				++classes.rewrites;
				classes.rewriteBytes += below;
			} else if (request.units > (*head)->capacityClass) {
				++classes.grown;
				classes.grownBytes += past;
			} else {
				++classes.pure;
				classes.pureBytes += past;
				if (appendInPlace) {
					request.appendInto = *head;
					++classes.appended;
				}
			}
		}
	}

	void PayloadBuilder::CheckExtrasBacking(const ExtrasOut& a_rows, const SceneTables* a_tables, const org::services::JournaledPublication& a_publication,
		const std::array<bool, kPayloadParts>& a_shared, const PayloadVersions* a_previous) const
	{
		// CS_DCLF_PERSISTENT_PARITY: the extras rows are checked by the backing that holds them, as they are caught up into it (the
		// observer's mirror of that backing, replayed from what it held before; null without the observer). A shared backing is
		// caught up from where it is (nothing to replay) and checked again; an appended one from where it was.
		if (const auto& extras = a_publication.parts[Part(PayloadPart::Extras)]; a_shared[Part(PayloadPart::Extras)])
			CheckExtras(a_rows, a_previous->leases[Part(PayloadPart::Extras)], kept[Part(PayloadPart::Extras)].contentEpoch, a_tables);
		else if (extras.lease)
			CheckExtras(a_rows, extras.lease, extras.caughtUpFrom, a_tables);
	}

	void PayloadBuilder::CheckAppends(const PayloadVersions& a_payload, const org::services::JournaledPublication& a_publication)
	{
		if (!Diagnostics::Observe().scene)
			return;
		// Every append's first byte against the most the payloads named of its backing (kept here from the units published, apart
		// from the set's mark), then what this payload names: a shared or appended part adds to its backing's, a leased one starts it.
		for (std::size_t k = 0; k < kPayloadParts; ++k) {
			const auto& lease = a_payload.leases[k];
			if (!lease)
				continue;
			auto& mark = namedMarks[k];
			const bool same = mark.generation == lease->generation;
			if (a_payload.appended[k]) {
				const std::uint64_t first = a_publication.parts[k].firstByte;
				const bool above = same && (first == ~std::uint64_t{ 0 } || first >= mark.bytes);
				++stats.appendChecks;
				if (!above && stats.appendsBelowMark++ == 0)
					stats.firstBelowMark = fmt::format("{} backing {}: first byte {} against {} bytes named{}", set->Part(k).name, lease->generation, first,
						same ? mark.bytes : 0, same ? "" : " (not the head)");
			}
			const std::uint64_t named = a_payload.parts[k].units * kElementBytes[k];
			mark = { lease->generation, same && (a_payload.shared[k] || a_payload.appended[k]) ? std::max(mark.bytes, named) : named };
		}
	}

	void PayloadBuilder::KeepTails(const std::array<bool, kPayloadParts>& a_shared, const std::vector<GeometryDraw>& a_streamRows, const std::vector<DrawInput>* a_mainList,
		const std::vector<DrawInput>* a_shadowList)
	{
		// The whole tails the versions written now hold (a shared one's are the same already).
		if (!a_shared[Part(PayloadPart::Geometries)])
			keptStreamRows.assign(a_streamRows.begin(), a_streamRows.end());
		if (!a_shared[Part(PayloadPart::Inputs)]) {
			a_mainList ? keptMainList.assign(a_mainList->begin(), a_mainList->end()) : keptMainList.clear();
			a_shadowList ? keptShadowList.assign(a_shadowList->begin(), a_shadowList->end()) : keptShadowList.clear();
		}
	}

	void PayloadBuilder::Discard(const PayloadVersions& a_payload)
	{
		// The backings hold nothing a later publication may catch up from: written whole when next leased (a shared one too: the
		// publication that wrote it keeps its bytes, which only lose their journal position), and none is shared again.
		for (const auto& lease : a_payload.leases)
			if (lease)
				lease->contentEpoch.store(0, std::memory_order_release);
		kept = {};
		last.reset();
	}

	std::optional<std::uint64_t> PayloadBuilder::OldestContentEpoch(PayloadPart a_part) const
	{
		return set->OldestContentEpoch(Part(a_part));
	}

	org::services::BackingPool::Census PayloadBuilder::Census(PayloadPart a_part) const noexcept
	{
		return set->Pool(Part(a_part))->Count();
	}

	PayloadBuilder::Stats PayloadBuilder::TakeStats()
	{
		auto taken = std::exchange(stats, {});
		for (std::size_t k = 0; k < kPayloadParts; ++k)
			taken.pools[k] = set->Pool(k)->Count();
		return taken;
	}
}
