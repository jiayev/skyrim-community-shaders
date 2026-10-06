#pragma once

// IndirectDraws' growable tables. Included by Internal.h, after the headers these declarations use.

namespace DCLF::Draws
{
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
	 * Render thread, between epochs: a growth publishes a new version (PersistentGraphHost::NoteNewVersions), nothing waits.
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

		/** @brief The first backing: a_rows rows of a_stride bytes. False when it has no device address. */
		bool Create(std::uint32_t a_stride, std::uint32_t a_rows, const char* a_name);
		/** @brief Room for a_rows: true when it grew; a_adopted runs when the new version is adopted (every row must be sent again). */
		bool Reserve(std::uint32_t a_rows, const std::function<void()>& a_adopted = {});
	};
}
