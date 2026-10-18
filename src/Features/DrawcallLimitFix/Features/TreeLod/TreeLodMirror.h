#pragma once

// Tree LOD's channel and mirror: the engine's tree LOD groups by events (pushed by the feature's hooks, TreeLodHooks.cpp), and the render
// thread's mirror of them in slots for the TreeInstances family's tables (TreeInstancesLayout.h), drained at the
// frame requests (the family's serve) and uploaded by the depth commit (TreeLodTables.cpp). Included by the feature's sources only.

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <winrt/base.h>

#include "Features/DrawcallLimitFix/Features/TreeLod/TreeInstancesLayout.h"
#include "Features/DrawcallLimitFix/Core/Runtime/EventQueue.h"

struct ID3D11Buffer;

namespace DCLF::Features::TreeLod
{
	/*
	 * Tree LOD (AE 1.6.1170; dclf-architecture.md, "Tree LOD: what the engine does").
	 *
	 * Each BGSDistantTreeBlock's TreeGroup becomes one BSMultiStreamInstanceTriShape under the LOD trees root (0x14315b880),
	 * made by the block's attach (FUN_140503630) from its tree type's base shape and drawn instanced from one instance group.
	 * The refill (FUN_140504a40, from the attach and from the block's visibility update FUN_140503f70) packs at most 75 of the
	 * group's instances into 32-byte records and gives them to the shape's AddGroup (vfunc 0x1E0), which makes the group's
	 * vertex buffer; the update first removes the old group (RemoveGroup, vfunc 0x1E8), and so does the detach
	 * (FUN_1405033d0) before it detaches the shape. So the records the engine draws exist at those two calls only: the mirror
	 * takes them there, as events, and never reads the blocks per frame.
	 */

	/** @brief A shape's mesh (its tree type's base shape's renderer data), as AddGroup's shape holds it. */
	struct MeshSource
	{
		const void* key = nullptr;  // the BSGraphics::TriShape
		winrt::com_ptr<ID3D11Buffer> vertexBuffer, indexBuffer;  // held from the hook until the render thread leases them
		std::uint64_t vertexDesc = 0;
		std::uint32_t indexCount = 0;
	};

	/**
	 * @brief A group's records, as the shape's AddGroup took them or as the rule packs them after a block update (intended);
	 * or (kind Remove) its group removed.
	 */
	struct Event
	{
		enum class Kind : std::uint8_t
		{
			Add,
			Remove
		};
		const void* shape = nullptr;  // a key: never dereferenced by the drain
		const void* block = nullptr;  // BGSDistantTreeBlock and TreeGroup, for the parity's repack (Add)
		const void* group = nullptr;
		std::vector<TreeInstances::Instance> instances;
		Kind kind = Kind::Add;
		bool intended = false;  // packed by the rule after a block update, not given to AddGroup (TreeLodMirror.cpp, BlockUpdate)
		// An AddGroup's: the shape's placement, mesh and its type's extent (the larger of the type's width and height).
		std::array<float, 3> translate{};
		float extent = 0.0f;
		MeshSource mesh;
	};

	/**
	 * @brief The engine's tree LOD groups by events: pushed by the hooks (any thread: Push), drained by the render thread. The hooks
	 * (the refill's two calls, the block update's call, the shape's AddGroup and RemoveGroup slots) are the source of this channel, the
	 * feature's early hooks, which set installed once they are in.
	 */
	inline EventQueue<Event> events;
	inline bool installed = false;

	/** @brief The hooks' push, any thread: a_event onto events, counted when pushed off the drain's thread (TakeOffThread). */
	void Push(Event&& a_event);

	/** @brief A group's records packed by the refill's rule (FUN_140504a40), with the engine's own conversions. */
	void PackGroup(bool a_allVisible, const RE::BGSDistantTreeBlock::TreeGroup& a_group, std::vector<TreeInstances::Instance>& a_out);
	/** @brief The LOD trees root, the shapes' parent (0x14315b880). */
	RE::NiNode* LodTreesRoot();
	/** @brief The events the hooks pushed from another thread than the drain's since the last call (the report's). */
	std::uint64_t TakeOffThread();

	/**
	 * @brief A mirrored group: its shape's records by the refill's rule, the block and group they were packed from, and its slot.
	 * The records are the engine's, except after a block update where the engine keeps hidden records the rule shows
	 * (BlockUpdate).
	 */
	struct Group
	{
		const void* block = nullptr;
		const void* group = nullptr;
		std::vector<TreeInstances::Instance> instances;
		std::uint32_t slot = 0;
		const void* mesh = nullptr;
		TreeInstances::ShapeRow row;
	};

	/** @brief A mesh shared by the shapes of its tree type: its slot, its users, and its buffers' leases once resolved. */
	struct Mesh
	{
		std::uint32_t slot = 0;
		std::uint32_t users = 0;
		MeshSource source;
		TreeInstances::MeshRow row;
		std::shared_ptr<const void> vertexOwner, indexOwner;  // GpuResources leases
		bool resolved = false, failed = false;
	};

	struct Stats
	{
		std::uint64_t adds = 0, removes = 0, intended = 0;
	};

	/**
	 * @brief The tree LOD mirror: every tree LOD shape the engine has, with the records it draws, in slots for the GPU's tables.
	 * Render thread.
	 *
	 * Drain applies the events in push order (CS_DCLF_PERSISTENT_PARITY compares it with the LOD trees root's shapes:
	 * Diagnostics/TreeLodParity.cpp).
	 *
	 * The slots a drain changed (a shape's records or placement, a slot freed) and the meshes it added wait for the depth commit
	 * (TakeChanges), which uploads them.
	 */
	class Mirror
	{
	public:
		using Groups = ankerl::unordered_dense::map<const void*, Group>;

		void Drain(std::uint32_t a_frame);
		std::size_t Size() const { return groups.size(); }
		std::size_t InstanceCount() const;
		/** @brief The adds, removes and rule writes since the last call (the report's). */
		Stats TakeStats() { return std::exchange(stats, {}); }
		/** @brief The mirrored groups by shape (the parity's). */
		const Groups& MirroredGroups() const { return groups; }

		/** @brief The shape slots' and mesh slots' high-water marks: what the GPU's tables must hold. */
		std::uint32_t ShapeSlots() const { return static_cast<std::uint32_t>(slotGroups.size()); }
		std::uint32_t MeshSlots() const { return static_cast<std::uint32_t>(meshSlots.size()); }
		/** @brief Every slot and mesh again (a new backing of the tables). */
		void MarkAllChanged();
		/**
		 * @brief The changed shape slots and mesh slots since the last call, each once. Leases the new meshes' buffers first
		 * (GpuResources::Frame, the render thread's): a mesh that cannot be leased draws nothing, and its shapes' rows name mesh slot ~0u.
		 */
		void TakeChanges(std::vector<std::uint32_t>& a_slots, std::vector<std::uint32_t>& a_meshes);
		/** @brief A shape slot's row (count 0 when free) and records. */
		const TreeInstances::ShapeRow& SlotRow(std::uint32_t a_slot) const;
		const std::vector<TreeInstances::Instance>* SlotInstances(std::uint32_t a_slot) const;
		const TreeInstances::MeshRow& MeshSlotRow(std::uint32_t a_slot) const;
		/** @brief The largest index count of the meshes (the draw's vertices per instance). */
		std::uint32_t MaxIndices() const { return maxIndices; }
		/** @brief The leases of the meshes no shape draws any more, for the next execution to hold until it retires. */
		std::vector<std::shared_ptr<const void>> TakeRetired() { return std::exchange(retired, {}); }

	private:
		std::uint32_t AcquireSlot();
		void ReleaseSlot(std::uint32_t a_slot);
		void MarkSlot(std::uint32_t a_slot);
		const void* AddMeshUser(MeshSource&& a_source);
		void RemoveMeshUser(const void* a_key);

		Groups groups;
		// Slots: each slot's shape (null when free), the free ones, and the changed ones waiting for the commit.
		std::vector<const void*> slotGroups;
		std::vector<std::uint32_t> freeSlots;
		std::vector<std::uint32_t> changedSlots;
		std::vector<std::uint8_t> slotChanged;
		ankerl::unordered_dense::map<const void*, Mesh> meshes;
		std::vector<const void*> meshSlots;  // each mesh slot's key (null when free)
		std::vector<std::uint32_t> freeMeshSlots;
		std::vector<std::uint32_t> changedMeshes;
		std::vector<std::shared_ptr<const void>> retired;
		std::uint32_t maxIndices = 0;
		Stats stats;
	};

	/**
	 * @brief The render thread's mirror (dclf-architecture.md, "Tree LOD: the draws"): the engine's tree LOD groups and their instance records, by events. Drained at the
	 * frame requests, running or not (the family's serve); the depth commit uploads what it changed.
	 */
	Mirror& RenderMirror();
}
