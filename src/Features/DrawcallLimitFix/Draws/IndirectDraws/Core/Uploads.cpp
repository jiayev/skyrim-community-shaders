#if defined(CS_HAS_RENDER_GRAPH) && defined(CS_HAS_ORG_MODULE_SERVICES)
#	include "Features/DrawcallLimitFix/Draws/IndirectDraws/Internal.h"
#	include "Features/DrawcallLimitFix/Scene/Coordinator/Coordinator.h"
#include "Features/DrawcallLimitFix/Scene/FrameView/FrameView.h"
#	include "Features/TerrainBlending.h"
#	include "Features/DrawcallLimitFix/Diagnostics/Draws/Events.h"
#	include "Features/DrawcallLimitFix/Diagnostics/Reports/Draws.h"

#	include "Features/DrawcallLimitFix/Engine/Capture/SceneTracker.h"

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
		StreamsKey StreamsKeyOf(const SceneTables& a_tables, std::uint32_t a_generation)
		{
			return { &a_tables, a_tables.changeLog.End(), a_tables.geometryLog.End(), a_generation };
		}

	}

	/** @brief The stores brought up to the tables (from what the buffers hold), as views (StreamViews). */
	std::shared_ptr<const StreamViews> Draws::MakeStreamViews(ObjectRecordStore& a_objects, ExtrasStore& a_extras, GeometryStore& a_geometries, const TablesHeld& a_from,
			std::shared_ptr<const SceneTables> a_hold, const SceneTables& a_tables, std::uint32_t a_generation, std::uint32_t a_frame)
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
		auto& store = Scene::Coordinator::Get();
		const auto& tables = Scene::FrameView::Get().GetTables();
		if (installedDraws && installedDraws->tables.get() == &tables)
			return installedDraws->streams;
		const auto key = StreamsKeyOf(tables, Scene::FrameView::Get().GetTablesGeneration());
		if (streamViews && streamViewsKey == key)
			return streamViews;
		if (!resources || !resources->scene || tables.objects.empty())
			return nullptr;
		if (store.SceneTaskInFlight() || aheadDone.load(std::memory_order_acquire) < aheadKicked.load(std::memory_order_acquire)) {
			++streamsRefused;
			return nullptr;
		}
		// Their journals keep what the scene buffers and the ring's entries lack.
		const auto& held = resources->scene->held;
		const auto& holders = ringHolders;
		const TablesHeld from{ std::min(held.objects, holders.objects.Oldest()), std::min(held.extras, holders.extras.Oldest()),
			std::min(held.geometries, holders.geometries.Oldest()) };
		streamViews = MakeStreamViews(objectStore, extrasStore, geometryStore, from, Scene::FrameView::Get().AcceptedTables(), tables, Scene::FrameView::Get().GetTablesGeneration(), Scene::FrameView::Get().GetFrame());
		streamViewsKey = key;
		return streamViews;
	}

	IndirectDraws::Impl::SceneStreams IndirectDraws::Impl::CommitSceneStreams(SceneBuffers& a_scene, const SceneTables& a_tables, std::uint32_t a_frame,
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
		std::vector<float>* extrasMirror = nullptr;
		Diagnostics::Notify(Diagnostics::ExtrasSending{ Diagnostics::ExtrasSending::kSceneBuffers, extrasMirror });
		sent.extraRowsSent = EmitExtras(extras, a_scene.held.extras, extrasMirror, [&](const void* a_data, std::size_t a_bytes, std::size_t a_offset) {
			a_uploads(a_scene.extras, a_data, a_bytes, a_offset);
		}, a_scene.extraRows);
		Diagnostics::Notify(Diagnostics::ExtrasSent{ Diagnostics::ExtrasSending::kSceneBuffers, a_frame, extras, a_scene.extraRows });
		if (extras.Version() && extras.Rows() <= a_scene.extraRows)
			a_scene.held.extras = extras.Version();
		extrasStore.rowsSent += sent.extraRowsSent;
		return sent;
	}

	bool IndirectDraws::Impl::CommitMainPayload(const Capture& a_capture, const FrameBlocks& a_blocks, const MainInputs& a_frame, MainPayload& a_payload,
		const std::shared_ptr<Resources>& a_resources, IndirectDraws::Stats& a_stats,
		std::vector<std::shared_ptr<const void>>& a_bindingOwners, const EpochRevision& a_revision)
	{
		ZoneScopedN("CS.DCLF.CommitMainPayload");
		// The cleared geometry slots' buffers, held until this execution retires (Coordinator::TakeRetiredImports).
		for (auto& owner : Scene::FrameView::Get().TakeRetiredImports())
			a_bindingOwners.push_back(std::move(owner));
		// The frame's part (its number, frame slots, the frame record's buffers and ring indices) is the frame's: a payload built ahead
		// was built a frame before (step 6e E3b).
		const auto& in = a_frame;
		const auto& tables = Scene::FrameView::Get().GetTables();
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
		// checked against it.
		const auto revisionEpoch = static_cast<std::uint32_t>(latchedShape);
		const auto revisionShape = a_revision.shape->Value<PassFrame>();
		const PassFrame& shape = *revisionShape;
		// The colour pass's cascades and local shadow volumes (the sun's Accumulate has run, the shadow maps are drawn), as many as the
		// revision's latch holds (its producer sizes it for every shadow view: ReserveMainLatch at the join).
		if (!depthOnly) {
			SunAccumulation::Get().GpuCascades(sunCascades);
			// Frames whose sun test runs without cascades: every pass with the sun's bits draws unshadowed (right only when the sun did
			// not accumulate).
			sunTestEmpty += sunCascades.empty() ? 1 : 0;
			if (sunTestWindow.Tick())
				Reports::ColourSunTestLine(std::exchange(sunTestEmpty, 0));
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
		// T7c1: Terrain Blending's mask (TerrainBlending.hlsli, t55), which its terrain's pixel stage reads, is that feature's terrain depth -
		// bound by its replay after this epoch (RenderTerrainBlendingPasses), so named here rather than captured. By the colour epoch it
		// holds the main depth as the Z-prepass left it (TerrainBlending::BlendPrepassDepths copies it there).
		constexpr std::uint32_t kTerrainBlendingMaskRegister = 55;
		const auto& terrainBlending = globals::features::terrainBlending;
		auto* const terrainMask = TerrainBlendingDefersTerrain() ? terrainBlending.terrainDepth.depthSRV : nullptr;
		// T7d's Effect port: the soft effects' depth (kEffectSoftDepthRegister): the depth copy SetupTechnique binds for a Soft pass in the main
		// view (kPOST_ZPREPASS_COPY), which Community Shaders fills from the main depth after the deferred pass, before the forward epoch reads it.
		auto* const effectSoftDepth = globals::game::renderer ?
		                                  reinterpret_cast<ID3D11ShaderResourceView*>(
											  globals::game::renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY].depthSRV) :
		                                  nullptr;
		for (std::uint32_t t = kPixelTextureSlots; t < kTextureRegisters && !depthOnly; ++t) {
			auto& held = frameTextureBindings[t];
			// The character light's noise is the frame's view (FrameCapture::characterLightView), not a captured register.
			auto* const view = t == kCharacterLightRegister                         ? Scene::FrameView::Get().GetFrameCapture().characterLightView :
			                   (t == kTerrainBlendingMaskRegister && terrainMask) ? terrainMask :
			                   t == kEffectSoftDepthRegister                      ? effectSoftDepth :
			                                                                         a_capture.psViews[t];
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
			// The fades every draw's alpha takes (T1c): the states the frame's builds read, and the roots' static rows.
			if (const auto& sceneBuffers = *a_resources->scene; sceneBuffers.fadeRoots && sceneBuffers.fadeRootsIndex) {
				frameRecord.textures[kFadeStatesRegister] = sceneBuffers.FadeStatesReadIndex(frameNumber);
				frameRecord.textures[kFadeRootsRegister] = sceneBuffers.fadeRootsIndex;
			}
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
		const auto& frameCapture = Scene::FrameView::Get().GetFrameCapture();
		latched(a_resources->frameConstants, frameCapture.lighting.data(), sizeof(frameCapture.lighting), std::uint64_t(kFrameSlotLighting) * kFrameSlotBytes);
		// The LOD fades' frame inputs, after the frame lighting in the same block (PS b13, c6): this frame's camera, every
		// epoch (the draw fades specular and envmap by distance, LodFadeFrame).
		const LodFadeFrame lodFadeFrame = SampleLodFadeFrame();
		latched(a_resources->frameConstants, &lodFadeFrame, sizeof(lodFadeFrame), std::uint64_t(kFrameSlotLighting) * kFrameSlotBytes + sizeof(FrameLighting));
		// The frame's fog, into the vertex stage's b13 slot (DCLFFrameFog), every epoch: the technique rows keep theirs.
		latched(a_resources->frameConstants, frameCapture.fog.data(), sizeof(FrameFog), FrameSlotOffset(false, kFrameFogRegister));
		// The extras' frame inputs (ExtrasFrame): the vertex part after the fog (c3-c7), the pixel part after the foliage record's rows
		// (PS b13, c16). The draw completes each object's static rows from them and its placement.
		const ExtrasFrame extrasFrame = SampleExtrasFrame();
		latched(a_resources->frameConstants, &extrasFrame, kExtrasFrameVertexBytes, FrameSlotOffset(false, kFrameFogRegister) + sizeof(FrameFog));
		latched(a_resources->frameConstants, extrasFrame.projectedGlobals, sizeof(extrasFrame.projectedGlobals), kExtrasPixelFrameOffset);
		// T7d's Effect port: the camera's planes the soft effects read (PS b13 c17, after the extras' row: Effect.hlsl, DCLFEffectCamera).
		const auto effectCamera = SampleEffectCamera();
		latched(a_resources->frameConstants, effectCamera.data(), sizeof(effectCamera), kEffectCameraFrameOffset);
		// Its Lighting variant's DLightColor (PS b13 c18: Effect.hlsl, DCLFEffectDLightColor), the sun light's effect colour this frame.
		const auto effectLight = SampleEffectLight();
		latched(a_resources->frameConstants, effectLight.data(), sizeof(effectLight), kEffectLightFrameOffset);
		// Its Blood's AlphaTestRef.yz (PS b13 c19: Effect.hlsl, DCLFEffectBlood), the weapon blood's two globals.
		const auto effectBlood = SampleEffectBlood();
		latched(a_resources->frameConstants, effectBlood.data(), sizeof(effectBlood), kEffectBloodFrameOffset);
		// The foliage record (Policy::RecordFoliagePixels): the colour epoch's buffers (by its parity) and its tag, after the LOD fades in
		// the same block (PS b13, c14), and the compare pass's counters zeroed.
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
		UploadTrees(Scene::FrameView::Get().GetTables(), Scene::FrameView::Get().GetFrame(), *a_resources->scene, uploads, depthOnly);
		// Tree LOD (dclf-lod.md, "Tree LOD: the draws"): the mirror's changes, the draw row and the list's arguments, for the depth
		// epoch's cull and both passes' draws; it draws while its toggle is on and its programs and pipelines are built.
		if (depthOnly && a_resources->scene->treeLodCull) {
			auto& sceneBuffers = *a_resources->scene;
			UploadTreeLod(Scene::FrameView::Get().TreeLodMirror(), sceneBuffers, latched, a_bindingOwners, treeLodOwned);
			// The registrations withheld the engine's passes on the frame's decision: a commit that cannot draw leaves a hole.
			if (treeLodOwned && !sceneBuffers.treeLodReady.load(std::memory_order_acquire) && treeLodMissed++ < 8)
				logger::warn("[DCLF] tree LOD: frame {} withheld the engine's passes but the depth commit could not draw (tables {} of {} shape slots)", frameNumber,
					Scene::FrameView::Get().TreeLodMirror().ShapeSlots(), sceneBuffers.treeLodShapeCapacity);
		}
		// The fade roots, and once a frame their inputs, for FadeStateCS ahead of the depth segment's culling: the main camera
		// the list jobs cull with (PrimaryCull::FadeEye) and the engine's fade globals.
		if (depthOnly && a_resources->scene->fadeState) {
			auto& buffers = *a_resources->scene;
			std::uint32_t logBase = buffers.fadeLogBase;
			FadeFrame inputs = buffers.fadeFrame;
			if (buffers.fadeFrameNumber != Scene::FrameView::Get().GetFrame()) {
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
				const auto& index = Scene::FrameView::Get().GetTables().fadeRootIndex;
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
					animatedWords[root] = FadeAnimatedWord(Scene::FrameView::Get().GetFrame(), count);
					stamped.push_back(root);
				}
				std::sort(stamped.begin(), stamped.end());
				SendWordRuns(stamped, [&](std::uint32_t a_first, std::uint32_t a_count) {
					uploads(buffers.fadeAnimated, animatedWords.data() + a_first, std::size_t(a_count) * sizeof(std::uint32_t), std::uint64_t(a_first) * sizeof(std::uint32_t));
				});
				if (anim.varied) {
					static std::uint32_t reported = 0;
					if (reported++ < 5)
						logger::info("[DCLF] the animation job's fade updates of frame {} had differing inputs; the first's stand for all", Scene::FrameView::Get().GetFrame());
				}
				// The frame's FadeStateCS log window, when an observer asks for one (CS_DCLF_FADE_PARITY).
				logBase = ~0u;
				Diagnostics::Notify(Diagnostics::FadeLogWindow{ Scene::FrameView::Get().GetFrame(), Scene::FrameView::Get().GetTables(), inputs, logBase });
			}
			UploadFadeRoots(Scene::FrameView::Get().GetTables(), Scene::FrameView::Get().GetFrame(), inputs, logBase, buffers, uploads, PortalViews::Get().Encoded());
			Scene::FrameView::Get().FadeRootsSent(buffers.fadeRootsHeld);
			// T7b6: the fade events' header and the frame's readback slot (the slot's last execution harvested first).
			ArmFadeReadback(buffers, uploads);
			// T7b1 (CS_DCLF_ACCUMULATED_PARITY): the visibility events' count and the frame's readback slot.
			ArmVisibilityReadback(buffers, uploads);
			// Each root's portal program (its room's), when the roots or a room's children changed, or the graph (PortalViews::ProgramOf).
			const auto& rootTables = Scene::FrameView::Get().GetTables();
			auto& portals = PortalViews::Get();
			const std::uint64_t programsKey = rootTables.FadeRootsVersion() * 0x9E3779B97F4A7C15ull ^ portals.ProgramsVersion();
			if (buffers.fadeRootPrograms && buffers.fadeRootProgramsHeld != programsKey && rootTables.fadeRootNode.size() <= buffers.fadeRootCapacity) {
				std::vector<std::uint32_t> programs(rootTables.fadeRootNode.size(), kPortalNoProgram);
				for (std::size_t r = 0; r < programs.size(); ++r)
					if (const auto* node = static_cast<const RE::NiAVObject*>(rootTables.fadeRootNode[r]))
						programs[r] = portals.ProgramOf(node);
				// The words that differ from what the buffer holds (all of them for a new buffer: fadeRootProgramsHeld reset).
				auto& sent = buffers.fadeRootProgramsMirror;
				if (buffers.fadeRootProgramsHeld == ~0ull)
					sent.clear();
				std::vector<std::uint32_t> differing;
				for (std::uint32_t r = 0; r < programs.size(); ++r)
					if (r >= sent.size() || sent[r] != programs[r])
						differing.push_back(r);
				SendWordRuns(differing, [&](std::uint32_t a_first, std::uint32_t a_count) {
					uploads(buffers.fadeRootPrograms, programs.data() + a_first, std::size_t(a_count) * sizeof(std::uint32_t), std::uint64_t(a_first) * sizeof(std::uint32_t));
				});
				sent = std::move(programs);
				buffers.fadeRootProgramsHeld = programsKey;
			}
		}
		// The streams as the tables hold them now, whichever frame's build this is (the ring entry's, filled by the producer).
		const auto streams = ring ? SceneStreams{} : CommitSceneStreams(*a_resources->scene, Scene::FrameView::Get().GetTables(), Scene::FrameView::Get().GetFrame(), Scene::FrameView::Get().GetTablesGeneration(), uploads);
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
			// T7c1: the terrain groups' counts, which their draws append through (BuildDrawsCS).
			latched(a_resources->count, kZeroCounts, kTerrainGroups * sizeof(std::uint32_t), kCountTerrainGroupWord * sizeof(std::uint32_t));
			// T7d: the forward range's count (its draw's) and the forward culling's tallies (BuildDrawsCS).
			latched(a_resources->count, kZeroCounts, 4 * sizeof(std::uint32_t), kCountForwardWord * sizeof(std::uint32_t));
			// T7d's refraction: the refraction range's count (its draw's) and the refraction culling's tallies (BuildDrawsCS).
			latched(a_resources->count, kZeroCounts, 4 * sizeof(std::uint32_t), kCountRefractionWord * sizeof(std::uint32_t));
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
		// What a deferred growth of the rows' tables is filled with: posted for the revision code (it reads its copy, never this commit's).
		committedRowsSlot.Post(std::make_unique<CommittedRows>(CommittedRows{ a_payload.materialRows, a_payload.pipelineRows }));
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
		Diagnostics::Notify(Diagnostics::MainPayloadCommitted{ a_payload });
		a_stats.extraRows = static_cast<std::uint32_t>(streams.extraRowsSent);

		lap(4);
		// The build's stats.
		a_stats.skipped = a_payload.skipped;
		a_stats.missingTextures = a_payload.missingTextures;
		a_stats.missingVertexConstants = a_payload.missingVertexConstants;
		a_stats.missingPixelConstants = a_payload.missingPixelConstants;
		a_stats.deferredTextures = a_payload.deferredTextures;
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
			if (drawCount > a_resources->askedSequenceDraws.load(std::memory_order_acquire))
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
		// The colour epoch's VS_PerFrame block (the replayed Z-prepass bytes, or the capture's), for the observers (CS_DCLF_STATS).
		if (!depthOnly && Diagnostics::Armed<Diagnostics::ColourFrameBlock>()) {
			std::span<const std::byte> packed;
			if (replayVertexInputs)
				packed = prepassVS[kPerFrameVertexRegister];
			else if (auto* buffer = a_capture.vsBuffers[kPerFrameVertexRegister])
				packed = mirror.Contents(buffer);
			Diagnostics::Notify(Diagnostics::ColourFrameBlock{ a_capture, packed });
		}
		a_stats.drawn = drawCount;
		++a_stats.commitEpochs;

		// The execution's values, into its slot of the latch (this runs inside the epoch, after the host
		// waited for the slot): what the BuildDraws dispatches of the segment read instead of push constants.
		BuildDrawsLatch latch{};
		latch.placementsIndex = FrameValues::Get().PlacementsIndex();
		latch.fadeSeedsIndex = FrameValues::Get().FadeSeedsIndex();
		if (ring)
			RingLatch(ringFrame, latch);
		latch.dispatch[0] = (inputCount + 63) / 64;
		latch.dispatch[1] = 1;
		latch.dispatch[2] = 1;
		latch.drawCount = inputCount;
		latch.cullFlags = hasViewProj ? shape.cullMode : 0u;
		// The one main list (U4a): the depth segment culls every candidate, the colour segment draws the members and writes every
		// decal's slot (an undrawable one's blank).
		latch.viewBits = depthOnly ? kDepthViewBits : kColourViewBits;
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
		Diagnostics::Notify(Diagnostics::MainCullViewProj{ latch, shape.width, shape.height, frameNumber, depthOnly, tables });
		// The depth segment: the camera the fade roots' distances are measured from (kObjectFadeTest).
		if (depthOnly) {
			const auto eye = PrimaryCull::Get().FadeEye();
			std::copy(eye.begin(), eye.end(), latch.fadeEye);
			const auto treeHeight = PrimaryCull::Get().TreeHeightTest();
			latch.treeHeight[0] = treeHeight[0];
			latch.treeHeight[1] = treeHeight[1];
			// T7c1: Terrain Blending's near test (TerrainBlending.cpp, its depth hook), with its eye as it took it at the depth pass's
			// start, for the terrain group 1 draws' twin buckets (BuildDrawsCS, kBucketTerrainPair). None while it does not hold the terrain.
			if (TerrainBlendingDefersTerrain()) {
				const auto& terrainEye = terrainBlending.eyePosition;
				latch.terrainNear[0] = terrainEye.x;
				latch.terrainNear[1] = terrainEye.y;
				latch.terrainNear[2] = terrainEye.z;
				latch.terrainNear[3] = TerrainBlending::kTerrainDepthDistance;
			}
			Diagnostics::Notify(Diagnostics::CullLatchWritten{ latch, true });
		} else {
			// T7c1: the terrain groups' ranges this shape records their draws over.
			latch.terrainCapacity[0] = shape.terrainCapacity[0];
			latch.terrainCapacity[1] = shape.terrainCapacity[1];
		}
		// Both segments: the states a LOD skin's level is read from (LodPartitions), the same for the depth and colour draws.
		latch.fadeStatesIndex = a_resources->scene->FadeStatesReadIndex(frameNumber);
		// The colour pass: this frame's cascades, for the synthetic passes' sun test, in the slot's region after the latch (the
		// block holds them: ReserveMainLatch, before the shape). The sun's Accumulate has run.
		const std::uint32_t latchSlot = RenderGraphRuntime::Get().Host()->CurrentFrameSlot();
		// The depth segment's plain draws (MainOpaquePass): the index pool brought up to the tables (unless the shadow commit did,
		// earlier this frame); the buckets' map and each phase's table (PlanZBuckets, whose calls are the shape's) in the slot's
		// region, and their count words zeroed. A slot without a published pipeline maps to none: BuildDraws drops its draws.
		if (depthOnly && a_resources->pool) {
			const auto& layout = latchLayout;
			UpdateIndexPool(*a_resources->pool, Scene::FrameView::Get().GetTables(), Scene::FrameView::Get().GetTablesGeneration(), latchBlock, latchSlot, layout.PoolOffset(), uploads);
			// The plan's slots, and none for every other pipeline slot the installed publication's members can draw with: gained since
			// the plan was made, or past the bucket layout while its growth is outstanding (Growths). None of their draws is the frame's.
			const auto planned = static_cast<std::uint32_t>(writePlan.map.size());
			auto slots = std::max({ planned, static_cast<std::uint32_t>(a_resources->zBucketCapacity.size()), InstalledPipelineSlots() });
			// Past the revision's bucket map: slots no member draws with.
			if (slots > layout.buckets)
				slots = layout.buckets;
			const std::uint32_t buckets = writePlan.Buckets();
			WriteBucketMap(latchBlock, "main bucket map", latchSlot, layout.BucketMapOffset(), writePlan.map, slots);
			if (buckets) {
				const std::size_t words = std::size_t(buckets) * 2;
				LatchWrite(latchBlock, "main bucket tables", latchSlot, layout.BucketTableOffset(), std::as_bytes(std::span(writePlan.table.data(), words)));
				LatchWrite(latchBlock, "main bucket tables", latchSlot, layout.PhaseTwoBucketTableOffset(), std::as_bytes(std::span(writePlan.table.data() + words, words)));
				zBucketZeros.assign(buckets, 0u);
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
			Diagnostics::Notify(Diagnostics::CullLatchWritten{ latch, false });
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

}

#endif

namespace DCLF
{
	bool IndirectDraws::DecideTreeLod()
	{
		bool owned = false;
		auto* buffers = impl->scene.get();
		// A frame without claims (Coordinator::WithdrawSet) is the engine's whole: its tree LOD too.
		if (buffers && buffers->treeLodCull && ActiveToggles().lodTrees && Scene::FrameView::Get().TreeLodMirror().Size() && !Scene::FrameView::Get().SetWithdrawn()) {
			// Its programs and pipelines are the pipeline lane's (T6b2c step 9): asked for once, with the DistantTree shader as a frame
			// input, and read from the frame's catalog. Until that catalog has them, the engine draws tree LOD.
			auto& drawPipelines = DrawPipelines::Get();
			if (auto* shader = Engine::Global<RE::BSShader*>(0x33dcd10)) {  // the BSDistantTreeShader
				drawPipelines.SetTreeLodInputs(*shader);
				drawPipelines.RequestTreeLod();
			}
			const auto* catalog = FrameCatalog();
			const auto* texture = Engine::Global<RE::NiSourceTexture*>(Engine::Address::kTreeLodAtlas);  // its tree LOD atlas (UploadTreeLod)
			TreeLodPipelines pipelines;
			const bool built = TreeLodPipelinesOf(catalog, pipelines);
			if (built && texture && texture->rendererTexture && texture->rendererTexture->resourceView) {
				const auto published = buffers->treeLodPipelines.load(std::memory_order_acquire);
				if (!published || !SameHandle(published->depth, pipelines.depth) || !SameHandle(published->colour, pipelines.colour))
					buffers->treeLodPipelines.store(std::make_shared<const TreeLodPipelines>(pipelines), std::memory_order_release);
				owned = true;
			}
			drawPipelines.NoteFrameWaits(!built && !(catalog && catalog->treeLod.failed), false);
		}
		impl->treeLodOwned = owned;
		PassCapture::Get().SetTreeLodOwned(owned);
		return owned;
	}

	void IndirectDraws::WriteFadeStores(bool a_live, const std::function<bool(const void*)>& a_written)
	{
		if (!impl->scene || !impl->scene->fadeWriteBack)
			return;
		auto& writeBack = *impl->scene->fadeWriteBack;
		// The stores the last finished task found (step 6e F1). T7b4: made by the main thread at Main::Update's entry, the write point
		// T7b0 found (no list cull, cell animation or animation job runs; every reader of the node - the animation gate, the tree LOD,
		// the culls - sees them in this frame), and only on the roots DCLF still writes (a_written: the engine's cull steps any other
		// itself, so one writer). Their snapshot is let go after, here.
		std::unique_ptr<FadeWriteBack::Stores> stores{ writeBack.ready.exchange(nullptr, std::memory_order_acq_rel) };
		if (!stores)
			return;
		// DCLF does not run: the engine culls every root this frame, and their nodes are its.
		if (!a_live) {
			writeBack.dropped.fetch_add(stores->stores.size(), std::memory_order_relaxed);
			return;
		}
		ZoneScopedN("CS.DCLF.FadeWriteBack.Stores");
		const auto start = std::chrono::steady_clock::now();
		std::uint64_t applied = 0, notWritten = 0;
		for (const auto& store : stores->stores) {
			if (!a_written(store.node)) {
				++notWritten;
				continue;
			}
			// The fade, and the fade bits of the flags (atomically: the engine owns the others). The fade watch is not told: a
			// stood-in root's dependents read its fade from FadeStateCS's state (Coordinator::MarkFadeRootOwned).
			auto* node = static_cast<std::byte*>(store.node);
			std::atomic_ref<std::uint32_t> flags(*reinterpret_cast<std::uint32_t*>(node + 0xF4));
			flags.fetch_and(~kFadeFlagMask, std::memory_order_relaxed);
			flags.fetch_or(store.flags, std::memory_order_relaxed);
			*reinterpret_cast<float*>(node + 0x130) = store.fade;
			// T7b4: a leaf or tree root's LOD state (the level, the transition and its blend: FUN_14147a430's, the only writer besides
			// OnVisible, neither of which runs now).
			std::uint32_t fields = SceneCapture::NodeRecord::kFadeCurrent;
			if (store.lod) {
				*reinterpret_cast<float*>(node + 0x14C) = store.blend;
				*reinterpret_cast<std::uint8_t*>(node + 0x152) = static_cast<std::uint8_t>(store.levels);
				*reinterpret_cast<std::uint8_t*>(node + 0x153) = static_cast<std::uint8_t>(store.levels >> 8);
				fields |= SceneCapture::NodeRecord::kFadeLevel;
			}
			// The mirror's value (T6b1a): an update, not a fade event.
			SceneTracker::Get().PushUpdate(SceneCapture::Update{ fields, SceneCapture::CaptureNodeFields(*static_cast<const RE::NiAVObject*>(store.node), fields) });
			++applied;
		}
		writeBack.applied.fetch_add(applied, std::memory_order_relaxed);
		writeBack.notWritten.fetch_add(notWritten, std::memory_order_relaxed);
		writeBack.storeNs.fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count()),
			std::memory_order_relaxed);
	}

	void IndirectDraws::Impl::ArmFadeReadback(SceneBuffers& a_scene, CommitUploads& a_uploads)
	{
		if (!a_scene.fadeWriteBack || !a_scene.fadeEvents)
			return;
		auto& writeBack = *a_scene.fadeWriteBack;
		auto* host = RenderGraphRuntime::Get().Host();
		if (!host)
			return;
		const std::uint32_t frameSlot = host->CurrentFrameSlot();
		if (frameSlot >= writeBack.armed.size())
			return;
		// The slot's last execution (the host has waited for it, and not submitted the next): its events, if its buffer holds the frame
		// that armed it, in the staging set the execution's revision copied into (a growth's revisions copy into the next set; a buffer
		// of another set holds an older frame).
		// The slot's last execution: its events, when its buffer holds the frame that armed it. The epoch did not run for that frame (or
		// ran a revision without the readback): neither did FadeStateCS's report, so the changes it would have reported are reported by
		// the next execution. The roots past the list's end report again next frame; the list grows to them (ReserveSceneTables).
		if (auto& armed = writeBack.armed[frameSlot]; armed.armed) {
			armed.armed = false;
			writeBack.Harvest(frameSlot, armed.frame, [&](const std::uint32_t* a_words, std::uint32_t a_count, std::uint32_t a_capacity) {
				if (const std::uint32_t events = std::min(a_count, a_capacity)) {
					auto* batch = new FadeWriteBack::Batch();
					batch->frame = armed.frame;
					const auto* list = reinterpret_cast<const FadeEvent*>(a_words + kFadeEventHeaderWords);
					batch->events.assign(list, list + events);
					writeBack.Push(batch);
					writeBack.harvestedWithEvents.fetch_add(1, std::memory_order_relaxed);
					writeBack.events.fetch_add(events, std::memory_order_relaxed);
				}
				writeBack.harvested.fetch_add(1, std::memory_order_relaxed);
			});
		}
		// The header for this execution's FadeStateCS to append after: the count, and the frame its harvest matches.
		const std::uint32_t header[kFadeEventHeaderWords]{ 0u, Scene::FrameView::Get().GetFrame(), 0u, 0u };
		a_uploads(a_scene.fadeEvents, header, sizeof(header), 0);
		auto& armed = writeBack.armed[frameSlot];
		armed.armed = true;
		armed.frame = Scene::FrameView::Get().GetFrame();
	}

	void IndirectDraws::KickFadeWriteBack()
	{
		if (!impl->scene || !impl->scene->fadeWriteBack)
			return;
		auto& writeBack = *impl->scene->fadeWriteBack;
		// One task at a time; one still running takes the new batches with it (or the next task does).
		if (bool idle = false; !writeBack.running.compare_exchange_strong(idle, true, std::memory_order_acq_rel)) {
			writeBack.busy.fetch_add(1, std::memory_order_relaxed);
			return;
		}
		// The task judges the events against the frame's tables (immutable), which the stores then hold: so the publication's
		// retirement node, and the root nodes it names (step 6e E3a).
		std::shared_ptr<const SceneTables> tables = Scene::FrameView::Get().AcceptedTables();
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
					// The root the event was of, still DCLF's to write (kFadeRootWritten: any listed root):
					// one listed again since, or the engine's own again, keeps its node; and an event older than the one taken last is not
					// written over it.
					if (event.root >= tables->fadeRoots.size() || tables->fadeRoots[event.root].generation != event.generation ||
						!(tables->fadeRoots[event.root].bits & kFadeRootWritten) || !tables->fadeRootNode[event.root] || batch->frame < state.appliedFrame[event.root]) {
						state.stale.fetch_add(1, std::memory_order_relaxed);
						continue;
					}
					state.appliedFrame[event.root] = batch->frame;
					const std::uint32_t plan = tables->fadeRoots[event.root].bits & kFadeRootPlanMask;
					stores->stores.push_back({ const_cast<void*>(tables->fadeRootNode[event.root]), event.flags & kFadeFlagMask, event.currentFade,
						plan == kFadeRootLeaf || plan == kFadeRootTree, event.levels, event.blend });
				}
				delete batch;
			}
			// What an earlier task found and no Main::Update entry has made yet goes first (T7b4: the entry makes them, so a task that
			// finishes after the next entry's exchange merges with what that entry found nothing of).
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
		if (!SceneScheduler::Executor().Dispatch(SceneScheduler::Scope(), SceneExecutor::Preparation, org::async::TaskDispatch::Cpu, "fade write-back",
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
