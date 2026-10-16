#pragma once

// The root states contract's Core side (Api/RootStates.h): the list view the producer fills from its family's columns (RootListView) and
// Core's readers take (Extensions::RootListOf: per decode, commit or parity check), what the root test is handed (RootTestInput), and the
// contribution's entry, resolved once at InstallFeatures (none registered: Extensions::RootStates() null), with the calls Core makes at
// its points (the join's root test, the membership witness, the frame's write-back task and its join).

#include "Features/DrawcallLimitFix/Api/RootStates.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Records.h"

#include <ankerl/unordered_dense.h>

#include <cstdint>
#include <span>
#include <string>

namespace DCLF
{
	struct SceneTables;
	struct FrameGlobals;
	namespace SceneCapture
	{
		struct NodeRecord;
	}
	namespace Extensions
	{
		class TableFamilies;
	}
}

namespace DCLF::Api
{
	/**
	 * @brief The listed roots of one tables, viewed in place (valid while those tables are held): by root slot, each root's row
	 * (FadeRootStatic, the layout the shaders mirror) and node (null: a free slot); the index from node to slot; by object slot, each
	 * object's root (the family's objectRow: kNoFadeRoot none); and the content epoch of the family's journal. The producer fills rows,
	 * nodes and index (RootStatesContribution::view); Core fills objectRoot and contentEpoch (Extensions::RootListOf). Empty without a
	 * producer or before its family's first write.
	 */
	class RootListView
	{
	public:
		std::span<const FadeRootStatic> rows;
		std::span<const void* const> nodes;
		const ankerl::unordered_dense::map<const void*, std::uint32_t>* index = nullptr;
		std::span<const std::uint32_t> objectRoot;
		std::uint64_t contentEpoch = 0;

		/** @brief How many root slots the tables hold (free ones included). */
		std::size_t Size() const { return rows.size(); }
		/** @brief a_node's root slot, kNoFadeRoot when it is not listed. */
		std::uint32_t Find(const void* a_node) const
		{
			if (!index)
				return kNoFadeRoot;
			const auto it = index->find(a_node);
			return it != index->end() ? it->second : kNoFadeRoot;
		}
		/** @brief Whether a_node is a listed root. */
		bool Contains(const void* a_node) const { return index && index->contains(a_node); }
		/** @brief a_object's root slot, kNoFadeRoot for none (or past the objects). */
		std::uint32_t RootOf(std::uint32_t a_object) const { return a_object < objectRoot.size() ? objectRoot[a_object] : kNoFadeRoot; }
		/** @brief a_root's node, null for a free slot or one past the slots. */
		const void* NodeOf(std::uint32_t a_root) const { return a_root < nodes.size() ? nodes[a_root] : nullptr; }
	};

	/**
	 * @brief What the root test is handed (RootStatesContribution::test, globalsKey): the root's fade node as the mirror holds it (its fade
	 * near, far, type and kind: no engine memory; null for globalsKey) and the frame's captured globals.
	 */
	class RootTestInput
	{
	public:
		const SceneCapture::NodeRecord* node = nullptr;
		const FrameGlobals& globals;
	};
}

namespace DCLF::Extensions
{
	/** @brief The contribution's entry: its registration (copied, its name owned). */
	struct RootStatesEntry
	{
		std::string name;
		Api::RootStatesContribution contribution;
	};

	/** @brief Main thread, the end of InstallFeatures: the entry from the registration (no name: none). */
	void ResolveRootStates(const Api::RootStatesContribution& a_registered);
	/** @brief Any thread, after InstallFeatures (null before): the contribution's entry, or null without one. */
	const RootStatesEntry* RootStates();

	/** @brief Main thread at PostPostLoad, from the producer's early hooks: its engine hooks' early part is in (the feature is listed). */
	void NoteRootStatesInstalled();
	/**
	 * @brief Main thread at PostPostLoad (PrimaryCull::Install, before InstallFeatures, so before RootStates()): whether
	 * NoteRootStatesInstalled came; the primary cull requires it.
	 */
	bool RootStatesInstalled();

	/**
	 * @brief Any thread holding a_tables, per decode, commit or parity check (never per object): the listed roots of a_tables (the
	 * producer's view of its family's columns, with the family's object references and journal epoch). Empty without a producer or before
	 * its family's first write.
	 */
	Api::RootListView RootListOf(const SceneTables& a_tables);
	Api::RootListView RootListOf(const TableFamilies& a_families);

	/**
	 * @brief Any thread under the accumulate phase's shard, per join: the root test of an object whose root is a_node (the fade node's
	 * mirror record) under a_globals, into a_out (zero without a producer).
	 */
	void RootStatesTest(const SceneCapture::NodeRecord& a_node, const FrameGlobals& a_globals, Api::RootTestOutput& a_out);
	/** @brief Per membership pass: the producer's globals mixed into a_witness (MembershipWitness's FNV-1a state); nothing without one. */
	void RootStatesGlobalsKey(const FrameGlobals& a_globals, std::uint32_t& a_witness);
	/** @brief The root test's record bits (Api::RootStatesContribution::objectFlags; 0 without a producer). */
	std::uint32_t RootStatesObjectFlags();
	/** @brief The producer's family (none without one). */
	Api::TableFamilyId RootStatesFamily();
	/** @brief Render thread, once a frame in the frame driver (where the fade write-back is kicked today): the producer's kick. */
	void RootStatesKick();
	/** @brief Render thread, IndirectDraws::DrainAsync after the builds ahead (never per frame): the producer's wait. */
	void RootStatesWait();
}
