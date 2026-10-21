#pragma once

// Grass's channel and mirror: the engine's grass shapes by events (pushed by the feature's sink on GrassShared's capture,
// GrassHooks.cpp), and the render thread's mirror of them - shapes in slots, their records in ranges of a flat instance table, their
// buckets and meshes - for the Grass family's tables (GrassLayout.h), drained at the frame requests (the family's serve) and uploaded by
// the depth commit (GrassTables.cpp). Included by the feature's sources only.

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <utility>
#include <vector>

#include <winrt/base.h>

#include "Features/DrawcallLimitFix/Features/Grass/GrassLayout.h"
#include "Features/DrawcallLimitFix/Core/Runtime/EventQueue.h"

struct ID3D11Buffer;

namespace DCLF::Features::Grass
{
	/** @brief A mesh's source (a grass shape's renderer data, or a Grass Optimizations LOD mesh), as the capture or the library hold it. */
	struct MeshSource
	{
		const void* key = nullptr;  // the BSGraphics::TriShape, or the LOD mesh entry
		winrt::com_ptr<ID3D11Buffer> vertexBuffer, indexBuffer;  // held from the sink until the render thread leases them
		std::uint64_t vertexDesc = 0;
		std::uint32_t indexCount = 0;
	};

	/**
	 * @brief What makes shapes draw alike (one bucket): the mesh, the material and texture, the vertex layout, the descriptors' bits,
	 * the sampler's address mode and the alpha reference.
	 */
	struct BucketKey
	{
		const void* mesh = nullptr;
		const void* material = nullptr;
		const void* diffuse = nullptr;
		std::uint64_t vertexDesc = 0;
		std::uint32_t clampMode = 0;
		float alphaRef = 0.0f;
		bool pbr = false, sphereNormal = false, uniformScale = false;
		bool operator==(const BucketKey&) const = default;
	};

	struct BucketKeyHash
	{
		using is_avalanching = void;
		std::uint64_t operator()(const BucketKey& a_key) const noexcept;
	};

	/** @brief A bucket's material, as the capture read it (the textures referenced while the bucket lives). */
	struct BucketSource
	{
		RE::NiPointer<RE::NiSourceTexture> diffuse, normal, rmaos, features0;
		float boundCenter[3]{};
		float modelRadius = 0.0f;
		float wavePeriod = 1.0f;
		std::uint32_t triangleCount = 0;
		std::uint32_t pbrFlags = 0;
		float roughnessScale = 1.0f, specularLevel = 0.0f;
		float subsurface[4]{};
	};

	/**
	 * @brief A grass shape's records as the capture took them (kind Add, replacing what the mirror holds of the shape), its removal
	 * (Remove: every BSMultiStreamInstanceTriShape's destruction, grass or not), or a capture GrassShared could not read (Rejected: the
	 * engine still draws the shape, so the family must not be DCLF's while it lives).
	 */
	struct Event
	{
		enum class Kind : std::uint8_t
		{
			Add,
			Remove,
			Rejected
		};
		Kind kind = Kind::Add;
		std::uint64_t seq = 0;        // GrassShared's: orders a shape's events across the capture's threads
		const void* shape = nullptr;  // a key: never dereferenced by the drain (the shape may be dead)
		std::vector<GrassInstances::Instance> instances;
		std::array<float, 3> origin{};
		BucketKey key;
		BucketSource source;
		MeshSource mesh;
	};

	/**
	 * @brief The engine's grass shapes by events: pushed by the sink (any thread: Push), drained by the render thread. The sink is
	 * GrassShared's capture's (the feature's early hooks register it), which sets installed once it is in.
	 */
	inline EventQueue<Event> events;
	inline bool installed = false;

	/** @brief The sink's push, any thread: a_event onto events. */
	void Push(Event&& a_event);

	/**
	 * @brief One capture of a shape (a shape slot): its records, bucket, instance range and row. The engine adds a shape's instances in
	 * groups (DoneAddingInstances, the GID buffers and files), each one more capture of the same shape, as Grass Optimizations' slices.
	 */
	struct Group
	{
		std::vector<GrassInstances::Instance> instances;
		std::uint32_t bucket = 0;  // bucket slot
		std::uint32_t first = 0;   // its range in the instance table
		GrassInstances::ShapeRow row;
	};

	/** @brief A mirrored shape: its groups' slots, their records' total, and the last event's seq it applied. */
	struct Shape
	{
		std::vector<std::uint32_t> slots;
		std::uint32_t records = 0;
		std::uint64_t seq = 0;
	};

	/** @brief A mesh shared by buckets: its slot, its users, and its buffers' leases once resolved. */
	struct Mesh
	{
		std::uint32_t slot = 0;
		std::uint32_t users = 0;
		MeshSource source;
		GrassInstances::MeshRow row;
		std::shared_ptr<const void> vertexOwner, indexOwner;  // GpuResources leases
		bool resolved = false, failed = false;
	};

	/**
	 * @brief A bucket: its slot, its shapes and their instances (its tiers' capacity), its meshes per tier, its material, and the
	 * texture and sampler bindings its row names (the depth commit resolves them: bound, it holds their owners until it goes).
	 */
	struct Bucket
	{
		BucketKey key;
		BucketSource source;
		std::uint32_t slot = 0;
		std::uint32_t shapes = 0;
		std::uint32_t total = 0;
		std::array<const void*, GrassInstances::kTiers> meshes{};
		GrassInstances::BucketRow row;
		std::vector<std::shared_ptr<const void>> owners;  // the row's bindings
		std::uint32_t bindingGeneration = ~0u;           // GpuTextures::Generation the bindings were resolved in
		bool bound = false;
		bool live = false;  // a free slot's is not
	};

	struct Stats
	{
		std::uint64_t adds = 0, removes = 0, stale = 0, rejected = 0;
	};

	/**
	 * @brief The grass mirror: every grass shape the capture handed the sink, with its records, in slots for the GPU's tables. Render
	 * thread.
	 *
	 * Drain applies the events in seq order per shape: the queue drains its ring before its spill, so an event older than the one a
	 * shape last applied (or than its removal, kept a while: kTombstoneDrains) is dropped (stale). A remove of an unknown shape is
	 * nothing; an add of a known shape replaces it.
	 *
	 * The instance ranges come from a flat table by first fit (the free ranges coalesced); a freed range's owners are sent ~0u. When a
	 * bucket's instance total changes, every bucket's visible bases are laid out again (a prefix over the buckets, a range per tier
	 * with a mesh) and every bucket row is sent. The changes wait for the depth commit (TakeChanges), which uploads them.
	 */
	class Mirror
	{
	public:
		using Shapes = ankerl::unordered_dense::map<const void*, Shape>;
		using Buckets = std::vector<Bucket>;  // by bucket slot
		// Drains a removal's seq is kept for (the queue's reorder window is a drain or two).
		static constexpr std::uint32_t kTombstoneDrains = 16;

		void Drain(std::uint32_t a_frame);
		std::size_t Size() const { return shapes.size(); }
		std::uint64_t InstanceCount() const { return instanceCount; }
		std::size_t BucketCount() const { return bucketIndex.size(); }
		std::size_t MeshCount() const { return meshes.size(); }
		/** @brief Live shapes whose capture was rejected: the engine draws them, so the family stays the engine's. */
		std::size_t RejectedLive() const { return rejected.size(); }
		/** @brief The fade clock (the engine's frame time summed, as Grass Optimizations' timeAccum): the spawn fade's. */
		float FadeClock() const { return fadeClock; }
		/** @brief The adds, removes, stale drops and rejections since the last call (the report's). */
		Stats TakeStats() { return std::exchange(stats, {}); }
		/** @brief How much of the mirror can draw: its meshes by lease state, its buckets with a drawn tier 0 (the report's). */
		struct Readiness
		{
			std::uint32_t meshesResolved = 0, meshesFailed = 0, meshesPending = 0, bucketsDrawable = 0;
		};
		Readiness ReadinessOf() const;
		/** @brief The mirrored shapes by shape, and the buckets by slot (the parity's). */
		const Shapes& MirroredShapes() const { return shapes; }
		const Buckets& MirroredBuckets() const { return buckets; }

		/** @brief The slot tables' high-water marks: what the GPU's tables must hold. */
		std::uint32_t InstanceSlots() const { return instanceSlots; }
		std::uint32_t ShapeSlots() const { return static_cast<std::uint32_t>(slotShapes.size()); }
		std::uint32_t BucketSlots() const { return static_cast<std::uint32_t>(buckets.size()); }
		std::uint32_t MeshSlots() const { return static_cast<std::uint32_t>(meshSlots.size()); }
		/** @brief Every slot, bucket and mesh again (a new backing of the tables). */
		void MarkAllChanged();

		/** @brief What the depth commit sends: the changed slots of each table, each once. */
		struct Changes
		{
			std::vector<std::uint32_t> shapes, buckets, meshes;
			std::vector<std::pair<std::uint32_t, std::uint32_t>> freedRanges;  // instance ranges whose owners go ~0u
		};
		/**
		 * @brief The changes since the last call. Leases the new meshes' buffers first (GpuResources::Frame, the render thread's): a mesh
		 * that cannot be leased draws nothing, and its buckets' rows name mesh slot kNoMesh for its tier.
		 */
		void TakeChanges(Changes& a_changes);
		/** @brief A slot's row and records (count 0 when free). */
		const GrassInstances::ShapeRow& SlotRow(std::uint32_t a_slot) const;
		const std::vector<GrassInstances::Instance>* SlotInstances(std::uint32_t a_slot) const;
		const GrassInstances::MeshRow& MeshSlotRow(std::uint32_t a_slot) const;
		/** @brief A bucket slot's bucket (null when free), for the commit's row and bindings. */
		Bucket* SlotBucket(std::uint32_t a_slot);
		/** @brief A bucket slot's row again at the next commit (one whose bindings could not be resolved yet). */
		void MarkBucket(std::uint32_t a_slot);
		/** @brief Leases and bindings no table names any more, for the next execution to hold until it retires. */
		std::vector<std::shared_ptr<const void>> TakeRetired() { return std::exchange(retired, {}); }

	private:
		bool Stale(const void* a_shape, std::uint64_t a_seq);
		void Apply(Event&& a_event);
		void Remove(const void* a_shape);
		void ReleaseGroup(Group& a_group);
		std::uint32_t AcquireSlot();
		std::uint32_t AllocateRange(std::uint32_t a_count);
		void FreeRange(std::uint32_t a_first, std::uint32_t a_count);
		void MarkSlot(std::uint32_t a_slot);
		Bucket& AddBucketUser(Event& a_event);
		void RemoveBucketUser(std::uint32_t a_slot, std::uint32_t a_instances);
		void ResolveTiers(Bucket& a_bucket, const void* a_shape, MeshSource&& a_mesh);
		const void* AddMeshUser(MeshSource&& a_source);
		void RemoveMeshUser(const void* a_key);
		void LayOutVisible();
		void PruneTombstones();

		Shapes shapes;
		Buckets buckets;
		ankerl::unordered_dense::map<BucketKey, std::uint32_t, BucketKeyHash> bucketIndex;  // the live buckets' slots
		// Shape slots: each slot's shape (null when free), the free ones, the changed ones waiting for the commit.
		std::vector<const void*> slotShapes;
		std::vector<Group> groups;  // by shape slot
		std::vector<std::uint32_t> freeSlots, changedSlots;
		std::vector<std::uint8_t> slotChanged;
		// Bucket slots: the free ones, the changed ones.
		std::vector<std::uint32_t> freeBuckets, changedBuckets;
		std::vector<std::uint8_t> bucketChanged;
		bool totalsChanged = false;
		// Meshes by key, their slots.
		ankerl::unordered_dense::map<const void*, Mesh> meshes;
		std::vector<const void*> meshSlots;
		std::vector<std::uint32_t> freeMeshSlots, changedMeshes;
		// The instance table: its high-water mark, its free ranges (first -> count, coalesced), the freed ranges waiting for the commit.
		std::uint32_t instanceSlots = 0;
		std::uint64_t instanceCount = 0;
		std::map<std::uint32_t, std::uint32_t> freeRanges;
		std::vector<std::pair<std::uint32_t, std::uint32_t>> freedRanges;
		// Removed shapes' seqs (a stale add after its removal is dropped), with the drain they were removed in.
		ankerl::unordered_dense::map<const void*, std::pair<std::uint64_t, std::uint32_t>> tombstones;
		ankerl::unordered_dense::set<const void*> rejected;
		std::vector<std::shared_ptr<const void>> retired;
		std::uint32_t drains = 0;
		float fadeClock = 0.0f;
		Stats stats;
	};

	/**
	 * @brief The render thread's mirror (dclf-architecture.md, "Grass: the draws"): the engine's grass shapes and their records, by
	 * events. Drained at the frame requests, running or not (the family's serve); the depth commit uploads what it changed.
	 */
	Mirror& RenderMirror();
}
