#if defined(CS_HAS_RENDER_GRAPH) && defined(CS_HAS_ORG_MODULE_SERVICES)
#	include "Internal.h"

#	include "Features/DrawcallLimitFix/Engine/ReflectionFaces.h"
#	include "Features/DrawcallLimitFix/Scene/LightingDescriptors.h"

namespace DCLF
{
	/*
	 * The water reflection's cube map faces drawn by DCLF (dclf-lod.md, "Water reflections"): the faces' capture, the forward
	 * programs and pipelines of the LOD they draw, the reflection phase's readiness, and the faces' epoch (ReflectionFrame).
	 */

	namespace
	{
		/** @brief A render target view's format, its face (the array slice it views) and its texture. */
		struct ViewTarget
		{
			DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
			std::uint32_t face = ~0u;
			winrt::com_ptr<ID3D11Texture2D> texture;
			std::uint32_t width = 0, height = 0;
		};

		ViewTarget TargetOf(ID3D11RenderTargetView* a_view)
		{
			ViewTarget out;
			if (!a_view)
				return out;
			D3D11_RENDER_TARGET_VIEW_DESC desc{};
			a_view->GetDesc(&desc);
			out.format = desc.Format;
			if (desc.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2DARRAY && desc.Texture2DArray.ArraySize == 1)
				out.face = desc.Texture2DArray.FirstArraySlice;
			winrt::com_ptr<ID3D11Resource> resource;
			a_view->GetResource(resource.put());
			if (resource && SUCCEEDED(resource->QueryInterface(IID_PPV_ARGS(out.texture.put())))) {
				D3D11_TEXTURE2D_DESC textureDesc{};
				out.texture->GetDesc(&textureDesc);
				out.width = textureDesc.Width;
				out.height = textureDesc.Height;
			}
			return out;
		}
	}

	void IndirectDraws::CaptureReflectionFace()
	{
		// The face's colour target, as the engine's accumulator render left it bound: the cube target's face. Its depth target (6)
		// is unbound by then, and DCLF's faces test against a depth of their own, cleared each face, at the engine's precision
		// (D24S8, the faces' depth target's R24G8).
		ID3D11RenderTargetView* view = nullptr;
		globals::d3d::context->OMGetRenderTargets(1, &view, nullptr);
		auto target = TargetOf(view);
		if (view)
			view->Release();
		auto& reflection = impl->reflection;
		const ForwardTargets targets{ target.format, DXGI_FORMAT_D24_UNORM_S8_UINT };
		if (targets.colour == DXGI_FORMAT_UNKNOWN)
			return;
		if (!(reflection.targets == targets) && reflection.targets.colour != DXGI_FORMAT_UNKNOWN)
			logger::info("[DCLF] reflection faces' targets changed: colour {} -> {}, depth {} -> {}", static_cast<int>(reflection.targets.colour),
				static_cast<int>(targets.colour), static_cast<int>(reflection.targets.depth), static_cast<int>(targets.depth));
		reflection.targets = targets;
		++reflection.facesCaptured;
		// The face for this update's epoch (ExecuteReflection): only a plain face render's, whose LOD the registrations withhold.
		if (!ReflectionFaces::Plain() || target.face >= kReflectionFaces || !target.texture)
			return;
		if (reflection.executedFrame == SceneStore::Get().GetFrame())
			++reflection.lateFaces;
		auto& face = reflection.faces[target.face];
		face = {};
		// VS_PerFrame (b12) as the engine wrote it for the face's camera (Draws::CapturePerFrame's rule): from the mirror, or from
		// Community Shaders' copy of the same buffer until the mirror has seen a write. Its view-projection is camera-relative, and
		// its CameraPosAdjust (c40) the eye.
		auto& mirror = ConstantMirror::Get();
		if (auto* perFrame = *globals::game::perFrame.get()) {
			mirror.Watch(perFrame);
			const auto contents = mirror.Contents(perFrame);
			if (contents.size() >= 164 * sizeof(float)) {
				face.perFrameBytes = static_cast<std::uint32_t>(std::min<std::size_t>(contents.size(), face.perFrame.size()));
				std::memcpy(face.perFrame.data(), contents.data(), face.perFrameBytes);
			}
		}
		if (!face.perFrameBytes) {
			const auto& cached = globals::game::frameBufferCached.data;
			static_assert(sizeof(cached) >= 164 * sizeof(float) && sizeof(cached) <= kReflectionFaceBlockBytes);
			face.perFrameBytes = sizeof(cached);
			std::memcpy(face.perFrame.data(), &cached, sizeof(cached));
		}
		const auto* floats = reinterpret_cast<const float*>(face.perFrame.data());
		std::memcpy(face.viewProj.data(), floats + 32, sizeof(float) * 16);
		face.eye = { floats[160], floats[161], floats[162] };
		face.captured = true;
		reflection.cube = std::move(target.texture);
		reflection.width = target.width;
		reflection.height = target.height;
	}

	void IndirectDraws::PrepareReflection()
	{
		// The forward programs and pipelines of every LOD pipeline slot the frame's objects use, and tree LOD's, for the faces'
		// targets once a face has been seen: requested here, ready a few frames later. The slots whose pipeline is ready are the
		// reflection phase's readiness (PhaseReady): a change of them is a readiness event for the set.
		auto& reflection = impl->reflection;
		// The faces' targets, known before any face is drawn: the engine's reflection cube target's face views (a face's capture
		// checks them), so the reflection phase is the set's from the start rather than from the first face.
		if (reflection.targets.colour == DXGI_FORMAT_UNKNOWN)
			if (auto* renderer = globals::game::renderer)
				if (auto* face = renderer->GetRendererData().cubemapRenderTargets[RE::RENDER_TARGETS_CUBEMAP::kREFLECTIONS].cubeSideRTV[0])
					reflection.targets = { TargetOf(face).format, DXGI_FORMAT_D24_UNORM_S8_UINT };
		auto* lighting = ConstantEvaluator::Get().GetLightingShader();
		const bool on = reflection.targets.colour != DXGI_FORMAT_UNKNOWN && ActiveToggles().reflections && !failed && lighting;
		const auto& tables = SceneStore::Get().GetTables();
		std::uint32_t slots = 0, programs = 0, pipelines = 0;
		std::uint64_t key = 0;
		reflection.slotPipelines.assign(on ? tables.pipelines.size() : 0, rhi::PipelineHandle{});
		for (std::size_t p = 0; on && p < tables.pipelines.size(); ++p) {
			if (!tables.PipelineUsed(p))
				continue;
			const auto& pipelineKey = tables.pipelines[p];
			if (!LodLightingTechnique(pipelineKey.passDescriptor))
				continue;
			++slots;
			const auto* program = ShaderPrograms::Get().FindForward(pipelineKey.vertexDescriptor, pipelineKey.pixelDescriptor & ~kLightingPixelDeferred, *lighting);
			if (!program)
				continue;
			++programs;
			// A cube face's projection mirrors the image: the engine culls front faces there (dclf-lod.md, "The state").
			const auto cull = (pipelineKey.rasterFlags & kRasterTwoSided) ? rhi::CullMode::None : rhi::CullMode::Front;
			const auto pipeline = FindForwardPipeline(*program, reflection.targets, cull);
			if (!pipeline.valid())
				continue;
			++pipelines;
			reflection.slotPipelines[p] = pipeline;
			key = key * 0x100000001b3ull ^ (p + 1);
		}
		if (key != reflection.readinessKey) {
			reflection.readinessKey = key;
			++impl->shadowReadinessSerial;
		}
		reflection.treePipeline = {};
		if (auto* distantTree = Engine::Global<RE::BSShader*>(0x33dcd10); on && distantTree && ActiveToggles().lodTrees)
			if (const auto* program = ShaderPrograms::Get().FindForwardTreeLod(*distantTree))
				reflection.treePipeline = FindForwardPipeline(*program, reflection.targets, rhi::CullMode::Front);
		reflection.lodSlots = slots;
		reflection.programsReady = programs;
		reflection.pipelinesReady = pipelines;
		reflection.treeReady = reflection.treePipeline.valid();
		if (on)
			impl->SetupReflection();
		// The faces' tree LOD: DCLF's while the main view's is (DecideTreeLod, before this), the last depth commit uploaded the tables
		// the faces draw from, and the faces are drawn.
		const auto* scene = impl->scene.get();
		reflection.treeOwned = ReflectionDrawable() && impl->reflectionCovered && reflection.treeReady && impl->treeLodOwned && scene && scene->treeLodRow.shapeSlots &&
		                       scene->treeLodPipelines.load(std::memory_order_acquire);
		PassCapture::Get().SetReflectionTreeLodOwned(reflection.treeOwned);
	}

	bool IndirectDraws::ReflectionDrawable() const
	{
		// A capability, not whether the last update drew nor what the faces' resources or pipelines are yet: the faces' targets, known
		// at the first frame (PrepareReflection). Each member then waits for its own pipeline (ReflectionPhaseReady), and the faces
		// withhold nothing until their epoch can draw them (DecideCoverage: the resources and the revision's shape).
		const auto& reflection = impl->reflection;
		return ActiveToggles().reflections && !failed && reflection.targets.colour != DXGI_FORMAT_UNKNOWN;
	}

	bool IndirectDraws::Impl::ReflectionPhaseReady(const SceneStore::Tables& a_tables, std::uint32_t a_slot) const
	{
		const std::uint32_t p = a_slot < a_tables.objects.size() ? a_tables.objects[a_slot].pipelineIndex : ~0u;
		return p < reflection.slotPipelines.size() && reflection.slotPipelines[p].valid();
	}

	void IndirectDraws::ExecuteReflection()
	{
		ZoneScopedN("CS.DCLF.ExecuteReflection");
		auto& reflection = impl->reflection;
		++reflection.updates;
		reflection.executedFrame = SceneStore::Get().GetFrame();
		// The frame's faces, taken whatever happens: the next frame captures its own.
		auto faces = reflection.faces;
		for (auto& face : reflection.faces)
			face.captured = false;
		auto skip = [&](std::uint32_t a_cause) { ++reflection.skipped[a_cause]; };
		const auto set = PassCapture::Get().CurrentSet();
		if (!set || !(set->drawn & kSetReflection))
			return skip(0);
		if (std::none_of(faces.begin(), faces.end(), [](const auto& a_face) { return a_face.captured; }))
			return skip(2);
		// What the faces draw from: the frame before's depth inputs and colour frame record, in the backings they were written to.
		auto& store = SceneStore::Get();
		const std::uint32_t frameNumber = store.GetFrame();
		const auto main = impl->resources;
		const auto& depthCommit = main ? main->committed[kDepthShape] : Resources::Committed{};
		const auto& colourCommit = main ? main->committed[kColourShape] : Resources::Committed{};
		if (!main || !main->inputsDepth || !impl->scene || depthCommit.frame != colourCommit.frame || frameNumber - depthCommit.frame > 1 ||
			depthCommit.sceneGeneration != impl->scene->generation || depthCommit.objectCapacity != main->objectCapacity ||
			depthCommit.rowsGeneration != main->MainRowsGeneration() || colourCommit.rowsGeneration != main->MainRowsGeneration())
			return skip(3);
		const auto indirect = GetIndirectState();
		if (!indirect.valid || !impl->SetupReflection() || !impl->ImportReflectionCube(reflection.cube.get()))
			return skip(4);
		// Not DCLF's this frame (no claims, or the cube was imported since the graph was built): the engine rendered the faces whole.
		if (!impl->reflectionCovered)
			return skip(1);

		// The selected revision's shape and recording (DecideCoverage covered the faces): trusted, its map, buckets and latch are what
		// the values go into (CS_DCLF_REVISION_PARITY checks them against the frame's own).
		const auto revision = impl->RevisionOf(4);
		if (!revision)
			return skip(1);
		const auto revisionShape = revision.shape->Value<ReflectionFrame>();
		const ReflectionFrame& target = *revisionShape;
		auto resources = reflection.resources;
		auto& scene = *impl->scene;
		const auto treeLod = scene.treeLodPipelines.load(std::memory_order_acquire);
		// Tree LOD's draws, when the revision's faces have them and the scene's tables are the ones they were sized for.
		const bool trees = target.tree.valid() && reflection.treePipeline.valid() && treeLod && scene.treeLodCull &&
		                   resources->treeShapeCapacity == scene.treeLodShapeCapacity;
		// Every pipeline slot the installed publication's members can draw with: the map's, and none for the rest (gained since the
		// revision's join), as many as its latch holds.
		auto slots = std::max(static_cast<std::uint32_t>(target.map->size()), impl->InstalledPipelineSlots());
		if (slots > target.latchLayout.slots) {
			if (RevisionParityEnabled() && impl->MemberPastSlots(target.latchLayout.slots))
				++impl->revisions.latchClamped[4];
			slots = target.latchLayout.slots;
		}
		const auto cleanup = RenderGraphRuntime::Get().Host()->ResourceCleanup();
		if (!cleanup)
			return skip(4);
		auto owners = cleanup->Make<std::vector<std::shared_ptr<const void>>>();
		// The colour segment's frame record names the frame textures it resolved, held by their bindings since (frameTextureBindings).
		for (const auto& held : impl->frameTextureBindings)
			if (held.binding.owner)
				owners->push_back(held.binding.owner);
		std::uint32_t drawn = 0;
		const bool ok = RenderGraphRuntime::Get().ExecuteEpoch(RenderGraphRuntime::Segment::Reflection, [&](org::RenderGraph&) {
			// CS_DCLF_REVISION_PARITY: the commit's own shape and the revision's against it (inside the epoch: the graph's heaps). It
			// observes only.
			if (RevisionParityEnabled() && ParityDue(frameNumber))
				impl->CheckReflectionRevision(target, indirect, slots, trees ? treeLod.get() : nullptr, frameNumber);
			const std::uint32_t latchSlot = RenderGraphRuntime::Get().Host()->CurrentFrameSlot();
			const auto& latchBlock = *target.latch;
			const auto& layout = target.latchLayout;
			const auto& writeMap = *target.map;
			const auto& writeBuckets = target.buckets;
			const auto region = static_cast<std::uint32_t>(latchBlock.Offset(latchSlot));
			if (!writeMap.empty())
				LatchWrite(latchBlock, "reflection bucket map", latchSlot, ReflectionLatchLayout::MapOffset(), std::as_bytes(std::span(writeMap)));
			// A pipeline slot the tables have gained since the map was made has no bucket (none of its draws is the frame's).
			if (slots > writeMap.size()) {
				const std::vector<std::uint32_t> none(slots - writeMap.size(), kNoBucket);
				LatchWrite(latchBlock, "reflection bucket map", latchSlot, ReflectionLatchLayout::MapOffset() + static_cast<std::uint32_t>(writeMap.size() * sizeof(std::uint32_t)), std::as_bytes(std::span(none)));
			}
			// Every value the faces' buffers take goes into the latch; the epoch's latched copies (ReflectionLatchedCopiesPass) take
			// them there and zero the counters, so this commit records no copy.
			std::vector<std::uint32_t> table;
			// Tree LOD's row as the last depth commit uploaded it, its texture bound again for this execution.
			TreeLod::DrawRow treeRow = scene.treeLodRow;
			const bool treeRowBound = trees && reflection.treeOwned && treeRow.shapeSlots && TreeLodTextureBinding(treeRow, *owners);
			for (std::uint32_t f = 0; f < kReflectionFaces; ++f) {
				const auto& face = faces[f];
				const std::uint32_t inputs = face.captured ? depthCommit.inputs : 0u;
				BuildDrawsLatch latch{};
				latch.dispatch[0] = (inputs + 63) / 64;
				latch.dispatch[1] = 1;
				latch.dispatch[2] = 1;
				latch.drawCount = inputs;
				latch.cullFlags = 1;  // the frustum alone, near plane included
				latch.viewBits = kViewReflection;  // the depth segment's inputs: the reflection phase's members
				latch.visibilityStamp = frameNumber & 0x0FFFFFFFu;
				latch.placementsIndex = FrameValues::Get().PlacementsIndex();
				// The depth inputs the last Z-prepass commit read: its ring entry's, when it read one (step 6e E4).
				Impl::RingLatch(impl->ringDepth, kAsyncZPrepass, latch);
				FoldEyeIntoViewProj(face.viewProj, face.eye, latch.viewProj);
				latch.bucketMapOffset = region + ReflectionLatchLayout::MapOffset();
				latch.bucketTableOffset = region + layout.TableOffset(f);
				table.clear();
				for (const auto& bucket : writeBuckets) {
					table.push_back(f * target.sequenceDraws + bucket.first);
					table.push_back(bucket.capacity);
				}
				if (!table.empty())
					LatchWrite(latchBlock, "reflection bucket tables", latchSlot, layout.TableOffset(f), std::as_bytes(std::span(table)));
				LatchWriteValue(latchBlock, "reflection culling latches", latchSlot, f * static_cast<std::uint32_t>(sizeof(BuildDrawsLatch)), latch);
				// A face not captured draws nothing (no inputs): its block is copied as the latch holds it.
				if (face.captured)
					LatchWrite(latchBlock, "reflection face per-frame data", latchSlot, layout.FaceOffset(f), std::span(face.perFrame.data(), face.perFrameBytes));
				if (target.tree.valid()) {
					// The face's row (its own list), naming no slot when the face is not drawn, the faces' tree LOD is the engine's, or the
					// scene's tables are not the ones the revision's tree passes were sized for.
					TreeLod::DrawRow row = treeRow;
					row.visible = resources->treeVisibleAddress[f] + TreeLod::kVisibleHeaderWords * sizeof(std::uint32_t);
					row.shapeSlots = face.captured && treeRowBound ? treeRow.shapeSlots : 0u;
					LatchWriteValue(latchBlock, "reflection tree rows", latchSlot, layout.FaceOffset(f) + ReflectionLatchLayout::kTreeRowInFace, row);
					TreeLod::VisibleHeader header{};
					header.phaseOne[0] = header.phaseTwo[0] = header.colour[0] = std::max(row.maxIndices, 1u);
					LatchWriteValue(latchBlock, "reflection tree rows", latchSlot, layout.FaceOffset(f) + ReflectionLatchLayout::kTreeHeaderInFace, header);
				}
				drawn += face.captured ? 1u : 0u;
			}

			impl->SubmitRevisionRecording(4, *revision.recordings, 0);
		}, owners);
		if (!ok) {
			logger::error("[DCLF] the reflection faces' epoch failed");
			return skip(5);
		}
		++reflection.epochs;
		reflection.facesDrawn += drawn;
	}

	void IndirectDraws::Impl::CheckReflectionRevision(const ReflectionFrame& a_shape, const IndirectState& a_indirect, std::uint32_t a_slots,
		const TreeLodPipelines* a_treeLod, std::uint32_t a_frame)
	{
		using R = SceneRevisions;
		const auto& faces = *reflection.resources;
		ReflectionPlan plan;
		PlanReflectionBuckets(*faces.main, reflection.slotPipelines, plan);
		ReflectionShapeInputs in;
		in.resourceHeap = org::runtime::GetActiveSRVDescriptorHeap().GetHandle();
		in.samplerHeap = org::runtime::GetActiveSamplerDescriptorHeap().GetHandle();
		in.indirect = a_indirect;
		in.buckets = plan.buckets;
		in.map = std::make_shared<const std::vector<std::uint32_t>>(plan.map);
		in.materialRows = faces.main->materialRows.address;
		in.pipelineRows = faces.main->pipelineRows.address;
		if (a_treeLod) {
			in.tree = reflection.treePipeline;
			in.treeSignature = a_treeLod->drawSignature;
			in.treeShapes = scene->treeLodShapeCapacity;
		}
		const auto own = MakeReflectionShape(faces, in);
		NoteReflectionParity(*own, a_frame);
		std::shared_ptr<const org::async::RevisionFragment> fragment;
		std::shared_ptr<const RevisionRecordings> recordings;
		++revisions.parityChecks[4];
		if (!ActiveRevision(4, fragment, recordings))
			return;
		std::uint32_t miss = R::kMisses;
		if (!a_shape.latch || !a_shape.map)
			miss = R::kNoRecording;
		else if (!SameHandle(a_shape.resourceHeap, own->resourceHeap) || !SameHandle(a_shape.samplerHeap, own->samplerHeap) ||
				 !SameHandle(a_shape.indirect.zLayout, a_indirect.zLayout) || !SameHandle(a_shape.indirect.zDrawSignature, a_indirect.zDrawSignature) ||
				 (a_shape.indirect.version != a_indirect.version && !RevisionHoldsClaims()))
			miss = R::kPipelines;
		else if (a_shape.width != own->width || a_shape.height != own->height)
			miss = R::kViewport;
		else if (!SameHandle(a_shape.tree, own->tree) || a_shape.treeGroups != own->treeGroups || a_slots > a_shape.latchLayout.slots)
			miss = R::kShape;
		if (miss == R::kMisses)
			return;
		NoteRevisionMiss(4, miss);
		static std::uint32_t logged = 0;
		if (logged++ < 16)
			logger::warn("[DCLF] revision parity: the reflection commit of frame {} does not fit the selected revision's shape: {} <- REVISION", a_frame, R::kMissNames[miss]);
	}

	std::string IndirectDraws::ReflectionReport()
	{
		auto& reflection = impl->reflection;
		const auto& forward = DrawPipelines::Get().GetForwardStats();
		const auto& capture = PassCapture::Get().GetStats();
		const auto& s = reflection.skipped;
		std::string text = fmt::format("[DCLF] reflection faces: {} captured (targets colour {}, depth {}); {} LOD pipeline slots, {} forward programs and {} forward pipelines ready, "
									   "tree LOD's {}; forward pipelines {} requested, {} built, {} failed{}\n",
			reflection.facesCaptured, static_cast<int>(reflection.targets.colour), static_cast<int>(reflection.targets.depth), reflection.lodSlots, reflection.programsReady,
			reflection.pipelinesReady, reflection.treeReady ? "ready" : "not ready", forward.requested, forward.ready, forward.failed, forward.failed ? " <- FAILED" : "");
		text += fmt::format("[DCLF] reflection faces drawn: {} frames, {} epochs, {} faces drawn, {} captured after their frame's epoch{}; not drawn: {} not the set's, {} without faces, "
							"{} the engine's (not covered), {} stale inputs, {} without resources, {} failed{}; last frame's registrations: {} member passes and {} tree LOD passes withheld (faces' tree LOD {})\n",
			reflection.updates, reflection.epochs, reflection.facesDrawn, reflection.lateFaces, reflection.lateFaces ? " <- LATE FACES" : "", s[0], s[2], s[1], s[3], s[4], s[5],
			s[5] ? " <- EPOCH FAILED" : "", capture.reflectionWithheld, capture.reflectionTreeLodWithheld, reflection.treeOwned ? "DCLF's" : "the engine's");
		reflection.facesCaptured = reflection.updates = reflection.epochs = reflection.facesDrawn = reflection.lateFaces = 0;
		reflection.skipped = {};
		return text;
	}
}
#endif
