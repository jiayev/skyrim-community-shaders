// The features' coordinator components (scene lane): their states, the residency points' calls to their hooks (Api::CoordinatorComponent),
// and the coordinator's services their context reaches (Api::CoordinatorComponentContext).

#include "Internal.h"

#include "Features/DrawcallLimitFix/Core/Extensions/Registry.h"

namespace DCLF::Api
{
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

	bool CoordinatorComponentContext::Counted(std::uint32_t a_slot) const
	{
		const auto& counted = coordinator.residentCounted;
		return a_slot < counted.size() && counted[a_slot].pipeline != Scene::CoordinatorResidents::ResidentCounted::kNone;
	}

	const SceneMirror& CoordinatorComponentContext::Mirror() const
	{
		return coordinator.mirror;
	}

	std::uint64_t CoordinatorComponentContext::PassSerial() const
	{
		return coordinator.passSerial;
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
			delta.facePublished += scheduling.events;
			delta.faceUpdated += scheduling.updated;
			delta.faceWritten += scheduling.written;
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

	void Coordinator::ComponentsForgotAll()
	{
		const auto registered = Extensions::RegisteredCoordinatorComponents();
		for (std::uint32_t c = 0; c < registered.size(); ++c)
			if (registered[c].retirement.forgotAll)
				registered[c].retirement.forgotAll({ tables, ComponentState(c), *this, c });
	}

	void Coordinator::ComponentRecycled(std::uint32_t a_component, std::uint32_t a_first, std::uint32_t a_count)
	{
		const auto registered = Extensions::RegisteredCoordinatorComponents();
		if (a_component < registered.size() && registered[a_component].retirement.recycled)
			registered[a_component].retirement.recycled({ tables, ComponentState(a_component), *this, a_component }, a_first, a_count);
	}

	void Coordinator::ComponentsListed(RE::BSGeometry& a_geometry, const Tracked& a_tracked, Api::EntryClass a_class)
	{
		const auto registered = Extensions::RegisteredCoordinatorComponents();
		for (std::uint32_t c = 0; c < registered.size(); ++c)
			if (registered[c].entryClass.follows == a_class && registered[c].entryClass.listed) {
				const Api::EntryWrite write{ a_geometry, a_tracked.faceHeadRef.get() };
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
