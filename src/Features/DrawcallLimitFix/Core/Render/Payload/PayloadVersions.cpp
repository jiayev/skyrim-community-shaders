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

		template <class T>
		constexpr std::uint64_t Words(std::uint64_t a_elements) { return a_elements * sizeof(T) / 4; }
	}

	PayloadBuilder::PayloadBuilder(std::shared_ptr<org::services::RetirementClock> a_clock) :
		// DCLF's allocate (structured buffers with their SRV, device addresses for the row tables) and retire (released backings are
		// destroyed on the graph host's cleanup lane, never on the thread that let them go). No caps: a backing is made whenever none is
		// reusable, and the retirement clock (exact GPU completion) frees them.
		set(std::make_unique<org::services::JournaledBufferSet>(PartConfigs(), PooledBackings::Hooks(), std::move(a_clock),
			org::services::BackingPool::Config{ .name = "cs.dclf.payload", .maxPerClass = 0, .spareFree = PooledBackings::PerFrameSpares() }))
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
			out->products = a_main->products;
			Extensions::CarryPayloadProducts(out->products);
		}
		out->shadow = a_shadow != nullptr;
		std::array<std::uint64_t, kPayloadParts> units{};  // each part's elements (PayloadBuffer::units)

		// The objects' records and the extras rows: the publication's stream views.
		units[Part(PayloadPart::Objects)] = a_views.objects.Count();
		requests[Part(PayloadPart::Objects)] = { a_views.objects.Count(), a_views.objects.ContentEpoch(), [&](const Target& a_target, Sink& a_sink) {
													 a_views.objects.Emit(a_target.contentEpoch, a_sink);
													 return a_views.objects.ContentEpoch();
												 } };
		units[Part(PayloadPart::Extras)] = a_views.extras.Rows();
		requests[Part(PayloadPart::Extras)] = { a_views.extras.Rows(), a_views.extras.ContentEpoch(), [&](const Target& a_target, Sink& a_sink) {
												   EmitExtras(a_views.extras, a_target.contentEpoch, nullptr, a_sink);
												   return a_views.extras.ContentEpoch();
											   } };
		// The geometry table: the main payloads' and the shadow payload's are the same (the publication's stream views and face streams).
		const GeometryDrawsOut& geometries = a_main ? a_main->geometryDraws : a_shadow->geometries;
		units[Part(PayloadPart::Geometries)] = geometries.Count();
		requests[Part(PayloadPart::Geometries)] = { Words<GeometryDraw>(geometries.Count()), geometries.ContentEpoch(), [&](const Target& a_target, Sink& a_sink) {
													   EmitGeometryDraws(geometries, a_target.contentEpoch, a_sink);
													   return geometries.ContentEpoch();
												   } };
		// The main rows (the material rows by part: kMaterialRowParts), their headers made absolute for the backing they go to (a
		// backing at another address is written whole: a fresh one holds nothing).
		if (a_main) {
			units[Part(PayloadPart::MaterialRows)] = a_main->materialRows.Count();
			requests[Part(PayloadPart::MaterialRows)] = { a_main->materialRows.Count(), a_main->materialRows.ContentEpoch(), [&](const Target& a_target, Sink& a_sink) {
															 EmitMainRowParts(a_main->materialRows, kMaterialRowParts, a_target.contentEpoch, a_target.address, a_target.capacityUnits,
																 [](MaterialRow& a_row, std::uint64_t a_address) { PatchRowAddresses(a_row, a_address); }, a_sink);
															 return a_main->materialRows.ContentEpoch();
														 } };
			units[Part(PayloadPart::PipelineRows)] = a_main->pipelineRows.Count();
			requests[Part(PayloadPart::PipelineRows)] = { a_main->pipelineRows.Count(), a_main->pipelineRows.ContentEpoch(), [&](const Target& a_target, Sink& a_sink) {
															 EmitMainRows(a_main->pipelineRows, a_target.contentEpoch, a_target.address, a_target.capacityUnits,
																 [](PipelineRow& a_row, std::uint64_t a_address) { PatchRowAddresses(a_row, a_address); }, a_sink);
															 return a_main->pipelineRows.ContentEpoch();
														 } };
		}
		// The scene list: the region every payload of the publication holds at its head (what changed since), then the main payloads'
		// frame inputs, then the shadow payload's (both written whole: they are the frame's).
		const KeptView<DrawInput>& region = a_main ? a_main->resident : a_shadow->regionInputs;
		const std::vector<DrawInput>* mainList = a_main ? &a_main->inputList : nullptr;
		const std::vector<DrawInput>* shadowList = a_shadow ? &a_shadow->inputList : nullptr;
		out->regionCount = region.Count();
		out->mainInputsAt = out->regionCount;
		out->shadowInputsAt = out->mainInputsAt + (mainList ? mainList->size() : 0);
		units[Part(PayloadPart::Inputs)] = out->shadowInputsAt + (shadowList ? shadowList->size() : 0);
		requests[Part(PayloadPart::Inputs)] = { Words<DrawInput>(units[Part(PayloadPart::Inputs)]), region.ContentEpoch(), [&](const Target& a_target, Sink& a_sink) {
												   region.Emit(a_target.contentEpoch, a_sink);
												   if (mainList && !mainList->empty())
													   a_sink(mainList->data(), mainList->size() * sizeof(DrawInput), out->mainInputsAt * sizeof(DrawInput));
												   if (shadowList && !shadowList->empty())
													   a_sink(shadowList->data(), shadowList->size() * sizeof(DrawInput), out->shadowInputsAt * sizeof(DrawInput));
												   return region.ContentEpoch();
											   } };
		// The shadow payload's rows.
		if (a_shadow) {
			units[Part(PayloadPart::ShadowRows)] = a_shadow->materialRows.Count();
			requests[Part(PayloadPart::ShadowRows)] = { a_shadow->materialRows.Count(), a_shadow->materialRows.ContentEpoch(), [&](const Target& a_target, Sink& a_sink) {
														   a_shadow->materialRows.Emit(a_target.contentEpoch, a_sink);
														   return a_shadow->materialRows.ContentEpoch();
													   } };
		}

		auto publication = set->Publish(requests, a_uploads);
		// CS_DCLF_PERSISTENT_PARITY: the extras rows are checked by the backing that holds them, as they are caught up into it (the
		// observer's mirror of that backing, replayed from what it held before; null without the observer).
		if (const auto& extras = publication.parts[Part(PayloadPart::Extras)]; extras.lease) {
			std::vector<float>* mirror = nullptr;
			Diagnostics::Notify(Diagnostics::ExtrasSending{ extras.lease, mirror });
			if (mirror)
				EmitExtras(a_views.extras, extras.caughtUpFrom, mirror, [](const void*, std::size_t, std::size_t) {});
			Diagnostics::Notify(Diagnostics::ExtrasSent{ extras.lease, a_views.extras });
		}
		for (std::size_t k = 0; k < kPayloadParts; ++k) {
			auto& version = publication.parts[k];
			if (!version.lease)
				continue;
			out->parts[k] = { version.srvIndex, version.address, units[k] };
			out->bytes[k] = version.bytes;
			out->runs[k] = version.runs;
			out->allocated[k] = version.allocated;
			out->leases[k] = std::move(version.lease);
			stats.bytes[k] += version.bytes;
			stats.runs[k] += version.runs;
			stats.allocated[k] += version.allocated ? 1 : 0;
		}
		out->ready = std::move(publication.ready);
		out->serial = ++serial;
		++stats.publications;
		return out;
	}

	void PayloadBuilder::Discard(const PayloadVersions& a_payload)
	{
		// The backings hold nothing a later publication may catch up from: written whole when next leased.
		for (const auto& lease : a_payload.leases)
			if (lease)
				lease->contentEpoch.store(0, std::memory_order_release);
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
