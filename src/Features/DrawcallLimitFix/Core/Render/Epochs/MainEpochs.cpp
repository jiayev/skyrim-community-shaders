// The main epochs on the render thread: the Z-prepass's and colour pass's captures at their engine hooks, the forward and
// refraction epoch, each epoch's commit of the installed publication's payload, and the frame blocks taken from the capture.

#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ObjectContributors.h"
#include "Features/DrawcallLimitFix/Scene/Ports/EngineShaders.h"
#include "Features/DrawcallLimitFix/Diagnostics/Draws/Events.h"

namespace DCLF
{
	IndirectDraws::IndirectDraws() :
		impl(std::make_unique<Impl>())
	{}

	IndirectDraws::~IndirectDraws() = default;

	IndirectDraws& IndirectDraws::Get()
	{
		static IndirectDraws draws;
		return draws;
	}

	void IndirectDraws::CaptureMainPass()
	{
		if (impl->pending)
			impl->pending->Release();
		impl->pending = CaptureBindings();
		impl->mainPassDepth = impl->pending->depth;
		impl->mainMinDepth = impl->pending->minDepth;
		impl->mainMaxDepth = impl->pending->maxDepth;
	}

	void IndirectDraws::CaptureDepthPass()
	{
		// Main::RenderDepth sets the world depth target and viewport in the engine's shadow state,
		// but only a native draw flushes that state to D3D11. When ownership withheld all such draws,
		// RSGetViewports can still return the last shadow map's 4096x4096 viewport. Pairing that with
		// the main depth texture below records an out-of-bounds Vulkan render area and can lose the
		// device. Apply the pending world state at this hook, before the first-person camera replaces
		// it, exactly as BeforeOpaquePass does for the colour capture. This also applies pending clears
		// and preserves the engine's dynamic-resolution viewport instead of using the texture's extent.
		Engine::ApplyPendingState();
		auto capture = CaptureBindings();
		// The engine's main depth, not whatever is bound: inside the depth pass Terrain Blending alternates the
		// bound target between it and its own terrain depth while terrain draws.
		if (auto* renderer = globals::game::renderer) {
			if (auto* main = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN].texture) {
				capture.depth = nullptr;
				capture.depth.copy_from(main);
			}
		}
		// The Z-prepass writes into the depth the native pass just finished, which has to be the one the
		// main pass then tests against; otherwise DCLF's objects would be written somewhere nothing reads.
		if (!capture.depth || (impl->mainPassDepth && capture.depth.get() != impl->mainPassDepth.get())) {
			if (!impl->loggedDepthMismatch) {
				impl->loggedDepthMismatch = true;
				logger::warn("[DCLF] The depth pass and the main pass bind different depth textures; the Z-prepass stays with the main pass");
			}
			capture.Release();
			return;
		}
		if (impl->pending)
			impl->pending->Release();
		impl->pending = std::move(capture);
		ProbeTargets("before z-prepass");
		RunEpoch(true);
		ProbeTargets("after z-prepass");
	}

	void IndirectDraws::ProbeTargets(const char* a_label)
	{
		Diagnostics::Notify(Diagnostics::TargetPoint{ a_label });
	}

	void IndirectDraws::ExecuteColour()
	{
		// The colour epoch assembles from the main pass's own capture: the Z-prepass ran off the depth
		// pass's, where the pixel-stage bindings were not available. Both draw the same tables with the
		// same camera, so the depths agree and the colour pass can test EQUAL.
		RunEpoch(false);
	}

	bool IndirectDraws::ExecuteForward()
	{
		ZoneScopedN("CS.DCLF.ExecuteForward");
		if (!impl || failed)
			return false;
		const ToggleSet toggles = ActiveToggles();
		if (!Extensions::ObjectClassOn(toggles, Api::ObjectClass::Forward))
			return false;
		auto& f = impl->forwardEpoch;
		const std::uint32_t frame = Scene::FrameView::Get().GetFrame();
		// Once a frame (the engine's blended draw has two call sites: the camera under water draws it first, else after the water).
		if (f.submittedFrame == frame)
			return false;
		++f.reached;
		// What it draws is what this frame's colour epoch built and sorted: nothing without it (no claims: the engine drew everything,
		// the forward objects' passes included).
		const MainPayload* colour = impl->committedPayload[kAsyncColour];
		if (!colour || impl->committedFrame[kAsyncColour] != frame || Scene::FrameView::Get().SetWithdrawn()) {
			++f.noColour;
			return false;
		}
		// The selected revision's forward recording (DecideCoverage required it for the frame's claims while the toggle is on).
		const auto revision = impl->RevisionOf(Epoch::Forward);
		if (!revision) {
			++f.noRecording;
			return false;
		}
		// Held until this execution retires (its draws read what the colour commit bound): the colour payload's bindings, the frame
		// textures its frame record names, the adopted snapshot whole.
		const std::array<const MainPayload*, 1> payloads{ colour };
		const auto owners = impl->EpochOwners(payloads);
		if (!owners) {
			++f.failed;
			return false;
		}
		// Refraction: the pipeline asked for and its inputs posted (each once, or when they change), and whether this recording draws
		// the refraction normals (RefractionDrawPass: the colour shape has refraction slots and the refraction pipeline of the imported
		// normals' format).
		bool refraction = false;
		if (Extensions::ObjectClassOn(toggles, Api::ObjectClass::Refraction)) {
			const auto& resources = impl->resources;
			auto& pipelines = DrawPipelines::Get();
			pipelines.RequestRefraction();
			if (resources && resources->refractionNormals && globals::game::utilityShader) {
				// The engine's write mask for write mode 2 (FUN_1414b4860), blending off: the forward table's, as it is outside the deferred pass.
				std::uint8_t writeMask = 0xF;
				if (auto* blend = EngineBlendStates()[0][0][2][0]) {
					D3D11_BLEND_DESC desc{};
					blend->GetDesc(&desc);
					writeMask = static_cast<std::uint8_t>(desc.RenderTarget[0].RenderTargetWriteMask & 0xF);
				}
				pipelines.SetRefractionInputs(*globals::game::utilityShader, resources->refractionFormat, writeMask);
			}
			const auto shape = revision.shape ? revision.shape->Value<PassFrame>() : nullptr;
			if (shape && shape->refractionCapacity) {
				refraction = resources && resources->refractionNormals && resources->pool && shape->indirect.refraction.valid() &&
				             shape->indirect.refractionFormat == resources->refractionFormat;
				if (!refraction)
					++f.refractionNotReady;
			}
		}
		const bool ok = RenderGraphRuntime::Get().ExecuteEpoch(RenderGraphRuntime::Segment::Forward, [&](org::RenderGraph&) {
			impl->SubmitRevisionRecording(Epoch::Forward, *revision.recordings, 0);
		}, owners);
		if (!ok) {
			++f.failed;
			return false;
		}
		f.submittedFrame = frame;
		++f.epochs;
		// Refraction: the epoch cleared and drew the refraction normals, ahead of Main::Draw's own group 5 (FUN_1414b4860). The caller
		// (Features/Forward's blended-draw point) marks them begun and drawn, as that function does, before the engine's blended draws.
		if (refraction) {
			f.refractionFrame = frame;
			++f.refractionDrawn;
		}
		return refraction;
	}

	bool IndirectDraws::RefractionDrawnThisFrame() const
	{
		return impl && impl->forwardEpoch.refractionFrame == Scene::FrameView::Get().GetFrame();
	}

	void IndirectDraws::RunEpoch(bool a_depthOnly)
	{
		const auto segment = a_depthOnly ? RenderGraphRuntime::Segment::ZPrepass : RenderGraphRuntime::Segment::MainOpaque;
		ZoneScopedN("CS.DCLF.RunEpoch");
		RenderGraphRuntime::EpochBodyScope body(segment);
		ScopedPerfEvent event(segment == RenderGraphRuntime::Segment::ZPrepass ? "CS DCLF: Z-prepass (CPU)" : "CS DCLF: main opaque (CPU)");
		const auto start = std::chrono::steady_clock::now();
		const bool depthOnly = a_depthOnly;
		const std::size_t jobIndex = depthOnly ? kAsyncZPrepass : kAsyncColour;
		if (!impl->pending) {
			return;
		}
		auto capture = std::move(*impl->pending);
		impl->pending.reset();
		auto& pipelines = DrawPipelines::Get();
		auto* lighting = EngineShaders::Lighting();
		if (failed || !lighting || !pipelines.Enabled() || !FrameIndirectState().valid) {
			capture.Release();
			return;
		}
		bool ready = false;
		const auto resourcesBefore = impl->resources;
		try {
			ready = impl->Setup(capture, depthOnly);
			// New main resources (the targets or the frame buffers changed) under claims: the graph is built for them now, not at the
			// next build point - the colour epoch, the frame's last, draws its own preparation with them; the next frame's revision
			// recordings are then of the build before, and that frame is the engine's (DecideCoverage).
			if (ready && impl->resources != resourcesBefore && resourcesBefore)
				if (auto* host = RenderGraphRuntime::Get().Host())
					(void)host->BuildIfRequested();
		} catch (const std::exception& e) {
			// Never retried: the feature stays on the native path.
			logger::error("[DCLF] Main-pass graph resources could not be created: {}", e.what());
			failed = true;
		}
		if (!ready) {
			capture.Release();
			++stats.notReady;
			return;
		}

		auto resources = impl->resources;
		stats.skipped = {};
		stats.missingTextures = {};
		stats.missingVertexConstants = stats.missingPixelConstants = 0;

		// The per-frame constant blocks, from the capture's mirrors: render thread, before the epoch. Their
		// slot mask is an input of the build; their bytes are uploaded by the commit.
		FrameBlocks blocks;
		impl->PackFrameBlocks(capture, depthOnly, blocks);
		// What a scene revision's shape of the segment is made from that only the frame's capture has (MakeRevision): its
		// viewport, both segments at the main pass's depth range (Impl::mainMinDepth), and its frame blocks' sizes, the largest seen
		// (a revision's latched copies hold every block a frame binds: a commit's smaller or absent block is zeroed past its bytes,
		// never staged). Taken here, whether or not the epoch is submitted, so revisions never wait for a commit.
		{
			auto& parity = impl->mainEpochInputs;
			const std::size_t shape = depthOnly ? kDepthShape : kColourShape;
			const MainViewport viewportBefore = parity.viewport[shape];
			const FrameBlockSizes sizesBefore = parity.blockSizes[shape];
			const bool knownBefore = parity.known[shape];
			const bool mainRange = impl->mainMaxDepth > 0.0f;
			parity.viewport[shape] = { capture.viewportWidth, capture.viewportHeight, mainRange ? impl->mainMinDepth : capture.minDepth,
				mainRange ? impl->mainMaxDepth : capture.maxDepth };
			for (std::uint32_t slot = 0; slot < kConstantBufferRegisters; ++slot) {
				auto& sizes = parity.blockSizes[shape];
				sizes.vs[slot] = std::max(sizes.vs[slot], static_cast<std::uint32_t>(blocks.vs[slot].size()));
				sizes.ps[slot] = std::max(sizes.ps[slot], static_cast<std::uint32_t>(blocks.ps[slot].size()));
			}
			parity.known[shape] = true;
			// Revision inputs: posted when they moved.
			if (!knownBefore || !(viewportBefore == parity.viewport[shape]) || !(sizesBefore == parity.blockSizes[shape]))
				impl->PostRevisionInputs();
		}
		// The frame slots the epoch supplies, and the Z-prepass's vertex inputs the colour epoch replays: what the builds ahead are made
		// for (PostAheadContext). Taken on a frame without claims too, so the first publication's payloads are built before any frame
		// is DCLF's (a snapshot without them gives the frame no claims).
		auto& masks = impl->epochMasks[jobIndex];
		masks.vsMask = blocks.vsMask;
		masks.psMask = blocks.psMask;
		masks.known = true;
		if (depthOnly) {
			impl->prepassInputs = true;
		}
		// A frame without claims (Coordinator::WithdrawSet: no revision covers it) has nothing of DCLF's to draw, and is not submitted.
		if (Scene::FrameView::Get().SetWithdrawn()) {
			capture.Release();
			return;
		}
		// The selected revision's shape and recording (DecideCoverage covered the main epochs), and the installed publication's
		// payload (a snapshot without one for each epoch gives the frame no claims: DecideCoverage): trusted, nothing is built or checked here. A payload built
		// for other main resources (made at this frame's Setup: a new render size) names their buffers: the frame is not submitted.
		const auto revision = impl->RevisionOf(static_cast<std::uint32_t>(depthOnly ? kDepthShape : kColourShape));
		std::shared_ptr<MainPayload> ahead = impl->installedDraws ? impl->installedDraws->payloads[jobIndex] : nullptr;
		if (!revision || !ahead || ahead->inputs.addresses.identity != resources.get()) {
			// Resources made at this frame's Setup are the only way here (the graph is built for them, and the frames until their
			// revision are the engine's); anything else is a defect.
			if (revision && resources == resourcesBefore && impl->unbuilt[jobIndex]++ == 0)
				logger::warn("[DCLF] the {} epoch of frame {} has no payload for its resources in the installed publication: not submitted <- UNBUILT",
					depthOnly ? "Z-prepass" : "colour", Scene::FrameView::Get().GetFrame());
			capture.Release();
			return;
		}
		MainInputs in = impl->PrepareMainInputs(depthOnly, *resources, blocks.vsMask, blocks.psMask);
		auto& async = stats.async[jobIndex];
		const MainPayload* committed = nullptr;

		const auto cleanup = RenderGraphRuntime::Get().Host()->ResourceCleanup();
		if (!cleanup) {
			capture.Release();
			++stats.notReady;
			return;
		}
		auto frameOwners = cleanup->Make<std::vector<std::shared_ptr<const void>>>();
		const bool ok = RenderGraphRuntime::Get().ExecuteEpoch(segment, [&](org::RenderGraph&) {
			// The lookups hold still for the whole frame: the installed publication's, which its payloads were built with.
			const auto& lookups = Scene::FrameView::Get().GetLookups();
			in.lookupGeneration = lookups.generation;
			MainPayload* payload = ahead.get();
			++async.used;
			committed = payload;
			// Copied: a frame that keeps the publication commits the payload again.
			*frameOwners = payload->bindingOwners;
			frameOwners->push_back(ahead);
			// The adopted snapshot whole (its versions, its draws, what they embed), until this execution retires: released on ORG's
			// host thread through the cleanup queue, never before the GPU is done with the frame (the builder's drop of a replaced one is a
			// CPU drop alone).
			if (impl->adoptedSnapshot)
				frameOwners->push_back(impl->adoptedSnapshot);
			impl->CommitMainPayload(capture, blocks, in, *payload, resources, stats, *frameOwners, revision);
		}, frameOwners);
		impl->committedPayload[jobIndex] = committed;
		impl->committedFrame[jobIndex] = Scene::FrameView::Get().GetFrame();
		capture.Release();
		++stats.epochs;
		if (!committed) {
			stats.cpuMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
			return;
		}
		// The epoch's readbacks (Diagnostics/Draws: the culling's counters, the colour epoch's only - both epochs run BuildDraws into
		// the same count buffer, and the Z-prepass builds from the far smaller set the colour epoch drew last frame - and the parities).
		if (ok) {
			const MainPayload* depth = !depthOnly && impl->committedFrame[kAsyncZPrepass] == Scene::FrameView::Get().GetFrame() ? impl->committedPayload[kAsyncZPrepass] : nullptr;
			Diagnostics::Notify(Diagnostics::MainEpochDone{ resources, *committed, depth, depthOnly, impl->InstalledPayload(), stats });
		}

		stats.cpuMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
		// The remainder is the epoch execution itself and the commit: the payload was built ahead, so the build's parts are the
		// builder's time and the render thread's is all remainder.
		stats.partMs[7] = stats.cpuMs;
		if (!ok)
			logger::error("[DCLF] The main-pass epoch failed; the render graph is disabled");
	}

	void IndirectDraws::DrainAsync()
	{
		impl->WaitAhead();
		impl->WaitFadeWriteBack();
		impl->WaitVisibilityReadback();
	}

	namespace
	{
		/**
		 * @brief The blocks the epoch supplies whatever the pass binds: the game's PerFrame (b12), Community Shaders' SharedData (b5)
		 * and feature data, and DCLF's own frame fog slot (VS b13), named in the mask.
		 */
		void SupplyFixedBlocks(FrameBlocks& a_out)
		{
			// b12 is the game's PerFrame buffer. The constant mirror does not always hold its contents - the
			// engine does not rewrite it through the hooked context every frame - and an empty slot drops
			// every object whose shaders read it. Community Shaders already caches this buffer between Map
			// and Unmap for its own use, so take it from there.
			{
				const auto& cached = globals::game::frameBufferCached.data;
				const auto* bytes = reinterpret_cast<const std::byte*>(&cached);
				if (a_out.vs[kPerFrameVertexRegister].empty())
					a_out.vs[kPerFrameVertexRegister].assign(bytes, bytes + sizeof(cached));
				if (a_out.ps[kPerFrameVertexRegister].empty())
					a_out.ps[kPerFrameVertexRegister].assign(bytes, bytes + sizeof(cached));
			}
			// b5 is Community Shaders' own SharedData, written through its ConstantBuffer helper rather than
			// through the hooked device context, so the constant mirror never observes the write and the
			// slot would stay empty, dropping every object whose shader reads it. CS keeps the struct it
			// uploaded, so pack that directly.
			// These are supplied in the Z-prepass too. Its pixel stage is the DCLF_DEPTH_ONLY build,
			// which still reads them in the code that runs before the alpha test, and they all come from
			// Community Shaders or the game's own cache rather than from the main pass's bindings - so they
			// are available during the native depth pass, where the main pass's bindings are not.
			if (auto* state = globals::state) {
				const auto* shared = reinterpret_cast<const std::byte*>(&state->lastSharedData);
				a_out.ps[kSharedDataRegister].assign(shared, shared + sizeof(State::SharedDataCB));
				if (!state->lastFeatureData.empty()) {
					const auto* feature = reinterpret_cast<const std::byte*>(state->lastFeatureData.data());
					a_out.ps[kFeatureDataRegister].assign(feature, feature + state->lastFeatureData.size());
				}
			}
			// The vertex stage's b13 is DCLF's own (DCLFFrameFog), latched by the commit: nothing the pass binds there is the draws'.
			// Its block stays empty here, but the slot is supplied, so the mask names it (a pipeline reading an unsupplied slot is
			// rejected by the constants check).
			a_out.vs[kFrameFogRegister].clear();
			a_out.vsMask |= 1u << kFrameFogRegister;
		}
	}

	void IndirectDraws::Impl::PackFrameBlocks(const Capture& a_capture, bool a_depthOnly, FrameBlocks& a_out)
	{
		RenderThreadBudget::Part budget(RenderThreadBudget::Bucket::Capture);
		// The colour epoch replays the vertex-stage bytes the Z-prepass used, so the
		// two agree to the bit and the colour pass's EQUAL test passes.
		const bool replayVertexInputs = !a_depthOnly && prepassInputs;
		MirrorFrameBlocks(a_capture, a_depthOnly, replayVertexInputs, a_out);
		SupplyFixedBlocks(a_out);
		CarryMissingBlocks(a_depthOnly, replayVertexInputs, a_out);
		for (std::uint32_t slot = 0; slot < kConstantBufferRegisters; ++slot) {
			if (!a_out.vs[slot].empty())
				a_out.vsMask |= 1u << slot;
			if (!a_out.ps[slot].empty())
				a_out.psMask |= 1u << slot;
		}

		// One line saying, per pixel register, whether the pass had a buffer bound and whether the
		// constant mirror could supply its contents: a slot that is bound but not mirrored ends up as a
		// null address and drops every object whose shader reads it.
		if (!a_depthOnly && !loggedFrameConstants) {
			loggedFrameConstants = true;
			std::string text;
			for (std::uint32_t slot = 0; slot < kConstantBufferRegisters; ++slot) {
				if ((kPerDrawPS >> slot) & 1)
					continue;
				text += fmt::format("{}b{}={}/{}", text.empty() ? "" : " ", slot, a_capture.psBuffers[slot] ? "bound" : "unbound",
					((a_out.psMask >> slot) & 1) ? "supplied" : "empty");
			}
			logger::info("[DCLF] main-pass per-frame pixel constants: {}", text);
		}
	}

	void IndirectDraws::Impl::MirrorFrameBlocks(const Capture& a_capture, bool a_depthOnly, bool a_replayVertexInputs, FrameBlocks& a_out)
	{
		// Per-frame constant buffers: whatever the main pass binds outside the per-draw slots.
		// On the Z-prepass the pixel-stage per-frame bindings are skipped entirely: the native depth pass
		// has not bound the main pass's yet, and the DCLF_DEPTH_ONLY build of the pixel stage compiles
		// away everything that would read them.
		auto& mirror = ConstantMirror::Get();
		if (a_depthOnly)
			for (auto& bytes : prepassVS)
				bytes.clear();
		for (std::uint32_t slot = 0; slot < kConstantBufferRegisters; ++slot) {
			if (a_replayVertexInputs && !((kPerDrawVS >> slot) & 1)) {
				const auto& bytes = prepassVS[slot];
				if (!bytes.empty())
					a_out.vs[slot].assign(bytes.begin(), bytes.end());
			}
			for (auto [buffers, perDraw, out] : { std::tuple{ &a_capture.vsBuffers, kPerDrawVS, &a_out.vs }, std::tuple{ &a_capture.psBuffers, kPerDrawPS, &a_out.ps } }) {
				if (a_depthOnly && out == &a_out.ps)
					continue;
				if (a_replayVertexInputs && out == &a_out.vs)
					continue;
				auto* buffer = (*buffers)[slot];
				if (!buffer || ((perDraw >> slot) & 1))
					continue;
				mirror.Watch(buffer);
				const auto contents = mirror.Contents(buffer);
				if (!contents.empty())
					(*out)[slot].assign(contents.begin(), contents.end());
				if (a_depthOnly && out == &a_out.vs && !contents.empty())
					prepassVS[slot].assign(contents.begin(), contents.end());
			}
		}
	}

	void IndirectDraws::Impl::CarryMissingBlocks(bool a_depthOnly, bool a_replayVertexInputs, FrameBlocks& a_out)
	{
		// A slot the epoch supplied before and lacks now - a feature's block the capture point sees bound in some frames
		// only (VS b7) - is supplied from its last capture, counted, so the frame's slots are the same every frame and the builds
		// ahead, made for the slots the epoch last supplied, stay usable. The Z-prepass's carried vertex blocks are
		// the colour epoch's replay too: the two transform alike.
		auto& carry = carriedBlocks[a_depthOnly ? 0 : 1];
		for (std::uint32_t slot = 0; slot < kConstantBufferRegisters; ++slot) {
			for (auto [out, kept, vertex] : { std::tuple{ &a_out.vs[slot], &carry.vs[slot], true }, std::tuple{ &a_out.ps[slot], &carry.ps[slot], false } }) {
				if (!out->empty()) {
					kept->assign(out->begin(), out->end());
					continue;
				}
				if (kept->empty() || (vertex && a_replayVertexInputs) || (!vertex && a_depthOnly))
					continue;
				out->assign(kept->begin(), kept->end());
				++frameSlotsCarried;
				if (const std::uint32_t bit = 1u << (slot + (vertex ? 0 : 16) + (a_depthOnly ? 0 : 8)); !(carriedLogged & bit)) {
					carriedLogged |= bit;
					logger::info("[DCLF] {} epoch, frame {}: {} b{} not bound at the capture, supplied from an earlier one ({} bytes)", a_depthOnly ? "zprepass" : "colour",
						Scene::FrameView::Get().GetFrame(), vertex ? "VS" : "PS", slot, out->size());
				}
				if (vertex && a_depthOnly)
					prepassVS[slot] = *out;
			}
		}
	}

	MainInputs IndirectDraws::Impl::PrepareMainInputs(bool a_depthOnly, const Resources& a_resources, std::uint32_t a_vsMask, std::uint32_t a_psMask)
	{
		MainInputs in;
		in.frameNumber = Scene::FrameView::Get().GetFrame();
		in.depthOnly = a_depthOnly;
		in.vsFrameMask = a_vsMask;
		in.psFrameMask = a_psMask;
		// The versioned buffers' addresses, SRV indices and capacities, the fit and the decal range are the sealed set's and layout's,
		// and what the buffers hold the installed payload's (BuildSnapshotDraws: AddressesOf; RunAhead).
		in.addresses.frameConstants = a_resources.frameConstantsAddress;
		in.addresses.placementsIndex = FrameValues::Get().PlacementsIndex();
		in.addresses.palettesIndex = FrameValues::Get().PalettesIndex();
		in.addresses.shadingIndex = FrameValues::Get().ShadingIndex();
		in.addresses.identity = &a_resources;
		in.tablesGeneration = Scene::FrameView::Get().GetTablesGeneration();
		in.lookupGeneration = Scene::FrameView::Get().GetLookups().generation;
		return in;
	}
}
