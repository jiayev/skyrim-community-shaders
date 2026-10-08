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
			return;
		}
		auto capture = std::move(*impl->pending);
		impl->pending.reset();
		auto& pipelines = DrawPipelines::Get();
		auto* lighting = ConstantEvaluator::Get().GetLightingShader();
		if (failed || !lighting || !pipelines.Enabled() || !GetIndirectState().valid) {
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
			return;
		}
		MainInputs in = impl->PrepareMainInputs(&capture, depthOnly, *resources, blocks.vsMask, blocks.psMask, store);
		auto& masks = impl->epochMasks[jobIndex];
		masks.vsMask = blocks.vsMask;
		masks.psMask = blocks.psMask;
		masks.known = true;
		auto& async = stats.async[jobIndex];
		bool builtAhead = false;
		const MainPayload* committed = nullptr;

		const auto cleanup = RenderGraphRuntime::Get().Host()->ResourceCleanup();
		if (!cleanup) {
			capture.Release();
			++stats.notReady;
			return;
		}
		auto frameOwners = cleanup->Make<std::vector<std::shared_ptr<const void>>>();
		const bool ok = RenderGraphRuntime::Get().ExecuteEpoch(segment, [&](org::RenderGraph&) {
			// The lookups hold still for the whole frame (refreshed at its start alone, step 6e C).
			const auto& lookups = store.GetLookups();
			in.lookupGeneration = lookups.generation;
			// The installed publication's payload, built ahead by the coordinator (step 6e E3b): nothing is built or waited for here.
			// One it cannot commit (none built yet, other resources or frame slots) is built here with rows of its own, counted.
			std::shared_ptr<MainPayload> ahead;
			if (const auto& draws = impl->installedDraws; !draws) {
				++async.notKicked;  // no publication installed with draws
			} else if (!draws->payloads[jobIndex]) {
				++async.late;  // its builds had no context (the frame slots not known yet, other resources)
			} else if (!impl->AheadUsable(*draws->payloads[jobIndex], in, *resources)) {
				++async.stale;
				impl->LogStalePayload(jobIndex, in, draws->payloads[jobIndex]->inputs);
			} else {
				ahead = draws->payloads[jobIndex];
			}
			MainPayload* payload = ahead.get();
			if (ahead) {
				++async.used;
				builtAhead = true;
			} else {
				++async.builtInline;
				payload = &impl->fallbackPayloads[jobIndex];
				BuildMainPayload(in, tables, store.GetFrameTables(), lookups, *payload, impl->fallbackRows, nullptr, impl->StreamsNow());
				payload->foreignRows = true;
			}
			committed = payload;
			// Copied: a frame that keeps the publication commits the payload again.
			*frameOwners = payload->bindingOwners;
			if (ahead)
				frameOwners->push_back(ahead);
			impl->CommitMainPayload(capture, blocks, in, *payload, resources, store, stats, *frameOwners);
		}, frameOwners);
		impl->committedPayload[jobIndex] = committed;
		if (depthOnly) {
			const bool ring = committed && impl->RingFor(*committed, kAsyncZPrepass);
			impl->ringDepth = ring ? impl->ringFrame : Impl::RingFrame{};
			impl->ringDepth.draws.reset();
		}
		impl->committedFrame[jobIndex] = store.GetFrame();
		capture.Release();
		++stats.epochs;
		if (!committed) {
			stats.cpuMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
			return;
		}
		const MainPayload& payload = *committed;
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
			if (const auto* depth = impl->committedPayload[kAsyncZPrepass]; depth && impl->committedFrame[kAsyncZPrepass] == store.GetFrame())
				impl->CheckSetParity(resources, *depth, payload);

		stats.cpuMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
		// The remainder is the epoch execution itself and the commit. It stays a subtraction, but it is now
		// a small one rather than the bucket the per-draw work hid in. When the worker built the payload, the
		// build's parts are the worker's time and the render thread's is all remainder (the join included).
		stats.partMs[7] = builtAhead ? stats.cpuMs :
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
		UpdateShadowCapability();
		// The shadow lookups too (step 6e S1), for the capability's modes and the catalog: the builds ahead read them, and the epoch
		// refreshes them only for a view the capability did not foresee (counted).
		auto& views = impl->lastShadow;
		if (!ActiveToggles().shadows || !views.known || !globals::game::utilityShader)
			return;
		const auto& tables = store.GetTables();
		if (tables.objects.empty() || tables.shadowTechnique.size() != tables.objects.size())
			return;
		RefreshShadowLookups(store, tables, views.modes, views.rasterStates, views.dsvFormat, impl->OcclusionFormats(), store.MutableLookups());
		impl->shadowLookupsFor = views;
	}

	bool IndirectDraws::Impl::AheadUsable(const MainPayload& a_payload, const MainInputs& a_frame, const Resources& a_resources) const
	{
		// Built for these resources, this segment and these frame slots (a change of them shows a frame late: built here meanwhile).
		const auto& built = a_payload.inputs;
		// And the face positions its geometry rows name (a growth of them moves the address).
		return built.addresses.identity == &a_resources && built.addresses.facePositions == a_frame.addresses.facePositions && built.depthOnly == a_frame.depthOnly &&
		       built.vsFrameMask == a_frame.vsFrameMask &&
		       built.psFrameMask == a_frame.psFrameMask && !a_frame.bindlessParity;
	}

	void IndirectDraws::PostAheadContext()
	{
		auto& c = impl->aheadContext;
		c = {};
		if (failed || !impl->resources || !impl->resources->scene)
			return;
		auto* lighting = ConstantEvaluator::Get().GetLightingShader();
		// Under the bindless parity the builds read the eye the Z-prepass epoch captures: built at the epochs.
		if (!lighting || !DrawPipelines::Get().Enabled() || !GetIndirectState().valid || SwitchEnabled(Switch::BindlessParity)) {
			impl->PostFit();
			return;
		}
		auto& store = SceneStore::Get();
		// The capacities the builds are made against, grown for the frame's tables now (the first reserve of the frame grows).
		impl->ReserveSceneTables(store.GetTables());
		impl->ReserveMainSequences(store.GetTables());
		for (const std::size_t j : { kAsyncZPrepass, kAsyncColour }) {
			const auto& masks = impl->epochMasks[j];
			// The frame slots the epochs last supplied; the colour build replays the Z-prepass's vertex inputs.
			if (!masks.known || (j == kAsyncColour && !impl->prepassInputs))
				continue;
			c.inputs[j] = impl->PrepareMainInputs(nullptr, j == kAsyncZPrepass, *impl->resources, masks.vsMask, masks.psMask, store);
			c.build[j] = true;
		}
		c.target = impl->resources;
		// The shadow payload's (step 6e S1), for the last epoch's views; the lookups the frame's start refreshed for them.
		const auto& tables = store.GetTables();
		if (ActiveToggles().shadows && impl->shadow && impl->lastShadow.known && globals::game::utilityShader && !tables.objects.empty() &&
			tables.shadowTechnique.size() == tables.objects.size()) {
			impl->ReserveShadowRows();
			c.shadowInputs = impl->PrepareShadowInputs(store, *impl->shadow, impl->lastShadow.modes, impl->lastShadow.rasterStates);
			c.shadowTarget = impl->shadow;
			c.shadow = true;
		}
		c.valid = c.build[kAsyncZPrepass] || c.build[kAsyncColour] || c.shadow;
		impl->PostFit();
	}

	void IndirectDraws::Impl::PostFit()
	{
		// What the commit's set may claim (FitsScene): the buffers as reserved now.
		const std::array<SceneFit, 2> fit{ SceneFitOf(*resources->scene, resources->objectCapacity),
			shadow && shadow->scene ? SceneFitOf(*shadow->scene, shadow->objectCapacity) : SceneFit{} };
		const std::array<std::uint32_t, 2> rows{ static_cast<std::uint32_t>(resources->materialRows.capacity), static_cast<std::uint32_t>(resources->pipelineRows.capacity) };
		if (fit != postedFit || rows != postedRows) {
			postedFit = fit;
			postedRows = rows;
			++postedFitSerial;
		}
	}

	bool IndirectDraws::FitsScene(const void* a_tables, std::uint32_t a_slot, bool a_shadow) const
	{
		if (!impl)
			return true;
		const auto& tables = *static_cast<const SceneStore::Tables*>(a_tables);
		if (a_slot >= tables.objects.size() || !ObjectFits(tables, a_slot, impl->postedFit[a_shadow ? 1 : 0]))
			return false;
		// The main pair's rows (MainBuild::ResolvePair: a row past its table waits for its growth).
		const auto& object = tables.objects[a_slot];
		return a_shadow || (object.flags & kObjectNoBindings) || (object.materialIndex < impl->postedRows[0] && object.pipelineIndex < impl->postedRows[1]);
	}

	std::uint64_t IndirectDraws::SceneFitSerial() const
	{
		return impl ? impl->postedFitSerial : 0;
	}

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
		std::array<std::uint64_t, 2> inputs{};
		for (const std::size_t j : { kAsyncColour, kAsyncZPrepass })
			if (const auto& payload = draws->payloads[j]) {
				geometryRows = std::max<std::uint64_t>(geometryRows, payload->geometryDraws.Count());
				inputs[j] = payload->resident.Count() + payload->inputList.size();
			}
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
				SceneStore::Get().RetireImport(std::move(a_part.buffer));
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
		for (const std::size_t j : { kAsyncColour, kAsyncZPrepass })
			ensure(entry.inputs[j], inputs[j] * sizeof(DrawInput) / 4, 4, j == kAsyncZPrepass ? "inputs-depth" : "inputs", false);
		if (shadow) {
			ensure(entry.shadowRows, shadow->materialRows.Count(), sizeof(ShadowMaterialRow), "shadow-rows", true);
			for (std::uint32_t m = 0; m < kShadowModeCount; ++m)
				if (shadow->inputs.modeUsed[m])
					ensure(entry.shadowInputs[m], shadow->ModeInputs(m) * sizeof(DrawInput) / 4, 4, fmt::format("shadow-inputs-{}", m).c_str(), false);
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
		for (const std::size_t j : { kAsyncColour, kAsyncZPrepass })
			frame.inputsIndex[j] = entry.inputs[j].srvIndex;
		if (shadow) {
			frame.shadow = true;
			frame.shadowRows = entry.shadowRows.address;
			for (std::uint32_t m = 0; m < kShadowModeCount; ++m)
				frame.shadowInputsIndex[m] = entry.shadowInputs[m].srvIndex;
		}
		frame.materialRows = entry.materialRows.address;
		frame.pipelineRows = entry.pipelineRows.address;
		// The producer's part: the entry brought up to the publication, once the frame that last read it is done on the GPU.
		return [&s, draws = std::move(draws), &entry, r, reuse = entry.reuse](org::runtime::IUploadService& a_uploads) {
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
			EmitExtras(views.extras, entry.extras.held, nullptr, sender(entry.extras, Impl::kRingExtras));
			entry.extras.held = views.extras.Version();
			holders.extras.Set(r, entry.extras.held);
			const MainPayload* newest = draws->payloads[kAsyncColour] ? draws->payloads[kAsyncColour].get() : draws->payloads[kAsyncZPrepass].get();
			const ShadowPayload* shadow = draws->shadow.get();
			const auto& geometries = newest ? newest->geometryDraws : shadow->geometries;
			EmitGeometryDraws(geometries, entry.geometries.held, sender(entry.geometries, Impl::kRingGeometries));
			entry.geometries.held = geometries.Version();
			holders.geometries.Set(r, entry.geometries.held);
			// The shadow payload's rows and each used mode's inputs (step 6e S2).
			if (shadow) {
				shadow->materialRows.Emit(entry.shadowRows.held, sender(entry.shadowRows, Impl::kRingShadowRows));
				entry.shadowRows.held = shadow->materialRows.Version();
				holders.shadowRows.Set(r, entry.shadowRows.held);
				for (std::uint32_t m = 0; m < kShadowModeCount; ++m) {
					if (!shadow->inputs.modeUsed[m])
						continue;
					auto& part = entry.shadowInputs[m];
					EmitShadowInputs(*shadow, m, part.held, sender(part, Impl::kRingShadowInputs));
					part.held = shadow->regionInputs[m].Version();
					holders.shadowInputs[m].Set(r, part.held);
				}
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
			// Each segment's inputs: its resident region at the head (what changed since), the build's own after it (whole).
			for (const std::size_t j : { kAsyncColour, kAsyncZPrepass }) {
				const auto& payload = draws->payloads[j];
				if (!payload)
					continue;
				auto& part = entry.inputs[j];
				payload->resident.Emit(part.held, sender(part, Impl::kRingResident));
				part.held = payload->resident.Version();
				holders.resident[j].Set(r, part.held);
				if (!payload->inputList.empty())
					sender(part, Impl::kRingFrameInputs)(payload->inputList.data(), payload->inputList.size() * sizeof(DrawInput), payload->resident.Count() * sizeof(DrawInput));
			}
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

	void IndirectDraws::InstallDraws(std::shared_ptr<const void> a_draws)
	{
		if (!impl)
			return;
		const auto slot = std::static_pointer_cast<const Impl::AheadSlot>(std::move(a_draws));
		impl->installedDraws = slot && slot->done.load(std::memory_order_acquire) ? slot->result : nullptr;
	}

	bool IndirectDraws::DrawsReady(const std::shared_ptr<const void>& a_draws) const
	{
		// A publication without draws (DCLF failed, none asked for) draws nothing of DCLF's: nothing to wait for.
		const auto* slot = static_cast<const Impl::AheadSlot*>(a_draws.get());
		return !slot || slot->done.load(std::memory_order_acquire);
	}

	std::shared_ptr<const void> IndirectDraws::BuildAhead(std::shared_ptr<const void> a_tables)
	{
		if (!impl || failed || !a_tables)
			return nullptr;
		auto& s = *impl;
		auto& store = SceneStore::Get();
		auto slot = std::make_shared<Impl::AheadSlot>();
		const std::uint64_t seq = ++s.aheadKicked;
		// What the task reads that the coordinator or the frame write later is copied into it now: the context, the lookups.
		auto job = [&s, slot, seq, tables = std::static_pointer_cast<const SceneStore::Tables>(std::move(a_tables)), context = s.aheadContext,
						lookups = store.SharedLookups() ? store.SharedLookups() : std::make_shared<const Lookups>(store.CoordinatorLookups()), generation = store.GetTablesGeneration(),
						frame = store.GetFrame(), sun = store.CoordinatorSunCandidates(), light = store.CoordinatorLightCandidates()](const auto&) mutable {
			// One at a time, in publication order: each builds on the stores and rows the one before left.
			for (auto finished = s.aheadDone.load(std::memory_order_acquire); finished + 1 < seq; finished = s.aheadDone.load(std::memory_order_acquire))
				s.aheadDone.wait(finished, std::memory_order_acquire);
			try {
				slot->result = s.RunAhead(std::move(tables), context, *lookups, generation, frame, std::move(sun), std::move(light));
			} catch (const std::exception& e) {
				static std::atomic<std::uint32_t> logged{ 0 };
				if (logged++ < 4)
					logger::error("[DCLF] builds ahead {} failed: {}; its frames draw what the epochs build", seq, e.what());
			}
			slot->done.store(true, std::memory_order_release);
			s.aheadDone.store(seq, std::memory_order_release);
			s.aheadDone.notify_all();
		};
		if (!SceneScheduler::Executor().Dispatch(SceneScheduler::Scope(), PublishedSceneExecutor::Preparation, org::async::TaskDispatch::Cpu, "builds ahead", std::move(job)))
			stl::report_and_fail("Drawcall Limit Fix: the builds ahead were refused by DCLF's executor");
		return slot;
	}

	void IndirectDraws::Impl::WaitAhead()
	{
		for (auto finished = aheadDone.load(std::memory_order_acquire); finished < aheadKicked; finished = aheadDone.load(std::memory_order_acquire))
			aheadDone.wait(finished, std::memory_order_acquire);
	}

	std::shared_ptr<const IndirectDraws::Impl::DrawPublication> IndirectDraws::Impl::RunAhead(std::shared_ptr<const SceneStore::Tables> a_tables,
		const AheadContext& a_context, const Lookups& a_lookups, std::uint32_t a_generation, std::uint32_t a_frame,
		std::shared_ptr<const SunCandidates> a_sunCandidates, std::shared_ptr<const SunCandidates> a_lightCandidates)
	{
		ZoneScopedN("CS.DCLF.BuildAhead");
		auto& store = SceneStore::Get();
		auto out = std::make_shared<DrawPublication>();
		out->tables = a_tables;
		const auto& holders = ringHolders;
		// The publication's stream views: the stores are this task's alone (no build in the frame updates them). Their journals keep
		// what the oldest ring entry lacks.
		const TablesHeld from{ holders.objects.Oldest(), holders.extras.Oldest(), holders.geometries.Oldest() };
		out->streams = MakeStreamViews(objectStore, extrasStore, geometryStore, from, a_tables, *a_tables, a_generation, a_frame);
		// CS_DCLF_PERSISTENT_PARITY: the extras rows against the tables' (the commits that read the ring send none, step 6e S3).
		if (PersistentParityEnabled() && ParityDue(a_frame))
			CheckExtras(extrasStore, out->streams->extras);
		const auto& c = a_context;
		if (!c.valid || !c.target || !c.target->scene)
			return out;
		// The Z-prepass's first, as its commit.
		for (const std::size_t j : { kAsyncZPrepass, kAsyncColour }) {
			if (!c.build[j])
				continue;
			MainInputs in = c.inputs[j];
			in.frameNumber = a_frame;
			in.tablesGeneration = a_generation;
			in.lookupGeneration = a_lookups.generation;
			// The oldest versions the ring's entries hold: what the rows' and the region's journals keep changes back to.
			in.materialRowsHeld = holders.materialRows.Oldest();
			in.pipelineRowsHeld = holders.pipelineRows.Oldest();
			in.residentUploaded = holders.resident[j].Oldest();
			in.tablesHeld = from;
			auto payload = AcquirePayload();
			BuildMainPayload(in, *a_tables, store.GetFrameTables(), a_lookups, *payload, mainRows, CacheFor(j), out->streams);
			out->payloads[j] = std::move(payload);
		}
		// The shadow payload (step 6e S1), from the same tables, stream views and lookups, with the publication's candidates, and the
		// next frame's exclusions from it.
		if (c.shadow && c.shadowTarget && c.shadowTarget->scene) {
			ShadowInputs in = c.shadowInputs;
			in.frameNumber = a_frame;
			in.tablesGeneration = a_generation;
			in.lookupGeneration = a_lookups.shadowGeneration;
			in.sunCandidates = a_sunCandidates;
			in.lightCandidates = a_lightCandidates;
			in.tablesHeld = from;
			// The oldest versions the ring's entries hold: what the shadow journals keep changes back to (step 6e S2).
			in.materialRowsHeld = ringHolders.shadowRows.Oldest();
			for (std::uint32_t m = 0; m < kShadowModeCount; ++m)
				in.inputsHeld[m] = ringHolders.shadowInputs[m].Oldest();
			auto payload = AcquireShadowPayload();
			BuildShadowPayload(in, *a_tables, a_lookups, *payload, out->streams, ShadowKeptState());
			// The publication holds the views; the payload, which the epochs may hold past it, does not.
			payload->streams.reset();
			if (PassCapture::ShadowWithholdingEnabled()) {
				ZoneScopedN("CS.DCLF.BuildShadow.Exclusions");
				if (in.modeUsed[kSunShadowMode])
					payload->sunExclusion = BuildSunExclusion(in.sunCandidates, *payload, kSunShadowMode, *a_tables, &sunExclusionCache);
				if (in.modeUsed[kParabolicShadowMode])
					payload->parabolicExclusion = BuildSunExclusion(in.lightCandidates, *payload, kParabolicShadowMode, *a_tables, &parabolicExclusionCache);
			}
			out->shadow = std::move(payload);
		}
		return out;
	}

	std::uint64_t IndirectDraws::TakeStreamsRefused()
	{
		return impl ? std::exchange(impl->streamsRefused, 0) : 0;
	}

	void IndirectDraws::EndFrame()
	{
		// The epochs' own builds hold the frame's stream views (and through them its publication's retirement node): let go.
		for (auto& payload : impl->fallbackPayloads)
			payload.Reset();
		// The ring entry the frame read: free again once what the frame submitted is done on the GPU.
		if (impl->ringFrame.valid)
			if (auto* host = RenderGraphRuntime::Get().Host())
				impl->payloadRing[impl->ringFrame.entry].reuse = host->SubmittedPoint();
		impl->ringFrame.draws.reset();
		impl->ringShadow = {};
		impl->shadowFallback.streams.reset();
		AsyncWorker::Get().NoteFrame();
		// Views made for the frame's tables hold its publication: let go.
		impl->streamViews.reset();
	}

	void IndirectDraws::DrainAsync()
	{
		impl->WaitAhead();
		impl->WaitFadeWriteBack();
		impl->streamViews.reset();
		AsyncWorker::Get().Drain();
	}

	void IndirectDraws::Impl::LogStalePayload(std::size_t a_job, const MainInputs& a_actual, const MainInputs& a_built)
	{
		if (epochMasks[a_job].loggedStale++ >= 4)
			return;
		const auto& k = a_built;
		logger::info("[DCLF] async {}: the installed payload's inputs are stale (frame {} vs {}, VS mask {:#x} vs {:#x}, PS mask {:#x} vs {:#x}, eye ({:.3f} {:.3f} {:.3f}) vs ({:.3f} {:.3f} {:.3f}), previous eye ({:.3f} {:.3f} {:.3f}) vs ({:.3f} {:.3f} {:.3f}), tables {} vs {}, lookups {} vs {}, resources {})",
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
		// Step 6e E5: a slot the epoch supplied before and lacks now - a feature's block the capture point sees bound in some frames
		// only (VS b7) - is supplied from its last capture, counted, so the frame's slots are the same every frame and the builds
		// ahead, made for the slots the epoch last supplied, stay usable (AheadUsable). The Z-prepass's carried vertex blocks are
		// the colour epoch's replay too: the two transform alike.
		auto& carry = carriedBlocks[a_depthOnly ? 0 : 1];
		for (std::uint32_t slot = 0; slot < kConstantBufferRegisters; ++slot) {
			for (auto [out, kept, vertex] : { std::tuple{ &a_out.vs[slot], &carry.vs[slot], true }, std::tuple{ &a_out.ps[slot], &carry.ps[slot], false } }) {
				if (!out->empty()) {
					kept->assign(out->begin(), out->end());
					continue;
				}
				if (kept->empty() || (vertex && replayVertexInputs) || (!vertex && a_depthOnly))
					continue;
				out->assign(kept->begin(), kept->end());
				++frameSlotsCarried;
				if (const std::uint32_t bit = 1u << (slot + (vertex ? 0 : 16) + (a_depthOnly ? 0 : 8)); !(carriedLogged & bit)) {
					carriedLogged |= bit;
					logger::info("[DCLF] {} epoch, frame {}: {} b{} not bound at the capture, supplied from an earlier one ({} bytes)", a_depthOnly ? "zprepass" : "colour",
						SceneStore::Get().GetFrame(), vertex ? "VS" : "PS", slot, out->size());
				}
				if (vertex && a_depthOnly)
					prepassVS[slot] = *out;
			}
		}
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
