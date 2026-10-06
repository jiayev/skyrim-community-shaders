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
		reflection.treeOwned = ReflectionDrawable() && reflection.treeReady && impl->treeLodOwned && scene && scene->treeLodRow.shapeSlots &&
		                       scene->treeLodPipelines.load(std::memory_order_acquire);
		PassCapture::Get().SetReflectionTreeLodOwned(reflection.treeOwned);
	}

	bool IndirectDraws::ReflectionDrawable() const
	{
		// Readiness, not whether the last update drew: it flips only when the faces' resources or their first pipeline appear.
		const auto& reflection = impl->reflection;
		return ActiveToggles().reflections && !failed && reflection.resources && impl->resources && reflection.pipelinesReady;
	}

	bool IndirectDraws::Impl::ReflectionPhaseReady(std::uint32_t a_slot) const
	{
		const auto& tables = SceneStore::Get().GetTables();
		const std::uint32_t p = a_slot < tables.objects.size() ? tables.objects[a_slot].pipelineIndex : ~0u;
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
			depthCommit.sceneGeneration != impl->scene->generation || depthCommit.objectCapacity != main->objectCapacity)
			return skip(3);
		const auto indirect = GetIndirectState();
		if (!indirect.valid || !impl->SetupReflection() || !impl->ImportReflectionCube(reflection.cube.get()))
			return skip(4);

		// The buckets (PlanReflectionBuckets), and the resources reserved for them.
		ReflectionPlan plan;
		PlanReflectionBuckets(*main, reflection.slotPipelines, plan);
		const auto slots = static_cast<std::uint32_t>(plan.map.size());
		const auto& map = plan.map;
		const auto& buckets = plan.buckets;
		impl->ReserveReflection(slots, static_cast<std::uint32_t>(buckets.size()), plan.draws);
		auto resources = reflection.resources;
		auto& scene = *impl->scene;
		const auto treeLod = scene.treeLodPipelines.load(std::memory_order_acquire);
		const bool trees = reflection.treePipeline.valid() && treeLod && scene.treeLodCull && resources->treeShapeCapacity == scene.treeLodShapeCapacity;

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
			resources->frame.store(nullptr, std::memory_order_release);
			const std::uint32_t latchSlot = RenderGraphRuntime::Get().Host()->CurrentFrameSlot();
			const auto& layout = resources->latchLayout;
			const auto region = static_cast<std::uint32_t>(resources->latch->Offset(latchSlot));
			if (slots)
				resources->latch->Write(latchSlot, ReflectionLatchLayout::MapOffset(), std::as_bytes(std::span(map)));
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
				latch.visibilityStamp = frameNumber & 0x0FFFFFFFu;
				FoldEyeIntoViewProj(face.viewProj, face.eye, latch.viewProj);
				latch.bucketMapOffset = region + ReflectionLatchLayout::MapOffset();
				latch.bucketTableOffset = region + layout.TableOffset(f);
				table.clear();
				for (const auto& bucket : buckets) {
					table.push_back(f * resources->sequenceDraws + bucket.first);
					table.push_back(bucket.capacity);
				}
				if (!table.empty())
					resources->latch->Write(latchSlot, layout.TableOffset(f), std::as_bytes(std::span(table)));
				resources->latch->WriteValue(latchSlot, f * static_cast<std::uint32_t>(sizeof(BuildDrawsLatch)), latch);
				// A face not captured draws nothing (no inputs): its block is copied as the latch holds it.
				if (face.captured)
					resources->latch->Write(latchSlot, layout.FaceOffset(f), std::span(face.perFrame.data(), face.perFrameBytes));
				if (trees) {
					// The face's row (its own list), naming no slot when the face is not drawn or the faces' tree LOD is the engine's.
					TreeLod::DrawRow row = treeRow;
					row.visible = resources->treeVisibleAddress[f] + TreeLod::kVisibleHeaderWords * sizeof(std::uint32_t);
					row.shapeSlots = face.captured && treeRowBound ? treeRow.shapeSlots : 0u;
					resources->latch->WriteValue(latchSlot, layout.FaceOffset(f) + ReflectionLatchLayout::kTreeRowInFace, row);
					TreeLod::VisibleHeader header{};
					header.phaseOne[0] = header.phaseTwo[0] = header.colour[0] = std::max(row.maxIndices, 1u);
					resources->latch->WriteValue(latchSlot, layout.FaceOffset(f) + ReflectionLatchLayout::kTreeHeaderInFace, header);
				}
				drawn += face.captured ? 1u : 0u;
			}

			ReflectionShapeInputs shapeIn;
			shapeIn.resourceHeap = org::runtime::GetActiveSRVDescriptorHeap().GetHandle();
			shapeIn.samplerHeap = org::runtime::GetActiveSamplerDescriptorHeap().GetHandle();
			shapeIn.indirect = indirect;
			shapeIn.buckets = buckets;
			if (trees) {
				shapeIn.tree = reflection.treePipeline;
				shapeIn.treeSignature = treeLod->drawSignature;
				shapeIn.treeShapes = scene.treeLodShapeCapacity;
			}
			auto frame = MakeReflectionShape(*resources, shapeIn);
			impl->NoteReflectionParity(*frame, frameNumber);
			PublishShape(std::move(frame), resources->published, resources->frame, resources->shapeGenerations, resources->recentShapes);
		}, owners);
		if (!ok) {
			logger::error("[DCLF] the reflection faces' epoch failed");
			return skip(5);
		}
		++reflection.epochs;
		reflection.facesDrawn += drawn;
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
							"{} stale inputs, {} without resources, {} failed{}; last frame's registrations: {} member passes and {} tree LOD passes withheld (faces' tree LOD {})\n",
			reflection.updates, reflection.epochs, reflection.facesDrawn, reflection.lateFaces, reflection.lateFaces ? " <- LATE FACES" : "", s[0], s[2], s[3], s[4], s[5],
			s[5] ? " <- EPOCH FAILED" : "", capture.reflectionWithheld, capture.reflectionTreeLodWithheld, reflection.treeOwned ? "DCLF's" : "the engine's");
		reflection.facesCaptured = reflection.updates = reflection.epochs = reflection.facesDrawn = reflection.lateFaces = 0;
		reflection.skipped = {};
		return text;
	}
}
#endif
