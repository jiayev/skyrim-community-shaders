#if defined(CS_HAS_RENDER_GRAPH) && defined(CS_HAS_ORG_MODULE_SERVICES)
#	include "Internal.h"

#	include "Features/DrawcallLimitFix/Engine/ReflectionFaces.h"
#	include "Features/DrawcallLimitFix/Engine/SunViews.h"
#	include "Features/DrawcallLimitFix/Scene/LightingDescriptors.h"

namespace DCLF::Scene
{
	std::uintptr_t HiddenStoreSiteAt(std::uint32_t a_index);
}

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

		/** @brief The inverse of a row-major 4x4 (cofactors, in double); zero when singular. */
		std::array<float, 16> Inverse(const std::array<float, 16>& a_m)
		{
			std::array<double, 16> m{}, inv{};
			for (std::uint32_t i = 0; i < 16; ++i)
				m[i] = a_m[i];
			inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
			inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
			inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
			inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
			inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
			inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
			inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
			inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
			inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
			inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
			inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
			inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
			inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
			inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
			inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
			inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
			const double det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
			std::array<float, 16> out{};
			if (det == 0.0)
				return out;
			for (std::uint32_t i = 0; i < 16; ++i)
				out[i] = static_cast<float>(inv[i] / det);
			return out;
		}

		/**
		 * @brief VS_PerFrame (b12) for a face drawn through a_camera (T4), SunViews::kBlockBytes at a_out, as the engine uploads it for a
		 * face (measured against its block at the face's draws, y80-y82):
		 * - the camera's registers as SetCameraData builds them (the accumulator render's flags 8: no jitter, so the unjittered matrices
		 *   are the others): CameraView, CameraProj, CameraViewProj and CameraViewProjUnjittered (SunViews::Block), CameraPosAdjust;
		 * - c20 the projection's inverse and c24 the projection (the engine's order: FrameBuffer.hlsli names them the other way), c28
		 *   the view's inverse, c32 the view-projection's, c36 the projection's;
		 * - zero: CameraPreviousViewProjUnjittered (c16) and CameraPreviousPosAdjust (c41, fDRClampOffset in w included);
		 * - the frame's: FrameParams (c42) as a_frame, the renderer's last upload, holds it; the dynamic resolution parameters (c43, c44)
		 *   from the renderer state (State::dynamicResolution*Ratio, fDRClampOffset).
		 */
		void FaceBlock(const SunViews::Cascade& a_camera, std::span<const std::byte> a_frame, std::byte* a_out)
		{
			std::memcpy(a_out, a_frame.data(), SunViews::kBlockBytes);
			std::array<std::byte, SunViews::kBlockBytes> camera{};
			SunViews::Block(a_camera, camera.data());
			auto* out = reinterpret_cast<float*>(a_out);
			std::memcpy(out, camera.data(), 16 * 4 * sizeof(float));  // c0-c15
			auto transposed = [&](std::uint32_t a_register, const std::array<float, 16>& a_m) {
				for (std::uint32_t row = 0; row < 4; ++row)
					for (std::uint32_t c = 0; c < 4; ++c)
						out[a_register * 4 + row * 4 + c] = a_m[c * 4 + row];
			};
			// CameraPreviousViewProjUnjittered: zero for a face (y80's parity: the camera state the face's SetCameraData fills has none).
			std::memset(out + 16 * 4, 0, 4 * 4 * sizeof(float));
			const auto inverseProj = Inverse(a_camera.proj);
			transposed(20, inverseProj);
			transposed(24, a_camera.proj);
			transposed(28, Inverse(a_camera.view));
			transposed(32, Inverse(a_camera.viewProj));
			transposed(36, inverseProj);
			out[160] = a_camera.eye[0];
			out[161] = a_camera.eye[1];
			out[162] = a_camera.eye[2];
			std::memset(out + 41 * 4, 0, 4 * sizeof(float));
			if (auto* state = RE::BSGraphics::State::GetSingleton()) {
				const auto& data = state->GetRuntimeData();
				static auto* clampSetting = RE::GetINISetting("fDRClampOffset:Display");
				const float clamp = clampSetting ? clampSetting->data.f : 0.0f;
				const float w = data.dynamicResolutionWidthRatio, h = data.dynamicResolutionHeightRatio;
				const float pw = data.dynamicResolutionPreviousWidthRatio, ph = data.dynamicResolutionPreviousHeightRatio;
				const float c43[4] = { w, h, pw, ph };
				const float c44[4] = { 1.0f / w, 1.0f / h, w - clamp, pw - clamp };
				std::memcpy(out + 43 * 4, c43, sizeof(c43));
				std::memcpy(out + 44 * 4, c44, sizeof(c44));
			}
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
		++reflection.facesCaptured;
		if (reflection.executedFrame == SceneStore::Get().GetFrame())
			++reflection.lateFaces;
		auto& face = reflection.faces[a_face];
		face = {};
		// The frame's registers of VS_PerFrame (FaceBlock): the block as the renderer last uploaded it - from the mirror, or from
		// Community Shaders' copy of the same buffer until the mirror has seen a write.
		std::span<const std::byte> frame;
		auto& mirror = ConstantMirror::Get();
		if (auto* perFrame = *globals::game::perFrame.get()) {
			mirror.Watch(perFrame);
			frame = mirror.Contents(perFrame);
		}
		if (frame.size() < SunViews::kBlockBytes) {
			const auto& cached = globals::game::frameBufferCached.data;
			static_assert(sizeof(cached) >= SunViews::kBlockBytes);
			frame = std::as_bytes(std::span(&cached, 1));
		}
		SunViews::Cascade view;
		SunViews::CameraMatrices(a_camera, view);
		FaceBlock(view, frame, face.perFrame.data());
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
		// The parity's half (T4): the face the engine has just drawn, as its accumulator render left it - the bound colour target's
		// face and VS_PerFrame (b12) from the mirror - against DCLF's (ReflectionFaceCamera).
		auto& reflection = impl->reflection;
		const std::uint32_t f = reflection.orientedFace;
		if (!SunViews::ParityEnabled() || !ReflectionFaces::Plain() || f >= kReflectionFaces || !reflection.faces[f].captured)
			return;
		const auto& face = reflection.faces[f];
		auto& parity = reflection.cameraParity;
		++parity.faces;
		ID3D11RenderTargetView* bound = nullptr;
		globals::d3d::context->OMGetRenderTargets(1, &bound, nullptr);
		const auto target = TargetOf(bound);
		if (bound)
			bound->Release();
		if (target.face != f || target.texture.get() != reflection.cube.get()) {
			if (parity.slices++ == 0 && parity.first.empty())
				parity.first = fmt::format("face {}: the engine drew into face {}", f, target.face);
		}
		auto* perFrame = *globals::game::perFrame.get();
		const auto contents = perFrame ? ConstantMirror::Get().Contents(perFrame) : std::span<const std::byte>{};
		if (contents.size() < SunViews::kBlockBytes)
			return;
		const auto* theirs = reinterpret_cast<const float*>(contents.data());
		const auto* mine = reinterpret_cast<const float*>(face.perFrame.data());
		// Every register within rounding but the inverses DCLF computes with arithmetic of its own (c20-c23, c32-c39: within 1e-3).
		float largest = 0.0f;
		std::int32_t first = -1;
		for (std::uint32_t i = 0; i < SunViews::kBlockBytes / sizeof(float); ++i) {
			const std::uint32_t c = i / 4;
			const bool loose = (c >= 20 && c < 24) || (c >= 32 && c < 40);
			const float d = std::abs(mine[i] - theirs[i]);
			const bool same = loose ? d <= 1e-3f * std::max(1.0f, std::abs(theirs[i])) : SunViews::Close(mine[i], theirs[i]);
			if (!same) {
				largest = std::max(largest, d);
				parity.registers |= 1ull << c;
				if (first < 0)
					first = static_cast<std::int32_t>(i);
			}
		}
		if (first >= 0) {
			parity.largest = std::max(parity.largest, largest);
			if (parity.blocks++ == 0 && parity.first.empty()) {
				const std::uint32_t row = static_cast<std::uint32_t>(first) / 4 * 4;
				parity.first = fmt::format("face {}: c{} ({} {} {} {}), the engine's ({} {} {} {})", f, row / 4, mine[row], mine[row + 1], mine[row + 2], mine[row + 3], theirs[row],
					theirs[row + 1], theirs[row + 2], theirs[row + 3]);
			}
		}
	}

	namespace
	{
		/** @brief T6b0: the residue by the furthest stage its geometries reached, with their ages since tracked and the stale verdicts' shows. */
		std::string ResidueStageReport(const SceneStore::ResidueClasses& a_classes)
		{
			std::uint64_t total = 0;
			for (const auto count : a_classes.stages)
				total += count;
			if (!total)
				return {};
			auto histogram = [](const auto& a_buckets) {
				std::string text;
				for (std::size_t b = 0; b < a_buckets.size(); ++b)
					if (a_buckets[b])
						text += fmt::format("{}{}:{}", text.empty() ? "" : " ", SceneStore::kAgeBucketNames[b], a_buckets[b]);
				return text.empty() ? std::string("-") : text;
			};
			std::string text = fmt::format("[DCLF] residue by stage (T6b0): {} passes", total);
			for (std::size_t s = 0; s < SceneStore::kStageCount; ++s) {
				if (!a_classes.stages[s])
					continue;
				text += fmt::format("; {} {} ({:.0f}%, frames since tracked {})", a_classes.stages[s], SceneStore::kStageNames[s], 100.0 * a_classes.stages[s] / total,
					histogram(a_classes.ages[s]));
				if (s == SceneStore::kStageWaiting) {
					std::string why;
					for (std::size_t w = 0; w < a_classes.waitingBy.size(); ++w)
						if (a_classes.waitingBy[w])
							why += fmt::format("{}{}:{}", why.empty() ? "" : " ", w, a_classes.waitingBy[w]);
					text += " [reasons " + why + "]";
				}
				if (s == SceneStore::kStageUnbound) {
					std::string why;
					for (std::size_t w = 0; w < a_classes.unboundBy.size(); ++w)
						if (a_classes.unboundBy[w])
							why += fmt::format("{}{} {}", why.empty() ? "" : ", ", w ? PrimaryCull::kSyntheticFailNames[w] : "no failed membership pass", a_classes.unboundBy[w]);
					for (std::size_t r = 0; r < a_classes.unboundJoin.size(); ++r)
						if (a_classes.unboundJoin[r])
							why += fmt::format("; join verdict {} {}", r ? kIneligibleNames[r] : "none", a_classes.unboundJoin[r]);
					text += " [" + why + "]";
				}
				if (s == SceneStore::kStageHiddenStale) {
					std::string sites;
					for (const auto& [site, count] : a_classes.staleSites)
						sites += fmt::format("{}{} {}", sites.empty() ? "" : ", ", site == ~0u ? std::string("no show seen") : fmt::format("{:#x}", Scene::HiddenStoreSiteAt(site)), count);
					text += fmt::format(" [last show by: {}; frames since it {}]", sites, histogram(a_classes.sinceShow));
				}
				if (!a_classes.stageFirst[s].empty())
					text += " (first: " + a_classes.stageFirst[s] + ")";
			}
			return text + "\n";
		}
	}

	std::uint32_t IndirectDraws::ReflectionRootsOwned(bool a_plain)
	{
		// The roots whose members DCLF's faces draw, in a plain update of a frame whose faces are DCLF's (DecideCoverage):
		// - LOD trees while the faces' tree LOD is DCLF's (PrepareReflection): drawn from DCLF's mirror, no registration needed.
		// - Not yet LOD land and objects (until T6b): the LOD chunks and objects the engine shows or attaches in motion take DCLF a
		//   few frames to bind (ClassifyResidue: hidden in its tables, or not bound yet), and meanwhile they are no reflection-phase
		//   members. Their roots stay; the members are withheld (PassCapture).
		auto& reflection = impl->reflection;
		auto& capture = PassCapture::Get();
		capture.WatchReflectionResidue(0);
		if (!a_plain || !ReflectionDrawable() || !impl->reflectionCovered || PassCapture::ParityBoth())
			return 0;
		const std::uint32_t owned = reflection.treeOwned ? ReflectionFaces::kTreeRoot : 0u;
		// The residue parity: what the engine registers into the faces and DCLF does not withhold, by root: the LOD roots' on every
		// 30th update (what skipping them would lose: T6b's gate), the tree root's on every 30th of those that skip it (kept then).
		if ((SunViews::ParityEnabled() || SceneStore::TimelineEnabled()) && reflection.residueUpdates++ % 30 == 0) {
			capture.WatchReflectionResidue(ReflectionFaces::kLodRoots | owned);
			return 0;
		}
		return owned;
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
		// What the faces draw from: the frame's scene list (its ring entry, as the shadow views'; U5: a member is the reflection's from
		// the frame it joins) with this frame's depth commit's index pool, depth inputs, object records and geometry rows - the frame's
		// publication whole - and the frame before's colour frame record, in the backing it was written to.
		auto& store = SceneStore::Get();
		const std::uint32_t frameNumber = store.GetFrame();
		const auto main = impl->resources;
		const auto& depthCommit = main ? main->committed[kDepthShape] : Resources::Committed{};
		const auto& colourCommit = main ? main->committed[kColourShape] : Resources::Committed{};
		if (!main || !main->inputsDepth || !impl->scene || depthCommit.frame != frameNumber || colourCommit.frame + 1 != frameNumber ||
			depthCommit.sceneGeneration != impl->scene->generation || depthCommit.objectCapacity != main->objectCapacity ||
			depthCommit.rowsGeneration != main->MainRowsGeneration() || colourCommit.rowsGeneration != main->MainRowsGeneration())
			return skip(3);
		const auto& ring = impl->ringFrame;
		const MainPayload* listed = ring.valid && ring.draws ? ring.draws->payloads[kAsyncZPrepass].get() : nullptr;
		if (!listed)
			return skip(3);
		// The main part of the scene list: its region and the main frame inputs (the shadow frame inputs after them are no main input).
		const auto listInputs = static_cast<std::uint32_t>(listed->resident.Count() + listed->inputList.size());
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
				const std::uint32_t inputs = face.captured ? listInputs : 0u;
				BuildDrawsLatch latch{};
				latch.dispatch[0] = (inputs + 63) / 64;
				latch.dispatch[1] = 1;
				latch.dispatch[2] = 1;
				latch.drawCount = inputs;
				latch.cullFlags = 1;  // the frustum alone, near plane included
				latch.viewBits = kViewReflection;  // the main list's: the reflection phase's members
				latch.visibilityStamp = frameNumber & 0x0FFFFFFFu;
				latch.placementsIndex = FrameValues::Get().PlacementsIndex();
				latch.fadeSeedsIndex = FrameValues::Get().FadeSeedsIndex();
				// The frame's scene list: its ring entry (step 6e E4).
				Impl::RingLatch(ring, latch);
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
		// T4: the faces' cameras against the engine's at their draws, and the LOD roots the engine did not cull.
		const auto roots = ReflectionFaces::TakeRootStats();
		const auto residue = PassCapture::Get().TakeReflectionResidue();
		std::string residueKinds, residueStages;
		{
			const auto classes = SceneStore::Get().TakeResidueClasses();
			for (std::size_t k = 0; k < SceneStore::kResidueKinds; ++k) {
				if (!classes.counts[k])
					continue;
				const std::string_view name = k < SceneStore::kResidueUntracked ? kIneligibleNames[k] :
				                              k == SceneStore::kResidueUntracked ? "untracked" :
				                              k == SceneStore::kResidueUnbound   ? "eligible, unbound" :
				                              k == SceneStore::kResidueNotReflection ? "bound, not a reflection member" :
				                                                                       "a reflection member";
				residueKinds += fmt::format("{}{} {}{}", residueKinds.empty() ? " [" : "; ", classes.counts[k], name, classes.first[k].empty() ? "" : " (" + classes.first[k] + ")");
			}
			if (!residueKinds.empty())
				residueKinds += fmt::format("; {} geometries, {} of them on 10 frames or more]", classes.seen.size(), classes.persistent);
			if (SceneStore::TimelineEnabled())
				residueStages = ResidueStageReport(classes);
		}
		auto& parity = reflection.cameraParity;
		text += fmt::format("[DCLF] reflection faces (T4: DCLF's cameras, the engine's tree LOD root skipped): {} updates, {} without the tree root; parity: {} faces, {} blocks "
							"and {} slices differ (registers {:#x}, largest {:.3g}){}{}; residue: {} tree LOD passes{}, {} under the LOD land and objects roots (kept until "
							"T6b){}{}\n",
			roots.updates, roots.treeSkipped, parity.faces, parity.blocks, parity.slices, parity.registers, parity.largest,
			parity.blocks || parity.slices ? " <- FACE CAMERA" : (parity.faces ? " <- OK" : ""), parity.first.empty() ? "" : " (first: " + parity.first + ")",
			residue.treePasses, residue.treePasses ? " <- REFLECTION RESIDUE" : "", residue.lodPasses, residue.first.empty() ? "" : " (first: " + residue.first + ")", residueKinds);
		text += residueStages;
		parity = {};
		reflection.facesCaptured = reflection.updates = reflection.epochs = reflection.facesDrawn = reflection.lateFaces = 0;
		reflection.skipped = {};
		return text;
	}
}
#endif
