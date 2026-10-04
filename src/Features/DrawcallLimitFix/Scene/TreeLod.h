#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <winrt/base.h>

#include "Features/DrawcallLimitFix/Common/EventQueue.h"

struct ID3D11Buffer;

namespace DCLF::TreeLod
{
	/*
	 * Tree LOD (AE 1.6.1170; dclf-lod.md, "Tree LOD: what the engine does").
	 *
	 * Each BGSDistantTreeBlock's TreeGroup becomes one BSMultiStreamInstanceTriShape under the LOD trees root (0x14315b880),
	 * made by the block's attach (FUN_140503630) from its tree type's base shape and drawn instanced from one instance group.
	 * The refill (FUN_140504a40, from the attach and from the block's visibility update FUN_140503f70) packs at most 75 of the
	 * group's instances into 32-byte records and gives them to the shape's AddGroup (vfunc 0x1E0), which makes the group's
	 * vertex buffer; the update first removes the old group (RemoveGroup, vfunc 0x1E8), and so does the detach
	 * (FUN_1405033d0) before it detaches the shape. So the records the engine draws exist at those two calls only: the mirror
	 * takes them there, as events, and never reads the blocks per frame.
	 */
	inline constexpr std::uint32_t kMaxGroupInstances = 75;  // the refill's cap (0x4B), and the property's (+0x88)
	inline constexpr std::uint32_t kInstanceShorts = 16;     // shorts per instance record (AddGroup's p2)

	/**
	 * @brief One instance record, as the refill packs it (FUN_140504a40), halfs unless noted:
	 * [0..2] position (InstanceData x, y, z, block-relative); [3] scale, 0 when hidden (the block's allVisible ignores hidden);
	 * [4] cos and [5] sin of rotZ; [6] alpha (InstanceData::alpha, copied); [7] 1.0; [8..15] zero.
	 * DistantTree.hlsl reads it as TEXCOORD4 (position, scale) and TEXCOORD5 (cos, sin, alpha, 1).
	 */
	struct Instance
	{
		std::array<std::uint16_t, kInstanceShorts> shorts{};
		bool operator==(const Instance&) const = default;
	};
	static_assert(sizeof(Instance) == 32);

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
		std::vector<Instance> instances;
		Kind kind = Kind::Add;
		bool intended = false;  // packed by the rule after a block update, not given to AddGroup (TreeLod.cpp, BlockUpdate)
		// An AddGroup's: the shape's placement, mesh and its type's extent (the larger of the type's width and height).
		std::array<float, 3> translate{};
		float extent = 0.0f;
		MeshSource mesh;
	};

	/** @brief The engine's tree LOD groups by events: pushed by the hooks (any thread), drained by the render thread. */
	inline EventQueue<Event> events;
	inline bool installed = false;

	/** @brief Patches the refill's two calls, the block update's call, and the shape's AddGroup and RemoveGroup slots. */
	bool Install();

	/** @brief Whether a geometry is a tree LOD shape: its shader property is a BSDistantTreeShaderProperty. Any thread. */
	bool IsTreeLodShape(const RE::BSGeometry* a_shape);

	/*
	 * The GPU's tables (TreeLodCullCS.hlsl, DistantTree.hlsl's DCLF_PULLED path), all read through device addresses:
	 * - a shape row per shape slot, and kMaxGroupInstances instance records per shape slot (slot * 75 + i);
	 * - a mesh row per mesh slot;
	 * - the draw row, one, every depth commit's: the tables' addresses and the frame's texture, sampler and alpha reference;
	 * - the visible list: the draw's arguments (a non-indexed DrawInstanced: the meshes' largest index count, the visible count),
	 *   then the visible instances' indices, which the cull appends and the draws' vertex stage reads by its instance.
	 */
	struct ShapeRow
	{
		float translate[3]{};
		std::uint32_t mesh = 0;
		std::uint32_t count = 0;  // the slot's records; 0 for a free slot
		float extent = 0.0f;      // the type's: an instance's bound is its scale times this, about its position
		std::uint32_t pad[2]{};
	};
	static_assert(sizeof(ShapeRow) == 32);

	struct MeshRow
	{
		std::uint64_t vertexAddress = 0;
		std::uint64_t indexAddress = 0;  // R16 indices
		std::uint32_t vertexStride = 0;
		std::uint32_t indexCount = 0;
		std::uint32_t texcoordOffset = 0;  // TEXCOORD0's, a half2; the position is a float4 at 0 (VertexInput.cpp)
		std::uint32_t pad = 0;
	};
	static_assert(sizeof(MeshRow) == 32);

	struct DrawRow
	{
		std::uint64_t instances = 0, shapes = 0, meshes = 0, visible = 0;  // visible: the list's indices (past the arguments)
		std::uint32_t textureIndex = 0, samplerIndex = 0;
		float alphaRef = 0.0f;
		std::uint32_t shapeSlots = 0;
		std::uint32_t maxIndices = 0;
		std::uint32_t pad[3]{};
	};
	static_assert(sizeof(DrawRow) == 64);
	inline constexpr std::uint32_t kVisibleHeaderWords = 4;  // the draw's arguments: vertices per instance, instances, 0, 0

	/**
	 * @brief A mirrored group: its shape's records by the refill's rule, the block and group they were packed from, and its slot.
	 * The records are the engine's, except after a block update where the engine keeps hidden records the rule shows
	 * (BlockUpdate).
	 */
	struct Group
	{
		const void* block = nullptr;
		const void* group = nullptr;
		std::vector<Instance> instances;
		std::uint32_t slot = 0;
		const void* mesh = nullptr;
		ShapeRow row;
	};

	/** @brief A mesh shared by the shapes of its tree type: its slot, its users, and its buffers' leases once resolved. */
	struct Mesh
	{
		std::uint32_t slot = 0;
		std::uint32_t users = 0;
		MeshSource source;
		MeshRow row;
		std::shared_ptr<const void> vertexOwner, indexOwner;  // GpuResources leases
		bool resolved = false, failed = false;
	};

	struct Stats
	{
		std::uint64_t adds = 0, removes = 0, checks = 0, shapes = 0, stale = 0, missing = 0, differ = 0, late = 0, intended = 0, unsettled = 0;
		std::string first;
	};

	/**
	 * @brief The tree LOD mirror: every tree LOD shape the engine has, with the records it draws, in slots for the GPU's tables.
	 * Render thread.
	 *
	 * Drain applies the events in push order. On parity frames (CS_DCLF_PERSISTENT_PARITY) the LOD trees root's shapes are
	 * compared with it: a shape it lacks, a shape it holds that the root does not, and records that differ from the group's
	 * instances packed again by the refill's rule (a group whose shaderPropertyUpToDate is clear is skipped: its refill is due).
	 * The refills run on the terrain manager's threads, so a group can be refilled between a drain and the check: a shape that
	 * differs counts as late when its Add arrives at the next drain, and as differing only when it does not.
	 *
	 * The slots a drain changed (a shape's records or placement, a slot freed) and the meshes it added wait for the depth commit
	 * (TakeChanges), which uploads them.
	 */
	class Mirror
	{
	public:
		void Drain(std::uint32_t a_frame);
		std::size_t Size() const { return groups.size(); }
		std::size_t InstanceCount() const;
		/** @brief The report's line, then the counters reset. */
		std::string Report();

		/** @brief The shape slots' and mesh slots' high-water marks: what the GPU's tables must hold. */
		std::uint32_t ShapeSlots() const { return static_cast<std::uint32_t>(slotGroups.size()); }
		std::uint32_t MeshSlots() const { return static_cast<std::uint32_t>(meshSlots.size()); }
		/** @brief Every slot and mesh again (a new backing of the tables). */
		void MarkAllChanged();
		/**
		 * @brief The changed shape slots and mesh slots since the last call, each once. Leases the new meshes' buffers first
		 * (GpuResources): a mesh that cannot be leased draws nothing, and its shapes' rows name mesh slot ~0u.
		 */
		void TakeChanges(std::vector<std::uint32_t>& a_slots, std::vector<std::uint32_t>& a_meshes);
		/** @brief A shape slot's row (count 0 when free) and records. */
		const ShapeRow& SlotRow(std::uint32_t a_slot) const;
		const std::vector<Instance>* SlotInstances(std::uint32_t a_slot) const;
		const MeshRow& MeshSlotRow(std::uint32_t a_slot) const;
		/** @brief The largest index count of the meshes (the draw's vertices per instance). */
		std::uint32_t MaxIndices() const { return maxIndices; }
		/** @brief The leases of the meshes no shape draws any more, for the next execution to hold until it retires. */
		std::vector<std::shared_ptr<const void>> TakeRetired() { return std::exchange(retired, {}); }

	private:
		void CheckParity();
		std::uint32_t AcquireSlot();
		void ReleaseSlot(std::uint32_t a_slot);
		void MarkSlot(std::uint32_t a_slot);
		const void* AddMeshUser(MeshSource&& a_source);
		void RemoveMeshUser(const void* a_key);

		ankerl::unordered_dense::map<const void*, Group> groups;
		ankerl::unordered_dense::map<const void*, std::string> suspects;  // differed at the last check, with what
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
		std::uint32_t parityFrame = ~0u;
	};
}
