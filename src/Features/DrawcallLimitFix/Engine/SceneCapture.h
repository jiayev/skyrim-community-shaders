#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <variant>
#include <vector>

struct ID3D11Buffer;

namespace DCLF::SceneCapture
{
	/**
	 * @brief Values of the engine's scene objects, as the scene work reads them (dclf-async-publication.md, "F").
	 *
	 * The scene work is to read no engine memory: what it reads becomes a record, captured by the hook that changed it on the
	 * writer's thread, carried by the event, and applied in order into the coordinator's mirror (SceneMirror). The same capture
	 * functions take the parity's probe at the frame's start (CS_DCLF_MIRROR_PARITY), so a record and the live object compare
	 * field by field, and a field that differs with no event names a writer the hooks miss.
	 *
	 * Only what holds still between its writers' events is a record: per-frame values (transforms, bounds, currentFade, the LOD
	 * level, palettes) are FrameValues' or their events'. A record holds no reference: keys are addresses, and the hooks capture
	 * only what is in the world (InWorld), whose detach drops the records again (SceneTracker).
	 */

	enum NodeKind : std::uint16_t
	{
		kKindGeometry = 1u << 0,
		kKindNode = 1u << 1,
		kKindSwitch = 1u << 2,
		kKindBillboard = 1u << 3,
		kKindOrdered = 1u << 4,
		kKindMultiBound = 1u << 5,
		kKindFadeNode = 1u << 6,
		kKindFaceGen = 1u << 7,
		kKindTree = 1u << 8,
	};

	struct NodeRecord
	{
		enum Field : std::uint32_t
		{
			kParent = 1u << 0,
			kRtti = 1u << 1,
			kKind = 1u << 2,
			kHidden = 1u << 3,       // flags bit 0
			kFlags = 1u << 4,        // the flag bits the scene work reads that hold still (RecordFlags)
			kUserData = 1u << 5,     // the reference, its form type, whether an actor
			kControllers = 1u << 6,
			kBody = 1u << 7,         // a rigid body whose motion is not fixed
			kChildren = 1u << 8,
			kSwitch = 1u << 9,       // index, flags, the selected child's revision current
			kFadeNear = 1u << 10,
			kFadeFar = 1u << 11,
			kFade109 = 1u << 12,
			kFadeType = 1u << 13,    // the LOD type (+0x153 & 0xF)
			kTreeLodSwitch = 1u << 14,
			kName = 1u << 15,        // for the logs only: not compared (Differ)
			kFieldCount = 16,
		};
		static constexpr std::array<const char*, kFieldCount> kFieldNames{ "parent", "rtti", "kind", "hidden", "flags", "user data", "controllers",
			"body", "children", "switch", "fade near", "fade far", "fade +0x109", "fade type", "tree LOD switch", "name" };

		// The flag bits a record holds besides kHidden: those the scene work reads that hold still between their writers. A
		// geometry's kMeshLOD (12, set by its loader). Not records (FrameValues', step F4): the bits the frame's culls and updates
		// write - kTopFadeNode (14, the fade's faded-in), kIgnoreFade (15, settled), kRenderUse (22), kAccumulated (26), 27 (the LOD
		// step), and on a fade root kAlwaysDraw (11: TESWaterReflections::Update), kPreProcessedNode (12: BSFadeNodeCuller::Process1)
		// and 20 (HighActorCuller::Operate), which FadeState::StaticOf reads - and the bits the scene work never reads (17, ...).
		static constexpr std::uint32_t RecordFlags(std::uint16_t a_kind) { return (a_kind & kKindGeometry) ? (1u << 12) : 0u; }

		const void* key = nullptr;
		const void* parent = nullptr;
		const void* rtti = nullptr;
		std::uint16_t kind = 0;
		std::uint32_t flags = 0;
		const void* userData = nullptr;
		std::uint8_t formType = 0;
		bool actor = false;
		bool controllers = false;
		bool body = false;
		std::vector<const void*> children;  // a node's non-null children, in order
		std::int32_t switchIndex = -1;
		std::uint16_t switchFlags = 0;
		bool switchCurrent = false;
		float fadeNear = 0.0f, fadeFar = 0.0f;
		std::uint8_t fade109 = 0, fadeType = 0;
		const void* treeLodSwitch = nullptr;
		const char* name = nullptr;
		// Diagnostics, not compared: the frame of the capture and of the last update (Frame()), and the capturing thread.
		std::uint32_t capturedFrame = 0, updatedFrame = 0, thread = 0;

		// The capture that took it (SceneCapture::NextSequence at its start): not compared.
		std::uint64_t sequence = 0;

		std::uint32_t Differ(const NodeRecord& a_other) const;
		/** @brief a_from's a_fields into this record (an update's). */
		void Assign(const NodeRecord& a_from, std::uint32_t a_fields);
	};

	struct GeometryRecord
	{
		enum Field : std::uint32_t
		{
			kType = 1u << 0,
			kRenderer = 1u << 1,    // renderer data, its buffers and vertex layout, the counts
			kSkin = 1u << 2,        // instance, its type, partition, data, bone count, the partitions' buffers, layouts, counts, LOD bytes
			kDismember = 1u << 3,   // the dismember partitions' shown flags
			kProperty = 1u << 4,
			kAlpha = 1u << 5,
			kLayer = 1u << 6,       // the additional property, the alt index buffer and its count
			kFieldCount = 7,
		};
		static constexpr std::array<const char*, kFieldCount> kFieldNames{ "type", "renderer", "skin", "dismember", "property", "alpha", "layer" };

		struct Partition
		{
			ID3D11Buffer* vertexBuffer = nullptr;
			ID3D11Buffer* indexBuffer = nullptr;
			std::uint64_t vertexDesc = 0;
			std::uint16_t vertices = 0, triangles = 0;
			std::uint8_t lodByte = 0;
			bool operator==(const Partition&) const = default;
		};

		const void* key = nullptr;
		std::uint8_t type = 0;
		const void* rendererData = nullptr;
		ID3D11Buffer* vertexBuffer = nullptr;
		ID3D11Buffer* indexBuffer = nullptr;
		std::uint64_t vertexDesc = 0;
		std::uint32_t vertexCount = 0, triangleCount = 0;
		const void* skin = nullptr;
		const void* skinRtti = nullptr;
		const void* skinPartition = nullptr;
		const void* skinData = nullptr;
		std::uint32_t boneCount = 0;
		std::vector<Partition> partitions;
		std::vector<std::uint8_t> shown;  // a dismember skin's partitions' editorVisible
		const void* property = nullptr;
		const void* alpha = nullptr;
		const void* layerProperty = nullptr;
		ID3D11Buffer* altIndexBuffer = nullptr;
		std::uint32_t altPrimCount = 0;

		// The capture that took it (SceneCapture::NextSequence at its start): not compared.
		std::uint64_t sequence = 0;

		std::uint32_t Differ(const GeometryRecord& a_other) const;
		void Assign(const GeometryRecord& a_from, std::uint32_t a_fields);
	};

	struct PropertyRecord
	{
		enum Field : std::uint32_t
		{
			kRtti = 1u << 0,
			kFlags = 1u << 1,
			kMaterial = 1u << 2,
			kMaterialAlpha = 1u << 3,
			kMaterialOther = 1u << 4,  // the material's feature, PBR glints, the diffuse view
			kFadeNode = 1u << 5,
			kEmissive = 1u << 6,
			kControllers = 1u << 7,
			kFieldCount = 8,
		};
		static constexpr std::array<const char*, kFieldCount> kFieldNames{ "rtti", "flags", "material", "material alpha", "material other", "fade node",
			"emissive", "controllers" };

		const void* key = nullptr;
		const void* rtti = nullptr;
		bool lighting = false;
		std::uint64_t flags = 0;
		const void* material = nullptr;
		float materialAlpha = 1.0f;
		std::uint32_t feature = 0;
		bool glints = false;
		const void* diffuseView = nullptr;
		const void* fadeNode = nullptr;
		const void* emissive = nullptr;
		bool controllers = false;

		// The capture that took it (SceneCapture::NextSequence at its start): not compared.
		std::uint64_t sequence = 0;
		// Diagnostics, not compared: the frame and thread of the capture, and the last writer in the mirror (1 a capture, 2 an
		// update's fields), its frame.
		std::uint32_t capturedFrame = 0, thread = 0, writtenFrame = 0;
		std::uint8_t writer = 0;

		std::uint32_t Differ(const PropertyRecord& a_other) const;
		void Assign(const PropertyRecord& a_from, std::uint32_t a_fields);
	};

	struct AlphaRecord
	{
		enum Field : std::uint32_t
		{
			kFlags = 1u << 0,
			kThreshold = 1u << 1,
			kFieldCount = 2,
		};
		static constexpr std::array<const char*, kFieldCount> kFieldNames{ "flags", "threshold" };

		const void* key = nullptr;
		std::uint16_t flags = 0;
		std::uint8_t threshold = 0;

		// The capture that took it (SceneCapture::NextSequence at its start): not compared.
		std::uint64_t sequence = 0;

		std::uint32_t Differ(const AlphaRecord& a_other) const;
		void Assign(const AlphaRecord& a_from, std::uint32_t a_fields);
	};

	/** @brief What one capture took: nodes (geometries' node half included), geometries, and the properties they name. */
	struct Records
	{
		std::vector<NodeRecord> nodes;
		std::vector<GeometryRecord> geometries;
		std::vector<PropertyRecord> properties;
		std::vector<AlphaRecord> alphas;
		// NextSequence at the capture's start: an update numbered after it may have written after the capture read the object.
		std::uint64_t sequence = 0;
		bool Empty() const { return nodes.empty(); }
	};

	/**
	 * @brief A hook's values (step F3b): the fields its writer changed, captured after the write on the writer's thread, applied
	 * into the mirror's record in push order when the mirror holds one (an object out of the world has none: its attach captures
	 * it whole). The record carries only a_fields' values and its key.
	 */
	struct Update
	{
		std::uint32_t fields = 0;
		// A geometry's update may name it by its skin alone (key null, skin set: the dismember setter knows only the skin).
		std::variant<NodeRecord, PropertyRecord, AlphaRecord, GeometryRecord> record;
		// A geometry's new property or alpha (a swap): its whole leaf (CaptureLeaf), applied as a capture when the mirror holds the
		// geometry, so the new property's record comes with it and the uses are counted.
		std::shared_ptr<const Records> leaf;
		// NextSequence after the write (SceneTracker::PushUpdate takes it): an update numbered before a capture's start is in it.
		std::uint64_t sequence = 0;
	};

	/**
	 * @brief The order of captures and updates (step F3c). A capture on a loader thread takes milliseconds; a writer on another
	 * thread can change an object it already read before its attach event is pushed, and that update, pushed first, would be
	 * applied before the stale capture. The mirror applies a capture, then the recent updates numbered after its start, and skips
	 * an update numbered before the capture of the record it names.
	 */
	std::uint64_t NextSequence();

	NodeRecord CaptureNode(const RE::NiAVObject& a_object);
	/** @brief Only a_fields of a node (no children list): a hook's update. A fade field needs a BSFadeNode. */
	NodeRecord CaptureNodeFields(const RE::NiAVObject& a_object, std::uint32_t a_fields);
	GeometryRecord CaptureGeometry(const RE::BSGeometry& a_geometry);
	PropertyRecord CaptureProperty(const RE::BSShaderProperty& a_property);
	AlphaRecord CaptureAlpha(const RE::NiAlphaProperty& a_alpha);
	/** @brief The geometry's records (its node, itself, its properties) into a_out. */
	void CaptureLeaf(const RE::BSGeometry& a_geometry, Records& a_out);

	/** @brief Whether a_object is reached from Main::WorldRootNode by its parent chain. */
	bool InWorld(const RE::NiAVObject* a_object);
	/**
	 * @brief The hook's capture of an attach (on the attaching thread, after the engine's call): every object under a_root and the
	 * ancestors up to the world's root (their children lists changed). Null when a_root is not in the world: a subtree still being
	 * assembled is captured whole when it is attached to the world.
	 */
	std::unique_ptr<Records> CaptureAttached(const RE::NiAVObject& a_root);

	/** @brief The hooks' capture counters, since the last call: captures, records, the time, and how many on the main thread. */
	struct Counters
	{
		std::uint64_t attaches = 0, records = 0, ns = 0, mainThread = 0, mainThreadNs = 0, outOfWorld = 0;
	};
	Counters TakeCounters();
	void NoteCapture(std::uint64_t a_records, std::uint64_t a_ns);
	void NoteOutOfWorld();
	/** @brief The main thread (the render thread), recorded at the frame's start, for the counters. */
	void SetMainThread(std::uint32_t a_thread);
	/** @brief The render thread's frame (set with SetMainThread), for the records' diagnostics. */
	void SetFrame(std::uint32_t a_frame);
	std::uint32_t Frame();
}
