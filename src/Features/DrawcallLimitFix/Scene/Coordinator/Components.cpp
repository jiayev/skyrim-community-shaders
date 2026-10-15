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
			if (registered[c].resident)
				registered[c].resident({ tables, ComponentState(c), *this, c }, a_slot);
	}

	void Coordinator::ComponentsLeft(std::uint32_t a_slot)
	{
		const auto registered = Extensions::RegisteredCoordinatorComponents();
		for (std::uint32_t c = 0; c < registered.size(); ++c)
			if (registered[c].left)
				registered[c].left({ tables, ComponentState(c), *this, c }, a_slot);
	}

	void Coordinator::ComponentsRecount(bool a_rebuild, std::span<const std::uint32_t> a_touched)
	{
		const auto registered = Extensions::RegisteredCoordinatorComponents();
		const Api::ResidentRecount recount{ a_rebuild, a_touched, residents };
		for (std::uint32_t c = 0; c < registered.size(); ++c)
			if (registered[c].recount)
				registered[c].recount({ tables, ComponentState(c), *this, c }, recount);
	}

	void Coordinator::ComponentsReset()
	{
		const auto registered = Extensions::RegisteredCoordinatorComponents();
		for (std::uint32_t c = 0; c < registered.size(); ++c)
			if (registered[c].reset)
				registered[c].reset({ tables, ComponentState(c), *this, c });
	}

	void Coordinator::ComponentsAllLeft()
	{
		const auto registered = Extensions::RegisteredCoordinatorComponents();
		for (std::uint32_t c = 0; c < registered.size(); ++c)
			if (registered[c].allLeft)
				registered[c].allLeft({ tables, ComponentState(c), *this, c });
	}

	void Coordinator::ComponentsAccumulated(std::uint32_t a_slot, std::uint64_t a_chain)
	{
		const auto registered = Extensions::RegisteredCoordinatorComponents();
		const Api::AccumulatedPatch patch{ a_chain };
		for (std::uint32_t c = 0; c < registered.size(); ++c)
			if (registered[c].accumulated)
				registered[c].accumulated({ tables, ComponentState(c), *this, c }, a_slot, patch);
	}

	void Coordinator::ComponentsRewritten(std::uint32_t a_slot, bool a_written, std::uint32_t a_partitionsBefore, std::uint32_t a_partitionsNow)
	{
		const auto registered = Extensions::RegisteredCoordinatorComponents();
		const Api::SlotRewrite rewrite{ a_written, a_partitionsBefore, a_partitionsNow };
		for (std::uint32_t c = 0; c < registered.size(); ++c)
			if (registered[c].rewritten)
				registered[c].rewritten({ tables, ComponentState(c), *this, c }, a_slot, rewrite);
	}

	void Coordinator::ComponentsJoinsDone()
	{
		const auto registered = Extensions::RegisteredCoordinatorComponents();
		for (std::uint32_t c = 0; c < registered.size(); ++c)
			if (registered[c].joinsDone)
				registered[c].joinsDone({ tables, ComponentState(c), *this, c });
	}
}
