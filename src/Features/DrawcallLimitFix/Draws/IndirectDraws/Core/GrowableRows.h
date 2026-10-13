#pragma once

// IndirectDraws' growable tables: versioned-buffer growth (Growth, Adopt), growth as graph work named by scene revisions (Growths:
// the snapshot builder posts and settles, the frame start adopts), and GrowableRows. Included by Internal.h.

namespace DCLF::Draws
{
	/** @brief Any thread: wakes the snapshot builder (Impl::SnapshotPass). Coalesced; a no-op before its first work. */
	void WakeSnapshotBuilder();

	/**
	 * @brief A growth of versioned buffers: their next versions at the new sizes. Consequences describe the new versions, so they
	 * run at adoption.
	 */
	struct Growth
	{
		std::vector<std::pair<Versioned, std::shared_ptr<const org::BufferVersion>>> versions;
		Growth& Structured(const Versioned& a_buffer, std::uint32_t a_elements)
		{
			versions.emplace_back(a_buffer, a_buffer->MakeStructured(a_elements));
			return *this;
		}
		Growth& Bytes(const Versioned& a_buffer, std::uint64_t a_bytes)
		{
			versions.emplace_back(a_buffer, a_buffer->MakeBytes(a_bytes));
			return *this;
		}
	};

	/**
	 * @brief Render thread: makes a growth's versions current, runs a_adopted, then tells the graph host (NoteNewVersions).
	 * Nothing changes in place, so nothing waits; old versions stay intact for their holders.
	 */
	void Adopt(Growth&& a_growth, const std::function<void()>& a_adopted = {});

	/**
	 * @brief Growth as graph work (the BasicRenderer model), in revision mode. A change is an owner's sizing plus the buffers that
	 * grow with it, made and filled on the scene graph (SceneGraph::PostGrowth). The next sealed revision names each ready change;
	 * it is adopted when a snapshot naming it is adopted (AdoptSnapshot). Until then everything uses the current sizing and
	 * versions, and what is past them waits. No revision is sealed while a change is pending (its claims could reach past the
	 * versions it names). Without revisions a change is adopted as it is made.
	 *
	 * Two sides share only immutable entries and one atomic:
	 * - Producer (snapshot builder): Post, Settle, Prune, the sizing/version queries, Naming. Entries are immutable once ready
	 *   except ready/adoptedSeen. Each owner's last adopted change is kept as the producer's record, so it never reads an owner
	 *   sizing the frame writes.
	 * - Frame: AdoptNamed adopts, in request order, and publishes adoptedPublished for Prune.
	 * A change holds its owner's life: a dead owner's changes are dropped, never matched by a later owner at the same address.
	 */
	struct Growths
	{
		/** @brief Producer thread: a new version's contents (VersionGrowthRequest::contentsOf). */
		using ContentsOf = std::function<void(const org::BufferVersion&, std::vector<org::services::VersionFill>&, std::shared_ptr<const void>&)>;
		/** @brief One buffer of a change: new size (structured elements, else bytes) and contents (none: its users write it whole). */
		struct Part
		{
			Versioned buffer;
			std::uint32_t elements = 0;
			std::uint64_t bytes = 0;
			ContentsOf contents;
			std::uint64_t Bytes() const { return elements ? std::uint64_t(elements) * 4 : bytes; }
		};
		struct Entry
		{
			std::uint64_t serial = 0;              // request order = adoption order
			const void* owner = nullptr;           // the sizing it changes, or none
			std::weak_ptr<const void> life;        // expired: the change is dropped
			std::shared_ptr<const void> sizing;    // the owner's sizing once adopted
			std::vector<Part> parts;
			std::function<void()> adopted;         // frame, at adoption (after versions and sizing)
			// Producer's: seen ready by Settle (a revision may name it), seen adopted by Prune.
			bool ready = false;
			bool adoptedSeen = false;
			enum : std::uint8_t
			{
				kPending,
				kReady,
				kFailed
			};
			std::atomic<std::uint8_t> state = kPending;
			std::vector<std::shared_ptr<const org::BufferVersion>> versions;  // per part, written before state turns ready
			std::string error;                                                // before it turns failed
			bool Alive() const { return !life.expired(); }
		};
		/** @brief A sealed revision's growths fragment: the ready changes not yet seen adopted, in order. */
		struct NamedChanges
		{
			std::vector<std::shared_ptr<const Entry>> entries;
		};
		// Producer's.
		std::vector<std::shared_ptr<Entry>> entries;  // request order: outstanding, plus each owner's (buffer's) last adopted
		std::uint64_t serial = 0;                     // the last Post's
		std::uint64_t stamp = 0;                      // moves when what a revision may name moves
		std::uint64_t requested = 0, failed = 0;      // since the last report
		std::uint32_t failuresLogged = 0;
		// Frame's: the last change adopted, published for Prune.
		std::uint64_t adoptedThrough = 0;
		std::atomic<std::uint64_t> adoptedPublished{ 0 };
		std::atomic<std::uint64_t> adopted{ 0 };      // since the last report
		// Growths adopted at once in revision mode, by buffer name.
		std::map<std::string, std::uint32_t> atOnce;
		// Whether changes are graph work (posted by the frame), and the uploader fills go through (set at each make).
		std::atomic<bool> deferred{ false };
		std::weak_ptr<org::runtime::IUploadService> uploads;

		static Growths& Get();
		/** @brief Any thread: changes are graph work (revision mode with scene graph and uploader), else adopted at once. */
		static bool Deferred();
		/** @brief Owner thread (the frame's commits): the same, from the host's current state. */
		static bool OwnerDeferred();

		/**
		 * @brief Producer: the owner's sizing changes to a_sizing with a_parts grown; adopted now without revisions, else posted.
		 * a_life: the owner's shared state. a_adopted runs at adoption, after the owner took a_sizing.
		 */
		template <class Sizing>
		void Change(Sizing& a_owner, std::weak_ptr<const void> a_life, Sizing a_sizing, std::vector<Part> a_parts, std::function<void()> a_adopted = {})
		{
			auto sizing = std::make_shared<const Sizing>(std::move(a_sizing));
			Post(&a_owner, std::move(a_life), sizing, std::move(a_parts), [&a_owner, sizing, adopted = std::move(a_adopted)] {
				a_owner = *sizing;
				if (adopted)
					adopted();
			});
		}
		/** @brief Producer: the sizing a revision made now names: the newest ready change's (adopted or not), else the owner's. */
		template <class Sizing>
		const Sizing& RevisionSizing(const Sizing& a_owner) const
		{
			for (auto it = entries.rbegin(); it != entries.rend(); ++it)
				if ((*it)->owner == &a_owner && (*it)->ready && (*it)->Alive())
					return *static_cast<const Sizing*>((*it)->sizing.get());
			return a_owner;
		}
		/** @brief Producer: what a new change builds on: the newest change's sizing (any state), else the owner's. */
		template <class Sizing>
		const Sizing& LatestSizing(const Sizing& a_owner) const
		{
			for (auto it = entries.rbegin(); it != entries.rend(); ++it)
				if ((*it)->owner == &a_owner && (*it)->Alive())
					return *static_cast<const Sizing*>((*it)->sizing.get());
			return a_owner;
		}
		/** @brief Producer: posts a change of a_owner (null: of a_parts alone); a_adopted is its whole consequence. */
		void Post(const void* a_owner, std::weak_ptr<const void> a_life, std::shared_ptr<const void> a_sizing, std::vector<Part> a_parts, std::function<void()> a_adopted);
		/** @brief Producer: bytes the buffer's newest outstanding change asks for (0: none). */
		std::uint64_t Asked(const Versioned& a_buffer) const;
		/** @brief Producer: bytes the buffer's newest change asks for or was adopted at (0: still its first backing). */
		std::uint64_t Held(const Versioned& a_buffer) const;
		/** @brief Producer, at each make's start: takes in adoptedPublished, keeping the records. */
		void Prune();
		/** @brief Producer, at the make: takes in what the graph settled. True while a change is pending (no seal). */
		bool Settle();
		/** @brief Producer: the newest named, not-yet-adopted version of a_buffer, else null (the current one). */
		std::shared_ptr<const org::BufferVersion> Ready(const Versioned& a_buffer) const;
		/** @brief Producer: the newest named version of a_buffer (a ready change's, adopted or not), else null. */
		std::shared_ptr<const org::BufferVersion> Named(const Versioned& a_buffer) const;
		/** @brief Producer, before a seal: every ready change not seen adopted, carried in the revision. */
		std::shared_ptr<const NamedChanges> Naming() const;
		/** @brief Frame, at a selection: adopts a_named's not-yet-adopted changes in order. True when one was. */
		bool AdoptNamed(const NamedChanges& a_named);
		/** @brief A report line, its counters reset; empty when nothing happened. */
		std::string Report();
	};

	/**
	 * @brief A row table growth's fill, from immutable kept rows: `bytes` lays them out at an address; `held` is their version,
	 * which the table holds once adopted.
	 */
	struct RowsCapture
	{
		std::uint64_t held = 0;
		std::function<std::vector<std::byte>(std::uint64_t a_address)> bytes;
	};

	/**
	 * @brief A table of fixed-stride rows in one device buffer, read by address, that grows instead of capping.
	 *
	 * Rows are constant-buffer blocks (stride a multiple of kConstantAlignment); the caller owns the values. Reserve at least
	 * doubles the backing; a new backing holds nothing, so every row is sent again. A row past the capacity waits for the next
	 * Reserve: nothing is dropped, nothing falls back. Reserved by the revision code from Growths::Held; capacity, address and
	 * generation move when the frame adopts the version.
	 */
	struct GrowableRows
	{
		Versioned buffer;  // a growth is a new version; the old one stays intact for its holders
		std::uint64_t address = 0;
		std::uint32_t stride = 0;
		std::uint32_t capacity = 0;    // rows the backing holds
		std::uint64_t generation = 0;  // counts the backings: a new one holds nothing
		std::uint32_t growths = 0;     // since the last report
		std::string name;
		std::shared_ptr<int> life = std::make_shared<int>();  // a deferred growth's consequence runs only while this lives

		/** @brief The first backing: a_rows rows of a_stride bytes. False when it has no device address. */
		bool Create(std::uint32_t a_stride, std::uint32_t a_rows, const char* a_name);
		/**
		 * @brief Room for a_rows: true when it grew or asked to. a_adopted(held) runs at adoption with the kept-rows version the
		 * backing holds: a_capture's, else 0 (every row must be sent again).
		 */
		bool Reserve(std::uint32_t a_rows, const std::function<void(std::uint64_t)>& a_adopted = {}, const RowsCapture* a_capture = nullptr);
		/** @brief Producer: the address a revision made now names (a ready growth's version), else the current one. */
		std::uint64_t RevisionAddress() const;
	};
}
