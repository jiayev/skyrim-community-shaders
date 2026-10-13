#if defined(CS_HAS_RENDER_GRAPH) && defined(CS_HAS_ORG_MODULE_SERVICES)
#	include "DrawsState.h"
#include "Features/DrawcallLimitFix/Scene/FrameView/FrameView.h"
#include "Features/DrawcallLimitFix/Engine/Access/EngineAccess.h"
#	include "Events.h"
#	include "Observers.h"
#	include "Features/DrawcallLimitFix/Diagnostics/Engine/ShadowViewTally.h"
#	include "Features/DrawcallLimitFix/Diagnostics/Engine/SunCasters.h"

// The shadow epochs' checks (render thread): DCLF's views against the engine's (the viewport, eye, matrices and caster volume at each
// view's draw, the predicted target, the occlusion maps' views, the sun's full-frustum processes), the sun views' culling against the
// engine's cascade culls, the sun entry test on the GPU against the CPU's, and the index pool's ranges against the tables.

namespace DCLF::Draws
{
	// ShadowEpochs.cpp's: the engine's rasterizer state id, and where and with which block the engine has just drawn a view.
	std::uint32_t EngineRasterStateId(std::uint32_t a_fill, std::uint32_t a_cull, std::uint32_t a_bias, std::uint32_t a_scissor, std::uint32_t a_renderMode);
	void CaptureViewTarget(PendingView& a_view, std::uint32_t a_target);
	void CapturePerFrame(PendingView& a_view);
}

namespace DCLF::Diagnostics
{
	namespace
	{
		/** @brief BuildDrawsCS's Culled, on the CPU: the latch's planes, then its view-projection's frustum over the bound's box. */
		bool CulledByLatch(const BuildDrawsLatch& a_latch, const RE::NiBound& a_bound, std::string* a_why = nullptr)
		{
			const float c[3] = { a_bound.center.x, a_bound.center.y, a_bound.center.z };
			const float r = a_bound.radius;
			for (std::uint32_t p = 0; p < 6; ++p)
				if ((a_latch.cullPlaneMask & (1u << p)) &&
					a_latch.cullPlanes[p][0] * c[0] + a_latch.cullPlanes[p][1] * c[1] + a_latch.cullPlanes[p][2] * c[2] - a_latch.cullPlanes[p][3] < -r) {
					if (a_why)
						*a_why = fmt::format("cull plane {} of mask {:#x}, distance {:.0f}", p, a_latch.cullPlaneMask,
							a_latch.cullPlanes[p][0] * c[0] + a_latch.cullPlanes[p][1] * c[1] + a_latch.cullPlanes[p][2] * c[2] - a_latch.cullPlanes[p][3]);
					return true;
				}
			const bool noNear = (a_latch.cullFlags & kCullNoNearPlane) != 0;
			bool out[6] = { true, true, true, true, true, true };
			for (std::uint32_t corner = 0; corner < 8; ++corner) {
				const float x = c[0] + ((corner & 1) ? r : -r), y = c[1] + ((corner & 2) ? r : -r), z = c[2] + ((corner & 4) ? r : -r);
				float clip[4];
				for (std::uint32_t row = 0; row < 4; ++row)
					clip[row] = a_latch.viewProj[row * 4 + 0] * x + a_latch.viewProj[row * 4 + 1] * y + a_latch.viewProj[row * 4 + 2] * z + a_latch.viewProj[row * 4 + 3];
				out[0] = out[0] && clip[0] < -clip[3];
				out[1] = out[1] && clip[0] > clip[3];
				out[2] = out[2] && clip[1] < -clip[3];
				out[3] = out[3] && clip[1] > clip[3];
				out[4] = out[4] && clip[2] < 0 && !noNear;
				out[5] = out[5] && clip[2] > clip[3];
			}
			if (a_why)
				for (std::uint32_t side = 0; side < 6; ++side)
					if (out[side]) {
						*a_why = fmt::format("clip side {} (near plane {})", side, noNear ? "off" : "on");
						break;
					}
			return out[0] || out[1] || out[2] || out[3] || out[4] || out[5];
		}
	}

	void CheckIndexPool(const IndexPoolUpdated& a_updated)
	{
		// Every range against its slot's buffer as the tables have it now (a slot whose buffer changed with no log entry would keep
		// drawing the old buffer's indices).
		auto& checks = DrawsState::Get().indexPoolChecks;
		if ((++checks % 300) != 0)
			return;
		const auto& p = a_updated.pool;
		const auto& a_tables = a_updated.tables;
		const std::uint32_t count = a_updated.count;
		std::uint32_t held = 0, stale = 0, unheld = 0, firstStale = IndexPool::kNoRange;
		for (std::uint32_t g = 0; g < count; ++g) {
			const auto& geometry = a_tables.geometries[g];
			const auto& range = p.slots[g];
			const bool wants = geometry.indexAddress && geometry.indexBytes;
			if (range.first == IndexPool::kNoRange) {
				unheld += wants ? 1 : 0;
				continue;
			}
			++held;
			if (!wants || range.address != geometry.indexAddress || range.bytes != geometry.indexBytes) {
				if (stale++ == 0)
					firstStale = g;
			}
		}
		logger::info("[DCLF] index pool parity: {} slots, {} ranges ({} indices held of {}), {} stale, {} without a range ({} waiting for the pool's growth){}{}",
			count, held, p.indicesHeld, p.capacity, stale, unheld, p.waiting.size(), stale || unheld > p.waiting.size() ? " <- INDEX POOL" : " <- OK",
			firstStale != IndexPool::kNoRange ? fmt::format("; first stale: slot {}", firstStale) : std::string());
	}

	void CheckShadowTarget(const ShadowTargetPredicted& a_predicted)
	{
		// DCLF's predicted depth target and slice against the renderer's at the draw.
		auto& s = DrawsState::Get().shadowTargets;
		const auto& shadowState = globals::game::shadowState->GetRuntimeData();
		const auto* shadowView = a_predicted.shadowView;
		++s.checked;
		const std::uint32_t engineTarget = shadowState.depthStencil, engineSlice = shadowState.depthStencilSlice;
		if (engineTarget != a_predicted.target || engineSlice != a_predicted.slice) {
			if (s.differ++ == 0)
				s.first = fmt::format("view {} ({}{}, light {} descriptor {}, capture {}): target {} slice {}, the engine's {} slice {}", a_predicted.viewId,
					ShadowViews::KindName(shadowView->kind), shadowView->focus ? " focus" : "", shadowView->lightIndex, shadowView->descriptor, a_predicted.occurrence,
					a_predicted.target, a_predicted.slice, engineTarget, engineSlice);
		}
		if (s.checked == 3000) {
			logger::info("[DCLF] shadow targets (T3d: DCLF's predicted depth target and slice against the renderer's at the draw): {} views, {} differ{}{}", s.checked,
				s.differ, s.differ ? " <- SHADOW TARGET" : " <- OK", s.first.empty() ? "" : "; first: " + s.first);
			s = {};
		}
	}

	void CheckOwnViewport(const OwnShadowViewport& a_own)
	{
		// The viewport and eye CaptureViewTarget read from the renderer's state, against DCLF's (its port times the depth target's size).
		auto& p = ShadowViewTally(a_own.kind);
		++p.views;
		auto* texture = globals::game::renderer->GetDepthStencilData().depthStencils[a_own.target].texture;
		if (!a_own.cascade || !texture) {
			++p.missing;
			return;
		}
		const auto& a_view = a_own.view;
		D3D11_TEXTURE2D_DESC desc{};
		texture->GetDesc(&desc);
		const auto port = SunViews::Viewport(a_own.cascade->port, desc.Width, desc.Height);
		const RE::NiPoint3 eye{ a_own.cascade->eye[0], a_own.cascade->eye[1], a_own.cascade->eye[2] };
		// The depth range against the camera state's (0x14202b130, FUN_140e57e10), which every draw's state flush starts from
		// (FUN_140e4b5b0): the renderer's viewport holds the last draw's, less its depth bias's step (0x143286a80[bias]) when it had
		// one - a per-draw state, not the view's.
		static const REL::Relocation<const float*> cameraDepthRange{ REL::Offset(Engine::Address::kCameraDepthRange) };
		const float engineMin = cameraDepthRange.get()[0], engineMax = cameraDepthRange.get()[1];
		if (port[0] != a_view.x || port[1] != a_view.y || port[2] != a_view.width || port[3] != a_view.height || engineMin != 0.0f || engineMax != 1.0f) {
			++p.viewport;
			p.Note("viewport", 1.0f, fmt::format("descriptor {}: ({} {}) {}x{}, the engine's ({} {}) {}x{} camera depth range {}-{}", a_own.descriptor, port[0], port[1],
				port[2], port[3], a_view.x, a_view.y, a_view.width, a_view.height, engineMin, engineMax));
		}
		if (!SunViews::Close(eye.x, a_view.eye.x) || !SunViews::Close(eye.y, a_view.eye.y) || !SunViews::Close(eye.z, a_view.eye.z)) {
			++p.eye;
			p.Note("eye", (eye - a_view.eye).Length(),
				fmt::format("descriptor {}: ({} {} {}), the engine's ({} {} {})", a_own.descriptor, eye.x, eye.y, eye.z, a_view.eye.x, a_view.eye.y, a_view.eye.z));
		}
	}

	void CheckOwnCamera(const OwnShadowCamera& a_own)
	{
		// The matrices the draws use: CameraView, CameraProj and CameraViewProj (c0-c11; Utility.hlsl's paraboloid modes read the view
		// too), and the engine's caster volume, against DCLF's block (the engine's taken for this: Policy::CaptureEngineShadowBlock).
		auto& p = ShadowViewTally(a_own.kind);
		const auto& a_view = a_own.view;
		const auto& a_cascade = a_own.cascade;
		std::array<std::byte, SunViews::kBlockBytes> block{};
		SunViews::Block(a_cascade, block.data());
		const auto* mine = reinterpret_cast<const float*>(block.data());
		const auto* theirs = reinterpret_cast<const float*>(a_view.perFrame.data());
		float largest = 0.0f;
		std::int32_t first = -1;
		for (std::uint32_t i = 0; i < 48; ++i) {
			if (!SunViews::Close(mine[i], theirs[i]) && first < 0)
				first = static_cast<std::int32_t>(i);
			largest = std::max(largest, std::abs(mine[i] - theirs[i]));
		}
		if (first >= 0) {
			++p.viewProj;
			const std::uint32_t row = static_cast<std::uint32_t>(first) / 4 * 4;
			p.Note("matrices", largest, fmt::format("descriptor {}: c{} ({} {} {} {}), the engine's ({} {} {} {})", a_own.descriptor, row / 4, mine[row], mine[row + 1],
				mine[row + 2], mine[row + 3], theirs[row], theirs[row + 1], theirs[row + 2], theirs[row + 3]));
		}
		SunViews::Planes captured;
		for (std::uint32_t q = 0; q < 6; ++q)
			for (std::uint32_t i = 0; i < 4; ++i)
				captured.plane[q][i] = a_view.cullPlanes[q][i];
		captured.mask = a_view.cullPlaneMask;
		if (!SunViews::SamePlanes(a_cascade.caster, captured)) {
			++p.casters;
			p.Note("caster volume", 1.0f, fmt::format("descriptor {}: mask {:#x}, the engine's {:#x}", a_own.descriptor, a_cascade.caster.mask, captured.mask));
		}
	}

	void CheckSunEntryPlanes(const SunEntryPlanesTaken& a_taken)
	{
		// The processes' own planes after the full-frustum cull against DCLF's (SunViews), which the entry rule tests.
		auto& tally = ShadowViewTally(ShadowView::Sun);
		const auto& sunViews = SunViews::Get().Current();
		if (!a_taken.taken) {
			++tally.missing;
			tally.Note("missing full frustum", 0.0f, fmt::format("inputs of frame {}, DCLF's views of frame {} (valid {}, {} processes)", a_taken.frame,
				sunViews.sceneFrame, sunViews.valid, sunViews.fullFrusta.size()));
			return;
		}
		auto* node = globals::game::smState ? globals::game::smState->shadowSceneNode[0] : nullptr;
		auto* sun = node ? node->GetRuntimeData().sunShadowDirLight : nullptr;
		if (!sun)
			return;
		std::size_t index = 0;
		for (const auto& process : sun->GetShadowDirectionalLightRuntimeData().fullFrustumCullingProcessArray) {
			if (!process)
				continue;
			SunViews::Planes engine;
			for (std::uint32_t p = 0; p < 6; ++p)
				engine.plane[p] = { process->planes.cullingPlanes[p].normal.x, process->planes.cullingPlanes[p].normal.y, process->planes.cullingPlanes[p].normal.z,
					process->planes.cullingPlanes[p].constant };
			engine.mask = process->planes.activePlanes.underlying() & 0x3Fu;
			++tally.fullFrusta;
			if (index >= sunViews.fullFrusta.size()) {
				++tally.fullFrustumPlanes;
				tally.Note("full-frustum planes", 1.0f, fmt::format("process {}: DCLF has {} processes", index, sunViews.fullFrusta.size()));
			} else if (!SunViews::SamePlanes(sunViews.fullFrusta[index], engine)) {
				++tally.fullFrustumPlanes;
				tally.Note("full-frustum planes", 1.0f, fmt::format("process {}: {}", index, SunViews::DescribePlanes(sunViews.fullFrusta[index], engine)));
			}
			++index;
		}
	}

	void CheckOcclusionView(const OcclusionCaptured& a_captured)
	{
		// Where the engine's RenderMask has just drawn the map (the clear, and whatever SetupMask registered), with which camera and
		// state, against DCLF's view of it this frame (OcclusionView). The renderer's state here is what the view's last pass left, or
		// whatever came before when nothing drew; the cull mode is the Utility shader's per pass, so only the fill, bias and scissor modes
		// are compared.
		const std::uint32_t a_view = a_captured.view;
		if (a_view >= kOcclusionViews || !ActiveToggles().shadows || IndirectDraws::Get().Failed() || !OcclusionEnabled(a_view))
			return;
		const auto& occlusion = DrawsView::Get().occlusion[a_view];
		if (occlusion.capturedFrame != Scene::Coordinator::Get().GetFrame())
			return;
		auto& s = DrawsState::Get().occlusionViews[a_view];
		auto& shadowState = globals::game::shadowState->GetRuntimeData();
		const auto& mine = occlusion.view;
		PendingView theirs;
		const std::uint32_t target = shadowState.depthStencil;
		CaptureViewTarget(theirs, target);
		CapturePerFrame(theirs);
		const std::uint32_t rasterState = EngineRasterStateId(shadowState.rasterStateFillMode, 1, shadowState.rasterStateDepthBiasMode, shadowState.rasterStateScissorMode,
			kOcclusionRenderMode);
		++s.maps;
		// Every field compared; what differs is listed.
		std::string what;
		auto note = [&](std::string a_text) { what += (what.empty() ? "" : "; ") + a_text; };
		float largest = 0.0f;
		if (target != static_cast<std::uint32_t>(RE::RENDER_TARGETS_DEPTHSTENCIL::kPRECIPITATION_OCCLUSION_MAP) || shadowState.depthStencilSlice != mine.slice)
			note(fmt::format("target {} slice {}", target, shadowState.depthStencilSlice));
		if (rasterState != mine.rasterState)
			note(fmt::format("rasterizer state {} (fill {}, bias {}, scissor {}), DCLF's {}", rasterState, shadowState.rasterStateFillMode, shadowState.rasterStateDepthBiasMode,
				shadowState.rasterStateScissorMode, mine.rasterState));
		// The depth range against the camera state's (0x14202b130), as the shadow views' check: the renderer's viewport holds the last
		// draw's, less its depth bias's step when it had one - a per-draw state, not the view's.
		static const REL::Relocation<const float*> cameraDepthRange{ REL::Offset(Engine::Address::kCameraDepthRange) };
		const float engineMin = cameraDepthRange.get()[0], engineMax = cameraDepthRange.get()[1];
		if (theirs.dsvFormat != mine.dsvFormat || theirs.x != mine.x || theirs.y != mine.y || theirs.width != mine.width || theirs.height != mine.height ||
			engineMin != mine.minDepth || engineMax != mine.maxDepth)
			note(fmt::format("viewport ({} {}) {}x{} {}-{} format {}, DCLF's ({} {}) {}x{} {}-{} format {}", theirs.x, theirs.y, theirs.width, theirs.height, engineMin, engineMax,
				static_cast<int>(theirs.dsvFormat), mine.x, mine.y, mine.width, mine.height, mine.minDepth, mine.maxDepth, static_cast<int>(mine.dsvFormat)));
		{
			// The registers the depth draws read: CameraView, CameraProj, CameraViewProj (c0-c11), and the eye.
			const auto* a = reinterpret_cast<const float*>(mine.perFrame.data());
			const auto* b = reinterpret_cast<const float*>(theirs.perFrame.data());
			std::int32_t first = -1;
			for (std::uint32_t i = 0; i < 48; ++i) {
				if (!SunViews::Close(a[i], b[i]) && first < 0)
					first = static_cast<std::int32_t>(i);
				largest = std::max(largest, std::abs(a[i] - b[i]));
			}
			for (std::uint32_t i = 160; i < 163; ++i) {
				if (!SunViews::Close(a[i], b[i]) && first < 0)
					first = static_cast<std::int32_t>(i);
				largest = std::max(largest, std::abs(a[i] - b[i]));
			}
			if (first >= 0) {
				const std::uint32_t row = static_cast<std::uint32_t>(first) / 4 * 4;
				note(fmt::format("c{} ({} {} {} {}), the engine's ({} {} {} {})", row / 4, a[row], a[row + 1], a[row + 2], a[row + 3], b[row], b[row + 1], b[row + 2], b[row + 3]));
			}
		}
		if (!what.empty()) {
			s.largest = std::max(s.largest, largest);
			if (s.differ++ == 0)
				s.first = std::move(what);
		}
		if (s.maps == 300) {
			logger::info("[DCLF] occlusion views (T4: DCLF's view of map {} from its camera against the engine's at RenderMask): {} maps, {} differ (largest {:.3g}){}{}", a_view,
				s.maps, s.differ, s.largest, s.differ ? " <- OCCLUSION VIEW" : " <- OK", s.first.empty() ? "" : "; first: " + s.first);
			s = {};
		}
	}

	void CheckSunEntry(const ShadowViewLatched& a_latched)
	{
		// The test BuildDraws makes on the placement row's entry sphere (the kept rows the epoch uploads), against the CPU's verdict from
		// the tables' entry, per input of a sun view.
		if (!a_latched.view.sunView || !ParityDue(a_latched.frame))
			return;
		const auto& payload = a_latched.payload;
		const auto& tablesNow = Scene::FrameView::Get().GetTables();
		static const std::vector<BindlessPlacement> kNone;
		const auto* frameRows = FrameValues::Get().RowsIfDone();
		const auto& placements = frameRows ? *frameRows : kNone;
		for (const auto& input : payload.Flat(a_latched.view.modeIndex)) {
			if (input.objectIndex >= placements.size())
				continue;
			const bool cpu = OutsideSunEntry(payload.inputs, tablesNow, input.objectIndex);
			++a_latched.stats.sunEntryChecks;
			a_latched.stats.sunEntryMismatches += cpu != OutsideSunEntryProcesses(payload.inputs.sunEntryProcesses, placements[input.objectIndex].sunEntry) ? 1 : 0;
		}
	}

	void CheckCascadeCulling(const ShadowViewLatched& a_latched)
	{
		// A sun view's culling against the engine's own cascade cull of the same frame: every caster of the set the engine's cull of the
		// view's cascade reached (SunCasters::Claimed) must pass the view's latch, or it is drawn by nobody - the engine skips it as the
		// set's, and DCLF's view rejects it.
		const auto& a_view = a_latched.view;
		const auto& a_latch = a_latched.latch;
		const std::uint32_t a_frame = a_latched.frame;
		const auto& a_payload = a_latched.payload;
		auto& s = DrawsState::Get().cascadeCulling;
		auto& checks = s.checks;
		auto& registered = s.registered;
		auto& rejected = s.rejected;
		auto& offscreen = s.offscreen;
		auto& first = s.first;
		if (!a_view.sunView || a_view.casterClass != 0)
			return;
		std::uint32_t frame = 0;
		const auto& registrations = SunCasters::Claimed(frame);
		if (frame != a_frame)
			return;
		const auto* shadowView = ShadowViews::Get().At(a_view.viewId);
		if (!shadowView)
			return;
		++checks;
		for (const auto& registration : registrations) {
			if (registration.descriptor != shadowView->descriptor || !registration.geometry)
				continue;
			++registered;
			std::string why;
			if (CulledByLatch(a_latch, registration.geometry->worldBound, &why)) {
				// Wholly past a side of the view's own projection (the bound's box against its clip x or y): the engine's looser sphere
				// cull registers it, but its draw would write no texel either. Not a caster nobody draws.
				if (why.starts_with("clip side 0") || why.starts_with("clip side 1") || why.starts_with("clip side 2") || why.starts_with("clip side 3")) {
					++offscreen;
					continue;
				}
				++rejected;
				if (first.empty()) {
					const auto& b = registration.geometry->worldBound;
					first = fmt::format("'{}' in cascade {} (view {}), bound ({:.0f} {:.0f} {:.0f}) r {:.0f}: {}", registration.geometry->name.c_str() ? registration.geometry->name.c_str() : "?",
						shadowView->descriptor, a_view.viewId, b.center.x, b.center.y, b.center.z, b.radius, why);
				}
			}
		}
		// The other direction: what the view draws that covers most of the map at the light's near plane (a caster in front of
		// everything), and anything drawn whose world transform is far from its own bound. From the rows the GPU reads.
		{
			const auto& records = DrawsView::Get().objectStore.records.Get();
			static const std::vector<BindlessPlacement> kNone;
			const auto* frameRows = FrameValues::Get().RowsIfDone();
			const auto& placements = frameRows ? *frameRows : kNone;
			const auto& tablesNow = Scene::FrameView::Get().GetTables();
			ankerl::unordered_dense::set<const RE::BSGeometry*> reached;
			for (const auto& registration : registrations)
				if (registration.descriptor == shadowView->descriptor)
					reached.insert(registration.geometry);
			struct Cover
			{
				float area;
				std::uint32_t object;
				float maxZ;
			};
			std::vector<Cover> covers;
			std::uint32_t drawn = 0, farWorld = 0, staleBuffers = 0;
			std::string farFirst, staleFirst;
			a_payload.ForEachInput(a_view.modeIndex, [&](const DrawInput& a_input) {
				const std::uint32_t object = a_input.objectIndex;
				if (object >= placements.size() || (a_input.flags & kObjectVolumetricOnly) || OutsideSunEntry(a_payload.inputs, tablesNow, object))
					return;
				const auto& placement = placements[object];
				RE::NiBound bound;
				bound.center = { placement.bound[0], placement.bound[1], placement.bound[2] };
				bound.radius = placement.bound[3];
				if (CulledByLatch(a_latch, bound))
					return;
				++drawn;
				// The geometry row the draw pulls from against the buffers the object's TriShape holds now.
				if (const auto* geometry = object < tablesNow.objectGeometry.size() ? tablesNow.objectGeometry[object] : nullptr;
					geometry && !geometry->GetGeometryRuntimeData().skinInstance && a_input.geometryIndex < tablesNow.geometries.size() &&
					a_input.geometryIndex < tablesNow.geometryLayerKey.size() && !tablesNow.geometryLayerKey[a_input.geometryIndex]) {
					const auto* triShape = geometry->GetGeometryRuntimeData().rendererData;
					const auto& row = tablesNow.geometries[a_input.geometryIndex];
					if (triShape && (reinterpret_cast<ID3D11Buffer*>(triShape->vertexBuffer) != row.vertexBuffer || reinterpret_cast<ID3D11Buffer*>(triShape->indexBuffer) != row.indexBuffer)) {
						if (staleBuffers++ == 0)
							staleFirst = fmt::format("'{}' (object {}, slot {}): row vertex {} index {}, TriShape vertex {} index {}", geometry->name.c_str() ? geometry->name.c_str() : "?",
								object, a_input.geometryIndex, static_cast<const void*>(row.vertexBuffer), static_cast<const void*>(row.indexBuffer),
								static_cast<const void*>(triShape->vertexBuffer), static_cast<const void*>(triShape->indexBuffer));
					}
				}
				const float dx = placement.world[3] - bound.center.x, dy = placement.world[7] - bound.center.y, dz = placement.world[11] - bound.center.z;
				if (std::sqrt(dx * dx + dy * dy + dz * dz) > 4.0f * bound.radius + 512.0f) {
					if (farWorld++ == 0) {
						const auto* geometry = object < tablesNow.objectGeometry.size() ? tablesNow.objectGeometry[object] : nullptr;
						farFirst = fmt::format("'{}' world at ({:.0f} {:.0f} {:.0f}), bound ({:.0f} {:.0f} {:.0f}) r {:.0f}", geometry && geometry->name.c_str() ? geometry->name.c_str() : "?",
							placement.world[3], placement.world[7], placement.world[11], bound.center.x, bound.center.y, bound.center.z, bound.radius);
					}
				}
				float minX = 1, maxX = -1, minY = 1, maxY = -1, maxZ = -1e30f;
				for (std::uint32_t corner = 0; corner < 8; ++corner) {
					const float p[3] = { bound.center.x + ((corner & 1) ? bound.radius : -bound.radius), bound.center.y + ((corner & 2) ? bound.radius : -bound.radius),
						bound.center.z + ((corner & 4) ? bound.radius : -bound.radius) };
					float clip[4];
					for (std::uint32_t row = 0; row < 4; ++row)
						clip[row] = a_latch.viewProj[row * 4] * p[0] + a_latch.viewProj[row * 4 + 1] * p[1] + a_latch.viewProj[row * 4 + 2] * p[2] + a_latch.viewProj[row * 4 + 3];
					const float w = clip[3] != 0.0f ? clip[3] : 1.0f;
					minX = std::min(minX, clip[0] / w);
					maxX = std::max(maxX, clip[0] / w);
					minY = std::min(minY, clip[1] / w);
					maxY = std::max(maxY, clip[1] / w);
					maxZ = std::max(maxZ, clip[2] / w);
				}
				const float area = (std::clamp(maxX, -1.0f, 1.0f) - std::clamp(minX, -1.0f, 1.0f)) * (std::clamp(maxY, -1.0f, 1.0f) - std::clamp(minY, -1.0f, 1.0f)) / 4.0f;
				if (area > 0.25f && maxZ < 0.02f)
					covers.push_back({ area, object, maxZ });
			});
			std::sort(covers.begin(), covers.end(), [](const Cover& a, const Cover& b) { return a.area > b.area; });
			if (!covers.empty() || farWorld || staleBuffers) {
				std::string text;
				for (std::size_t i = 0; i < covers.size() && i < 5; ++i) {
					const auto& c = covers[i];
					const auto* geometry = c.object < tablesNow.objectGeometry.size() ? tablesNow.objectGeometry[c.object] : nullptr;
					const auto& r = c.object < records.size() ? records[c.object] : BindlessObject{};
					const auto& b = placements[c.object].bound;
					text += fmt::format("; '{}' (object {}) {:.0f}% of the map, depth at most {:.4f}, bound ({:.0f} {:.0f} {:.0f}) r {:.0f}, the engine's ({:.0f} {:.0f} {:.0f}) r {:.0f}, {}",
						geometry && geometry->name.c_str() ? geometry->name.c_str() : "?", c.object, 100.0f * c.area, c.maxZ, b[0], b[1], b[2], b[3],
						geometry ? geometry->worldBound.center.x : 0.0f, geometry ? geometry->worldBound.center.y : 0.0f, geometry ? geometry->worldBound.center.z : 0.0f,
						geometry ? geometry->worldBound.radius : 0.0f, reached.contains(geometry) ? "the engine's cull reached it" : "the engine's cull did not reach it");
					{
						const auto& t = r.tree;
						const std::uint32_t treeSlot = std::bit_cast<std::uint32_t>(t.windTimers[2]);
						text += fmt::format(", tree params ({} {} {} {}), wind slot {:#x} generation {:#x}", t.treeParams[0], t.treeParams[1], t.treeParams[2], t.treeParams[3],
							treeSlot, std::bit_cast<std::uint32_t>(t.windTimers[3]));
						if (treeSlot < tablesNow.trees.size()) {
							const auto& row = tablesNow.trees[treeSlot];
							text += fmt::format(", tree row: model amplitude {}, amplitude {}, leaf frequency {}, animated {}, generation {:#x}, at ({:.0f} {:.0f} {:.0f})", row.modelAmplitude,
								row.amplitude, row.leafFrequency, row.animated, row.generation, row.position[0], row.position[1], row.position[2]);
						}
					}
				}
				if (s.coversLogged++ < 40)
					logger::warn("[DCLF] cascade cover: view {} (cascade {}) frame {}: {} drawn, {} cover over a quarter of the map at the near plane, {} with the world far from the bound{}, {} pulling from buffers their TriShape no longer holds{}{}",
						a_view.viewId, shadowView->descriptor, a_frame, drawn, covers.size(), farWorld, farFirst.empty() ? "" : " (first " + farFirst + ")", staleBuffers,
						staleFirst.empty() ? "" : " (first " + staleFirst + ")", text);
			}
		}
		if (checks == 20) {
			logger::info("[DCLF] cascade culling parity: {} sun views checked, {} claimed casters the engine's cascade culls reached, {} of them outside the view's viewport (no texels), {} rejected by DCLF's view{}{}",
				checks, registered, offscreen, rejected, rejected ? " <- CASCADE CULL" : " <- OK", first.empty() ? "" : "; first " + first);
			checks = registered = rejected = offscreen = 0;
			first.clear();
		}
	}
}

#endif
