#pragma once

// The scene revisions' participants: each shape kind's owner (ShapeKind: the main segments and the views, Impl's; the shadow epoch, the
// occlusion maps' views in it, the shadow engine's), with the steps the revision producer (MakeRevision), the revision inputs
// (PostRevisionInputs) and the builds ahead (PostAheadContext, RunAhead) take of each. Registered at Impl's construction, one per ShapeKind in its order
// (Impl::participants), the order every loop calls them in, which is the order these steps ran in when the producer named each kind.
// Called per make, post and build ahead, never per object.
// Part of IndirectDraws' implementation: included by Impl.h, before the struct.

namespace DCLF
{
	/**
	 * @brief A make (MakeRevision, the snapshot builder's): the producer's state, its publication (what its shapes are made from), and
	 * what the main participant's requirements hand the others' and the shapes (the participants' own scratch is theirs).
	 */
	struct RevisionMake
	{
		ImplProducer& producer;
		const ScenePublication& publication;
		const SceneTables& tables;
		const Lookups& lookups;
		const PipelineCatalog* catalog = nullptr;
		IndirectState indirect;
		// By draw family, its pipelines as the catalog has them, and whether it has them.
		std::array<FamilyPipelines, Extensions::kDrawFamilies> familyPipelines;
		std::array<bool, Extensions::kDrawFamilies> familyOn{};
		std::array<bool, kViews> viewOn{};
		std::array<ZBucketPlan, 2> zPlans;
		// The colour segment's state class sub-ranges (PlanSetClasses), from the main layout RequireMain left and indirect's classes.
		std::shared_ptr<const SetClassPlan> classPlan;
		std::array<std::vector<LatchedCopy>, 2> mainLatched;
		// The lookups' entries the requirements and the shapes read of the pipeline slots, as one witness (LookupsWitness, Producer.cpp).
		std::uint64_t lookupsWitness = 0;
	};

	namespace Draws
	{
		/** @brief Producer: a latched-copies block holding a_copies (none: nothing asked). Revision/Producer.cpp. */
		void RequireLatchedBlock(GrowthBuild& a_build, LatchHandle a_latch, const std::vector<LatchedCopy>& a_copies);

		/** @brief RevisionShapesKey's hash: each value mixed into the current group's word. */
		struct ShapesKeyMixer
		{
			ImplProducer::ShapesKey& key;
			std::size_t group = 0;
			void Mix(std::uint64_t a_value)
			{
				auto& h = key[group];
				h = (h ^ a_value) * 0x100000001b3ull;
				h ^= h >> 29;
			}
			void Ptr(const void* a_pointer) { Mix(reinterpret_cast<std::uintptr_t>(a_pointer)); }
			template <class T>
			void Raw(const T& a_value)
			{
				static_assert(std::is_trivially_copyable_v<T>);
				std::array<std::byte, (sizeof(T) + 7) / 8 * 8> bytes{};
				std::memcpy(bytes.data(), &a_value, sizeof(T));
				for (std::size_t b = 0; b < bytes.size(); b += 8) {
					std::uint64_t word;
					std::memcpy(&word, bytes.data() + b, 8);
					Mix(word);
				}
			}
		};
	}

	/**
	 * @brief A build ahead's (RunAhead's, the snapshot builder's) once its main build finished: what a participant's payload is built
	 * from, and the publication it goes in.
	 */
	struct AheadBuild
	{
		ImplSnapshotBuilder::DrawPublication& out;
		const ImplSnapshotBuilder::AheadContext& context;
		const SceneTables& tables;
		const Lookups& lookups;
		std::uint32_t generation = 0, frame = 0;
		const std::shared_ptr<const EntrySpace>& sunCandidates;
		const std::shared_ptr<const EntrySpace>& lightCandidates;
		const TablesContentEpochs& oldestTables;
		const PayloadBuilder& payloads;  // the payloads' pools: their journals' oldest content epochs
		ResidentRegion& mainRegion;      // the main build's region (BuildCache::region), as it left it
		MainRows::PairStats& stats;      // the stages' times
	};

	class RevisionParticipant
	{
	public:
		/**
		 * @brief The snapshot builder (MakeRevision): what the set must hold for its shapes - its buffers' sequences and counts, its latch
		 * layout reserved (grown) - from the make's publication. The views' after the others', so they read the main layout and pipelines.
		 */
		virtual void RequireSequences(RevisionMake& a_make, GrowthBuild& a_build) = 0;
		/** @brief The snapshot builder: the latched-copies blocks its commits latch into, after every participant's sequences. */
		virtual void RequireLatched(RevisionMake&, GrowthBuild&) {}
		/** @brief The snapshot builder: the set sealed, before the shapes key: what it notes of the make (a bound its shapes outgrew). */
		virtual void NoteSealed(RevisionMake&) {}
		/** @brief The snapshot builder: its parts of the shapes key (RevisionShapesKey), each into its group. */
		virtual void MixShapesKey(const RevisionMake& a_make, Draws::ShapesKeyMixer& a_mixer) const = 0;
		/** @brief The snapshot builder: its shapes (ImplProducer::RevisionProducer::made), from the sealed set and the main layout. */
		virtual void MakeShapes(RevisionMake& a_make, const NamedVersions& a_sealed, const MainLayout& a_layout) = 0;
		/** @brief Render thread (PostRevisionInputs): its part of the revision inputs, as the frame has it now. */
		virtual void PostInputs(RevisionInputs&) {}
		/** @brief Render thread (PostAheadContext, the frame's start): its part of the ahead context the next builds take. */
		virtual void PrepareAhead(ImplSnapshotBuilder::AheadContext&) {}
		/**
		 * @brief The snapshot builder (RunAhead): the half of its payload that needs no main build, ahead of BuildPayload (which runs it
		 * itself when RunAhead did not). It reads of a_build only the stream views (out.streams, made), the context, the tables, the
		 * lookups, the frame and generation, the candidates, the oldest content epochs (oldestTables, payloads: read only) and adds to
		 * its own stats; it never touches mainRegion or the main payloads, and writes nothing into a_build.out (BuildPayload does), so it
		 * may run beside the main build. Every call is followed by BuildPayload of the same build ahead (one abandoned is carried into the
		 * next build's).
		 */
		virtual void BuildPayloadShared(const AheadBuild&) {}
		/** @brief The snapshot builder (RunAhead, after the main build): its payload of the publication, into a_build.out. */
		virtual void BuildPayload(const AheadBuild&) {}

	protected:
		~RevisionParticipant() = default;
	};
}
