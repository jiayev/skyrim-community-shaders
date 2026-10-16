#pragma once

// The ancestor value contract's Core side (Api/AncestorValues.h): the contribution's entry, resolved once at InstallFeatures (its ancestor
// classes' RTTI called then; none registered: no key walk, no map, the column none), and the sink its fill hands the map through.

#include "Features/DrawcallLimitFix/Api/AncestorValues.h"

#include <ankerl/unordered_dense.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace DCLF::Extensions
{
	/** @brief The map an ancestor value's fill hands: key node to value (the copy the render thread posts to the scene lane). */
	using AncestorValueMap = ankerl::unordered_dense::map<const void*, std::int32_t>;
}

namespace DCLF::Api
{
	/** @brief Where an ancestor value's fill puts the map: Core's copy, posted once filled (FrameView's accumulate inputs). */
	class AncestorMapSink
	{
	public:
		explicit AncestorMapSink(Extensions::AncestorValueMap& a_out) :
			out(a_out) {}

		/** @brief Room for a_count entries (the feature's map's size), before they are put. */
		void Reserve(std::size_t a_count) { out.reserve(a_count); }
		/** @brief a_node's value (a node put twice keeps its first). */
		void Put(const void* a_node, std::int32_t a_value) { out.emplace(a_node, a_value); }

	private:
		Extensions::AncestorValueMap& out;
	};
}

namespace DCLF::Extensions
{
	/** @brief The contribution's entry (registered false: none, every field its default). */
	struct AncestorValueEntry
	{
		bool registered = false;
		std::string name;
		std::uint32_t recordWord = 0;
		std::int32_t none = -1;
		// The key classes' RTTI (a node whose mirrored RTTI is either is a key; null: unused), called once at the resolution.
		std::array<const void*, 2> ancestorRtti{};
		Api::ClassifyInputId whileOn;
		decltype(Api::AncestorValueContribution::generation) generation = nullptr;
		decltype(Api::AncestorValueContribution::fill) fill = nullptr;

		/** @brief Whether a node of RTTI a_rtti is a key. */
		bool IsKey(const void* a_rtti) const { return a_rtti && (a_rtti == ancestorRtti[0] || a_rtti == ancestorRtti[1]); }
	};

	/** @brief Main thread, the end of InstallFeatures: the entry from the registration (no name: none), its RTTI called. Once. */
	void ResolveAncestorValue(const Api::AncestorValueContribution& a_registered);
	/** @brief Any thread, after InstallFeatures (null before): the contribution's entry, or null without one. */
	const AncestorValueEntry* AncestorValue();
	/**
	 * @brief Any thread, after InstallFeatures (the scene pass's snapshot of the inputs while one is installed): whether the value is on,
	 * a contribution registered and its whileOn input on. Off: no post, no lookup, the column none.
	 */
	bool AncestorValueOn();
}
