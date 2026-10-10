#pragma once

// IndirectDraws' versioned buffers (immutable inputs). Included by Internal.h, after the headers these declarations use.

namespace DCLF::Draws
{
	/**
	 * @brief A buffer that grows by versions (immutable inputs, org::VersionedBuffer): a growth publishes a new buffer at the new
	 * size and leaves the old one as it was, for whatever prepared or recorded against it; nothing changes in place, so nothing
	 * waits (Growth, Adopt). Passes declare it (a resolver: a preparation resolves its revision's version, else the current one);
	 * writes go to the current version's buffer (->Get()).
	 */
	using Versioned = std::shared_ptr<org::VersionedBuffer>;
	/**
	 * @brief Every versioned buffer DCLF made, and a count of the changes to their current versions (a growth adopted): what a
	 * scene revision's version set snapshots (VersionSet), again when the count moved or the graph was built again. A buffer
	 * made is no change: no revision before it names it, and the passes that declare it come with a graph build.
	 *
	 * T6b3b b2a, lock-free across threads: a buffer is registered from any thread (MakeVersioned: a push onto registered, an MPSC
	 * queue), and the snapshot's owner (the revision code, VersionSet::Snapshot) takes them into buffers, its own list. changes is the
	 * frame's (Changed at an adoption, SetChanges at a selection), compared at adoption; published is its value for the revision code,
	 * which stamps a set current with it. next hands out new values to either side.
	 */
	struct VersionRegistry
	{
		// Any thread: buffers made since the snapshot owner last took them.
		EventQueue<std::weak_ptr<org::VersionedBuffer>, 256> registered;
		// The revision code's: every buffer registered, as taken (Snapshot drops the expired).
		std::vector<std::weak_ptr<org::VersionedBuffer>> buffers;
		// Names the current versions: a version set taken with this value is current while it holds. Every value is new (`next`),
		// so a set that names versions not yet current (a growth's: Growths) takes one of its own, and the selection that makes
		// exactly its versions current makes it the registry's again. The frame's, published for the revision code.
		std::uint64_t changes = 0;
		std::atomic<std::uint64_t> published{ 0 }, next{ 0 };
		/** @brief Any thread: a value no set or registry has had. */
		std::uint64_t Next() { return next.fetch_add(1, std::memory_order_relaxed) + 1; }
		/** @brief The frame: the current versions changed (a growth adopted). */
		void Changed() { SetChanges(Next()); }
		/** @brief The frame: the current versions are exactly those of a set taken with a_changes (its selection adopted them). */
		void SetChanges(std::uint64_t a_changes)
		{
			changes = a_changes;
			published.store(a_changes, std::memory_order_release);
		}
		/** @brief The revision code: the frame's changes as last published. */
		std::uint64_t Published() const { return published.load(std::memory_order_acquire); }
		/** @brief The snapshot's owner: the buffers registered since, taken into buffers. */
		void TakeRegistered()
		{
			registered.Drain([this](std::weak_ptr<org::VersionedBuffer>&& a_buffer) { buffers.push_back(std::move(a_buffer)); });
		}
		static VersionRegistry& Get()
		{
			static VersionRegistry registry;
			return registry;
		}
	};
	/** @brief Any thread: the first version of a buffer that grows (registered: VersionRegistry::registered). */
	inline Versioned MakeVersioned(std::shared_ptr<org::Buffer> a_buffer)
	{
		if (!a_buffer)
			return nullptr;
		auto versioned = org::VersionedBuffer::Create(std::move(a_buffer));
		VersionRegistry::Get().registered.Push(versioned);
		return versioned;
	}
	/** @brief Where a write goes: the buffer, or a versioned buffer's current version. */
	inline const std::shared_ptr<org::Buffer>& Target(const std::shared_ptr<org::Buffer>& a_buffer) { return a_buffer; }
	inline std::shared_ptr<org::Buffer> Target(const Versioned& a_buffer) { return a_buffer ? a_buffer->Get() : nullptr; }
}
