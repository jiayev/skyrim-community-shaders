#pragma once

// IndirectDraws' versioned buffers (immutable inputs). Included by Internal.h, after the headers these declarations use.

namespace DCLF::Draws
{
	/**
	 * @brief A buffer that grows by versions (immutable inputs, org::VersionedBuffer): a growth publishes a new buffer at the new
	 * size and leaves the old one as it was, for whatever prepared or recorded against it; nothing changes in place, so nothing
	 * waits. Every one is registered with the scene's set (SceneBufferSet), which grows it to the producer's requirements and names
	 * its versions: passes declare it (a resolver: a preparation resolves the version its revision names), and a commit's writes go
	 * to the version the adopted set names (CommitUploads).
	 */
	using Versioned = std::shared_ptr<org::VersionedBuffer>;
	/**
	 * @brief The versions a sealed set names (org::services::NamedVersions): the only source of a versioned buffer's address, SRV
	 * index and capacity. Builds ahead read the sealed revision's set, commits and latch writes the adopted one's
	 * (IndirectDraws::Impl::Adopted), each through Of.
	 */
	using NamedVersions = org::services::NamedVersions;

	/** @brief How a versioned buffer grows: its size in units, its growth policy, and what a new version holds. */
	struct GrowableSpec
	{
		org::services::BufferExtent extent;
		org::services::GrowthPolicy policy = org::services::GrowthPolicy::Doubling();
		org::services::GrowableContents contents = org::services::GrowableContents::Fresh;
		org::services::GrowableCapture capture;  // Capture's
	};

	/** @brief Any thread: the first version of a buffer that grows, registered with the scene's set (SceneBufferSet::Make). */
	Versioned MakeVersioned(std::shared_ptr<org::Buffer> a_buffer, GrowableSpec a_spec);

	/**
	 * @brief The units the version a_named names holds, 0 when the set names none (a set made before the buffer, or before the first
	 * adoption): what a fit made on the render thread for the coordinator reads. The draws' paths read Of.
	 */
	inline std::uint64_t NamedUnits(const NamedVersions& a_named, const Versioned& a_buffer)
	{
		return a_buffer && a_named.Names(*a_buffer) ? a_named.Of(*a_buffer).units : 0;
	}

	/** @brief Where a write goes: the buffer, or a versioned buffer's current version. */
	inline const std::shared_ptr<org::Buffer>& Target(const std::shared_ptr<org::Buffer>& a_buffer) { return a_buffer; }
	inline std::shared_ptr<org::Buffer> Target(const Versioned& a_buffer) { return a_buffer ? a_buffer->Get() : nullptr; }
}
