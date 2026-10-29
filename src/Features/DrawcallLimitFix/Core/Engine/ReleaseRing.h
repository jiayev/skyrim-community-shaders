#pragma once

// The scene work's engine references let go of, held until no frame of the release horizon names them (EngineReleases). Scene lane only.

#include <cstdint>
#include <deque>
#include <memory>
#include <utility>
#include <vector>

#include "Features/DrawcallLimitFix/Core/Engine/EngineReleases.h"

namespace DCLF
{
	/**
	 * @brief Engine references by the first publication that no longer names them: released (EngineReleases, at Present) once every
	 * frame of the release horizon installed that publication or a later one. Readers hold none of them (dclf-scene-lifetime-survey.md,
	 * section 12): what a publication names lives while a frame may read it, and nothing a reader keeps holds it longer.
	 *
	 * Open collects what is let go of before the next tables publication, which stamps it (Close); a holder that knows its publication
	 * stamps its own (Retire: the candidate tables' entry roots).
	 */
	class ReleaseRing
	{
	public:
		struct Batch
		{
			std::vector<RE::NiPointer<RE::NiRefObject>> references;
			std::vector<std::shared_ptr<void>> owners;  // whose last release runs the engine's destructors (event batches, material references)
			bool Empty() const { return references.empty() && owners.empty(); }
			std::size_t Size() const { return references.size() + owners.size(); }
		};

		/** @brief What is let go of now: named by the publications made so far. */
		Batch& Open() { return open; }
		/** @brief The open batch stamped with a_publication, the first publication that does not name it. */
		void Close(std::uint64_t a_publication)
		{
			if (!open.Empty())
				Stamp(a_publication, std::exchange(open, Batch{}));
		}
		/** @brief References a_publication is the first publication not to name. */
		void Retire(std::uint64_t a_publication, std::vector<RE::NiPointer<RE::NiRefObject>>&& a_references)
		{
			if (a_references.empty())
				return;
			Batch batch;
			batch.references = std::move(a_references);
			Stamp(a_publication, std::move(batch));
		}
		/** @brief The batches the horizon passed, to EngineReleases. Returns how many references went. */
		std::size_t Drain()
		{
			const std::uint64_t through = EngineReleases::Releasable();
			std::size_t released = 0;
			while (!stamped.empty() && stamped.front().first <= through) {
				auto& batch = stamped.front().second;
				released += batch.Size();
				EngineReleases::NoteReleased(stamped.front().first);
				for (auto& reference : batch.references)
					EngineReleases::Push(std::move(reference));
				for (auto& owner : batch.owners)
					EngineReleases::PushOwner(std::move(owner));
				pending -= batch.Size();
				stamped.pop_front();
			}
			return released;
		}
		/** @brief Everything to EngineReleases at once: nothing reads any publication any more (a teardown). */
		void ReleaseAll()
		{
			Close(0);
			for (auto& [stamp, batch] : stamped) {
				for (auto& reference : batch.references)
					EngineReleases::Push(std::move(reference));
				for (auto& owner : batch.owners)
					EngineReleases::PushOwner(std::move(owner));
			}
			stamped.clear();
			pending = 0;
		}

		/** @brief The references waiting on the horizon, and the oldest stamp among them (0: none). */
		std::size_t Pending() const { return pending; }
		std::uint64_t OldestStamp() const { return stamped.empty() ? 0 : stamped.front().first; }

	private:
		void Stamp(std::uint64_t a_publication, Batch&& a_batch)
		{
			pending += a_batch.Size();
			// Stamps come in publication order; one behind the newest waits with it.
			if (!stamped.empty() && a_publication < stamped.back().first)
				a_publication = stamped.back().first;
			if (!stamped.empty() && stamped.back().first == a_publication) {
				auto& into = stamped.back().second;
				into.references.insert(into.references.end(), std::make_move_iterator(a_batch.references.begin()), std::make_move_iterator(a_batch.references.end()));
				into.owners.insert(into.owners.end(), std::make_move_iterator(a_batch.owners.begin()), std::make_move_iterator(a_batch.owners.end()));
				return;
			}
			stamped.emplace_back(a_publication, std::move(a_batch));
		}

		Batch open;
		std::deque<std::pair<std::uint64_t, Batch>> stamped;
		std::size_t pending = 0;
	};
}
