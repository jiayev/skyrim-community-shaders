#if defined(CS_HAS_RENDER_GRAPH) && defined(CS_HAS_ORG_MODULE_SERVICES)
#	include "DrawsState.h"
#	include "DrawsView.h"
#	include "Events.h"
#	include "Observers.h"

// CS_DCLF_GBUFFER_PROBE=<x>x<y>: every main-pass target's texel and the depth at the main pass's probe points, copied out by the
// in-epoch probe passes and read back a few frames later.

namespace DCLF::Diagnostics
{
	struct ProbeBindings
	{
		std::array<org::ResourceBindingToken, kColorTargets> sources;
		org::ResourceBindingToken depth;
		org::ResourceBindingToken destination;
	};

	struct ProbeFrame
	{
		std::uint32_t count = 0;
		std::uint32_t base = 0;        // first slot this pass writes
		std::uint32_t depthSlot = ~0u;  // where this pass puts the depth texel, if it samples it
		std::uint32_t x = 0, y = 0;
	};

	// Copies one texel of each main-pass target into the probe buffer. Two of these are declared, one
	// either side of the opaque pass, so the same texel is sampled before and after DCLF's colour draws
	// within a single frame.
	class ProbePass final : public org::TypedRenderGraphPass<ProbePass, ProbeFrame, ProbeBindings>
	{
	public:
		ProbePass(std::shared_ptr<Resources> a_resources, RenderGraphRuntime::Segment a_segment, bool a_after) :
			resources(std::move(a_resources)), segment(a_segment), after(a_after) {}

		ProbeBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			ProbeBindings bindings{};
			for (std::uint32_t i = 0; i < resources->targetCount; ++i)
				bindings.sources[i] = a_builder.CopySource(resources->native[i]);
			bindings.depth = a_builder.CopySource(resources->nativeDepth);
			bindings.destination = a_builder.CopyDestination(resources->probe);
			return bindings;
		}

		void InvocationRevision([[maybe_unused]] const org::PassPrepareContext& a_preparation, std::vector<std::uint64_t>& a_out) const
		{
			const auto now = segment;
			const auto frame = CurrentFrame(a_preparation,
				now == RenderGraphRuntime::Segment::SkyOcclusion || now == RenderGraphRuntime::Segment::LightCulling ? RenderGraphRuntime::Segment::ZPrepass : now);
			a_out.push_back(frame ? frame->generation : 0);
			a_out.push_back(static_cast<std::uint64_t>(now));
		}

		ProbeFrame Prepare(const ProbeBindings&, [[maybe_unused]] const org::PassPrepareContext& a_preparation) const
		{
			ProbeFrame prepared{};
			const auto now = segment;
			const bool zPrepass = now == RenderGraphRuntime::Segment::ZPrepass;
			const bool gapProbe = now == RenderGraphRuntime::Segment::SkyOcclusion || now == RenderGraphRuntime::Segment::LightCulling;
			if (now != RenderGraphRuntime::Segment::MainOpaque && !(zPrepass && after) && !gapProbe)
				return prepared;
			const auto frame = CurrentFrame(a_preparation, gapProbe ? RenderGraphRuntime::Segment::ZPrepass : now);
			if (!frame || !frame->probePixel)
				return prepared;
			prepared.x = frame->probeX;
			prepared.y = frame->probeY;
			if (zPrepass) {
				prepared.depthSlot = kProbeDepthAfterPrepass;  // what the Z-prepass left in the buffer
				return prepared;
			}
			if (gapProbe) {
				prepared.depthSlot = now == RenderGraphRuntime::Segment::SkyOcclusion ? kProbeDepthAfterSky : kProbeDepthAfterLightCulling;
				return prepared;
			}
			prepared.count = resources->targetCount;
			prepared.base = after ? kColorTargets : 0;
			// What the colour pass tests against.
			prepared.depthSlot = after ? kProbeDepthAfterColour : kProbeDepthBeforeColour;
			return prepared;
		}

		static void Record(const ProbeBindings& a_bindings, const ProbeFrame& a_frame, org::PassRecordContext& a_recording)
		{
			if (!a_frame.count && a_frame.depthSlot == ~0u)
				return;
			auto& commands = a_recording.Commands();
			const auto buffer = a_recording.Resolve(a_bindings.destination).GetHandle();
			if (a_frame.depthSlot != ~0u) {
				rhi::BufferTextureCopyFootprint copy{};
				copy.texture = a_recording.Resolve(a_bindings.depth).GetHandle();
				copy.buffer = buffer;
				copy.x = a_frame.x;
				copy.y = a_frame.y;
				copy.footprint.offset = std::uint64_t(a_frame.depthSlot) * kProbeSlotBytes;
				copy.footprint.rowPitch = kProbeSlotBytes;
				copy.footprint.width = 1;
				copy.footprint.height = 1;
				copy.footprint.depth = 1;
				commands.CopyTextureToBuffer(copy);
			}
			for (std::uint32_t i = 0; i < a_frame.count; ++i) {
				rhi::BufferTextureCopyFootprint copy{};
				copy.texture = a_recording.Resolve(a_bindings.sources[i]).GetHandle();
				copy.buffer = buffer;
				copy.mip = 0;
				copy.arraySlice = 0;
				copy.x = a_frame.x;
				copy.y = a_frame.y;
				copy.z = 0;
				copy.footprint.offset = std::uint64_t(a_frame.base + i) * kProbeSlotBytes;
				copy.footprint.rowPitch = kProbeSlotBytes;
				copy.footprint.width = 1;
				copy.footprint.height = 1;
				copy.footprint.depth = 1;
				commands.CopyTextureToBuffer(copy);
			}
		}

	private:
		std::shared_ptr<Resources> resources;
		RenderGraphRuntime::Segment segment;
		bool after = false;
	};

	void NoteProbeTargets(const MainPassCaptured& a_captured)
	{
		auto& state = DrawsState::Get();
		state.probeTargetCount = std::min<std::uint32_t>(a_captured.capture.targetCount, kColorTargets);
		for (std::uint32_t i = 0; i < state.probeTargetCount; ++i)
			state.probeTargets[i] = a_captured.capture.targets[i];
	}

	void ProbeGBuffer(const ProbePoint&)
	{
		const auto& resources = DrawsView::Get().resources;
		auto& state = DrawsState::Get();
		auto& gbufferStaging = state.gbufferStaging;
		auto& gbufferFramesLeft = state.gbufferFramesLeft;
		auto& gbufferEpochs = state.gbufferEpochs;
		auto& gbufferCount = state.gbufferCount;
		auto& gbufferX = state.gbufferX;
		auto& gbufferY = state.gbufferY;
		auto& gbufferBytes = state.gbufferBytes;
		const auto& probeTargets = state.probeTargets;
		const auto probeTargetCount = state.probeTargetCount;
		if (!resources || !resources->probeD3D11)
			return;
		auto* context = globals::d3d::context;
		if (gbufferStaging) {
			if (--gbufferFramesLeft)
				return;
			D3D11_MAPPED_SUBRESOURCE mapped{};
			if (SUCCEEDED(context->Map(gbufferStaging.get(), 0, D3D11_MAP_READ, 0, &mapped))) {
				const auto* bytes = static_cast<const std::uint8_t*>(mapped.pData);
				auto slots = [&](std::uint32_t a_base) {
					std::string text;
					for (std::uint32_t i = 0; i < gbufferCount; ++i) {
						const auto* texel = bytes + std::size_t(a_base + i) * kProbeSlotBytes;
						std::string hex;
						for (std::uint32_t b = 0; b < gbufferBytes[i]; ++b)
							hex += fmt::format("{:02X}", texel[b]);
						text += fmt::format("{}rt{}={}", text.empty() ? "" : " | ", i, hex);
					}
					return text;
				};
				auto depthAt = [&](std::uint32_t a_slot) {
					std::uint32_t packed = 0;
					std::memcpy(&packed, bytes + std::size_t(a_slot) * kProbeSlotBytes, sizeof(packed));
					return packed & 0x00FFFFFFu;
				};
				logger::info("[DCLF] G-buffer at ({}, {}) before DCLF's colour draws: {}", gbufferX, gbufferY, slots(0));
				logger::info("[DCLF] G-buffer at ({}, {}) after  DCLF's colour draws: {}", gbufferX, gbufferY, slots(kColorTargets));
				logger::info("[DCLF] depth at ({}, {}): {:06X} after the z-prepass, {:06X} when the colour pass tests it, {:06X} after it",
					gbufferX, gbufferY, depthAt(kProbeDepthAfterPrepass), depthAt(kProbeDepthBeforeColour), depthAt(kProbeDepthAfterColour));
				logger::info("[DCLF] depth gap at ({}, {}): {:06X} after sky occlusion, {:06X} after light culling",
					gbufferX, gbufferY, depthAt(kProbeDepthAfterSky), depthAt(kProbeDepthAfterLightCulling));
				context->Unmap(gbufferStaging.get(), 0);
			}
			gbufferStaging = nullptr;
			return;
		}
		if ((gbufferEpochs++ % 480) != 0)
			return;
		D3D11_BUFFER_DESC desc{};
		resources->probeD3D11->GetDesc(&desc);
		desc.Usage = D3D11_USAGE_STAGING;
		desc.BindFlags = 0;
		desc.MiscFlags = 0;
		desc.StructureByteStride = 0;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		winrt::com_ptr<ID3D11Buffer> staging;
		if (FAILED(globals::d3d::device->CreateBuffer(&desc, nullptr, staging.put())))
			return;
		ScopedPerfEvent event("CS DCLF: G-buffer probe readback");
		context->CopyResource(staging.get(), resources->probeD3D11.get());
		gbufferStaging = std::move(staging);
		gbufferFramesLeft = 4;
		gbufferCount = probeTargetCount;
		for (std::uint32_t i = 0; i < gbufferCount; ++i) {
			D3D11_TEXTURE2D_DESC textureDesc{};
			if (probeTargets[i])
				probeTargets[i]->GetDesc(&textureDesc);
			gbufferBytes[i] = FormatBytes(textureDesc.Format);
		}
		if (const auto& pixel = Diagnostics::Observe().gbufferProbe; !pixel.empty()) {
			if (const auto sep = pixel.find_first_of(",x"); sep != std::string::npos) {
				gbufferX = static_cast<std::uint32_t>(std::strtoul(pixel.substr(0, sep).c_str(), nullptr, 10));
				gbufferY = static_cast<std::uint32_t>(std::strtoul(pixel.substr(sep + 1).c_str(), nullptr, 10));
			}
		}
	}

	void AppendProbePasses(const MainGraphPasses& a_passes)
	{
		using Segment = RenderGraphRuntime::Segment;
		const auto& resources = a_passes.resources;
		if (!resources->probe)
			return;
		auto& a_out = a_passes.out;
		switch (a_passes.point) {
		case MainGraphPasses::kBeforeColour:
			a_out.push_back(org::RenderGraph::ExternalPassDesc::Copy("cs.dclf.probe-before",
				std::static_pointer_cast<org::RenderPass>(std::make_shared<ProbePass>(resources, Segment::MainOpaque, false)))
					.Epoch(a_passes.epoch));
			break;
		case MainGraphPasses::kAfterColour:
			a_out.push_back(org::RenderGraph::ExternalPassDesc::Copy("cs.dclf.probe-after",
				std::static_pointer_cast<org::RenderPass>(std::make_shared<ProbePass>(resources, Segment::MainOpaque, true)))
					.Epoch(a_passes.epoch));
			break;
		case MainGraphPasses::kAfterDepth:
			a_out.push_back(org::RenderGraph::ExternalPassDesc::Copy("cs.dclf.z.probe-after",
				std::static_pointer_cast<org::RenderPass>(std::make_shared<ProbePass>(resources, Segment::ZPrepass, true)))
					.Epoch(a_passes.epoch));
			for (const auto segment : { Segment::SkyOcclusion, Segment::LightCulling })
				a_out.push_back(org::RenderGraph::ExternalPassDesc::Copy(segment == Segment::SkyOcclusion ? "cs.dclf.sky.probe-depth" : "cs.dclf.light.probe-depth",
					std::static_pointer_cast<org::RenderPass>(std::make_shared<ProbePass>(resources, segment, false)))
						.Epoch(RenderGraphRuntime::EpochOf(segment)));
			break;
		}
	}
}

#endif
