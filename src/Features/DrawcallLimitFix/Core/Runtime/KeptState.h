#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <memory>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

// The building blocks of DCLF's persistent draw state (dclf-architecture.md, "Persistent draw state"): what the tables
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
	 * @brief An EventLog whose entries sit in fixed chunks its copies share (the tables' logs, which every published snapshot holds):
	 * copying one, bringing a copy up to another (assigning) and trimming cost O(chunks), not O(entries). The entries a copy holds never
	 * change: only the copy at a chunk's end appends to it in place (its fill, claimed atomically); another copy appending there takes
	 * its own copy of the chunk first. From gives a range (LogRange) for range-for, not a span.
	 */
	template <class E>
	struct SharedEventLog
	{
		static_assert(std::is_trivially_copyable_v<E>);
		static constexpr std::size_t kChunk = 4096;
		struct Chunk
		{
			std::array<E, kChunk> entries;
			std::atomic<std::uint32_t> filled{ 0 };  // the entries appended: a copy whose end reaches it appends in place
		};
		/** @brief Entries [first, first + count) across the chunks (indices from the first chunk's start). */
		struct LogRange
		{
			const std::shared_ptr<Chunk>* chunks = nullptr;
			std::size_t first = 0, count = 0;

			struct iterator
			{
				const std::shared_ptr<Chunk>* chunks = nullptr;
				std::size_t at = 0;
				const E& operator*() const { return chunks[at / kChunk]->entries[at % kChunk]; }
				iterator& operator++()
				{
					++at;
					return *this;
				}
				bool operator==(const iterator& a_other) const { return at == a_other.at; }
			};
			iterator begin() const { return { chunks, first }; }
			iterator end() const { return { chunks, first + count }; }
			std::size_t size() const { return count; }
			bool empty() const { return count == 0; }
		};

		std::vector<std::shared_ptr<Chunk>> chunks;
		std::uint64_t base = 0;
		std::size_t head = 0;   // the base entry's index in the first chunk
		std::size_t count = 0;

		std::uint64_t End() const { return base + count; }
		std::size_t Size() const { return count; }
		void Push(const E& a_entry)
		{
			const std::size_t at = head + count;
			const std::size_t c = at / kChunk, slot = at % kChunk;
			if (c == chunks.size()) {
				chunks.push_back(std::make_shared<Chunk>());
				chunks.back()->filled.store(1, std::memory_order_relaxed);
			} else if (auto expected = static_cast<std::uint32_t>(slot);
					   !chunks[c]->filled.compare_exchange_strong(expected, static_cast<std::uint32_t>(slot + 1), std::memory_order_relaxed)) {
				// Another copy appended past this one's end here: this one's own chunk, with its entries.
				auto own = std::make_shared<Chunk>();
				std::copy_n(chunks[c]->entries.begin(), slot, own->entries.begin());
				own->filled.store(static_cast<std::uint32_t>(slot + 1), std::memory_order_relaxed);
				chunks[c] = std::move(own);
			}
			chunks[c]->entries[slot] = a_entry;
			++count;
		}
		bool Readable(std::uint64_t a_position) const { return a_position >= base && a_position <= End(); }
		LogRange From(std::uint64_t a_position) const
		{
			const auto skip = static_cast<std::size_t>(a_position - base);
			return { chunks.data(), head + skip, count - skip };
		}
		void Invalidate()
		{
			base += count + 1;
			chunks.clear();
			head = 0;
			count = 0;
		}
		/** @brief Drops the older half once the log holds more than a_max entries: whole chunks let go (a copy holding them keeps them). */
		void Trim(std::size_t a_max)
		{
			if (count <= a_max)
				return;
			const std::size_t half = count / 2;
			const std::size_t at = head + half;
			chunks.erase(chunks.begin(), chunks.begin() + static_cast<std::ptrdiff_t>(at / kChunk));
			head = at % kChunk;
			base += half;
			count -= half;
		}
		/** @brief The same base and entries (the replay parity's). */
		bool operator==(const SharedEventLog& a_other) const
		{
			if (base != a_other.base || count != a_other.count)
				return false;
			auto left = From(base), right = a_other.From(base);
			for (auto l = left.begin(), r = right.begin(); l != left.end(); ++l, ++r)
				if (std::memcmp(&*l, &*r, sizeof(E)) != 0)
					return false;
			return true;
		}
	};

	/**
	 * @brief A reader's place in an EventLog (or a SharedEventLog), and the tables generation it read from. It continues only in the
	 * same generation and while its position is readable; otherwise the reader rebuilds from the tables and restarts.
	 */
	struct LogCursor
	{
		bool active = false;
		std::uint32_t generation = 0;
		std::uint64_t position = 0;

		template <class Log>
		bool Continues(const Log& a_log, std::uint32_t a_generation) const
		{
			return active && generation == a_generation && a_log.Readable(position);
		}
		void Restart(std::uint32_t a_generation)
		{
			active = true;
			generation = a_generation;
		}
		template <class Log>
		auto Unread(const Log& a_log) const
		{
			using Range = decltype(a_log.From(position));
			return a_log.Readable(position) ? a_log.From(position) : Range{};
		}
		template <class Log>
		void Advance(const Log& a_log)
		{
			position = a_log.End();
		}
	};

	/**
	 * @brief What changed in a kept array, by content epoch, for the buffers that mirror it.
	 *
	 * Each build that changes anything is one new content epoch (a journal position). The journal keeps every change after its floor
	 * as (contentEpoch, range) entries, so a holder - a GPU buffer whose contents are at some content epoch - is sent the ranges
	 * changed since that epoch,
	 * coalesced; a holder below the floor (or a new buffer, which holds 0) is sent everything. One holder or several (the
	 * shadow epoch's view slots each hold their own content epoch): BeginBuild trims what the oldest holder still counted has.
	 */
	class ChangeJournal
	{
	public:
		struct Entry
		{
			std::uint64_t contentEpoch = 0;
			std::uint64_t first = 0;
			std::uint64_t count = 0;
		};

		/** @brief What a build's payload carries: the content epoch, and the changes since the floor. */
		struct Snapshot
		{
			std::uint64_t contentEpoch = 0;  // 0: not a kept array's, sent whole
			std::uint64_t floor = 0;
			std::vector<Entry> entries;  // in content epoch order, as the journal keeps them (ForEachRun relies on it)

			/** @brief Whether a holder at a_contentEpoch has this snapshot's contents. */
			bool Holds(std::uint64_t a_contentEpoch) const { return contentEpoch && a_contentEpoch == contentEpoch; }
			/**
			 * @brief The runs of a_count elements a holder at a_contentEpoch lacks, coalesced and clipped: a_run(first, count). All of
			 * them when it holds nothing this journal can build on. Returns the elements sent.
			 */
			template <class Run>
			std::uint64_t ForEachRun(std::uint64_t a_contentEpoch, std::uint64_t a_count, Run&& a_run) const
			{
				return Runs(contentEpoch, floor, entries, a_contentEpoch, a_count, a_run);
			}
			void Reset() { *this = {}; }
		};

		static constexpr std::size_t kMaxEntries = 1u << 16;

		/**
		 * @brief A build's start: forgets the changes a holder at a_oldestContentEpoch has (the oldest holder still counted), and
		 * opens the build, which takes a new content epoch at its first change.
		 */
		void BeginBuild(std::uint64_t a_oldestContentEpoch)
		{
			// Clamped to the current epoch: ~0 (no holder holds anything) lets every change go.
			a_oldestContentEpoch = std::min(a_oldestContentEpoch, contentEpoch);
			if (a_oldestContentEpoch > floor) {
				std::erase_if(entries, [&](const Entry& a_entry) { return a_entry.contentEpoch <= a_oldestContentEpoch; });
				floor = a_oldestContentEpoch;
			}
			if (entries.size() > kMaxEntries) {
				// A holder that fell this far behind is sent everything.
				entries.clear();
				floor = contentEpoch;
			}
			changed = false;
		}
		/** @brief One element changed (once a build however often it is marked). */
		void Mark(std::uint64_t a_index)
		{
			Touch();
			if (markedAt.size() <= a_index)
				markedAt.resize(static_cast<std::size_t>(a_index) + 1, 0);
			if (markedAt[a_index] == contentEpoch)
				return;
			markedAt[a_index] = contentEpoch;
			entries.push_back({ contentEpoch, a_index, 1 });
		}
		/** @brief A range changed. */
		void MarkRange(std::uint64_t a_first, std::uint64_t a_count)
		{
			if (!a_count)
				return;
			Touch();
			entries.push_back({ contentEpoch, a_first, a_count });
		}
		/** @brief Everything changed: every holder is sent everything. The content epoch counts on, so no old one is taken for it. */
		void Resync()
		{
			Touch();
			entries.clear();
			floor = contentEpoch;
		}
		std::uint64_t ContentEpoch() const { return contentEpoch; }
		/** @brief Whether this build changed anything. */
		bool Changed() const { return changed; }
		Snapshot Take() const { return { contentEpoch, floor, entries }; }
		/**
		 * @brief Snapshot::ForEachRun on the journal itself, nothing copied: only while no build changes it (its owner, between
		 * builds).
		 */
		template <class Run>
		std::uint64_t ForEachRun(std::uint64_t a_contentEpoch, std::uint64_t a_count, Run&& a_run) const
		{
			return Runs(contentEpoch, floor, entries, a_contentEpoch, a_count, a_run);
		}

	private:
		/** @brief ForEachRun over a journal's content epoch, floor and entries (a snapshot's or the journal's own). */
		template <class Run>
		static std::uint64_t Runs(std::uint64_t a_epoch, std::uint64_t a_floor, std::span<const Entry> a_entries, std::uint64_t a_contentEpoch,
			std::uint64_t a_count, Run&& a_run)
		{
			if (!a_count || (a_epoch && a_contentEpoch == a_epoch))
				return 0;
			if (!a_epoch || a_contentEpoch < a_floor || a_contentEpoch > a_epoch) {
				a_run(std::uint64_t{ 0 }, a_count);
				return a_count;
			}
			// The entries are in content epoch order (a build's marks follow the last build's): the holder lacks those from the first
			// newer than it on, however far back the floor keeps the rest.
			const auto newer = a_entries.subspan(static_cast<std::size_t>(std::partition_point(a_entries.begin(), a_entries.end(),
															 [&](const Entry& a_entry) { return a_entry.contentEpoch <= a_contentEpoch; }) -
														 a_entries.begin()));
			// Dense dirty tables: a bitmap avoids sorting thousands of single-element entries. Sparse or large arenas sort ranges.
			if (reuseKeptStorage && newer.size() >= 128 && a_count <= (1u << 20) && a_count <= newer.size() * 64) {
				// The thread's scratch, taken for the call (a nested call on the thread makes its own).
				auto bits = std::exchange(BitsScratch(), {});
				bits.assign(static_cast<std::size_t>((a_count + 63) / 64), 0);
				for (const auto& entry : newer) {
					if (!entry.count || entry.first >= a_count)
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
				BitsScratch() = std::move(bits);
				return sent;
			}
			auto runs = std::exchange(RunsScratch(), {});
			runs.clear();
			runs.reserve(newer.size());
			for (const auto& entry : newer)
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
			RunsScratch() = std::move(runs);
			return sent;
		}
		// Runs' per-thread scratch: no allocation per call once warm.
		static std::vector<std::uint64_t>& BitsScratch()
		{
			static thread_local std::vector<std::uint64_t> bits;
			return bits;
		}
		static std::vector<std::pair<std::uint64_t, std::uint64_t>>& RunsScratch()
		{
			static thread_local std::vector<std::pair<std::uint64_t, std::uint64_t>> runs;
			return runs;
		}

		void Touch()
		{
			if (!changed) {
				++contentEpoch;
				changed = true;
			}
		}

		std::uint64_t contentEpoch = 0;
		std::uint64_t floor = 0;
		bool changed = false;
		std::vector<Entry> entries;
		std::vector<std::uint64_t> markedAt;  // per element: the content epoch that last marked it
	};

	/**
	 * @brief The bytes of a kept array's chunk (KeptArray): a write after a view copies one chunk of this, not the array. 16 KB
	 * keeps a 1 KB material row's write at 16 rows' copy while the chunk table (copied once a build that writes) stays short.
	 */
	inline constexpr std::size_t kKeptChunkBytes = 16384;
	/** @brief A kept array's chunk length (KeptArray's kChunk): chunks of about kKeptChunkBytes, the default. */
	inline constexpr std::size_t kKeptChunkBySize = 0;
	/**
	 * @brief Or one contiguous array, copied whole by the first write of a build a view shares it with: for arrays whose users need
	 * one pointer over many elements (FrameValues' rows and palettes) or write them from many threads after one Mutable().
	 */
	inline constexpr std::size_t kKeptWhole = ~std::size_t(0);
	template <class T>
	constexpr std::size_t KeptChunkElements()
	{
		return std::bit_floor(std::max<std::size_t>(1, kKeptChunkBytes / sizeof(T)));
	}

	/** @brief What a kept array copied and reused (cumulative): a reporter diffs it between reports. */
	struct KeptCopyStats
	{
		std::uint64_t tableCopies = 0;       // chunk tables copied (the first write of a build a view shares the table with)
		std::uint64_t chunkCopies = 0;       // chunks copied (the first write to a chunk a view holds)
		std::uint64_t chunkReuses = 0;       // chunks taken from those every view had released
		std::uint64_t chunkAllocations = 0;  // chunks allocated
		std::uint64_t bytesCopied = 0;
	};

	template <class T, std::size_t kChunk>
	class KeptChunkStore;

	/**
	 * @brief A kept array's elements as views see them: fixed-size chunks (a power of two each, contiguous only within one),
	 * shared with the array and with other views; immutable once shared. Reads like a const std::vector without data():
	 * contiguous memory comes as chunk-bounded spans (ForEachSpan).
	 */
	template <class T>
	class KeptChunks
	{
	public:
		using value_type = T;
		using Chunk = std::vector<T>;

		class const_iterator
		{
		public:
			using iterator_category = std::random_access_iterator_tag;
			using value_type = T;
			using difference_type = std::ptrdiff_t;
			using pointer = const T*;
			using reference = const T&;

			const_iterator() = default;
			const_iterator(const KeptChunks* a_owner, std::size_t a_index) :
				owner(a_owner), index(a_index) {}
			reference operator*() const { return (*owner)[index]; }
			pointer operator->() const { return &(*owner)[index]; }
			reference operator[](difference_type a_n) const { return (*owner)[Moved(a_n)]; }
			const_iterator& operator++()
			{
				++index;
				return *this;
			}
			const_iterator operator++(int)
			{
				auto old = *this;
				++index;
				return old;
			}
			const_iterator& operator--()
			{
				--index;
				return *this;
			}
			const_iterator operator--(int)
			{
				auto old = *this;
				--index;
				return old;
			}
			const_iterator& operator+=(difference_type a_n)
			{
				index = Moved(a_n);
				return *this;
			}
			const_iterator& operator-=(difference_type a_n)
			{
				index = Moved(-a_n);
				return *this;
			}
			friend const_iterator operator+(const_iterator a_it, difference_type a_n) { return a_it += a_n; }
			friend const_iterator operator+(difference_type a_n, const_iterator a_it) { return a_it += a_n; }
			friend const_iterator operator-(const_iterator a_it, difference_type a_n) { return a_it -= a_n; }
			friend difference_type operator-(const const_iterator& a_left, const const_iterator& a_right)
			{
				return static_cast<difference_type>(a_left.index) - static_cast<difference_type>(a_right.index);
			}
			friend bool operator==(const const_iterator& a_left, const const_iterator& a_right) { return a_left.index == a_right.index; }
			friend auto operator<=>(const const_iterator& a_left, const const_iterator& a_right) { return a_left.index <=> a_right.index; }

		private:
			std::size_t Moved(difference_type a_n) const { return static_cast<std::size_t>(static_cast<difference_type>(index) + a_n); }

			const KeptChunks* owner = nullptr;
			std::size_t index = 0;
		};
		using iterator = const_iterator;

		KeptChunks() = default;
		explicit KeptChunks(std::uint32_t a_shift) :
			shift(a_shift) {}
		/** @brief Elements of one's own (not a kept array's) as one chunk, not copied. */
		static KeptChunks Adopt(std::shared_ptr<Chunk> a_flat)
		{
			KeptChunks out(63);
			if (a_flat) {
				out.count = a_flat->size();
				out.chunks.push_back(std::move(a_flat));
			}
			return out;
		}

		std::size_t size() const { return count; }
		bool empty() const { return !count; }
		const T& operator[](std::size_t a_index) const { return (*chunks[a_index >> shift])[a_index & Mask()]; }
		const T& back() const { return (*this)[count - 1]; }
		const_iterator begin() const { return { this, 0 }; }
		const_iterator end() const { return { this, count }; }
		std::size_t ChunkElements() const { return std::size_t{ 1 } << shift; }
		/** @brief The elements of [a_first, a_first + a_count) (clipped) as contiguous spans, each within one chunk: a_span(data, count, first). */
		template <class Span>
		void ForEachSpan(std::size_t a_first, std::size_t a_count, Span&& a_span) const
		{
			const std::size_t end = a_first + std::min(a_count, count > a_first ? count - a_first : 0);
			for (std::size_t at = a_first; at < end;) {
				const std::size_t offset = at & Mask();
				const std::size_t n = std::min(end - at, ChunkElements() - offset);
				a_span(static_cast<const T*>(chunks[at >> shift]->data() + offset), n, at);
				at += n;
			}
		}
		/** @brief A flat copy. */
		operator std::vector<T>() const
		{
			std::vector<T> out;
			out.reserve(count);
			ForEachSpan(0, count, [&](const T* a_data, std::size_t a_count, std::size_t) { out.insert(out.end(), a_data, a_data + a_count); });
			return out;
		}
		friend bool operator==(const KeptChunks& a_left, const std::vector<T>& a_right)
		{
			if (a_left.size() != a_right.size())
				return false;
			bool same = true;
			a_left.ForEachSpan(0, a_left.size(), [&](const T* a_data, std::size_t a_count, std::size_t a_at) {
				same = same && std::equal(a_data, a_data + a_count, a_right.begin() + static_cast<std::ptrdiff_t>(a_at));
			});
			return same;
		}
		friend bool operator==(const KeptChunks& a_left, const KeptChunks& a_right)
		{
			return a_left.size() == a_right.size() && std::equal(a_left.begin(), a_left.end(), a_right.begin());
		}

	private:
		template <class, std::size_t>
		friend class KeptChunkStore;

		std::size_t Mask() const { return ChunkElements() - 1; }

		std::vector<std::shared_ptr<Chunk>> chunks;  // ceil(count / ChunkElements()) of them, each ChunkElements() long (an adopted one: count)
		std::size_t count = 0;
		std::uint32_t shift = 0;
	};

	/**
	 * @brief A view's handle on a kept array's elements (shared, immutable). Reads like the shared_ptr<const std::vector> it
	 * replaced; a vector of one's own (not a kept array's) is adopted as one chunk.
	 */
	template <class T>
	class KeptElements
	{
	public:
		KeptElements() = default;
		KeptElements(std::nullptr_t) {}
		KeptElements(std::shared_ptr<const KeptChunks<T>> a_chunks) :
			chunks(std::move(a_chunks)) {}
		KeptElements(std::shared_ptr<std::vector<T>> a_flat) :
			chunks(a_flat ? std::shared_ptr<const KeptChunks<T>>(std::make_shared<KeptChunks<T>>(KeptChunks<T>::Adopt(std::move(a_flat)))) : nullptr) {}
		// Never written through: an adopted chunk belongs to no store.
		KeptElements(std::shared_ptr<const std::vector<T>> a_flat) :
			KeptElements(std::const_pointer_cast<std::vector<T>>(std::move(a_flat))) {}

		explicit operator bool() const { return chunks != nullptr; }
		const KeptChunks<T>& operator*() const { return *chunks; }
		const KeptChunks<T>* operator->() const { return chunks.get(); }
		const KeptChunks<T>* get() const { return chunks.get(); }
		void reset() { chunks.reset(); }

	private:
		std::shared_ptr<const KeptChunks<T>> chunks;
	};

	/**
	 * @brief The most bytes between two runs of one emit (KeptView::Spans) that go with them as one span: a 16-byte row's gap of 16
	 * rows costs less than another upload's bookkeeping.
	 */
	inline constexpr std::size_t kKeptMergeGapBytes = 256;

	/** @brief A build's view of a kept array: the elements (shared with the array, copy-on-write by chunk) and their changes. */
	template <class T>
	struct KeptView
	{
		// Shared with the array: it copies a chunk before it writes it while a view holds it. Consumers must not
		// mutate the view; current-frame bindings belong in separate patch storage.
		KeptElements<T> elements;
		ChangeJournal::Snapshot changes;

		std::size_t Count() const { return elements ? elements->size() : 0; }
		const T* At(std::size_t a_index) const { return elements && a_index < elements->size() ? &(*elements)[a_index] : nullptr; }
		std::uint64_t ContentEpoch() const { return changes.contentEpoch; }
		/**
		 * @brief The elements a holder at a_contentEpoch lacks (ChangeJournal::ForEachRun's runs), as chunk-bounded spans:
		 * a_span(data, count, first); runs of one chunk at most kKeptMergeGapBytes apart go as one span. None past a_capacity.
		 * Returns the elements.
		 */
		template <class Span>
		std::size_t ForEachSpan(std::uint64_t a_contentEpoch, Span&& a_span, std::size_t a_capacity = ~std::size_t(0)) const
		{
			return elements ? Spans(*elements, changes, a_contentEpoch, a_span, a_capacity) : 0;
		}
		/**
		 * @brief The uploads a holder at a_contentEpoch lacks: a_emit(data, bytes, offset), a run cut where a chunk ends. Returns the bytes
		 * sent. A holder of a_capacity elements is sent none past them (they wait for its growth: it is at a_contentEpoch for them only
		 * once it holds them all).
		 */
		template <class Emit>
		std::size_t Emit(std::uint64_t a_contentEpoch, Emit&& a_emit, std::size_t a_capacity = ~std::size_t(0)) const
		{
			return elements ? EmitSpans(*elements, changes, a_contentEpoch, a_emit, a_capacity) : 0;
		}
		void Reset() { *this = {}; }

		/**
		 * @brief ForEachSpan over any elements (KeptChunks, or a kept array's own std::vector) and changes (a snapshot, or a live
		 * ChangeJournal: KeptArray::Emit). Two runs in one chunk at most kKeptMergeGapBytes apart go as one span: the elements between
		 * them are unchanged since a_contentEpoch, so the holder has them already and ends the same, in one upload instead of two.
		 */
		template <class Elements, class Changes, class Span>
		static std::size_t Spans(const Elements& a_elements, const Changes& a_changes, std::uint64_t a_contentEpoch, Span&& a_span, std::size_t a_capacity)
		{
			const std::size_t chunk = ChunkOf(a_elements);
			struct Pending
			{
				const T* data = nullptr;
				std::size_t count = 0, first = 0;
			} pending;
			std::size_t sent = 0;
			const auto flush = [&] {
				if (pending.count) {
					a_span(pending.data, pending.count, pending.first);
					sent += pending.count;
				}
			};
			// The runs come in order and apart, each a chunk's spans in order.
			const auto add = [&](const T* a_data, std::size_t a_count, std::size_t a_first) {
				if (pending.count && a_first / chunk == pending.first / chunk && (a_first - (pending.first + pending.count)) * sizeof(T) <= kKeptMergeGapBytes) {
					pending.count = a_first + a_count - pending.first;
					return;
				}
				flush();
				pending = { a_data, a_count, a_first };
			};
			a_changes.ForEachRun(a_contentEpoch, a_elements.size(), [&](std::uint64_t a_first, std::uint64_t a_count) {
				if (a_first >= a_capacity)
					return;
				a_count = std::min<std::uint64_t>(a_count, a_capacity - a_first);
				SpansOf(a_elements, static_cast<std::size_t>(a_first), static_cast<std::size_t>(a_count), add);
			});
			flush();
			return sent;
		}
		/** @brief Emit over any elements and changes (Spans). */
		template <class Elements, class Changes, class EmitFn>
		static std::size_t EmitSpans(const Elements& a_elements, const Changes& a_changes, std::uint64_t a_contentEpoch, EmitFn&& a_emit, std::size_t a_capacity)
		{
			const auto emit = [&](const T* a_data, std::size_t a_count, std::size_t a_first) {
				a_emit(static_cast<const void*>(a_data), a_count * sizeof(T), a_first * sizeof(T));
			};
			return sizeof(T) * Spans(a_elements, a_changes, a_contentEpoch, emit, a_capacity);
		}

	private:
		static std::size_t ChunkOf(const KeptChunks<T>& a_elements) { return a_elements.ChunkElements(); }
		static std::size_t ChunkOf(const std::vector<T>&) { return ~std::size_t(0); }
		template <class Span>
		static void SpansOf(const KeptChunks<T>& a_elements, std::size_t a_first, std::size_t a_count, Span& a_span)
		{
			a_elements.ForEachSpan(a_first, a_count, a_span);
		}
		template <class Span>
		static void SpansOf(const std::vector<T>& a_elements, std::size_t a_first, std::size_t a_count, Span& a_span)
		{
			if (const std::size_t n = a_first < a_elements.size() ? std::min(a_count, a_elements.size() - a_first) : 0)
				a_span(a_elements.data() + a_first, n, a_first);
		}
	};

	/**
	 * @brief A kept array's own elements (KeptArray::Mutable): reads and writes like a std::vector. Every write first takes the
	 * chunk table and the written chunk for itself when a view shares them, so a view stays what it was whatever the order of
	 * writes and views; the chunks it replaced wait (retired) until every view has released them, and are reused.
	 *
	 * Single-threaded, except: after Prepare over a range, writes to distinct elements in it ([i], KeptArray::Write/WriteParts) may
	 * come from many threads until the next single-threaded call, since they only read the sharing counts.
	 */
	template <class T, std::size_t kChunk>
	class KeptChunkStore
	{
		// Functions, not constants: a kept array may be declared while T is incomplete.
		static constexpr std::size_t Length()
		{
			constexpr std::size_t length = kChunk == kKeptChunkBySize ? KeptChunkElements<T>() : kChunk;
			static_assert(std::has_single_bit(length));
			return length;
		}
		static constexpr std::uint32_t Shift() { return static_cast<std::uint32_t>(std::countr_zero(Length())); }
		static constexpr std::size_t Mask() { return Length() - 1; }
		using Chunk = std::vector<T>;

	public:
		KeptChunkStore() = default;
		// A copy shares the elements (each copies before it writes) but not the chunks kept for reuse: those are each store's own.
		KeptChunkStore(const KeptChunkStore& a_other) :
			table(a_other.table) {}
		KeptChunkStore& operator=(const KeptChunkStore& a_other)
		{
			if (this != &a_other) {
				table = a_other.table;
				free.clear();
				retired.clear();
			}
			return *this;
		}
		KeptChunkStore(KeptChunkStore&&) noexcept = default;
		KeptChunkStore& operator=(KeptChunkStore&&) noexcept = default;

		std::size_t size() const { return table->count; }
		bool empty() const { return !table->count; }
		const T& operator[](std::size_t a_index) const { return (*table)[a_index]; }
		T& operator[](std::size_t a_index) { return Own(a_index >> Shift())[a_index & Mask()]; }
		const T& back() const { return (*table)[table->count - 1]; }
		T& back() { return (*this)[table->count - 1]; }
		const KeptChunks<T>& Get() const { return *table; }
		KeptChunkStore& Mutable() { return *this; }

		void resize(std::size_t a_count) { resize(a_count, T{}); }
		void resize(std::size_t a_count, const T& a_value)
		{
			OwnTable();
			auto& t = *table;
			if (a_count <= t.count) {
				const std::size_t keep = (a_count + Mask()) >> Shift();
				while (t.chunks.size() > keep) {
					Drop(std::move(t.chunks.back()));
					t.chunks.pop_back();
				}
				t.count = a_count;
				return;
			}
			// The rest of the last chunk (an earlier shrink left its slack as it was), then whole chunks.
			if (const std::size_t used = t.count & Mask()) {
				auto& tail = Own(t.count >> Shift());
				std::fill(tail.begin() + static_cast<std::ptrdiff_t>(used), tail.begin() + static_cast<std::ptrdiff_t>(std::min(Length(), used + (a_count - t.count))), a_value);
			}
			const std::size_t need = (a_count + Mask()) >> Shift();
			while (t.chunks.size() < need) {
				auto chunk = Take();
				chunk->assign(Length(), a_value);
				t.chunks.push_back(std::move(chunk));
			}
			peakChunks = std::max(peakChunks, t.chunks.size());
			t.count = a_count;
		}
		void push_back(const T& a_value)
		{
			OwnTable();
			auto& t = *table;
			if (!(t.count & Mask())) {
				auto chunk = Take();
				chunk->resize(Length());
				t.chunks.push_back(std::move(chunk));
				peakChunks = std::max(peakChunks, t.chunks.size());
			}
			Own(t.count >> Shift())[t.count & Mask()] = a_value;
			++t.count;
		}
		void pop_back()
		{
			OwnTable();
			auto& t = *table;
			if (!(--t.count & Mask())) {
				Drop(std::move(t.chunks.back()));
				t.chunks.pop_back();
			}
		}
		void clear() { resize(0); }

		/** @brief Takes the table and every chunk of [a_first, a_first + a_count) (clipped) for this store: writes there are then concurrent-safe. */
		void Prepare(std::size_t a_first, std::size_t a_count)
		{
			OwnTable();
			const std::size_t end = a_first + std::min(a_count, size() > a_first ? size() - a_first : 0);
			if (a_first >= end)
				return;
			for (std::size_t c = a_first >> Shift(); c <= (end - 1) >> Shift(); ++c)
				Own(c);
		}
		/** @brief Chunks retired while views held them, reused once none does (a build's start). */
		void BeginBuild()
		{
			for (std::size_t i = 0; i < retired.size();) {
				if (retired[i].use_count() != 1) {
					++i;
					continue;
				}
				if (free.size() < peakChunks)
					free.push_back(std::move(retired[i]));
				retired[i] = std::move(retired.back());
				retired.pop_back();
			}
			// The views' last reads of what is reused happen before its next writes.
			std::atomic_thread_fence(std::memory_order_acquire);
		}
		std::shared_ptr<const KeptChunks<T>> Share() const { return table; }
		const KeptCopyStats& Stats() const { return stats; }

	private:
		/** @brief The chunk table, copied first while a view shares it (its chunk pointers: each chunk is then shared until written). */
		void OwnTable()
		{
			if (table.use_count() == 1) [[likely]]
				return;
			table = std::make_shared<KeptChunks<T>>(*table);
			++stats.tableCopies;
		}
		/** @brief Chunk a_chunk to write, copied first while a view holds it. */
		Chunk& Own(std::size_t a_chunk)
		{
			OwnTable();
			auto& chunk = table->chunks[a_chunk];
			if (chunk.use_count() == 1) [[likely]] {
				// A view that let go of it just now read it before this write.
				std::atomic_thread_fence(std::memory_order_acquire);
				return *chunk;
			}
			auto next = Take();
			*next = *chunk;
			++stats.chunkCopies;
			stats.bytesCopied += Length() * sizeof(T);
			Drop(std::move(chunk));
			chunk = std::move(next);
			return *chunk;
		}
		/** @brief A chunk of this store's own: one every view released, or a new one. Its contents are the caller's to set. */
		std::shared_ptr<Chunk> Take()
		{
			if (reuseKeptStorage && !free.empty()) {
				auto chunk = std::move(free.back());
				free.pop_back();
				++stats.chunkReuses;
				return chunk;
			}
			++stats.chunkAllocations;
			return std::make_shared<Chunk>();
		}
		/** @brief A chunk this store no longer has: reusable now if no view holds it, else once none does (BeginBuild). */
		void Drop(std::shared_ptr<Chunk>&& a_chunk)
		{
			if (!reuseKeptStorage || !a_chunk)
				return;
			if (a_chunk.use_count() != 1)
				retired.push_back(std::move(a_chunk));
			else if (free.size() < peakChunks)
				free.push_back(std::move(a_chunk));
		}

		std::shared_ptr<KeptChunks<T>> table = std::make_shared<KeptChunks<T>>(Shift());
		std::vector<std::shared_ptr<Chunk>> free;     // no view holds them: reused first
		std::vector<std::shared_ptr<Chunk>> retired;  // replaced or dropped while a view held them (freed here, never on a view's thread)
		std::size_t peakChunks = 0;                   // the most the table had: what is kept for reuse
		KeptCopyStats stats;
	};

	/**
	 * @brief A kept array's own elements as one contiguous std::vector (kKeptWhole): the first Mutable() of a build copies it whole
	 * while a view holds it, into a spare every view has released. After that Mutable(), writes to distinct elements may come from
	 * many threads. Views see it as one chunk (KeptChunks::Adopt).
	 */
	template <class T>
	class KeptWholeStore
	{
	public:
		void BeginBuild() { copied = false; }
		const std::vector<T>& Get() const { return *elements; }
		std::size_t size() const { return elements->size(); }
		const T& operator[](std::size_t a_index) const { return (*elements)[a_index]; }
		T& operator[](std::size_t a_index) { return Mutable()[a_index]; }
		std::vector<T>& Mutable()
		{
			if (copied)
				return *elements;
			copied = true;
			if (elements.use_count() == 1)
				return *elements;
			++stats.chunkCopies;
			stats.bytesCopied += elements->size() * sizeof(T);
			if (!reuseKeptStorage) {
				++stats.chunkAllocations;
				elements = std::make_shared<std::vector<T>>(*elements);
				return *elements;
			}
			// Reuse storage only after every view has released it; copy the whole array so a build cannot write an outstanding view.
			std::shared_ptr<std::vector<T>> next;
			for (auto it = spare.begin(); it != spare.end(); ++it) {
				if (it->use_count() == 1) {
					next = std::move(*it);
					spare.erase(it);
					break;
				}
			}
			if (next) {
				std::atomic_thread_fence(std::memory_order_acquire);
				++stats.chunkReuses;
			} else {
				++stats.chunkAllocations;
				next = std::make_shared<std::vector<T>>();
			}
			*next = *elements;
			if (spare.size() < 6)  // views live across frames (publications hold their payloads)
				spare.push_back(std::move(elements));
			elements = std::move(next);
			return *elements;
		}
		void Prepare(std::size_t, std::size_t) { Mutable(); }
		void clear() { Mutable().clear(); }
		std::shared_ptr<const KeptChunks<T>> Share() const { return std::make_shared<KeptChunks<T>>(KeptChunks<T>::Adopt(elements)); }
		const KeptCopyStats& Stats() const { return stats; }

	private:
		std::shared_ptr<std::vector<T>> elements = std::make_shared<std::vector<T>>();
		std::vector<std::shared_ptr<std::vector<T>>> spare;
		bool copied = false;
		KeptCopyStats stats;
	};

	/**
	 * @brief An array kept across builds and mirrored by GPU buffers: copy-on-write by chunk against the views the payloads hold
	 * (a write copies only the chunk it touches), and journalled (ChangeJournal), so each buffer is sent what changed since the
	 * content epoch it holds. kChunk: elements a chunk (a power of two), kKeptChunkBySize (about kKeptChunkBytes), or kKeptWhole.
	 *
	 * The journal is single-threaded. Parallel writers: PrepareWrites over the range (owner thread), Write/WriteParts to distinct
	 * elements of it from any thread, then the marks (Mark/MarkParts) on the owner thread, in a deterministic order.
	 */
	template <class T, std::size_t kChunk = kKeptChunkBySize>
	class KeptArray
	{
	public:
		using Elements = std::conditional_t<kChunk == kKeptWhole, KeptWholeStore<T>, KeptChunkStore<T, kChunk>>;

		/** @brief A build's start (ChangeJournal::BeginBuild); chunks every view released are reusable again. */
		void BeginBuild(std::uint64_t a_oldestContentEpoch)
		{
			journal.BeginBuild(a_oldestContentEpoch);
			elements.BeginBuild();
		}
		/** @brief The elements: KeptChunks (std::vector for kKeptWhole). */
		decltype(auto) Get() const { return elements.Get(); }
		std::size_t Size() const { return elements.size(); }
		/**
		 * @brief The elements to change; what changes is to be marked. Chunked: each chunk copied as it is first written while a view
		 * holds it (single-threaded, see PrepareWrites). kKeptWhole: the std::vector, copied whole first while a view holds it.
		 */
		decltype(auto) Mutable() { return elements.Mutable(); }
		/**
		 * @brief Takes the chunks of [a_first, a_first + a_count) for writing up front (copying those a view holds): until the next
		 * single-threaded call, Write/WriteParts/Mutable()[i] to distinct elements of the range are safe from many threads.
		 */
		void PrepareWrites(std::size_t a_first, std::size_t a_count) { elements.Prepare(a_first, a_count); }
		/** @brief Writes an element only when its bytes differ, unmarked; true when it did. Concurrent-safe in a prepared range. */
		bool Write(std::size_t a_index, const T& a_value)
		{
			if (std::memcmp(&std::as_const(elements)[a_index], &a_value, sizeof(T)) == 0)
				return false;
			elements[a_index] = a_value;
			return true;
		}
		/**
		 * @brief Writes an element when any of its a_parts (<= 64) equal slices differs, unmarked: the mask of those that did (MarkParts
		 * marks them). Concurrent-safe in a prepared range.
		 */
		std::uint64_t WriteParts(std::size_t a_index, const T& a_value, std::size_t a_parts)
		{
			static_assert(std::is_trivially_copyable_v<T>);
			const std::size_t partBytes = sizeof(T) / a_parts;
			const auto* held = reinterpret_cast<const std::byte*>(&std::as_const(elements)[a_index]);
			const auto* value = reinterpret_cast<const std::byte*>(&a_value);
			std::uint64_t mask = 0;
			for (std::size_t p = 0; p < a_parts; ++p)
				if (std::memcmp(held + p * partBytes, value + p * partBytes, partBytes) != 0)
					mask |= std::uint64_t{ 1 } << p;
			if (mask)
				elements[a_index] = a_value;
			return mask;
		}
		/** @brief Marks element a_index's parts in a_mask (WriteParts): part p is a_index * a_parts + p in the journal. */
		void MarkParts(std::size_t a_index, std::uint64_t a_mask, std::size_t a_parts)
		{
			for (; a_mask; a_mask &= a_mask - 1)
				journal.Mark(a_index * a_parts + static_cast<std::size_t>(std::countr_zero(a_mask)));
		}
		/** @brief Writes and marks an element only when its bytes differ; true when it did. */
		bool Set(std::size_t a_index, const T& a_value)
		{
			if (!Write(a_index, a_value))
				return false;
			journal.Mark(a_index);
			return true;
		}
		/**
		 * @brief Writes an element and marks only those of its a_parts equal slices whose bytes differ: an array written this way has a
		 * journal of parts (element i's part p is i * a_parts + p), and its views are emitted by part. Returns the parts marked.
		 */
		std::size_t SetParts(std::size_t a_index, const T& a_value, std::size_t a_parts)
		{
			const std::uint64_t mask = WriteParts(a_index, a_value, a_parts);
			MarkParts(a_index, mask, a_parts);
			return static_cast<std::size_t>(std::popcount(mask));
		}
		void Mark(std::size_t a_index) { journal.Mark(a_index); }
		void MarkRange(std::uint64_t a_first, std::uint64_t a_count) { journal.MarkRange(a_first, a_count); }
		void Resync() { journal.Resync(); }
		/** @brief Empty, and every holder sent everything again (the content epoch counts on). */
		void Clear()
		{
			elements.clear();
			journal.Resync();
		}
		std::uint64_t ContentEpoch() const { return journal.ContentEpoch(); }
		bool Changed() const { return journal.Changed(); }
		KeptView<T> View() const { return { KeptElements<T>(elements.Share()), journal.Take() }; }
		/**
		 * @brief View().Emit without the view's copies (no journal entries copied, no elements shared): the live journal's runs over the
		 * array's own elements. Only while nothing writes the array (its owner, between builds).
		 */
		template <class EmitFn>
		std::size_t Emit(std::uint64_t a_contentEpoch, EmitFn&& a_emit, std::size_t a_capacity = ~std::size_t(0)) const
		{
			return KeptView<T>::EmitSpans(elements.Get(), journal, a_contentEpoch, a_emit, a_capacity);
		}
		/** @brief The elements themselves, for a view that is not journalled (content epoch 0: sent whole). */
		KeptElements<T> Shared() const { return KeptElements<T>(elements.Share()); }
		const KeptCopyStats& CopyStats() const { return elements.Stats(); }

	private:
		Elements elements;
		ChangeJournal journal;
	};
	/** @brief A kept array of one contiguous std::vector (kKeptWhole). */
	template <class T>
	using KeptWholeArray = KeptArray<T, kKeptWhole>;

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
