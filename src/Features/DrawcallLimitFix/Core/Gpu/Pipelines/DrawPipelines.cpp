// DrawPipelines: the public face (requests and frame inputs), the pipeline lane's pass (DrainLane: passes on the coordinator
// executor, the published catalog) and the engine states the builds read. Impl is DrawPipelinesImpl.h; the main, shadow,
// forward and draw family pipelines are MainPipelines.cpp, ShadowPipelines.cpp, ForwardPipelines.cpp and FamilyPipelines.cpp.

// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "Features/DrawcallLimitFix/Core/Gpu/BuildExecutor.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelines.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelinesRhi.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/EngineStates.h"
#include "Features/DrawcallLimitFix/Core/Extensions/DrawFamilies.h"
#include "Features/DrawcallLimitFix/Core/Extensions/MaterialPorts.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Materials.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ProgramFamilies.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Views.h"
#include "Features/DrawcallLimitFix/Core/Runtime/EventQueue.h"
#include "Features/DrawcallLimitFix/Core/Runtime/LatestSlot.h"
#include "Features/DrawcallLimitFix/Core/Scene/SceneWake.h"
#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"

#include "RenderGraph/RenderGraphRuntime.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/SpirvReflection.h"
#include "Features/DrawcallLimitFix/Core/Shading/LightingConstants.h"
#include "Features/DrawcallLimitFix/Core/Shading/EngineShaders.h"
#include "Features/DrawcallLimitFix/Core/Shading/LightingDescriptors.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Lookups.h"
#include "Features/DrawcallLimitFix/Core/Shading/VertexInput.h"
#include "ShaderCache.h"

#include <Tracy/Tracy.hpp>

#include <OpenRenderGraph/PersistentGraphHost.h>
#include <ORGModuleServices/PipelineService.h>
#include <rhi_helpers.h>

#include <algorithm>
#include <atomic>
#include <limits>
#include <stdexcept>
#include <string_view>
#include "Features/DrawcallLimitFix/Diagnostics/Observation.h"
#include "Features/DrawcallLimitFix/Diagnostics/Policy.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelinesImpl.h"

namespace DCLF
{
	bool DrawPipelines::Impl::ToRHI(D3D11_BLEND a_factor, rhi::BlendFactor& a_out)
	{
		switch (a_factor) {
		case D3D11_BLEND_ONE: a_out = rhi::BlendFactor::One; return true;
		case D3D11_BLEND_ZERO: a_out = rhi::BlendFactor::Zero; return true;
		case D3D11_BLEND_SRC_COLOR: a_out = rhi::BlendFactor::SrcColor; return true;
		case D3D11_BLEND_INV_SRC_COLOR: a_out = rhi::BlendFactor::InvSrcColor; return true;
		case D3D11_BLEND_SRC_ALPHA: a_out = rhi::BlendFactor::SrcAlpha; return true;
		case D3D11_BLEND_INV_SRC_ALPHA: a_out = rhi::BlendFactor::InvSrcAlpha; return true;
		case D3D11_BLEND_DEST_COLOR: a_out = rhi::BlendFactor::DstColor; return true;
		case D3D11_BLEND_INV_DEST_COLOR: a_out = rhi::BlendFactor::InvDstColor; return true;
		case D3D11_BLEND_DEST_ALPHA: a_out = rhi::BlendFactor::DstAlpha; return true;
		case D3D11_BLEND_INV_DEST_ALPHA: a_out = rhi::BlendFactor::InvDstAlpha; return true;
		default: return false;  // SRC_ALPHA_SAT, BLEND_FACTOR and the SRC1 family: not in the RHI
		}
	}

	bool DrawPipelines::Impl::ToRHI(D3D11_BLEND_OP a_op, rhi::BlendOp& a_out)
	{
		switch (a_op) {
		case D3D11_BLEND_OP_ADD: a_out = rhi::BlendOp::Add; return true;
		case D3D11_BLEND_OP_SUBTRACT: a_out = rhi::BlendOp::Sub; return true;
		case D3D11_BLEND_OP_REV_SUBTRACT: a_out = rhi::BlendOp::RevSub; return true;
		case D3D11_BLEND_OP_MIN: a_out = rhi::BlendOp::Min; return true;
		case D3D11_BLEND_OP_MAX: a_out = rhi::BlendOp::Max; return true;
		default: return false;
		}
	}

	DrawPipelines::Impl::EngineState DrawPipelines::Impl::ReadEngineState(std::uint32_t a_stateBits, std::string& a_error)
	{
		EngineState state{};
		const std::uint32_t bias = RasterDepthBiasMode(a_stateBits);
		const std::uint32_t blendMode = RasterBlendMode(a_stateBits);
		const std::uint32_t writeMode = RasterWriteMode(a_stateBits);
		const std::uint32_t alphaToCoverage = (a_stateBits & kRasterAlphaToCoverage) ? 1u : 0u;
		const std::uint32_t extra = (a_stateBits & kRasterBlendExtra) ? 1u : 0u;
		if (bias >= 12 || blendMode >= 7 || writeMode >= 13) {
			a_error = fmt::format("state indices out of range (bias {}, blend {}, write {})", bias, blendMode, writeMode);
			return state;
		}
		// Fill solid, cull back, no scissor: the cull mode comes from the key, and bias is the same across the table's cull modes.
		auto* raster = EngineRasterStates()[0][1][bias][0];
		// The deferred variant: DCLF's main-pass draws are G-buffer draws.
		auto* blend = DeferredBlendState(blendMode, alphaToCoverage, writeMode, extra);
		if (!raster || !blend) {
			a_error = fmt::format("no engine state object at bias {} / blend [{}][{}][{}][{}] (the deferred blend states are made on the first deferred pass)",
				bias, blendMode, alphaToCoverage, writeMode, extra);
			return state;
		}
		D3D11_RASTERIZER_DESC rasterDesc{};
		raster->GetDesc(&rasterDesc);
		state.depthBias = static_cast<float>(rasterDesc.DepthBias);
		state.depthBiasClamp = rasterDesc.DepthBiasClamp;
		state.slopeScaledDepthBias = rasterDesc.SlopeScaledDepthBias;
		D3D11_BLEND_DESC blendDesc{};
		blend->GetDesc(&blendDesc);
		state.blend.alphaToCoverage = blendDesc.AlphaToCoverageEnable != FALSE;
		state.blend.independentBlend = true;
		state.blend.numAttachments = 8;
		for (std::uint32_t i = 0; i < 8; ++i) {
			const auto& source = blendDesc.RenderTarget[blendDesc.IndependentBlendEnable ? i : 0];
			auto& target = state.blend.attachments[i];
			target.enable = source.BlendEnable != FALSE;
			target.writeMask = static_cast<rhi::ColorWriteEnable>(source.RenderTargetWriteMask & 0xF);
			if (!ToRHI(source.SrcBlend, target.srcColor) || !ToRHI(source.DestBlend, target.dstColor) || !ToRHI(source.BlendOp, target.colorOp) ||
				!ToRHI(source.SrcBlendAlpha, target.srcAlpha) || !ToRHI(source.DestBlendAlpha, target.dstAlpha) || !ToRHI(source.BlendOpAlpha, target.alphaOp)) {
				a_error = fmt::format("blend [{}][{}][{}][{}] target {} uses a blend factor or op the RHI does not express", blendMode, alphaToCoverage, writeMode, extra, i);
				return state;
			}
		}
		state.valid = true;
		return state;
	}

	bool DrawPipelines::Impl::Initialize()
	{
		if (layout)
			return true;
		if (attempted || !RenderGraphRuntime::Get().IsActive() || !RenderGraphRuntime::Get().Host())
			return false;
		attempted = true;
		device = RenderGraphRuntime::Get().Host()->GetDesc().device;

		// No vertex buffer arguments: the vertex stages pull their attributes through the buffers' views in the draw's push data
		// (kDrawPushWords).
		::IndirectCommandsFeatureInfo indirect{};
		supported = device.QueryFeatureInfo(&indirect.header) == rhi::Result::Ok && indirect.pipelineSets && indirect.indexBufferArguments &&
		            indirect.indirectBindings && indirect.maxPipelineSetCount >= kMaxPipelines;
		if (!supported) {
			logger::warn("[DCLF] Indirect pipelines unavailable: pipeline sets {}, index buffers {}, indirect bindings {}, up to {} pipelines", indirect.pipelineSets,
				indirect.indexBufferArguments, indirect.indirectBindings, indirect.maxPipelineSetCount);
			return false;
		}
		// The layouts' push data, the draw's words then the pass's (the frame push; the shadow views', kShadowPushWords, fewer): BasicRHI refuses
		// a layout whose push data exceeds the device's (VK_EXT_descriptor_heap's maxPushDataSize), and the indirect pipelines are then
		// unavailable, as above.
		constexpr std::uint32_t kPushBytes = 4 * (kDrawPushWords + kFramePushWords);

		// A draw's push data (kDrawPushWords).
		rhi::PushConstantRangeDesc drawPush{};
		drawPush.visibility = rhi::ShaderStage::AllGraphics;
		drawPush.num32BitValues = kDrawPushWords;
		drawPush.set = 0;
		drawPush.binding = kRecordAddressBinding;
		rhi::PushConstantRangeDesc pushConstants[2] = { drawPush, {} };
		pushConstants[1].visibility = rhi::ShaderStage::AllGraphics;
		pushConstants[1].num32BitValues = kFramePushWords;
		pushConstants[1].set = 0;
		pushConstants[1].binding = kFramePushBinding;

		const auto mainRanges = MainLayoutRanges(false);
		const auto zRanges = MainLayoutRanges(true);
		// No input assembler: every vertex stage pulls its attributes (DCLF_PULLED_VERTICES).
		const rhi::PipelineLayoutDesc desc{ .ranges = rhi::Span<rhi::LayoutBindingRange>{ mainRanges.data(), static_cast<std::uint32_t>(mainRanges.size()) },
			.pushConstants = { pushConstants, 2u }, .staticSamplers = {}, .flags = rhi::PF_None };
		logger::info("[DCLF] main layout: {} ranges, {} frame push words", mainRanges.size(), kFramePushWords);
		if (device.CreatePipelineLayout(desc, layout) != rhi::Result::Ok) {
			supported = false;
			logger::error("[DCLF] Indirect pipelines unavailable: could not create the indirect draw pipeline layout ({} push data bytes)", kPushBytes);
			return false;
		}
		// The Z-prepass's plain draws: the main ranges minus the rows'.
		const rhi::PipelineLayoutDesc zDesc{ .ranges = rhi::Span<rhi::LayoutBindingRange>{ zRanges.data(), static_cast<std::uint32_t>(zRanges.size()) },
			.pushConstants = { pushConstants, 2u }, .staticSamplers = {}, .flags = rhi::PF_None };
		if (device.CreatePipelineLayout(zDesc, zLayout) != rhi::Result::Ok) {
			supported = false;
			logger::error("[DCLF] Indirect pipelines unavailable: could not create the Z-prepass's pipeline layout ({} push data bytes)", kPushBytes);
			return false;
		}
		// The shadow views (kShadowPushWords): the draw's words name its material row (pipeline row words unused), the view's the rest.
		rhi::PushConstantRangeDesc shadowPush[2] = { drawPush, pushConstants[1] };
		shadowPush[1].num32BitValues = kShadowPushWords;
		const auto shadowRanges = ShadowLayoutRanges();
		const rhi::PipelineLayoutDesc shadowDesc{ .ranges = rhi::Span<rhi::LayoutBindingRange>{ shadowRanges.data(), static_cast<std::uint32_t>(shadowRanges.size()) },
			.pushConstants = { shadowPush, 2u }, .staticSamplers = {}, .flags = rhi::PF_None };
		if (device.CreatePipelineLayout(shadowDesc, shadowLayout) != rhi::Result::Ok) {
			supported = false;
			logger::error("[DCLF] Indirect pipelines unavailable: could not create the shadow views' pipeline layout ({} push data bytes)", 4 * (kDrawPushWords + kShadowPushWords));
			return false;
		}
		return CreateDrawSignatures();
	}

	bool DrawPipelines::Impl::CreateDrawSignatures()
	{
		rhi::IndirectArg draw{};
		draw.kind = rhi::IndirectArgKind::DrawIndexed;
		rhi::CommandSignatureDesc drawDesc{};
		drawDesc.args = { &draw, 1 };
		drawDesc.byteStride = sizeof(DrawSequence);
		if (device.CreateCommandSignature(drawDesc, ShadowLayout(), shadowDrawSignature) != rhi::Result::Ok) {
			supported = false;
			logger::error("[DCLF] Could not create the shadow views' draw signature");
			return false;
		}
		if (device.CreateCommandSignature(drawDesc, zLayout->GetHandle(), zDrawSignature) != rhi::Result::Ok) {
			supported = false;
			logger::error("[DCLF] Could not create the Z-prepass's draw signature");
			return false;
		}
		// Every draw family slot's signature, whether a feature takes it or not (this may run before InstallFeatures resolves them).
		rhi::IndirectArg familyDraw{};
		familyDraw.kind = rhi::IndirectArgKind::Draw;
		rhi::CommandSignatureDesc familyDesc{};
		familyDesc.args = { &familyDraw, 1 };
		familyDesc.byteStride = 4 * sizeof(std::uint32_t);
		for (std::size_t f = 0; f < kDrawFamilyLanes; ++f)
			if (device.CreateCommandSignature(familyDesc, zLayout->GetHandle(), familySignatures[f]) != rhi::Result::Ok) {
				const auto& family = Extensions::DrawFamilies()[f];
				logger::warn("[DCLF] Could not create {}'s draw signature; {}", family.taken ? std::string_view(family.label) : std::string_view("a draw family"),
					family.taken ? std::string_view(family.nativeNote) : std::string_view("it stays native"));
			}
		return true;
	}

	DrawPipelines::DrawPipelines() :
		impl(std::make_unique<Impl>())
	{
		// Named once this is whole (a pass that runs meanwhile waits for Get's construction).
		SetPipelineLaneDrain(&Impl::DrainLaneThunk);
	}

	DrawPipelines::~DrawPipelines() = default;

	DrawPipelines& DrawPipelines::Get()
	{
		static DrawPipelines pipelines;
		return pipelines;
	}

	bool DrawPipelines::Enabled() const
	{
		return impl->Initialize() && impl->supported;
	}

	void DrawPipelines::SetTargetFormats(const TargetFormats& a_formats)
	{
		// A draw with nothing bound (seen while the game shuts down) is not the main pass.
		if (a_formats == targets || a_formats.colorCount == 0 || a_formats.depth == DXGI_FORMAT_UNKNOWN)
			return;
		if (HasTargetFormats()) {
			auto describe = [](const TargetFormats& a_formats) {
				std::string text;
				for (std::uint32_t i = 0; i < a_formats.colorCount; ++i)
					text += fmt::format("{} ", static_cast<int>(a_formats.colors[i]));
				return text + fmt::format("depth {}", static_cast<int>(a_formats.depth));
			};
			// The lane rebuilds (RecreateMainSet); until the frame's catalog catches up, GetIndirectState and CatalogFamilyPipelines give none.
			logger::info("[DCLF] Main pass targets changed ({} -> {}); the indirect pipelines and tree LOD's are built again", describe(targets), describe(a_formats));
			++stats.targetChanges;
			++generation;
		}
		targets = a_formats;
		if (Enabled())
			impl->PostInput(*this);
	}

	std::shared_ptr<const PipelineCatalog> DrawPipelines::TakeCatalog()
	{
		return std::shared_ptr<const PipelineCatalog>(impl->catalogSlot.Take());
	}

	DrawPipelines::Stats DrawPipelines::GetStats() const
	{
		Stats result = stats;
		const auto& counters = impl->lane.counters;
		result.requested = counters.requested.load(std::memory_order_relaxed);
		result.ready = counters.ready.load(std::memory_order_relaxed);
		result.failed = counters.failed.load(std::memory_order_relaxed);
		result.zPipelines = counters.zPipelines.load(std::memory_order_relaxed);
		result.zDepthOnly = counters.zDepthOnly.load(std::memory_order_relaxed);
		result.stateClasses = impl->lane.stateClasses.load(std::memory_order_relaxed);
		result.largestClass = impl->lane.largestClass.load(std::memory_order_relaxed);
		result.classParityChecks = impl->lane.classParityChecks.load(std::memory_order_relaxed);
		result.classParityMismatches = impl->lane.classParityMismatches.load(std::memory_order_relaxed);
		result.setPublishes = counters.setPublishes.load(std::memory_order_relaxed);
		result.setWaits = counters.setWaits.load(std::memory_order_relaxed);
		for (std::size_t f = 0; f < Extensions::kProgramFamilies; ++f) {
			const auto& programCounters = impl->lane.programFamilyCounters[f];
			result.families[f] = { programCounters.requested.load(std::memory_order_relaxed), programCounters.ready.load(std::memory_order_relaxed),
				programCounters.failed.load(std::memory_order_relaxed) };
		}
		const auto& shadowCounters = impl->lane.shadow.counters;
		result.shadowRequested = shadowCounters.requested.load(std::memory_order_relaxed);
		result.shadowReady = shadowCounters.ready.load(std::memory_order_relaxed);
		result.shadowFailed = shadowCounters.failed.load(std::memory_order_relaxed);
		result.shadowSetPublishes = shadowCounters.setPublishes.load(std::memory_order_relaxed);
		result.shadowSetWaits = shadowCounters.setWaits.load(std::memory_order_relaxed);
		for (std::size_t f = 0; f < kDrawFamilyLanes; ++f) {
			const auto& family = Extensions::DrawFamilies()[f];
			const auto& familyCounters = impl->lane.families[f].counters;
			auto& out = result.drawFamilies[f];
			out.label = family.taken ? family.label.c_str() : nullptr;
			out.requested = familyCounters.requested.load(std::memory_order_relaxed);
			out.ready = familyCounters.ready.load(std::memory_order_relaxed);
			out.failed = familyCounters.failed.load(std::memory_order_relaxed);
		}
		const auto& forwardCounters = impl->lane.forward.counters;
		result.forwardRequested = forwardCounters.requested.load(std::memory_order_relaxed);
		result.forwardReady = forwardCounters.ready.load(std::memory_order_relaxed);
		result.forwardFailed = forwardCounters.failed.load(std::memory_order_relaxed);
		return result;
	}

	std::span<const std::string_view> DrawPipelines::LightingVariableNames(bool a_pixel)
	{
		return LightingNames(a_pixel);
	}

	void DrawPipelines::Impl::DrainLaneThunk()
	{
		DrawPipelines::Get().impl->DrainLane();
	}

	void DrawPipelines::Impl::DrainLane()
	{
		ZoneScopedN("CS.DCLF.PipelineLane");
		auto& l = lane;
		auto& s = l.shadow;
		auto& f = l.forward;
		// New inputs: new targets restart the main set and the draw families', a new shadow format the shadow set, new forward targets the
		// forward pipelines; anything new may unpark keys waiting on an input.
		bool inputsChanged = false;
		if (auto input = inputSlot.Take()) {
			if (l.input && l.input->targets.colorCount != 0 && !(l.input->targets == input->targets))
				RecreateMainSet(l.input->targets, input->targets);
			if (l.input && l.input->shadowFormat != DXGI_FORMAT_UNKNOWN && input->shadowFormat != l.input->shadowFormat)
				RecreateShadowSet(l.input->shadowFormat, input->shadowFormat);
			// A view's new targets rebuild every forward pipeline (the first view in slot order whose targets changed names them).
			for (std::size_t v = 0; l.input && v < Extensions::kEngineViews; ++v)
				if (l.input->viewTargets[v].colour != DXGI_FORMAT_UNKNOWN && !(l.input->viewTargets[v] == input->viewTargets[v])) {
					RecreateForward(l.input->viewTargets[v], input->viewTargets[v]);
					break;
				}
			if (l.input && (l.input->refractionFormat != input->refractionFormat || l.input->refractionWriteMask != input->refractionWriteMask))
				l.refraction.Reset();
			l.input = std::shared_ptr<const FrameInput>(std::move(input));
			inputsChanged = true;
			l.dirty = true;  // the catalog names the targets' generation, the shadow format and the forward targets
		}
		// Each key once (the lane keeps them and rebuilds them after a target or format change).
		completions->requests.Drain([&](LightingRequest&& a_request) {
			if (l.asked.try_emplace(a_request.key, a_request.slot).second)
				TryRequest(a_request.key);
		});
		completions->shadowRequests.Drain([&](ShadowRequest&& a_request) {
			if (s.asked.try_emplace(a_request.viewKey, a_request).second)
				TryRequestShadow(a_request.viewKey);
		});
		completions->forwardRequests.Drain([&](ForwardPipelineKey&& a_key) {
			if (f.asked.insert(a_key).second)
				TryRequestForward(a_key);
		});
		TakeFamilyRequests();
		if (completions->refractionRequest.exchange(false, std::memory_order_acq_rel))
			l.refraction.asked = true;
		// A shader reload began reading the files (dclf-hot-reload.md): the programs are made again as asked for, so the keys parked on one,
		// and those that failed, are requested again (the pipelines built already are the registry's to rebuild).
		if (ShaderPrograms::Get().FollowSources()) {
			RetryAfterSourcesMoved();
			inputsChanged = true;
		}
		if (inputsChanged) {
			l.RetryBlocked([&](const auto& a_key) { TryRequest(a_key); });
			s.RetryBlocked([&](const auto& a_key) { TryRequestShadow(a_key); });
			f.RetryBlocked([&](const auto& a_key) { TryRequestForward(a_key); });
		}
		// The draw families and refraction wait on nothing a list holds: tried every pass (cheap once requested, built or failed).
		TryRequestFamilies();
		TryRequestRefraction();
		// Programs finished since the last pass: their keys go on to their pipelines. Loop until none: a program found complete at once
		// (every stage cached) finishes with no completion to wake the lane.
		auto& finished = l.finishedPrograms;
		for (;;) {
			finished.Clear();
			ShaderPrograms::Get().UpdateLane(finished);
			if (finished.Empty())
				break;
			// A family program's id is distinct from every Lighting one (FamilyProgramId); its keys wait in the same map.
			for (const auto& ids : finished.families)
				finished.lighting.insert(finished.lighting.end(), ids.begin(), ids.end());
			for (const std::uint64_t id : finished.lighting)
				l.RetryProgram(id, [&](const auto& a_key) { TryRequest(a_key); });
			for (const std::uint32_t technique : finished.shadow)
				s.RetryProgram(technique, [&](const auto& a_key) { TryRequestShadow(a_key); });
			for (const std::uint64_t id : finished.forward)
				f.RetryProgram(id, [&](const auto& a_key) { TryRequestForward(a_key); });
			RetryFamilyPrograms(finished);
			if (!finished.shadow.empty())
				TryRequestRefraction();
		}
		// What a shader reload swapped into the registry's slots, before this pass's admissions publish the sets.
		AdoptLivePipelines();
		AdmitLane();
		AdmitShadow();
		AdmitFamiliesAndForward();
		// Completed builds become the service's active pipelines (a later request completes at once). The ones they displace retire at once:
		// every payload a recording may use is held by the lane's own sets and the catalogs (Versions, the retired lists), not the service.
		service.PublishReady(++l.servicePass);
		service.Retire(l.servicePass);
		if (l.dirty)
			PublishCatalog();
	}

	void DrawPipelines::Impl::RetryAfterSourcesMoved()
	{
		auto& l = lane;
		// A keyed family's keys parked on a program (its entry is gone), and its failed keys (a program or a build that failed may build from
		// the files as they are now): blocked, so RetryBlocked requests each again.
		const auto unpark = [](auto& a_family) {
			std::size_t retried = 0;
			for (auto& [id, keys] : std::exchange(a_family.programWaiters, {})) {
				a_family.blocked.insert(a_family.blocked.end(), keys.begin(), keys.end());
				retried += keys.size();
			}
			std::vector<std::remove_cvref_t<decltype(a_family.entries.begin()->first)>> failed;
			for (const auto& [key, entry] : a_family.entries)
				if (entry.failed)
					failed.push_back(key);
			for (const auto& key : failed)
				a_family.entries.erase(key);
			a_family.failedEntries = 0;
			a_family.blocked.insert(a_family.blocked.end(), failed.begin(), failed.end());
			return retried + failed.size();
		};
		const std::size_t main = unpark(l), shadow = unpark(l.shadow), forward = unpark(l.forward);
		// The draw families and refraction are tried every pass: what failed for good is forgotten.
		for (auto& t : l.families)
			if (t.failed && !t.built)
				t.failed = t.programFailed = t.requested = false;
		if (l.refraction.failed && !l.refraction.built)
			l.refraction.failed = l.refraction.programFailed = l.refraction.requested = false;
		if (main || shadow || forward)
			logger::info("[DCLF] pipeline lane: the shader sources moved; {} main, {} shadow and {} forward keys waiting on a program or failed are requested again",
				main, shadow, forward);
		l.dirty = true;
	}

	void DrawPipelines::Impl::AdoptLivePipelines()
	{
		auto& l = lane;
		const auto* registry = RenderGraphRuntime::PipelineRegistry();
		const std::uint64_t registryGeneration = registry ? registry->Generation() : 0;
		if (registryGeneration == l.registryGeneration)
			return;
		l.registryGeneration = registryGeneration;
		const auto swapped = [](const std::shared_ptr<org::services::LivePipelineSlot>& a_slot, const org::services::PipelinePayload& a_payload) {
			return a_slot && a_slot->Peek() != a_payload.get();
		};
		// The main set: each index's payload, usage and constant tables (the catalog's entries, which the scene lane resolves again), then
		// its Z-prepass groups and state classes taken again (a new pipeline may defer depth now, or share another's depth pipeline).
		std::uint32_t main = 0, shadow = 0, others = 0;
		for (std::size_t i = 0; i < l.setSlots.size(); ++i) {
			if (!swapped(l.setSlots[i], l.setPipelines[i]))
				continue;
			l.setPipelines[i] = l.setSlots[i]->Load();
			const auto* built = static_cast<const Built*>(l.setPipelines[i].get());
			l.usage[i] = built->usage;
			l.constantTables[i] = built->tables;
			++main;
		}
		if (main) {
			ReindexMainSet();
			// Every version starts over: the next publication (AdmitLane) makes one from every index's pipeline as it is now, never writing
			// an index a version held (the versions a recording holds keep their pipelines until it lets go).
			l.versions.Retire();
		}
		auto& s = l.shadow;
		for (std::size_t i = 0; i < s.setSlots.size(); ++i) {
			if (!swapped(s.setSlots[i], s.setPipelines[i]))
				continue;
			s.setPipelines[i] = s.setSlots[i]->Load();
			s.discards[i] = static_cast<const Built*>(s.setPipelines[i].get())->discards ? 1 : 0;
			++shadow;
		}
		if (shadow)
			s.versions.Retire();
		// The pipelines in no set: the catalog names them; the ones replaced are kept for the process, as a target change's.
		const auto adopt = [&](std::shared_ptr<org::services::LivePipelineSlot>& a_slot, org::services::PipelinePayload& a_built,
							   std::vector<org::services::PipelinePayload>& a_retired) {
			if (!swapped(a_slot, a_built))
				return;
			a_retired.push_back(std::move(a_built));
			a_built = a_slot->Load();
			++others;
		};
		for (auto& [key, entry] : l.forward.entries)
			adopt(entry.slot, entry.built, l.forward.retired);
		for (auto& t : l.families)
			adopt(t.slot, t.built, t.retired);
		adopt(l.refraction.slot, l.refraction.built, l.refraction.retired);
		if (!main && !shadow && !others)
			return;
		l.dirty = true;
		logger::info("[DCLF] pipeline lane: registry generation {}: {} main, {} shadow and {} forward, draw family and refraction pipelines swapped in; the "
					 "catalog is published again with them",
			registryGeneration, main, shadow, others);
	}

	void DrawPipelines::Impl::PublishCatalog()
	{
		auto& l = lane;
		auto catalog = std::make_unique<PipelineCatalog>();
		catalog->entries.reserve(l.entries.size());
		for (const auto& [key, entry] : l.entries) {
			PipelineCatalog::Entry out;
			out.failed = entry.failed;
			if (entry.index != kNotReady) {
				out.setIndex = entry.index;
				out.usage = l.usage[entry.index];
				out.tables = l.constantTables[entry.index];
			}
			catalog->entries.emplace(key, out);
		}
		catalog->generation = l.generation;
		catalog->targetsGeneration = l.input ? l.input->targetsGeneration : 0;
		catalog->revision = ++l.revision;
		// The version the indices were handed out of, or a later one (published only grows within a generation).
		catalog->setVersion = l.versions.published.load();
		const auto& s = l.shadow;
		catalog->shadowEntries.reserve(s.entries.size());
		for (const auto& [key, entry] : s.entries) {
			PipelineCatalog::ShadowEntry out;
			out.failed = entry.failed;
			out.setIndex = entry.index != kNotReady ? entry.index : PipelineCatalog::kNone;
			catalog->shadowEntries.emplace(key, out);
		}
		catalog->shadowGeneration = s.generation;
		catalog->shadowFormat = l.input ? l.input->shadowFormat : DXGI_FORMAT_UNKNOWN;
		catalog->shadowSetVersion = s.versions.published.load();
		for (std::size_t d = 0; d < kDrawFamilyLanes; ++d)
			catalog->families[d] = { l.families[d].built, l.families[d].failed };
		catalog->refraction.pipeline = l.refraction.built;
		catalog->refraction.format = l.refraction.built ? l.refraction.format : DXGI_FORMAT_UNKNOWN;
		catalog->refraction.failed = l.refraction.failed;
		const auto& f = l.forward;
		catalog->forwardEntries.reserve(f.entries.size());
		for (const auto& [key, entry] : f.entries) {
			PipelineCatalog::ForwardEntry out;
			out.pipeline = entry.built;
			out.program = entry.requested;
			out.failed = entry.failed;
			catalog->forwardEntries.emplace(key, out);
		}
		catalog->viewTargets = l.input ? l.input->viewTargets : decltype(catalog->viewTargets){};
		catalogSlot.Post(std::move(catalog));
		// The scene lane takes it at its next pass (ResolveLookups).
		WakeScenePass(SceneWake::Catalog);
		l.dirty = false;
	}

	void DrawPipelines::CaptureEngineStates()
	{
		if (!Enabled())
			return;
		// State bits the lane asked for: read here (the deferred pass), or kept for a later one.
		impl->completions->stateRequests.Drain([&](std::uint32_t&& a_bits) { impl->pendingStateBits.push_back(a_bits); });
		bool changed = false;
		auto& input = impl->renderInput;
		if (!input.frontCCW)
			if (const auto frontCCW = impl->EngineFrontCCW()) {
				input.frontCCW = frontCCW;
				changed = true;
			}
		if (auto* lighting = EngineShaders::Lighting(); lighting != input.lighting) {
			input.lighting = lighting;
			changed = true;
		}
		// The program families' shaders (their programSource: the Effect port's BSEffectShader).
		for (std::size_t f = 0; f < Extensions::kProgramFamilies; ++f) {
			const auto& family = Extensions::ProgramFamilies()[f];
			if (const void* source = family.taken ? family.contribution.programSource() : nullptr; source != input.familySource[f]) {
				input.familySource[f] = source;
				changed = true;
			}
		}
		// The deferred blend states exist from the first deferred pass; read nothing before, so no state is recorded unreadable for good.
		if (DeferredBlendState(0, 0, 10, 0)) {
			auto read = [&](std::uint32_t a_bits, bool a_opaque) {
				if (impl->engineStates.contains(a_bits))
					return;
				std::string error;
				auto state = impl->ReadEngineState(a_bits, error);
				if (a_opaque) {
					const std::uint32_t writeMode = RasterWriteMode(a_bits);
					if (!state.valid && impl->loggedStateFailures++ < kMaxLoggedFailures)
						logger::warn("[DCLF] opaque write mode {} state cannot be read: {}; its objects stay native", writeMode, error);
					else if (state.valid)
						logger::info("[DCLF] opaque write mode {}: rt0 mask {:X}, rt1 mask {:X}", writeMode, static_cast<unsigned>(state.blend.attachments[0].writeMask),
							static_cast<unsigned>(state.blend.attachments[1].writeMask));
				} else if (!state.valid && impl->loggedStateFailures++ < kMaxLoggedFailures) {
					logger::warn("[DCLF] pipeline state {:05X} cannot be built: {}; its objects stay native", a_bits, error);
				} else if (state.valid) {
					logger::info("[DCLF] pipeline state {:05X}: depth bias {} (clamp {}, slope {}), blend rt0 {} mask {:X}, rt1 {} mask {:X}",
						a_bits, state.depthBias, state.depthBiasClamp, state.slopeScaledDepthBias, state.blend.attachments[0].enable ? "on" : "off",
						static_cast<unsigned>(state.blend.attachments[0].writeMask), state.blend.attachments[1].enable ? "on" : "off",
						static_cast<unsigned>(state.blend.attachments[1].writeMask));
				}
				impl->engineStates.emplace(a_bits, state);
				changed = true;
			};
			// The opaque write modes (opaque Lighting keys and tree LOD), unasked; then what the lane asked for.
			for (const std::uint32_t writeMode : { Impl::kOpaqueWriteMode, Impl::kAlphaTestedWriteMode })
				read(writeMode << kRasterWriteModeShift, true);
			for (const std::uint32_t bits : std::exchange(impl->pendingStateBits, {}))
				read(bits, false);  // the opaque modes are read above
		}
		if (changed) {
			input.states = std::make_shared<const decltype(impl->engineStates)>(impl->engineStates);
			impl->PostInput(*this);
		}
	}
}
