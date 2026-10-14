// The water reflection's cube map faces drawn by DCLF (dclf-lod.md, "Water reflections"): the faces' capture, the forward
// programs and pipelines of the LOD they draw, the reflection phase's readiness, and the faces' commit (their epoch is
// ViewEpoch.cpp's, its passes ViewEpochRecord.cpp's). Render thread, but for ReflectionDrawable and ReflectionSlotReady (the scene lane's commit).

#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Scene/FrameView/FrameView.h"
#include "Features/DrawcallLimitFix/Diagnostics/Draws/Events.h"

#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"
#include "Features/DrawcallLimitFix/Engine/Cull/LodGates.h"
#include "Features/DrawcallLimitFix/Engine/Views/ReflectionFaces.h"
#include "Features/DrawcallLimitFix/Engine/Views/SunViews.h"
#include "Features/DrawcallLimitFix/Scene/Ports/LightingDescriptors.h"

namespace DCLF
{
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

	void IndirectDraws::ReflectionFaceCamera(const RE::NiCamera& a_camera, std::uint32_t a_face)
	{
		auto& reflection = impl->reflection;
		reflection.orientedFace = a_face;
		// The face for this update's epoch (ExecuteReflection): only a plain face render's, whose LOD the registrations withhold or
		// whose LOD roots were not added (ReflectionRootsOwned).
		if (!ReflectionFaces::Plain() || a_face >= kReflectionFaces)
			return;
		// The face's colour target, the engine's reflection cube target's face a_face; its depth DCLF's own, cleared each face, at the
		// engine's precision (D24S8, the faces' depth target's R24G8).
		auto* renderer = globals::game::renderer;
		auto target = TargetOf(renderer ? renderer->GetRendererData().cubemapRenderTargets[RE::RENDER_TARGETS_CUBEMAP::kREFLECTIONS].cubeSideRTV[a_face] : nullptr);
		const ForwardTargets targets{ target.format, DXGI_FORMAT_D24_UNORM_S8_UINT };
		if (targets.colour == DXGI_FORMAT_UNKNOWN || !target.texture)
			return;
		if (!(reflection.targets == targets) && reflection.targets.colour != DXGI_FORMAT_UNKNOWN)
			logger::info("[DCLF] reflection faces' targets changed: colour {} -> {}, depth {} -> {}", static_cast<int>(reflection.targets.colour),
				static_cast<int>(targets.colour), static_cast<int>(reflection.targets.depth), static_cast<int>(targets.depth));
		reflection.targets = targets;
		reflection.PublishTargets();
		++reflection.facesCaptured;
		if (reflection.executedFrame == Scene::FrameView::Get().GetFrame())
			++reflection.lateFaces;
		auto& face = reflection.faces[a_face];
		face = {};
		// VS_PerFrame for the face's camera over the frame's registers as the renderer last uploaded them (as the engine uploads it for
		// a face: no jitter, no previous camera).
		SunViews::Cascade view;
		SunViews::CameraMatrices(a_camera, view);
		PerFrameBlock(view, FrameBlockNow(), false, nullptr, face.perFrame.data());
		face.perFrameBytes = SunViews::kBlockBytes;
		const auto* floats = reinterpret_cast<const float*>(face.perFrame.data());
		std::memcpy(face.viewProj.data(), floats + 32, sizeof(float) * 16);
		face.eye = { floats[160], floats[161], floats[162] };
		face.captured = true;
		reflection.cube = std::move(target.texture);
		reflection.width = target.width;
		reflection.height = target.height;
	}

	void IndirectDraws::CaptureReflectionFace()
	{
		// The face the engine has just drawn, for the observers (its camera against DCLF's: ReflectionFaceCamera).
		Diagnostics::Notify(Diagnostics::ReflectionFaceDrawn{});
	}

	std::uint32_t IndirectDraws::ReflectionRootsOwned(bool a_plain)
	{
		// The roots whose members DCLF's faces draw, in a plain update of a frame whose faces are DCLF's (DecideCoverage):
		// - LOD trees while the faces' tree LOD is DCLF's (PrepareReflection): drawn from DCLF's mirror, no registration needed.
		// - LOD land and objects while the engine's LOD swaps are gated (LodGates) and both are DCLF's (the toggles): a
		//   swap's incoming blocks are shown only by the frame whose claims hold them, so the faces never need the engine's
		//   registrations of them. Ungated, the LOD chunks and objects the engine shows or attaches in motion take DCLF a few frames
		//   to bind (ClassifyResidue) and their roots stay, the members withheld (PassCapture).
		auto& reflection = impl->reflection;
		auto& capture = PassCapture::Get();
		capture.WatchReflectionResidue(0);
		if (!a_plain || !ReflectionDrawable() || !reflection.covered || !Policy::WithholdFromEngine())
			return 0;
		const auto toggles = ActiveToggles();
		const bool lodOwned = LodGates::Enabled() && toggles.lodObjects && toggles.lodTerrain;
		const std::uint32_t owned = (reflection.treeOwned ? ReflectionFaces::kTreeRoot : 0u) | (lodOwned ? ReflectionFaces::kLodRoots : 0u);
		// The updates that keep the roots (Policy::KeepReflectionRoots): what the engine registers into the faces and DCLF does not
		// withhold is watched, by root - the LOD roots' (what skipping them loses: any is a defect once they are owned) and the tree root's.
		if (Policy::KeepReflectionRoots(reflection.residueUpdates)) {
			capture.WatchReflectionResidue(ReflectionFaces::kLodRoots | owned);
			return 0;
		}
		return owned;
	}

	void IndirectDraws::PrepareReflection()
	{
		// The forward pipelines of every LOD pipeline slot the frame's objects use, and tree LOD's, for the faces' targets: asked of the
		// pipeline lane here (each key once, the targets a frame input) and read from the frame's catalog, ready a few
		// frames later. The slots whose pipeline is ready are the reflection phase's readiness (PhaseReady): a change of them is a
		// readiness event for the set. A slot without one is no reflection-phase member meanwhile (the engine draws it in the faces).
		auto& reflection = impl->reflection;
		// The faces' targets, known before any face is drawn: the engine's reflection cube target's face views (a face's capture
		// checks them), so the reflection phase is the set's from the start rather than from the first face.
		if (reflection.targets.colour == DXGI_FORMAT_UNKNOWN)
			if (auto* renderer = globals::game::renderer)
				if (auto* face = renderer->GetRendererData().cubemapRenderTargets[RE::RENDER_TARGETS_CUBEMAP::kREFLECTIONS].cubeSideRTV[0]) {
					reflection.targets = { TargetOf(face).format, DXGI_FORMAT_D24_UNORM_S8_UINT };
					reflection.PublishTargets();
					// A revision input: the targets the faces' pipelines are resolved for (from a revision's request's catalog).
					impl->PostRevisionInputs();
				}
		const bool on = reflection.targets.colour != DXGI_FORMAT_UNKNOWN && ActiveToggles().reflections && !failed;
		auto& drawPipelines = DrawPipelines::Get();
		if (on)
			drawPipelines.SetForwardTargets(reflection.targets);
		const auto* catalog = FrameCatalog();
		const auto& tables = Scene::FrameView::Get().GetTables();
		std::uint32_t slots = 0, programs = 0, pipelines = 0;
		bool waited = false;
		reflection.slotPipelines.assign(on ? tables.pipelines.size() : 0, rhi::PipelineHandle{});
		for (std::size_t p = 0; on && p < tables.pipelines.size(); ++p) {
			ForwardPipelineKey forwardKey;
			if (!ViewForwardKey(ViewKind::Reflection, tables, p, forwardKey))
				continue;
			++slots;
			drawPipelines.RequestForward(forwardKey);
			const auto* entry = catalog ? catalog->FindForward(forwardKey) : nullptr;
			programs += entry && entry->program ? 1u : 0u;
			const auto pipeline = ForwardPipelineOf(catalog, forwardKey, reflection.targets);
			if (!pipeline.valid()) {
				waited |= !(entry && entry->failed);
				continue;
			}
			++pipelines;
			reflection.slotPipelines[p] = pipeline;
		}
		reflection.treePipeline = {};
		if (on && ActiveToggles().lodTrees) {
			// Its program is built of the DistantTree shader DecideTreeLod posts.
			const ForwardPipelineKey treeKey{ 0, 0, ForwardPipelineKey::kForwardTreeLod };
			drawPipelines.RequestForward(treeKey);
			reflection.treePipeline = ForwardPipelineOf(catalog, treeKey, reflection.targets);
			const auto* entry = catalog ? catalog->FindForward(treeKey) : nullptr;
			waited |= !reflection.treePipeline.valid() && impl->treeLodOwned && !(entry && entry->failed);
		}
		drawPipelines.NoteFrameWaits(false, waited);
		reflection.lodSlots = slots;
		reflection.programsReady = programs;
		reflection.pipelinesReady = pipelines;
		reflection.treeReady = reflection.treePipeline.valid();
		if (on)
			impl->SetupView(ViewKind::Reflection);
		// The faces' tree LOD: DCLF's while the main view's is (DecideTreeLod, before this), the last depth commit uploaded the tables
		// the faces draw from, and the faces are drawn.
		const auto* scene = impl->scene.get();
		reflection.treeOwned = ReflectionDrawable() && reflection.covered && reflection.treeReady && impl->treeLodOwned && scene && scene->treeLodRow.shapeSlots &&
		                       scene->treeLodPipelines.load(std::memory_order_acquire);
		PassCapture::Get().SetReflectionTreeLodOwned(reflection.treeOwned);
	}

	bool IndirectDraws::ReflectionDrawable() const
	{
		// A capability, not whether the last update drew nor what the faces' resources or pipelines are yet: the faces' targets, known
		// at the first frame (PrepareReflection). Each member then waits for its own pipeline (ReflectionPhaseReady), and the faces
		// withhold nothing until their epoch can draw them (DecideCoverage: the resources and the revision's shape).
		// The published targets (the scene lane's commit calls this: Coordinator::SetCapability), never the render thread's own copy.
		return ActiveToggles().reflections && !failed && impl->reflection.LaneTargets().colour != DXGI_FORMAT_UNKNOWN;
	}

	bool IndirectDraws::ReflectionSlotReady(const void* a_tables, const PipelineCatalog* a_catalog, std::uint32_t a_slot) const
	{
		// The faces' forward pipeline of the slot's pipeline as a_catalog has it for the faces' targets (ReflectionPipelinesOf's rule,
		// which the revision's shapes resolve from their publication's catalog): the scene lane's commit's, from its own catalog, never the
		// render thread's per-frame list (Reflection::slotPipelines, which PrepareReflection assigns whole).
		const auto& tables = *static_cast<const SceneTables*>(a_tables);
		const ForwardTargets targets = impl->reflection.LaneTargets();  // the published copy (the render thread writes `targets` at every face capture)
		if (!a_catalog || targets.colour == DXGI_FORMAT_UNKNOWN || a_slot >= tables.objects.size())
			return false;
		const std::uint32_t p = tables.objects[a_slot].pipelineIndex;
		ForwardPipelineKey key;
		return p < tables.pipelines.size() && ViewForwardKey(ViewKind::Reflection, tables, p, key) && ForwardPipelineOf(a_catalog, key, targets).valid();
	}

	bool IndirectDraws::Impl::ReflectionPhaseReady(const SceneTables& a_tables, std::uint32_t a_slot) const
	{
		const std::uint32_t p = a_slot < a_tables.objects.size() ? a_tables.objects[a_slot].pipelineIndex : ~0u;
		return p < reflection.slotPipelines.size() && reflection.slotPipelines[p].valid();
	}

	void IndirectDraws::ExecuteReflection()
	{
		ZoneScopedN("CS.DCLF.ExecuteReflection");
		auto& reflection = impl->reflection;
		++reflection.updates;
		reflection.executedFrame = Scene::FrameView::Get().GetFrame();
		// The frame's faces, taken whatever happens: the next frame captures its own.
		auto faces = reflection.faces;
		for (auto& face : reflection.faces)
			face.captured = false;
		auto skip = [&](std::uint32_t a_cause) { ++reflection.skipped[a_cause]; };
		const auto set = Scene::FrameView::Get().FrameClaims();
		if (!set || !(set->drawn & kSetReflection))
			return skip(0);
		if (std::none_of(faces.begin(), faces.end(), [](const auto& a_face) { return a_face.captured; }))
			return skip(2);
		// What the faces draw from: the frame's scene list (the installed payload, as the shadow views': a member is the reflection's from
		// the frame it joins) with this frame's depth commit's index pool, depth inputs, object records and geometry rows - the frame's
		// publication whole - and the frame before's colour frame record, in the backing it was written to.
		const std::uint32_t frameNumber = Scene::FrameView::Get().GetFrame();
		const auto main = impl->resources;
		const auto& depthCommit = main ? main->committed[kDepthShape] : Resources::Committed{};
		const auto& colourCommit = main ? main->committed[kColourShape] : Resources::Committed{};
		const std::uint64_t adoptedSerial = impl->Adopted().Serial();
		if (!main || !impl->scene || depthCommit.frame != frameNumber || colourCommit.frame + 1 != frameNumber || depthCommit.adoptedSerial != adoptedSerial ||
			colourCommit.adoptedSerial != adoptedSerial)
			return skip(3);
		const auto draws = impl->installedDraws;
		const MainPayload* listed = impl->InstalledPayload() && draws ? draws->payloads[kAsyncZPrepass].get() : nullptr;
		if (!listed)
			return skip(3);
		// The main part of the scene list: its region and the main frame inputs (the shadow frame inputs after them are no main input).
		const auto listInputs = static_cast<std::uint32_t>(listed->resident.Count() + listed->inputList.size());
		const auto indirect = FrameIndirectState();
		if (!indirect.valid || !impl->SetupView(ViewKind::Reflection) || !impl->ImportReflectionCube(reflection.cube.get()))
			return skip(4);
		// Not DCLF's this frame (no claims, or the cube was imported since the graph was built): the engine rendered the faces whole.
		if (!reflection.covered)
			return skip(1);
		// The selected revision's shape and recording (DecideCoverage covered the faces): trusted, its map, buckets and latch are what
		// the values go into.
		const auto revision = impl->RevisionOf(Epoch::Reflection);
		if (!revision)
			return skip(1);
		const auto shape = revision.shape->Value<ViewFrame>();
		const auto& payloads = draws->payloads;
		const std::array<const MainPayload*, 2> listedPayloads{ payloads[0].get(), payloads[1].get() };
		auto owners = impl->EpochOwners(listedPayloads);
		if (!owners)
			return skip(4);
		auto& scene = *impl->scene;
		const auto treeLod = scene.treeLodPipelines.load(std::memory_order_acquire);
		// Tree LOD's row as the last depth commit uploaded it, its texture bound again for this execution, when the revision's faces have
		// tree LOD (their lists hold the scene's shape slots: one unit group) and the faces' tree LOD is DCLF's.
		TreeLod::DrawRow treeRow = scene.treeLodRow;
		const bool trees = shape->tree.valid() && reflection.treePipeline.valid() && treeLod && scene.treeLodCull;
		if (!(trees && reflection.treeOwned && treeRow.shapeSlots && TreeLodTextureBinding(treeRow, *owners)))
			treeRow.shapeSlots = 0;
		std::array<ViewFace, kReflectionFaces> views{};
		for (std::uint32_t f = 0; f < kReflectionFaces; ++f)
			if (faces[f].captured)
				views[f] = { true, std::span(faces[f].perFrame.data(), faces[f].perFrameBytes), faces[f].viewProj, faces[f].eye };
		std::uint32_t drawn = 0;
		const bool ok = RenderGraphRuntime::Get().ExecuteEpoch(RenderGraphRuntime::Segment::Reflection, [&](org::RenderGraph&) {
			drawn = impl->CommitView(*shape, views, listInputs, &treeRow);
			impl->SubmitRevisionRecording(Epoch::Reflection, *revision.recordings, 0);
		}, owners);
		if (!ok) {
			logger::error("[DCLF] the reflection faces' epoch failed");
			return skip(5);
		}
		++reflection.epochs;
		reflection.facesDrawn += drawn;
	}

}
