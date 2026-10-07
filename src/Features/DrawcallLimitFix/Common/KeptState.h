#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <vector>

// The building blocks of DCLF's persistent draw state (drawcall-limit-fix.md, "Persistent draw state"): what the tables
// log as they change, and the arrays kept across frames from them that the GPU buffers mirror. Every kept structure is
// one of these rather than its own copy of the protocol.
namespace DCLF
{
	// Whether the kept arrays reuse their storage across builds. Only the CPU tests turn it off, to compare against fresh
	// storage.
	inline bool reuseKeptStorage = true;
	/**
	 * @brief An append-only log with absolute positions: entry k is at base + k. Readers keep their own position
	 * (LogCursor); trimming the head, or invalidating the log, leaves a reader behind the base, and a reader behind the
	 * base reads everything again.
	 */
	template <class E>
	struct EventLog
	{
		std::vector<E> entries;
		std::uint64_t base = 0;

		std::uint64_t End() const { return base + entries.size(); }
		std::size_t Size() const { return entries.size(); }
		void Push(const E& a_entry) { entries.push_back(a_entry); }
		/** @brief Whether a reader at a_position can read on (it is neither behind the base nor past the end). */
		bool Readable(std::uint64_t a_position) const { return a_position >= base && a_position <= End(); }
		/** @brief The entries from a_position on (a readable position). */
		std::span<const E> From(std::uint64_t a_position) const
		{
			return std::span<const E>(entries).subspan(static_cast<std::size_t>(a_position - base));
		}
		/** @brief Every reader is to read everything again: a gap past the end leaves them all behind the base. */
		void Invalidate()
		{
			base += entries.size() + 1;
			entries.clear();
		}
		/** @brief Drops the older half once the log holds more than a_max entries (a reader still there reads everything again). */
		void Trim(std::size_t a_max)
		{
			if (entries.size() <= a_max)
				return;
			const std::size_t half = entries.size() / 2;
			entries.erase(entries.begin(), entries.begin() + static_cast<std::ptrdiff_t>(half));
			base += half;
		}
	};

	/**
	 * @brief A reader's place in an EventLog, and the tables generation it read from. It continues only in the same
	 * generation and while its position is readable; otherwise the reader rebuilds from the tables and restarts.
	 */
	struct LogCursor
	{
		bool active = false;
		std::uint32_t generation = 0;
		std::uint64_t position = 0;

		template <class E>
		bool Continues(const EventLog<E>& a_log, std::uint32_t a_generation) const
		{
			return active && generation == a_generation && a_log.Readable(position);
		}
		void Restart(std::uint32_t a_generation)
		{
			active = true;
			generation = a_generation;
		}
		template <class E>
		std::span<const E> Unread(const EventLog<E>& a_log) const
		{
			return a_log.Readable(position) ? a_log.From(position) : std::span<const E>{};
		}
		template <class E>
		void Advance(const EventLog<E>& a_log)
		{
			position = a_log.End();
		}
	};

	/**
	 * @brief What changed in a kept array, by version, for the buffers that mirror it.
	 *
	 * Each build that changes anything is one new version. The journal keeps every change after its floor as (version,
	 * range) entries, so a holder - a GPU buffer that holds some version - is sent the ranges changed since that version,
	 * coalesced; a holder below the floor (or a new buffer, which holds 0) is sent everything. One holder or several (the
	 * shadow epoch's view slots each hold their own version): BeginBuild trims what the oldest holder still counted has.
	 */
	class ChangeJournal
	{
	public:
		struct Entry
		{
			std::uint64_t version = 0;
			std::uint64_t first = 0;
			std::uint64_t count = 0;
		};

		/** @brief What a build's payload carries: the version, and the changes since the floor. */
		struct Snapshot
		{
			std::uint64_t version = 0;  // 0: not a kept array's, sent whole
			std::uint64_t floor = 0;
			std::vector<Entry> entries;

			/** @brief Whether a holder at a_held has this version. */
			bool Holds(std::uint64_t a_held) const { return version && a_held == version; }
			/**
			 * @brief The runs of a_count elements a holder at a_held lacks, coalesced and clipped: a_run(first, count). All of
			 * them when it holds nothing this journal can build on. Returns the elements sent.
			 */
			template <class Run>
			std::uint64_t ForEachRun(std::uint64_t a_held, std::uint64_t a_count, Run&& a_run) const
			{
				if (!a_count || Holds(a_held))
					return 0;
				if (!version || a_held < floor || a_held > version) {
					a_run(std::uint64_t{ 0 }, a_count);
					return a_count;
				}
				// Dense dirty object/input tables: a compact bitmap avoids sorting thousands of
				// single-element journal entries. Sparse/large byte arenas keep the range sorter.
				if (reuseKeptStorage && entries.size() >= 128 && a_count <= (1u << 20) && a_count <= entries.size() * 64) {
					std::vector<std::uint64_t> bits(static_cast<std::size_t>((a_count + 63) / 64), 0);
					for (const auto& entry : entries) {
						if (entry.version <= a_held || !entry.count || entry.first >= a_count)
							continue;
						const auto lastIndex = entry.first + std::min(entry.count, a_count - entry.first) - 1;
						const auto firstWord = entry.first / 64, lastWord = lastIndex / 64;
						const auto head = ~std::uint64_t{0} << (entry.first % 64);
						const auto tail = ~std::uint64_t{0} >> (63 - lastIndex % 64);
						if (firstWord == lastWord)
							bits[firstWord] |= head & tail;
						else {
							bits[firstWord] |= head;
							for (auto word = firstWord + 1; word < lastWord; ++word)
								bits[word] = ~std::uint64_t{0};
							bits[lastWord] |= tail;
						}
					}
					std::uint64_t first = 0, end = 0, sent = 0;
					for (std::size_t i = 0; i < bits.size(); ++i) {
						auto word = bits[i];
						while (word) {
							const auto startBit = std::countr_zero(word);
							const auto endBit = startBit + std::countr_one(word >> startBit);
							const std::uint64_t start = i * 64 + startBit;
							if (start != end) {
								if (first < end) { a_run(first, end - first); sent += end - first; }
								first = start;
							}
							end = i * 64 + endBit;
							word = endBit == 64 ? 0 : word & (~std::uint64_t{0} << endBit);
						}
					}
					if (first < end) { a_run(first, end - first); sent += end - first; }
					return sent;
				}
				std::vector<std::pair<std::uint64_t, std::uint64_t>> runs;
				runs.reserve(entries.size());
				for (const auto& entry : entries)
					if (entry.version > a_held)
						runs.emplace_back(entry.first, entry.first + entry.count);
				std::sort(runs.begin(), runs.end());
				std::uint64_t sent = 0;
				for (std::size_t k = 0; k < runs.size();) {
					const std::uint64_t first = runs[k].first;
					std::uint64_t end = runs[k].second;
					while (++k < runs.size() && runs[k].first <= end)
						end = std::max(end, runs[k].second);
					end = std::min(end, a_count);
					if (first < end) {
						a_run(first, end - first);
						sent += end - first;
					}
				}
				return sent;
			}
			void Reset() { *this = {}; }
		};

		static constexpr std::size_t kMaxEntries = 1u << 16;

		/**
		 * @brief A build's start: forgets the changes a holder at a_oldestHeld has (the oldest holder still counted), and
		 * opens the build, which takes a new version at its first change.
		 */
		void BeginBuild(std::uint64_t a_oldestHeld)
		{
			if (a_oldestHeld > floor && a_oldestHeld <= version) {
				std::erase_if(entries, [&](const Entry& a_entry) { return a_entry.version <= a_oldestHeld; });
				floor = a_oldestHeld;
			}
			if (entries.size() > kMaxEntries) {
				// A holder that fell this far behind is sent everything.
				entries.clear();
				floor = version;
			}
			changed = false;
		}
		/** @brief One element changed (once a build however often it is marked). */
		void Mark(std::uint64_t a_index)
		{
			Touch();
			if (markedAt.size() <= a_index)
				markedAt.resize(static_cast<std::size_t>(a_index) + 1, 0);
			if (markedAt[a_index] == version)
				return;
			markedAt[a_index] = version;
			entries.push_back({ version, a_index, 1 });
		}
		/** @brief A range changed. */
		void MarkRange(std::uint64_t a_first, std::uint64_t a_count)
		{
			if (!a_count)
				return;
			Touch();
			entries.push_back({ version, a_first, a_count });
		}
		/** @brief Everything changed: every holder is sent everything. The version counts on, so no old one is taken for it. */
		void Resync()
		{
			Touch();
			entries.clear();
			floor = version;
		}
		std::uint64_t Version() const { return version; }
		/** @brief Whether this build changed anything. */
		bool Changed() const { return changed; }
		Snapshot Take() const { return { version, floor, entries }; }

	private:
		void Touch()
		{
			if (!changed) {
				++version;
				changed = true;
			}
		}

		std::uint64_t version = 0;
		std::uint64_t floor = 0;
		bool changed = false;
		std::vector<Entry> entries;
		std::vector<std::uint64_t> markedAt;  // per element: the version that last marked it
	};

	/** @brief A build's view of a kept array: the elements (shared with the array, copy-on-write) and their changes. */
	template <class T>
	struct KeptView
	{
		// Shared with the array: it copies before it writes while a view holds them. Consumers must not
		// mutate the view; current-frame bindings belong in separate patch storage.
		std::shared_ptr<const std::vector<T>> elements;
		ChangeJournal::Snapshot changes;

		std::size_t Count() const { return elements ? elements->size() : 0; }
		const T* At(std::size_t a_index) const { return elements && a_index < elements->size() ? &(*elements)[a_index] : nullptr; }
		std::uint64_t Version() const { return changes.version; }
		/**
		 * @brief The uploads a holder at a_held lacks: a_emit(data, bytes, offset). Returns the bytes sent. A holder of a_capacity
		 * elements is sent none past them (they wait for its growth: it holds a_held's version of them only once it holds them all).
		 */
		template <class Emit>
		std::size_t Emit(std::uint64_t a_held, Emit&& a_emit, std::size_t a_capacity = ~std::size_t(0)) const
		{
			if (!elements)
				return 0;
			const auto* data = elements->data();
			std::size_t sent = 0;
			changes.ForEachRun(a_held, Count(), [&](std::uint64_t a_first, std::uint64_t a_count) {
				if (a_first >= a_capacity)
					return;
				a_count = std::min<std::uint64_t>(a_count, a_capacity - a_first);
				a_emit(static_cast<const void*>(data + a_first), static_cast<std::size_t>(a_count * sizeof(T)), static_cast<std::size_t>(a_first * sizeof(T)));
				sent += static_cast<std::size_t>(a_count * sizeof(T));
			});
			return sent;
		}
		void Reset() { *this = {}; }
	};

	/**
	 * @brief An array kept across builds and mirrored by GPU buffers: copy-on-write against the views the payloads hold,
	 * and journalled (ChangeJournal), so each buffer is sent what changed since the version it holds.
	 */
	template <class T>
	class KeptArray
	{
	public:
		/** @brief A build's start (ChangeJournal::BeginBuild). */
		void BeginBuild(std::uint64_t a_oldestHeld)
		{
			journal.BeginBuild(a_oldestHeld);
			copied = false;
		}
		const std::vector<T>& Get() const { return *elements; }
		std::size_t Size() const { return elements->size(); }
		/** @brief The elements to change (copied first while a view holds them); what changes is to be marked. */
		std::vector<T>& Mutable()
		{
			if (!copied) {
				if (elements.use_count() > 1) {
					if (!reuseKeptStorage) {
						elements = std::make_shared<std::vector<T>>(*elements);
						copied = true;
						return *elements;
					}
					// Reuse storage only after every snapshot has released it. Copy the whole current
					// array so a new build cannot mutate an outstanding immutable view.
					std::shared_ptr<std::vector<T>> next;
					for (auto it = spare.begin(); it != spare.end(); ++it) {
						if (it->use_count() == 1) {
							next = std::move(*it);
							spare.erase(it);
							break;
						}
					}
					if (!next)
						next = std::make_shared<std::vector<T>>();
					*next = *elements;
					if (spare.size() < 2)
						spare.push_back(std::move(elements));
					elements = std::move(next);
				}
				copied = true;
			}
			return *elements;
		}
		/** @brief Writes and marks an element only when its bytes differ; true when it did. */
		bool Set(std::size_t a_index, const T& a_value)
		{
			if (std::memcmp(&(*elements)[a_index], &a_value, sizeof(T)) == 0)
				return false;
			Mutable()[a_index] = a_value;
			journal.Mark(a_index);
			return true;
		}
		void Mark(std::size_t a_index) { journal.Mark(a_index); }
		void MarkRange(std::uint64_t a_first, std::uint64_t a_count) { journal.MarkRange(a_first, a_count); }
		void Resync() { journal.Resync(); }
		/** @brief Empty, and every holder sent everything again (the versions count on). */
		void Clear()
		{
			Mutable().clear();
			journal.Resync();
		}
		std::uint64_t Version() const { return journal.Version(); }
		bool Changed() const { return journal.Changed(); }
		KeptView<T> View() const { return { elements, journal.Take() }; }
		/** @brief The elements themselves, for a view that is not journalled (version 0: sent whole). */
		std::shared_ptr<const std::vector<T>> Shared() const { return elements; }

	private:
		std::shared_ptr<std::vector<T>> elements = std::make_shared<std::vector<T>>();
		std::vector<std::shared_ptr<std::vector<T>>> spare;
		ChangeJournal journal;
		bool copied = false;
	};

	/**
	 * @brief The versions several buffers mirroring one kept array hold (a ring of them, one per frame in flight: step 6e E), each
	 * set by the uploader that fills it, read by the array's builds for the oldest (ChangeJournal::BeginBuild). Lock-free: an
	 * uploader raises its entry once its upload is queued; a build that reads an older value only keeps more of the journal.
	 */
	template <std::size_t N>
	class KeptHolders
	{
	public:
		void Set(std::size_t a_holder, std::uint64_t a_version) { held[a_holder].store(a_version, std::memory_order_release); }
		std::uint64_t Get(std::size_t a_holder) const { return held[a_holder].load(std::memory_order_acquire); }
		/** @brief The oldest version any holder has (0 while one holds nothing: the journal is kept whole). */
		std::uint64_t Oldest() const
		{
			std::uint64_t oldest = ~std::uint64_t{ 0 };
			for (const auto& version : held)
				oldest = std::min(oldest, version.load(std::memory_order_acquire));
			return oldest;
		}
		static constexpr std::size_t Size() { return N; }

	private:
		std::array<std::atomic<std::uint64_t>, N> held{};
	};

	/**
	 * @brief A parity check's counters, for its report line: what was checked, what differed, and the first difference.
	 * Every kept structure checks itself against a rebuild from the tables (CS_DCLF_PERSISTENT_PARITY) through one.
	 */
	struct ParityCounter
	{
		std::uint64_t checks = 0, mismatches = 0;
		std::string first;

		/** @brief One comparison; a_describe() names the first difference. Returns a_equal. */
		template <class F>
		bool Check(bool a_equal, F&& a_describe)
		{
			++checks;
			if (!a_equal && mismatches++ == 0)
				first = a_describe();
			return a_equal;
		}
		bool Check(bool a_equal) { return Check(a_equal, [] { return std::string(); }); }
		/** @brief The report's verdict: " <- OK", " <- DIFFER" (with the first difference when a_first), or nothing unchecked. */
		std::string Verdict(bool a_first = false) const
		{
			if (!checks)
				return {};
			if (!mismatches)
				return " <- OK";
			return a_first ? " <- DIFFER; first: " + first : " <- DIFFER";
		}
		void Reset() { *this = {}; }
	};

	/** @brief The parity checks' schedule: every 60 frames, each check at its own offset so their rebuilds do not share a frame. */
	constexpr bool ParityDue(std::uint32_t a_frame, std::uint32_t a_offset = 0) { return a_frame % 60 == a_offset; }

	/** @brief A list of indices with a mark per index, so each is listed once. */
	struct MarkedList
	{
		std::vector<std::uint32_t> list;
		std::vector<std::uint8_t> mark;

		/** @brief Lists a_index; false when it was listed already. */
		bool Add(std::uint32_t a_index)
		{
			if (mark.size() <= a_index)
				mark.resize(std::size_t(a_index) + 1, 0);
			if (mark[a_index])
				return false;
			mark[a_index] = 1;
			list.push_back(a_index);
			return true;
		}
		bool Contains(std::uint32_t a_index) const { return a_index < mark.size() && mark[a_index]; }
		/** @brief Unlists the element at list position a_at (the last takes its place). */
		void RemoveAt(std::size_t a_at)
		{
			mark[list[a_at]] = 0;
			list[a_at] = list.back();
			list.pop_back();
		}
		void Clear()
		{
			for (const auto index : list)
				mark[index] = 0;
			list.clear();
		}
		std::size_t Size() const { return list.size(); }
		bool Empty() const { return list.empty(); }
	};
}
