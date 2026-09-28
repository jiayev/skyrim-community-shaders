#include "Internal.h"

#include "Features/DrawcallLimitFix/Common/AsyncWorker.h"

namespace DCLF
{
	void SceneStore::QueuePlacement(RE::BSGeometry* a_geometry, Tracked& a_tracked, std::uint8_t a_take)
	{
		// The sun entry node is resolved here, once per entry, so the job only reads its bound.
		ResolveSunEntry(a_tracked, *a_geometry);
		placements.push_back({ a_geometry, &a_tracked, a_tracked.sunEntryNode, a_tracked.slot, a_take });
		a_tracked.movedWalk = walkSerial;
		++delta.moved;
	}

	std::uint8_t SceneStore::TakePlacement(const Placement& a_item)
	{
		// What WriteObject writes from the geometry's placement and its skin's palette, the same way, into the kept
		// record's own fields only: the render thread may touch the record's other fields while this runs.
		std::uint8_t changed = 0;
		auto* geometry = a_item.geometry;
		const std::uint32_t slot = a_item.slot;
		if (a_item.take & kTakePalette) {
			auto* skin = geometry->GetGeometryRuntimeData().skinInstance.get();
			UpdateSkin(skin, geometry->world);
			const std::uint32_t rows = skin->numMatrices * 3;
			const auto* current = static_cast<const float*>(skin->boneMatrices);
			const auto* previous = static_cast<const float*>(skin->prevBoneMatrices);
			if (!current || !previous || rows != tables.boneRows[slot]) {
				changed |= kTakeDefect;
			} else {
				// The rows into the skin's own block, noted only when they differ.
				const std::size_t at = std::size_t(tables.boneOffset[slot]) * 4;
				const std::size_t bytes = std::size_t(rows) * 4 * sizeof(float);
				if (std::memcmp(&tables.bones[at], current, bytes) != 0 || std::memcmp(&tables.previousBones[at], previous, bytes) != 0) {
					std::memcpy(&tables.bones[at], current, bytes);
					std::memcpy(&tables.previousBones[at], previous, bytes);
					changed |= kTakePalette;
				}
			}
		}
		if (a_item.take & kTakePlacement) {
			float world[12];
			float previousWorld[12];
			StoreTransform(geometry->world, world);
			StoreTransform(geometry->previousWorld, previousWorld);
			const float center[3]{ geometry->worldBound.center.x, geometry->worldBound.center.y, geometry->worldBound.center.z };
			const float radius = geometry->worldBound.radius;
			// SunEntryOf, for a resolved entry.
			std::array<float, 4> entry{ 0.0f, 0.0f, 0.0f, -1.0f };
			if (a_item.sunEntryNode) {
				const auto& bound = a_item.sunEntryNode->worldBound;
				entry = { bound.center.x, bound.center.y, bound.center.z, bound.radius };
			}
			auto& object = tables.objects[slot];
			// Only a real change is one: a mover that stood still this frame changes nothing.
			if (std::memcmp(object.world, world, sizeof(world)) != 0 || std::memcmp(object.previousWorld, previousWorld, sizeof(previousWorld)) != 0 ||
				std::memcmp(object.boundCenter, center, sizeof(center)) != 0 || object.boundRadius != radius || tables.sunEntry[slot] != entry) {
				std::memcpy(object.world, world, sizeof(world));
				std::memcpy(object.previousWorld, previousWorld, sizeof(previousWorld));
				std::memcpy(object.boundCenter, center, sizeof(center));
				object.boundRadius = radius;
				tables.sunEntry[slot] = entry;
				changed |= kTakePlacement;
			}
		}
		return changed;
	}

	void SceneStore::RunPlacements()
	{
		const auto count = static_cast<std::uint32_t>(placements.size());
		for (std::uint32_t i = placementsDone.load(std::memory_order_acquire); i < count; ++i) {
			placementChanges[i] = TakePlacement(placements[i]);
			placementsDone.store(i + 1, std::memory_order_release);
		}
	}

	void SceneStore::KickPlacements()
	{
		DCLF_SCENE_PART(Placements, "CS.DCLF.Scene.Placements");
		// An entry a later round wrote in full (or released) took its placement and palette there.
		std::erase_if(placements, [&](const Placement& a_item) { return a_item.tracked->movedWalk != walkSerial || a_item.tracked->slot != a_item.slot; });
		placementChanges.assign(placements.size(), 0);
		placementsDone.store(0, std::memory_order_relaxed);
		if (placements.empty())
			return;
		// The walk parity reads every record right after the walk, so its frames take them here.
		const bool inline_ = !AsyncEnabled() || (SwitchEnabled(Switch::WalkParity) && ParityDue(frame));
		if (inline_) {
			RunPlacements();
			ApplyPlacements(false);
			return;
		}
		placementJob = std::static_pointer_cast<void>(std::make_shared<AsyncWorker::JobHandle>(
			AsyncWorker::Get().Submit("scene placement", [this](std::stop_token) { RunPlacements(); })));
	}

	void SceneStore::JoinPlacements()
	{
		if (!placementJob && placements.empty())
			return;
		DCLF_SCENE_PART(PlacementJoin, "CS.DCLF.Scene.PlacementJoin");
		bool failed = false;
		if (auto job = std::static_pointer_cast<AsyncWorker::JobHandle>(std::exchange(placementJob, nullptr))) {
			auto& worker = AsyncWorker::Get();
			const auto result = worker.Wait(*job, AsyncWaitBudget());
			// Late: waited for when running, dropped when still queued; what it did not take is taken below.
			if (result != AsyncWorker::WaitResult::Done)
				worker.Cancel(*job);
			failed = result == AsyncWorker::WaitResult::Failed;
			placementStats.late += result == AsyncWorker::WaitResult::Late ? 1 : 0;
		}
		const std::uint32_t resumeAt = placementsDone.load(std::memory_order_acquire);
		placementStats.inlineItems += placements.size() - resumeAt;
		RunPlacements();
		// An item the job threw in may be half taken, so it and the rest are noted in full.
		if (failed)
			for (std::size_t i = resumeAt; i < placementChanges.size(); ++i)
				placementChanges[i] |= kTakePlacement | kTakePalette;
		ApplyPlacements(AsyncModeSetting() == AsyncMode::Probe);
	}

	std::string SceneStore::PlacementReport()
	{
		auto& p = placementStats;
		if (!p.items)
			return {};
		auto line = fmt::format("[DCLF] scene placement: {} items, {} taken inline ({} joins late), {} palette size defects", p.items, p.inlineItems, p.late, p.defects);
		if (p.probes)
			line += fmt::format("; probe: {} joins, {} items moved between the job and the join{}{}", p.probes, p.probeMoved,
				p.probeMoved ? " <- MOVED; first: " : " <- OK", p.firstMoved);
		p = {};
		return line;
	}

	void SceneStore::ApplyPlacements(bool a_probe)
	{
		for (std::size_t i = 0; i < placements.size(); ++i) {
			const std::uint8_t changed = placementChanges[i];
			const std::uint32_t slot = placements[i].slot;
			if (changed & kTakePlacement) {
				tables.NoteChange(slot, kChangePlacement);
				++stats.lightPlacedChanged;
			}
			if (changed & kTakePalette) {
				tables.NoteChange(slot, kChangePalette);
				++stats.lightSkinsChanged;
			}
			if (changed & kTakeDefect) {
				// The palette's size moved under a kept record: the next walk writes it in full.
				tables.NoteChange(slot, kChangePalette | kChangeSkin);
				pendingEvaluation.push_back(placements[i].geometry);
				if (!placementStats.defects++)
					logger::warn("[DCLF] scene placement: the palette of '{}' changed size under its kept record", placements[i].geometry->name.c_str() ? placements[i].geometry->name.c_str() : "?");
			}
		}
		// CS_DCLF_ASYNC=probe: every item taken again now. Taking is idempotent, so anything it changes moved between the
		// job's read and this join: an engine writer inside the window the job assumes is quiet.
		if (a_probe) {
			++placementStats.probes;
			for (const auto& item : placements) {
				const std::uint8_t moved = TakePlacement(item) & (kTakePlacement | kTakePalette);
				if (!moved)
					continue;
				tables.NoteChange(item.slot, (moved & kTakePlacement ? kChangePlacement : 0u) | (moved & kTakePalette ? kChangePalette : 0u));
				if (!placementStats.probeMoved++)
					placementStats.firstMoved = item.geometry->name.c_str() ? item.geometry->name.c_str() : "?";
			}
		}
		placementStats.items += placements.size();
		placements.clear();
		placementChanges.clear();
		placementsDone.store(0, std::memory_order_relaxed);
	}
}
