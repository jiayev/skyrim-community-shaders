#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <vector>

namespace DCLF::Published
{
	struct Identity
	{
		std::uint64_t id = 0, incarnation = 0;
		bool operator==(const Identity&) const = default;
	};

	// These are captured evaluated outputs, not pointers to engine materials/passes.
	// Their concrete binary schemas are supplied by the future engine adapter.
	enum class ComponentKind : std::uint8_t { Geometry, Material, Pose, Face };
	struct ComponentInput
	{
		Identity member;
		ComponentKind kind;
		std::span<const std::byte> values;
	};
	struct MemberInput
	{
		Identity identity;
		std::array<float, 12> world{};
		bool skinned = false, face = false;
	};
	struct ResourceLease
	{
		Identity identity;
		std::uint64_t version = 0;
		// Lifetime only. The adapter must guarantee immutable/versioned content or
		// explicitly ordered patches; const void does not freeze a GPU resource.
		std::shared_ptr<const void> owner;
	};
	struct CapturedComponent
	{
		Identity member;
		ComponentKind kind;
		std::vector<std::byte> values;
	};

	// Factory copies values and membership; no mutable alias to this page escapes.
	// Every mesh requires Geometry+Material, skins additionally Pose, faces Face.
	// All components must come from this ONE engine-safe source update.
	class ConsistencyGroupUpdate final
	{
	public:
		static std::shared_ptr<const ConsistencyGroupUpdate> Capture(Identity group, std::uint64_t lifecycle, std::uint64_t sourceUpdate,
			std::span<const MemberInput>, std::span<const ComponentInput>, std::span<const ResourceLease>);
		Identity Group() const { return group; }
		std::uint64_t Lifecycle() const { return lifecycle; }
		std::uint64_t SourceUpdate() const { return sourceUpdate; }
		const auto& Members() const { return members; }
		const auto& Components() const { return components; }
		const auto& Resources() const { return resources; }
		std::size_t OwnedBytes() const;
		bool CanRefresh(const ConsistencyGroupUpdate& previous) const;
	private:
		ConsistencyGroupUpdate() = default;
		Identity group;
		std::uint64_t lifecycle = 0, sourceUpdate = 0;
		std::vector<MemberInput> members;
		std::vector<CapturedComponent> components;
		std::vector<ResourceLease> resources;
	};

	// ReplaceGroup includes membership/resource/material changes and is ordered.
	// RefreshGroup changes only transform/pose/face values of an existing group.
	enum class SceneEventKind : std::uint8_t { ReplaceGroup, DetachGroup, Reset, RefreshGroup };
	struct CapturedSceneEvent
	{
		SceneEventKind kind;
		std::uint64_t sequence = 0, lifecycle = 0;
		Identity group;
		std::shared_ptr<const ConsistencyGroupUpdate> update;
	};
	struct CapturedSceneUpdate
	{
		// Consumed in order, never coalesced across lifecycle events. This envelope
		// is passed by value; group pages are immutable owning values.
		std::vector<CapturedSceneEvent> events;
	};
	struct CapturedSceneState
	{
		std::uint64_t lifecycle = 0, sequence = 0;
		std::map<std::uint64_t, std::shared_ptr<const ConsistencyGroupUpdate>> groups;
	};

	// Coordinator-only offline derivation. Not a render-thread mailbox or GPU
	// publication: readiness, resource imports and ownership admission come later.
	class CapturedSceneReducer
	{
	public:
		struct Result { std::size_t applied = 0, rejected = 0; };
		Result Apply(CapturedSceneUpdate);
		std::shared_ptr<const CapturedSceneState> Snapshot() const { return current; }
	private:
		struct Head { std::uint64_t incarnation = 0, source = 0; bool detached = false; };
		std::shared_ptr<const CapturedSceneState> current = std::make_shared<const CapturedSceneState>();
		std::map<std::uint64_t, Head> heads;
	};
}
