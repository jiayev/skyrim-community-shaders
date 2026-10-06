#if defined(CS_HAS_RENDER_GRAPH) && defined(CS_HAS_ORG_MODULE_SERVICES)
#	include "Internal.h"

namespace DCLF
{
	namespace
	{
		// The payload's own uploads: the rows, the per-object records and bone rows, and the draw inputs
		// with their geometry. Each buffer has its own condition: a depth epoch where every candidate is cull-only has plenty
		// of inputs, and BuildDraws dispatches over the inputs.
		template <class Emit>
		void ForEachMainPayloadUpload(const MainPayload& a_payload, const Resources& a_resources, Emit&& a_emit)
		{
			{
				ZoneScopedN("CS.DCLF.UploadRanges.Rows");
				// The rows the tables do not hold, their headers' addresses made absolute (both segments share them).
				EmitMainRows(a_payload.materialRows, a_payload.inputs.materialRowsHeld, a_resources.materialRows.address, [](MaterialRow& a_row, std::uint64_t a_address) {
					PatchRowAddresses(a_row, a_address);
				}, [&](const void* a_data, std::size_t a_bytes, std::size_t a_offset) { a_emit(a_resources.materialRows.buffer, a_data, a_bytes, a_offset); });
				EmitMainRows(a_payload.pipelineRows, a_payload.inputs.pipelineRowsHeld, a_resources.pipelineRows.address, [](PipelineRow& a_row, std::uint64_t a_address) {
					PatchRowAddresses(a_row, a_address);
				}, [&](const void* a_data, std::size_t a_bytes, std::size_t a_offset) { a_emit(a_resources.pipelineRows.buffer, a_data, a_bytes, a_offset); });
			}
			// Not the object records or the palettes: the commit's (CommitSceneStreams).
			const auto& scene = *a_resources.scene;
			// The segment's input buffer: the resident region at its head when the buffer does not hold this version of it,
			// then the frame's own inputs after it.
			const bool depth = a_payload.inputs.depthOnly && a_resources.inputsDepth;
			const auto& inputs = depth ? a_resources.inputsDepth : a_resources.inputs;
			const std::size_t regionCount = a_payload.resident.Count();
			TracyCZoneN(residentUploadZone, "CS.DCLF.UploadRanges.Resident", true);
			a_payload.resident.Emit(a_resources.residentUploaded[depth ? 0 : 1],
				[&](const void* a_data, std::size_t a_bytes, std::size_t a_offset) { a_emit(inputs, a_data, a_bytes, a_offset); });
			if (!a_payload.inputList.empty())
				a_emit(inputs, a_payload.inputList.data(), a_payload.inputList.size() * sizeof(DrawInput), regionCount * sizeof(DrawInput));
			TracyCZoneEnd(residentUploadZone);
			EmitGeometryDraws(a_payload.geometryDraws, scene.held.geometries, [&](const void* a_data, std::size_t a_bytes, std::size_t a_offset) {
				a_emit(scene.geometries->Get(), a_data, a_bytes, a_offset);
			});
		}

		// Render thread: the payload through the commit's uploads, copied now.
		void UploadMainPayload(const MainPayload& a_payload, const Resources& a_resources, CommitUploads& a_uploads)
		{
			ForEachMainPayloadUpload(a_payload, a_resources, [&](const auto& a_target, const void* a_data, std::size_t a_bytes, std::size_t a_offset) {
				a_uploads(a_target, a_data, a_bytes, a_offset);
			});
		}

		/** @brief The rows' backings a staged batch was staged against: one of them grown since, and it is not submitted. */
		std::uint64_t RowsGeneration(const Resources& a_resources)
		{
			return (a_resources.materialRows.generation << 32) ^ a_resources.pipelineRows.generation;
		}
	}

	// On the worker, after the build: the payload's uploads into a staged batch (a released one from the job's
	// pool, or a new one), so the commit copies nothing.
	void StageMainPayload(MainPayload& a_payload, const Resources& a_resources, std::vector<std::shared_ptr<org::runtime::StagedUploadBatch>>& a_pool,
		rhi::Device a_device)
	{
		ZoneScopedN("CS.DCLF.StageMainPayload");
		auto batch = AcquireStagedBatch(a_pool);
		ForEachMainPayloadUpload(a_payload, a_resources, [&](const auto& a_target, const void* a_data, std::size_t a_bytes, std::size_t a_offset) {
			batch->Stage(org::runtime::UploadTarget::FromShared(Target(a_target)), a_offset, a_data, a_bytes);
		});
		a_payload.stagedFor = &a_resources;
		a_payload.stagedRowsGeneration = RowsGeneration(a_resources);
		a_payload.stagedSceneGeneration = a_resources.scene->generation;
		if (a_device)
			batch->Record(a_device);
		a_payload.staged = std::move(batch);
	}

	// On the worker, after the shadow build: what the commit would upload that does not depend on the views it
	// captures - the shared tables, the material rows, the used modes' inputs, the arena (the frame record and the blocks). The
	// views' blocks and counters are the epoch's latched copies (ShadowLatchedCopiesPass).
	void StageShadowPayload(ShadowPayload& a_payload, const ShadowResources& a_resources, std::vector<std::shared_ptr<org::runtime::StagedUploadBatch>>& a_pool,
		rhi::Device a_device)
	{
		ZoneScopedN("CS.DCLF.StageShadowPayload");
		using org::runtime::UploadTarget;
		auto batch = AcquireStagedBatch(a_pool);
		// Against the versions the buffers held when the build's inputs were taken: no commit runs between then and this
		// one's (frame order), and one that did would only have sent a subset of this.
		const auto& scene = *a_resources.scene;
		const TablesHeld& held = a_payload.inputs.tablesHeld;
		EmitGeometryDraws(a_payload.geometries, held.geometries, [&](const void* a_data, std::size_t a_bytes, std::size_t a_offset) {
			batch->Stage(UploadTarget::FromShared(scene.geometries->Get()), a_offset, a_data, a_bytes);
		});
		for (std::uint32_t m = 0; m < kShadowModeCount; ++m) {
			if (!a_payload.inputs.modeUsed[m])
				continue;
			EmitShadowInputs(a_payload, m, a_payload.kept ? a_resources.inputsUploaded[m] : 0, [&](const void* a_data, std::size_t a_bytes, std::size_t a_offset) {
				batch->Stage(UploadTarget::FromShared(Target(a_resources.inputs[m])), a_offset, a_data, a_bytes);
			});
		}
		// The material rows the table does not hold (all of them without the kept state, or in a new backing).
		a_payload.materialRows.Emit(a_payload.kept ? a_payload.inputs.materialRowsHeld : 0, [&](const void* a_data, std::size_t a_bytes, std::size_t a_offset) {
			batch->Stage(UploadTarget::FromShared(Target(a_resources.materialRows.buffer)), a_offset, a_data, a_bytes);
		});
		if (const auto& bytes = a_payload.arena.Bytes(); !bytes.empty())
			batch->Stage(UploadTarget::FromShared(a_resources.constants), 0, bytes.data(), bytes.size());
		a_payload.stagedFor = &a_resources;
		if (a_device)
			batch->Record(a_device);
		a_payload.staged = std::move(batch);
	}

	void IndirectDraws::Impl::KickSceneStreams()
	{
		ZoneScopedN("CS.DCLF.KickSceneStreams");
		DropSceneStreams();
		// The persistent parity checks what this thread uploads; the job's batch would bypass it.
		if (!AsyncEnabled() || PersistentParityEnabled() || !resources || !resources->scene)
			return;
		auto& store = SceneStore::Get();
		const auto& tables = store.GetTables();
		if (tables.objects.empty())
			return;
		// The capacities the job stages against, grown now (the first reserve of the frame does the growing).
		ReserveSceneTables(tables);
		auto& job = streamsJob;
		const auto& sceneBuffers = *resources->scene;
		job.scene = &sceneBuffers;
		job.from = sceneBuffers.held;
		job.tablesGeneration = store.GetTablesGeneration();
		job.sceneGeneration = sceneBuffers.generation;
		job.objects = job.bones = 0;
		job.staged = false;
		job.batch = AcquireStagedBatch(job.pool);
		++job.kicked;
		const rhi::Device device = RecordingDevice();
		job.handle = AsyncWorker::Get().Submit("streams", [result = &job, batch = job.batch, objectsBuffer = sceneBuffers.objects->Get(), bonesBuffer = sceneBuffers.bones->Get(),
															objectCapacity = sceneBuffers.objectCapacity, boneRows = sceneBuffers.boneRows, tables = &tables, objects = SceneObjects(),
															bonesStore = SceneBones(), from = job.from, generation = job.tablesGeneration, frame = store.GetFrame(), device](std::stop_token) {
			ZoneScopedN("CS.DCLF.StageSceneStreams");
			using org::runtime::UploadTarget;
			ObjectRecordsOut records;
			UpdateObjectRecords(objects, from.objects, *tables, generation, frame, records);
			BonesOut bones;
			UpdateBones(bonesStore, from.bones, *tables, generation, bones);
			// Past a buffer: the commit's own update reports it (CheckSceneCapacity).
			if (records.Count() > objectCapacity || bones.Rows() > boneRows)
				return;
			records.Emit(from.objects, [&](const void* a_data, std::size_t a_bytes, std::size_t a_offset) {
				batch->Stage(UploadTarget::FromShared(objectsBuffer), a_offset, a_data, a_bytes);
			});
			EmitBones(bones, from.bones, nullptr, [&](const void* a_data, std::size_t a_bytes, std::size_t a_offset) {
				batch->Stage(UploadTarget::FromShared(bonesBuffer), a_offset, a_data, a_bytes);
			});
			if (device && !batch->Entries().empty())
				batch->Record(device);
			result->objects = records.Version();
			result->bones = bones.Version();
			result->staged = true;
		});
	}

	void IndirectDraws::Impl::DropSceneStreams()
	{
		auto& job = streamsJob;
		if (job.handle) {
			AsyncWorker::Get().Cancel(job.handle);
			job.handle = {};
			++job.dropped;
		}
		job.batch.reset();
		job.staged = false;
	}

	IndirectDraws::Impl::SceneStreams IndirectDraws::Impl::CommitSceneStreams(SceneBuffers& a_scene, const SceneStore::Tables& a_tables, std::uint32_t a_frame,
		std::uint32_t a_generation, CommitUploads& a_uploads)
	{
		ZoneScopedN("CS.DCLF.CommitSceneStreams");
		// The streams' job, if one is out: its batch first (ahead of this commit's own, submitted when it ends), when the buffers
		// still hold what it started from.
		if (auto& job = streamsJob; job.handle) {
			ZoneScopedN("CS.DCLF.CommitSceneStreams.JoinJob");
			const auto joined = JoinJob(job.handle);
			job.handle = {};
			if (joined == AsyncWorker::WaitResult::Done && job.staged && job.scene == &a_scene && a_scene.held.objects == job.from.objects &&
				a_scene.held.bones == job.from.bones && job.tablesGeneration == a_generation && job.sceneGeneration == a_scene.generation) {
				if (!job.batch->Entries().empty())
					SubmitWorkerBatch(std::move(job.batch));
				if (job.objects)
					a_scene.held.objects = job.objects;
				if (job.bones)
					a_scene.held.bones = job.bones;
				++job.used;
			} else {
				++job.dropped;
			}
			job.batch.reset();
			job.staged = false;
		}
		SceneStreams sent;
		ObjectRecordsOut objects;
		UpdateObjectRecords(SceneObjects(), a_scene.held.objects, a_tables, a_generation, a_frame, objects);
		sent.objects = objects.Count();
		CheckSceneCapacity(a_scene, objects.Count(), 0, 0, 0, ~0u, "the object records");
		sent.objectBytes = objects.Emit(a_scene.held.objects, [&](const void* a_data, std::size_t a_bytes, std::size_t a_offset) {
			a_uploads(a_scene.objects, a_data, a_bytes, a_offset);
		});
		if (objects.Version())
			a_scene.held.objects = objects.Version();
		BonesOut bones;
		UpdateBones(SceneBones(), a_scene.held.bones, a_tables, a_generation, bones);
		sent.boneRows = bones.Rows();
		CheckSceneCapacity(a_scene, 0, 0, bones.Rows(), 0, ~0u, "the bone rows");
		BonesStore* bonesParity = PersistentParityEnabled() ? &boneStore : nullptr;
		sent.boneRowsSent = EmitBones(bones, a_scene.held.bones, bonesParity, [&](const void* a_data, std::size_t a_bytes, std::size_t a_offset) {
			a_uploads(a_scene.bones, a_data, a_bytes, a_offset);
		});
		if (bonesParity && ParityDue(a_frame))
			CheckBones(*bonesParity, bones);
		if (bones.Version())
			a_scene.held.bones = bones.Version();
		boneStore.rowsSent += sent.boneRowsSent;
		return sent;
	}

	bool IndirectDraws::Impl::CommitMainPayload(const Capture& a_capture, const FrameBlocks& a_blocks, MainPayload& a_payload,
		const std::shared_ptr<Resources>& a_resources, SceneStore& a_store, IndirectDraws::Stats& a_stats,
		std::vector<std::shared_ptr<const void>>& a_bindingOwners)
	{
		ZoneScopedN("CS.DCLF.CommitMainPayload");
		// The cleared geometry slots' buffers, held until this execution retires (SceneStore::TakeRetiredImports).
		for (auto& owner : a_store.TakeRetiredImports())
			a_bindingOwners.push_back(std::move(owner));
		const auto& in = a_payload.inputs;
		const auto& tables = a_store.GetTables();
		const bool depthOnly = in.depthOnly;
		const std::uint32_t frameNumber = in.frameNumber;
		auto& textures = GpuTextures::Get();
		auto& mirror = ConstantMirror::Get();
		const bool replayVertexInputs = !depthOnly && prepassInputs;
		CommitUploads uploads(commitStagedPool);
		// What the segment's shape is made of that this commit fixes before writing anything (MakeMainShape): the pipeline set,
		// the Z-prepass's buckets (PlanZBuckets) and the latched copies' layout (MainLatchedLayout), its block reserved to hold it.
		const std::size_t latchedShape = a_payload.inputs.depthOnly ? kDepthShape : kColourShape;
		const auto indirect = GetIndirectState();
		auto& zPlan = zBucketPlan;
		zPlan = {};
		if (depthOnly && a_resources->pool)
			PlanZBuckets(*a_resources, a_store.GetLookups(), tables, indirect, zPlan);
		FrameBlockSizes blockSizes;
		for (std::uint32_t slot = 0; slot < kConstantBufferRegisters; ++slot) {
			blockSizes.vs[slot] = static_cast<std::uint32_t>(a_blocks.vs[slot].size());
			blockSizes.ps[slot] = static_cast<std::uint32_t>(a_blocks.ps[slot].size());
		}
		const auto latchedLayout = MainLatchedLayout(*a_resources, depthOnly, blockSizes, zPlan.Buckets());
		ReserveLatchedBlock(a_resources->latchedBlocks[latchedShape], LatchedBytes(latchedLayout), RenderGraphRuntime::Get().Host()->FrameSlots());
		shapeParity.blockSizes[latchedShape] = blockSizes;
		// The per-frame values (the frame constants, the counters, the frame buffers' copies, tree LOD's row): copied from the
		// latch by the epoch's first pass, so this commit records no copy for them.
		LatchedUploads latched(a_resources->latchedTargets, uploads, a_resources->latchedBlocks[latchedShape], RenderGraphRuntime::Get().Host()->CurrentFrameSlot(),
			RenderGraphRuntime::Get().Host()->FrameSlots());
		auto lap = [&, last = std::chrono::steady_clock::now()](std::size_t a_part) mutable {
			const auto now = std::chrono::steady_clock::now();
			a_stats.commitUs[a_part] += std::chrono::duration<double, std::micro>(now - last).count();
			last = now;
		};
		// Nothing to draw until this commit publishes the segment's shape again (a failed commit leaves it so).
		const std::size_t shapeIndex = depthOnly ? kDepthShape : kColourShape;
		a_resources->frames[shapeIndex].store(nullptr, std::memory_order_release);

		// The frame's textures (t16 and up), resolved now into the frame record.
		std::array<std::uint32_t, kTextureRegisters> frameTextures;
		frameTextures.fill(kInvalidIndex);
		if (frameTextureGeneration != textures.Generation()) {
			frameTextureBindings = {};
			frameTextureGeneration = textures.Generation();
		}
		for (std::uint32_t t = kPixelTextureSlots; t < kTextureRegisters && !depthOnly; ++t) {
			auto& held = frameTextureBindings[t];
			// The character light's noise is the frame's view (Tables::characterLightView), not a captured register.
			auto* const view = t == kCharacterLightRegister ? tables.characterLightView : a_capture.psViews[t];
			if (!view) {
				if (held.view)
					held = {};
				frameTextures[t] = textures.NullIndex();
				continue;  // the shared lookup root already owns the null descriptor
			}
			if (held.view != view || !held.binding.owner || held.binding.index == GpuTextures::kInvalid) {
				held.view = view;
				held.binding = textures.ResolveBinding(held.view, 64 + t);
			}
			frameTextures[t] = held.binding.index;
			if (held.binding.owner)
				a_bindingOwners.push_back(held.binding.owner);
		}
		if (a_resources->lightLimitFix && !depthOnly)
			ORGLightCulling::Get().GetShaderResourceIndices(frameTextures[kLightsRegister], frameTextures[kLightsRegister + 1], frameTextures[kLightsRegister + 2]);
		for (const auto& frameBuffer : depthOnly ? decltype(a_resources->frameBuffers){} : a_resources->frameBuffers) {
			auto* buffer = BufferOf(a_capture.psViews[frameBuffer.textureRegister]);
			mirror.Watch(buffer);
			const auto contents = mirror.Contents(buffer);
			const std::size_t offset = std::size_t(frameBuffer.firstElement) * frameBuffer.stride;
			const std::size_t bytes = std::size_t(frameBuffer.elements) * frameBuffer.stride;
			if (contents.size() < offset + bytes) {
				// A structured buffer the commit cannot fill: its copy reads zero (written, as the shape's latched copies name it).
				static std::array<std::uint32_t, kTextureRegisters> unfilled{};
				if ((unfilled[frameBuffer.textureRegister]++ % 600) == 0)
					logger::warn("[DCLF] frame buffer t{} ({} elements of {} bytes) not filled: the mirror holds {} bytes of it ({} times)", frameBuffer.textureRegister,
						frameBuffer.elements, frameBuffer.stride, contents.size(), unfilled[frameBuffer.textureRegister]);
				static std::vector<std::byte> zeros;
				if (zeros.size() < bytes)
					zeros.resize(bytes);
				latched(frameBuffer.copy, zeros.data(), bytes, 0);
				continue;  // not written since it is watched
			}
			latched(frameBuffer.copy, contents.data() + offset, bytes, 0);
			frameTextures[frameBuffer.textureRegister] = frameBuffer.copy->GetSRVInfo(0).slot.index;
		}
		// The frame textures the drawn pipelines read but the commit could not resolve: they read zero, as an unbound view does
		// natively; counted, because the build could not skip the draws for them. A view bound natively that could not be
		// resolved (a buffer that is not a CPU-written structured buffer) lands here too, and reads zero where the native draw
		// reads the resource.
		std::uint32_t frameTexturesMissing = 0;
		std::array<std::uint64_t, 2> missingRegisters{};
		for (std::uint32_t t = kPixelTextureSlots; t < kTextureRegisters; ++t) {
			if (!((a_payload.frameRegisters[t / 64] >> (t % 64)) & 1) || frameTextures[t] != kInvalidIndex)
				continue;
			++frameTexturesMissing;
			missingRegisters[t / 64] |= 1ull << (t % 64);
			static std::array<bool, kTextureRegisters> described{};
			if (!std::exchange(described[t], true)) {
				if (auto* view = a_capture.psViews[t]) {
					D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{};
					view->GetDesc(&viewDesc);
					winrt::com_ptr<ID3D11Resource> resource;
					view->GetResource(resource.put());
					std::string what = fmt::format("view dimension {}, format {}", static_cast<std::uint32_t>(viewDesc.ViewDimension), static_cast<std::uint32_t>(viewDesc.Format));
					if (auto buffer = resource.try_as<ID3D11Buffer>()) {
						D3D11_BUFFER_DESC bufferDesc{};
						buffer->GetDesc(&bufferDesc);
						what += fmt::format(", buffer of {} bytes, stride {}, usage {}, CPU access {:X}, bind {:X}, misc {:X}", bufferDesc.ByteWidth, bufferDesc.StructureByteStride,
							static_cast<std::uint32_t>(bufferDesc.Usage), bufferDesc.CPUAccessFlags, bufferDesc.BindFlags, bufferDesc.MiscFlags);
					}
					char name[128]{};
					UINT size = sizeof(name) - 1;
					if (SUCCEEDED(resource->GetPrivateData(WKPDID_D3DDebugObjectName, &size, name)))
						what += fmt::format(", '{}'", name);
					logger::warn("[DCLF] frame texture t{} is bound natively but not resolved: {}", t, what);
				} else {
					logger::warn("[DCLF] frame texture t{} is read but nothing is bound there at the capture", t);
				}
			}
		}
		a_stats.frameTexturesMissing = frameTexturesMissing;
		a_stats.frameTexturesMissingRegisters = missingRegisters;
		// The frame record: every register a draw's rows do not give (DrawPipelines.h) - the frame slots, the frame's textures,
		// the object and bone tables.
		{
			DrawBindings frameRecord{};
			const std::uint64_t frameConstants = a_resources->frameConstantsAddress;
			for (std::uint32_t r = 0; r < kConstantBufferRegisters; ++r) {
				frameRecord.vertexConstants[r] = ((in.vsFrameMask >> r) & 1) ? frameConstants + FrameSlotOffset(false, r) : 0;
				frameRecord.pixelConstants[r] = ((in.psFrameMask >> r) & 1) ? frameConstants + FrameSlotOffset(true, r) : 0;
			}
			frameRecord.pixelConstants[3] = frameConstants + std::uint64_t(kFrameSlotSharedLight) * kFrameSlotBytes;
			frameRecord.pixelConstants[kFrameLightingRegister] = frameConstants + std::uint64_t(kFrameSlotLighting) * kFrameSlotBytes;
			const std::uint32_t nullIndex = textures.NullIndex() != kInvalidIndex ? textures.NullIndex() : 0u;
			for (std::uint32_t t = 0; t < kTextureRegisters; ++t)
				frameRecord.textures[t] = frameTextures[t] == kInvalidIndex ? nullIndex : frameTextures[t];
			frameRecord.textures[kObjectBufferRegister] = in.addresses.objectsIndex;
			frameRecord.textures[kBonesBufferRegister] = in.addresses.bonesIndex;
			frameRecord.textures[kTreeWindRegister] = in.addresses.treeWindIndex;
			latched(a_resources->frameConstants, &frameRecord, sizeof(frameRecord), std::uint64_t(kFrameSlotRecord) * kFrameSlotBytes);
		}
		lap(2);

		// The frame slots: each block into its slot, and the zeroed light block every draw's b3 reads.
		for (std::uint32_t slot = 0; slot < kConstantBufferRegisters; ++slot) {
			if (!a_blocks.vs[slot].empty())
				latched(a_resources->frameConstants, a_blocks.vs[slot].data(), a_blocks.vs[slot].size(), FrameSlotOffset(false, slot));
			if (!a_blocks.ps[slot].empty())
				latched(a_resources->frameConstants, a_blocks.ps[slot].data(), a_blocks.ps[slot].size(), FrameSlotOffset(true, slot));
		}
		{
			static const std::array<std::uint32_t, kStrictLightDataBytes / 4> zeroLight{};
			latched(a_resources->frameConstants, zeroLight.data(), sizeof(zeroLight), std::uint64_t(kFrameSlotSharedLight) * kFrameSlotBytes);
		}
		// The frame lighting, every commit: a latched copy written only when it changed would change the frame's shape with it.
		{
			const auto& lightingTables = a_store.GetTables();
			latched(a_resources->frameConstants, lightingTables.frameLighting.data(), sizeof(lightingTables.frameLighting), std::uint64_t(kFrameSlotLighting) * kFrameSlotBytes);
			a_resources->frameLightingUploaded = lightingTables.frameLightingVersion;
		}
		// The LOD fades' frame inputs, after the frame lighting in the same block (PS b13, c6): this frame's camera, every
		// epoch (the draw fades specular and envmap by distance, LodFadeFrame).
		const LodFadeFrame lodFadeFrame = SampleLodFadeFrame();
		latched(a_resources->frameConstants, &lodFadeFrame, sizeof(lodFadeFrame), std::uint64_t(kFrameSlotLighting) * kFrameSlotBytes + sizeof(FrameLighting));
		// CS_DCLF_FOLIAGE_PARITY: the colour epoch's buffers (by its parity) and its tag, after the LOD fades in the same block (PS
		// b13, c14: DCLFFoliageParity), and the compare pass's counters zeroed.
		// The Z-prepass's stages read the owners' index and the size from it too, as the colour commit before them left it.
		if (const auto& foliage = a_resources->foliage; foliage && !depthOnly) {
			const std::uint32_t epoch = ++foliage->epoch, h = epoch & 1;
			const std::uint32_t words[8] = { static_cast<std::uint32_t>(foliage->idsAddress[h]), static_cast<std::uint32_t>(foliage->idsAddress[h] >> 32),
				static_cast<std::uint32_t>(foliage->coloursAddress[h]), static_cast<std::uint32_t>(foliage->coloursAddress[h] >> 32), foliage->width, foliage->height,
				epoch & 0xFFu, foliage->ownersIndex };
			latched(a_resources->frameConstants, words, sizeof(words),
				std::uint64_t(kFrameSlotLighting) * kFrameSlotBytes + sizeof(FrameLighting) + sizeof(LodFadeFrame));
			static const std::array<std::uint32_t, kFoliageCounters> zeros{};
			uploads(foliage->results, zeros.data(), sizeof(zeros), 0);
		}

		lap(3);
		// Upload (the graph's upload pass runs ahead of every pass of this epoch). The worker's build staged its
		// payload itself: one submission, no copies here. A build made here, or staged against resources since
		// recreated, is uploaded from its vectors.
		// The rows' tables hold every slot the tables have (ReserveMainSequences, before the epoch): past them is a defect of
		// that reserve, never a row to drop.
		if (a_payload.materialRows.Count() > a_resources->materialRows.capacity || a_payload.pipelineRows.Count() > a_resources->pipelineRows.capacity)
			stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} material and {} pipeline rows past their tables' {} and {}", a_payload.materialRows.Count(),
				a_payload.pipelineRows.Count(), a_resources->materialRows.capacity, a_resources->pipelineRows.capacity));
		// Likewise the scene tables and the inputs (ReserveSceneTables, before the build's inputs were taken).
		CheckSceneCapacity(*a_resources->scene, 0, a_payload.geometryDraws.Count(), 0, a_payload.resident.Count() + a_payload.inputList.size(), a_resources->objectCapacity,
			"the main pass");
		// A batch staged against these resources and these rows' backings (one grown since holds nothing it staged against).
		const bool staged = a_payload.staged && a_payload.stagedFor == a_resources.get() && a_payload.stagedRowsGeneration == RowsGeneration(*a_resources) &&
		                    a_payload.stagedSceneGeneration == a_resources->scene->generation;
		if (staged)
			SubmitWorkerBatch(std::move(a_payload.staged));
		UploadFaceStreams(a_payload.faceStreams, a_resources->scene->facePositions, a_resources->scene->faceUploaded, uploads);
		ZeroFrameAheadOutputs(*a_resources->scene, uploads);
		UploadTrees(a_store.GetTables(), a_store.GetFrame(), *a_resources->scene, uploads, depthOnly);
		// Tree LOD (dclf-lod.md, "Tree LOD: the draws"): the mirror's changes, the draw row and the list's arguments, for the depth
		// epoch's cull and both passes' draws; it draws while its toggle is on and its programs and pipelines are built.
		if (depthOnly && a_resources->scene->treeLodCull) {
			auto& sceneBuffers = *a_resources->scene;
			UploadTreeLod(a_store.TreeLodMirror(), sceneBuffers, latched, a_bindingOwners, treeLodOwned);
			// The registrations withheld the engine's passes on the frame's decision: a commit that cannot draw leaves a hole.
			if (treeLodOwned && !sceneBuffers.treeLodReady.load(std::memory_order_acquire) && treeLodMissed++ < 8)
				logger::warn("[DCLF] tree LOD: frame {} withheld the engine's passes but the depth commit could not draw (tables {} of {} shape slots)", frameNumber,
					a_store.TreeLodMirror().ShapeSlots(), sceneBuffers.treeLodShapeCapacity);
		}
		// The fade roots, and once a frame their inputs, for FadeStateCS ahead of the depth segment's culling: the main camera
		// the list jobs cull with (PrimaryCull::FadeEye) and the engine's fade globals.
		if (depthOnly && a_resources->scene->fadeState) {
			auto& buffers = *a_resources->scene;
			std::uint32_t logBase = buffers.fadeLogBase;
			FadeFrame inputs = buffers.fadeFrame;
			if (buffers.fadeFrameNumber != a_store.GetFrame()) {
				inputs = FadeState::SampleFrame(nullptr);
				const auto eye = PrimaryCull::Get().FadeEye();
				std::copy_n(eye.data(), 3, inputs.eye);
				inputs.lodAdjust = eye[3];
				// The animation job's updates since the last cull: their roots stamped with this frame, their inputs in the frame row.
				std::vector<const void*> nodes;
				AnimatedFadeInputs anim{};
				PrimaryCull::Get().TakeAnimatedBatch(nodes, anim);
				std::copy_n(anim.eye, 3, inputs.animEye);
				inputs.animLodAdjust = anim.lodAdjust;
				inputs.animCounter = anim.counter;
				inputs.animDeltaTime = anim.deltaTime;
				// Each root once, with its count of updates.
				const auto& index = a_store.GetTables().fadeRootIndex;
				ankerl::unordered_dense::map<std::uint32_t, std::uint32_t> counts;
				if (buffers.fadeAnimated && anim.valid)
					for (const auto* node : nodes)
						if (const auto it = index.find(node); it != index.end() && it->second < buffers.fadeRootCapacity)
							++counts[it->second];
				auto& animatedWords = buffers.fadeAnimatedMirror;
				if (animatedWords.size() < buffers.fadeRootCapacity)
					animatedWords.resize(buffers.fadeRootCapacity, 0u);
				std::uint32_t lowest = ~0u, highest = 0;
				for (const auto& [root, count] : counts) {
					animatedWords[root] = FadeAnimatedWord(a_store.GetFrame(), count);
					lowest = (std::min)(lowest, root);
					highest = (std::max)(highest, root);
				}
				if (lowest <= highest)
					uploads(buffers.fadeAnimated, animatedWords.data() + lowest, std::size_t(highest - lowest + 1) * sizeof(std::uint32_t),
						std::uint64_t(lowest) * sizeof(std::uint32_t));
				if (anim.varied) {
					static std::uint32_t reported = 0;
					if (reported++ < 5)
						logger::info("[DCLF] the animation job's fade updates of frame {} had differing inputs; the first's stand for all", a_store.GetFrame());
				}
				logBase = NextFadeLog(a_store.GetFrame(), a_store.GetTables(), inputs);
			}
			auto& cull = PrimaryCull::Get();
			UploadFadeRoots(a_store.GetTables(), a_store.GetFrame(), inputs, logBase, buffers, uploads, cull.FadeVisibility(), cull.FadeVisibilityBlocks());
			// Each root's list block, when the roots or an entry's list changed (rarely: a new snapshot, a cell's lists).
			const auto& rootTables = a_store.GetTables();
			const std::uint64_t listsKey = rootTables.fadeRootsVersion * 0x9E3779B97F4A7C15ull ^ cull.FadeRootListsVersion();
			if (buffers.fadeRootLists && buffers.fadeRootListsHeld != listsKey && rootTables.fadeRootNode.size() <= buffers.fadeRootCapacity) {
				std::vector<std::uint32_t> lists;
				cull.FadeRootLists(rootTables.fadeRootNode, lists);
				if (!lists.empty())
					uploads(buffers.fadeRootLists, lists.data(), lists.size() * sizeof(std::uint32_t), 0);
				buffers.fadeRootListsHeld = listsKey;
			}
		}
		// The streams as the tables hold them now, whichever frame's build this is.
		const auto streams = CommitSceneStreams(*a_resources->scene, a_store.GetTables(), a_store.GetFrame(), a_store.GetTablesGeneration(), uploads);
		// The depth segment clears every counter; the colour segment clears only the word its own draws
		// append through. The culling happens in the depth segment, so clearing the
		// whole buffer again here would erase the phase 1 and phase 2 numbers before anything read them
		// - they are written earlier in the same frame.
		const std::size_t zeroBytes = depthOnly ? sizeof(kZeroCounts) : sizeof(std::uint32_t);
		latched(a_resources->count, kZeroCounts, zeroBytes, 0);
		// The sort's counts start at zero; from then on the scan that reads them clears them.
		if (a_resources->sort)
			a_resources->sort->ZeroCountsOnce(uploads);
		// The HZB build's group counter likewise, before its first dispatch (the depth segment's).
		if (depthOnly && a_resources->hzbCounter && !a_resources->hzbCounterZeroed) {
			static constexpr std::uint32_t kZero = 0;
			uploads(a_resources->hzbCounter, &kZero, sizeof(kZero), 0);
			a_resources->hzbCounterZeroed = true;
		}
		// The decal words: each group's slot count for its draw, and the tallies zeroed. Written by the
		// colour segment only, which is the one that submits decals.
		if (!depthOnly) {
			decalWords = { a_payload.decalCount[0], a_payload.decalCount[1], 0u, 0u, a_payload.decalCount[2] };
			latched(a_resources->count, decalWords.data(), 4 * sizeof(std::uint32_t), kCountDecalGroupWord * sizeof(std::uint32_t));
			latched(a_resources->count, &decalWords[4], sizeof(std::uint32_t), kCountDecalLayerWord * sizeof(std::uint32_t));
		}
		if (!staged)
			UploadMainPayload(a_payload, *a_resources, uploads);
		// Either path uploaded the rows the tables did not hold.
		a_resources->materialRowsHeld = a_payload.materialRows.Version();
		a_resources->pipelineRowsHeld = a_payload.pipelineRows.Version();
		// Either path uploaded the resident region when the buffer held another version of it, and the object records.
		a_resources->residentUploaded[depthOnly && a_resources->inputsDepth ? 0 : 1] = a_payload.resident.Version();
		auto& held = a_resources->scene->held;
		const std::size_t objectBytes = streams.objectBytes;
		const std::size_t geometryBytes = EmitGeometryDraws(a_payload.geometryDraws, held.geometries, [](const void*, std::size_t, std::size_t) {});
		if (a_payload.geometryDraws.Version())
			held.geometries = a_payload.geometryDraws.Version();
		a_stats.residentInputs = static_cast<std::uint32_t>(a_payload.resident.Count());
		if (!depthOnly) {
			a_stats.residentDraws = a_payload.residentDraws;
			a_stats.residentPairs = a_payload.residentPairs;
			a_stats.residentUndrawable = a_payload.residentUndrawable;
		}
		a_stats.residentVersions += a_payload.resident.Version() != a_stats.residentLastVersion[depthOnly ? 0 : 1] ? 1 : 0;
		a_stats.residentLastVersion[depthOnly ? 0 : 1] = a_payload.resident.Version();
		a_stats.residentResyncs += a_payload.residentResyncs;
		a_stats.residentParityChecks += a_payload.residentParityChecks;
		a_stats.residentParityMismatches += a_payload.residentParityMismatches;
		a_stats.residentMissing += a_payload.residentMissing;
		a_stats.residentPairsChecked += a_payload.residentPairsChecked;
		a_stats.residentPairsStale += a_payload.residentPairsStale;
		a_stats.boneRows = static_cast<std::uint32_t>(streams.boneRowsSent);

		lap(4);
		// The build's stats.
		a_stats.skipped = a_payload.skipped;
		a_stats.missingTextures = a_payload.missingTextures;
		a_stats.missingVertexConstants = a_payload.missingVertexConstants;
		a_stats.missingPixelConstants = a_payload.missingPixelConstants;
		a_stats.deferredTextures = a_payload.deferredTextures;
		a_stats.bindlessParityChecks += a_payload.bindlessParityChecks;
		a_stats.bindlessParityMismatches += a_payload.bindlessParityMismatches;
		a_stats.rowTableConflicts += a_payload.rowTableConflicts;
		for (std::size_t i = 0; i < a_payload.partMs.size(); ++i)
			a_stats.partMs[i] = a_payload.partMs[i];
		if (!depthOnly)
			a_stats.decalsDrawn = a_payload.decalsDrawn;
		if (a_payload.shortBuffers) {
			const bool first = a_stats.shortBuffers == 0;
			a_stats.shortBuffers += a_payload.shortBuffers;
			const auto& shortBuffer = a_payload.shortBuffer;
			if (first && shortBuffer.object < tables.objects.size()) {
				const auto& geometry = tables.geometries[tables.objects[shortBuffer.object].geometryIndex];
				logger::warn("[DCLF] '{}' draws past its buffers: {} vertices x {} bytes needs {}, the slice holds {}; indices to {} need {}, the slice holds {}",
					tables.objectGeometry[shortBuffer.object] ? tables.objectGeometry[shortBuffer.object]->name.c_str() : "?", geometry.vertexCount, geometry.vertexStride,
					shortBuffer.vertexNeeded, geometry.vertexBytes, geometry.firstIndex + geometry.indexCount, shortBuffer.indexNeeded, geometry.indexBytes);
			}
		}
		a_stats.uploadBytes = (a_payload.materialRowsWritten * sizeof(MaterialRow)) + (a_payload.pipelineRowsWritten * sizeof(PipelineRow)) +
		                      a_payload.inputList.size() * sizeof(DrawInput) + geometryBytes + objectBytes;
		a_stats.records = static_cast<std::uint32_t>(a_payload.materialRows.Count());
		lap(6);
		lap(5);

		// The resident region's draws and inputs lead the frame's own (ResidentRegion).
		const std::uint32_t drawCount = static_cast<std::uint32_t>(a_payload.sequences.size() + a_payload.residentDraws);
		const auto decalCount = depthOnly ? std::array<std::uint32_t, kDecalGroups>{} : a_payload.decalCount;
		// Inputs the culling dispatch covers. In the depth segment this exceeds drawCount, because that
		// segment submits a cull-only input for every candidate it is not allowed to draw.
		const std::uint32_t inputCount = static_cast<std::uint32_t>(a_payload.inputList.size() + (a_payload.resident.Count()));
		// The sequence buffer's ranges hold every draw the scene can produce (ReserveMainSequences, before the epoch): a count past
		// them is a defect of that bound, never a draw to drop.
		if (drawCount > a_resources->sequenceDraws)
			stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} draws past the sequence buffer's {} (the scene's draw bound missed them)", drawCount, a_resources->sequenceDraws));
		for (std::uint32_t group = 0; group < kDecalGroups; ++group)
			if (decalCount[group] > a_resources->sequenceDecals)
				stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} decals past the sequence buffer's {} per group", decalCount[group], a_resources->sequenceDecals));
		// The colour pass's cascades and local shadow volumes (the sun's Accumulate has run, the shadow maps are drawn): the main
		// latch grown to hold them before the shape names it.
		if (!depthOnly) {
			SunAccumulation::Get().GpuCascades(sunCascades);
			{
				// Frames whose sun test runs without cascades: every pass with the sun's bits draws unshadowed (right only when the
				// sun did not accumulate).
				static std::uint32_t commits = 0, empty = 0;
				empty += sunCascades.empty() ? 1 : 0;
				if (++commits == 300) {
					logger::info("[DCLF] colour sun test: {} of 300 frames without the sun's cascades{}", empty, empty ? " <- NO CASCADES" : " <- OK");
					commits = empty = 0;
				}
			}
			// And the local shadow lights that accumulated this frame (LocalShadowLights), a volume per shadowmap descriptor, for
			// each input's Light Limit Fix shadow mask. They have accumulated: the shadow maps are drawn.
			localShadows = LocalShadowLights::Sample();
			shadowVolumes.clear();
			for (const auto& light : localShadows.lights) {
				for (const auto& volume : light.volumes) {
					auto& out = shadowVolumes.emplace_back();
					out.masks[0] = volume.masks[0];
					out.masks[1] = volume.masks[1];
					out.maskBit = light.maskBit;
					out.affectsLand = light.affectsLand ? 1u : 0u;
					out.sphere[0] = light.center[0];
					out.sphere[1] = light.center[1];
					out.sphere[2] = light.center[2];
					out.sphere[3] = light.radius;
					std::memcpy(out.planes, volume.planes.data(), sizeof(out.planes));
				}
			}
			ReserveMainLatch(*a_resources, static_cast<std::uint32_t>(sunCascades.size()), static_cast<std::uint32_t>(shadowVolumes.size()));
		}
		// The shape (MakeMainShape), from this commit's inputs; a scene revision makes it from its own (MakeRevisionShapes). Its max
		// counts hold this frame's draws whatever the bound says (a draw past the bound is the bound's defect, the parity's to show).
		auto shapeIn = MainShapeInputsOf(*a_resources, depthOnly, tables, std::max(drawBound.Draws(), drawCount));
		for (std::uint32_t group = 0; group < kDecalGroups; ++group)
			shapeIn.decalBound[group] = std::max(shapeIn.decalBound[group], decalCount[group]);
		// Both epochs rasterise with the main pass's depth range; see Impl::mainMinDepth.
		const bool useMainRange = mainMaxDepth > 0.0f;
		shapeIn.viewport = { a_capture.viewportWidth, a_capture.viewportHeight, useMainRange ? mainMinDepth : a_capture.minDepth,
			useMainRange ? mainMaxDepth : a_capture.maxDepth };
		shapeIn.resourceHeap = org::runtime::GetActiveSRVDescriptorHeap().GetHandle();
		shapeIn.samplerHeap = org::runtime::GetActiveSamplerDescriptorHeap().GetHandle();
		shapeIn.indirect = indirect;
		shapeIn.zCalls = zPlan.calls;
		shapeIn.latched.copies = latchedLayout;
		if (!latchedLayout.empty())
			shapeIn.latched.latch = a_resources->latchedBlocks[latchedShape];
		auto frame = MakeMainShape(shapeIn);
		shapeParity.known[shapeIndex] = true;
		shapeParity.viewport[shapeIndex] = shapeIn.viewport;
		shapeParity.resourceHeap = shapeIn.resourceHeap;
		shapeParity.samplerHeap = shapeIn.samplerHeap;
		// The main pass's ViewProj (VS_PerFrame c8), which the draws project with: the culling has to
		// use the same matrix or it would reject what the draws would have put on screen.
		std::array<float, 16> viewProj{};
		bool hasViewProj = false;
		if (auto* buffer = a_capture.vsBuffers[kPerFrameVertexRegister]) {
			mirror.Watch(buffer);
			const auto contents = mirror.Contents(buffer);
			if (contents.size() >= 48 * sizeof(float)) {
				std::memcpy(viewProj.data(), reinterpret_cast<const float*>(contents.data()) + 32, sizeof(float) * 16);
				hasViewProj = true;
			}
		}
		if (!loggedNoViewProj) {
			loggedNoViewProj = true;
			logger::info("[DCLF] culling setup: mode {}, ViewProj {}, VS_PerFrame b{} {}", frame->cullMode, hasViewProj ? "yes" : "no",
				kPerFrameVertexRegister, a_capture.vsBuffers[kPerFrameVertexRegister] ? "bound" : "not bound");
		}
		// The two epochs must rasterise into the same pixels for the colour pass's EQUAL test to have any
		// chance: a different viewport or depth range between the depth pass and the main pass puts the
		// same vertex on a different pixel, at a different depth.
		if ((depthOnly ? loggedDepthViewport : loggedColourViewport)++ % 480 == 0) {
			// The z row of the ViewProj each epoch actually packs: this is what turns a vertex into the
			// depth the test compares, so if the two epochs differ it shows up here.
			std::string viewProjZ = "(none)";
			{
				std::span<const std::byte> perFrame;
				if (replayVertexInputs)
					perFrame = prepassVS[kPerFrameVertexRegister];
				else if (auto* buffer = a_capture.vsBuffers[kPerFrameVertexRegister])
					perFrame = mirror.Contents(buffer);
				if (perFrame.size() >= 48 * sizeof(float)) {
					const auto* floats = reinterpret_cast<const float*>(perFrame.data());
					viewProjZ = fmt::format("({:.6f} {:.6f} {:.6f} {:.6f})", floats[40], floats[41], floats[42], floats[43]);
				}
			}
			logger::info("[DCLF] {} epoch: tables frame {} holding {} objects; render area {}x{}, depth range [{}, {}] (captured [{}, {}]), replay {}, eye ({:.2f} {:.2f} {:.2f}), ViewProj z row {}",
				depthOnly ? "z-prepass" : "colour", frameNumber, tables.liveObjects, frame->width, frame->height, frame->minDepth, frame->maxDepth,
				a_capture.minDepth, a_capture.maxDepth, replayVertexInputs ? "on" : "off", a_capture.eye.x, a_capture.eye.y, a_capture.eye.z, viewProjZ);
		}
		// The jitter the colour epoch projects with (dclf-open-defects.md, "DCLF's draws appear not to carry the TAA /
		// upscaler jitter"): the packed ViewProj (c8) against the same buffer's unjittered one (c12), the main pass's own
		// b12 now, Community Shaders' cached frame buffer, and the last colour epoch's.
		if (!depthOnly && SwitchEnabled(Switch::Stats)) {
			std::span<const std::byte> packed;
			if (replayVertexInputs)
				packed = prepassVS[kPerFrameVertexRegister];
			else if (auto* buffer = a_capture.vsBuffers[kPerFrameVertexRegister])
				packed = mirror.Contents(buffer);
			if (packed.size() >= 16 * 4 * sizeof(float)) {
				const auto* p = reinterpret_cast<const float*>(packed.data());
				auto maxDiff = [&](const float* a_other) {
					float d = 0.0f;
					for (int i = 0; i < 16; ++i)
						d = std::max(d, std::abs(p[32 + i] - a_other[i]));
					return d;
				};
				auto& j = jitterProbe;
				j.vsUnjittered += maxDiff(p + 48);
				j.vsUnjitteredMax = std::max(j.vsUnjitteredMax, maxDiff(p + 48));
				if (auto* buffer = a_capture.vsBuffers[kPerFrameVertexRegister]) {
					const auto main = mirror.Contents(buffer);
					if (main.size() >= 48 * sizeof(float)) {
						const float d = maxDiff(reinterpret_cast<const float*>(main.data()) + 32);
						j.vsMain += d;
						j.vsMainMax = std::max(j.vsMainMax, d);
					}
				}
				const float cached = maxDiff(reinterpret_cast<const float*>(&globals::game::frameBufferCached.data) + 32);
				j.vsCached += cached;
				j.vsCachedMax = std::max(j.vsCachedMax, cached);
				if (j.havePrevious) {
					const float d = maxDiff(j.previous.data());
					j.vsPrevious += d;
					j.vsPreviousMax = std::max(j.vsPreviousMax, d);
				}
				std::copy(p + 32, p + 48, j.previous.begin());
				j.havePrevious = true;
				if (++j.epochs == 300) {
					logger::info("[DCLF] colour epoch ViewProj over {} epochs (mean / max of the largest element difference): against its unjittered copy {:.2e} / {:.2e}, "
								 "the main pass's b12 now {:.2e} / {:.2e}, the cached frame buffer {:.2e} / {:.2e}, the previous epoch's {:.2e} / {:.2e}; jitter {:.6f} {:.6f}",
						j.epochs, j.vsUnjittered / j.epochs, j.vsUnjitteredMax, j.vsMain / j.epochs, j.vsMainMax, j.vsCached / j.epochs, j.vsCachedMax,
						j.vsPrevious / j.epochs, j.vsPreviousMax, p[32 + 2] - p[48 + 2], p[32 + 6] - p[48 + 6]);
					j = { .previous = j.previous, .havePrevious = true };
				}
			}
		}
		if (depthOnly) {
			prepassEye = a_capture.eye;
			prepassPreviousEye = a_capture.previousEye;
			prepassInputs = true;
		}
		a_stats.drawn = drawCount;
		++a_stats.commitEpochs;

		// The execution's values, into its slot of the latch (this runs inside the epoch, after the host
		// waited for the slot): what the BuildDraws dispatches of the segment read instead of push constants.
		BuildDrawsLatch latch{};
		latch.dispatch[0] = (inputCount + 63) / 64;
		latch.dispatch[1] = 1;
		latch.dispatch[2] = 1;
		latch.drawCount = inputCount;
		latch.cullFlags = hasViewProj ? frame->cullMode : 0u;
		// The frame number, not the epoch: the depth segment publishes and the colour segment reads within one
		// frame, so the stamp has to be the thing they share.
		latch.visibilityStamp = frameNumber & 0x0FFFFFFFu;  // 28 bits: BuildDrawsCS keeps flags below it
		if (a_resources->hzb && frame->width && frame->height) {
			// Mip 0 covers twice its own size in source pixels, of which only the rendered area holds real
			// depth. A texture coordinate in the image scales by that ratio to reach the HZB.
			const auto scale = [](std::uint32_t a_rendered, std::uint32_t a_covered) {
				const double ratio = a_covered ? std::min(1.0, double(a_rendered) / double(a_covered)) : 0.0;
				return static_cast<std::uint32_t>(std::lround(ratio * 65535.0)) & 0xFFFFu;
			};
			latch.hzbUvScalePacked = scale(frame->width, a_resources->hzbWidth * 2) | (scale(frame->height, a_resources->hzbHeight * 2) << 16);
		}
		FoldEyeIntoViewProj(viewProj, a_capture.eye, latch.viewProj);
		// CS_DCLF_TARGET_PROBE: the objects whose bound covers the probed pixel, nearest first, with their pipelines' descriptors.
		if (!depthOnly && frame->width && frame->height && (frameNumber % 240) == 0) {
			if (const auto& pixel = SwitchValue(Switch::TargetProbe); !pixel.empty()) {
				const auto sep = pixel.find_first_of(",x");
				const float px = std::strtof(pixel.substr(0, sep).c_str(), nullptr) + 32.0f, py = std::strtof(pixel.substr(sep + 1).c_str(), nullptr) + 32.0f;
				const float nx = px / frame->width * 2.0f - 1.0f, ny = 1.0f - py / frame->height * 2.0f;
				struct Hit { float w; std::uint32_t object; };
				std::vector<Hit> hits;
				for (std::uint32_t o = 0; o < tables.objects.size(); ++o) {
					const auto& r = tables.objects[o];
					if (r.boundRadius <= 0.0f)
						continue;
					float clip[4];
					for (std::uint32_t row = 0; row < 4; ++row)
						clip[row] = latch.viewProj[row * 4] * r.boundCenter[0] + latch.viewProj[row * 4 + 1] * r.boundCenter[1] + latch.viewProj[row * 4 + 2] * r.boundCenter[2] +
						            latch.viewProj[row * 4 + 3];
					if (clip[3] <= 1.0f)
						continue;
					// The sphere's screen radius, roughly (the projection's x scale over the distance).
					const float scale = std::sqrt(latch.viewProj[0] * latch.viewProj[0] + latch.viewProj[1] * latch.viewProj[1] + latch.viewProj[2] * latch.viewProj[2]);
					const float radius = r.boundRadius * scale / clip[3];
					const float dx = clip[0] / clip[3] - nx, dy = clip[1] / clip[3] - ny;
					if (dx * dx + dy * dy <= radius * radius)
						hits.push_back({ clip[3], o });
				}
				std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.w < b.w; });
				std::string text;
				const auto& keys = tables.pipelines;
				for (std::size_t i = 0; i < hits.size() && i < 8; ++i) {
					const auto o = hits[i].object;
					const auto* geometry = o < tables.objectGeometry.size() ? tables.objectGeometry[o] : nullptr;
					const auto pipeline = tables.objects[o].pipelineIndex;
					text += fmt::format("; '{}' (object {}, w {:.0f}, flags {:08X}, pipeline {} pixel {:08X})", geometry && geometry->name.c_str() ? geometry->name.c_str() : "?", o,
						hits[i].w, tables.objects[o].flags, pipeline, pipeline < keys.size() ? keys[pipeline].pixelDescriptor : 0u);
				}
				logger::info("[DCLF] target probe: {} objects cover the pixel{}", hits.size(), text);
			}
		}
		// The depth segment: the camera the fade roots' distances are measured from (kObjectFadeTest).
		if (depthOnly) {
			const auto eye = PrimaryCull::Get().FadeEye();
			std::copy(eye.begin(), eye.end(), latch.fadeEye);
			const auto treeHeight = PrimaryCull::Get().TreeHeightTest();
			latch.treeHeight[0] = treeHeight[0];
			latch.treeHeight[1] = treeHeight[1];
			latch.fadeStatesIndex = a_resources->scene->FadeStatesReadIndex(frameNumber);
		}
		// The colour pass: this frame's cascades, for the synthetic passes' sun test, in the slot's region after the latch (the
		// block holds them: ReserveMainLatch, before the shape). The sun's Accumulate has run.
		const std::uint32_t latchSlot = RenderGraphRuntime::Get().Host()->CurrentFrameSlot();
		// The depth segment's plain draws (MainOpaquePass): the index pool brought up to the tables (unless the shadow commit did,
		// earlier this frame); the buckets' map and each phase's table (PlanZBuckets, whose calls are the shape's) in the slot's
		// region, and their count words zeroed. A slot without a published pipeline maps to none: BuildDraws drops its draws.
		if (depthOnly && a_resources->pool) {
			const auto& layout = a_resources->latchLayout;
			UpdateIndexPool(*a_resources->pool, a_store.GetTables(), a_store.GetTablesGeneration(), *a_resources->latch, latchSlot, layout.PoolOffset(), uploads);
			const auto slots = static_cast<std::uint32_t>(zPlan.map.size());
			if (slots > layout.buckets)
				stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} Z-prepass pipeline slots past the latch's {}", slots, layout.buckets));
			const std::uint32_t buckets = zPlan.Buckets();
			if (slots)
				a_resources->latch->Write(latchSlot, layout.BucketMapOffset(), std::as_bytes(std::span(zPlan.map.data(), slots)));
			if (buckets) {
				const std::size_t words = std::size_t(buckets) * 2;
				a_resources->latch->Write(latchSlot, layout.BucketTableOffset(), std::as_bytes(std::span(zPlan.table.data(), words)));
				a_resources->latch->Write(latchSlot, layout.PhaseTwoBucketTableOffset(), std::as_bytes(std::span(zPlan.table.data() + words, words)));
				zBucketZeros.resize(std::max<std::size_t>(zBucketZeros.size(), buckets), 0u);
				for (const auto& counts : a_resources->zBucketCounts)
					latched(counts, zBucketZeros.data(), std::size_t(buckets) * sizeof(std::uint32_t), 0);
			}
			const auto region = static_cast<std::uint32_t>(a_resources->latch->Offset(latchSlot));
			latch.bucketTableOffset = region + layout.BucketTableOffset();
			latch.phaseTwoBucketTableOffset = region + layout.PhaseTwoBucketTableOffset();
			latch.bucketMapOffset = region + layout.BucketMapOffset();
		}
		if (!depthOnly) {
			const auto& layout = a_resources->latchLayout;
			const std::uint32_t header[4] = { static_cast<std::uint32_t>(sunCascades.size()), 0, 0, 0 };
			a_resources->latch->Write(latchSlot, MainLatchLayout::CascadeOffset(), std::as_bytes(std::span(header)));
			if (!sunCascades.empty())
				a_resources->latch->Write(latchSlot, MainLatchLayout::CascadeOffset() + kSunRegionHeader, std::as_bytes(std::span(sunCascades)));
			const std::uint32_t volumeHeader[4] = { static_cast<std::uint32_t>(shadowVolumes.size()), 0, 0, 0 };
			a_resources->latch->Write(latchSlot, layout.ShadowVolumeOffset(), std::as_bytes(std::span(volumeHeader)));
			if (!shadowVolumes.empty())
				a_resources->latch->Write(latchSlot, layout.ShadowVolumeOffset() + kSunRegionHeader, std::as_bytes(std::span(shadowVolumes)));
			latch.sunState = kSunTestOn;
			latch.sunCascadeOffset = static_cast<std::uint32_t>(a_resources->latch->Offset(latchSlot)) + MainLatchLayout::CascadeOffset();
			latch.localShadowOffset = static_cast<std::uint32_t>(a_resources->latch->Offset(latchSlot)) + layout.ShadowVolumeOffset();
			sunUpload = latch;
		}
		a_resources->latch->WriteValue(latchSlot, 0, latch);
		// What the reflection's faces, early next frame, draw from (ExecuteReflection): this commit's inputs and the buffers' backings.
		a_resources->committed[shapeIndex] = { frameNumber, inputCount, a_resources->scene->generation, a_resources->objectCapacity };

		frame->latched = latched.Finish();
		NoteShapeParity(shapeIndex, *frame, latchedLayout, frameNumber);
		PublishShape(std::move(frame), a_resources->published[shapeIndex], a_resources->frames[shapeIndex], a_resources->shapeGenerations,
			a_resources->recentShapes[shapeIndex]);
		lap(6);
		return true;
	}
}

#endif

namespace DCLF
{
	bool IndirectDraws::DecideTreeLod()
	{
		bool owned = false;
		auto* buffers = impl->scene.get();
		if (buffers && buffers->treeLodCull && ActiveToggles().lodTrees && SceneStore::Get().TreeLodMirror().Size()) {
			auto* shader = Engine::Global<RE::BSShader*>(0x33dcd10);  // the BSDistantTreeShader
			const auto* program = shader ? ShaderPrograms::Get().FindTreeLod(*shader) : nullptr;
			const auto* texture = Engine::Global<RE::NiSourceTexture*>(0x33dcd18);  // its tree LOD atlas (UploadTreeLod)
			TreeLodPipelines pipelines;
			if (program && DrawPipelines::Get().FindTreeLod(*program, pipelines) && texture && texture->rendererTexture && texture->rendererTexture->resourceView) {
				const auto published = buffers->treeLodPipelines.load(std::memory_order_acquire);
				if (!published || !SameHandle(published->depth, pipelines.depth) || !SameHandle(published->colour, pipelines.colour))
					buffers->treeLodPipelines.store(std::make_shared<const TreeLodPipelines>(pipelines), std::memory_order_release);
				owned = true;
			}
		}
		impl->treeLodOwned = owned;
		PassCapture::Get().SetTreeLodOwned(owned);
		return owned;
	}

	void IndirectDraws::KickFadeWriteBack()
	{
		JoinFadeWriteBack();  // one no join reached (a frame that rendered neither shadows nor the accumulate phase)
		if (!impl->scene || !impl->scene->fadeWriteBack)
			return;
		auto& writeBack = *impl->scene->fadeWriteBack;
		// The batches after what the last job left, in frame order (the stack hands them back newest first, and two recordings
		// need not finish in their frames' order).
		std::vector<FadeWriteBack::Batch*> batches;
		for (auto* batch = writeBack.batches.exchange(nullptr, std::memory_order_acquire); batch; batch = batch->next)
			batches.push_back(batch);
		std::reverse(batches.begin(), batches.end());
		std::stable_sort(batches.begin(), batches.end(), [](const auto* a_a, const auto* a_b) { return a_a->frame < a_b->frame; });
		for (auto* batch : batches) {
			writeBack.pending.insert(writeBack.pending.end(), batch->events.begin(), batch->events.end());
			writeBack.pendingFrames.insert(writeBack.pendingFrames.end(), batch->events.size(), batch->frame);
			delete batch;
		}
		if (writeBack.pending.empty())
			return;
		writeBack.done.store(0, std::memory_order_relaxed);
		// Between here and the join the tables are the frame's and no node is freed: the scene events are applied at the next
		// frame's start, after the join.
		auto apply = [scene = impl->scene](std::stop_token a_stop, bool a_worker) {
			auto& state = *scene->fadeWriteBack;
			const auto& tables = SceneStore::Get().GetTables();
			for (std::size_t i = state.done.load(std::memory_order_relaxed); i < state.pending.size(); ++i) {
				if (a_stop.stop_requested())
					return;
				// A worker writes the engine's nodes only inside the window; the next job takes what it left.
				std::optional<EngineReadWindow::Lease> lease;
				if (a_worker && !lease.emplace())
					return;
				const auto& event = state.pending[i];
				const std::uint32_t frame = state.pendingFrames[i];
				if (event.root >= state.appliedFrame.size())
					state.appliedFrame.resize(std::size_t(event.root) + 1, 0u);
				// The root the event was of, still stood in: one listed again since, or the engine's own again, keeps its node; and
				// an event older than the one written last is not written over it.
				if (event.root >= tables.fadeRoots.size() || tables.fadeRoots[event.root].generation != event.generation ||
					!(tables.fadeRoots[event.root].bits & kFadeRootStoodIn) || !tables.fadeRootNode[event.root] ||
					frame < state.appliedFrame[event.root]) {
					++state.stale;
				} else {
					state.appliedFrame[event.root] = frame;
					// The fade, and the fade bits of the flags (atomically: the engine owns the others). The fade watch is not told:
					// a stood-in root's dependents read its fade from FadeStateCS's state (SceneStore::MarkFadeRootOwned).
					auto* node = static_cast<std::byte*>(const_cast<void*>(tables.fadeRootNode[event.root]));
					std::atomic_ref<std::uint32_t> flags(*reinterpret_cast<std::uint32_t*>(node + 0xF4));
					flags.fetch_and(~kFadeFlagMask, std::memory_order_relaxed);
					flags.fetch_or(event.flags & kFadeFlagMask, std::memory_order_relaxed);
					*reinterpret_cast<float*>(node + 0x130) = event.currentFade;
					++state.applied;
				}
				state.done.store(i + 1, std::memory_order_release);
			}
		};
		if (!AsyncEnabled()) {
			apply(std::stop_token{}, false);
			writeBack.pending.clear();
			writeBack.pendingFrames.clear();
			return;
		}
		writeBack.job = std::make_shared<AsyncWorker::JobHandle>(
			AsyncWorker::Get().Submit("fade write-back", [apply = std::move(apply)](std::stop_token a_stop) { apply(a_stop, true); }));
	}

	void IndirectDraws::JoinFadeWriteBack()
	{
		if (!impl->scene || !impl->scene->fadeWriteBack || !impl->scene->fadeWriteBack->job)
			return;
		auto& writeBack = *impl->scene->fadeWriteBack;
		auto& worker = AsyncWorker::Get();
		const auto job = std::exchange(writeBack.job, nullptr);
		// Late: stopped (waited for while it runs); the rest is the next job's, not this thread's.
		if (worker.Wait(*job, AsyncWaitBudget()) != AsyncWorker::WaitResult::Done) {
			worker.Cancel(*job);
			++writeBack.late;
		}
		const std::size_t done = std::min(writeBack.done.load(std::memory_order_acquire), writeBack.pending.size());
		writeBack.pending.erase(writeBack.pending.begin(), writeBack.pending.begin() + static_cast<std::ptrdiff_t>(done));
		writeBack.pendingFrames.erase(writeBack.pendingFrames.begin(), writeBack.pendingFrames.begin() + static_cast<std::ptrdiff_t>(done));
	}
}
