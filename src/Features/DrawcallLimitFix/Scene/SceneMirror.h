#pragma once

#include "Features/DrawcallLimitFix/Engine/SceneCapture.h"

#include <ankerl/unordered_dense.h>

#include <array>
#include <span>
#include <string>

namespace DCLF
{
	/**
	 * @brief The coordinator's copy of the engine's scene (dclf-async-publication.md, "F3"): the records the hooks captured,
	 * applied in event order. Owned by the scene work.
	 *
	 * Step F3 builds it beside the live reads and checks it (CS_DCLF_MIRROR_PARITY): the render thread captures a slice of the
	 * tracked objects live at the frame's start (the probe), and the scene work compares it with the mirror after the frame's
	 * events. A field that differs is
	 * - evented: an event named the object since it was last captured (a hook that carries no value yet: F3b);
	 * - late: an event named it in the next batch (pushed after the drain);
	 * - missed: no event named it (a writer no hook sees: F3c).
	 */
	class SceneMirror
	{
	public:
		using KeySet = ankerl::unordered_dense::set<const void*>;

		/** @brief A capture's records, newest wins. */
		void Apply(const SceneCapture::Records& a_records);
		/** @brief A detached subtree: its root (taken off its parent's children) and every object under it. */
		void Detach(const void* a_root, std::span<RE::BSGeometry* const> a_geometries, std::span<const void* const> a_nodes);
		void Clear();

		const SceneCapture::NodeRecord* Node(const void* a_key) const;
		const SceneCapture::GeometryRecord* Geometry(const void* a_key) const;
		const SceneCapture::PropertyRecord* Property(const void* a_key) const;
		const SceneCapture::AlphaRecord* Alpha(const void* a_key) const;

		/**
		 * @brief The parity: the last probe's pending differences resolved by this batch's event keys (late or missed), then this
		 * probe against the mirror (a difference whose object an event of the probe's batch named is evented, else pending).
		 */
		void Check(const SceneCapture::Records& a_probe, const KeySet& a_eventKeys);
		/** @brief Since the last call: the records held and what the parity found. Empty when nothing to say. */
		std::string Report();

	private:
		template <class T>
		struct Counted
		{
			T record;
			std::uint32_t uses = 0;
		};
		void Use(const void* a_property, const void* a_layer, const void* a_alpha, int a_delta);

		ankerl::unordered_dense::map<const void*, SceneCapture::NodeRecord> nodes;
		ankerl::unordered_dense::map<const void*, SceneCapture::GeometryRecord> geometries;
		ankerl::unordered_dense::map<const void*, Counted<SceneCapture::PropertyRecord>> properties;
		ankerl::unordered_dense::map<const void*, Counted<SceneCapture::AlphaRecord>> alphas;

		// The parity, by record type (node, geometry, property, alpha) and field.
		static constexpr std::size_t kTypes = 4;
		struct Pending
		{
			std::uint8_t type = 0;
			std::uint32_t fields = 0;
			std::string what;
			std::uint32_t flagBits = 0;  // a node's: the flag bits that differ
		};
		// By object and record type (a geometry's node and geometry records share its key).
		ankerl::unordered_dense::map<std::uintptr_t, Pending> pending;
		// The objects an event named since their last capture (Apply takes them off): a field that differs on one has a hook.
		KeySet named;
		struct Tally
		{
			std::uint64_t probes = 0, checked = 0, absent = 0;
			std::array<std::array<std::uint64_t, 16>, kTypes> evented{}, late{}, missed{};
			std::string firstAbsent, firstMissed, firstEvented;
			// Per type and field, the first missed object; per node flag bit, how often it differed (missed, evented).
			std::array<std::array<std::string, 16>, kTypes> firstMissedBy;
			std::array<std::uint64_t, 32> flagBitsMissed{}, flagBitsEvented{};
		} tally;
		std::uint64_t applied = 0, detached = 0;
	};
}
