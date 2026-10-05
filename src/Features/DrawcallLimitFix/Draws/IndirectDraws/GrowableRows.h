#pragma once

// IndirectDraws' growable tables. Included by Internal.h, after the headers these declarations use.

namespace DCLF::Draws
{
	/**
	 * @brief The scope in which the render thread may change a graph buffer's backing (ResizeBytes, ResizeStructured).
	 *
	 * ORG's host thread prepares the next tickets while the render thread commits an epoch's inputs, and each preparation
	 * captures every slot's backing. A resize has no backing between releasing the old one and creating the new one: a
	 * preparation in that window bound nothing, or materialized the buffer itself, and its ticket wrote through a null or
	 * released buffer (crash-catalog.md, "the Z-prepass device loss"). The scope keeps the two apart
	 * (PersistentGraphHost::MutateBackings: it waits out a preparation in progress), and the tickets prepared before it are
	 * prepared again. Opened only where a backing does change, so a frame that grows nothing never waits; ORG throws on a
	 * resize outside it while async epochs run.
	 */
	std::optional<org::PersistentGraphHost::BackingMutation> MutateBackings();

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
	 * Render thread, between epochs, inside MutateBackings (Reserve opens it when the backing changes).
	 */
	struct GrowableRows
	{
		std::shared_ptr<org::Buffer> buffer;
		std::uint64_t address = 0;
		std::uint32_t stride = 0;
		std::uint32_t capacity = 0;    // rows the backing holds
		std::uint64_t generation = 0;  // counts the backings: a new one holds nothing
		std::uint32_t growths = 0;     // since the last report
		std::string name;

		/** @brief The first backing: a_rows rows of a_stride bytes. False when it has no device address. */
		bool Create(std::uint32_t a_stride, std::uint32_t a_rows, const char* a_name);
		/** @brief Room for a_rows: true when the backing changed (every row must be sent again). */
		bool Reserve(std::uint32_t a_rows);
	};
}
