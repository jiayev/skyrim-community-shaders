#pragma once

// IndirectDraws' growable tables. Included by Internal.h, after the headers these declarations use.

namespace DCLF::Draws
{
	/**
	 * @brief Any thread (T6b3b): wakes the snapshot builder (IndirectDraws::Impl::SnapshotPass) - a growth settled, a revision completed,
	 * the frame posted something it takes. Coalesced; nothing before the builder's first work.
	 */
	void WakeSnapshotBuilder();

	/**
	 * @brief A growth of versioned buffers (Versioned): their next versions, made at the new sizes, and what their adoption changes
	 * - the held versions reset (a new version holds nothing), the addresses and descriptor indices taken again, a layout made
	 * for the new size. A consequence describes the new versions, so it runs when they become the ones written: at adoption.
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
	 * @brief Adopts a growth (render thread): its versions become current, then a_adopted runs, then the graph host is told
	 * (PersistentGraphHost::NoteNewVersions: a ticket prepared against the old versions is prepared again or takes a kept recording).
	 * Nothing changes in place, so nothing waits; the old versions stay as they were for whatever holds them. The live path adopts a
	 * growth as it is made.
	 */
	void Adopt(Growth&& a_growth, const std::function<void()>& a_adopted = {});

	/**
	 * @brief Growth as graph work (G2; the SARP/BasicRenderer model, BuildVersionedGpuBuffer), in revision mode. A change is an
	 * owner's sizing (the capacities its writers and shapes index by) with the buffers that grow with it: their next versions are
	 * made and filled on the scene graph (Published::SceneGraph::PostGrowth: the preparation pool and the dedicated uploader), and
	 * a change without buffers is ready at once. A ready change is what the next revision sealed names (VersionSet::Snapshot, its
	 * shapes made from RevisionSizing); it is adopted - its versions made current, its sizing the owner's, its consequences run - when
	 * a snapshot whose revision names it is adopted (IndirectDraws::AdoptSnapshot). Until then the owner's sizing and the current versions are what
	 * every write and every recording uses, and what is past them waits. No revision is sealed while a change is pending: its claims
	 * could reach past the versions it names. Without revisions a change is adopted as it is made.
	 *
	 * T6b3b b2a, two sides and nothing shared but immutable entries and one atomic:
	 * - The producer's (the revision code: the reserves, MakeRevision, AssembleRevision): Post, Settle, Prune, the sizings and versions a
	 *   revision names (RevisionSizing, LatestSizing, Ready, Named, Asked, Held) and Naming, the changes a sealed revision names, handed
	 *   over by value in it (NamedChanges, its growths fragment). Its entries are its own; an entry is immutable once ready but for the
	 *   producer's own fields (ready, adoptedSeen). The last adopted change of an owner (or of a buffer, for an ownerless one) stays as
	 *   the producer's record of what was adopted, so it never reads an owner's sizing the frame writes at adoption.
	 * - The frame's (AdoptSnapshot): AdoptNamed adopts, in request order, the selected revision's changes it has not adopted yet (the
	 *   versions, the owner's sizing, the consequences, VersionRegistry::Changed, NoteNewVersions), and publishes how far it adopted
	 *   (adoptedPublished), which the producer's next Prune takes in.
	 * An entry is settled on the coordinator lane (published by its state). An owner's change holds its life (the owner's shared state):
	 * a dead owner's changes are dropped, never matched by a later owner at its address, and their consequences never run.
	 */
	struct Growths
	{
		/** @brief Producer thread: a new version's contents (VersionGrowthRequest::contentsOf). */
		using ContentsOf = std::function<void(const org::BufferVersion&, std::vector<org::services::VersionFill>&, std::shared_ptr<const void>&)>;
		/** @brief One buffer of a change: its new size (structured elements, else bytes) and its contents (none: written whole by its users). */
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
			std::uint64_t serial = 0;              // request order (Post's): the order changes are adopted in
			const void* owner = nullptr;           // the sizing it changes (RevisionSizing, LatestSizing), or none
			std::weak_ptr<const void> life;        // the owner's (or an ownerless table's) life: expired, the change is dropped
			std::shared_ptr<const void> sizing;    // the owner's sizing once adopted
			std::vector<Part> parts;
			std::function<void()> adopted;         // the frame, when it is adopted (after its versions and sizing)
			// The producer's: seen ready at a make (Settle: what a revision may name), and seen adopted (Prune: its record).
			bool ready = false;
			bool adoptedSeen = false;
			enum : std::uint8_t
			{
				kPending,
				kReady,
				kFailed
			};
			std::atomic<std::uint8_t> state = kPending;
			std::vector<std::shared_ptr<const org::BufferVersion>> versions;  // by its parts, written before the state turns ready
			std::string error;                                                // before it turns failed
			bool Alive() const { return !life.expired(); }
		};
		/** @brief What a sealed revision names (its growths fragment): the ready changes the producer had not seen adopted, in order. */
		struct NamedChanges
		{
			std::vector<std::shared_ptr<const Entry>> entries;
		};
		// The producer's.
		std::vector<std::shared_ptr<Entry>> entries;  // by request order: outstanding, and each owner's (buffer's) last adopted
		std::uint64_t serial = 0;                     // the last Post's
		std::uint64_t stamp = 0;                      // moves when what a revision may name moves (a change seen ready, or adopted)
		std::uint64_t requested = 0, failed = 0;      // since the last report
		std::uint32_t failuresLogged = 0;
		// The frame's: the last change adopted (AdoptNamed), and its publication for the producer (Prune).
		std::uint64_t adoptedThrough = 0;
		std::atomic<std::uint64_t> adoptedPublished{ 0 };
		std::atomic<std::uint64_t> adopted{ 0 };      // since the last report (the builder's report exchanges it)
		// Growths adopted at once in revision mode, by buffer name (Adopt: the thread that made them).
		std::map<std::string, std::uint32_t> atOnce;
		// Whether changes are graph work (Deferred), as the frame last posted it (RevisionInputs: async epochs and the host's uploader,
		// owner-thread state), and the uploader a change's fills go through (the producer's copy, set at each make: RevisionInputs).
		std::atomic<bool> deferred{ false };
		std::weak_ptr<org::runtime::IUploadService> uploads;

		static Growths& Get();
		/**
		 * @brief Any thread: changes are graph work - revision mode, with the scene graph and the host's uploader (posted by the frame:
		 * Impl::PostRevisionInputs) - else they are adopted at once.
		 */
		static bool Deferred();
		/** @brief The owner thread (the frame's commits): the same, from the host as it is now. */
		static bool OwnerDeferred();

		/**
		 * @brief The producer: an owner's sizing changes to a_sizing, with a_parts' buffers grown: adopted now without revisions, else
		 * posted. a_life: the owner's life (its shared state). a_adopted runs once it is adopted, after the owner took a_sizing.
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
		/** @brief The producer: the owner's sizing a revision made now names: its newest ready change's (adopted or not), else its own. */
		template <class Sizing>
		const Sizing& RevisionSizing(const Sizing& a_owner) const
		{
			for (auto it = entries.rbegin(); it != entries.rend(); ++it)
				if ((*it)->owner == &a_owner && (*it)->ready && (*it)->Alive())
					return *static_cast<const Sizing*>((*it)->sizing.get());
			return a_owner;
		}
		/** @brief The producer: what a new change builds on: the owner's newest change's sizing (pending, ready or adopted), else its own. */
		template <class Sizing>
		const Sizing& LatestSizing(const Sizing& a_owner) const
		{
			for (auto it = entries.rbegin(); it != entries.rend(); ++it)
				if ((*it)->owner == &a_owner && (*it)->Alive())
					return *static_cast<const Sizing*>((*it)->sizing.get());
			return a_owner;
		}
		/** @brief The producer: a change of a_owner's (none: of a_parts' buffers alone), a_adopted its whole consequence. */
		void Post(const void* a_owner, std::weak_ptr<const void> a_life, std::shared_ptr<const void> a_sizing, std::vector<Part> a_parts, std::function<void()> a_adopted);
		/** @brief The producer: the size a buffer's newest outstanding change asks for, in bytes (0: none). */
		std::uint64_t Asked(const Versioned& a_buffer) const;
		/** @brief The producer: the size a buffer's newest change asks for or was adopted at, in bytes (0: none: its first backing's). */
		std::uint64_t Held(const Versioned& a_buffer) const;
		/** @brief The producer, at each make's start: takes in what the frame adopted (adoptedPublished), its records kept. */
		void Prune();
		/** @brief The producer, the make: takes in what the graph settled. True while a change is pending (no revision is sealed). */
		bool Settle();
		/** @brief The producer: the newest version of a_buffer a revision made now names that is not adopted yet, else null (the current one). */
		std::shared_ptr<const org::BufferVersion> Ready(const Versioned& a_buffer) const;
		/** @brief The producer: the newest version of a_buffer a revision made now names: a ready change's, adopted or not, else null. */
		std::shared_ptr<const org::BufferVersion> Named(const Versioned& a_buffer) const;
		/** @brief The producer, before a seal: what the revision names (every ready change not seen adopted), handed over in it. */
		std::shared_ptr<const NamedChanges> Naming() const;
		/** @brief The frame, at a selection: adopts, in order, a_named's changes it has not adopted yet. True when one was. */
		bool AdoptNamed(const NamedChanges& a_named);
		/** @brief A report line, its counters reset; empty when nothing happened. */
		std::string Report();
	};

	/**
	 * @brief A table's rows as a growth's new version holds them (Growths: the fill made on the producer's thread, from rows kept
	 * immutable): `bytes` lays the rows out for a table at an address, and `held` is the kept rows' version they are, which the table
	 * holds once the version is adopted (the rows changed since are sent then).
	 */
	struct RowsCapture
	{
		std::uint64_t held = 0;
		std::function<std::vector<std::byte>(std::uint64_t a_address)> bytes;
	};

	/**
	 * @brief A table of fixed-stride rows in one device buffer the shaders read by address, which grows instead of capping.
	 *
	 * Each row is a constant-buffer block (the stride a multiple of kConstantAlignment), so the table is an array of them.
	 * Rows are named by index; what holds their values (a KeptArray, a payload's vector) is the caller's. Reserve grows
	 * the backing when the rows asked for do not fit - to twice the capacity, or more - through Buffer::ResizeBytes, which
	 * keeps the graph resource (its backing generation changes) and releases the old backing through ORG's deletion queue,
	 * frames in flight after the GPU last used it. The new backing holds
	 * nothing, so after a growth (the generation changes) every row is sent again. A row past the capacity waits for the
	 * next Reserve, as a caster waits for its texture: nothing is dropped and nothing falls back.
	 *
	 * Reserved by the revision code (T6b3b b2a: sized from Growths::Held, never the capacity the frame's adoption writes): a growth
	 * publishes a new version (PersistentGraphHost::NoteNewVersions), nothing waits. Given the rows' capture, a growth is graph work
	 * (Growths::Deferred): the capacity, address and generation move when its version is adopted (the frame), and until then a row
	 * past the capacity waits.
	 */
	struct GrowableRows
	{
		Versioned buffer;  // a growth is a new version (the old one stays as it was for what holds it)
		std::uint64_t address = 0;
		std::uint32_t stride = 0;
		std::uint32_t capacity = 0;    // rows the backing holds
		std::uint64_t generation = 0;  // counts the backings: a new one holds nothing
		std::uint32_t growths = 0;     // since the last report
		std::string name;
		std::shared_ptr<int> life = std::make_shared<int>();  // a deferred growth's consequence runs only while the table is

		/** @brief The first backing: a_rows rows of a_stride bytes. False when it has no device address. */
		bool Create(std::uint32_t a_stride, std::uint32_t a_rows, const char* a_name);
		/**
		 * @brief Room for a_rows: true when it grew or asked to. a_adopted(held) runs when the new version is adopted, with the kept
		 * rows' version it holds: a_capture's (a deferred growth is filled from it), else 0 (every row must be sent again).
		 */
		bool Reserve(std::uint32_t a_rows, const std::function<void(std::uint64_t)>& a_adopted = {}, const RowsCapture* a_capture = nullptr);
		/** @brief The producer: the address a revision made now names (Growths::Named: a ready growth's version), else the first one's. */
		std::uint64_t RevisionAddress() const;
	};
}
