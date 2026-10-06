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
	 */
	struct VersionRegistry
	{
		std::vector<std::weak_ptr<org::VersionedBuffer>> buffers;
		// Names the current versions: a version set taken with this value is current while it holds. Every value is new (`next`),
		// so a set that names versions not yet current (a growth's: Growths) takes one of its own, and the selection that makes
		// exactly its versions current makes it the registry's again.
		std::uint64_t changes = 0, next = 0;
		void Changed() { changes = ++next; }
		static VersionRegistry& Get()
		{
			static VersionRegistry registry;
			return registry;
		}
	};
	/** @brief The first version of a buffer that grows (registered: VersionRegistry). */
	inline Versioned MakeVersioned(std::shared_ptr<org::Buffer> a_buffer)
	{
		if (!a_buffer)
			return nullptr;
		auto versioned = org::VersionedBuffer::Create(std::move(a_buffer));
		auto& registry = VersionRegistry::Get();
		registry.buffers.push_back(versioned);
		return versioned;
	}
	/** @brief Where a write goes: the buffer, or a versioned buffer's current version. */
	inline const std::shared_ptr<org::Buffer>& Target(const std::shared_ptr<org::Buffer>& a_buffer) { return a_buffer; }
	inline std::shared_ptr<org::Buffer> Target(const Versioned& a_buffer) { return a_buffer ? a_buffer->Get() : nullptr; }
}
