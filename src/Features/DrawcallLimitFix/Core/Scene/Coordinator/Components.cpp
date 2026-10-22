// The features' coordinator components (scene lane): their states, the residency points' calls to their hooks (Api::CoordinatorComponent),
// and the coordinator's services their context reaches (Api::CoordinatorComponentContext).

#include "Internal.h"

#include "Features/DrawcallLimitFix/Core/Extensions/Registry.h"

namespace DCLF::Api
{
	static_assert(kMirroredKindTree == SceneCapture::kKindTree, "the Api's mirrored kind bits are the capture's");
	static_assert(kMirroredKindCaptured == SceneCapture::kKindCaptured, "the Api's mirrored kind bits are the capture's");

	bool CoordinatorComponentContext::DenseWalk() const
	{
		return coordinator.denseWalk;
	}

	const void* CoordinatorComponentContext::MirroredFadeNode(std::uint32_t a_slot, std::uint32_t& a_kind) const
	{
		a_kind = 0;
		const auto* geometry = a_slot < tables.objectGeometry.size() ? tables.objectGeometry[a_slot] : nullptr;
		const auto* record = geometry ? coordinator.mirror.Leaf(geometry).property : nullptr;
		if (!record || !record->fadeNode)
			return nullptr;
		if (const auto* node = coordinator.mirror.Node(record->fadeNode))
			a_kind = node->kind;
		return record->fadeNode;
	}

	RE::NiPointer<RE::NiAVObject> CoordinatorComponentContext::HoldFadeNode(std::uint32_t a_slot, const void* a_node) const
	{
		return coordinator.HeldFadeNode(a_slot, a_node);
	}

	void CoordinatorComponentContext::NoteTouched(std::uint32_t a_slot) const
	{
		coordinator.NoteResidentTouched(a_slot);
	}

	void CoordinatorComponentContext::HandBack(RE::NiPointer<RE::NiAVObject>&& a_reference) const
	{
		coordinator.HandBack(std::move(a_reference));
	}

	void CoordinatorComponentContext::HandBack(std::vector<RE::NiPointer<RE::NiAVObject>>& a_references) const
	{
		coordinator.HandBack(a_references);
	}

	void CoordinatorComponentContext::HandBack(RE::NiPointer<RE::NiRefObject>&& a_reference) const
	{
		coordinator.HandBack(std::move(a_reference));
	}

	CoordinatorComponentContext::TrackedWrite CoordinatorComponentContext::TrackedWriteOf(const RE::BSGeometry* a_geometry) const
	{
		const auto entry = coordinator.tracked.find(const_cast<RE::BSGeometry*>(a_geometry));
		if (entry == coordinator.tracked.end())
			return {};
		return { true, entry->second.slot, entry->second.streamWaiting };
	}

	bool CoordinatorComponentContext::ScheduleWrite(RE::BSGeometry* a_geometry) const
	{
		const auto entry = coordinator.tracked.find(a_geometry);
		if (entry == coordinator.tracked.end())
			return false;
		coordinator.Schedule(entry->first, entry->second, true);
		return true;
	}

	RE::BSGeometry* CoordinatorComponentContext::TrackedGeometry(const void* a_key) const
	{
		const auto entry = coordinator.tracked.find(static_cast<RE::BSGeometry*>(const_cast<void*>(a_key)));
		return entry != coordinator.tracked.end() ? entry->second.geometry.get() : nullptr;
	}

	void CoordinatorComponentContext::RetireRange(std::uint32_t a_first, std::uint32_t a_count) const
	{
		tables.Retire(SceneTables::kRetiredComponentRange, a_first, a_count, static_cast<std::uint8_t>(component));
	}

	void CoordinatorComponentContext::RequestSeed(FadeSeedItem&& a_seed) const
	{
		auto& requests = coordinator.componentSeedRequests;
		if (requests.size() <= component)
			requests.resize(std::size_t(component) + 1);
		requests[component].push_back(std::move(a_seed));
	}

	void CoordinatorComponentContext::PublishDelta(std::shared_ptr<const void> a_delta) const
	{
		// Pending until the deltas are taken (TakeDeltas): DeltasPending counts them, so the next pass publishes.
		auto& requests = coordinator.componentDeltaRequests;
		if (requests.size() <= component)
			requests.resize(std::size_t(component) + 1);
		requests[component].push_back(std::move(a_delta));
	}

	bool CoordinatorComponentContext::Counted(std::uint32_t a_slot) const
	{
		const auto& counted = coordinator.residentCounted;
		return a_slot < counted.size() && counted[a_slot].pipeline != Scene::CoordinatorResidents::ResidentCounted::kNone;
	}

	const SceneMirror& CoordinatorComponentContext::Mirror() const
	{
		return coordinator.mirror;
	}

	MirroredNode CoordinatorComponentContext::MirroredNodeOf(const void* a_key) const
	{
		const auto* record = coordinator.mirror.Node(a_key);
		if (!record)
			return {};
		return { true, record->parent, record->rtti, record->kind, record->children, record->captured };
	}

	std::uint32_t CoordinatorComponentContext::ObjectSlots() const
	{
		return static_cast<std::uint32_t>(tables.objects.size());
	}

	const RE::BSGeometry* CoordinatorComponentContext::ObjectGeometry(std::uint32_t a_slot) const
	{
		return a_slot < tables.objectGeometry.size() ? tables.objectGeometry[a_slot] : nullptr;
	}

	std::uint32_t CoordinatorComponentContext::RangeSpan(std::uint32_t a_slot) const
	{
		return RangeSpanOf(a_slot < tables.skinPartitions.size() ? tables.skinPartitions[a_slot] : 0u);
	}

	std::uint32_t CoordinatorComponentContext::RangeSpanOf(std::uint32_t a_partitions)
	{
		return DecalSpanOf(a_partitions);
	}

	std::span<std::uint32_t> CoordinatorComponentContext::SizedRangeOrdinals() const
	{
		if (tables.rangeOrdinal.size() != tables.objects.size())
			tables.rangeOrdinal.resize(tables.objects.size(), ~0u);
		return tables.rangeOrdinal;
	}

	std::span<std::uint32_t, kFixedMainRanges> CoordinatorComponentContext::RangeCounts() const
	{
		return tables.rangeCount;
	}

	void CoordinatorComponentContext::NoteRangeOrdinalChanged(std::uint32_t a_slot) const
	{
		tables.NoteChange(a_slot, kChangeBindings);
	}

	std::uint64_t CoordinatorComponentContext::PassSerial() const
	{
		return coordinator.passSerial;
	}

	std::uint32_t CoordinatorComponentContext::PassStamp() const
	{
		return coordinator.PassStamp();
	}

	const CoordinatorComponentContext::EntryRangesMap& CoordinatorComponentContext::EntryRanges() const
	{
		return coordinator.entryRanges;
	}

	const std::vector<EntryRange>* CoordinatorComponentContext::EntryRangesOf(const void* a_entry) const
	{
		return coordinator.EntryRangesOf(static_cast<const RE::BSGeometry*>(a_entry));
	}

	const std::vector<EntryRange>* CoordinatorComponentContext::MirroredRanges(const void* a_entry) const
	{
		const auto* record = coordinator.mirror.Geometry(a_entry);
		return record ? &record->ranges : nullptr;
	}

	void CoordinatorComponentContext::SetEntryRanges(const void* a_entry, std::vector<EntryRange>&& a_ranges, bool a_rewrite) const
	{
		auto* geometry = static_cast<RE::BSGeometry*>(const_cast<void*>(a_entry));
		if (a_rewrite)
			coordinator.pendingEvaluation.push_back(geometry);
		coordinator.entryRanges[geometry] = std::move(a_ranges);
	}

	Mark CoordinatorComponentContext::MarkRoot(const void* a_key, std::uint64_t a_token, std::uint8_t a_side, bool a_ownHide) const
	{
		// A root another mark names (another component's, or this one's under another token) is this one's from here: the displaced
		// mark's hide goes with it unless this one owns it too. Marked again under the same token, the root keeps a hide it owns.
		auto& marks = coordinator.rootMarks;
		const auto [at, inserted] = marks.try_emplace(a_key);
		Mark displaced;
		bool ownHide = a_ownHide;
		if (!inserted) {
			if (at->second.mark.component.index != component || at->second.mark.token != a_token) {
				displaced = at->second.mark;
				if (at->second.ownHide && !a_ownHide)
					coordinator.mirror.SetGated(a_key, false);
			} else {
				ownHide = ownHide || at->second.ownHide;
			}
		}
		at->second = { Mark{ CoordinatorComponentId{ component }, a_token, a_side }, ownHide };
		// Its own hide the component's from here (HiddenForWalk).
		if (a_ownHide)
			coordinator.mirror.SetGated(a_key, true);
		return displaced;
	}

	bool CoordinatorComponentContext::UnmarkRoot(const void* a_key, std::uint64_t a_token) const
	{
		auto& marks = coordinator.rootMarks;
		const auto at = marks.find(a_key);
		if (at == marks.end() || at->second.mark.component.index != component || at->second.mark.token != a_token)
			return false;
		const bool ownHide = at->second.ownHide;
		marks.erase(at);
		if (ownHide)
			coordinator.mirror.SetGated(a_key, false);
		return true;
	}

	Mark CoordinatorComponentContext::MarkOf(const void* a_key) const
	{
		const auto& marks = coordinator.rootMarks;
		const auto at = marks.find(a_key);
		return at != marks.end() ? at->second.mark : Mark{};
	}

	void CoordinatorComponentContext::SetEntryVeto(const void* a_entry, std::uint8_t a_veto) const
	{
		coordinator.SetEntryVeto(a_entry, a_veto);
	}

	ObservedState CoordinatorComponentContext::ObservedSlot(std::uint32_t a_slot) const
	{
		return coordinator.ObservedSlotOf(a_slot);
	}

	bool CoordinatorComponentContext::LayerSlot(std::uint32_t a_slot) const
	{
		return tables.IsLayer(a_slot);
	}

	std::uint32_t CoordinatorComponentContext::VetoCount(std::uint8_t a_veto) const
	{
		return a_veto == kVetoHold ? coordinator.vetoHeld : a_veto == kVetoObserve ? coordinator.vetoObserved : 0u;
	}

	void CoordinatorComponentContext::SetCommitLive(bool a_live) const
	{
		auto& live = coordinator.componentCommitLive;
		if (live.size() <= component)
			live.resize(std::size_t(component) + 1, 0);
		live[component] = a_live ? 1 : 0;
	}

	TrackedState CoordinatorComponentContext::TrackedStateOf(const void* a_entry) const
	{
		return coordinator.TrackedStateOf(a_entry);
	}

	bool CoordinatorComponentContext::RestructuredSincePublication(std::uint32_t a_slot) const
	{
		return coordinator.RestructuredSincePublication(a_slot);
	}

	void CoordinatorComponentContext::Requeue(const void* a_entry, std::uint8_t a_flags) const
	{
		const auto it = coordinator.tracked.find(static_cast<RE::BSGeometry*>(const_cast<void*>(a_entry)));
		if (it == coordinator.tracked.end())
			return;
		auto& entry = it->second;
		// Its verdict taken again: always, or only when it was classified (an unclassified entry's evaluation is pending already).
		if ((a_flags & kRequeueReevaluateAlways) || ((a_flags & kRequeueReevaluate) && entry.candidateFrame != 0)) {
			entry.candidateFrame = 0;
			coordinator.pendingEvaluation.push_back(it->first);
		}
		// Its slots committed again by the next commit (queued at its start).
		if (a_flags & kRequeueSlots)
			for (const std::uint32_t slot : { entry.slot, entry.layerSlot })
				if (slot != kNoObjectSlot)
					coordinator.requeued.push_back(slot);
	}

	void CoordinatorComponentContext::SubtreeEntries(const void* a_root, std::vector<const void*>& a_out, bool& a_lacking) const
	{
		// One walk of the mirror: the subtree as the tracking walks it (MirrorSubtree), its tracked geometries.
		std::vector<std::pair<RE::BSGeometry*, Ineligible>> found;
		if (!coordinator.mirror.Node(a_root))
			a_lacking = true;
		coordinator.MirrorSubtree(a_root, Ineligible::None, found, a_lacking);
		for (const auto& [geometry, reason] : found)
			if (coordinator.tracked.contains(geometry))
				a_out.push_back(geometry);
	}
}

namespace DCLF::Scene
{
	const std::shared_ptr<void>& Coordinator::ComponentState(std::uint32_t a_component)
	{
		const auto registered = Extensions::RegisteredCoordinatorComponents();
		if (components.size() < registered.size()) {
			const std::size_t first = components.size();
			components.resize(registered.size());
			for (std::size_t c = first; c < registered.size(); ++c)
				components[c] = registered[c].make();
		}
		return components[a_component];
	}

	void Coordinator::ComponentsResident(std::uint32_t a_slot)
	{
		const auto registered = Extensions::RegisteredCoordinatorComponents();
		for (std::uint32_t c = 0; c < registered.size(); ++c)
			if (registered[c].residency.resident)
				registered[c].residency.resident({ tables, ComponentState(c), *this, c }, a_slot);
	}

	void Coordinator::ComponentsLeft(std::uint32_t a_slot)
	{
		const auto registered = Extensions::RegisteredCoordinatorComponents();
		for (std::uint32_t c = 0; c < registered.size(); ++c)
			if (registered[c].residency.left)
				registered[c].residency.left({ tables, ComponentState(c), *this, c }, a_slot);
	}

	void Coordinator::ComponentsSlotWritten(std::uint32_t a_slot)
	{
		const auto registered = Extensions::RegisteredCoordinatorComponents();
		for (std::uint32_t c = 0; c < registered.size(); ++c)
			if (registered[c].slots.written)
				registered[c].slots.written({ tables, ComponentState(c), *this, c }, a_slot);
	}

	void Coordinator::ComponentsSlotReleased(std::uint32_t a_slot)
	{
		const auto registered = Extensions::RegisteredCoordinatorComponents();
		for (std::uint32_t c = 0; c < registered.size(); ++c)
			if (registered[c].slots.released)
				registered[c].slots.released({ tables, ComponentState(c), *this, c }, a_slot);
	}

	void Coordinator::ComponentsSwitched(const void* a_switch)
	{
		const auto registered = Extensions::RegisteredCoordinatorComponents();
		for (std::uint32_t c = 0; c < registered.size(); ++c)
			if (registered[c].events.switched)
				registered[c].events.switched({ tables, ComponentState(c), *this, c }, a_switch);
	}

	void Coordinator::ComponentsAdmitted(std::span<const Api::StandInRoot> a_roots)
	{
		const auto registered = Extensions::RegisteredCoordinatorComponents();
		for (std::uint32_t c = 0; c < registered.size(); ++c)
			if (registered[c].events.admitted)
				registered[c].events.admitted({ tables, ComponentState(c), *this, c }, a_roots);
	}

	void Coordinator::ComponentsRecount(bool a_rebuild, std::span<const std::uint32_t> a_touched)
	{
		const auto registered = Extensions::RegisteredCoordinatorComponents();
		const Api::ResidentRecount recount{ a_rebuild, a_touched, residents };
		for (std::uint32_t c = 0; c < registered.size(); ++c)
			if (registered[c].residency.recount)
				registered[c].residency.recount({ tables, ComponentState(c), *this, c }, recount);
	}

	void Coordinator::ComponentsReset()
	{
		const auto registered = Extensions::RegisteredCoordinatorComponents();
		for (std::uint32_t c = 0; c < registered.size(); ++c)
			if (registered[c].retirement.reset)
				registered[c].retirement.reset({ tables, ComponentState(c), *this, c });
	}

	void Coordinator::ComponentsAllLeft()
	{
		const auto registered = Extensions::RegisteredCoordinatorComponents();
		for (std::uint32_t c = 0; c < registered.size(); ++c)
			if (registered[c].residency.allLeft)
				registered[c].residency.allLeft({ tables, ComponentState(c), *this, c });
	}

	void Coordinator::ComponentsAccumulated(std::uint32_t a_slot, std::uint64_t a_chain)
	{
		const auto registered = Extensions::RegisteredCoordinatorComponents();
		const Api::AccumulatedPatch patch{ a_chain };
		for (std::uint32_t c = 0; c < registered.size(); ++c)
			if (registered[c].accumulate.patched)
				registered[c].accumulate.patched({ tables, ComponentState(c), *this, c }, a_slot, patch);
	}

	void Coordinator::ComponentsRewritten(std::uint32_t a_slot, bool a_written, std::uint32_t a_partitionsBefore, std::uint32_t a_partitionsNow)
	{
		const auto registered = Extensions::RegisteredCoordinatorComponents();
		const Api::SlotRewrite rewrite{ a_written, a_partitionsBefore, a_partitionsNow };
		for (std::uint32_t c = 0; c < registered.size(); ++c)
			if (registered[c].walk.rewritten)
				registered[c].walk.rewritten({ tables, ComponentState(c), *this, c }, a_slot, rewrite);
	}

	void Coordinator::ComponentsJoinsDone()
	{
		const auto registered = Extensions::RegisteredCoordinatorComponents();
		for (std::uint32_t c = 0; c < registered.size(); ++c)
			if (registered[c].accumulate.joinsDone)
				registered[c].accumulate.joinsDone({ tables, ComponentState(c), *this, c });
	}

	void Coordinator::ComponentsWalkBegun()
	{
		const auto registered = Extensions::RegisteredCoordinatorComponents();
		for (std::uint32_t c = 0; c < registered.size(); ++c)
			if (registered[c].walk.begun)
				registered[c].walk.begun({ tables, ComponentState(c), *this, c });
	}

	void Coordinator::ComponentsScheduling(bool a_full)
	{
		const auto registered = Extensions::RegisteredCoordinatorComponents();
		for (std::uint32_t c = 0; c < registered.size(); ++c) {
			if (!registered[c].walk.scheduling)
				continue;
			Api::WalkScheduling scheduling{ a_full };
			registered[c].walk.scheduling({ tables, ComponentState(c), *this, c }, scheduling);
		}
	}

	void Coordinator::ComponentsWalkEnded()
	{
		const auto registered = Extensions::RegisteredCoordinatorComponents();
		for (std::uint32_t c = 0; c < registered.size(); ++c)
			if (registered[c].walk.ended)
				registered[c].walk.ended({ tables, ComponentState(c), *this, c });
	}

	void Coordinator::ComponentsDense(bool a_begun)
	{
		const auto registered = Extensions::RegisteredCoordinatorComponents();
		for (std::uint32_t c = 0; c < registered.size(); ++c)
			if (const auto hook = a_begun ? registered[c].dense.begun : registered[c].dense.ended)
				hook({ tables, ComponentState(c), *this, c });
	}

	void Coordinator::ComponentsForgot(const RE::BSGeometry* a_geometry)
	{
		const auto registered = Extensions::RegisteredCoordinatorComponents();
		for (std::uint32_t c = 0; c < registered.size(); ++c)
			if (registered[c].retirement.forgot)
				registered[c].retirement.forgot({ tables, ComponentState(c), *this, c }, a_geometry);
	}

	void Coordinator::ComponentRecycled(std::uint32_t a_component, std::uint32_t a_first, std::uint32_t a_count)
	{
		const auto registered = Extensions::RegisteredCoordinatorComponents();
		if (a_component < registered.size() && registered[a_component].retirement.recycled)
			registered[a_component].retirement.recycled({ tables, ComponentState(a_component), *this, a_component }, a_first, a_count);
	}

	void Coordinator::ComponentsChannelKeys(Api::ChannelApply a_apply, const EventBatch& a_batch)
	{
		const auto engineChannels = Extensions::RegisteredEngineChannels();
		const auto registered = Extensions::RegisteredCoordinatorComponents();
		for (std::uint32_t k = 0; k < engineChannels.size(); ++k) {
			if (engineChannels[k].apply != a_apply)
				continue;
			const std::span<const void* const> channelKeys = a_batch.channelKeys[k];
			for (std::uint32_t c = 0; c < registered.size(); ++c)
				if (registered[c].events.keys)
					registered[c].events.keys({ tables, ComponentState(c), *this, c }, Api::EngineChannelId{ k }, channelKeys);
		}
	}

	void Coordinator::ComponentsChannelKeys(Api::ChannelApply a_apply)
	{
		const auto engineChannels = Extensions::RegisteredEngineChannels();
		const auto registered = Extensions::RegisteredCoordinatorComponents();
		for (std::uint32_t k = 0; k < engineChannels.size(); ++k) {
			if (engineChannels[k].apply != a_apply)
				continue;
			for (std::uint32_t c = 0; c < registered.size(); ++c)
				if (registered[c].events.keys)
					registered[c].events.keys({ tables, ComponentState(c), *this, c }, Api::EngineChannelId{ k }, {});
		}
	}

	void Coordinator::SetEntryVeto(const void* a_entry, std::uint8_t a_veto)
	{
		// One byte: Hold and Observe exclude each other.
		if (a_veto != 0 && a_veto != Api::kVetoHold && a_veto != Api::kVetoObserve) {
			static std::uint32_t logged = 0;
			if (logged++ < 4)
				logger::warn("[DCLF] entry veto {:#x}: not Hold, Observe or none, ignored <- ENTRY VETO", a_veto);
			return;
		}
		const auto it = tracked.find(static_cast<RE::BSGeometry*>(const_cast<void*>(a_entry)));
		if (it == tracked.end())
			return;
		auto& entry = it->second;
		const std::uint8_t before = entry.veto;
		if (before != a_veto) {
			if (before)
				--(before == Api::kVetoHold ? vetoHeld : vetoObserved);
			if (a_veto)
				++(a_veto == Api::kVetoHold ? vetoHeld : vetoObserved);
			entry.veto = a_veto;
		}
		// Observed from here: ready only by an evaluation under this Observe (an earlier record of the slot is not this one's).
		if (a_veto == Api::kVetoObserve)
			for (const std::uint32_t slot : { entry.slot, entry.layerSlot })
				if (slot != kNoObjectSlot && slot < observedSlots.size())
					observedSlots[slot] = {};
		if (!settling)
			return;
		// In the settle, this commit: a Hold applies none to the entry's slots; an Observe dropped applies the phases the commit recorded of
		// each slot it last evaluated for the entry. Otherwise the requeued slots carry the change to the next commit.
		if (a_veto == Api::kVetoHold) {
			for (const std::uint32_t slot : { entry.slot, entry.layerSlot })
				if (slot != kNoObjectSlot)
					settleApply.emplace_back(slot, std::uint8_t{ 0 });
		} else if (!a_veto && before == Api::kVetoObserve) {
			for (const std::uint32_t slot : { entry.slot, entry.layerSlot })
				if (slot != kNoObjectSlot && slot < observedSlots.size() && observedSlots[slot].geometry == a_entry)
					settleApply.emplace_back(slot, observedSlots[slot].phases);
		}
	}

	Api::ObservedState Coordinator::ObservedSlotOf(std::uint32_t a_slot) const
	{
		return a_slot < observedSlots.size() ? observedSlots[a_slot] : Api::ObservedState{};
	}

	Api::TrackedState Coordinator::TrackedStateOf(const void* a_entry) const
	{
		const auto it = tracked.find(static_cast<RE::BSGeometry*>(const_cast<void*>(a_entry)));
		if (it == tracked.end())
			return {};
		const auto& entry = it->second;
		Api::TrackedState state;
		state.tracked = true;
		state.classified = entry.candidateFrame != 0;
		state.leafWaiting = entry.leafWaiting;
		state.slot = entry.slot;
		state.layerSlot = entry.layerSlot;
		state.reason = static_cast<std::uint8_t>(entry.candidateReason);
		state.veto = entry.veto;
		// Its slot's commit state: its base's, else its layer's.
		if (const std::uint32_t slot = entry.slot != kNoObjectSlot ? entry.slot : entry.layerSlot; slot != kNoObjectSlot) {
			state.waitCause = slot < setWaitCause.size() ? setWaitCause[slot] : std::uint8_t{ 0 };
			state.waitWhy = slot < setWaitWhy.size() ? setWaitWhy[slot] : std::uint8_t{ 0 };
			state.resident = IsResidentSlot(slot);
		}
		return state;
	}

	bool Coordinator::CommitLive() const
	{
		for (const std::uint8_t live : componentCommitLive)
			if (live)
				return true;
		return vetoHeld != 0 || vetoObserved != 0;
	}

	void Coordinator::ComponentsStacked(Api::EngineChannelId a_channel, const Api::Stacked& a_event)
	{
		const auto registered = Extensions::RegisteredCoordinatorComponents();
		for (std::uint32_t c = 0; c < registered.size(); ++c)
			if (registered[c].events.stacked)
				registered[c].events.stacked({ tables, ComponentState(c), *this, c }, a_channel, a_event);
	}

	void Coordinator::ComponentsDetached(const void* a_key, const Api::Mark& a_mark)
	{
		const auto registered = Extensions::RegisteredCoordinatorComponents();
		const std::uint32_t c = a_mark.component.index;
		if (c < registered.size() && registered[c].events.detached)
			registered[c].events.detached({ tables, ComponentState(c), *this, c }, a_key, a_mark);
	}

	void Coordinator::UnmarkDetached(const void* a_root, std::span<const void* const> a_nodes)
	{
		// A root detached loses its mark (its hide ungated), then its component hears of it (its entries leave with their own erasure).
		auto leave = [&](const void* a_key) {
			const auto at = rootMarks.find(a_key);
			if (at == rootMarks.end())
				return;
			const Api::Mark mark = at->second.mark;
			if (at->second.ownHide)
				mirror.SetGated(a_key, false);
			rootMarks.erase(at);
			ComponentsDetached(a_key, mark);
		};
		leave(a_root);
		for (const void* node : a_nodes)
			leave(node);
	}

	void Coordinator::ComponentsUnderMark(const RE::BSGeometry* a_geometry, const Api::Mark& a_mark)
	{
		const auto registered = Extensions::RegisteredCoordinatorComponents();
		const std::uint32_t c = a_mark.component.index;
		if (c < registered.size() && registered[c].events.underMark)
			registered[c].events.underMark({ tables, ComponentState(c), *this, c }, a_geometry, a_mark);
	}

	void Coordinator::ComponentsSettle(bool a_live)
	{
		const auto registered = Extensions::RegisteredCoordinatorComponents();
		for (std::uint32_t c = 0; c < registered.size(); ++c)
			if (registered[c].commit.settle)
				registered[c].commit.settle({ tables, ComponentState(c), *this, c }, a_live);
	}

	void Coordinator::ComponentsTake()
	{
		const auto registered = Extensions::RegisteredCoordinatorComponents();
		for (std::uint32_t c = 0; c < registered.size(); ++c)
			if (registered[c].commit.take)
				registered[c].commit.take({ tables, ComponentState(c), *this, c });
	}

	void Coordinator::ComponentsPublished()
	{
		const auto registered = Extensions::RegisteredCoordinatorComponents();
		for (std::uint32_t c = 0; c < registered.size(); ++c)
			if (registered[c].commit.published)
				registered[c].commit.published({ tables, ComponentState(c), *this, c });
	}

	void Coordinator::ComponentsReportText(Api::ReportPoint a_point, std::string& a_text)
	{
		// The features' text lines and the components', in registration order.
		for (const auto& reportLines : Extensions::RegisteredReportLines()) {
			if (reportLines.point != a_point)
				continue;
			if (reportLines.text)
				reportLines.text(a_text);
			else if (reportLines.componentText)
				reportLines.componentText({ tables, ComponentState(reportLines.component.index), *this, reportLines.component.index }, a_text);
		}
	}

	void Coordinator::ComponentsReportLines(Api::ReportPoint a_point, std::uint32_t a_frame)
	{
		// The features' logged lines and the components' (their text logged as one line when not empty), in registration order.
		for (const auto& reportLines : Extensions::RegisteredReportLines()) {
			if (reportLines.point != a_point)
				continue;
			if (reportLines.lines)
				reportLines.lines(a_frame);
			else if (reportLines.componentText) {
				std::string text;
				reportLines.componentText({ tables, ComponentState(reportLines.component.index), *this, reportLines.component.index }, text);
				if (!text.empty())
					logger::info("{}", text);
			}
		}
	}

	void Coordinator::ComponentsListed(RE::BSGeometry& a_geometry, const Tracked& a_tracked, Api::EntryClass a_class)
	{
		const auto registered = Extensions::RegisteredCoordinatorComponents();
		for (std::uint32_t c = 0; c < registered.size(); ++c)
			if (registered[c].entryClass.follows == a_class && registered[c].entryClass.listed) {
				const Api::EntryWrite write{ a_geometry, a_tracked.sourceRef.get() };
				registered[c].entryClass.listed({ tables, ComponentState(c), *this, c }, write);
			}
	}

	bool Coordinator::ComponentsResolve(Api::EntryWrite& a_write, Api::EntryClass a_class)
	{
		const auto registered = Extensions::RegisteredCoordinatorComponents();
		for (std::uint32_t c = 0; c < registered.size(); ++c)
			if (registered[c].entryClass.follows == a_class)
				return registered[c].entryClass.resolve && registered[c].entryClass.resolve({ tables, ComponentState(c), *this, c }, a_write);
		return false;
	}

	void Coordinator::ComponentsStreamed(std::uint32_t a_slot, Api::EntryWrite& a_write, Api::EntryClass a_class)
	{
		const auto registered = Extensions::RegisteredCoordinatorComponents();
		for (std::uint32_t c = 0; c < registered.size(); ++c)
			if (registered[c].entryClass.follows == a_class && registered[c].entryClass.streamed)
				registered[c].entryClass.streamed({ tables, ComponentState(c), *this, c }, a_slot, a_write);
	}
}

namespace DCLF::Extensions
{
	void ComponentsAdopt(std::uint32_t a_frame, const void* a_claims, std::span<const std::vector<std::shared_ptr<const void>>> a_deltas)
	{
		const auto registered = RegisteredCoordinatorComponents();
		for (std::uint32_t c = 0; c < registered.size(); ++c) {
			if (!registered[c].adoption.adopt)
				continue;
			Api::AdoptionContext context{ a_frame, a_claims };
			if (c < a_deltas.size())
				context.deltas = a_deltas[c];
			registered[c].adoption.adopt(context);
		}
	}
}
