// The first-person view's frame start and callbacks: the latch, its targets, the skeleton's walk and pipelines, its resources and target
// (BeginFrame, the view's work before the coverage), its coverage (Covered), its root (CaptureRoot) and the accumulators its members
// register into (Accumulators), and the skeleton's cull hidden source (CullHiddenSkeleton). Render thread.

#include "Features/DrawcallLimitFix/Features/FirstPerson/FirstPersonView.h"

#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Views.h"
#include "Features/DrawcallLimitFix/Core/Gpu/FrameValues.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelines.h"
#include "Features/DrawcallLimitFix/Core/Handoff/PassCapture.h"
#include "Features/DrawcallLimitFix/Core/Render/IndirectDraws.h"
#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/SceneSet.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Tables.h"
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"
#include "Features/DrawcallLimitFix/Diagnostics/Draws/Events.h"
#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"
#include "Features/DrawcallLimitFix/Core/Engine/Capture/SceneTracker.h"

namespace DCLF::Features::FirstPerson
{
	/*
	 * The first-person view (dclf-architecture.md, "First person as a view bit"; skyrim-engine-notes.md, "The first-person
	 * view"). The first-person skeleton's geometry is tracked under its root (Coordinator::CaptureCategories, from CaptureRoot) and its
	 * members (kSetFirstPerson) are inputs of the one main list with the view bit kViewFirstPerson alone (MainMaskOf). On a frame the engine
	 * draws first person (the latch: Main::Draw's cVar23), two epochs draw them with the first-person camera as the engine set it up
	 * (FUN_1414cb020: the world camera's rotation at GetFirstPersonNodePosition, firstPersonFOV plus the animated offset, near
	 * fNear1stPersonDistance, the world's far), in world space: the depth epoch at RenderDepth's first-person draw, the colour epoch at
	 * FUN_1414cb6d0's accumulator render (FirstPersonEpochs.cpp). The engine's shifted space (FUN_1414ce9d0 moves the root to the origin
	 * around those draws) is the same image; DCLF never reads the skeleton while it is shifted (FrameValues takes the members' rows and
	 * palettes at the frame's start).
	 */

	namespace
	{
		/**
		 * @brief The latch: Main::Draw's cVar23 (AE 0x1406444b0, taken at its entry), recomputed here (Main_Draw_Early runs after FUN_1406452c0 and
		 * before 0x1406445a1 hides the first-person root): the root's kHidden clear, Main::Draw's third argument false (FUN_140644360:
		 * UI::IsMenuOpen("Main Menu")), and PlayerCamera +0x28 (the current state) not +0xD0 (the free camera, tfc).
		 */
		bool FirstPersonLatch()
		{
			bool latch = false;
			if (auto* player = RE::PlayerCharacter::GetSingleton())
				if (const auto* root = player->Get3D(true); root && root != player->Get3D(false) && !root->GetFlags().any(RE::NiAVObject::Flag::kHidden)) {
					auto* ui = RE::UI::GetSingleton();
					const auto* camera = RE::PlayerCamera::GetSingleton();
					const bool mainMenu = ui && ui->IsMenuOpen(RE::MainMenu::MENU_NAME);
					const bool freeCamera = camera && camera->IsCurrentState(RE::CameraState::kFree, RE::CameraState::kFree);
					latch = !mainMenu && camera && !freeCamera;
				}
			return latch;
		}

		/**
		 * @brief The colour epoch's targets: the main pass's targets 0 and 1 (kMAIN and the motion vectors, as FUN_1414cb6d0 binds them; the
		 * main pass's capture's formats) and kPOST_ZPREPASS_COPY's depth format, into the view's slot and published. True when they changed.
		 */
		bool CaptureFirstPersonTargets(const TargetFormats& a_formats)
		{
			auto* renderer = globals::game::renderer;
			if (renderer && a_formats.colorCount >= 2) {
				const auto& copy = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];
				if (copy.views[0]) {
					D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
					copy.views[0]->GetDesc(&dsvDesc);
					ForwardTargets targets;
					targets.colour = a_formats.colors[0];
					targets.colour2 = a_formats.colors[1];
					targets.depth = dsvDesc.Format;
					return IndirectDraws::Get().SetViewTargets(kView, targets);
				}
			}
			return false;
		}

		/** @brief The pipeline slots the skeleton's geometry draws with, found again (see BeginFrame), and its forward members. */
		void WalkFirstPersonSkeleton(FirstPersonFrame& a_f, const SceneTables& a_tables, std::uint32_t a_frame)
		{
			auto& f = a_f;
			const auto& tables = a_tables;
			f.slotsFrame = a_frame;
			f.slotsWanted.assign(tables.pipelines.size(), 0);
			std::vector<RE::BSGeometry*> geometry;
			if (auto* player = RE::PlayerCharacter::GetSingleton())
				if (auto* root = player->Get3D(true); root && root != player->Get3D(false))
					SceneTracker::CollectGeometry(root, geometry);
			const ankerl::unordered_dense::set<const RE::BSGeometry*> skeleton(geometry.begin(), geometry.end());
			const auto claims = Scene::FrameView::Get().FrameClaims();
			f.forwardMembersNow = 0;
			for (std::size_t o = 0; o < tables.objects.size() && !skeleton.empty(); ++o)
				if (o < tables.objectGeometry.size() && tables.objectGeometry[o] && skeleton.contains(tables.objectGeometry[o]) &&
					!(tables.objects[o].flags & kObjectFree) && tables.objects[o].pipelineIndex < f.slotsWanted.size()) {
					f.slotsWanted[tables.objects[o].pipelineIndex] = 1;
					// The forward part: a forward member of the frame's claims (the report's count).
					if ((tables.objects[o].flags & kObjectForward) && claims && (claims->PhasesOf(tables.objectGeometry[o]) & kSetFirstPerson))
						++f.forwardMembersNow;
				}
			if (claims)
				Diagnostics::Notify(Diagnostics::FirstPersonSkeletonWalked{ tables, skeleton, geometry.size(), *claims });
		}
	}

	void BeginFrame(const Api::ViewPointContext& a_context)
	{
		ZoneScopedN("CS.DCLF.BeginFirstPersonFrame");
		auto& draws = IndirectDraws::Get();
		const auto* formats = draws.MainTargetFormats();
		if (!formats)
			return;
		auto& f = FrameState();
		const std::uint32_t frame = a_context.frame;
		const bool on = Extensions::ViewOn(ActiveToggles(), kView) && !a_context.failed;
		f.latch = false;
		f.latchFrame = frame;
		if (!on)
			return;
		++f.frames;
		f.latch = FirstPersonLatch();
		f.latched += f.latch ? 1 : 0;
		const auto& state = *draws.ViewState(kView);
		if (CaptureFirstPersonTargets(*formats)) {
			draws.PostViewInputs();
			logger::info("[DCLF] first-person targets: colour {}, motion vectors {}, depth {}", static_cast<int>(state.targets.colour), static_cast<int>(state.targets.colour2),
				static_cast<int>(state.targets.depth));
		}
		if (state.targets.colour == DXGI_FORMAT_UNKNOWN)
			return;
		// The slots' first-person forward pipelines, asked of the pipeline lane (each key once) and read from the frame's catalog.
		DrawPipelines::Get().SetViewForwardTargets(Api::EngineView::FirstPerson, state.targets);
		const auto* catalog = a_context.catalog;
		const auto& tables = *a_context.tables;
		// The pipeline slots the skeleton's geometry draws with: the installed tables' slots of the geometry under the first-person root
		// (the render thread's own walk of it, before Main::Draw touches it), found again every 16 frames or when the tables' slots grow.
		// Their pipelines are asked for before any is a member (the commit waits for them: IndirectDraws::ViewSlotReady).
		if (frame - f.slotsFrame >= 16 || f.slotsWanted.size() != tables.pipelines.size())
			WalkFirstPersonSkeleton(f, tables, frame);
		// The forward part: the main set's forward pipelines draw into this pass when its depth is the main pass's format (the colour
		// attachments are the main pass's own); the forward members the installed tables hold.
		const bool forwardCompatible = formats->depth != DXGI_FORMAT_UNKNOWN && formats->depth == state.targets.depth;
		if (draws.ExchangeViewForwardCompatible(kView, forwardCompatible) != forwardCompatible)
			logger::info("[DCLF] first-person forward part: the main depth format {} and kPOST_ZPREPASS_COPY's {}: {}", static_cast<int>(formats->depth),
				static_cast<int>(state.targets.depth), forwardCompatible ? "drawn by DCLF" : "the engine's");
		f.forwardMembers += f.forwardMembersNow;
		const auto pipelines = draws.ResolveViewPipelines(kView, tables, catalog, f.slotsWanted);
		f.slots = pipelines.slots;
		f.pipelinesReady = pipelines.ready;
		if (draws.SetUpView(kView))
			draws.ImportViewTarget(kView);
	}

	bool Covered(const Api::ViewCoverage& a_coverage)
	{
		// The first-person members are withheld from the engine (PassCapture) and drawn by the first-person epochs on a frame the engine
		// draws first person (the latch), whose claims stand, both epochs covered (its depth epoch's passes in the depth epoch's recordings,
		// its colour epoch's own) and the depth copy imported into the
		// graph as built. Its members need no frame before's inputs: both epochs draw this frame's scene list with this frame's camera.
		const auto& f = FrameState();
		const auto& c = a_coverage;
		return c.main && c.on && f.latch && f.latchFrame == c.frame && c.epochsCovered && c.targetImported && !c.rebuildPending && c.mainResourcesCurrent &&
		       !c.rebuildRequested;
	}

	void CaptureRoot(Api::ViewRootCapture& a_capture)
	{
		// The player's first-person skeleton (PlayerCharacter +0x8F8, Load3D: no parent; Main::unk_FPCulling and RenderDepth cull it alone),
		// while it is not the third-person one; found from both roots, which the category signatures fold in.
		if (auto* player = RE::PlayerCharacter::GetSingleton()) {
			auto* firstPerson = player->Get3D(true);
			const auto* thirdPerson = player->Get3D(false);
			a_capture.inputs = { firstPerson, thirdPerson };
			a_capture.inputCount = 2;
			if (firstPerson && firstPerson != thirdPerson)
				a_capture.root = firstPerson->AsNode();
		}
	}

	void CullHiddenSkeleton(Api::CullHiddenNodes& a_nodes)
	{
		// The first-person skeleton: Main::Draw (AE 0x1406444b0) hides it right after the call the frame's start runs from,
		// keeps it hidden through the main camera's cull and the sun's shadow casters, and shows it only to draw the
		// first-person view with its own camera. For every view the scene serves it is hidden (the source's bit). The third-person
		// skeleton is Features/Reflection's source's.
		if (auto* player = RE::PlayerCharacter::GetSingleton()) {
			const RE::NiAVObject* thirdPerson = player->Get3D(false);
			const RE::NiAVObject* firstPerson = player->Get3D(true);
			if (firstPerson && firstPerson != thirdPerson)
				a_nodes.Add(firstPerson);
		}
	}

	void Accumulators(Api::ViewAccumulators& a_accumulators)
	{
		// The first-person accumulator (Main::unk_FPCulling culls the first-person root alone into it; FUN_1414cb6d0 renders it). Its
		// registrations are the first-person skeleton's alone.
		static REL::Relocation<RE::BSGraphics::BSShaderAccumulator**> firstPersonAccumulator{ REL::Offset(Engine::Address::kFirstPersonAccumulator) };
		a_accumulators.accumulators[0] = *firstPersonAccumulator;
		a_accumulators.count = 1;
	}
}
