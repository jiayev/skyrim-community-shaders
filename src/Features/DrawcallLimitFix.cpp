#include "DrawcallLimitFix.h"

#include "Deferred.h"
#include "DrawcallLimitFix/AsyncWorker.h"
#include "DrawcallLimitFix/CaptureParity.h"
#include "DrawcallLimitFix/ConstantEvaluator.h"
#include "DrawcallLimitFix/MaterialSources.h"
#include "DrawcallLimitFix/OpenDefectProbes.h"
#include "DrawcallLimitFix/TestHarness.h"
#include "DrawcallLimitFix/DrawPipelines.h"
#include "DrawcallLimitFix/FaceSnapshots.h"
#include "DrawcallLimitFix/SunAccumulation.h"
#include "DrawcallLimitFix/PrimaryCull.h"
#include "Features/Skylighting.h"

#include <filesystem>
#include <fstream>
#include "DrawcallLimitFix/GpuResources.h"
#include "DrawcallLimitFix/GpuTextures.h"
#include "DrawcallLimitFix/IndirectDraws.h"
#include "DrawcallLimitFix/ShaderPrograms.h"
#include "DrawcallLimitFix/ShadowViews.h"
#include "DrawcallLimitFix/Toggles.h"
#include "DrawcallLimitFix/PassCapture.h"
#include "DrawcallLimitFix/SceneStore.h"
#include "DrawcallLimitFix/SceneTracker.h"
#include "DrawcallLimitFix/Switches.h"
#include "RenderGraph/RenderGraphRuntime.h"
#include "ShaderCache.h"
#include "State.h"
#include "TerrainBlending.h"
#include "VolumetricShadows.h"

namespace
{
	DCLF::TargetFormats CurrentTargetFormats()
	{
		DCLF::TargetFormats formats;
		ID3D11RenderTargetView* views[8] = {};
		ID3D11DepthStencilView* depth = nullptr;
		globals::d3d::context->OMGetRenderTargets(8, views, &depth);
		for (std::uint32_t i = 0; i < 8; ++i) {
			if (!views[i])
				continue;
			D3D11_RENDER_TARGET_VIEW_DESC desc;
			views[i]->GetDesc(&desc);
			formats.colors[i] = desc.Format;
			formats.colorCount = i + 1;
			views[i]->Release();
		}
		if (depth) {
			D3D11_DEPTH_STENCIL_VIEW_DESC desc;
			depth->GetDesc(&desc);
			formats.depth = desc.Format;
			depth->Release();
		}
		return formats;
	}

	double MillisecondsSince(std::chrono::steady_clock::time_point a_start)
	{
		return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a_start).count();
	}
}

void DrawcallLimitFix::PostPostLoad()
{
	// Every engine hook and offset DCLF uses is AE's (1.6.1170).
	if (!REL::Module::IsAE()) {
		unavailableReason = "DCLF supports Skyrim AE only";
		logger::info("[DCLF] {}; the game renders natively", unavailableReason);
		return;
	}
	// CS_DCLF=0 is the off switch every gate in this work compares against, and until now nothing read it:
	// the switch appeared in the summary line below and nowhere else, so runs labelled "CS_DCLF=0 baseline"
	// had the feature fully installed and were not baselines at all. Unset still means on, which is the
	// behaviour everything else here was built against.
	if (DCLF::SwitchValue(DCLF::Switch::Dclf) == "0") {
		logger::info("[DCLF] CS_DCLF=0; the feature stays off and the game renders natively");
		return;
	}
	DCLF::SceneTracker::Get().Install();
	DCLF::SceneStore::InstallSceneEvents();
	// Capture at registration, the foundation for static ownership: withholding a pass from the batch
	// renderer removes the very data the tables are built from today, so the capture has to prove itself
	// first (it claims nothing and withholds nothing yet).
	DCLF::PassCapture::Get().Install();
	DCLF::MaterialSources::Install();
	DCLF::FaceSnapshots::Get().Install();
	DCLF::SunAccumulation::Get().Install();
	DCLF::PrimaryCull::Get().Install();
	Hooks::Install();
	installed = true;
	// The switches this process actually sees, once. Several reports below are gated on them, so without
	// this a silent log is indistinguishable from a switch that never reached the game - which is exactly
	// what happened when they came from the environment alone (see Switches.h).
	logger::info("[DCLF] switches: {}", DCLF::SwitchSummary());
	if (const auto reduced = DCLF::ReducedFeatures(); reduced.empty())
		logger::info("[DCLF] featureset: full (every feature on; the menu can still turn some off live)");
	else
		logger::warn("[DCLF] featureset: REDUCED by {}; this run does not exercise DCLF's full featureset", reduced);
}

void DrawcallLimitFix::SetupResources()
{
	if (installed && RenderGraphRuntime::Get().IsActive() && RenderGraphRuntime::EpochsEnabled())
		DCLF::ShaderPrograms::Get().StartPrecompile();
	// Runs right after the render graph tried to adopt DXVK's device (State::SetupResources). The hooks went in
	// at PostPostLoad, before any device existed; without the graph there is nothing to draw with, and left
	// on, DCLF would track, classify and skip for nothing while the frame silently stays native. So it is
	// forced off here, with the reason in the log and in the menu instead of only a line of zeroes in the stats.
	// DCLF's passes run in epochs of their own (CS_ORG_EPOCHS, which Light Limit Fix can do without).
	if (!installed || (RenderGraphRuntime::Get().IsActive() && RenderGraphRuntime::EpochsEnabled()))
		return;
	unavailableReason = RenderGraphRuntime::Get().IsActive() ? std::string("the render graph runs without epochs (CS_ORG_EPOCHS=0)") :
	                                                           RenderGraphRuntime::Get().GetDisabledReason();
	installed = false;
	DCLF::SceneTracker::Get().Stop();
	DCLF::PassCapture::Get().SetBypassed(true);
	logger::warn("[DCLF] Forced off: the render graph is unavailable ({}); the game renders natively", unavailableReason);
}

void DrawcallLimitFix::Reset()
{
	// The test commands run before the installed check, so a CS_DCLF=0 baseline reaches the same place at
	// the same in-game hour as the run it is compared against.
	DCLF::RunTestHarness(GetShortName());

	// Every Present, in menus too: the tracker's queue holds references to attached subtrees and
	// must not grow while the world is not rendered.
	if (!installed)
		return;
	const auto start = std::chrono::steady_clock::now();
	DCLF::PrimaryCull::Get().EndFrame();
	DCLF::IndirectDraws::Get().EndFrame();
	DCLF::SceneStore::Get().ProcessEvents();
	timing.eventsMs += MillisecondsSince(start);
	// The menu's toggle, between frames. Scene events keep flowing above while off, so the tracked set is
	// current the moment it comes back on.
	UpdateActive();
}

void DrawcallLimitFix::UpdateActive()
{
	if (!installed)
		return;
	if (const bool wanted = loaded && !globals::state->IsFeatureDisabled(GetShortName()); wanted != switchedOn)
		SetActive(wanted);
}

void DrawcallLimitFix::SetActive(bool a_active)
{
	switchedOn = a_active;
	if (!a_active)
		DCLF::IndirectDraws::Get().DrainAsync();
	auto& capture = DCLF::PassCapture::Get();
	// Off: every pass reaches the batch renderers again, and no claim outlives the switch. Back on, the claims
	// are empty until the first frame republishes them, so nothing is withheld that DCLF has not drawn.
	capture.SetBypassed(!a_active);
	capture.ClearFrameClaims();
	capture.PublishClaims(nullptr);
	for (std::uint32_t mode = 0; mode < DCLF::PassCapture::kShadowModes; ++mode)
		capture.PublishShadowClaims(mode, nullptr);
	if (a_active)
		DCLF::SceneStore::Get().InvalidateVerdicts();
	skipCounters = {};
	logger::info("[DCLF] {} from the menu", a_active ? "Switched on" : "Switched off; the game renders natively");
}

std::int64_t DrawcallLimitFix::Hooks::Main_Draw_Early::thunk(void* a_main)
{
	const auto result = func(a_main);
	globals::features::drawcallLimitFix.BeginSceneFrame();
	return result;
}

bool DrawcallLimitFix::BeginSceneFrame()
{
	// Called every frame, loaded or not: the one place an unload (which stops Reset from being called) is
	// noticed.
	UpdateActive();
	if (!Running())
		return false;
	// The scene half of the tables, before the main camera's cull: everything the walk reads is final from
	// Main::Draw on (the world update is done, and the palette update's frame counter moves only at
	// Renderer::End), except what is written between here and BeforeShadowMaps: BSFadeNode::currentFade (the
	// main cull), whose changes reach the walk as fade events; a billboard's rotation (the main cull), which
	// keeps billboards native (SceneStore::FindCategoryNode); and animated texture transforms, which the shadow
	// build reads off the material itself. The accumulator's half follows at EarlyPrepass, once the
	// registration jobs have finished.
	auto& store = DCLF::SceneStore::Get();
	// The frame's toggles, before anything reads them. A change that enters the classification drops the
	// cached verdicts, so the next frame classifies every object under the new switches.
	if (DCLF::Toggles::Get().BeginFrame())
		store.InvalidateVerdicts();
	ScopedPerfEvent event("CS DCLF: scene tables");
	// The scene events of this frame's world update, before the walk reads the tracked set. Present's Reset
	// applies them too, but a cell attached during the update would otherwise be drawn natively for its first
	// frame and join the tables only on the next one (capture parity's "untracked eligible": every such
	// geometry was tracked by its attach event at that frame's Present). Nothing of DCLF's is in flight here,
	// as after Reset: the previous frame's jobs were joined there, and this frame's start below.
	const auto eventsStart = std::chrono::steady_clock::now();
	store.ProcessEvents();
	timing.eventsMs += MillisecondsSince(eventsStart);
	// Registration hooks may run on cull workers. They must all read one
	// ownership selection even when an epoch publishes next frame's claims.
	DCLF::PassCapture::Get().SelectLegacyFrameClaims();
	const auto start = std::chrono::steady_clock::now();
	store.BuildFrame(DCLF::SceneStore::Phase::Scene);
	const double sceneMs = MillisecondsSince(start);
	timing.sceneMs += sceneMs;
	timing.sceneMaxMs = std::max(timing.sceneMaxMs, sceneMs);
	return true;
}

void DrawcallLimitFix::BeforeShadowMaps()
{
	if (!Running())
		return;
	ScopedPerfEvent event("CS DCLF: shadow views");
	const auto start = std::chrono::steady_clock::now();
	// The frame's shadow views, in the order the engine is about to render them. Everything downstream -
	// the capture's attribution, the claims, the epochs - identifies a view by this list.
	DCLF::ShadowViews::Get().Rebuild();
	if (DCLF::ActiveToggles().shadows) {
		DCLF::IndirectDraws::Get().BeginShadowFrame();
		// The shadow epoch's build, on the worker, while the engine draws the shadow maps (CS_DCLF_ASYNC).
		DCLF::IndirectDraws::Get().KickShadowBuild();
	}
	const double sceneMs = MillisecondsSince(start);
	timing.sceneMs += sceneMs;
	timing.sceneMaxMs = std::max(timing.sceneMaxMs, sceneMs);
}

void DrawcallLimitFix::AfterShadowMaps()
{
	RenderGraphRuntime::EpochBodyScope body(RenderGraphRuntime::Segment::ShadowView);
	if (!Running())
		return;
	// The frame's shadow epoch: every view the 0x2A hook captured, drawn in one graph execution.
	DCLF::IndirectDraws::Get().ExecuteShadowFrame();
}

void DrawcallLimitFix::EarlyPrepass()
{
	if (!Running())
		return;

	// The tables are built here, from Main_RenderShadowMaps, rather than in Prepass from StartDeferred.
	// Both DCLF epochs then read the same generation: the Z-prepass runs inside Main_RenderDepth,
	// which is after this and before Prepass, so it used to see the *previous* frame's tables and the
	// per-object visibility verdicts it wrote could not be applied by index in the colour epoch.
	//
	// This is only possible because the accumulator is already complete here - the cull job finishes
	// before the shadow maps (Main::Draw) - and because the tables read the latched accumulator rather
	// than `currentAccumulator`, which is not set this early.
	auto& store = DCLF::SceneStore::Get();
	ScopedPerfEvent event("CS DCLF: accumulator tables and pipelines");
	const auto start = std::chrono::steady_clock::now();
	store.BuildFrame(DCLF::SceneStore::Phase::Accumulate);
	const double buildMs = MillisecondsSince(start);
	timing.buildMs += buildMs;
	timing.buildMaxMs = std::max(timing.buildMaxMs, buildMs);
	++timing.frames;
	// The shadow probe reads this frame's tables (the main pass's kept objects) and the Utility
	// registrations, which are complete now that the thunk has returned.

	// Phase 2: SPIR-V programs and indirect pipelines for the pipelines drawn this frame (built once, asynchronously).
	auto& programs = DCLF::ShaderPrograms::Get();
	auto& pipelines = DCLF::DrawPipelines::Get();
	if (auto* lighting = DCLF::ConstantEvaluator::Get().GetLightingShader(); lighting && programs.Enabled()) {
		ZoneScopedN("CS.DCLF.Accumulate.Pipelines");
		TracyCZoneN(requestZone, "CS.DCLF.Accumulate.RequestLighting", true);
		const auto& tables = store.GetTables();
		for (std::size_t p = 0; p < tables.pipelines.size(); ++p) {
			if (!tables.PipelineUsed(p, store.GetFrame()))
				continue;
			if (const auto* program = programs.Find(tables.pipelines[p], *lighting))
				pipelines.Find(tables.pipelines[p], *program);
		}
		TracyCZoneEnd(requestZone);
		TracyCZoneN(updateZone, "CS.DCLF.Accumulate.PublishPipelines", true);
		programs.Update();
		pipelines.Update();
		TracyCZoneEnd(updateZone);
		TracyCZoneN(lookupZone, "CS.DCLF.Accumulate.PipelineLookups", true);
		// The pipeline lookups an epoch's build reads (Lookups.h): the set index of every pipeline used this
		// frame, its shaders' constant tables and its register usage, after Update has admitted this frame's
		// finished builds. Resolved here, where the GPU is busy with the shadow maps, rather than in the epoch.
		auto& lookups = store.MutableLookups();
		if (lookups.pipelineSetGeneration != pipelines.Generation()) {
			// The set was recreated (a target change): every index a build may hold is stale.
			lookups.pipelineSetGeneration = pipelines.Generation();
			++lookups.generation;
		}
		lookups.pipelines.resize(tables.pipelines.size());
		auto& cache = SIE::ShaderCache::Instance();
		for (std::size_t p = 0; p < tables.pipelines.size(); ++p) {
			auto& entry = lookups.pipelines[p];
			if (!tables.PipelineUsed(p, store.GetFrame())) {
				if (entry.setIndex != DCLF::Lookups::kNone)
					entry.version = lookups.NextVersion();
				entry.setIndex = DCLF::Lookups::kNone;
				continue;
			}
			const auto& key = tables.pipelines[p];
			const auto* program = programs.Find(key, *lighting);
			const std::uint32_t setIndex = program ? pipelines.Find(key, *program) : DCLF::DrawPipelines::kNotReady;
			auto* vs = cache.GetVertexShader(*lighting, key.vertexDescriptor);
			auto* ps = cache.GetPixelShader(*lighting, key.pixelDescriptor);
			const std::uint32_t resolved = (setIndex != DCLF::DrawPipelines::kNotReady && vs && ps) ? setIndex : DCLF::Lookups::kNone;
			// Written only where it differs, so an entry's version is new only when it changed.
			bool changed = false;
			if (!(entry.key == key)) {
				entry.key = key;
				entry.setIndex = DCLF::Lookups::kNone;
				entry.shadowMaskIndex = DCLF::Lookups::kNone;
				changed = true;
			}
			if (entry.setIndex != DCLF::Lookups::kNone && entry.setIndex != resolved)
				++lookups.generation;  // a build may hold the old index
			changed |= entry.setIndex != resolved;
			entry.setIndex = resolved;
			if (resolved != DCLF::Lookups::kNone) {
				auto assign = [&](std::vector<std::uint8_t>& a_table, const auto& a_source) {
					if (!std::equal(a_table.begin(), a_table.end(), a_source.begin(), a_source.end())) {
						a_table.assign(a_source.begin(), a_source.end());
						changed = true;
					}
				};
				assign(entry.vsTable, vs->constantTable);
				assign(entry.psTable, ps->constantTable);
				for (std::uint32_t variant = 0; variant < 2; ++variant) {
					const auto& usage = pipelines.Usage(resolved, variant);
					auto& bits = entry.usage[variant];
					changed |= bits.vertexConstants != usage.vertexConstants || bits.pixelConstants != usage.pixelConstants || bits.textures != usage.textures ||
					           bits.samplers != usage.samplers;
					bits.vertexConstants = usage.vertexConstants;
					bits.pixelConstants = usage.pixelConstants;
					bits.textures = usage.textures;
					bits.samplers = usage.samplers;
				}
			}
			if (changed)
				entry.version = lookups.NextVersion();
		}
		TracyCZoneEnd(lookupZone);
	}

	// What the native loop was told to leave to DCLF but DCLF cannot draw this frame goes back to it now,
	// before the depth and main passes: an object DCLF has no bindings for, or whose pipeline is not built.
	if (DCLF::ActiveToggles().ownership)
		DCLF::PassCapture::Get().HandBackUndrawable(DrawableThisFrame);

	// The shadow views' programs: one Utility build per technique of the frame's casters, per render mode
	// among the views the engine drew. Requested here, beside the Lighting builds, so they are compiled
	// long before a shadow epoch would draw with them.
	if (auto* utility = globals::game::utilityShader; utility && programs.Enabled()) {
		ZoneScopedN("CS.DCLF.Accumulate.ShadowPipelines");
		std::uint32_t modeBits = 0;
		for (const auto& view : DCLF::ShadowViews::Get().All()) {
			// Clamped for cascades and spot lights, the paraboloid warp for point lights; the engine
			// picks the render mode from the light, not from the descriptor (engine notes: shadow maps).
			modeBits |= DCLF::ShadowModeBits(view.kind == DCLF::ShadowViews::Kind::Parabolic ? 0xFu : 0xEu);
		}
		const auto& tables = store.GetTables();
		// The shadow map array the views draw into; its format is what a shadow pipeline is built for.
		DXGI_FORMAT shadowFormat = DXGI_FORMAT_UNKNOWN;
		if (auto* renderer = globals::game::renderer) {
			const auto& depthStencils = renderer->GetDepthStencilData().depthStencils;
			// The depth-stencil view's format, not the texture's: the texture is typeless (R16_TYPELESS)
			// and a pipeline is built against the view it renders through (D16_UNORM), as the epoch does.
			if (auto* view = depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kSHADOWMAPS_ESRAM].views[0]) {
				D3D11_DEPTH_STENCIL_VIEW_DESC desc{};
				view->GetDesc(&desc);
				shadowFormat = desc.Format;
			}
		}
		// Under every rasterizer state the mode's views have drawn with (DrawPipelines::ShadowRasterStateId):
		// the states are only known once a view has been seen, so the first frame's are requested by the epoch.
		for (const auto& key : tables.shadowKeysUsed) {
			for (std::uint32_t mode : { 0xEu, 0xFu }) {
				const std::uint32_t bits = DCLF::ShadowModeBits(mode);
				if (!(modeBits & bits))
					continue;
				const auto* program = programs.FindShadow(key.technique | bits, *utility);
				if (!program)
					continue;
				for (std::uint32_t states = pipelines.ShadowRasterStatesOfMode(mode); states; states &= states - 1) {
					const DCLF::ShadowPipelineKey viewKey{ key.technique | bits, DCLF::WithShadowState(key.rasterFlags, std::countr_zero(states)), key.vertexLayout };
					pipelines.FindShadow(viewKey, *program, shadowFormat);
				}
			}
		}
	}


	// The Z-prepass epoch's build, on the worker, from here to the Z-prepass in Main_RenderDepth (CS_DCLF_ASYNC).
	DCLF::IndirectDraws::Get().KickZPrepassBuild();
}

namespace
{
	/** @brief CS_DCLF_SKYLIGHT_PARITY: the two renders of one map, kept and compared once readable. */
	struct SkyParity
	{
		std::array<winrt::com_ptr<ID3D11Texture2D>, 2> staging;
		D3D11_TEXTURE2D_DESC desc{};
		std::uint32_t framesLeft = 0;
		bool pending = false;
	};
	SkyParity skyParity;

	bool SkyParityEnabled()
	{
		const bool enabled = DCLF::SwitchEnabled(DCLF::Switch::SkylightParity);
		return enabled;
	}

	void CompareSkyParity()
	{
		auto& parity = skyParity;
		if (!parity.pending || --parity.framesLeft)
			return;
		parity.pending = false;
		auto* context = globals::d3d::context;
		std::array<D3D11_MAPPED_SUBRESOURCE, 2> mapped{};
		if (FAILED(context->Map(parity.staging[0].get(), 0, D3D11_MAP_READ, 0, &mapped[0])))
			return;
		if (FAILED(context->Map(parity.staging[1].get(), 0, D3D11_MAP_READ, 0, &mapped[1]))) {
			context->Unmap(parity.staging[0].get(), 0);
			return;
		}
		// Depth as the texture holds it: 16-bit UNORM (the engine's D16 maps), else the top 24 of 32 bits, else a float.
		const auto format = parity.desc.Format;
		const bool unorm16 = format == DXGI_FORMAT_R16_TYPELESS || format == DXGI_FORMAT_D16_UNORM || format == DXGI_FORMAT_R16_UNORM;
		const bool float32 = format == DXGI_FORMAT_R32_TYPELESS || format == DXGI_FORMAT_D32_FLOAT || format == DXGI_FORMAT_R32_FLOAT;
		auto depthAt = [&](std::uint32_t a_image, std::uint32_t x, std::uint32_t y) {
			const auto* row = static_cast<const std::uint8_t*>(mapped[a_image].pData) + std::size_t(y) * mapped[a_image].RowPitch;
			if (unorm16)
				return reinterpret_cast<const std::uint16_t*>(row)[x] / 65535.0;
			const std::uint32_t word = reinterpret_cast<const std::uint32_t*>(row)[x];
			if (float32)
				return static_cast<double>(std::bit_cast<float>(word));
			return (word & 0xFFFFFF) / 16777215.0;
		};
		std::uint64_t texels = 0, equal = 0, dclfFarther = 0, dclfNearer = 0, engineFar = 0, dclfFar = 0, bigDiff = 0;
		double maxDiff = 0.0, sumDiff = 0.0;
		for (std::uint32_t y = 0; y < parity.desc.Height; ++y)
			for (std::uint32_t x = 0; x < parity.desc.Width; ++x) {
				const double engine = depthAt(0, x, y), dclf = depthAt(1, x, y);
				++texels;
				engineFar += engine >= 1.0 ? 1 : 0;
				dclfFar += dclf >= 1.0 ? 1 : 0;
				const double diff = std::abs(engine - dclf);
				if (diff == 0.0) {
					++equal;
					continue;
				}
				(dclf > engine ? dclfFarther : dclfNearer) += 1;
				bigDiff += diff > 1.0 / 256.0 ? 1 : 0;
				sumDiff += diff;
				maxDiff = std::max(maxDiff, diff);
			}
		// A map that differs over more than 5% of its texels: both written out (16-bit PGM, the depth scaled to 0..65535).
		if (bigDiff * 20 > texels) {
			static std::uint32_t dumps = 0;
			if (dumps < 4) {
				const auto directory = std::filesystem::path(DCLF::SwitchValue(DCLF::Switch::SkylightDumpDir).empty() ? "." : DCLF::SwitchValue(DCLF::Switch::SkylightDumpDir));
				for (std::uint32_t image = 0; image < 2; ++image) {
					std::ofstream out(directory / fmt::format("sky-parity-{}-{}.pgm", dumps, image ? "dclf" : "engine"), std::ios::binary);
					out << "P5\n" << parity.desc.Width << " " << parity.desc.Height << "\n65535\n";
					for (std::uint32_t y = 0; y < parity.desc.Height; ++y)
						for (std::uint32_t x = 0; x < parity.desc.Width; ++x) {
							const auto value = static_cast<std::uint16_t>(std::clamp(depthAt(image, x, y), 0.0, 1.0) * 65535.0);
							const char bytes[2] = { static_cast<char>(value >> 8), static_cast<char>(value & 0xFF) };
							out.write(bytes, 2);
						}
				}
				logger::info("[DCLF] Skylighting occlusion parity: map {} written to {}", dumps, directory.string());
				++dumps;
			}
		}
		context->Unmap(parity.staging[0].get(), 0);
		context->Unmap(parity.staging[1].get(), 0);
		logger::info("[DCLF] Skylighting occlusion parity: {}x{} format {}: {} texels equal of {}, DCLF farther {} nearer {} ({} by more than 1/256), max diff {:.6f}, mean diff {:.6f}; clear texels engine {} DCLF {}",
			parity.desc.Width, parity.desc.Height, static_cast<std::uint32_t>(format), equal, texels, dclfFarther, dclfNearer, bigDiff, maxDiff,
			texels - equal ? sumDiff / double(texels - equal) : 0.0, engineFar, dclfFar);
	}
}

bool DrawcallLimitFix::SkyOcclusionReady()
{
	if (!Running() || !DCLF::SceneStore::SkyOcclusionEnabled())
		return false;
	const bool ready = DCLF::IndirectDraws::Get().SkyOcclusionReady();
	skyNativeFrames += ready ? 0 : 1;
	return ready;
}

void DrawcallLimitFix::DrawSkyOcclusion()
{
	DCLF::IndirectDraws::Get().ExecuteSkyOcclusion();
}

bool DrawcallLimitFix::SkyOcclusionParityFrame()
{
	static std::uint32_t frames = 0;
	return SkyParityEnabled() && !skyParity.pending && (frames++ % 120) == 60;
}

void DrawcallLimitFix::CopySkyOcclusion(std::uint32_t a_stage)
{
	auto* texture = globals::features::skylighting.texOcclusion ? globals::features::skylighting.texOcclusion->resource.get() : nullptr;
	if (!texture || a_stage > 1)
		return;
	auto& parity = skyParity;
	if (a_stage == 0) {
		texture->GetDesc(&parity.desc);
		D3D11_TEXTURE2D_DESC desc = parity.desc;
		desc.Usage = D3D11_USAGE_STAGING;
		desc.BindFlags = 0;
		desc.MiscFlags = 0;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		for (auto& staging : parity.staging) {
			staging = nullptr;
			if (FAILED(globals::d3d::device->CreateTexture2D(&desc, nullptr, staging.put())))
				return;
		}
	}
	if (!parity.staging[a_stage])
		return;
	globals::d3d::context->CopyResource(parity.staging[a_stage].get(), texture);
	if (a_stage == 1) {
		parity.pending = true;
		parity.framesLeft = 4;
	}
}

void DrawcallLimitFix::Prepass()
{
	CompareSkyParity();
	DCLF::ProbeShadowMask(Running());
	DCLF::ProbeShadowMaps(Running());
	if (!Running())
		return;

	auto& store = DCLF::SceneStore::Get();
	// The one point in the frame where the main camera's accumulator is identifiable: EarlyPrepass, where
	// the tables are now built, is too early for `currentAccumulator` to be set.
	store.LatchAccumulator();
	// The camera-dependent half of the per-frame constants, now that the main camera's shadow state is
	// current (BuildFrame ran at EarlyPrepass, where it still belonged to the shadow-map camera).
	store.RefreshFrameConstants();
	// The colour epoch's build, on the worker, from here to the epoch (CS_DCLF_ASYNC).
	DCLF::IndirectDraws::Get().KickColourBuild();
	skipStats = skipCounters;
	skipCounters = {};
	if ((store.GetFrame() % kReportInterval) == 1)
		skipSamples.clear();  // collected again for the next report

	const std::uint32_t frame = store.GetFrame();
	if (DCLF::CaptureParity::Enabled())
		DCLF::CaptureParity::Get().Report(frame, kReportInterval);
	DCLF::SunAccumulation::Get().Report(frame, kReportInterval);
	DCLF::PrimaryCull::Get().Report(frame, kReportInterval);

	ReportStats(frame);
}

bool DrawcallLimitFix::DrawableThisFrame(const RE::BSGeometry* a_geometry)
{
	const auto& store = DCLF::SceneStore::Get();
	const auto& tables = store.GetTables();
	const auto& lookups = store.GetLookups();
	const auto object = store.FindObject(a_geometry);
	if (object < 0 || static_cast<std::size_t>(object) >= tables.objects.size())
		return false;
	const auto& record = tables.objects[object];
	if (record.flags & DCLF::kObjectNoBindings)
		return false;
	return record.pipelineIndex < lookups.pipelines.size() && lookups.pipelines[record.pipelineIndex].setIndex != DCLF::Lookups::kNone;
}

bool DrawcallLimitFix::SkipNativePass(RE::BSRenderPass* a_pass)
{
	if (!Running() || !a_pass || !a_pass->geometry)
		return false;
	if (!inDepthPass && !globals::deferred->deferredPass)
		return false;  // shadows, reflections and cubemaps keep drawing everything
	// A decal's passes offered in the depth pass (blended ones with kZBufferWrite) draw nothing there: the Lighting shader
	// never reaches SetupGeometry in the depth pass (engine notes, "Decals"), so they are skipped like any other pass.
	// Never a pass DCLF does not model, whoever draws its object: a LOD cross-fade's copy of the old level
	// (hint 10) is the native loop's while DCLF draws the object's own pass.
	if (DCLF::PassCapture::FadingAtRegistration(a_pass))
		return false;
	auto& store = DCLF::SceneStore::Get();
	if (!DCLF::IndirectDraws::Get().DrewLastFrame(a_pass->geometry, store.GetFrame()))
		return false;
	// A geometry the epoch drew in the frame before but that is not in this frame's tables would be left
	// out of the frame entirely: the pass stays native and the mismatch is reported.
	if (store.FindObject(a_pass->geometry) < 0) {
		++skipCounters.notInTables;
		if (skipCounters.notInTables == 1 && a_pass->geometry->name.c_str())
			logger::warn("[DCLF] skip: '{}' was drawn last frame but is not in this frame's tables; it stays native", a_pass->geometry->name.c_str());
		return false;
	}
	// Nor one DCLF cannot draw this frame although it has a record: the accumulate phase gave it no bindings
	// (it started fading - a tree's LOD cross-fade begins at a fixed distance - or its switch no longer
	// selects it) or its pipeline is not built. The skip set is last frame's draws, so this is the frame it
	// would be drawn by nobody: the flicker trees showed at their LOD distances.
	if (!DrawableThisFrame(a_pass->geometry)) {
		++skipCounters.undrawable;
		return false;
	}
	if (skipSamples.size() < 12 && a_pass->geometry->name.c_str())
		skipSamples.emplace_back(a_pass->geometry->name.c_str());
	return true;
}

void DrawcallLimitFix::Hooks::Main_RenderDepth::thunk(bool a_firstPerson, bool a_a2)
{
	auto& feature = globals::features::drawcallLimitFix;
	feature.inDepthPass = true;
	func(a_firstPerson, a_a2);
	feature.inDepthPass = false;
}

void DrawcallLimitFix::Hooks::Main_RenderDepth_WorldDrawn::thunk(void* a_accumulator, bool a_a2)
{
	func(a_accumulator, a_a2);
	auto& feature = globals::features::drawcallLimitFix;
	if (!feature.inDepthPass || !feature.Running())
		return;
	// The world's depth draws are done and its camera is still current. The engine's own copy of the depth
	// (kPOST_ZPREPASS_COPY), at the end of the pass, now includes DCLF's objects, and the first-person model's
	// depth goes in after the world's, as it does natively, so there is nothing to refresh.
	DCLF::IndirectDraws::Get().CaptureDepthPass();
	if (DCLF::SceneStore::Get().GetTables().objects.size() >= 400)
		globals::BeginDCLFDepthTrace(globals::game::renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN].texture);
	// DCLF's epoch leaves the context's bindings to the engine's state tracking: rebind its targets for the
	// rest of the pass.
	globals::game::stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);
}

template <int N>
void DrawcallLimitFix::Hooks::BSBatchRenderer_RenderPassImmediately<N>::thunk(RE::BSRenderPass* a_pass, std::uint32_t a_technique, bool a_alphaTest, std::uint32_t a_renderFlags)
{
	auto& feature = globals::features::drawcallLimitFix;
	++feature.skipCounters.offered;
	if (feature.SkipNativePass(a_pass)) {
		++(feature.inDepthPass ? feature.skipCounters.skippedInDepth : feature.skipCounters.skippedInOpaque);
		++feature.skipCounters.skipped;
		return;
	}
	func(a_pass, a_technique, a_alphaTest, a_renderFlags);
}

void DrawcallLimitFix::Hooks::BSShaderAccumulator_FinishAccumulating::thunk(RE::BSGraphics::BSShaderAccumulator* a_accumulator, std::uint32_t a_renderFlags)
{
	func(a_accumulator, a_renderFlags);
	if (!globals::features::drawcallLimitFix.Running())
		return;
	const auto mode = static_cast<std::uint32_t>(a_accumulator->GetRuntimeData().renderMode);
	// Skylighting's occlusion map (render mode 0x1C, the precipitation accumulator while Skylighting draws its own map).
	if (mode == 0x1C) {
		auto* sky = globals::game::sky;
		auto* precip = sky ? sky->precip : nullptr;
		if (globals::features::skylighting.inOcclusion && precip &&
			precip->occlusionData.accumulator.get() == reinterpret_cast<RE::BSShaderAccumulator*>(a_accumulator))
			DCLF::IndirectDraws::Get().CaptureSkyOcclusion();
		return;
	}
	if (mode < 0xD || mode > 0xF)
		return;  // shadow-map modes only: plain, clamped, paraboloid (engine notes: shadow maps)
	const auto view = DCLF::ShadowViews::Get().ViewOfAccumulator(a_accumulator);
	if (view != ~0u)
		DCLF::IndirectDraws::Get().ExecuteShadowView(view, mode);
}

void DrawcallLimitFix::Hooks::Install()
{
	// Always installed: the shadow views are a live toggle (Toggles.h), and the thunk costs one compare per
	// accumulator finish when they are off.
	stl::write_vfunc<0x2A, BSShaderAccumulator_FinishAccumulating>(RE::VTABLE_BSShaderAccumulator[0]);
	logger::info("[DCLF] shadow view hook installed on BSShaderAccumulator::FinishAccumulatingPreResolveDepth");
	// The main camera's depth pass, hooked where Terrain Blending hooks it.
	stl::write_thunk_call<Main_RenderDepth>(REL::RelocationID(35560, 36559).address() + Util::VersionedRelocation::Select(0x395, 0x395, 0x3B3));
	stl::write_thunk_call<Main_RenderDepth_WorldDrawn>(REL::RelocationID(100421, 107139).address() + 0x1AA);
	logger::info("[DCLF] Z-prepass hook installed inside Main::RenderDepth, after the world's depth draws");
	stl::write_thunk_call<BSBatchRenderer_RenderPassImmediately<1>>(REL::RelocationID(100877, 107667).address() + REL::Relocate(0x1E5, 0xED));
	stl::write_thunk_call<BSBatchRenderer_RenderPassImmediately<2>>(REL::RelocationID(100852, 107642).address() + REL::Relocate(0x29E, 0x28F));
	stl::write_thunk_call<Main_Draw_Early>(REL::RelocationID(35560, 36559).address() + 0xD3);
	logger::info("[DCLF] scene phase hook installed on Main::Draw");
	logger::info("[DCLF] Native pass hooks installed");
}

void DrawcallLimitFix::OnNativeLightingDraw(RE::BSRenderPass* a_pass, std::uint32_t a_renderFlags)
{
	if (!Running() || DCLF::ConstantEvaluator::Evaluating())
		return;
	if (globals::deferred->deferredPass) {
		auto& store = DCLF::SceneStore::Get();
		// A ProjectedUV draw has SetupGeometry's four projected textures bound now: keep them for the
		// pipelines DCLF builds with that bit (the Hair technique binds none).
		if ((DCLF::PassDescriptorOf(a_pass->passEnum) & 0x8000u) && ((DCLF::PassDescriptorOf(a_pass->passEnum) >> 24) & 0x3f) != 6)
			store.NoteProjectedTextures();
		// CS_DCLF_CAPTURE_POINT_PARITY: the bindings at the frame's first lighting draw the engine makes, against what
		// BeforeOpaquePass captured for the colour epoch.
		const bool captureParity = DCLF::SwitchEnabled(DCLF::Switch::CapturePointParity);
		if (captureParity && captureFrame == store.GetFrame() && parityFrame != store.GetFrame()) {
			parityFrame = store.GetFrame();
			DCLF::IndirectDraws::Get().CheckCapturePoint();
		}
	}
	if (DCLF::CaptureParity::Enabled())
		DCLF::CaptureParity::Get().OnNativeLightingDraw(a_pass, a_renderFlags);
}

void DrawcallLimitFix::BeforeOpaquePass()
{
	if (!Running())
		return;
	// The main pass's bindings for this frame's colour epoch, where the opaque batches start. The engine binds the
	// G-buffer (and clears it) only when a draw applies its state, so the state is applied here as the first draw would
	// apply it. They were taken at the first lighting draw the engine made, which a frame whose every lighting draw in
	// view is DCLF's (an interior, facing away from anything left native) does not have: its colour epoch was dropped,
	// and the objects it withheld from the engine went undrawn. The end of the pass is no substitute: by then the sky's
	// clouds have rebound Cloud Shadows' t26. CS_DCLF_CAPTURE_POINT_PARITY checks this capture against the first
	// lighting draw's.
	static REL::Relocation<void (*)(bool)> SetDirtyStates{ REL::RelocationID(75580, 77386) };
	SetDirtyStates(false);
	globals::EndDCLFDepthTrace();
	if (!CaptureMainPass() && !loggedCaptureFailure) {
		loggedCaptureFailure = true;
		logger::warn("[DCLF] the main pass's targets are not bound where its opaque batches start; the colour epoch is skipped");
	}
}

bool DrawcallLimitFix::CaptureMainPass()
{
	const auto formats = CurrentTargetFormats();
	if (formats.colorCount == 0 || formats.depth == DXGI_FORMAT_UNKNOWN)
		return false;
	auto& store = DCLF::SceneStore::Get();
	captureFrame = store.GetFrame();
	DCLF::DrawPipelines::Get().SetTargetFormats(formats);
	// Phase 2: what the main pass binds, for this frame's indirect draws (run before the composite).
	DCLF::IndirectDraws::Get().CaptureMainPass();
	// The engine's state objects behind any key that carries state bits (decals), read here
	// because this is inside the deferred pass, where the blend table holds the deferred variants.
	DCLF::DrawPipelines::Get().CaptureEngineStates(store.GetTables().pipelines);
	return true;
}

void DrawcallLimitFix::AfterOpaquePass()
{
	if (!Running())
		return;
	// Natively, DCLF's objects are drawn among the opaque batches, so they are in the G-buffer and the depth
	// before Terrain Blending draws its held terrain: with alpha blending onto the G-buffer, a LESS_EQUAL test
	// and depth writes, its alpha the distance to the object beneath. Drawn after that terrain, a buried
	// object was missing from what the terrain blended onto (the G-buffer's clear values showed through,
	// whitish-blue), and the terrain's depth writes then failed DCLF's EQUAL test where it overlapped.
	auto& draws = DCLF::IndirectDraws::Get();
	auto* context = globals::d3d::context;
	context->OMSetRenderTargets(0, nullptr, nullptr);  // the epoch writes the G-buffer the context has bound
	draws.ProbeTargets("before colour");
	draws.ExecuteColour();  // the colour pass, against the depth written at the first draw
	draws.ProbeTargets("after colour");
	// DCLF's epoch leaves the context's bindings to the engine's state tracking: rebind its targets for the
	// rest of the pass.
	globals::game::stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);
}

void DrawcallLimitFix::PublishOwnership()
{
	if (!Running())
		return;
	// Publish what DCLF owns now that the colour epoch has said what it actually drew. The registration
	// hook reads this on the next frame, before BuildFrame - which is the point: a claim is a standing
	// statement of ownership, not a per-frame decision.
	DCLF::IndirectDraws::Get().PublishClaims();
}

void DrawcallLimitFix::DrawSettings()
{
	const auto& stats = DCLF::SceneStore::Get().GetStats();
	if (!unavailableReason.empty()) {
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.55f, 0.3f, 1.0f));
		ImGui::TextUnformatted("Unavailable this session: everything renders natively.");
		ImGui::PopStyleColor();
		ImGui::TextWrapped("Reason: %s", unavailableReason.c_str());
		return;
	}
	if (!installed) {
		ImGui::TextUnformatted("Not installed (CS_DCLF=0).");
		return;
	}
	if (!switchedOn) {
		ImGui::TextWrapped("Switched off with the feature's toggle: the game renders natively and DCLF does no frame work. Switch it back on to resume; no restart needed.");
		return;
	}
	// The live toggles, for A/B comparisons without a restart. They are the REQUESTED set; the render thread applies them at the start of the next frame (Toggles.h).
	auto& toggles = DCLF::Toggles::Get().Requested();
	const auto active = DCLF::ActiveToggles();
	if (ImGui::TreeNodeEx("Live toggles (A/B)", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::TextWrapped("Each is seeded from its CS_DCLF_* switch and applied at the next frame. A change to an object class drops the classification caches, so the frame after it re-classifies everything.");
		for (const auto& toggle : DCLF::ToggleTable()) {
			if (!toggle.section.empty())
				ImGui::SeparatorText(toggle.section.data());
			ImGui::BeginDisabled(!DCLF::Toggles::Get().Editable(toggle));
			ImGui::Checkbox(toggle.label, &(toggles.*toggle.member));
			if (toggle.tooltip)
				if (auto _tt = Util::HoverTooltipWrapper())
					ImGui::TextUnformatted(toggle.tooltip);
			ImGui::EndDisabled();
			if (toggle.member == &DCLF::ToggleSet::ownership) {
				int cull = toggles.cullMode;
				if (ImGui::Combo("GPU culling (CS_DCLF_CULL)", &cull, "off\0frustum\0frustum + occlusion\0"))
					toggles.cullMode = static_cast<std::uint8_t>(cull);
			}
		}
		ImGui::TreePop();
	}
	if (ImGui::TreeNodeEx("This frame", ImGuiTreeNodeFlags_DefaultOpen)) {
		std::string flags = fmt::format("Active: cull {}", active.cullMode);
		for (const auto& toggle : DCLF::ToggleTable())
			flags += fmt::format(", {} {}", toggle.name, active.*toggle.member ? 1 : 0);
		ImGui::TextWrapped("%s", flags.c_str());
		ImGui::Text("Tracked geometry: %u (under %u category nodes)", stats.tracked, stats.categoryNodes);
		ImGui::Text("Objects this frame: %u (%u the engine's culling also kept), geometries: %u, pipelines: %u", stats.objects, stats.nativeVisible, stats.geometries, stats.pipelines);
		for (std::size_t i = 1; i < stats.ineligible.size(); ++i) {
			if (stats.ineligible[i])
				ImGui::Text("Left native (%s): %u", DCLF::kIneligibleNames[i].data(), stats.ineligible[i]);
		}
		const auto& draws = DCLF::IndirectDraws::Get().GetStats();
		ImGui::Text("Indirect draws: %u drawn from %u records, %u epochs; culling tested %u, rejected %u, false negatives %u",
			draws.drawn, draws.records, draws.epochs, draws.cullTested, draws.cullRejected, draws.cullFalseNegatives);
		if (const auto& gpu = RenderGraphRuntime::Get().GpuTimingSummary(); !gpu.empty())
			ImGui::TextUnformatted(("GPU (ORG pass timestamps, last report):\n" + gpu).c_str());
		const auto& capture = DCLF::PassCapture::Get().GetStats();
		if (active.ownership)
			ImGui::Text("Ownership: %u withheld, %u claimed, %u holes", capture.withheld, capture.claimed, capture.holes);
		if (active.shadows) {
			const auto& shadow = DCLF::IndirectDraws::Get().GetShadowStats();
			ImGui::Text("Shadow views (since the last report): %u offered, %u drawn, %u not ready; last view %u inputs, %u records; sampled culling: %u tested, %u rejected",
				shadow.views, shadow.viewsDrawn, shadow.notReady, shadow.inputs, shadow.records, shadow.cullTested, shadow.cullRejected);
			if (active.shadowOwnership)
				ImGui::Text("Shadow ownership: withheld %u / %u / %u (plain / clamped / paraboloid), claimed %u / %u / %u",
					capture.shadowWithheld[0], capture.shadowWithheld[1], capture.shadowWithheld[2], shadow.claimed[0], shadow.claimed[1], shadow.claimed[2]);
		}
		ImGui::TreePop();
	}
}
