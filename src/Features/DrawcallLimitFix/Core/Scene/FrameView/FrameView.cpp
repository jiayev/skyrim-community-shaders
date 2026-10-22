// FrameView's frame start and hand-over: the frame inputs posted, the adopted publication installed with the log walked up to it,
// the claims installed (never withdrawn once installed), the components' deltas and the published classes' membership writes (the
// shading fields') gathered for their adoption, and load screens noted. Render thread.

#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"
#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"
#include "Features/DrawcallLimitFix/Diagnostics/Scene/Events.h"
#include "Features/DrawcallLimitFix/Core/Render/IndirectDraws.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelines.h"
#include "Features/DrawcallLimitFix/Core/Handoff/PassCapture.h"
#include "Features/DrawcallLimitFix/Core/Handoff/PrimaryCull.h"
#include "Features/DrawcallLimitFix/Core/Engine/Capture/SceneCapture.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ShadowViews.h"
#include "Features/DrawcallLimitFix/Core/Shading/FrameGlobals.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ShadingFields.h"

namespace DCLF::Scene
{
	FrameView& FrameView::Get()
	{
		static FrameView view;
		return view;
	}

	void FrameView::BeginFrame()
	{
		// Switch events are taken on the render thread only (PushSwitch); the passes drain and apply them on the coordinator.
		switchEventThread.store(::GetCurrentThreadId(), std::memory_order_relaxed);
		++frame;
		// The frame's engine globals, before anything of the frame reads them; the scene work's passes take them with the
		// frame inputs.
		const auto captureStart = std::chrono::steady_clock::now();
		// The Lighting shader first (once): the globals' material sources read it.
		CaptureLightingShader();
		frameGlobals = FrameGlobals::Capture();
		SceneCapture::SetMainThread(::GetCurrentThreadId());
		SceneCapture::SetFrame(frame);
		captureNs += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - captureStart).count());
		++captureFrames;
		// What the coordinator's passes take of the frame: posted after the capture; the post wakes the pump (folded with
		// the frame start's other posts, ScenePassWakeBatch).
		PostFrameInputs();
	}

	void FrameView::PostFrameInputs()
	{
		auto inputs = std::make_unique<FrameInputs>();
		inputs->globals = frameGlobals;
		inputs->frame = frame;
		// The membership witness, sampled here (PrimaryCull reads the same sample this frame): the binds' (BindByMembership, the
		// commit after them reads none).
		inputs->membershipWitness = PrimaryCull::Get().SampleMembershipWitness();
		// The toggles the coordinator's next commit is made under (the newest, applied at this frame's start), with their generation.
		inputs->toggles = Toggles::Get().Active();
		inputs->togglesGeneration = Toggles::Get().Generation();
		inputs->verdictsGeneration = verdictsRequested;
		channels.frameInputs.Post(std::move(inputs));
		WakeScenePass(SceneWake::FrameInput);
	}

	void FrameView::HandOverAtFrameStart(std::shared_ptr<const void> a_adopted)
	{
		ZoneScopedN("CS.DCLF.Scene.HandOverAtFrameStart");
		// Nothing of the coordinator's is read here. The notes the scene work held for PrimaryCull are delivered first.
		DeliverPrimaryNotes();
		// The publication of the snapshot the frame adopted (IndirectDraws::AdoptSnapshot), installed when newer than the installed
		// one; a snapshot made again for the installed one (new recordings, the frame's inputs moved) brings none.
		auto adopted = std::static_pointer_cast<const ScenePublication>(std::move(a_adopted));
		const bool fresh = adopted && (!installed || adopted->sequence > installed->sequence);
		// The frame inputs of every publication up to the installed one, in order: the log holds one the builder skipped too.
		using Log = PublicationLog<PublicationDeltas>;
		Log::Node* at = deltasCursor ? deltasCursor : channels.publicationLog.First();
		const std::uint64_t upTo = fresh ? adopted->sequence : (installed ? installed->sequence : 0);
		auto append = [](auto& a_to, auto& a_from) {
			a_to.insert(a_to.end(), std::make_move_iterator(a_from.begin()), std::make_move_iterator(a_from.end()));
			a_from.clear();
		};
		for (Log::Node* next = Log::Next(at); next && next->value.publication <= upTo; next = Log::Next(at)) {
			at = next;
			auto& deltas = at->value;
			append(framePlans, deltas.plans);
			append(frameShading, deltas.shading);
			if (frameComponentSeeds.size() < deltas.componentSeeds.size())
				frameComponentSeeds.resize(deltas.componentSeeds.size());
			for (std::size_t c = 0; c < deltas.componentSeeds.size(); ++c)
				append(frameComponentSeeds[c], deltas.componentSeeds[c]);
			append(frameSwitchChanges, deltas.switches);
			frameSwitchResync = frameSwitchResync || std::exchange(deltas.switchResync, false);
			append(frameRetiredImports, deltas.retiredImports);
			// The published classes' membership writes, to the shading fields whose members they are (their own indexes of owners).
			for (std::size_t c = 0; c < deltas.classMembers.size(); ++c) {
				if (!deltas.classMembers[c].empty())
					Extensions::ForwardClassMembers(static_cast<Api::EntryClass>(c), deltas.classMembers[c]);
				deltas.classMembers[c].clear();
			}
			for (const auto& change : deltas.lightEntries) {
				if (change.kind > 0)
					frameLightEntrySet.insert(change.node);
				else if (change.kind < 0)
					frameLightEntrySet.erase(change.node);
				else
					frameLightEntrySet.clear();
			}
			deltas.lightEntries.clear();
		}
		deltasCursor = at;
		frameSwitchResync = frameSwitchResync || std::exchange(switchResyncNext, false);
		if (fresh) {
			// The claims' changes of every publication up to it, in order (never past the frame inputs' cursor).
			const std::uint64_t sequence = adopted->sequence;
			Log::Node* note = notesCursor ? notesCursor : channels.publicationLog.First();
			Log::Node* const bound = deltasCursor ? deltasCursor : channels.publicationLog.First();
			std::uint64_t oldestNs = 0;
			std::uint32_t oldestFrame = 0;
			while (note != bound) {
				Log::Node* next = Log::Next(note);
				if (!next || next->value.publication > sequence)
					break;
				note = next;
				auto& deltas = note->value;
				installNotes.emplace_back(std::move(deltas.joined), std::move(deltas.left));
				// The components' deltas these publications carried (the LOD gates these claims flipped), adopted by the frame installing
				// them (Extensions::ComponentsAdopt).
				if (frameComponentDeltas.size() < deltas.componentDeltas.size())
					frameComponentDeltas.resize(deltas.componentDeltas.size());
				for (std::size_t c = 0; c < deltas.componentDeltas.size(); ++c)
					append(frameComponentDeltas[c], deltas.componentDeltas[c]);
				// The oldest event these publications carried (the ones the builder skipped past included).
				if (deltas.eventNs && (!oldestNs || deltas.eventNs < oldestNs)) {
					oldestNs = deltas.eventNs;
					oldestFrame = deltas.eventFrame;
				}
			}
			notesCursor = note;
			if (oldestNs) {
				const auto nowNs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
				IndirectDraws::Get().NoteEventAdoption(nowNs > oldestNs ? static_cast<double>(nowNs - oldestNs) / 1.0e6 : 0.0, frame >= oldestFrame ? frame - oldestFrame : 0u);
			}
			channels.publicationLog.Release(note);
			TracyPlot("CS.DCLF.InstallDelayFrames", static_cast<std::int64_t>(frame - adopted->commitFrame));
			// The publications the builder skipped past, and the replaced one back to the coordinator, which drops it.
			publicationStats.skipped += installed ? sequence - installed->sequence - 1 : 0;
			ReturnPublication(std::move(installed));
			installed = std::move(adopted);
			installPending = true;
			++publicationStats.installed;
		} else {
			++publicationStats.kept;
		}
		// The frame's tables, lookups and catalog are one publication's: the installed one's, none before the first (empty
		// tables and lookups). The indices its epochs resolve are of the set versions it binds (FrameIndirectState, FrameShadowIndirectState
		// read the held catalog), and its entries are parallel to its tables.
		acceptedTables = installed ? installed->tables : nullptr;
		frameLookups = installed ? installed->lookups : nullptr;
		frameCatalog = installed ? installed->catalog : nullptr;
		frameTablesGeneration = installed ? installed->tablesGeneration : 0u;
		DrawPipelines::Get().HoldCatalog(frameCatalog);
		// The candidates the installed publication carries (the coordinator's as its pass left them), and the toggles its commit was made
		// under, the frame's from here (FrameToggles: a toggle takes effect for the frames with the first publication committed under it).
		if (installed) {
			Toggles::Get().Adopt(installed->toggles);
			frameSunCandidates = installed->sunCandidates;
			frameLightCandidates = installed->lightCandidates;
			frameLightEntriesAppeared = installed->lightEntriesAppeared;
			frameCategoryNodes = installed->categoryNodes;
		}
	}

	void FrameView::InstallClaims()
	{
		if (!installed)
			return;
		// The first installation (NotDrawingYet ends here, for good: no claim is ever withdrawn after it).
		const bool whole = !std::exchange(drawingStarted, true);
		if (!whole && !installPending)
			return;
		installPending = false;
		auto& primary = PrimaryCull::Get();
		PublishFrameClaims(installed->claims);
		frameLackingCount = installed->lackingCount;
		if (whole) {
			// Before the first installation the engine's hooks hold no claim: every main claim joins.
			std::vector<const RE::BSGeometry*> joined;
			for (const auto& [geometry, phases] : installed->claims->phases)
				if (phases & kSetMain)
					joined.push_back(geometry);
			if (!joined.empty())
				primary.NoteSetChanges(joined, {});
		} else {
			for (const auto& [joined, left] : installNotes)
				if (!joined.empty() || !left.empty())
					primary.NoteSetChanges(joined, left);
		}
		installNotes.clear();
		notedClaims = installed->claims;
	}

	void FrameView::PublishFrameClaims(std::shared_ptr<const SetSnapshot> a_claims)
	{
		// DCLF's readers take the frame's claims from FrameClaims; the registration hooks withhold by the same object until the culls stop.
		PassCapture::Get().PublishSet(a_claims);
		std::atomic_store(&frameClaims, std::move(a_claims));
	}

	void FrameView::ReturnPublication(std::shared_ptr<const ScenePublication>&& a_publication)
	{
		// The coordinator drops it at its next pass (no retirement on the render thread). Its tables stay in the pool, which the
		// coordinator alone writes; its lookups', catalog's and draws' owners are cleanup-queue owners.
		channels.retiredPublications.Push(std::move(a_publication));
	}

	void FrameView::DeliverPrimaryNotes()
	{
		auto& primary = PrimaryCull::Get();
		channels.primaryNotes.Drain([&primary](PrimaryNote&& a_note) {
			switch (a_note.kind) {
			case PrimaryNote::Kind::HiddenKey:
				primary.NoteHiddenKey(a_note.key);
				break;
			case PrimaryNote::Kind::MemberLost:
				primary.NoteMemberLost(static_cast<const RE::BSGeometry*>(a_note.key));
				break;
			case PrimaryNote::Kind::AllMembersLost:
				primary.NoteAllMembersLost();
				break;
			}
		});
	}

	std::vector<ShadingItem> FrameView::PeekShadingItems() const
	{
		// Those the log holds past the frame's cursor (published, not taken).
		std::vector<ShadingItem> items;
		using Log = PublicationLog<PublicationDeltas>;
		for (const Log::Node* next = Log::Next(deltasCursor ? deltasCursor : channels.publicationLog.First()); next; next = Log::Next(const_cast<Log::Node*>(next)))
			items.insert(items.end(), next->value.shading.begin(), next->value.shading.end());
		return items;
	}

	const SetSnapshot* FrameView::AdoptedClaims() const
	{
		// None before the first installation (NotDrawingYet): the engine draws everything, and every later publication is post-swap.
		return drawingStarted && installed && installed->claims ? installed->claims.get() : nullptr;
	}

	void FrameView::ClearAdoptedDeltas()
	{
		for (auto& deltas : frameComponentDeltas)
			deltas.clear();
	}

	bool FrameView::IsLoadingScreenUp()
	{
		auto* ui = RE::UI::GetSingleton();
		return ui && ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME);
	}

	bool FrameView::NoteLoadingScreen()
	{
		// Switch events are taken on the render thread only (PushSwitch): this is it (Present and the frame's start).
		switchEventThread.store(::GetCurrentThreadId(), std::memory_order_relaxed);
		// Nothing may walk the scene graph while a load screen is up. A load tears down and rebuilds TES::objRoot and the cell 3D
		// under it; a live walk gives a pointer that is stale or garbage.
		//
		// The coordinator's passes apply the load's events as any others: the unloading world's detaches erase what it tracked, the
		// loading world's attaches (captured at their hooks: the mirror's records, no walk) track what comes, under the category nodes
		// the first capture after the load names. No table is written while the load is up (BuildScenePhase).
		if (!IsLoadingScreenUp()) {
			if (std::exchange(loadingPosted, false)) {
				channels.loadingSeen.store(false, std::memory_order_release);
				// The engine's events wake the passes again.
				SetScenePassLoading(false);
				WakeScenePass(SceneWake::LoadDrain);
			}
			return false;
		}
		if (!std::exchange(loadingPosted, true)) {
			channels.loadingSeen.store(true, std::memory_order_release);
			// The first category capture after the load is made whatever the signature says, and the frame start's job of before it is
			// not resumed (its capture and requests name the world it tore down); the switches are brought up to date at the first
			// frame's start after it (CatchUpSwitches), and PrimaryCull reads them all again from the next frame's start.
			loadSinceCapture = true;
			categoryCapturePending = true;
			worldCatchUpPending = true;
			switchResyncNext = true;
		}
		// The set does not survive a load: it names geometry from the cell being torn down, and withholds its passes from the
		// engine. Nothing is withheld until the next commit publishes the set again.
		PublishFrameClaims(nullptr);
		// While it is up the engine's events wake no pass (a load pushes thousands a frame): one pass applies them here, once a Present
		// and frame start.
		SetScenePassLoading(true);
		WakeScenePass(SceneWake::LoadDrain);
		return true;
	}

	std::span<RE::BSGeometry* const> FrameView::NewestObjectGeometry() const
	{
		const Tables* source = installed ? installed->tables.get() : nullptr;
		return source ? std::span<RE::BSGeometry* const>(source->objectGeometry) : std::span<RE::BSGeometry* const>{};
	}
}
