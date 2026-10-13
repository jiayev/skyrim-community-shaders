#include "SceneObservers.h"
#include "Features/DrawcallLimitFix/Engine/Access/Walkers.h"
#include "SceneState.h"

#include "Features/DrawcallLimitFix/Scene/Coordinator/Internal.h"

// CS_DCLF_TIMELINE: where an object's time goes between the engine's event and DCLF's publication. The stamps are the entries' notes
// (writtenFrame, boundFrame, memberFrame), the shows the hidden events' (unhideKeys); the residue's stages are SceneRegistrations.cpp's.
// Observers only: nothing here changes what the coordinator decides.

namespace DCLF::Diagnostics
{
	namespace
	{
		using V = SceneView;

		const SceneView::Tracked* EntryOf(const Scene::Coordinator& a_store, const RE::BSGeometry* a_geometry)
		{
			const auto& tracked = V::tracked(a_store);
			const auto it = tracked.find(const_cast<RE::BSGeometry*>(a_geometry));
			return it == tracked.end() ? nullptr : &it->second;
		}

		void OnRecordWritten(const RecordWrittenAnew& a_event)
		{
			if (const auto* entry = EntryOf(a_event.store, a_event.geometry))
				NotesOf(*entry).writtenFrame = V::sceneFrame(a_event.store);
		}

		void OnResidentBound(const ResidentBound& a_event)
		{
			// The binding's time from the record.
			const auto& store = a_event.store;
			const auto& tables = V::tables(store);
			if (a_event.slot >= tables.objectGeometry.size() || tables.IsLayer(a_event.slot))
				return;
			const auto* entry = EntryOf(store, tables.objectGeometry[a_event.slot]);
			if (!entry || entry->slot != a_event.slot)
				return;
			auto& notes = NotesOf(*entry);
			const std::uint32_t sceneFrame = V::sceneFrame(store);
			notes.boundFrame = sceneFrame;
			auto& state = SceneDiagnostics();
			std::scoped_lock lock(state.residueLock);
			++state.timeline.bound;
			if (notes.writtenFrame)
				++state.timeline.writtenToBound[Scene::AgeBucket(sceneFrame - notes.writtenFrame)];
		}

		void OnMembershipPassFailed(const MembershipPassFailed& a_event)
		{
			const auto* entry = EntryOf(a_event.store, a_event.geometry);
			if (!entry)
				return;
			const std::uint8_t why = a_event.why;
			const auto& leaf = a_event.leaf;
			auto& notes = NotesOf(*entry);
			notes.bindFail = why;
			notes.bindFailFrame = V::sceneFrame(a_event.store);
			auto& state = SceneDiagnostics();
			std::scoped_lock lock(state.residueLock);
			auto& timeline = state.timeline;
			++timeline.bindFailures[why];
			if (timeline.bindFailFirst.empty()) {
				timeline.bindFailFirst = fmt::format("'{}' under '{}' ({}, material alpha {:.3f}, alpha property {})", leaf.node && leaf.node->name ? leaf.node->name : "",
					leaf.parent && leaf.parent->name ? leaf.parent->name : "", PrimaryCull::kSyntheticFailNames[why],
					leaf.property && leaf.property->material ? leaf.property->materialAlpha : -1.0f, leaf.alpha ? leaf.alpha->flags : 0xFFFF);
			}
		}

		void OnSetJoined(const SetJoined& a_event)
		{
			const auto& store = a_event.store;
			const auto& tables = V::tables(store);
			const std::uint32_t slot = a_event.slot;
			if (slot >= tables.objectGeometry.size() || tables.IsLayer(slot))
				return;
			auto* geometry = tables.objectGeometry[slot];
			const auto* entry = geometry ? EntryOf(store, geometry) : nullptr;
			if (!entry || entry->slot != slot)
				return;
			bool hiddenNow = false;
			const auto [shown, site] = LastShowOnChain(store, *geometry, *entry, hiddenNow);
			const std::uint32_t sceneFrame = V::sceneFrame(store);
			auto& notes = NotesOf(*entry);
			auto& state = SceneDiagnostics();
			std::scoped_lock lock(state.residueLock);
			auto& timeline = state.timeline;
			++timeline.joins;
			if (!notes.memberFrame) {
				++timeline.firstJoins;
				++timeline.attachToMember[Scene::AgeBucket(sceneFrame - entry->trackedFrame)];
			}
			if (notes.writtenFrame)
				++timeline.writtenToMember[Scene::AgeBucket(sceneFrame - notes.writtenFrame)];
			// A show on its chain since its last join: what its join waited on.
			if (shown && shown > notes.memberFrame)
				++timeline.showToMember[Scene::AgeBucket(sceneFrame - shown)];
			notes.memberFrame = sceneFrame;
		}

		void OnHiddenStore(const HiddenStoreApplied& a_event)
		{
			// The node's last show, for the residue's stale hidden verdicts.
			if (!a_event.hidden)
				SceneDiagnostics().unhideKeys[a_event.key] = { V::sceneFrame(a_event.store), a_event.site };
		}

		void OnHiddenDrained(const HiddenEventsDrained& a_event)
		{
			const std::uint32_t sceneFrame = V::sceneFrame(a_event.store);
			if (sceneFrame % 300 == 0)
				std::erase_if(SceneDiagnostics().unhideKeys, [&](const auto& a_key) { return sceneFrame - a_key.second.first > 600; });
		}
	}

	std::pair<std::uint32_t, std::uint32_t> LastShowOnChain(const Scene::Coordinator& a_store, const RE::BSGeometry& a_geometry, const SceneView::Tracked& a_entry,
		bool& a_hiddenNow)
	{
		// ClassifyFrame's walk: the leaf up to its category node, both included.
		const auto& unhideKeys = SceneDiagnostics().unhideKeys;
		a_hiddenNow = false;
		std::pair<std::uint32_t, std::uint32_t> last{ 0, ~0u };
		Engine::ForEachParent(&a_geometry, [&](const RE::NiAVObject& a_object) {
			a_hiddenNow = a_hiddenNow || V::HiddenForWalk(a_store, &a_object);
			if (const auto it = unhideKeys.find(&a_object); it != unhideKeys.end() && it->second.first >= last.first)
				last = it->second;
			return &a_object != a_entry.categoryNode;
		});
		return last;
	}

	TimelineStats TakeTimeline()
	{
		auto& state = SceneDiagnostics();
		std::scoped_lock lock(state.residueLock);
		return std::exchange(state.timeline, {});
	}

	void InstallTimeline()
	{
		Subscribe<RecordWrittenAnew>(OnRecordWritten);
		Subscribe<ResidentBound>(OnResidentBound);
		Subscribe<MembershipPassFailed>(OnMembershipPassFailed);
		Subscribe<SetJoined>(OnSetJoined);
		Subscribe<HiddenStoreApplied>(OnHiddenStore);
		Subscribe<HiddenEventsDrained>(OnHiddenDrained);
	}
}
