// The first-person view drawn by DCLF: its capture, targets and pipelines, the first-person phase's readiness, and its two
// epochs' commits (their passes are ViewEpochRecord.cpp's). Render thread, but for FirstPersonDrawable and FirstPersonSlotReady (the scene lane's commit).

#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Diagnostics/Draws/Events.h"

#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"
#include "Features/DrawcallLimitFix/Engine/Capture/SceneTracker.h"
#include "Features/DrawcallLimitFix/Scene/Ports/LightingDescriptors.h"

namespace DCLF
{
	/*
	 * The first-person view (drawcall-limit-fix.md, "First person as a view bit"; skyrim-engine-notes.md, "The first-person
	 * view"). The first-person skeleton's geometry is tracked under its root (Coordinator::CaptureCategories) and its members
	 * (kSetFirstPerson) are inputs of the one main list with the view bit kViewFirstPerson alone (MainMaskOf). On a frame the engine draws
	 * first person (the latch: Main::Draw's cVar23), two epochs draw them with the first-person camera as the engine set it up
	 * (FUN_1414cb020: the world camera's rotation at GetFirstPersonNodePosition, firstPersonFOV plus the animated offset, near
	 * fNear1stPersonDistance, the world's far), in world space: the depth epoch at RenderDepth's first-person draw, the colour epoch at
	 * FUN_1414cb6d0's accumulator render. The engine's shifted space (FUN_1414ce9d0 moves the root to the origin around those draws) is the
	 * same image; DCLF never reads the skeleton while it is shifted (FrameValues takes the members' rows and palettes at the frame's start).
	 */

	namespace
	{
		// The engine's first-person globals (AE 1.6.1170; skyrim-engine-notes.md, "The first-person view").
		// DrawWorld (FUN_1414cafd0), filled by FUN_1406452c0 (Main::Draw's first call, before BeginSceneFrame): +0x00 the camera position
		// (PlayerCamera::GetFirstPersonNodePosition), in world space.
		// The first-person camera (an NiCamera*), set up by FUN_1414cb020 (FUN_140d2cda0: FOV firstPersonFOV + 0x14338c628, near the float at
		// 0x14202a928, far the world scene graph's GetFarDistance, the screen's aspect; the world camera's rotation), translated into the
		// root's space (its position minus the root's).
		constexpr std::uintptr_t kFirstPersonCamera = 0x338c820;
		// The viewport depth range both first-person passes set (FUN_140e57cb0(state, 0, max)): f1stPersonFarDepthRange, or
		// f1stPersonFarDepthRangeControlDriven with RenderDepth's and FUN_1414cb6d0's second argument (the movement controller's vfunc 0x78).
		constexpr std::uintptr_t kFarDepthRange = 0x202b2d0, kFarDepthRangeControlDriven = 0x202b2e8;

		using FirstPersonState = ImplViews::FirstPersonState;

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
		 * main pass's capture's formats) and kPOST_ZPREPASS_COPY's depth format, into a_f and published. True when they changed.
		 */
		bool CaptureFirstPersonTargets(const TargetFormats& a_formats, FirstPersonState& a_f)
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
					if (!(targets == a_f.targets)) {
						a_f.targets = targets;
						a_f.PublishTargets();
						return true;
					}
				}
			}
			return false;
		}

		/** @brief The pipeline slots the skeleton's geometry draws with, found again (see BeginFirstPersonFrame), and its forward members. */
		void WalkFirstPersonSkeleton(FirstPersonState& a_f, const SceneTables& a_tables, std::uint32_t a_frame)
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

		/** @brief The wanted slots' first-person forward pipelines, asked of the pipeline lane (each key once) and read from a_catalog. */
		void ResolveFirstPersonPipelines(FirstPersonState& a_f, const SceneTables& a_tables, const PipelineCatalog* a_catalog, DrawPipelines& a_drawPipelines)
		{
			auto& f = a_f;
			const auto& tables = a_tables;
			std::uint32_t slots = 0, ready = 0;
			f.slotPipelines.assign(tables.pipelines.size(), rhi::PipelineHandle{});
			for (std::size_t p = 0; p < tables.pipelines.size(); ++p) {
				ForwardPipelineKey key;
				if (!f.slotsWanted[p] || !ViewForwardKey(ViewKind::FirstPerson, tables, p, key))
					continue;
				++slots;
				a_drawPipelines.RequestForward(key);
				if (const auto pipeline = ForwardPipelineOf(a_catalog, key, f.targets); pipeline.valid()) {
					f.slotPipelines[p] = pipeline;
					++ready;
				}
			}
			f.slots = slots;
			f.pipelinesReady = ready;
		}
	}

	bool IndirectDraws::FirstPersonDrawable() const
	{
		// A capability (Coordinator::SetCapability, the scene lane's commit): the toggle, and the first-person targets published (the colour
		// epoch's formats, known once the main pass's targets were captured and kPOST_ZPREPASS_COPY exists). Each member then waits for its
		// own pipelines (FirstPersonSlotReady), and the engine draws it until the frame's epochs are covered (PassCapture).
		return impl && ActiveToggles().firstPerson && !failed && impl->firstPerson.LaneTargets().colour != DXGI_FORMAT_UNKNOWN;
	}

	bool IndirectDraws::FirstPersonCovered() const
	{
		return impl && impl->firstPerson.covered;
	}

	bool IndirectDraws::FirstPersonSlotReady(const void* a_tables, const void* a_lookups, const PipelineCatalog* a_catalog, std::uint32_t a_slot, std::uint32_t* a_why) const
	{
		// a_why (the set parity's observer): 1 no catalog or targets, 2 a forward key the forward part cannot draw, 3 no first-person key,
		// 4 its pipeline not in a_catalog, 5 no Z-prepass group pipeline.
		auto fail = [&](std::uint32_t a_cause) {
			if (a_why)
				*a_why = a_cause;
			return false;
		};
		// The slot's first-person forward pipeline as a_catalog has it for the published first-person targets, and its Z-prepass group's
		// depth pipeline in that catalog's main set (FirstPersonPipelinesOf's and PlanFirstPersonBuckets' rules): the scene lane's commit's.
		const auto& tables = *static_cast<const SceneTables*>(a_tables);
		const ForwardTargets targets = impl->firstPerson.LaneTargets();
		if (!a_catalog || targets.colour == DXGI_FORMAT_UNKNOWN || a_slot >= tables.objects.size())
			return fail(1);
		const std::uint32_t p = tables.objects[a_slot].pipelineIndex;
		// The forward part: a forward key's slot draws with the main set's colour pipeline (main-ready: the commit's MainReady) through the
		// colour signature, into the main pass's attachments with kPOST_ZPREPASS_COPY's depth, which must be the main pass's format.
		if (p < tables.pipelines.size() && RasterForward(tables.pipelines[p].rasterFlags))
			return (!RasterRefraction(tables.pipelines[p].rasterFlags) && impl->firstPerson.forwardCompatible.load(std::memory_order_acquire) &&
			           GetIndirectState(*a_catalog, a_catalog->targetsGeneration).valid) ||
			       fail(2);
		ForwardPipelineKey key;
		if (p >= tables.pipelines.size() || !ViewForwardKey(ViewKind::FirstPerson, tables, p, key))
			return fail(3);
		if (!ForwardPipelineOf(a_catalog, key, targets).valid())
			return fail(4);
		const auto indirect = GetIndirectState(*a_catalog, a_catalog->targetsGeneration);
		const std::uint32_t group = Draws::ZGroupOf(static_cast<const Lookups*>(a_lookups), indirect, p);
		return (indirect.valid && group < indirect.zPipelines.size() && indirect.zPipelines[group].valid()) || fail(5);
	}

	void IndirectDraws::BeginFirstPersonFrame()
	{
		ZoneScopedN("CS.DCLF.BeginFirstPersonFrame");
		if (!impl)
			return;
		auto& f = impl->firstPerson;
		const std::uint32_t frame = Scene::FrameView::Get().GetFrame();
		const bool on = ActiveToggles().firstPerson && !failed;
		f.latch = false;
		f.latchFrame = frame;
		if (!on)
			return;
		++f.frames;
		f.latch = FirstPersonLatch();
		f.latched += f.latch ? 1 : 0;
		const auto& formats = impl->formats;
		if (CaptureFirstPersonTargets(formats, f)) {
			impl->PostRevisionInputs();
			logger::info("[DCLF] first-person targets: colour {}, motion vectors {}, depth {}", static_cast<int>(f.targets.colour), static_cast<int>(f.targets.colour2),
				static_cast<int>(f.targets.depth));
		}
		if (f.targets.colour == DXGI_FORMAT_UNKNOWN)
			return;
		// The slots' first-person forward pipelines, asked of the pipeline lane (each key once) and read from the frame's catalog.
		auto& drawPipelines = DrawPipelines::Get();
		drawPipelines.SetFirstPersonTargets(f.targets);
		const auto* catalog = FrameCatalog();
		const auto& tables = Scene::FrameView::Get().GetTables();
		// The pipeline slots the skeleton's geometry draws with: the installed tables' slots of the geometry under the first-person root
		// (the render thread's own walk of it, before Main::Draw touches it), found again every 16 frames or when the tables' slots grow.
		// Their pipelines are asked for before any is a member (the commit waits for them: FirstPersonSlotReady).
		if (frame - f.slotsFrame >= 16 || f.slotsWanted.size() != tables.pipelines.size())
			WalkFirstPersonSkeleton(f, tables, frame);
		// The forward part: the main set's forward pipelines draw into this pass when its depth is the main pass's format (the colour
		// attachments are the main pass's own); the forward members the installed tables hold.
		const bool forwardCompatible = formats.depth != DXGI_FORMAT_UNKNOWN && formats.depth == f.targets.depth;
		if (f.forwardCompatible.exchange(forwardCompatible, std::memory_order_acq_rel) != forwardCompatible)
			logger::info("[DCLF] first-person forward part: the main depth format {} and kPOST_ZPREPASS_COPY's {}: {}", static_cast<int>(formats.depth),
				static_cast<int>(f.targets.depth), forwardCompatible ? "drawn by DCLF" : "the engine's");
		f.forwardMembers += f.forwardMembersNow;
		ResolveFirstPersonPipelines(f, tables, catalog, drawPipelines);
		if (impl->SetupView(ViewKind::FirstPerson))
			impl->ImportFirstPersonDepth();
	}

	void IndirectDraws::ExecuteFirstPersonDepth(bool a_controlDriven)
	{
		ZoneScopedN("CS.DCLF.ExecuteFirstPersonDepth");
		if (!impl)
			return;
		auto& f = impl->firstPerson;
		const std::uint32_t frameNumber = Scene::FrameView::Get().GetFrame();
		auto skip = [&](std::uint32_t a_cause) { ++f.skipped[a_cause]; };
		// The engine's depth range for the frame (a revision input: the shapes' viewport).
		if (const float range = *REL::Relocation<float*>{ REL::Offset(a_controlDriven ? kFarDepthRangeControlDriven : kFarDepthRange) }; range > 0.0f && range != f.depthRange) {
			f.depthRange = range;
			impl->PostRevisionInputs();
		}
		if (!ActiveToggles().firstPerson || !f.latch || f.latchFrame != frameNumber)
			return skip(0);
		const auto set = Scene::FrameView::Get().FrameClaims();
		if (!set || !(set->drawn & kSetFirstPerson))
			return skip(0);
		if (!f.covered)
			return skip(1);
		// The first-person camera as FUN_1414cb020 set it up: its frustum and rotation, and its position in world space (DrawWorld +0x00:
		// its translation is in the root's space here).
		auto* camera = *REL::Relocation<RE::NiCamera**>{ REL::Offset(kFirstPersonCamera) };
		if (!camera)
			return skip(2);
		const auto* eyeWorld = REL::Relocation<const float*>{ REL::Offset(Engine::Address::kDrawWorldEye) }.get();
		SunViews::Cascade view;
		SunViews::CameraMatrices(*camera, view);
		const RE::NiPoint3 eye{ eyeWorld[0], eyeWorld[1], eyeWorld[2] };
		// VS_PerFrame (b12) for the camera, as the engine's SetCameraData builds it for the first-person draws (the accumulator renders'
		// flags 3 and 0x43: jittered), over the renderer's last block; the motion vectors' previous camera last frame's, its own on the first.
		const PreviousCamera current{ view.viewProj, eye };
		f.perFrame.fill(std::byte{ 0 });
		PerFrameBlock(view, FrameBlockNow(), true, f.havePrevious ? &f.previous : &current, f.perFrame.data());
		f.viewProj = view.viewProj;
		f.eye = eye;
		f.previous = current;
		f.havePrevious = true;

		// What the epoch draws from: the frame's scene list (the installed payload) with this frame's depth commit's index pool, inputs, object
		// records and geometry rows - the frame's publication whole.
		const auto main = impl->resources;
		const auto& depthCommit = main ? main->committed[kDepthShape] : Resources::Committed{};
		if (!main || !impl->scene || depthCommit.frame != frameNumber || depthCommit.adoptedSerial != impl->Adopted().Serial())
			return skip(3);
		const auto& draws = impl->installedDraws;
		const MainPayload* listed = impl->InstalledPayload() && draws ? draws->payloads[kAsyncZPrepass].get() : nullptr;
		if (!listed)
			return skip(3);
		const auto listInputs = static_cast<std::uint32_t>(listed->resident.Count() + listed->inputList.size());
		if (!f.resources || !f.resources->target)
			return skip(4);
		const auto revision = impl->RevisionOf(Epoch::FirstPersonDepth);
		if (!revision)
			return skip(1);
		const auto shape = revision.shape->Value<ViewFrame>();
		const auto& payloads = draws->payloads;
		const std::array<const MainPayload*, 2> listedPayloads{ payloads[0].get(), payloads[1].get() };
		const auto owners = impl->EpochOwners(listedPayloads);
		if (!owners)
			return skip(4);
		const std::array<ViewFace, 1> faces{ ViewFace{ true, std::span<const std::byte>(f.perFrame.data(), f.perFrame.size()), f.viewProj, f.eye } };
		const bool ok = RenderGraphRuntime::Get().ExecuteEpoch(RenderGraphRuntime::Segment::FirstPersonDepth, [&](org::RenderGraph&) {
			impl->CommitView(*shape, faces, listInputs, nullptr);
			impl->SubmitRevisionRecording(Epoch::FirstPersonDepth, *revision.recordings, 0);
		}, owners);
		if (!ok) {
			logger::error("[DCLF] the first-person depth epoch failed");
			return skip(5);
		}
		++f.depthEpochs;
		f.depthFrame = frameNumber;
	}

	void IndirectDraws::ExecuteFirstPersonColour()
	{
		ZoneScopedN("CS.DCLF.ExecuteFirstPersonColour");
		if (!impl)
			return;
		auto& f = impl->firstPerson;
		const std::uint32_t frameNumber = Scene::FrameView::Get().GetFrame();
		auto skip = [&](std::uint32_t a_cause) { ++f.skipped[a_cause]; };
		if (!ActiveToggles().firstPerson || !f.latch || f.latchFrame != frameNumber || !f.covered)
			return;
		// What it draws is what this frame's depth epoch culled (its sequences and bucket counts), with this frame's colour commit's frame
		// constants (the frame record, the lighting at PS b13).
		if (f.depthFrame != frameNumber)
			return skip(6);
		if (impl->committedFrame[kAsyncColour] != frameNumber || Scene::FrameView::Get().SetWithdrawn())
			return skip(3);
		const auto revision = impl->RevisionOf(Epoch::FirstPersonColour);
		if (!revision)
			return skip(1);
		const std::array<const MainPayload*, 1> colour{ impl->committedPayload[kAsyncColour] };
		const auto owners = impl->EpochOwners(colour);
		if (!owners)
			return skip(4);
		const bool ok = RenderGraphRuntime::Get().ExecuteEpoch(RenderGraphRuntime::Segment::FirstPerson, [&](org::RenderGraph&) {
			impl->SubmitRevisionRecording(Epoch::FirstPersonColour, *revision.recordings, 0);
		}, owners);
		if (!ok) {
			logger::error("[DCLF] the first-person colour epoch failed");
			return skip(5);
		}
		++f.colourEpochs;
	}

}
