// The shadow epochs' checks (render thread): DCLF's views against the engine's (the viewport, eye, matrices and caster volume at each
// view's draw, the predicted target), the sun entry test on the GPU against the CPU's, and the index pool's ranges against the tables.
// (The occlusion maps' views and the sun's full-frustum processes are their features': Features/OcclusionMaps/Diagnostics,
// Features/Shadows/Sun/Diagnostics.)

#include "DrawsState.h"
#include <ankerl/unordered_dense.h>
#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Events.h"
#include "Observers.h"
#include "Features/DrawcallLimitFix/Diagnostics/Engine/ShadowViewTally.h"
// ShadowCapture.cpp's: the engine's rasterizer state id, and where and with which block the engine has just drawn a view.
#include "Features/DrawcallLimitFix/Core/Render/Shadow/ShadowEpochs.h"

namespace DCLF::Diagnostics
{
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
			count, held, p.indicesHeld, NamedUnits(DrawsView::Get().Adopted(), p.indices), stale, unheld, p.waiting.size(), stale || unheld > p.waiting.size() ? " <- INDEX POOL" : " <- OK",
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
		const auto port = ShadowCameras::Viewport(a_own.cascade->port, desc.Width, desc.Height);
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
		if (!ShadowCameras::Close(eye.x, a_view.eye.x) || !ShadowCameras::Close(eye.y, a_view.eye.y) || !ShadowCameras::Close(eye.z, a_view.eye.z)) {
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
		std::array<std::byte, ShadowCameras::kBlockBytes> block{};
		ShadowCameras::Block(a_cascade, block.data());
		const auto* mine = reinterpret_cast<const float*>(block.data());
		const auto* theirs = reinterpret_cast<const float*>(a_view.perFrame.data());
		float largest = 0.0f;
		std::int32_t first = -1;
		for (std::uint32_t i = 0; i < 48; ++i) {
			if (!ShadowCameras::Close(mine[i], theirs[i]) && first < 0)
				first = static_cast<std::int32_t>(i);
			largest = std::max(largest, std::abs(mine[i] - theirs[i]));
		}
		if (first >= 0) {
			++p.viewProj;
			const std::uint32_t row = static_cast<std::uint32_t>(first) / 4 * 4;
			p.Note("matrices", largest, fmt::format("descriptor {}: c{} ({} {} {} {}), the engine's ({} {} {} {})", a_own.descriptor, row / 4, mine[row], mine[row + 1],
				mine[row + 2], mine[row + 3], theirs[row], theirs[row + 1], theirs[row + 2], theirs[row + 3]));
		}
		ShadowCameras::Planes captured;
		for (std::uint32_t q = 0; q < 6; ++q)
			for (std::uint32_t i = 0; i < 4; ++i)
				captured.plane[q][i] = a_view.cullPlanes[q][i];
		captured.mask = a_view.cullPlaneMask;
		if (!ShadowCameras::SamePlanes(a_cascade.caster, captured)) {
			++p.casters;
			p.Note("caster volume", 1.0f, fmt::format("descriptor {}: mask {:#x}, the engine's {:#x}", a_own.descriptor, a_cascade.caster.mask, captured.mask));
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
			const float* row = placements[input.objectIndex].sunEntry;
			const bool differ = cpu != OutsideSunEntryProcesses(payload.inputs.sunEntryProcesses, row);
			++a_latched.stats.sunEntryChecks;
			a_latched.stats.sunEntryMismatches += differ ? 1 : 0;
			// The first ones named: the entry root, its reference, and the placement row's sphere against the node's bound now.
			static std::uint32_t named = 0;
			if (differ && named < 40) {
				++named;
				const auto* node = input.objectIndex < tablesNow.listedRoot.size() ? tablesNow.listedRoot[input.objectIndex] : nullptr;
				if (!node) {
					logger::info("[DCLF] sun entry differs, frame {}: object {} has no entry root (the row's entry ({:.1f} {:.1f} {:.1f}) r {:.1f} is outside)",
						a_latched.frame, input.objectIndex, row[0], row[1], row[2], row[3]);
					continue;
				}
				// Each root once (its other objects and later frames repeat it).
				static ankerl::unordered_dense::set<const void*> seen;
				if (!seen.insert(node).second) {
					--named;
					continue;
				}
				const RE::TESObjectREFR* owner = nullptr;
				for (const RE::NiAVObject* at = node; at && !owner; at = at->parent)
					owner = at->GetUserData();
				const auto* base = owner ? owner->GetBaseObject() : nullptr;
				const auto& now = node->worldBound;
				logger::info("[DCLF] sun entry differs, frame {}: object {} entry root '{}' of {} {:08X} '{}': the row's entry ({:.1f} {:.1f} {:.1f}) r {:.1f}, the node's "
							 "now ({:.1f} {:.1f} {:.1f}) r {:.1f} (moved {:.1f}); the CPU says {}",
					a_latched.frame, input.objectIndex, node->name.c_str() ? node->name.c_str() : "", base ? RE::FormTypeToString(base->GetFormType()) : "no ref",
					owner ? owner->GetFormID() : 0u, base && base->GetName() ? base->GetName() : "", row[0], row[1], row[2], row[3], now.center.x, now.center.y,
					now.center.z, now.radius, std::sqrt((now.center.x - row[0]) * (now.center.x - row[0]) + (now.center.y - row[1]) * (now.center.y - row[1]) +
					                                        (now.center.z - row[2]) * (now.center.z - row[2])),
					cpu ? "outside" : "inside");
				// The object's own bound as the row has it against now, and the root's children now (the union its bound is).
				const auto* geometry = input.objectIndex < tablesNow.objectGeometry.size() ? tablesNow.objectGeometry[input.objectIndex] : nullptr;
				const float* sampled = placements[input.objectIndex].bound;
				std::string children;
				if (auto* asNode = const_cast<RE::NiAVObject*>(node)->AsNode())
					for (const auto& child : asNode->GetChildren())
						if (child && children.size() < 600)
							children += fmt::format(" '{}' {} ({:.1f} {:.1f} {:.1f}) r {:.1f}{};", child->name.c_str() ? child->name.c_str() : "", child->GetRTTI() ? child->GetRTTI()->name : "?",
								child->worldBound.center.x, child->worldBound.center.y, child->worldBound.center.z, child->worldBound.radius,
								child->GetFlags().any(RE::NiAVObject::Flag::kHidden) ? " hidden" : "");
				logger::info("[DCLF] sun entry differs:   its geometry '{}': the row's bound ({:.1f} {:.1f} {:.1f}) r {:.1f}, now ({:.1f} {:.1f} {:.1f}) r {:.1f}; the root's "
							 "children now:{}",
					geometry && geometry->name.c_str() ? geometry->name.c_str() : "", sampled[0], sampled[1], sampled[2], sampled[3], geometry ? geometry->worldBound.center.x : 0.0f,
					geometry ? geometry->worldBound.center.y : 0.0f, geometry ? geometry->worldBound.center.z : 0.0f, geometry ? geometry->worldBound.radius : 0.0f,
					children);
			}
		}
	}
}
