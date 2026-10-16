#pragma once

// The root states contract (Api::InstallContext::RegisterRootStates): the one producer of the listed roots' OnVisible state - the engine
// nodes (fade nodes) whose state decides whether, and how faded, the objects under them draw. The producer lists the roots in a table
// family (Api/TableFamilies.h; every object's reference into it is its root), steps their states on the GPU (a GPU stage at
// DepthSegmentCullState) and binds the buffers Core's cull reads (Api/CullInputs.h's Root* cull bindings). Core keeps the vocabulary
// they share: the root rows' and states' layouts (Records.h's FadeRootStatic and FadeNodeState, mirrored by the shaders), the record's
// root reference, and every reader of them (the main cull, the shadows, the focus views, the LOD skins, the draws, the visibility
// events). Core reaches the producer per event through the callbacks below and per frame and per object only as data (the family's
// object references, the bound buffers, the root test's object flags). Without one the primary cull stays off.

#include "Features/DrawcallLimitFix/Api/TableFamilies.h"

#include <cstdint>
#include <string_view>

namespace DCLF::Api
{
	// Defined by Core (Core/Extensions/RootStates.h), which the features inside DCLF include for them.
	class RootListView;
	class RootTestInput;

	/** @brief What the root test answers for an object under a root (RootStatesContribution::test). */
	struct RootTestOutput
	{
		float distance = 0.0f;         // the fade-out distance BuildDraws tests from the root's centre (0: none)
		std::uint32_t objectFlags = 0;  // the root test's record bits the object carries (within RootStatesContribution::objectFlags)
	};

	/**
	 * @brief The root states. One contribution: a second fails the install.
	 *
	 * - name: the contribution's (the install's refusals).
	 * - family: the table family listing the roots, registered before: its objectRow is every object's root (Core reads it as data, the
	 *   record's root reference among it: Api::TableFamily::recordShift).
	 * - view: per decode, commit or parity check, never per object (any thread holding a_columns' tables): the roots' rows, nodes and
	 *   index in a_columns (the family's columns) into a_out (Extensions::RootListOf).
	 * - objectFlags: the record bits the root test sets (Records.h, kFeatureObjectFlags: the fade test and the tree height test).
	 * - test: per join (the accumulate phase's, any thread under its shard), never per frame: an object under a_input's root node, its
	 *   fade-out distance and object flags.
	 * - globalsKey (optional): per membership pass, into the membership globals' witness (a 32-bit FNV-1a state: mix each word as
	 *   witness = (witness ^ word) * 16777619), from a_input's globals alone (no node): what test reads of them, so a change rebinds
	 *   every object.
	 * - kick (optional): render thread, once a frame in the frame driver, after the next builds' ahead context is posted (where the
	 *   visibility readbacks are kicked): the producer's frame task on DCLF's executor (the write-back of the states to the engine
	 *   nodes), never joined per frame. A task still running hands what remains to the next.
	 * - wait (optional): render thread, when DCLF's async work is drained (IndirectDraws::DrainAsync, after the builds ahead; never per
	 *   frame): its task joined.
	 */
	struct RootStatesContribution
	{
		std::string_view name;
		TableFamilyId family;
		void (*view)(const void* a_columns, RootListView& a_out) = nullptr;
		std::uint32_t objectFlags = 0;
		void (*test)(const RootTestInput& a_input, RootTestOutput& a_out) = nullptr;
		void (*globalsKey)(const RootTestInput& a_input, std::uint32_t& a_witness) = nullptr;
		void (*kick)() = nullptr;
		void (*wait)() = nullptr;
	};
}
