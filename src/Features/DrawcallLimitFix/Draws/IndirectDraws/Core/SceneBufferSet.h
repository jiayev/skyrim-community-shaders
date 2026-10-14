#pragma once

// The scene's growable buffers (SceneBufferSet): one org::services::GrowableBufferSet every versioned buffer DCLF makes registers
// with, grown as graph work on DCLF's state graph to the revision producer's requirements and named by the sets it seals (the
// snapshot). Unit groups: one requirement over buffers indexed alike. Included by Internal.h.

namespace DCLF::Draws
{
	/** @brief Any thread: wakes the snapshot builder (Impl::SnapshotPass). Coalesced; a no-op before its first work. */
	void WakeSnapshotBuilder();

	using GrowthBuild = org::services::GrowableBufferSet::Build;
	using SealedSet = org::services::GrowableBufferSet::Sealed;
	using LatchHandle = org::services::LatchHandle;

	/**
	 * @brief The scene's set: requirement -> one growth batch (SceneGraph::PostGrowth, its fills on the frame's uploader) -> the
	 * next sealed set (its versions and latch blocks). Make and RegisterLatch register from any thread (lock-free); Begin, the
	 * requirements and Seal are the revision producer's alone (the snapshot builder). A set that cannot meet a requirement is never
	 * sealed; the producer seals again when the batch settles (it wakes the builder).
	 */
	class SceneBufferSet
	{
	public:
		static SceneBufferSet& Get();

		/** @brief Any thread: a_first as the first version of a buffer of the set, grown as a_spec says. */
		Versioned Make(std::shared_ptr<org::Buffer> a_first, GrowableSpec a_spec);

		/**
		 * @brief Any thread: a latch block of the set, a_slots frame slots, made by the seals that require it (another layout: a new
		 * block; the same one outgrown: grown by a_policy).
		 */
		LatchHandle RegisterLatch(std::string a_name, std::uint32_t a_slots, org::services::GrowthPolicy a_policy = org::services::GrowthPolicy::Headroom(0))
		{
			return set.Register(org::services::LatchRegistration{ std::move(a_name), a_slots, a_policy });
		}

		/** @brief Producer: the uploader a growth's fills go through (the frame's, as posted: RevisionInputs::uploads). */
		void SetUploads(std::weak_ptr<org::runtime::IUploadService> a_uploads) { uploads = std::move(a_uploads); }
		/** @brief Producer: a build's start (registrations and a settled batch taken in). */
		GrowthBuild Begin() { return set.Begin(); }
		/** @brief Producer: the set naming every requirement, or pending (a batch posted for what it lacks). */
		SealedSet Seal(GrowthBuild&& a_build);
		/** @brief Producer: at least a_units of a_buffer (none: nothing asked). */
		void Require(GrowthBuild& a_build, const Versioned& a_buffer, std::uint64_t a_units);
		/** @brief Producer: a version of a_buffer newer than serial a_serial (a relayout at the same size). */
		void RequireNewer(GrowthBuild& a_build, const Versioned& a_buffer, std::uint64_t a_serial);
		/** @brief Producer: a_latch's block of at least a_bytes a slot, laid out as a_layoutKey (LatchLayoutKey; 0: a block of bytes alone). */
		static void Require(GrowthBuild& a_build, LatchHandle a_latch, std::uint32_t a_bytes, std::uint64_t a_layoutKey) { a_build.Require(a_latch, a_bytes, a_layoutKey); }
		/** @brief Producer: the units a_buffer's newest ready version holds. */
		std::uint64_t Units(const GrowthBuild& a_build, const Versioned& a_buffer);
		/** @brief Producer: the most asked of a_buffer (this build's requirement or a growth in flight), at least what it holds. */
		std::uint64_t Asked(const GrowthBuild& a_build, const Versioned& a_buffer);
		/** @brief The producer's report line, its counters reset; empty when nothing happened. */
		std::string Report();

	private:
		SceneBufferSet();
		org::services::GrowableBufferSet set;
		std::weak_ptr<org::runtime::IUploadService> uploads;  // the producer's
		std::uint32_t failuresLogged = 0;
	};

	/**
	 * @brief Buffers indexed alike (a slot of each), each with its own extent: one requirement fans out to every member, and what
	 * the group holds is the least any member's named version holds.
	 */
	struct UnitGroup
	{
		std::vector<Versioned> members;

		UnitGroup& Add(const Versioned& a_buffer)
		{
			if (a_buffer)
				members.push_back(a_buffer);
			return *this;
		}
		/** @brief Producer: at least a_units of every member. */
		void Require(GrowthBuild& a_build, std::uint64_t a_units) const
		{
			auto& buffers = SceneBufferSet::Get();
			for (const auto& member : members)
				buffers.Require(a_build, member, a_units);
		}
		/** @brief The units every member's version a_named names holds (0 without members); throws for a member it does not name. */
		std::uint64_t Units(const NamedVersions& a_named) const
		{
			std::uint64_t units = members.empty() ? 0 : ~0ull;
			for (const auto& member : members)
				units = std::min(units, a_named.Of(member).units);
			return units;
		}
		/** @brief Producer: the most asked of any member (SceneBufferSet::Asked). */
		std::uint64_t Asked(const GrowthBuild& a_build) const
		{
			std::uint64_t asked = 0;
			for (const auto& member : members)
				asked = std::max(asked, SceneBufferSet::Get().Asked(a_build, member));
			return asked;
		}
	};
}
