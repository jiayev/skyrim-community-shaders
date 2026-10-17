// The entry exclusions' build (ExclusionBuild.h): the verdicts kept by what moved, their stamped words and forward links, and the caches
// by mode. Snapshot builder, on DCLF's pool.

#include "Features/DrawcallLimitFix/Features/Shadows/Local/ExclusionBuild.h"

#include "Features/DrawcallLimitFix/Core/Handoff/ShadowViews.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Tables.h"

#include <ankerl/unordered_dense.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <numeric>
#include <span>
#include <Tracy/Tracy.hpp>

#include "Features/DrawcallLimitFix/Diagnostics/Draws/Events.h"
#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"
#include "Features/DrawcallLimitFix/Diagnostics/Policy.h"

namespace DCLF::Features::Shadows::Local
{
	/**
	 * @brief What BuildSunExclusion keeps in a SunExclusionCache (its index) to update its verdicts by what moved instead of judging every
	 * object again: the objects indexed by their geometry (an entry's verdict is read from the objects of its geometries), the mode's
	 * inputs, the exclusion the verdicts were published in (published again while nothing moved), the stamped words every version
	 * shares (SunExclusion), and the ends of the forward chains (ForwardExclusion). The snapshot builder's thread only (one build
	 * at a time).
	 */
	struct SunExclusionIndex
	{
		static constexpr std::uint32_t kNone = ~0u;
		std::shared_ptr<SunExclusion> last;
		// Per object: the geometry it is indexed under (null: free, or none) and the next object indexed under it.
		std::vector<const RE::BSGeometry*> objectGeometry;
		std::vector<std::uint32_t> nextObject;
		ankerl::unordered_dense::map<const RE::BSGeometry*, std::uint32_t> firstObject;
		// The mode's inputs as of the last build, per object: kInput, of them kRegion (the region's, kept by the kept build's moves) and
		// kList (the frame's list's: a few, read again whenever the inputs may have moved), kScratch a build's; and the list's ones.
		static constexpr std::uint8_t kInput = 1, kScratch = 2, kRegion = 4, kList = 8;
		std::vector<std::uint8_t> inputMark;
		std::vector<std::uint32_t> faces;
		std::uint32_t modeBits = 0;
		// A build's scratch: the objects it looks at again, the entries it judges again, the inputs it looks at again.
		std::vector<std::uint8_t> objectMark, entryMark;
		std::vector<std::uint32_t> objects, entries, changed, moved;
		// The stamped words, grown by replacing them (the versions made before hold theirs).
		std::shared_ptr<std::atomic<std::uint32_t>[]> removed, cleared;
		std::shared_ptr<std::atomic<std::uint64_t>[]> lightReach;
		std::uint32_t removedCount = 0, clearedCount = 0;
		// Every exclusion made for the last candidates the mode was built for whose chain is still held (each link is held by the one
		// before it; the first by whoever holds the built exclusion): translated to the next candidates when they move.
		std::vector<std::weak_ptr<SunExclusion>> ends;
		bool valid = false;

		void Link(std::uint32_t a_object, const RE::BSGeometry* a_geometry)
		{
			objectGeometry[a_object] = a_geometry;
			if (!a_geometry)
				return;
			auto [it, fresh] = firstObject.try_emplace(a_geometry, a_object);
			nextObject[a_object] = fresh ? kNone : std::exchange(it->second, a_object);
		}
		void Unlink(std::uint32_t a_object)
		{
			const auto* geometry = std::exchange(objectGeometry[a_object], nullptr);
			if (!geometry)
				return;
			const auto it = firstObject.find(geometry);
			if (it == firstObject.end())
				return;
			if (it->second == a_object) {
				if (nextObject[a_object] == kNone)
					firstObject.erase(it);
				else
					it->second = nextObject[a_object];
				return;
			}
			for (std::uint32_t o = it->second; nextObject[o] != kNone; o = nextObject[o])
				if (nextObject[o] == a_object) {
					nextObject[o] = nextObject[a_object];
					return;
				}
		}
		/** @brief Every object indexed again from the tables. */
		void Reindex(const SceneTables& a_tables)
		{
			const std::size_t count = a_tables.objects.size();
			firstObject.clear();
			objectGeometry.assign(count, nullptr);
			nextObject.assign(count, kNone);
			for (std::uint32_t o = 0; o < count; ++o)
				if (!(a_tables.objects[o].flags & kObjectFree))
					Link(o, a_tables.objectGeometry[o]);
		}
		void Grow(std::size_t a_objects, std::size_t a_entries)
		{
			if (objectGeometry.size() < a_objects) {
				objectGeometry.resize(a_objects, nullptr);
				nextObject.resize(a_objects, kNone);
			}
			if (inputMark.size() < a_objects)
				inputMark.resize(a_objects, 0);
			if (objectMark.size() < a_objects)
				objectMark.resize(a_objects, 0);
			if (entryMark.size() < a_entries)
				entryMark.resize(a_entries, 0);
		}
		/** @brief The stamped words for a_entries entries and a_geometries geometries (a_reach: with the reach words). */
		void EnsureStamps(std::uint32_t a_entries, std::uint32_t a_geometries, bool a_reach)
		{
			if (!removed || removedCount < a_entries || (a_reach && !lightReach)) {
				const std::uint32_t n = std::max(a_entries, removedCount + removedCount / 2);
				removed = std::make_shared<std::atomic<std::uint32_t>[]>(n);
				lightReach = a_reach ? std::make_shared<std::atomic<std::uint64_t>[]>(std::size_t(n) * 3) : nullptr;
				removedCount = n;
			}
			if (!cleared || clearedCount < a_geometries) {
				const std::uint32_t n = std::max(a_geometries, clearedCount + clearedCount / 2);
				cleared = std::make_shared<std::atomic<std::uint32_t>[]>(n);
				clearedCount = n;
			}
		}
		void GiveStamps(SunExclusion& a_exclusion) const
		{
			a_exclusion.removed = removed;
			a_exclusion.cleared = cleared;
			a_exclusion.lightReach = lightReach;
			a_exclusion.removedCount = removedCount;
			a_exclusion.clearedCount = clearedCount;
		}
	};

	namespace
	{
		/** @brief The cache's index, made with its first build. */
		SunExclusionIndex& IndexOf(SunExclusionCache& a_cache)
		{
			if (!a_cache.index)
				a_cache.index = std::make_shared<SunExclusionIndex>();
			return *a_cache.index;
		}

		/**
		 * @brief An object blocks its entry when the engine would cast it here and this build does not draw it (volumetric-only too: a
		 * paraboloid light registers them). A point light's registration also sets its activeLightMask bit on every geometry its
		 * cull reaches; the Point source's culls (Features/Shadows/Local) write those for a skipped entry themselves. A fading object (ShadowReject::Faded) counts as a
		 * caster: the exclusion is used a frame later and the culls update the fade themselves (BSFadeNode::OnVisible).
		 */
		bool EngineCaster(const SceneTables& a_tables, std::size_t a_object)
		{
			return !(a_tables.objects[a_object].flags & kObjectNoShadow) ||
			       (a_object < a_tables.shadowReject.size() && a_tables.shadowReject[a_object] == static_cast<std::uint8_t>(ShadowReject::Faded));
		}

		/** @brief The mode's inputs: a_mark |= a_bit for each (a_list: each once). */
		void CollectInputs(const Api::ShadowExclusionBuild& a_build, std::vector<std::uint8_t>& a_mark, std::uint8_t a_bit, std::vector<std::uint32_t>& a_list)
		{
			a_build.ForEachInput([&](const Draws::DrawInput& a_input) {
				const std::uint32_t o = a_input.objectIndex;
				if (o < a_mark.size() && !(a_mark[o] & a_bit)) {
					a_mark[o] |= a_bit;
					a_list.push_back(o);
				}
			});
		}

		/** @brief Every entry judged from every object (the reference the kept verdicts are checked against). a_isInput: per object. */
		void JudgeAll(const EntrySpace& a_candidates, const SceneTables& a_tables, const std::vector<std::uint8_t>& a_isInput, std::uint8_t a_bit,
			std::vector<std::uint8_t>& a_out)
		{
			// Every live entry excluded unless a caster under it is not drawn; a free index never.
			const std::size_t count = a_candidates.Capacity();
			a_out.assign(count, 0);
			for (std::size_t e = 0; e < count; ++e)
				a_out[e] = a_candidates.entryNodes[e] ? 1 : 0;
			for (std::size_t o = 0; o < a_tables.objects.size(); ++o) {
				if (a_tables.objects[o].flags & kObjectFree)
					continue;
				if ((o < a_isInput.size() && (a_isInput[o] & a_bit)) || !EngineCaster(a_tables, o))
					continue;
				if (const auto it = a_candidates.geometries.find(a_tables.objectGeometry[o]); it != a_candidates.geometries.end())
					a_out[a_candidates.geometryEntry[it->second]] = 0;
			}
		}

		/**
		 * @brief The forward links: every chain end made for other candidates translated to a_built's (TranslateExclusion, sharing its
		 * stamped words), so a reader holding an older verdict takes the one for its frame's candidates by following links
		 * (ForwardExclusion) instead of translating. The ends whose chain nobody holds any more are dropped.
		 */
		void ForwardEnds(SunExclusionIndex& a_index, const std::shared_ptr<SunExclusion>& a_built, bool a_paraboloid)
		{
			std::vector<std::weak_ptr<SunExclusion>> ends;
			ends.reserve(a_index.ends.size() + 1);
			bool listed = false;
			for (const auto& weak : a_index.ends) {
				auto end = weak.lock();
				if (!end || end->forward.load(std::memory_order_relaxed))
					continue;
				listed |= end == a_built;
				if (end->candidates == a_built->candidates) {
					ends.push_back(end);
					continue;
				}
				// The paraboloid's translation keeps the version (the point-light culls' filter stands); the sun's takes a new one when an
				// entry leaves (the scene lists' filter is judged again), as the frame's translation did.
				auto next = TranslateExclusion(end, a_built->candidates, !a_paraboloid, a_paraboloid, &a_index.changed, a_built.get());
				next->forwardChanged = a_index.changed;
				end->forwardOwned = next;
				end->forward.store(next.get(), std::memory_order_release);
				ends.push_back(next);
			}
			if (!listed)
				ends.push_back(a_built);
			a_index.ends = std::move(ends);
		}

		/** @brief The builds' versions (translated copies count apart: TranslateExclusion). */
		std::uint64_t NextExclusionVersion()
		{
			static std::atomic<std::uint64_t> versions{ 0 };
			return versions.fetch_add(1, std::memory_order_relaxed) + 1;
		}

		/** @brief BuildSunExclusion without a cache: judged whole, with words of its own. */
		std::shared_ptr<SunExclusion> JudgeUncached(const Api::ShadowExclusionBuild& a_build, std::uint32_t a_count, std::uint32_t a_geometryCount, bool a_paraboloid)
		{
			auto exclusion = std::make_shared<SunExclusion>();
			exclusion->candidates = a_build.candidates;
			exclusion->builtFrame = a_build.frameNumber;
			std::vector<std::uint8_t> isInput(a_build.tables.objects.size(), 0);
			std::vector<std::uint32_t> list;
			CollectInputs(a_build, isInput, 1, list);
			JudgeAll(*a_build.candidates, a_build.tables, isInput, 1, exclusion->excluded);
			exclusion->excludedCount = static_cast<std::uint32_t>(std::count(exclusion->excluded.begin(), exclusion->excluded.end(), std::uint8_t(1)));
			exclusion->version = NextExclusionVersion();
			exclusion->removed = std::make_shared<std::atomic<std::uint32_t>[]>(a_count);
			exclusion->cleared = std::make_shared<std::atomic<std::uint32_t>[]>(a_geometryCount);
			if (a_paraboloid)
				exclusion->lightReach = std::make_shared<std::atomic<std::uint64_t>[]>(std::size_t(a_count) * 3);
			exclusion->removedCount = a_count;
			exclusion->clearedCount = a_geometryCount;
			return exclusion;
		}

		/** @brief A cached BuildSunExclusion's state shared by its stages. */
		struct SunExclusionBuild
		{
			const std::shared_ptr<const EntrySpace>& candidatesRef;
			const EntrySpace& candidates;
			const Api::ShadowExclusionBuild& build;
			const SceneTables& tables;
			SunExclusionCache& c;
			SunExclusionIndex& x;
			std::uint32_t count;
			std::size_t objects;
			std::uint64_t membership;
			std::uint32_t modeBits;

			bool IsBlocking(std::uint32_t a_object) const
			{
				return a_object < objects && !(tables.objects[a_object].flags & kObjectFree) && !(x.inputMark[a_object] & 1) && EngineCaster(tables, a_object);
			}
			void NoteEntry(std::uint32_t a_entry)
			{
				if (a_entry < count && !x.entryMark[a_entry]) {
					x.entryMark[a_entry] = 1;
					x.entries.push_back(a_entry);
				}
			}
			void NoteGeometry(const RE::BSGeometry* a_geometry)
			{
				if (!a_geometry)
					return;
				if (const auto it = candidates.geometries.find(a_geometry); it != candidates.geometries.end())
					NoteEntry(candidates.geometryEntry[it->second]);
			}
		};

		/** @brief The objects the change log moved: their old and new geometries' entries noted, the index relinked. */
		void NoteChangedObjects(SunExclusionBuild& b)
		{
			auto& x = b.x;
			constexpr std::uint32_t causes = kChangeBindings | kChangeShadow | kChangeGeometry;
			for (const auto& change : b.c.cursor.Unread(b.tables.changeLog))
				if ((change.causes & causes) && change.slot < b.objects && !x.objectMark[change.slot]) {
					x.objectMark[change.slot] = 1;
					x.objects.push_back(change.slot);
				}
			for (const std::uint32_t o : x.objects) {
				x.objectMark[o] = 0;
				const RE::BSGeometry* geometry = (b.tables.objects[o].flags & kObjectFree) ? nullptr : b.tables.objectGeometry[o];
				b.NoteGeometry(x.objectGeometry[o]);
				if (geometry != x.objectGeometry[o]) {
					x.Unlink(o);
					x.Link(o, geometry);
				}
				b.NoteGeometry(geometry);
			}
			x.objects.clear();
		}

		/** @brief The input marks of a_moved settled (kInput from kRegion, kList; kScratch cleared): each that joined or left noted (a_note). */
		void SettleInputs(SunExclusionBuild& b, std::span<const std::uint32_t> a_moved, bool a_note)
		{
			auto& x = b.x;
			constexpr std::uint8_t kHeld = SunExclusionIndex::kRegion | SunExclusionIndex::kList;
			for (const std::uint32_t o : a_moved) {
				auto& mark = x.inputMark[o];
				const bool was = (mark & SunExclusionIndex::kInput) != 0, now = (mark & kHeld) != 0;
				if (a_note && was != now && o < x.objectGeometry.size())
					b.NoteGeometry(x.objectGeometry[o]);
				mark = static_cast<std::uint8_t>((mark & kHeld) | (now ? SunExclusionIndex::kInput : 0));
			}
		}

		/**
		 * @brief The mode's inputs read whole: the region's and the list's marks set again from the build's inputs, every object's
		 * settled (O(objects): a new cache, or one that missed a kept build). a_note: the geometries of those that joined or left noted.
		 */
		void CollectWhole(SunExclusionBuild& b, bool a_note)
		{
			auto& x = b.x;
			auto& mark = x.inputMark;
			for (auto& m : mark)
				m = (m & SunExclusionIndex::kInput) ? SunExclusionIndex::kInput : std::uint8_t{ 0 };
			x.faces.clear();
			b.build.ForEachRegionInput([&](const Draws::DrawInput& a_input) {
				if (const std::uint32_t o = a_input.objectIndex; o < mark.size())
					mark[o] |= SunExclusionIndex::kRegion;
			});
			b.build.ForEachListInput([&](const Draws::DrawInput& a_input) {
				if (const std::uint32_t o = a_input.objectIndex; o < mark.size() && !(mark[o] & SunExclusionIndex::kList)) {
					mark[o] |= SunExclusionIndex::kList;
					x.faces.push_back(o);
				}
			});
			auto& all = x.moved;
			all.resize(mark.size());
			std::iota(all.begin(), all.end(), 0u);
			SettleInputs(b, all, a_note);
			all.clear();
		}

		/**
		 * @brief The mode's inputs by the kept build's moves (the region's joined and left, since the build the cache read last) and its
		 * list's again (a few): only the objects they name are looked at; those that joined or left have their geometries noted.
		 */
		void ApplyMovedInputs(SunExclusionBuild& b)
		{
			auto& x = b.x;
			auto& mark = x.inputMark;
			auto& moved = x.moved;
			const auto look = [&](std::uint32_t a_object) {
				if (a_object >= mark.size())
					return false;
				if (!(mark[a_object] & SunExclusionIndex::kScratch)) {
					mark[a_object] |= SunExclusionIndex::kScratch;
					moved.push_back(a_object);
				}
				return true;
			};
			// An object joins or leaves at most once a build: the lists are disjoint.
			for (const std::uint32_t o : b.build.left)
				if (look(o))
					mark[o] &= static_cast<std::uint8_t>(~SunExclusionIndex::kRegion);
			for (const std::uint32_t o : b.build.joined)
				if (look(o))
					mark[o] |= SunExclusionIndex::kRegion;
			for (const std::uint32_t o : x.faces)
				if (look(o))
					mark[o] &= static_cast<std::uint8_t>(~SunExclusionIndex::kList);
			x.faces.clear();
			b.build.ForEachListInput([&](const Draws::DrawInput& a_input) {
				const std::uint32_t o = a_input.objectIndex;
				if (look(o) && !(mark[o] & SunExclusionIndex::kList)) {
					mark[o] |= SunExclusionIndex::kList;
					x.faces.push_back(o);
				}
			});
			SettleInputs(b, moved, true);
			moved.clear();
		}

		/**
		 * @brief The mode's inputs again: the geometries of the objects that joined or left them noted. By the kept build's moves when the
		 * cache read the inputs as of the build before, else read whole (CollectWhole).
		 */
		void NoteMovedInputs(SunExclusionBuild& b)
		{
			ZoneScopedN("CS.DCLF.BuildSunExclusion.Inputs");
			auto& x = b.x;
			const auto& build = b.build;
			if (b.membership && build.movesValid && build.keptBuild && b.c.keptBuild + 1 == build.keptBuild && b.modeBits == x.modeBits)
				ApplyMovedInputs(b);
			else
				CollectWhole(b, true);
			x.modeBits = b.modeBits;
		}

		/** @brief The candidates' moved entries noted (ChangedEntriesByLog), the verdicts resized to them. */
		void NoteMovedCandidates(SunExclusionBuild& b)
		{
			auto& c = b.c;
			auto& x = b.x;
			x.changed.clear();
			ChangedEntriesByLog(c.candidates.get(), b.candidates, x.changed);
			for (const std::uint32_t e : x.changed)
				b.NoteEntry(e);
			if (c.excluded.size() > b.count)
				c.excludedCount -= static_cast<std::uint32_t>(std::count(c.excluded.begin() + b.count, c.excluded.end(), std::uint8_t(1)));
			c.excluded.resize(b.count, 0);
		}

		/** @brief Each noted entry judged again from the objects of its geometries. Returns whether a verdict flipped. */
		bool JudgeNotedEntries(SunExclusionBuild& b)
		{
			auto& c = b.c;
			auto& x = b.x;
			const auto& candidates = b.candidates;
			bool flipped = false;
			for (const std::uint32_t e : x.entries) {
				x.entryMark[e] = 0;
				std::uint8_t verdict = candidates.entryNodes[e] ? 1 : 0;
				for (std::size_t i = 0; verdict && i < candidates.entryGeometries[e].size(); ++i) {
					const auto* geometry = candidates.geometryNodes[candidates.entryGeometries[e][i]];
					const auto it = geometry ? x.firstObject.find(geometry) : x.firstObject.end();
					for (std::uint32_t o = it != x.firstObject.end() ? it->second : SunExclusionIndex::kNone; o != SunExclusionIndex::kNone; o = x.nextObject[o])
						if (b.IsBlocking(o)) {
							verdict = 0;
							break;
						}
				}
				if (c.excluded[e] != verdict) {
					c.excluded[e] = verdict;
					c.excludedCount = verdict ? c.excludedCount + 1 : c.excludedCount - 1;
					flipped = true;
				}
			}
			x.entries.clear();
			return flipped;
		}

		/** @brief The kept verdicts updated by what moved. Returns whether a verdict flipped. */
		bool UpdateKeptVerdicts(SunExclusionBuild& b, bool a_candidatesMoved)
		{
			b.x.Grow(b.objects, b.count);
			NoteChangedObjects(b);
			// The inputs, when an object may have joined or left them (the mode's membership version moved, or none is kept).
			if (!b.membership || b.membership != b.c.membership || b.modeBits != b.x.modeBits)
				NoteMovedInputs(b);
			if (a_candidatesMoved)
				NoteMovedCandidates(b);
			return JudgeNotedEntries(b);
		}

		/** @brief The kept verdicts' exclusion: the last again while nothing it was judged from moved, else a copy of the cache's. */
		std::shared_ptr<SunExclusion> KeptExclusion(SunExclusionBuild& b, bool a_flipped, bool a_candidatesMoved)
		{
			auto& c = b.c;
			auto& x = b.x;
			std::shared_ptr<SunExclusion> exclusion;
			++c.reused;
			c.translated += a_candidatesMoved ? 1 : 0;
			if (!a_flipped && !a_candidatesMoved && x.last && x.last->candidates == b.candidatesRef) {
				// Nothing it was judged from moved: the same exclusion again.
				exclusion = x.last;
				++c.same;
			} else {
				exclusion = std::make_shared<SunExclusion>();
				exclusion->candidates = b.candidatesRef;
				exclusion->builtFrame = b.build.frameNumber;
				exclusion->reused = true;
				exclusion->excluded = c.excluded;
				exclusion->excludedCount = c.excludedCount;
				exclusion->version = a_flipped ? NextExclusionVersion() : c.version;
			}
			return exclusion;
		}

		/** @brief Whole: the index, the inputs and every verdict again (a_whole: already judged when a_judgedWhole). */
		std::shared_ptr<SunExclusion> WholeExclusion(SunExclusionBuild& b, bool a_judgedWhole, std::vector<std::uint8_t>& a_whole)
		{
			auto& c = b.c;
			auto& x = b.x;
			x.Grow(b.objects, b.count);
			x.Reindex(b.tables);
			CollectWhole(b, false);
			x.modeBits = b.modeBits;
			if (!a_judgedWhole)
				JudgeAll(b.candidates, b.tables, x.inputMark, 1, a_whole);
			auto exclusion = std::make_shared<SunExclusion>();
			exclusion->candidates = b.candidatesRef;
			exclusion->builtFrame = b.build.frameNumber;
			exclusion->excludedCount = static_cast<std::uint32_t>(std::count(a_whole.begin(), a_whole.end(), std::uint8_t(1)));
			// A whole build matching the cache keeps its version.
			const bool same = c.valid && c.candidates == b.candidatesRef && c.excluded == a_whole;
			exclusion->version = same ? c.version : NextExclusionVersion();
			exclusion->excluded = a_whole;
			c.excluded = std::move(a_whole);
			c.excludedCount = exclusion->excludedCount;
			c.cursor.Restart(b.build.tablesGeneration);
			c.valid = true;
			x.valid = true;
			return exclusion;
		}
	}

	std::shared_ptr<SunExclusion> BuildSunExclusion(const Api::ShadowExclusionBuild& a_build, SunExclusionCache* a_cache)
	{
		ZoneScopedN("CS.DCLF.BuildSunExclusion");
		const auto& candidatesRef = a_build.candidates;
		const auto& tables = a_build.tables;
		if (!candidatesRef || !candidatesRef->Count())
			return nullptr;
		const auto& candidates = *candidatesRef;
		const std::uint32_t count = candidates.Capacity();
		const std::uint32_t geometryCount = candidates.GeometryCapacity();
		const bool paraboloid = a_build.Paraboloid();
		const std::size_t objects = tables.objects.size();
		if (!a_cache)
			return JudgeUncached(a_build, count, geometryCount, paraboloid);
		auto& c = *a_cache;
		auto& x = IndexOf(c);
		++c.builds;
		const auto buildStart = std::chrono::steady_clock::now();
		const std::uint64_t membership = a_build.membership;
		const std::uint32_t modeBits = a_build.modeBits;
		SunExclusionBuild b{ candidatesRef, candidates, a_build, tables, c, x, count, objects, membership, modeBits };
		// The kept verdicts follow what moved: the tables' change log (an object's flags, shadow reject or geometry: kChangeBindings,
		// kChangeShadow, kChangeGeometry), the mode's inputs (the objects that joined or left them) and the candidates (the entries
		// whose versions moved: ChangedEntriesByLog). Each entry any of them touches is judged again from the objects of its
		// geometries; the rest keep theirs. Whole only for a new cache or tables generation, or a reader behind the log's trim.
		bool kept = c.valid && x.valid && c.candidates && c.cursor.Continues(tables.changeLog, a_build.tablesGeneration);
		bool flipped = false;
		const bool candidatesMoved = c.candidates != candidatesRef;
		if (kept)
			flipped = UpdateKeptVerdicts(b, candidatesMoved);
		c.cursor.Advance(tables.changeLog);
		c.membership = membership;
		c.keptBuild = a_build.keptBuild;
		// Policy::ReuseShadowExclusion may judge every object to compare against the kept verdicts.
		std::vector<std::uint8_t> whole;
		bool judgedWhole = false;
		if (kept && !Policy::ReuseShadowExclusion(a_build.frameNumber)) {
			std::vector<std::uint8_t> isInput(objects, 0);
			std::vector<std::uint32_t> list;
			CollectInputs(a_build, isInput, 1, list);
			JudgeAll(candidates, tables, isInput, 1, whole);
			judgedWhole = true;
			const bool same = whole == c.excluded;
			if (Diagnostics::Armed<Diagnostics::ExclusionRebuilt>())
				Diagnostics::Notify(Diagnostics::ExclusionRebuilt{ paraboloid, same });
			kept = same;  // a difference is judged whole from here (and reported)
		}
		std::shared_ptr<SunExclusion> exclusion;
		if (kept)
			exclusion = KeptExclusion(b, flipped, candidatesMoved);
		else
			exclusion = WholeExclusion(b, judgedWhole, whole);
		if (exclusion != x.last) {
			x.EnsureStamps(count, geometryCount, paraboloid);
			x.GiveStamps(*exclusion);
		}
		c.candidates = candidatesRef;
		c.version = exclusion->version;
		x.last = exclusion;
		ForwardEnds(x, exclusion, paraboloid);
		const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - buildStart).count();
		c.buildMs += ms;
		c.buildMaxMs = std::max(c.buildMaxMs, ms);
		return exclusion;
	}

	SunExclusionCache& ExclusionCacheOf(Api::ShadowSource a_source)
	{
		// The Sun's, then the Point's; the build asks for no other.
		static std::array<SunExclusionCache, 2> caches;
		return caches[a_source == Api::ShadowSource::Point ? 1 : 0];
	}

	void BuildExclusion(Api::ShadowExclusionBuild& a_build)
	{
		a_build.result = BuildSunExclusion(a_build, &ExclusionCacheOf(a_build.source));
	}
}
