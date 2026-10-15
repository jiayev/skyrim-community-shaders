// FrameView's frame start and hand-over: the frame inputs posted, the adopted publication installed with the log walked up to it,
// the claims installed or withdrawn, the LOD gates released, load screens noted and the actors' wetness captured. Render thread.

#include "Features/DrawcallLimitFix/Scene/FrameView/FrameView.h"
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"
#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"
#include "Features/DrawcallLimitFix/Diagnostics/Reports/Scene.h"
#include "Features/DrawcallLimitFix/Diagnostics/Scene/Events.h"
#include "Features/DrawcallLimitFix/Core/Render/IndirectDraws.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelines.h"
#include "Features/DrawcallLimitFix/Engine/Cull/LodGates.h"
#include "Features/DrawcallLimitFix/Engine/Cull/PassCapture.h"
#include "Features/DrawcallLimitFix/Engine/Cull/PrimaryCull.h"
#include "Features/DrawcallLimitFix/Engine/Capture/SceneCapture.h"
#include "Features/DrawcallLimitFix/Engine/Cull/SunAccumulation.h"
#include "Features/DrawcallLimitFix/Scene/Ports/FrameGlobals.h"
#include "Features/Skin.h"

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
		// The LOD gates' forced releases so far, once a frame: the frame inputs carry it to the coordinator, and the frame withdraws its
		// claims while the installed publication was built for an older one (FrameDriver).
		frameLodForced = LodGates::ForcedGeneration();
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
		inputs->togglesGeneration = Toggles::Get().Generation();
		inputs->verdictsGeneration = verdictsRequested;
		inputs->lodForcedGeneration = frameLodForced;
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
			append(frameFadeSeeds, deltas.fadeSeeds);
			if (frameComponentSeeds.size() < deltas.componentSeeds.size())
				frameComponentSeeds.resize(deltas.componentSeeds.size());
			for (std::size_t c = 0; c < deltas.componentSeeds.size(); ++c)
				append(frameComponentSeeds[c], deltas.componentSeeds[c]);
			append(frameSwitchChanges, deltas.switches);
			frameSwitchResync = frameSwitchResync || std::exchange(deltas.switchResync, false);
			append(frameRetiredImports, deltas.retiredImports);
			for (const auto& change : deltas.actorWetness) {
				if (change.clear)
					frameActorWetness.Clear();
				else
					frameActorWetness.Set(change.slot, change.group, change.identity, change.resetValue);
			}
			deltas.actorWetness.clear();
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
				// The gates these claims flipped, released by the frame installing them (ReleaseGates).
				append(frameGateReleases, deltas.gateFlips);
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
		// The candidates the installed publication carries (the coordinator's as its pass left them).
		if (installed) {
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
		const bool whole = std::exchange(setWithdrawn, false);
		if (!whole && !installPending)
			return;
		installPending = false;
		auto& primary = PrimaryCull::Get();
		PublishFrameClaims(installed->claims);
		frameLackingCount = installed->lackingCount;
		if (whole) {
			// After a withdrawal the engine's hooks hold no claim: every main claim joins.
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

	void FrameView::WithdrawSet()
	{
		ZoneScopedN("CS.DCLF.Scene.WithdrawSet");
		if (setWithdrawn)
			return;
		setWithdrawn = true;
		// Every claim the engine's hooks were told of taken back (the frame's alone; the tables and the publications keep
		// their set, and the next installation applies its claims whole).
		std::vector<const RE::BSGeometry*> left;
		if (notedClaims)
			for (const auto& [geometry, phases] : notedClaims->phases)
				if (phases & kSetMain)
					left.push_back(geometry);
		notedClaims.reset();
		installNotes.clear();
		auto none = std::make_shared<SetSnapshot>();
		none->frame = frame;
		PublishFrameClaims(std::move(none));
		SunAccumulation::Get().PublishExclusion(nullptr);
		if (!left.empty())
			PrimaryCull::Get().NoteSetChanges({}, left);
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

	void FrameView::ReleaseGates()
	{
		auto& rs = gateReleaseStats;
		// The releases' report (the render thread's: the flip to its release), with the engine side's, once a report window.
		Reports::GateReleases(frame);
		if (frameGateReleases.empty())
			return;
		rs.any = true;
		// The claims the frame installed (none when withdrawn: the frame is the engine's, and every later publication is post-swap).
		const SetSnapshot* claims = !setWithdrawn && installed && installed->claims ? installed->claims.get() : nullptr;
		const std::uint64_t now = GateNowNs();
		for (const auto& flip : frameGateReleases) {
			if (!claims)
				++rs.withdrawn;
			else
				Diagnostics::Notify(Diagnostics::GateReleasing{ *claims, flip.token, flip.incoming, flip.outgoing });
			rs.flipToReleaseMs.push_back(GateMs(flip.flipNs, now));
			rs.requestToReleaseMs.push_back(GateMs(flip.requestNs, now));
			rs.flipToReleaseFrames.push_back(frame >= flip.flipFrame ? frame - flip.flipFrame : 0u);
			if (LodGates::Flip(flip.token))
				++rs.released;
			else
				++rs.refused;
		}
		frameGateReleases.clear();
		// The outcomes the flips posted, for the coordinator (folded into the frame start's wakes: ScenePassWakeBatch).
		WakeScenePass(SceneWake::Gate);
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
		// Nothing may walk the scene graph while a load screen is up. A load tears down and rebuilds
		// TES::objRoot and the cell 3D under it, and the attach events queued across it name subtrees that
		// are still being assembled; walking either gives a pointer that is stale or garbage.
		//
		// The queues are still drained (by the coordinator's passes, their one consumer), because they hold references to
		// attached subtrees and must not grow while the world is not rendered - but the events are discarded for the tracking rather
		// than applied (the mirror keeps them: loadingCarry), and the first frame after the load rebuilds the tracked set from scratch.
		// A load invalidates all of it anyway, so nothing is lost by not trying to track across it.
		if (!IsLoadingScreenUp()) {
			if (std::exchange(loadingPosted, false)) {
				channels.loadingSeen.store(false, std::memory_order_release);
				// The engine's events wake the passes again, and one takes the load's carry now.
				SetScenePassLoading(false);
				WakeScenePass(SceneWake::LoadDrain);
			}
			return false;
		}
		// Only the walking stops. The tracked set is deliberately left alone until the load is over:
		// dropping it here would release the game buffers the tables reference while the previous frame's
		// epoch is still in flight, and a draw would read a freed device address (device lost). The entries
		// hold NiPointers, so holding them across the load is safe, and the rescan replaces them on a normal frame.
		// What the load invalidates of the coordinator's (the rescan, the drained events not yet applied, the move events) is
		// dropped by the coordinator, told by a marker batch posted once a load (ApplyLoading): nothing of its state is written here.
		// LoadingSeen first, then the marker: a pass that takes the marker sees the load, and drains into the mirror's carry.
		if (!std::exchange(loadingPosted, true)) {
			channels.loadingSeen.store(true, std::memory_order_release);
			channels.loadMarkers.Push(true);
			// The first category capture after the load pins every category node (the coordinator rescans them all), and is made
			// whatever the signature says; the switches are brought up to date at the first frame's start after it (CatchUpSwitches),
			// and PrimaryCull reads them all again from the next frame's start.
			rescanCaptureAll = true;
			categoryCapturePending = true;
			worldCatchUpPending = true;
			switchResyncNext = true;
		}
		// The set does not survive a load: it names geometry from the cell being torn down, and withholds its passes from the
		// engine. Nothing is withheld until the next commit publishes the set again.
		PublishFrameClaims(nullptr);
		// While it is up the engine's events wake no pass (a load pushes thousands a frame, each for the mirror's carry alone): one
		// pass drains them here, once a Present and frame start.
		SetScenePassLoading(true);
		WakeScenePass(SceneWake::LoadDrain);
		return true;
	}

	std::vector<WetnessValue> FrameView::CaptureWetness()
	{
		// Advanced Skin's wetness, per actor-owned object. Skin::GetWetness keeps each actor's fading state and computes it once a
		// frame (the first call; later ones, including its own SetupGeometry hook for the draws the engine still makes this frame,
		// return the same value), so calling it here for every actor in the tables advances every actor's fade once a frame,
		// whether or not the engine draws it. Its cache is not thread-safe: the render thread's, at the frame's start.
		ZoneScopedN("CS.DCLF.Capture.Wetness");
		std::vector<WetnessValue> values;
		auto& skin = globals::features::skin;
		const bool wetness = skin.loaded && skin.settings.EnableSkin;
		// The actors' membership is the render thread's own index (the log's changes, HandOverAtFrameStart), their geometries the
		// newest publication's (held by it), never the coordinator's tables.
		const auto latest = Newest();
		const Tables* source = latest ? latest->tables.get() : nullptr;
		const auto wetnessStats = frameActorWetness.Update([&](std::uint32_t o) {
			auto* geometry = source && o < source->objectGeometry.size() ? source->objectGeometry[o] : nullptr;
			const float4 value = wetness && geometry ? skin.GetWetness(geometry) : float4{};
			return ActorValueIndex::Value{ value.x, value.y, value.z, value.w };
		}, [&](std::uint32_t o, const auto& row) { values.push_back({ o, row }); });
		TracyPlot("CS.DCLF.Wetness.Actors", static_cast<std::int64_t>(wetnessStats.actors));
		TracyPlot("CS.DCLF.Wetness.ChangedActors", static_cast<std::int64_t>(wetnessStats.changed));
		TracyPlot("CS.DCLF.Wetness.VisitedMeshes", static_cast<std::int64_t>(wetnessStats.visited));
		return values;
	}
}
