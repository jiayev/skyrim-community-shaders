#if defined(CS_HAS_RENDER_GRAPH) && defined(CS_HAS_ORG_MODULE_SERVICES)
#	include "Features/DrawcallLimitFix/Draws/IndirectDraws/Internal.h"
#include "Features/DrawcallLimitFix/Scene/FrameView/FrameView.h"
#	include "Features/DrawcallLimitFix/Diagnostics/Draws/Events.h"

namespace DCLF
{
	/*
	 * The payload ring (Impl::payloadRing): each frame's entry brought up to the installed publication by the frame's producer
	 * on the dedicated uploader, before it signals the frame's wait.
	 */

	std::function<void(org::runtime::IUploadService&)> IndirectDraws::PrepareFrameUploads()
	{
		if (!impl)
			return {};
		auto& s = *impl;
		s.ringFrame = {};
		auto draws = s.installedDraws;
		auto* host = RenderGraphRuntime::Get().Host();
		if (failed || !draws || !draws->streams || !host || (!draws->payloads[kAsyncZPrepass] && !draws->payloads[kAsyncColour] && !draws->shadow))
			return {};
		auto device = host->GetDesc().device;
		const std::uint32_t r = static_cast<std::uint32_t>(s.payloadRingSeq++ % Impl::kPayloadRing);
		auto& entry = s.payloadRing[r];
		const auto& views = *draws->streams;
		// What the publication's payloads need of each buffer (the colour build's rows are the newest: it ran after the Z-prepass's).
		const MainPayload* newest = draws->payloads[kAsyncColour] ? draws->payloads[kAsyncColour].get() : draws->payloads[kAsyncZPrepass].get();
		const ShadowPayload* shadow = draws->shadow.get();
		// The geometry table: the main payloads' and the shadow payload's are the same (the publication's stream views and face streams).
		std::uint64_t geometryRows = shadow ? shadow->geometries.Count() : 0;
		// The scene list (U4c): the region every payload of the publication holds, then the main payloads' frame inputs, then the shadow
		// payload's.
		if (newest)
			geometryRows = std::max<std::uint64_t>(geometryRows, newest->geometryDraws.Count());
		const std::uint64_t inputs = (newest ? newest->resident.Count() + newest->inputList.size() : shadow ? shadow->RegionCount() : 0) +
		                             (shadow ? shadow->inputList.size() : 0);
		bool grown = false;
		auto ensure = [&](Impl::RingPart& a_part, std::uint64_t a_elements, std::uint32_t a_stride, const char* a_name, bool a_address) {
			if (a_part.buffer && a_part.capacity >= a_elements)
				return;
			std::uint64_t capacity = std::max<std::uint64_t>(a_part.capacity, 64);
			while (capacity < a_elements)
				capacity *= 2;
			auto buffer = org::Buffer::CreateUnmaterializedStructuredBuffer(static_cast<std::uint32_t>(capacity), a_stride, false);
			buffer->SetName(fmt::format("cs.dclf.payload-ring.{}.{}", a_name, r).c_str());
			buffer->Materialize();
			// The old one goes when the frames that read it retire.
			if (a_part.buffer)
				Scene::FrameView::Get().RetireImport(std::move(a_part.buffer));
			a_part.buffer = std::move(buffer);
			a_part.capacity = capacity;
			a_part.srvIndex = a_part.buffer->GetSRVInfo(0).slot.index;
			a_part.address = a_address ? device.GetBufferDeviceAddress({ a_part.buffer->GetAPIResource().GetHandle(), 0 }) : 0;
			a_part.held = 0;  // holds nothing yet
			grown = true;
		};
		ensure(entry.objects, views.objects.Count(), sizeof(BindlessObject), "objects", false);
		ensure(entry.extras, views.extras.Rows(), 16, "extras", false);
		ensure(entry.geometries, geometryRows * sizeof(GeometryDraw) / 4, 4, "geometries", false);
		if (newest) {
			ensure(entry.materialRows, newest->materialRows.Count(), kMaterialRowBytes, "material-rows", true);
			ensure(entry.pipelineRows, newest->pipelineRows.Count(), kPipelineRowBytes, "pipeline-rows", true);
		}
		ensure(entry.inputs, inputs * sizeof(DrawInput) / 4, 4, "inputs", false);
		if (shadow) {
			ensure(entry.shadowRows, shadow->materialRows.Count(), sizeof(ShadowMaterialRow), "shadow-rows", true);
		}
		s.ringStats.grown += grown ? 1 : 0;
		++s.ringStats.frames;
		auto& frame = s.ringFrame;
		frame.valid = true;
		frame.entry = r;
		frame.draws = draws;
		frame.objectsIndex = entry.objects.srvIndex;
		frame.extrasIndex = entry.extras.srvIndex;
		frame.geometriesIndex = entry.geometries.srvIndex;
		frame.inputsIndex = entry.inputs.srvIndex;
		if (shadow) {
			frame.shadow = true;
			frame.shadowRows = entry.shadowRows.address;
			frame.shadowInputsIndex = entry.inputs.srvIndex;  // the scene list (U4c)
		}
		frame.materialRows = entry.materialRows.address;
		frame.pipelineRows = entry.pipelineRows.address;
		// The producer's part: the entry brought up to the publication, once the frame that last read it is done on the GPU (its
		// extras rows announced to the observers, Diagnostics::ExtrasSending and ExtrasSent, with the frame it is filled for).
		return [&s, draws = std::move(draws), &entry, r, reuse = entry.reuse, frame = Scene::FrameView::Get().GetFrame()](org::runtime::IUploadService& a_uploads) {
			ZoneScopedN("CS.DCLF.PayloadRing.Fill");
			if (!reuse.Reached() && !reuse.Wait(10000))
				throw std::runtime_error("the frame that last read the payload ring entry did not complete on the GPU");
			std::uint64_t runs = 0, bytes = 0;
			std::array<std::uint64_t, Impl::kRingParts> partBytes{};
			auto sender = [&](const Impl::RingPart& a_part, Impl::RingPartIndex a_index) {
				return [&, target = a_part.buffer, a_index](const void* a_data, std::size_t a_bytes, std::size_t a_offset) {
					const org::StreamingUploadSegment segment{ a_data, a_bytes };
					auto ticket = a_uploads.QueueTrackedStreamingUploadSegments({ &segment, 1 }, a_bytes,
						org::WorkerOwnedDestination{ target, org::WorkerOwnedDestination::Ownership::RetiredFrameRegion }, a_offset);
					if (!ticket || ticket->state.load(std::memory_order_acquire) == org::TrackedUploadTicketState::Cancelled)
						throw std::runtime_error("the dedicated uploader refused the payload ring's uploads");
					++runs;
					bytes += a_bytes;
					partBytes[a_index] += a_bytes;
				};
			};
			auto& holders = s.ringHolders;
			const auto& views = *draws->streams;
			views.objects.Emit(entry.objects.held, sender(entry.objects, Impl::kRingObjects));
			entry.objects.held = views.objects.Version();
			holders.objects.Set(r, entry.objects.held);
			std::vector<float>* extrasMirror = nullptr;
			Diagnostics::Notify(Diagnostics::ExtrasSending{ r, extrasMirror });
			EmitExtras(views.extras, entry.extras.held, extrasMirror, sender(entry.extras, Impl::kRingExtras));
			entry.extras.held = views.extras.Version();
			holders.extras.Set(r, entry.extras.held);
			Diagnostics::Notify(Diagnostics::ExtrasSent{ r, frame, views.extras, ~0u });
			const MainPayload* newest = draws->payloads[kAsyncColour] ? draws->payloads[kAsyncColour].get() : draws->payloads[kAsyncZPrepass].get();
			const ShadowPayload* shadow = draws->shadow.get();
			const auto& geometries = newest ? newest->geometryDraws : shadow->geometries;
			EmitGeometryDraws(geometries, entry.geometries.held, sender(entry.geometries, Impl::kRingGeometries));
			entry.geometries.held = geometries.Version();
			holders.geometries.Set(r, entry.geometries.held);
			// The shadow payload's rows (step 6e S2).
			if (shadow) {
				shadow->materialRows.Emit(entry.shadowRows.held, sender(entry.shadowRows, Impl::kRingShadowRows));
				entry.shadowRows.held = shadow->materialRows.Version();
				holders.shadowRows.Set(r, entry.shadowRows.held);
			}
			// The scene list (U4c): its region at the head (what changed since; every payload of the publication holds the same view), the
			// main payloads' frame inputs after it, then the shadow payload's (whole).
			{
				auto& part = entry.inputs;
				const auto& region = newest ? newest->resident : shadow->regionInputs;
				region.Emit(part.held, sender(part, Impl::kRingResident));
				part.held = region.Version();
				holders.resident.Set(r, part.held);
				std::size_t at = region.Count();
				if (newest && !newest->inputList.empty()) {
					sender(part, Impl::kRingFrameInputs)(newest->inputList.data(), newest->inputList.size() * sizeof(DrawInput), at * sizeof(DrawInput));
					at += newest->inputList.size();
				}
				if (shadow && !shadow->inputList.empty())
					sender(part, Impl::kRingShadowInputs)(shadow->inputList.data(), shadow->inputList.size() * sizeof(DrawInput), at * sizeof(DrawInput));
			}
			if (!newest) {
				s.ringRuns += runs;
				s.ringBytes += bytes;
				for (std::size_t k = 0; k < partBytes.size(); ++k)
					s.ringPartBytes[k] += partBytes[k];
				return;
			}
			// The rows' headers made absolute for this entry's tables.
			EmitMainRows(newest->materialRows, entry.materialRows.held, entry.materialRows.address, entry.materialRows.capacity,
				[](MaterialRow& a_row, std::uint64_t a_address) { PatchRowAddresses(a_row, a_address); }, sender(entry.materialRows, Impl::kRingMaterialRows));
			entry.materialRows.held = newest->materialRows.Version();
			holders.materialRows.Set(r, entry.materialRows.held);
			EmitMainRows(newest->pipelineRows, entry.pipelineRows.held, entry.pipelineRows.address, entry.pipelineRows.capacity,
				[](PipelineRow& a_row, std::uint64_t a_address) { PatchRowAddresses(a_row, a_address); }, sender(entry.pipelineRows, Impl::kRingPipelineRows));
			entry.pipelineRows.held = newest->pipelineRows.Version();
			holders.pipelineRows.Set(r, entry.pipelineRows.held);
			s.ringRuns += runs;
			s.ringBytes += bytes;
			for (std::size_t k = 0; k < partBytes.size(); ++k)
				s.ringPartBytes[k] += partBytes[k];
		};
	}

	void IndirectDraws::DropFrameUploads()
	{
		if (impl)
			impl->ringFrame = {};
	}
}
#endif
