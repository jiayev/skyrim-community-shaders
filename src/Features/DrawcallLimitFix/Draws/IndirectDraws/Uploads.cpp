#if defined(CS_HAS_RENDER_GRAPH) && defined(CS_HAS_ORG_MODULE_SERVICES)
#	include "Internal.h"

namespace DCLF
{
	namespace
	{
		// The payload's own uploads: the rows, the per-object records and bone rows, and the draw inputs
		// with their geometry. Each buffer has its own condition: a depth epoch where every candidate is cull-only has plenty
		// of inputs, and BuildDraws dispatches over the inputs.
		/** @brief The versions a payload's uploads are against: what the buffers hold (the commit), or are to hold (a build ahead's staging). */
		struct PayloadHeld
		{
			std::uint64_t materialRows = 0, pipelineRows = 0, resident = 0, geometries = 0;
		};
		PayloadHeld HeldNow(const MainPayload& a_payload, const Resources& a_resources)
		{
			const auto& addresses = a_payload.inputs.addresses;
			const bool depth = a_payload.inputs.depthOnly && a_resources.inputsDepth;
			const bool own = !a_payload.foreignRows;
			return { own && addresses.records == a_resources.materialRows.address ? a_resources.materialRowsHeld : 0,
				own && addresses.pipelineRows == a_resources.pipelineRows.address ? a_resources.pipelineRowsHeld : 0, a_resources.residentUploaded[depth ? 0 : 1],
				a_resources.scene->held.geometries };
		}

		template <class Emit>
		void ForEachMainPayloadUpload(const MainPayload& a_payload, const Resources& a_resources, const PayloadHeld& a_held, Emit&& a_emit)
		{
			{
				ZoneScopedN("CS.DCLF.UploadRanges.Rows");
				// The rows the tables do not hold, their headers' addresses made absolute (both segments share them).
				// Into the current tables; rows past them wait for their growth's adoption (Growths). A table grown at once since the
				// build's inputs were taken (live mode: the epoch's reserve) holds none of them: all are sent.
				const auto& addresses = a_payload.inputs.addresses;
				const auto& materials = a_resources.materialRows;
				const auto& pipelines = a_resources.pipelineRows;
				EmitMainRows(a_payload.materialRows, addresses.records == materials.address ? a_held.materialRows : 0, materials.address, materials.capacity,
					[](MaterialRow& a_row, std::uint64_t a_address) {
					PatchRowAddresses(a_row, a_address);
				}, [&](const void* a_data, std::size_t a_bytes, std::size_t a_offset) { a_emit(a_resources.materialRows.buffer, a_data, a_bytes, a_offset); });
				EmitMainRows(a_payload.pipelineRows, addresses.pipelineRows == pipelines.address ? a_held.pipelineRows : 0, pipelines.address, pipelines.capacity,
					[](PipelineRow& a_row, std::uint64_t a_address) {
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
			a_payload.resident.Emit(a_held.resident,
				[&](const void* a_data, std::size_t a_bytes, std::size_t a_offset) { a_emit(inputs, a_data, a_bytes, a_offset); });
			if (!a_payload.inputList.empty())
				a_emit(inputs, a_payload.inputList.data(), a_payload.inputList.size() * sizeof(DrawInput), regionCount * sizeof(DrawInput));
			TracyCZoneEnd(residentUploadZone);
			EmitGeometryDraws(a_payload.geometryDraws, a_held.geometries, [&](const void* a_data, std::size_t a_bytes, std::size_t a_offset) {
				a_emit(scene.geometries->Get(), a_data, a_bytes, a_offset);
			}, scene.geometryRows);
		}

		// Render thread: the payload through the commit's uploads, copied now.
		void UploadMainPayload(const MainPayload& a_payload, const Resources& a_resources, CommitUploads& a_uploads)
		{
			ForEachMainPayloadUpload(a_payload, a_resources, HeldNow(a_payload, a_resources), [&](const auto& a_target, const void* a_data, std::size_t a_bytes, std::size_t a_offset) {
				a_uploads(a_target, a_data, a_bytes, a_offset);
			});
		}

	}

	namespace
	{
		StreamsKey StreamsKeyOf(const SceneStore::Tables& a_tables, std::uint32_t a_generation)
		{
			return { &a_tables, a_tables.changeLog.End(), a_tables.geometryLog.End(), a_generation };
		}

	}

	/** @brief The stores brought up to the tables (from what the buffers hold), as views (StreamViews). */
	std::shared_ptr<const StreamViews> Draws::MakeStreamViews(ObjectRecordStore& a_objects, ExtrasStore& a_extras, GeometryStore& a_geometries, const TablesHeld& a_from,
			std::shared_ptr<const SceneStore::Tables> a_hold, const SceneStore::Tables& a_tables, std::uint32_t a_generation, std::uint32_t a_frame)
		{
			ZoneScopedN("CS.DCLF.MakeStreamViews");
			auto views = std::make_shared<StreamViews>();
			views->tables = std::move(a_hold);
			views->generation = a_generation;
			UpdateObjectRecords(&a_objects, a_from.objects, a_tables, a_generation, a_frame, views->objects);
			UpdateExtras(&a_extras, a_from.extras, a_tables, a_generation, views->extras);
			GeometryDrawsOut geometries;
			UpdateGeometryDraws(&a_geometries, a_from.geometries, a_tables, a_generation, a_frame, geometries);
			views->geometries = std::move(geometries.slots);
			return views;
		}

	std::shared_ptr<const StreamViews> IndirectDraws::Impl::StreamsNow()
	{
		ZoneScopedN("CS.DCLF.StreamsNow");
		auto& store = SceneStore::Get();
		const auto& tables = store.GetTables();
		if (installedDraws && installedDraws->tables.get() == &tables)
			return installedDraws->streams;
		const auto key = StreamsKeyOf(tables, store.GetTablesGeneration());
		if (streamViews && streamViewsKey == key)
			return streamViews;
		if (!resources || !resources->scene || tables.objects.empty())
			return nullptr;
		if (store.SceneTaskInFlight() || aheadDone.load(std::memory_order_acquire) < aheadKicked) {
			++streamsRefused;
			return nullptr;
		}
		// Their journals keep what the scene buffers and the ring's entries lack.
		const auto& held = resources->scene->held;
		const auto& holders = ringHolders;
		const TablesHeld from{ std::min(held.objects, holders.objects.Oldest()), std::min(held.extras, holders.extras.Oldest()),
			std::min(held.geometries, holders.geometries.Oldest()) };
		streamViews = MakeStreamViews(objectStore, extrasStore, geometryStore, from, store.AcceptedTables(), tables, store.GetTablesGeneration(), store.GetFrame());
		streamViewsKey = key;
		return streamViews;
	}

	IndirectDraws::Impl::SceneStreams IndirectDraws::Impl::CommitSceneStreams(SceneBuffers& a_scene, const SceneStore::Tables& a_tables, std::uint32_t a_frame,
		std::uint32_t a_generation, CommitUploads& a_uploads)
	{
		ZoneScopedN("CS.DCLF.CommitSceneStreams");
		// A commit that reads no ring entry (CS_DCLF_BINDLESS_PARITY's own build): what the scene buffers lack of the frame's views.
		(void)a_generation;
		const auto views = StreamsNow();
		SceneStreams sent;
		if (!views)
			return sent;
		(void)a_tables;
		const ObjectRecordsOut& objects = views->objects;
		sent.objects = objects.Count();
		// Within the buffers: a record or a row past them waits for their growth (Growths), sent again until it is adopted (the
		// version held moves only once all of them are sent). No build names an object past them (ObjectFits).
		sent.objectBytes = objects.Emit(a_scene.held.objects, [&](const void* a_data, std::size_t a_bytes, std::size_t a_offset) {
			a_uploads(a_scene.objects, a_data, a_bytes, a_offset);
		}, a_scene.objectCapacity);
		if (objects.Version() && objects.Count() <= a_scene.objectCapacity)
			a_scene.held.objects = objects.Version();
		const ExtrasOut& extras = views->extras;
		sent.extraRows = extras.Rows();
		ExtrasStore* extrasParity = PersistentParityEnabled() ? &extrasStore : nullptr;
		sent.extraRowsSent = EmitExtras(extras, a_scene.held.extras, extrasParity, [&](const void* a_data, std::size_t a_bytes, std::size_t a_offset) {
			a_uploads(a_scene.extras, a_data, a_bytes, a_offset);
		}, a_scene.extraRows);
		if (extrasParity && ParityDue(a_frame) && extras.Rows() <= a_scene.extraRows)
			CheckExtras(*extrasParity, extras);
		if (extras.Version() && extras.Rows() <= a_scene.extraRows)
			a_scene.held.extras = extras.Version();
		extrasStore.rowsSent += sent.extraRowsSent;
		return sent;
	}

	bool IndirectDraws::Impl::CommitMainPayload(const Capture& a_capture, const FrameBlocks& a_blocks, const MainInputs& a_frame, MainPayload& a_payload,
		const std::shared_ptr<Resources>& a_resources, SceneStore& a_store, IndirectDraws::Stats& a_stats,
		std::vector<std::shared_ptr<const void>>& a_bindingOwners, const EpochRevision& a_revision)
	{
		ZoneScopedN("CS.DCLF.CommitMainPayload");
		// The cleared geometry slots' buffers, held until this execution retires (SceneStore::TakeRetiredImports).
		for (auto& owner : a_store.TakeRetiredImports())
			a_bindingOwners.push_back(std::move(owner));
		// The frame's part (its number, frame slots, the frame record's buffers and ring indices) is the frame's: a payload built ahead
		// was built a frame before (step 6e E3b).
		const auto& in = a_frame;
		const auto& tables = a_store.GetTables();
		const bool depthOnly = in.depthOnly;
		// Step 6e E4: an installed payload is read from the frame's ring entry, which the frame's producer filled: nothing of it is
		// uploaded here.
		const std::size_t job = depthOnly ? kAsyncZPrepass : kAsyncColour;
		const bool ring = RingFor(a_payload, job);
		if (ring)
			++ringStats.committed;
		const std::uint32_t frameNumber = in.frameNumber;
		auto& textures = GpuTextures::Get();
		auto& mirror = ConstantMirror::Get();
		const bool replayVertexInputs = !depthOnly && prepassInputs;
		CommitUploads uploads(commitStagedPool);
		const std::size_t latchedShape = depthOnly ? kDepthShape : kColourShape;
		// The selected revision's shape, trusted (DecideCoverage covered the frame's main epochs): the commit writes the frame's values
		// into its latch block and layout, its bucket plan and its latched copies, and submits its recording. Nothing of the frame is
		// checked against it here (CS_DCLF_REVISION_PARITY does that).
		const auto revisionEpoch = static_cast<std::uint32_t>(latchedShape);
		const auto revisionShape = a_revision.shape->Value<PassFrame>();
		const PassFrame& shape = *revisionShape;
		// The colour pass's cascades and local shadow volumes (the sun's Accumulate has run, the shadow maps are drawn), as many as the
		// revision's latch holds (its producer sizes it for every shadow view: ReserveMainLatch at the join).
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
		}
		// CS_DCLF_REVISION_PARITY: the commit's own shape against the revision's. It observes only.
		if (RevisionParityEnabled() && ParityDue(frameNumber))
			CheckMainRevision(revisionEpoch, a_capture, a_blocks, a_payload, a_resources, a_store, shape);
		if (sunCascades.size() > shape.latchLayout.cascades || shadowVolumes.size() > shape.latchLayout.shadowVolumes) {
			++revisions.latchClamped[revisionEpoch];
			sunCascades.resize(std::min<std::size_t>(sunCascades.size(), shape.latchLayout.cascades));
			shadowVolumes.resize(std::min<std::size_t>(shadowVolumes.size(), shape.latchLayout.shadowVolumes));
		}
		static const ZBucketPlan kNoPlan;
		const ZBucketPlan& writePlan = shape.zPlan ? *shape.zPlan : kNoPlan;
		const org::LatchBlock& latchBlock = *shape.latch;
		const MainLatchLayout& latchLayout = shape.latchLayout;
		// The per-frame values (the frame constants, the counters, the frame buffers' copies, tree LOD's row): copied from the
		// latch by the epoch's first pass, so this commit records no copy for them.
		auto latched = LatchedUploads(shape.latched, uploads, RenderGraphRuntime::Get().Host()->CurrentFrameSlot());
		auto lap = [&, last = std::chrono::steady_clock::now()](std::size_t a_part) mutable {
			const auto now = std::chrono::steady_clock::now();
			a_stats.commitUs[a_part] += std::chrono::duration<double, std::micro>(now - last).count();
			last = now;
		};
		const std::size_t shapeIndex = latchedShape;

		// The frame's textures (t16 and up), resolved now into the frame record.
		std::array<std::uint32_t, kTextureRegisters> frameTextures;
		frameTextures.fill(kInvalidIndex);
		if (frameTextureGeneration != textures.Generation()) {
			frameTextureBindings = {};
			frameTextureGeneration = textures.Generation();
		}
		for (std::uint32_t t = kPixelTextureSlots; t < kTextureRegisters && !depthOnly; ++t) {
			auto& held = frameTextureBindings[t];
			// The character light's noise is the frame's view (FrameCapture::characterLightView), not a captured register.
			auto* const view = t == kCharacterLightRegister ? a_store.GetFrameCapture().characterLightView : a_capture.psViews[t];
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
			frameRecord.textures[kObjectBufferRegister] = ring ? ringFrame.objectsIndex : in.addresses.objectsIndex;
			frameRecord.textures[kExtrasBufferRegister] = ring ? ringFrame.extrasIndex : in.addresses.extrasIndex;
			frameRecord.textures[kPlacementBufferRegister] = in.addresses.placementsIndex;
			frameRecord.textures[kPaletteBufferRegister] = in.addresses.palettesIndex;
			frameRecord.textures[kShadingBufferRegister] = in.addresses.shadingIndex;
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
		if (!a_resources->sharedLightZeroed) {
			static const std::array<std::uint32_t, kStrictLightDataBytes / 4> zeroLight{};
			uploads(a_resources->frameConstants, zeroLight.data(), sizeof(zeroLight), std::uint64_t(kFrameSlotSharedLight) * kFrameSlotBytes);
			a_resources->sharedLightZeroed = true;
		}
		// The frame lighting, every commit: a latched copy written only when it changed would change the frame's shape with it.
		const auto& frameCapture = a_store.GetFrameCapture();
		latched(a_resources->frameConstants, frameCapture.lighting.data(), sizeof(frameCapture.lighting), std::uint64_t(kFrameSlotLighting) * kFrameSlotBytes);
		// The LOD fades' frame inputs, after the frame lighting in the same block (PS b13, c6): this frame's camera, every
		// epoch (the draw fades specular and envmap by distance, LodFadeFrame).
		const LodFadeFrame lodFadeFrame = SampleLodFadeFrame();
		latched(a_resources->frameConstants, &lodFadeFrame, sizeof(lodFadeFrame), std::uint64_t(kFrameSlotLighting) * kFrameSlotBytes + sizeof(FrameLighting));
		// The frame's fog, into the vertex stage's b13 slot (DCLFFrameFog), every epoch: the technique rows keep theirs.
		latched(a_resources->frameConstants, frameCapture.fog.data(), sizeof(FrameFog), FrameSlotOffset(false, kFrameFogRegister));
		// The extras' frame inputs (ExtrasFrame): the vertex part after the fog (c3-c7), the pixel part after the foliage parity's rows
		// (PS b13, c16). The draw completes each object's static rows from them and its placement.
		const ExtrasFrame extrasFrame = SampleExtrasFrame();
		latched(a_resources->frameConstants, &extrasFrame, kExtrasFrameVertexBytes, FrameSlotOffset(false, kFrameFogRegister) + sizeof(FrameFog));
		latched(a_resources->frameConstants, extrasFrame.projectedGlobals, sizeof(extrasFrame.projectedGlobals), kExtrasPixelFrameOffset);
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
		// Upload (the graph's upload pass runs ahead of every pass of this epoch). An installed payload is the ring entry's (the
		// frame's producer sent it); a build made here is uploaded from its vectors.
		// Rows past the rows' tables wait for their growth (EmitMainRows; the build drew none of them).
		// The inputs: one an object at most, and none for an object past the buffers (ObjectFits).
		CheckSceneCapacity(*a_resources->scene, 0, 0, 0, a_payload.resident.Count() + a_payload.inputList.size(), a_resources->objectCapacity, "the main pass");
		UploadFaceStreams(a_payload.faceStreams, a_resources->scene->facePositions, a_resources->scene->faceUploaded, uploads, a_resources->scene->faceVertices);
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
				std::vector<std::uint32_t> stamped;
				stamped.reserve(counts.size());
				for (const auto& [root, count] : counts) {
					animatedWords[root] = FadeAnimatedWord(a_store.GetFrame(), count);
					stamped.push_back(root);
				}
				std::sort(stamped.begin(), stamped.end());
				SendWordRuns(stamped, [&](std::uint32_t a_first, std::uint32_t a_count) {
					uploads(buffers.fadeAnimated, animatedWords.data() + a_first, std::size_t(a_count) * sizeof(std::uint32_t), std::uint64_t(a_first) * sizeof(std::uint32_t));
				});
				if (anim.varied) {
					static std::uint32_t reported = 0;
					if (reported++ < 5)
						logger::info("[DCLF] the animation job's fade updates of frame {} had differing inputs; the first's stand for all", a_store.GetFrame());
				}
				logBase = NextFadeLog(a_store.GetFrame(), a_store.GetTables(), inputs);
			}
			auto& cull = PrimaryCull::Get();
			UploadFadeRoots(a_store.GetTables(), a_store.GetFrame(), inputs, logBase, buffers, uploads, cull.FadeVisibility(), cull.FadeVisibilityBlocks());
			a_store.FadeRootsSent(buffers.fadeRootsHeld);
			// Each root's list block, when the roots or an entry's list changed (rarely: a new snapshot, a cell's lists).
			const auto& rootTables = a_store.GetTables();
			const std::uint64_t listsKey = rootTables.FadeRootsVersion() * 0x9E3779B97F4A7C15ull ^ cull.FadeRootListsVersion();
			if (buffers.fadeRootLists && buffers.fadeRootListsHeld != listsKey && rootTables.fadeRootNode.size() <= buffers.fadeRootCapacity) {
				std::vector<std::uint32_t> lists;
				cull.FadeRootLists(rootTables.fadeRootNode, lists);
				// The words that differ from what the buffer holds (all of them for a new buffer: fadeRootListsHeld reset).
				auto& sent = buffers.fadeRootListsMirror;
				if (buffers.fadeRootListsHeld == ~0ull)
					sent.clear();
				std::vector<std::uint32_t> differing;
				for (std::uint32_t r = 0; r < lists.size(); ++r)
					if (r >= sent.size() || sent[r] != lists[r])
						differing.push_back(r);
				SendWordRuns(differing, [&](std::uint32_t a_first, std::uint32_t a_count) {
					uploads(buffers.fadeRootLists, lists.data() + a_first, std::size_t(a_count) * sizeof(std::uint32_t), std::uint64_t(a_first) * sizeof(std::uint32_t));
				});
				sent = std::move(lists);
				buffers.fadeRootListsHeld = listsKey;
			}
		}
		// The streams as the tables hold them now, whichever frame's build this is (the ring entry's, filled by the producer).
		const auto streams = ring ? SceneStreams{} : CommitSceneStreams(*a_resources->scene, a_store.GetTables(), a_store.GetFrame(), a_store.GetTablesGeneration(), uploads);
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
		if (!ring)
			UploadMainPayload(a_payload, *a_resources, uploads);
		// The upload sent the rows the tables did not hold: all of them when they fit, else the ones that fit, sent again until
		// the growth is adopted (it then holds the version it was filled with). The ring's path uploaded nothing into them.
		// Rows of the payload's own (built at the epoch) are no version of the coordinator's journal: the tables hold nothing it counts.
		if (ring) {
		} else if (a_payload.foreignRows) {
			a_resources->materialRowsHeld = a_resources->pipelineRowsHeld = 0;
		} else {
			if (a_payload.materialRows.Count() <= a_resources->materialRows.capacity)
				a_resources->materialRowsHeld = a_payload.materialRows.Version();
			if (a_payload.pipelineRows.Count() <= a_resources->pipelineRows.capacity)
				a_resources->pipelineRowsHeld = a_payload.pipelineRows.Version();
		}
		committedMaterialRows = a_payload.materialRows;
		committedPipelineRows = a_payload.pipelineRows;
		// The upload sent the resident region when the buffer held another version of it, and the object records.
		if (!ring)
			a_resources->residentUploaded[depthOnly && a_resources->inputsDepth ? 0 : 1] = a_payload.resident.Version();
		auto& held = a_resources->scene->held;
		const std::size_t objectBytes = streams.objectBytes;
		const std::size_t geometryBytes = EmitGeometryDraws(a_payload.geometryDraws, held.geometries, [](const void*, std::size_t, std::size_t) {}, a_resources->scene->geometryRows);
		if (!ring && a_payload.geometryDraws.Version() && a_payload.geometryDraws.Count() <= a_resources->scene->geometryRows)
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
		for (std::size_t k = 0; k < a_stats.residentResyncBy.size(); ++k)
			a_stats.residentResyncBy[k] += (a_payload.residentResyncReasons >> k) & 1u;
		a_stats.residentDecalRetakes += a_payload.residentDecalRetakes;
		a_stats.residentParityChecks += a_payload.residentParityChecks;
		a_stats.residentParityMismatches += a_payload.residentParityMismatches;
		a_stats.residentMissing += a_payload.residentMissing;
		a_stats.residentPairsChecked += a_payload.residentPairsChecked;
		a_stats.residentPairsStale += a_payload.residentPairsStale;
		a_stats.extraRows = static_cast<std::uint32_t>(streams.extraRowsSent);

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
		// them is a defect of that bound, never a draw to drop - unless their growth is outstanding (Growths), when the draws past
		// them wait for it (BuildDrawsCS drops a slot past its range).
		if (drawCount > a_resources->sequenceDraws) {
			if (drawCount > Growths::Get().LatestSizing<MainSizing>(*a_resources).sequenceDraws)
				stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} draws past the sequence buffer's {} (the scene's draw bound missed them)", drawCount, a_resources->sequenceDraws));
			a_stats.drawsWaiting += drawCount - a_resources->sequenceDraws;
		}
		for (std::uint32_t group = 0; group < kDecalGroups; ++group)
			if (decalCount[group] > a_resources->sequenceDecals)
				stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} decals past the sequence buffer's {} per group", decalCount[group], a_resources->sequenceDecals));
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
			logger::info("[DCLF] culling setup: mode {}, ViewProj {}, VS_PerFrame b{} {}", shape.cullMode, hasViewProj ? "yes" : "no",
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
				depthOnly ? "z-prepass" : "colour", frameNumber, tables.liveObjects, shape.width, shape.height, shape.minDepth, shape.maxDepth,
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
		a_stats.drawn = drawCount;
		++a_stats.commitEpochs;

		// The execution's values, into its slot of the latch (this runs inside the epoch, after the host
		// waited for the slot): what the BuildDraws dispatches of the segment read instead of push constants.
		BuildDrawsLatch latch{};
		latch.placementsIndex = FrameValues::Get().PlacementsIndex();
		if (ring)
			RingLatch(ringFrame, job, latch);
		latch.dispatch[0] = (inputCount + 63) / 64;
		latch.dispatch[1] = 1;
		latch.dispatch[2] = 1;
		latch.drawCount = inputCount;
		latch.cullFlags = hasViewProj ? shape.cullMode : 0u;
		// Every main input is a candidate (the colour segment's undrawable decals among them, which write their slot's blank).
		latch.viewBits = kViewMainCull;
		// The frame number, not the epoch: the depth segment publishes and the colour segment reads within one
		// frame, so the stamp has to be the thing they share.
		latch.visibilityStamp = frameNumber & 0x0FFFFFFFu;  // 28 bits: BuildDrawsCS keeps flags below it
		if (a_resources->hzb && shape.width && shape.height) {
			// Mip 0 covers twice its own size in source pixels, of which only the rendered area holds real
			// depth. A texture coordinate in the image scales by that ratio to reach the HZB.
			const auto scale = [](std::uint32_t a_rendered, std::uint32_t a_covered) {
				const double ratio = a_covered ? std::min(1.0, double(a_rendered) / double(a_covered)) : 0.0;
				return static_cast<std::uint32_t>(std::lround(ratio * 65535.0)) & 0xFFFFu;
			};
			latch.hzbUvScalePacked = scale(shape.width, a_resources->hzbWidth * 2) | (scale(shape.height, a_resources->hzbHeight * 2) << 16);
		}
		FoldEyeIntoViewProj(viewProj, a_capture.eye, latch.viewProj);
		std::copy(std::begin(latch.viewProj), std::end(latch.viewProj), a_payload.cullViewProj.begin());
		a_payload.culled = latch.cullFlags != 0;
		// CS_DCLF_TARGET_PROBE: the objects whose bound covers the probed pixel, nearest first, with their pipelines' descriptors.
		if (!depthOnly && shape.width && shape.height && (frameNumber % 240) == 0) {
			if (const auto& pixel = SwitchValue(Switch::TargetProbe); !pixel.empty()) {
				const auto sep = pixel.find_first_of(",x");
				const float px = std::strtof(pixel.substr(0, sep).c_str(), nullptr) + 32.0f, py = std::strtof(pixel.substr(sep + 1).c_str(), nullptr) + 32.0f;
				const float nx = px / shape.width * 2.0f - 1.0f, ny = 1.0f - py / shape.height * 2.0f;
				struct Hit { float w; std::uint32_t object; };
				std::vector<Hit> hits;
				for (std::uint32_t o = 0; o < tables.objects.size() && o < tables.objectGeometry.size(); ++o) {
					// The bound now, from the engine (render thread).
					const auto* geometry = tables.objectGeometry[o];
					if (!geometry || geometry->worldBound.radius <= 0.0f)
						continue;
					const auto& bound = geometry->worldBound;
					float clip[4];
					for (std::uint32_t row = 0; row < 4; ++row)
						clip[row] = latch.viewProj[row * 4] * bound.center.x + latch.viewProj[row * 4 + 1] * bound.center.y + latch.viewProj[row * 4 + 2] * bound.center.z +
						            latch.viewProj[row * 4 + 3];
					if (clip[3] <= 1.0f)
						continue;
					// The sphere's screen radius, roughly (the projection's x scale over the distance).
					const float scale = std::sqrt(latch.viewProj[0] * latch.viewProj[0] + latch.viewProj[1] * latch.viewProj[1] + latch.viewProj[2] * latch.viewProj[2]);
					const float radius = bound.radius * scale / clip[3];
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
			const auto& layout = latchLayout;
			UpdateIndexPool(*a_resources->pool, a_store.GetTables(), a_store.GetTablesGeneration(), latchBlock, latchSlot, layout.PoolOffset(), uploads);
			// The plan's slots, and none for every other pipeline slot the installed publication's members can draw with: gained since
			// the plan was made, or past the bucket layout while its growth is outstanding (Growths). None of their draws is the frame's.
			const auto planned = static_cast<std::uint32_t>(writePlan.map.size());
			auto slots = std::max({ planned, static_cast<std::uint32_t>(a_resources->zBucketCapacity.size()), InstalledPipelineSlots() });
			// Past the revision's bucket map: slots no member draws with, or (flagged under CS_DCLF_REVISION_PARITY) a member's the
			// revision's latch does not hold.
			if (slots > layout.buckets) {
				if (RevisionParityEnabled() && MemberPastSlots(layout.buckets))
					++revisions.latchClamped[revisionEpoch];
				slots = layout.buckets;
			}
			const std::uint32_t buckets = writePlan.Buckets();
			if (planned)
				LatchWrite(latchBlock, "main bucket map", latchSlot, layout.BucketMapOffset(), std::as_bytes(std::span(writePlan.map.data(), planned)));
			if (slots > planned) {
				zBucketZeros.assign(slots - planned, kNoBucket);
				LatchWrite(latchBlock, "main bucket map", latchSlot, layout.BucketMapOffset() + planned * static_cast<std::uint32_t>(sizeof(std::uint32_t)),
					std::as_bytes(std::span(zBucketZeros.data(), slots - planned)));
			}
			if (buckets) {
				const std::size_t words = std::size_t(buckets) * 2;
				LatchWrite(latchBlock, "main bucket tables", latchSlot, layout.BucketTableOffset(), std::as_bytes(std::span(writePlan.table.data(), words)));
				LatchWrite(latchBlock, "main bucket tables", latchSlot, layout.PhaseTwoBucketTableOffset(), std::as_bytes(std::span(writePlan.table.data() + words, words)));
				zBucketZeros.assign(std::max<std::size_t>(zBucketZeros.size(), buckets), 0u);
				for (const auto& counts : a_resources->zBucketCounts)
					latched(counts, zBucketZeros.data(), std::size_t(buckets) * sizeof(std::uint32_t), 0);
			}
			const auto region = static_cast<std::uint32_t>(latchBlock.Offset(latchSlot));
			latch.bucketTableOffset = region + layout.BucketTableOffset();
			latch.phaseTwoBucketTableOffset = region + layout.PhaseTwoBucketTableOffset();
			latch.bucketMapOffset = region + layout.BucketMapOffset();
		}
		if (!depthOnly) {
			const auto& layout = latchLayout;
			const std::uint32_t header[4] = { static_cast<std::uint32_t>(sunCascades.size()), 0, 0, 0 };
			LatchWrite(latchBlock, "main sun cascades", latchSlot, MainLatchLayout::CascadeOffset(), std::as_bytes(std::span(header)));
			if (!sunCascades.empty())
				LatchWrite(latchBlock, "main sun cascades", latchSlot, MainLatchLayout::CascadeOffset() + kSunRegionHeader, std::as_bytes(std::span(sunCascades)));
			const std::uint32_t volumeHeader[4] = { static_cast<std::uint32_t>(shadowVolumes.size()), 0, 0, 0 };
			LatchWrite(latchBlock, "main shadow volumes", latchSlot, layout.ShadowVolumeOffset(), std::as_bytes(std::span(volumeHeader)));
			if (!shadowVolumes.empty())
				LatchWrite(latchBlock, "main shadow volumes", latchSlot, layout.ShadowVolumeOffset() + kSunRegionHeader, std::as_bytes(std::span(shadowVolumes)));
			latch.sunState = kSunTestOn;
			latch.sunCascadeOffset = static_cast<std::uint32_t>(latchBlock.Offset(latchSlot)) + MainLatchLayout::CascadeOffset();
			latch.localShadowOffset = static_cast<std::uint32_t>(latchBlock.Offset(latchSlot)) + layout.ShadowVolumeOffset();
			sunUpload = latch;
		}
		LatchWriteValue(latchBlock, "main culling latch", latchSlot, 0, latch);
		// What the reflection's faces, early next frame, draw from (ExecuteReflection): this commit's inputs and the buffers' backings.
		a_resources->committed[shapeIndex] = { frameNumber, inputCount, a_resources->scene->generation, a_resources->objectCapacity, a_resources->MainRowsGeneration() };

		(void)latched.Finish();
		revisions.latchedMisses += latched.Misses();
		SubmitRevisionRecording(revisionEpoch, *a_revision.recordings, 0);
		lap(6);
		return true;
	}

	void IndirectDraws::Impl::CheckMainRevision(std::uint32_t a_epoch, const Capture& a_capture, const FrameBlocks& a_blocks, const MainPayload& a_payload,
		const std::shared_ptr<Resources>& a_resources, SceneStore& a_store, const PassFrame& a_shape)
	{
		using R = SceneRevisions;
		const bool depthOnly = a_epoch == kDepthShape;
		const auto& tables = a_store.GetTables();
		const auto indirect = GetIndirectState();
		// The commit's own shape, as its frame's inputs make it (MakeMainShape), for the shape parity.
		ZBucketPlan plan;
		if (depthOnly && a_resources->pool)
			PlanZBuckets(*a_resources, a_store.GetLookups(), tables, indirect, plan);
		FrameBlockSizes blockSizes;
		for (std::uint32_t slot = 0; slot < kConstantBufferRegisters; ++slot) {
			blockSizes.vs[slot] = static_cast<std::uint32_t>(a_blocks.vs[slot].size());
			blockSizes.ps[slot] = static_cast<std::uint32_t>(a_blocks.ps[slot].size());
		}
		auto in = MainShapeInputsOf(*a_resources, depthOnly);
		const bool mainRange = mainMaxDepth > 0.0f;
		const MainViewport viewport{ a_capture.viewportWidth, a_capture.viewportHeight, mainRange ? mainMinDepth : a_capture.minDepth,
			mainRange ? mainMaxDepth : a_capture.maxDepth };
		in.viewport = viewport;
		in.resourceHeap = org::runtime::GetActiveSRVDescriptorHeap().GetHandle();
		in.samplerHeap = org::runtime::GetActiveSamplerDescriptorHeap().GetHandle();
		in.indirect = indirect;
		in.zCalls = plan.calls;
		in.zPlan = std::make_shared<const ZBucketPlan>(plan);
		in.latched.copies = MainLatchedLayout(*a_resources, depthOnly, blockSizes, plan.Buckets());
		if (!in.latched.copies.empty())
			in.latched.latch = a_resources->latchedBlocks[a_epoch];
		NoteShapeParity(a_epoch, *MakeMainShape(in), in.latched.copies, a_store.GetFrame());
		// What the revision's shape must hold for the frame (what a commit would have checked before writing into it).
		const auto& active = revisions.active;
		const auto& versionsFragment = active ? active->Fragment(R::kVersionsSlot) : nullptr;
		const auto versions = versionsFragment ? versionsFragment->Value<VersionSet>() : nullptr;
		const auto draws = static_cast<std::uint32_t>(a_payload.sequences.size() + a_payload.residentDraws);
		bool decalsFit = true;
		for (std::uint32_t group = 0; group < kDecalGroups && !depthOnly; ++group)
			decalsFit &= a_payload.decalCount[group] <= a_shape.decalCapacity[group];
		std::uint32_t miss = R::kMisses;
		std::string detail;
		if (!versions || versions->changes != VersionRegistry::Get().changes)
			miss = R::kVersions;
		else if (!a_shape.latch)
			miss = R::kNoRecording;
		else if (!indirect.valid || !a_shape.indirect.valid || !SameHandle(a_shape.indirect.layout, indirect.layout) || !SameHandle(a_shape.resourceHeap, in.resourceHeap) ||
				 !SameHandle(a_shape.samplerHeap, in.samplerHeap) || (a_shape.indirect.version != indirect.version && !RevisionHoldsClaims()))
			miss = R::kPipelines;
		else if (!(MainViewport{ a_shape.width, a_shape.height, a_shape.minDepth, a_shape.maxDepth } == viewport)) {
			miss = R::kViewport;
			detail = fmt::format("{}x{} [{}, {}], the frame's {}x{} [{}, {}]", a_shape.width, a_shape.height, a_shape.minDepth, a_shape.maxDepth, viewport.width,
				viewport.height, viewport.minDepth, viewport.maxDepth);
		} else if (a_shape.cullMode != ActiveToggles().cullMode)
			miss = R::kShape;
		else if (draws > a_shape.drawCapacity || !decalsFit) {
			miss = R::kCapacity;
			detail = fmt::format("{} draws for {}; decals", draws, a_shape.drawCapacity);
			for (std::uint32_t group = 0; group < kDecalGroups; ++group)
				detail += fmt::format(" {}/{}", a_payload.decalCount[group], a_shape.decalCapacity[group]);
		} else if (!depthOnly && (sunCascades.size() > a_shape.latchLayout.cascades || shadowVolumes.size() > a_shape.latchLayout.shadowVolumes)) {
			miss = R::kLatch;
			detail = fmt::format("{} cascades for {}, {} shadow volumes for {}", sunCascades.size(), a_shape.latchLayout.cascades, shadowVolumes.size(),
				a_shape.latchLayout.shadowVolumes);
		} else if (depthOnly && a_resources->pool &&
				   (!a_shape.zPlan || std::max({ a_resources->zBucketCapacity.size(), tables.pipelines.size(), a_shape.zPlan->map.size() }) > a_shape.latchLayout.buckets)) {
			miss = R::kLatch;
			detail = fmt::format("{} pipeline slots for {} buckets", std::max(a_resources->zBucketCapacity.size(), tables.pipelines.size()), a_shape.latchLayout.buckets);
		}
		++revisions.parityChecks[a_epoch];
		if (miss == R::kMisses)
			return;
		NoteRevisionMiss(a_epoch, miss);
		static std::uint32_t logged = 0;
		if (logged++ < 16)
			logger::warn("[DCLF] revision parity: the {} commit of frame {} does not fit the selected revision's shape: {}{} <- REVISION", SceneRevisions::kNames[a_epoch],
				a_store.GetFrame(), SceneRevisions::kMissNames[miss], detail.empty() ? std::string() : " (" + detail + ")");
	}
}

#endif

namespace DCLF
{
	bool IndirectDraws::DecideTreeLod()
	{
		bool owned = false;
		auto* buffers = impl->scene.get();
		// A frame without claims (SceneStore::WithdrawSet) is the engine's whole: its tree LOD too.
		if (buffers && buffers->treeLodCull && ActiveToggles().lodTrees && SceneStore::Get().TreeLodMirror().Size() && !SceneStore::Get().SetWithdrawn()) {
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
		if (!impl->scene || !impl->scene->fadeWriteBack)
			return;
		auto& writeBack = *impl->scene->fadeWriteBack;
		// The stores the last finished task found (step 6e F1): engine writes are the render thread's, here at the frame's start, with
		// the engine's update done and before the culls read the fades. Their snapshot is let go after.
		if (std::unique_ptr<FadeWriteBack::Stores> stores{ writeBack.ready.exchange(nullptr, std::memory_order_acq_rel) }) {
			ZoneScopedN("CS.DCLF.FadeWriteBack.Stores");
			const auto start = std::chrono::steady_clock::now();
			for (const auto& store : stores->stores) {
				// The fade, and the fade bits of the flags (atomically: the engine owns the others). The fade watch is not told: a
				// stood-in root's dependents read its fade from FadeStateCS's state (SceneStore::MarkFadeRootOwned).
				auto* node = static_cast<std::byte*>(store.node);
				std::atomic_ref<std::uint32_t> flags(*reinterpret_cast<std::uint32_t*>(node + 0xF4));
				flags.fetch_and(~kFadeFlagMask, std::memory_order_relaxed);
				flags.fetch_or(store.flags, std::memory_order_relaxed);
				*reinterpret_cast<float*>(node + 0x130) = store.fade;
			}
			writeBack.applied.fetch_add(stores->stores.size(), std::memory_order_relaxed);
			writeBack.storeNs.fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count()),
				std::memory_order_relaxed);
		}
		// One task at a time; one still running takes the new batches with it (or the next task does).
		if (bool idle = false; !writeBack.running.compare_exchange_strong(idle, true, std::memory_order_acq_rel)) {
			writeBack.busy.fetch_add(1, std::memory_order_relaxed);
			return;
		}
		// The task judges the events against the frame's tables (immutable), which the stores then hold: so the publication's
		// retirement node, and the root nodes it names (step 6e E3a).
		std::shared_ptr<const SceneStore::Tables> tables = SceneStore::Get().AcceptedTables();
		auto find = [scene = impl->scene, tables] {
			ZoneScopedN("CS.DCLF.FadeWriteBack");
			auto& state = *scene->fadeWriteBack;
			// The batches since, in frame order (the stack hands them back newest first, and two recordings need not finish in their
			// frames' order).
			std::vector<FadeWriteBack::Batch*> batches;
			for (auto* batch = state.batches.exchange(nullptr, std::memory_order_acquire); batch; batch = batch->next)
				batches.push_back(batch);
			std::reverse(batches.begin(), batches.end());
			std::stable_sort(batches.begin(), batches.end(), [](const auto* a_a, const auto* a_b) { return a_a->frame < a_b->frame; });
			auto stores = std::make_unique<FadeWriteBack::Stores>();
			stores->tables = tables;
			for (auto* batch : batches) {
				for (const auto& event : batch->events) {
					if (!tables)
						break;
					if (event.root >= state.appliedFrame.size())
						state.appliedFrame.resize(std::size_t(event.root) + 1, 0u);
					// The root the event was of, still stood in: one listed again since, or the engine's own again, keeps its node; and
					// an event older than the one taken last is not written over it.
					if (event.root >= tables->fadeRoots.size() || tables->fadeRoots[event.root].generation != event.generation ||
						!(tables->fadeRoots[event.root].bits & kFadeRootStoodIn) || !tables->fadeRootNode[event.root] || batch->frame < state.appliedFrame[event.root]) {
						state.stale.fetch_add(1, std::memory_order_relaxed);
						continue;
					}
					state.appliedFrame[event.root] = batch->frame;
					stores->stores.push_back({ const_cast<void*>(tables->fadeRootNode[event.root]), event.flags & kFadeFlagMask, event.currentFade });
				}
				delete batch;
			}
			// What an earlier task found and no frame start has made yet goes first (a task runs only after the frame's start has taken
			// the last one's, so this is only the toggle's drain).
			if (std::unique_ptr<FadeWriteBack::Stores> earlier{ state.ready.exchange(nullptr, std::memory_order_acq_rel) }) {
				earlier->stores.insert(earlier->stores.end(), stores->stores.begin(), stores->stores.end());
				earlier->tables = std::move(stores->tables);
				stores = std::move(earlier);
			}
			if (!stores->stores.empty())
				state.ready.store(stores.release(), std::memory_order_release);
			state.running.store(false, std::memory_order_release);
			state.running.notify_all();
		};
		if (!SceneScheduler::Executor().Dispatch(SceneScheduler::Scope(), PublishedSceneExecutor::Preparation, org::async::TaskDispatch::Cpu, "fade write-back",
				[find = std::move(find)](const auto&) { find(); }))
			stl::report_and_fail("Drawcall Limit Fix: the fade write-back was refused by DCLF's executor");
	}

	void IndirectDraws::Impl::WaitFadeWriteBack()
	{
		if (!scene || !scene->fadeWriteBack)
			return;
		auto& running = scene->fadeWriteBack->running;
		while (running.load(std::memory_order_acquire))
			running.wait(true, std::memory_order_acquire);
		// Teardown, the toggle, a load: what it found is not written (the roots may stand in no more).
		delete scene->fadeWriteBack->ready.exchange(nullptr, std::memory_order_acq_rel);
	}
}
