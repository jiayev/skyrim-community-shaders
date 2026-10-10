#include "Internal.h"

// T6b0 (CS_DCLF_TIMELINE): where an object's time goes between the engine's event and DCLF's publication. The stamps are
// Tracked's (writtenFrame, boundFrame, memberFrame), the shows the hidden events' (unhideKeys); the residue's stages are
// ClassifyResidue's. Observers only: nothing here changes what the coordinator decides.

namespace DCLF
{
	bool SceneStore::TimelineEnabled()
	{
		return SwitchEnabled(Switch::Timeline);
	}

	std::pair<std::uint32_t, std::uint32_t> SceneStore::LastShowOnChain(const RE::BSGeometry& a_geometry, const Tracked& a_entry, bool& a_hiddenNow) const
	{
		// ClassifyFrame's walk: the leaf up to its category node, both included.
		a_hiddenNow = false;
		std::pair<std::uint32_t, std::uint32_t> last{ 0, ~0u };
		for (const RE::NiAVObject* object = &a_geometry; object; object = object->parent) {
			a_hiddenNow = a_hiddenNow || HiddenForWalk(object);
			if (const auto it = unhideKeys.find(object); it != unhideKeys.end() && it->second.first >= last.first)
				last = it->second;
			if (object == a_entry.categoryNode)
				break;
		}
		return last;
	}

	void SceneStore::NoteTimelineJoin(std::uint32_t a_slot)
	{
		if (a_slot >= tables.objectGeometry.size() || tables.IsLayer(a_slot))
			return;
		auto* geometry = const_cast<RE::BSGeometry*>(tables.objectGeometry[a_slot]);
		const auto it = geometry ? tracked.find(geometry) : tracked.end();
		if (it == tracked.end() || it->second.slot != a_slot)
			return;
		auto& entry = it->second;
		bool hiddenNow = false;
		const auto [shown, site] = LastShowOnChain(*geometry, entry, hiddenNow);
		std::scoped_lock lock(residueClassesLock);
		++timelineStats.joins;
		if (!entry.memberFrame) {
			++timelineStats.firstJoins;
			++timelineStats.attachToMember[AgeBucket(frame - entry.trackedFrame)];
		}
		if (entry.writtenFrame)
			++timelineStats.writtenToMember[AgeBucket(frame - entry.writtenFrame)];
		// A show on its chain since its last join: what its join waited on.
		if (shown && shown > entry.memberFrame)
			++timelineStats.showToMember[AgeBucket(frame - shown)];
		entry.memberFrame = frame;
	}

	SceneStore::TimelineStats SceneStore::TakeTimelineStats()
	{
		std::scoped_lock lock(residueClassesLock);
		return std::exchange(timelineStats, {});
	}
}
