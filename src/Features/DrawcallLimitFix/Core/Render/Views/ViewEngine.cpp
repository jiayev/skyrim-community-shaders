// The view engine (IndirectDraws.h, "The view engine"): Core's calls for the features that take an engine view - the view slot's state,
// targets, depth range, pipelines, resources and target import at the frame's start, its epochs' inputs and submission, its coverage,
// and its phase's capability and slot readiness. Each is what Core did for the view itself, over the slot the view names. Render thread,
// but for ViewDrawable and ViewSlotReady (the scene lane's commit) and ViewCovered. (A trees view's draw families per face: ViewFaces.cpp; the
// roots its faces draw: ViewRoots.cpp.)

#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/FrameRecords.h"

#include "Features/DrawcallLimitFix/Core/Extensions/Registry.h"
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"

namespace DCLF
{
	const ViewEpochState* IndirectDraws::ViewState(Api::EngineView a_view) const
	{
		return impl ? &impl->View(a_view) : nullptr;
	}

	const TargetFormats* IndirectDraws::MainTargetFormats() const
	{
		return impl ? &impl->formats : nullptr;
	}

	bool IndirectDraws::SetViewTargets(Api::EngineView a_view, const ForwardTargets& a_targets)
	{
		auto& state = impl->View(a_view);
		if (a_targets == state.targets)
			return false;
		state.targets = a_targets;
		state.PublishTargets();
		return true;
	}

	void IndirectDraws::PostViewInputs()
	{
		impl->PostRevisionInputs();
	}

	void IndirectDraws::SetViewDepthRange(Api::EngineView a_view, float a_range)
	{
		if (a_range > 0.0f && a_range != ViewDepthRange(a_view)) {
			impl->View(a_view).depthRange = a_range;
			impl->PostRevisionInputs();
		}
	}

	float IndirectDraws::ViewDepthRange(Api::EngineView a_view) const
	{
		return impl ? impl->DepthRangeOf(a_view) : Extensions::ViewEntryOf(a_view).shape.depthRange;
	}

	bool IndirectDraws::ExchangeViewForwardCompatible(Api::EngineView a_view, bool a_compatible)
	{
		return impl->View(a_view).forwardCompatible.exchange(a_compatible, std::memory_order_acq_rel);
	}

	IndirectDraws::ViewPipelineCounts IndirectDraws::ResolveViewPipelines(Api::EngineView a_view, const SceneTables& a_tables, const PipelineCatalog* a_catalog,
		std::span<const std::uint8_t> a_wanted, bool a_programs)
	{
		auto& state = impl->View(a_view);
		auto& drawPipelines = DrawPipelines::Get();
		ViewPipelineCounts counts;
		state.slotPipelines.assign(a_tables.pipelines.size(), rhi::PipelineHandle{});
		for (std::size_t p = 0; p < a_tables.pipelines.size(); ++p) {
			ForwardPipelineKey key;
			if ((!a_wanted.empty() && !a_wanted[p]) || !ViewForwardKey(a_view, a_tables, p, key))
				continue;
			++counts.slots;
			drawPipelines.RequestForward(key);
			// With a_programs, the programs built and whether a pipeline missing is still to come (its entry not failed): the frame's waits.
			const auto* entry = a_programs && a_catalog ? a_catalog->FindForward(key) : nullptr;
			counts.programs += entry && entry->program ? 1u : 0u;
			const auto pipeline = ForwardPipelineOf(a_catalog, key, state.targets);
			if (!pipeline.valid()) {
				counts.waited |= a_programs && !(entry && entry->failed);
				continue;
			}
			state.slotPipelines[p] = pipeline;
			++counts.ready;
		}
		return counts;
	}

	void IndirectDraws::DropViewPipelines(Api::EngineView a_view)
	{
		impl->View(a_view).slotPipelines.clear();
	}

	bool IndirectDraws::SetUpView(Api::EngineView a_view)
	{
		return impl && impl->SetupView(a_view);
	}

	bool IndirectDraws::ImportViewTarget(Api::EngineView a_view, ID3D11Texture2D* a_texture)
	{
		if (!impl)
			return false;
		// A DepthTarget view's: kPOST_ZPREPASS_COPY. A ColourWithOwnDepth view's: the engine's colour target with DCLF's own depth (the
		// reflection's cube).
		if (Extensions::ViewEntryOf(a_view).targetImport == Api::ViewTargetImport::DepthTarget)
			return impl->ImportViewDepthTarget(a_view);
		return impl->ImportViewColourTarget(a_view, a_texture);
	}

	IndirectDraws::ViewInputsTaken IndirectDraws::TakeViewInputs(Api::EngineView a_view, ViewInputsFrom a_from, ViewListed& a_listed)
	{
		const std::uint32_t frameNumber = Scene::FrameView::Get().GetFrame();
		a_listed = {};
		if (a_from == ViewInputsFrom::ColourThisFrame) {
			// This frame's colour commit's payload and frame constants (the frame record, the lighting at PS b13).
			if (impl->committedFrame[kAsyncColour] != frameNumber || Scene::FrameView::Get().NotDrawingYet())
				return ViewInputsTaken::Stale;
			a_listed.owners = { impl->committedPayload[kAsyncColour], nullptr };
			a_listed.ownerCount = 1;
			return ViewInputsTaken::Taken;
		}
		// The frame's scene list (the installed payload) with this frame's depth commit's index pool, inputs, object records and geometry
		// rows - the frame's publication whole - of the adopted set.
		const auto main = impl->resources;
		const auto& depthCommit = main ? main->depthCommitted : Resources::Committed{};
		if (!main || !impl->scene || depthCommit.frame != frameNumber || depthCommit.adoptedSerial != impl->Adopted().Serial())
			return ViewInputsTaken::Stale;
		const auto& draws = impl->installedDraws;
		const MainPayload* listed = impl->InstalledPayload() && draws ? draws->payloads[kAsyncZPrepass].get() : nullptr;
		if (!listed)
			return ViewInputsTaken::Stale;
		// The main part of the scene list: its region and the main frame inputs (the shadow frame inputs after them are no main input).
		a_listed.inputs = static_cast<std::uint32_t>(listed->resident.Count() + listed->body->inputList.size());
		// The view's target: a DepthThisFrame view's imported before (its frame start); a DepthThisFrameTargetAfter view's work imports it
		// after taking its inputs.
		const auto& resources = impl->View(a_view).resources;
		if (a_from == ViewInputsFrom::DepthThisFrame && (!resources || !resources->target))
			return ViewInputsTaken::NoTarget;
		a_listed.owners = { draws->payloads[0].get(), draws->payloads[1].get() };
		a_listed.ownerCount = 2;
		return ViewInputsTaken::Taken;
	}

	namespace
	{
		/**
		 * @brief A view's frame textures (t16 and up) for its own frame record, from this frame's sources alone: the character light, the
		 * registered frame textures' views (Api::FrameTexture). Nothing the colour pass captured (that is the frame before's by the time
		 * the faces draw), nor the frame graph buffers (Light Limit Fix's clusters are the main camera's, culled after the faces' epoch):
		 * those read null. The frame buffers (t98) are the view's own copies, which its commit fills and names (CommitView). Each binding is
		 * held by a_owners for the execution.
		 */
		void ResolveViewFrameTextures(ViewEpochState& a_state, std::vector<std::shared_ptr<const void>>& a_owners)
		{
			auto& textures = GpuTextures::Get();
			if (a_state.frameTextureGeneration != textures.Generation()) {
				a_state.frameTextureViews = {};
				a_state.frameTextureBindings = {};
				a_state.frameTextureGeneration = textures.Generation();
			}
			a_state.frameTextures.fill(kInvalidIndex);
			const auto frameTextures = Extensions::RegisteredFrameTextures();
			const auto frameTextureAt = Extensions::FrameTextureIndices();
			for (std::uint32_t t = kPixelTextureSlots; t < kTextureRegisters; ++t) {
				const auto at = frameTextureAt[t];
				auto* const view = t == kCharacterLightRegister                               ? Scene::FrameView::Get().GetFrameCapture().characterLightView :
				                   at != Extensions::kNoFrameTexture && at < frameTextures.size() ? static_cast<ID3D11ShaderResourceView*>(frameTextures[at].view()) :
				                                                                                  nullptr;
				a_state.frameTextures[t] = HoldFrameTexture(a_state.frameTextureViews[t], a_state.frameTextureBindings[t], view, t, view ? 0u : textures.NullIndex(), a_owners);
			}
		}

		/** @brief What a view with frame constants of its own holds for its execution: its payloads' bindings and the adopted snapshot (its frame textures: ResolveViewFrameTextures). */
		std::shared_ptr<std::vector<std::shared_ptr<const void>>> OwnFrameOwners(std::span<const MainPayload* const> a_payloads, const std::shared_ptr<const void>& a_snapshot)
		{
			auto* host = RenderGraphRuntime::Get().Host();
			const auto cleanup = host ? host->ResourceCleanup() : nullptr;
			if (!cleanup)
				return nullptr;
			auto owners = cleanup->Make<std::vector<std::shared_ptr<const void>>>();
			for (const auto* payload : a_payloads)
				if (payload)
					owners->insert(owners->end(), payload->body->bindingOwners.begin(), payload->body->bindingOwners.end());
			if (a_snapshot)
				owners->push_back(a_snapshot);
			return owners;
		}
	}

	IndirectDraws::ViewSubmit IndirectDraws::Impl::CommitHostedView(ViewKind a_view, std::uint32_t a_epoch, const IndirectDraws::ViewListed& a_listed,
		std::span<const ViewFace> a_faces, std::uint32_t* a_drawn, std::array<std::uint64_t, 2>* a_nullTextures)
	{
		using R = SceneRevisions;
		auto& commit = hostedCommit;
		// Only inside its host's commit, before the host's recording is submitted (the depth commit runs the view's work there).
		if (commit.host == kNoEpoch || commit.host != EpochTraitsOf(a_epoch).hostedBy || !commit.owners)
			return ViewSubmit::NoRevision;
		// The selected revision's shape of the epoch, which the host's recordings carry (DecideEpochCoverage covers a hosted epoch with
		// them): trusted, its map, buckets and latch are what the values go into.
		const auto fragment = revisions.active ? revisions.active->Fragment(R::kShapeSlot + a_epoch) : nullptr;
		if (!EpochCovered(a_epoch) || !fragment)
			return ViewSubmit::NoRevision;
		if (a_faces.empty())
			return ViewSubmit::Submitted;  // nothing committed: the host's recording without the epoch's work
		const auto shape = fragment->Value<ViewFrame>();
		// Held until the host's execution retires, with its own owners (its payload's bindings, the adopted snapshot): the other payloads
		// the view draws, its frame textures (a view with frame constants of its own) and its draw families' rows' textures.
		auto& owners = *commit.owners;
		for (std::uint32_t i = 0; i < a_listed.ownerCount; ++i)
			if (const auto* payload = a_listed.owners[i]; payload && payload != commit.payload)
				owners.insert(owners.end(), payload->body->bindingOwners.begin(), payload->body->bindingOwners.end());
		auto& state = View(a_view);
		const bool ownFrame = state.resources && state.resources->frameConstants;
		if (ownFrame)
			ResolveViewFrameTextures(state, owners);
		ViewFamilyRows rows{};
		const bool trees = TraitsOf(a_view).trees;
		if (trees)
			rows = ViewFaceRows(a_view, *shape, owners);
		// Its latch in the host's frame slot (the host's execution's, CurrentFrameSlot), which the host's latched copies and its passes read.
		const std::uint32_t drawn = CommitView(*shape, a_faces, a_listed.inputs, trees ? &rows : nullptr);
		commit.drawn |= 1u << a_epoch;
		if (a_drawn)
			*a_drawn = drawn;
		if (a_nullTextures && ownFrame)
			*a_nullTextures = state.nullTextures;
		return ViewSubmit::Submitted;
	}

	IndirectDraws::ViewSubmit IndirectDraws::SubmitViewEpoch(Api::EngineView a_view, std::uint32_t a_epoch, const ViewListed& a_listed,
		std::span<const ViewFace> a_faces, std::uint32_t* a_drawn, std::array<std::uint64_t, 2>* a_nullTextures)
	{
		const auto& epochs = ViewEpochsOf(a_view);
		const std::uint32_t epoch = epochs.epochs[a_epoch];
		// A hosted epoch (the reflection's, first person's depth: the depth epoch runs their passes) commits into its host's commit, from
		// inside it; the host's recording, submitted after, carries its passes.
		if (EpochTraitsOf(epoch).hostedBy != kNoEpoch)
			return impl->CommitHostedView(a_view, epoch, a_listed, a_faces, a_drawn, a_nullTextures);
		// An epoch of its own is never submitted from inside another's commit (the view work a host's commit runs is its hosted epochs').
		if (impl->hostedCommit.host != kNoEpoch) {
			static bool logged = false;
			if (!std::exchange(logged, true))
				logger::error("[DCLF] the {} epoch was submitted inside the {} epoch's commit, which only its hosted epochs commit into", EpochTraitsOf(epoch).name,
					EpochTraitsOf(impl->hostedCommit.host).name);
			return ViewSubmit::Failed;
		}
		// The selected revision's shape and recording (DecideCoverage covered the view): trusted, its map, buckets and latch are what the
		// values go into.
		const auto revision = impl->RevisionOf(epoch);
		if (!revision)
			return ViewSubmit::NoRevision;
		std::shared_ptr<const ViewFrame> shape;
		if (!a_faces.empty())
			shape = revision.shape->Value<ViewFrame>();
		// A view with frame constants of its own holds its commit's frame textures, not the colour segment's.
		auto& state = impl->View(a_view);
		const bool ownFrame = !a_faces.empty() && state.resources && state.resources->frameConstants;
		const std::span<const MainPayload* const> payloads(a_listed.owners.data(), a_listed.ownerCount);
		const auto owners = ownFrame ? OwnFrameOwners(payloads, impl->adoptedSnapshot) : impl->EpochOwners(payloads);
		if (!owners)
			return ViewSubmit::NoOwners;
		if (ownFrame)
			ResolveViewFrameTextures(state, *owners);
		// A trees view's draw families' rows (ViewFaces.cpp), their textures held with the owners.
		ViewFamilyRows rows{};
		const bool trees = !a_faces.empty() && TraitsOf(a_view).trees;
		if (trees)
			rows = impl->ViewFaceRows(a_view, *shape, *owners);
		std::uint32_t drawn = 0;
		const bool ok = RenderGraphRuntime::Get().ExecuteEpoch(epochs.segments[a_epoch], [&](org::RenderGraph&) {
			if (!a_faces.empty())
				drawn = impl->CommitView(*shape, a_faces, a_listed.inputs, trees ? &rows : nullptr);
			impl->SubmitRevisionRecording(epoch, *revision.recordings, 0);
		}, owners);
		if (!ok)
			return ViewSubmit::Failed;
		if (a_drawn)
			*a_drawn = drawn;
		if (a_nullTextures && ownFrame)
			*a_nullTextures = state.nullTextures;
		return ViewSubmit::Submitted;
	}

	bool IndirectDraws::ViewCovered(Api::EngineView a_view) const
	{
		return impl && impl->ViewCovered(a_view);
	}

	bool IndirectDraws::ViewDrawable(Api::EngineView a_view, const ToggleSet& a_toggles) const
	{
		// A capability (Coordinator::SetCapability, the scene lane's commit), not whether the last epoch drew nor what the view's resources
		// or pipelines are yet: the toggle (in a_toggles: the commit's, or the frame's for the frame's own decisions), and the view's
		// targets published, never the render thread's own copy (the first person's colour epoch's formats, known once the main pass's
		// targets were captured; the reflection's faces', known at the first frame). Each member then waits for its own pipelines
		// (ViewSlotReady), and the engine draws it until the frame's epochs are covered (PassCapture).
		return impl && Extensions::ViewOn(a_toggles, a_view) && !failed && impl->View(a_view).LaneTargets().colour != DXGI_FORMAT_UNKNOWN;
	}

	bool IndirectDraws::ViewSlotReady(Api::EngineView a_view, const void* a_tables, const void* a_lookups, const PipelineCatalog* a_catalog, std::uint32_t a_slot,
		std::uint32_t* a_why) const
	{
		if (Extensions::ViewEntryOf(a_view).membership == Api::ViewMembership::LodTechniques) {
			// A LodTechniques view's member (the reflection's): the slot's forward pipeline as a_catalog has it for the view's targets
			// (ViewPipelinesOf's rule, which the revision's shapes resolve from their publication's catalog): the scene lane's commit's, from
			// its own catalog, never the render thread's per-frame list (the state's slot pipelines, which ResolveViewPipelines assigns whole).
			const auto& tables = *static_cast<const SceneTables*>(a_tables);
			const ForwardTargets targets = impl->View(a_view).LaneTargets();  // the published copy (the render thread writes its own at every face capture)
			if (!a_catalog || targets.colour == DXGI_FORMAT_UNKNOWN || a_slot >= tables.objects.size())
				return false;
			const std::uint32_t p = tables.objects[a_slot].pipelineIndex;
			ForwardPipelineKey key;
			return p < tables.pipelines.size() && ViewForwardKey(a_view, tables, p, key) && ForwardPipelineOf(a_catalog, key, targets).valid();
		}
		// A CategoryRoot view's member (the first person's). a_why (the set parity's observer): 1 no catalog or targets, 2 a forward key
		// the forward part cannot draw, 3 no key of the view's, 4 its pipeline not in a_catalog, 5 no Z-prepass group pipeline.
		auto fail = [&](std::uint32_t a_cause) {
			if (a_why)
				*a_why = a_cause;
			return false;
		};
		// The slot's forward pipeline as a_catalog has it for the view's published targets, and its Z-prepass group's depth pipeline in
		// that catalog's main set (the view's pipelines' and PlanViewBuckets' rules): the scene lane's commit's.
		const auto& tables = *static_cast<const SceneTables*>(a_tables);
		const auto& state = impl->View(a_view);
		const ForwardTargets targets = state.LaneTargets();
		if (!a_catalog || targets.colour == DXGI_FORMAT_UNKNOWN || a_slot >= tables.objects.size())
			return fail(1);
		const std::uint32_t p = tables.objects[a_slot].pipelineIndex;
		// The forward part: a forward key's slot draws with the main set's colour pipeline (main-ready: the commit's MainReady) through the
		// colour signature of its state class, into the main pass's attachments with the view's depth, which must be the main pass's format.
		if (p < tables.pipelines.size() && RasterForward(tables.pipelines[p].rasterFlags))
			return (!RasterRefraction(tables.pipelines[p].rasterFlags) && state.forwardCompatible.load(std::memory_order_acquire) &&
			           GetIndirectState(*a_catalog, a_catalog->targetsGeneration).valid) ||
			       fail(2);
		ForwardPipelineKey key;
		if (p >= tables.pipelines.size() || !ViewForwardKey(a_view, tables, p, key))
			return fail(3);
		if (!ForwardPipelineOf(a_catalog, key, targets).valid())
			return fail(4);
		const auto indirect = GetIndirectState(*a_catalog, a_catalog->targetsGeneration);
		const std::uint32_t group = Draws::ZGroupOf(static_cast<const Lookups*>(a_lookups), indirect, p);
		return (indirect.valid && group < indirect.zPipelines.size() && indirect.zPipelines[group].valid()) || fail(5);
	}
}
