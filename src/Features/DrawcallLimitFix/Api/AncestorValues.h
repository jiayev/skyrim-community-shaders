#pragma once

// The ancestor value contract (Api::InstallContext::RegisterAncestorValue): a per-object value another CS feature keys by an engine node
// above the object (Integrations/LightLimitFix's room index: the nearest BSMultiBoundRoom or BSPortalSharedNode), which every DCLF draw
// reads from the object's record. Core keeps the key walk (by the mirror's parents and RTTI, at the scene's prep and attachment), the
// map's post to the scene lane and its take, the lookup at a join when the map moved on, and the record's column; the feature hands the
// map. Nothing runs per frame per object. One contribution: a second fails the install. Without one the column holds none.

#include "Features/DrawcallLimitFix/Api/ClassifyInputs.h"

#include <array>
#include <cstdint>
#include <string_view>

namespace DCLF::Api
{
	// Defined by Core (Core/Extensions/AncestorValues.h), which the features inside DCLF include for it.
	class AncestorMapSink;

	/**
	 * @brief The ancestor value contribution.
	 *
	 * - name: the contribution's (the install's failures).
	 * - recordWord: the object record's word the value is written to (DCLFObjectRecord's; Features/Manifest.cpp checks it).
	 * - none: the value of an object with no keyed ancestor, or no entry in the map, and of every object while whileOn is off.
	 * - ancestorRtti: called once at install: the node classes (their RTTI exactly; one may be null) whose nearest instance above the object
	 *   is its key.
	 * - whileOn: a registered classify input; off: no map is posted and no lookup made.
	 * - generation: render thread, once a frame where the accumulate inputs are posted (PostAccumulateInputs), while whileOn: the map's
	 *   generation, changing when fill would hand another map.
	 * - fill: render thread, when generation moved: the map (key node, value) into a_map, whose copy is posted (the feature's own map is
	 *   the render thread's).
	 */
	struct AncestorValueContribution
	{
		std::string_view name;
		std::uint32_t recordWord = 0;
		std::int32_t none = -1;
		std::array<const void* (*)(), 2> ancestorRtti{};
		ClassifyInputId whileOn;
		std::uint64_t (*generation)() = nullptr;
		void (*fill)(AncestorMapSink& a_map) = nullptr;
	};
}
