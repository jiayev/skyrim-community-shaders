#if defined(CS_HAS_RENDER_GRAPH) && defined(CS_HAS_ORG_MODULE_SERVICES)
#	include "Internal.h"

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
		impl->probeTargetCount = std::min<std::uint32_t>(impl->pending->targetCount, kColorTargets);
		for (std::uint32_t i = 0; i < impl->probeTargetCount; ++i)
			impl->probeTargets[i] = impl->pending->targets[i];
		impl->probeDepth = impl->pending->depth;
	}

	void IndirectDraws::CheckCapturePoint()
	{
		if (!impl->pending)
			return;
		const auto& captured = *impl->pending;
		auto now = CaptureBindings();
		// Only what the colour epoch takes from a capture: the frame registers, the frame textures, the targets, the
		// viewport and the eye (the per-draw registers and textures are the draw's own).
		auto& p = impl->captureParity;
		bool any = false;
		auto note = [&](std::string a_what) {
			++p.differ[std::move(a_what)];
			any = true;
		};
		for (std::uint32_t slot = 0; slot < kConstantBufferRegisters; ++slot) {
			if (!((kPerDrawVS >> slot) & 1) && captured.vsBuffers[slot] != now.vsBuffers[slot])
				note(fmt::format("VS b{}", slot));
			if (!((kPerDrawPS >> slot) & 1) && captured.psBuffers[slot] != now.psBuffers[slot])
				note(fmt::format("PS b{}", slot));
		}
		for (std::uint32_t t = kPixelTextureSlots; t < kTextureRegisters; ++t)
			if (captured.psViews[t] != now.psViews[t])
				note(fmt::format("t{}", t));
		for (std::uint32_t i = 0; i < kColorTargets; ++i)
			if (captured.targets[i] != now.targets[i])
				note(fmt::format("rt{}", i));
		if (captured.depth != now.depth)
			note("depth");
		if (captured.viewportWidth != now.viewportWidth || captured.viewportHeight != now.viewportHeight || captured.minDepth != now.minDepth ||
			captured.maxDepth != now.maxDepth)
			note("viewport");
		if (std::memcmp(&captured.eye, &now.eye, sizeof(captured.eye)) != 0 || std::memcmp(&captured.previousEye, &now.previousEye, sizeof(captured.previousEye)) != 0)
			note("eye");
		now.Release();
		p.frames += any ? 1u : 0u;
		if (++p.checks == 300) {
			std::string text;
			for (const auto& [what, count] : p.differ)
				text += fmt::format(" {}={}", what, count);
			logger::info("[DCLF] capture point parity: {} frames' captures against the first lighting draw's bindings, {} differ{}", p.checks, p.frames,
				p.frames ? " <- DIFFER:" + text : std::string(" <- OK"));
			p = {};
		}
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
		impl->probeDepth = impl->pending->depth;
		ProbeTargets(kProbeFirstLabel);
		RunEpoch(true);
		ProbeTargets("after z-prepass");
	}

	void IndirectDraws::ProbeTargets(const char* a_label)
	{
		impl->ProbeGBuffer(a_label);
	}

	void IndirectDraws::ExecuteColour()
	{
		// The colour epoch assembles from the main pass's own capture: the Z-prepass ran off the depth
		// pass's, where the pixel-stage bindings were not available. Both draw the same tables with the
		// same camera, so the depths agree and the colour pass can test EQUAL.
		RunEpoch(false);
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
			impl->DropMainJob(jobIndex, stats);
			return;
		}
		auto capture = std::move(*impl->pending);
		impl->pending.reset();
		auto& pipelines = DrawPipelines::Get();
		auto* lighting = ConstantEvaluator::Get().GetLightingShader();
		if (failed || !lighting || !pipelines.Enabled() || !GetIndirectState().valid) {
			capture.Release();
			impl->DropMainJob(jobIndex, stats);
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
			impl->DropMainJob(jobIndex, stats);
			return;
		}

		auto& store = SceneStore::Get();
		const auto& tables = store.GetTables();
		auto resources = impl->resources;
		// The scene tables hold the frame's, and the sequence buffer every draw the scene can produce: grown here, before the
		// epoch, when they would not.
		impl->ReserveSceneTables(tables);
		impl->ReserveMainSequences(tables);
		stats.skipped = {};
		stats.missingTextures = {};
		stats.missingVertexConstants = stats.missingPixelConstants = 0;

		// The per-frame constant blocks, from the capture's mirrors: render thread, before the epoch. Their
		// slot mask is an input of the build; their bytes are uploaded by the commit.
		FrameBlocks blocks;
		impl->PackFrameBlocks(capture, depthOnly, blocks);
		// What a scene revision's shape of the segment is made from that only the frame's capture has (MakeRevisionShapes): its
		// viewport, both segments at the main pass's depth range (Impl::mainMinDepth), and its frame blocks' sizes, the largest seen
		// (a revision's latched copies hold every block a frame binds: a commit's smaller or absent block is zeroed past its bytes,
		// never staged). Taken here, whether or not the epoch is submitted, so revisions never wait for a commit.
		{
			auto& parity = impl->shapeParity;
			const std::size_t shape = depthOnly ? kDepthShape : kColourShape;
			const bool mainRange = impl->mainMaxDepth > 0.0f;
			parity.viewport[shape] = { capture.viewportWidth, capture.viewportHeight, mainRange ? impl->mainMinDepth : capture.minDepth,
				mainRange ? impl->mainMaxDepth : capture.maxDepth };
			for (std::uint32_t slot = 0; slot < kConstantBufferRegisters; ++slot) {
				auto& sizes = parity.blockSizes[shape];
				sizes.vs[slot] = std::max(sizes.vs[slot], static_cast<std::uint32_t>(blocks.vs[slot].size()));
				sizes.ps[slot] = std::max(sizes.ps[slot], static_cast<std::uint32_t>(blocks.ps[slot].size()));
			}
			parity.known[shape] = true;
		}
		// Strict epochs: a frame without claims (SceneStore::WithdrawSet: no revision covers it) has nothing of DCLF's to draw, and
		// is not submitted.
		if (RevisionClaims() && store.SetWithdrawn()) {
			capture.Release();
			impl->DropMainJob(jobIndex, stats);
			return;
		}
		MainInputs in = impl->PrepareMainInputs(&capture, depthOnly, *resources, blocks.vsMask, blocks.psMask, store);
		auto& job = impl->mainJobs[jobIndex];
		job.vsMask = blocks.vsMask;
		job.psMask = blocks.psMask;
		job.masksKnown = true;
		auto& payload = impl->mainPayload[jobIndex];
		auto& async = stats.async[jobIndex];
		bool builtOnWorker = false;

		const auto cleanup = RenderGraphRuntime::Get().Host()->ResourceCleanup();
		if (!cleanup) {
			capture.Release();
			++stats.notReady;
			impl->DropMainJob(jobIndex, stats);
			return;
		}
		auto frameOwners = cleanup->Make<std::vector<std::shared_ptr<const void>>>();
		const bool ok = RenderGraphRuntime::Get().ExecuteEpoch(segment, [&](org::RenderGraph&) {
			// The descriptor entries the build reads, resolved now that the descriptor service is active.

			// The worker's build, if one was kicked for this epoch and it was built for exactly these inputs;
			// otherwise the build runs here. A late or stale job is dropped (its payload is the one this
			// build overwrites, so a late job is waited for before the inline build; counted). Joined before
			// the lookup refresh: the worker reads the lookups until it is done. The kick refreshed them for the job, so
			// they are refreshed here after it is taken (queueing the imports only an epoch can), or before the build here.
			const auto joinStart = std::chrono::steady_clock::now();
			const auto joined = JoinJob(job.handle);
			stats.commitUs[0] += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - joinStart).count();
			// The lookups hold still for the whole frame (refreshed at its start alone, step 6e C).
			const auto& lookups = store.GetLookups();
			in.lookupGeneration = lookups.generation;
			bool useAsync = false;
			if (job.handle) {
				useAsync = TakeJob(
					joined, async, [&] { return SameInputs(job.inputs, in); }, [&] {
						if (job.inputs.lookupGeneration != in.lookupGeneration)
							++async.staleLookups;
						impl->LogStaleMainJob(jobIndex, in);
					});
				// The Z-prepass job's eye is a prediction: counted apart from the other reasons for staleness.
				if (depthOnly && joined == AsyncWorker::WaitResult::Done) {
					const bool eyeDiffers = std::memcmp(&job.inputs.eye, &in.eye, sizeof(RE::NiPoint3)) != 0;
					const bool previousDiffers = std::memcmp(&job.inputs.previousEye, &in.previousEye, sizeof(RE::NiPoint3)) != 0;
					if (eyeDiffers || previousDiffers)
						++async.eyeMismatches;
					if (!eyeDiffers && previousDiffers)
						++async.previousEyeMismatches;
				}
				job.handle = {};
			}
			builtOnWorker = useAsync;
			if (useAsync) {
				++async.used;
				ProbeWorkerBuild(payload, impl->probePayload, async, depthOnly ? "zprepass" : "colour",
					[&](MainPayload& a_probe) {
						// Its own rows, written from scratch (the worker's are the scene's, kept across frames).
						MainRows probeRows;
						BuildMainPayload(job.inputs, tables, store.GetFrameTables(), lookups, a_probe, probeRows);
					});
			} else {
				++async.builtInline;
				BuildMainPayload(in, tables, store.GetFrameTables(), lookups, payload, impl->mainRows, impl->CacheFor(jobIndex), impl->SceneObjects(), impl->SceneExtras(), impl->SceneGeometries());
			}
			*frameOwners = std::move(payload.bindingOwners);
			impl->CommitMainPayload(capture, blocks, payload, resources, store, stats, *frameOwners);
		}, frameOwners);
		capture.Release();
		++stats.epochs;
		if (ok && BuildParityEnabled())
			impl->CheckBuildParity(resources, payload, stats);
		// Only the colour epoch's counters. Both epochs run BuildDraws into the same count buffer, and the
		// Z-prepass builds from the far smaller set the colour epoch drew last frame, so sampling whichever
		// ran most recently alternates between two unrelated populations.
		if (ok && !depthOnly)
			impl->ReadCullCounters(resources, stats, payload);
		if (ok && !depthOnly && PersistentParityEnabled())
			impl->ReadTreeWind(resources);
		if (ok && depthOnly)
			impl->ReadFadeLog(resources);
		if (ok && !depthOnly && SetParityEnabled())
			impl->CheckSetParity(resources, impl->mainPayload[1], payload);

		stats.cpuMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
		// The remainder is the epoch execution itself and the commit. It stays a subtraction, but it is now
		// a small one rather than the bucket the per-draw work hid in. When the worker built the payload, the
		// build's parts are the worker's time and the render thread's is all remainder (the join included).
		stats.partMs[7] = builtOnWorker ? stats.cpuMs :
		                                  stats.cpuMs - (stats.partMs[0] + stats.partMs[1] + stats.partMs[2] +
		                                                    stats.partMs[3] + stats.partMs[4] + stats.partMs[5] + stats.partMs[6]);
		if (!ok)
			logger::error("[DCLF] The main-pass epoch failed; the render graph is disabled");
	}

	void IndirectDraws::RefreshMainLookups()
	{
		if (!impl || failed)
			return;
		auto& store = SceneStore::Get();
		RefreshMaterialLookups(store, store.GetTables(), true, store.GetProjectedTextures(), store.MutableLookups());
	}

	void IndirectDraws::KickColourBuild()
	{
		// The colour epoch's inputs are final from here (RefreshFrameConstants was the frame's last writer of
		// the tables), and the epoch itself is ~1.5-2 ms of native rendering away: the build runs on the
		// worker in between, so the render thread only commits when the epoch comes. It replays the Z-prepass's
		// eye and constants; without them the eye would come from a capture that does not exist yet.
		// A build kicked at EarlyPrepass (KickZPrepassBuild) is kept when nothing it read has changed since: not the versioned
		// values RefreshFrameConstants writes (BuildInputsWitness), nor the lookups (a refresh here, which an import the
		// Z-prepass epoch queued may move).
		auto& store = SceneStore::Get();
		auto& early = impl->mainJobs[kAsyncColour];
		if (std::exchange(impl->colourEarly, false) && early.handle) {
			// Nothing a build reads changes after the frame's start (the tables accepted, the lookups refreshed: step 6e): the early
			// build stands. One that does not is a defect, counted (and built again).
			if (store.BuildInputsWitness() == impl->colourWitness && early.inputs.lookupGeneration == store.GetLookups().generation) {
				++stats.async[kAsyncColour].earlyKept;
				return;
			}
			auto& by = stats.async[kAsyncColour].earlyRekickedBy;
			const auto witness = store.BuildInputsWitness();
			by[0] += (witness >> 32) != (impl->colourWitness >> 32) ? 1u : 0u;
			by[1] += static_cast<std::uint32_t>(witness) != static_cast<std::uint32_t>(impl->colourWitness) ? 1u : 0u;
			by[2] += early.inputs.lookupGeneration != store.GetLookups().generation ? 1u : 0u;
			++stats.async[kAsyncColour].earlyRekicked;
		}
		impl->DropMainJob(kAsyncColour, stats);
		if (!AsyncEnabled())
			return;
		if (!impl->resources || !impl->prepassInputs) {
			++stats.async[kAsyncColour].notKicked;
			return;
		}
		impl->KickMainJob(false, nullptr, nullptr, stats);
	}

	void IndirectDraws::KickZPrepassBuild()
	{
		// The Z-prepass epoch's inputs are final from here to the Z-prepass in Main_RenderDepth - the accumulate
		// phase was the tables' last writer, RefreshFrameConstants runs after the epoch - except the eye,
		// which the epoch captures from posAdjust. posAdjust still holds the shadow cameras' here, so the eye
		// is predicted: the main camera's world position, and last frame's captured eye as the previous one.
		// The epoch compares both exactly with its capture.
		impl->DropMainJob(kAsyncZPrepass, stats);
		if (!AsyncEnabled())
			return;
		auto* camera = RE::Main::WorldRootCamera();
		if (!impl->resources || !impl->prepassInputs || !camera) {
			++stats.async[kAsyncZPrepass].notKicked;
			return;
		}
		const RE::NiPoint3 eye = camera->world.translate;
		const RE::NiPoint3 previousEye = impl->prepassEye;
		// The lookups as the frame's start refreshed them (step 6e C): they hold still while the jobs read them.
		auto& store = SceneStore::Get();
		impl->KickMainJob(true, &eye, &previousEye, stats);
		// The colour build too, behind it on the worker: its inputs are final from here but for what RefreshFrameConstants
		// writes at Prepass, which first waits for it (BeforeFrameConstants) and kicks it again only when that changed what it
		// read. Not under the bindless parity, whose builds read the eye the Z-prepass epoch captures.
		impl->DropMainJob(kAsyncColour, stats);
		impl->colourEarly = false;
		if (impl->resources && impl->prepassInputs && !SwitchEnabled(Switch::BindlessParity)) {
			impl->colourWitness = store.BuildInputsWitness();
			impl->KickMainJob(false, nullptr, nullptr, stats);
			impl->colourEarly = static_cast<bool>(impl->mainJobs[kAsyncColour].handle);
			stats.async[kAsyncColour].earlyKicked += impl->colourEarly ? 1u : 0u;
		}
	}

	void IndirectDraws::Impl::KickMainJob(bool a_depthOnly, const RE::NiPoint3* a_eye, const RE::NiPoint3* a_previousEye, IndirectDraws::Stats& a_stats)
	{
		ZoneScopedN("CS.DCLF.KickMainJob");
		const std::size_t index = a_depthOnly ? kAsyncZPrepass : kAsyncColour;
		auto& job = mainJobs[index];
		auto& async = a_stats.async[index];
		auto* lighting = ConstantEvaluator::Get().GetLightingShader();
		if (!lighting || !DrawPipelines::Get().Enabled() || !GetIndirectState().valid || !job.masksKnown) {
			++async.notKicked;
			return;
		}
		// The scene stores are the shadow build's too: it is joined at AfterShadowMaps, before either main job is kicked.
		DropShadowJob(a_stats);
		auto& store = SceneStore::Get();
		// What the build is built against: the scene tables and the rows grown to the frame's (the epoch's own reserve then finds
		// nothing to grow).
		ReserveSceneTables(store.GetTables());
		ReserveMainSequences(store.GetTables());
		job.inputs = PrepareMainInputs(nullptr, a_depthOnly, *resources, job.vsMask, job.psMask, store);
		// A build without the bindless parity does not read the eye (PrepareMainInputs leaves it zero), so the
		// prediction only matters where it does.
		if (a_eye && BuildReadsEye(job.inputs))
			job.inputs.eye = *a_eye;
		if (a_previousEye && BuildReadsEye(job.inputs))
			job.inputs.previousEye = *a_previousEye;
		++async.kicked;
		const auto* tables = &store.GetTables();
		const auto* frameTables = &store.GetFrameTables();
		const auto* lookups = &store.GetLookups();
		auto* payload = &mainPayload[index];
		auto* cache = CacheFor(index);
		auto* objects = SceneObjects();
		auto* extras = SceneExtras();
		auto* geometriesStore = SceneGeometries();
		auto* rows = &mainRows;
		auto* pool = &stagedPools[index];
		const MainInputs inputs = job.inputs;
		const rhi::Device device = RecordingDevice();
		job.handle = AsyncWorker::Get().Submit(a_depthOnly ? "zprepass" : "colour", [inputs, tables, frameTables, lookups, payload, rows, cache, objects, extras, geometriesStore, pool,
																							target = resources, device](std::stop_token) {
			BuildMainPayload(inputs, *tables, *frameTables, *lookups, *payload, *rows, cache, objects, extras, geometriesStore);
			if (target)
				StageMainPayload(*payload, *target, *pool, device);
		});
	}

	void IndirectDraws::KickSceneStreams()
	{
		if (impl && !failed)
			impl->KickSceneStreams();
	}

	std::array<std::uint64_t, 3> IndirectDraws::TakeStreamsStats()
	{
		if (!impl)
			return {};
		auto& job = impl->streamsJob;
		return { std::exchange(job.kicked, 0), std::exchange(job.used, 0), std::exchange(job.dropped, 0) };
	}

	void IndirectDraws::EndFrame()
	{
		AsyncWorker::Get().NoteFrame();
		// Never taken this frame (no commit ran after it): its inputs name this frame's tables.
		impl->DropSceneStreams();
		// A job kicked this frame and never joined (the epoch did not run: a load screen, a failed setup) must
		// not outlive the frame: its inputs name this frame's tables.
		for (std::size_t j = 0; j < impl->mainJobs.size(); ++j) {
			if (impl->mainJobs[j].handle) {
				++stats.async[j].leaked;
				impl->DropMainJob(j, stats);
			}
		}
		if (impl->shadowJob.handle) {
			++stats.async[kAsyncShadow].leaked;
			impl->DropShadowJob(stats);
		}
	}

	void IndirectDraws::DrainAsync()
	{
		impl->DropSceneStreams();
		for (std::size_t j = 0; j < impl->mainJobs.size(); ++j)
			impl->DropMainJob(j, stats);
		impl->DropShadowJob(stats);
		AsyncWorker::Get().Drain();
	}

	void IndirectDraws::Impl::DropMainJob(std::size_t a_job, IndirectDraws::Stats& a_stats)
	{
		auto& job = mainJobs[a_job];
		if (!job.handle)
			return;
		AsyncWorker::Get().Cancel(job.handle);
		++a_stats.async[a_job].dropped;
		job.handle = {};
	}

	void IndirectDraws::Impl::LogStaleMainJob(std::size_t a_job, const MainInputs& a_actual)
	{
		auto& job = mainJobs[a_job];
		if (job.loggedStale++ >= 4)
			return;
		const auto& k = job.inputs;
		logger::info("[DCLF] async {}: the job's inputs are stale (frame {} vs {}, VS mask {:#x} vs {:#x}, PS mask {:#x} vs {:#x}, eye ({:.3f} {:.3f} {:.3f}) vs ({:.3f} {:.3f} {:.3f}), previous eye ({:.3f} {:.3f} {:.3f}) vs ({:.3f} {:.3f} {:.3f}), tables {} vs {}, lookups {} vs {}, resources {})",
			a_job == kAsyncZPrepass ? "zprepass" : "colour",
			k.frameNumber, a_actual.frameNumber, k.vsFrameMask, a_actual.vsFrameMask, k.psFrameMask, a_actual.psFrameMask,
			k.eye.x, k.eye.y, k.eye.z, a_actual.eye.x, a_actual.eye.y, a_actual.eye.z,
			k.previousEye.x, k.previousEye.y, k.previousEye.z, a_actual.previousEye.x, a_actual.previousEye.y, a_actual.previousEye.z,
			k.tablesGeneration, a_actual.tablesGeneration, k.lookupGeneration, a_actual.lookupGeneration,
			k.addresses == a_actual.addresses ? "same" : "changed");
	}

	void IndirectDraws::Impl::PackFrameBlocks(const Capture& a_capture, bool a_depthOnly, FrameBlocks& a_out)
	{
		RenderThreadBudget::Part budget(RenderThreadBudget::Bucket::Capture);
		// Per-frame constant buffers: whatever the main pass binds outside the per-draw slots.
		// On the Z-prepass the pixel-stage per-frame bindings are skipped entirely: the native depth pass
		// has not bound the main pass's yet, and the DCLF_DEPTH_ONLY build of the pixel stage compiles
		// away everything that would read them.
		auto& mirror = ConstantMirror::Get();
		// The colour epoch replays the vertex-stage bytes the Z-prepass used, so the
		// two agree to the bit and the colour pass's EQUAL test passes.
		const bool replayVertexInputs = !a_depthOnly && prepassInputs;
		if (a_depthOnly)
			for (auto& bytes : prepassVS)
				bytes.clear();
		for (std::uint32_t slot = 0; slot < kConstantBufferRegisters; ++slot) {
			if (replayVertexInputs && !((kPerDrawVS >> slot) & 1)) {
				const auto& bytes = prepassVS[slot];
				if (!bytes.empty())
					a_out.vs[slot].assign(bytes.begin(), bytes.end());
			}
			for (auto [buffers, perDraw, out] : { std::tuple{ &a_capture.vsBuffers, kPerDrawVS, &a_out.vs }, std::tuple{ &a_capture.psBuffers, kPerDrawPS, &a_out.ps } }) {
				if (a_depthOnly && out == &a_out.ps)
					continue;
				if (replayVertexInputs && out == &a_out.vs)
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

		// b12 is the game's PerFrame buffer. The constant mirror does not always hold its contents - the
		// engine does not rewrite it through the hooked context every frame - and an empty slot drops
		// every object whose shaders read it, which left distant buildings unshaded. Community Shaders
		// already caches this buffer between Map and Unmap for its own use, so take it from there.
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
		// slot would stay empty - which dropped every object whose shader reads it (the untextured
		// architecture in Dragonsreach). CS keeps the struct it uploaded, so pack that directly.
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

	MainInputs IndirectDraws::Impl::PrepareMainInputs(const Capture* a_capture, bool a_depthOnly, const Resources& a_resources, std::uint32_t a_vsMask, std::uint32_t a_psMask, const SceneStore& a_store)
	{
		const bool bindlessParity = SwitchEnabled(Switch::BindlessParity);
		MainInputs in;
		in.frameNumber = a_store.GetFrame();
		in.depthOnly = a_depthOnly;
		in.residentUploaded = a_resources.residentUploaded[a_depthOnly && a_resources.inputsDepth ? 0 : 1];
		in.tablesHeld = a_resources.scene->held;
		in.bindlessParity = bindlessParity;
		// Camera-relative world matrices: the colour epoch must use the eye the Z-prepass used, or the
		// same vertex lands somewhere else and the EQUAL test rejects it. Without a capture (the job kicked
		// ahead of the epoch) the replay is the only source, which KickColourBuild requires.
		const bool replayVertexInputs = !a_depthOnly && prepassInputs;
		if (replayVertexInputs || !a_capture) {
			in.eye = prepassEye;
			in.previousEye = prepassPreviousEye;
		} else {
			in.eye = a_capture->eye;
			in.previousEye = a_capture->previousEye;
		}
		// The records are absolute and the shaders subtract the eye, so a build reads it only for its parity checks
		// against the constant-group form, which is relative. Where it is not read it is not an
		// input: the Z-prepass job needs no eye prediction, and nothing goes stale on it.
		if (!BuildReadsEye(in))
			in.eye = in.previousEye = {};
		in.vsFrameMask = a_vsMask;
		in.psFrameMask = a_psMask;
		// The rows both segments share, and the versions of them the tables hold.
		in.addresses.records = a_resources.materialRows.address;
		in.addresses.pipelineRows = a_resources.pipelineRows.address;
		in.addresses.recordCapacity = a_resources.materialRows.capacity;
		in.addresses.pipelineCapacity = a_resources.pipelineRows.capacity;
		in.addresses.sequenceDecals = a_resources.sequenceDecals;
		in.materialRowsHeld = a_resources.materialRowsHeld;
		in.pipelineRowsHeld = a_resources.pipelineRowsHeld;
		in.addresses.frameConstants = a_resources.frameConstantsAddress;
		in.addresses.objectsIndex = a_resources.scene->objectsIndex;
		in.addresses.extrasIndex = a_resources.scene->extrasIndex;
		in.addresses.placementsIndex = FrameValues::Get().PlacementsIndex();
		in.addresses.palettesIndex = FrameValues::Get().PalettesIndex();
		in.addresses.shadingIndex = FrameValues::Get().ShadingIndex();
		in.addresses.treeWindIndex = a_resources.scene->TreeWindReadIndex(a_store.GetFrame());
		in.addresses.facePositions = FaceSnapshots::Enabled() ? a_resources.scene->facePositionsAddress : 0;
		in.addresses.fit = SceneFitOf(*a_resources.scene, a_resources.objectCapacity);
		in.addresses.identity = &a_resources;
		in.tablesGeneration = a_store.GetTablesGeneration();
		in.lookupGeneration = a_store.GetLookups().generation;
		in.materialPatchedFloats = a_store.GetMaterialPatchedFloats();
		in.materialPatchedVSFloats = a_store.GetMaterialPatchedVSFloats();
		return in;
	}
}

#endif
