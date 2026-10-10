#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <variant>
#include <vector>

#include "Features/DrawcallLimitFix/Scene/LodSegments.h"

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
		kKindDecalNode = 1u << 9,  // T6b1a: a BGSDecalNode (its RTTI exactly: what its OnVisible is)
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
			kExtra = 1u << 16,       // T6b1a: a fade node's BSX flags; the reference's emittance source; an actor's race keyword (BeastRaceFace)
			kFadeCurrent = 1u << 17, // T6b1a: a fade node's currentFade (+0x130): the cull's and the fade update's, every frame while it fades
			kFadeLevel = 1u << 18,   // T6b1a: its LOD level (+0x152 & 0xF): FUN_14147a430's
			kFadeDoor = 1u << 19,    // T6b1a: its screen-door byte (+0x154)
			kFadeAmount = 1u << 20,  // T6b1a: a fade node's fadeAmount (+0x100): Actor::SetAlpha's
			kDecals = 1u << 21,      // T6b1a: a decal node's decals' 3D, in its array's order (the decal array's edits)
			kFieldCount = 22,
		};
		static constexpr std::array<const char*, kFieldCount> kFieldNames{ "parent", "rtti", "kind", "hidden", "flags", "user data", "controllers",
			"body", "children", "switch", "fade near", "fade far", "fade +0x109", "fade type", "tree LOD switch", "name", "extra", "current fade", "LOD level",
			"screen door", "fade amount", "decals" };

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
		// A node's children by slot (T6b1a: the engine's slots, which do not move, a null slot null; no trailing null): what its
		// OnVisible visits, and in that order.
		std::vector<const void*> children;
		std::int32_t switchIndex = -1;
		std::uint16_t switchFlags = 0;
		bool switchCurrent = false;
		const void* switchChild = nullptr;  // children[switchIndex]
		// A decal node's decals' 3D (BSTempEffect::Get3D, null for none), in its array's order: its OnVisible culls them last to first,
		// and not its children (T6b1a).
		std::vector<const void*> decals;
		// kExtra (T6b1a). bsx: the BSX extra data's value on a fade node, -1 none. emittance: the reference (userData) has an
		// ExtraEmittanceSource with a source. beast: BeastRaceFace's answer for the reference (true without an actor or a race).
		std::int32_t bsx = -1;
		bool emittance = false;
		bool beast = true;
		float currentFade = 1.0f;
		std::uint8_t fadeLevel = 0, fadeDoor = 0;
		float fadeAmount = 1.0f;
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
			kExtra = 1u << 7,       // T6b1a: the AnisotropicAlphaMaterial extra data (ExtendedTranslucency::MaterialModelOf)
			kMultiParams = 1u << 8, // T6b1a: a multi-index shape's material projection, parameters, scale and normal dampener
			kSegments = 1u << 9,    // T6b1a: a sub-index shape's drawn ranges (LodSegments::DrawnRanges)
			kFieldCount = 10,
		};
		static constexpr std::array<const char*, kFieldCount> kFieldNames{ "type", "renderer", "skin", "dismember", "property", "alpha", "layer", "extra",
			"multi-index parameters", "segments" };

		struct Partition
		{
			const void* rendererData = nullptr;  // T6b1b: its buffData (a geometry slot's key)
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
		const void* altIndexList = nullptr;  // T6b1b: the second index list itself (its first qword the buffer: the layer's slot key)
		ID3D11Buffer* altIndexBuffer = nullptr;
		std::uint32_t altPrimCount = 0;
		// kExtra: the extra data's value (an NiIntegerExtraData's), kNoExtra none, kWrongExtra another type.
		static constexpr std::int64_t kNoExtra = -1, kWrongExtra = -2;
		std::int64_t anisotropic = kNoExtra;
		// kMultiParams: materialProjection (16), materialParams (4), materialScale, normalDampener.
		std::array<float, 22> multiParams{};
		// kSegments: what the engine's draw of a sub-index shape draws now (compared by triangles: the draw's rebuild of the runs moves
		// their starts, not their triangles).
		std::vector<LodSegments::Range> segments;

		// The capture that took it (SceneCapture::NextSequence at its start): not compared.
		std::uint64_t sequence = 0;

		std::uint32_t Differ(const GeometryRecord& a_other) const;
		void Assign(const GeometryRecord& a_from, std::uint32_t a_fields);
		/** @brief The layer the engine draws (LayerPropertyOf's): the additional property, with an alt index buffer and its count. */
		const void* Layer() const { return altIndexBuffer && altPrimCount ? layerProperty : nullptr; }
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
			kAlphaValue = 1u << 8,  // T6b1a: BSShaderProperty::alpha, a Lighting property's (1 for any other)
			kProjected = 1u << 9,   // T6b1a: projectedUVParams and projectedUVColor
			kLandBlend = 1u << 10,  // T6b1a: a landscape material's landBlendParams
			kShadowPasses = 1u << 11,  // T6b1a: whether a Lighting property's shadow pass list (shadowMapOrMaskPasses) has a head
			kFieldCount = 12,
		};
		static constexpr std::array<const char*, kFieldCount> kFieldNames{ "rtti", "flags", "material", "material alpha", "material other", "fade node",
			"emissive", "controllers", "alpha", "projected UV", "land blend", "shadow passes" };

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
		float alpha = 1.0f;
		std::array<float, 8> projected{};  // projectedUVParams, projectedUVColor
		std::array<float, 4> landBlend{};
		bool shadowPasses = false;

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
			kRtti = 1u << 2,         // T6b1a
			kControllers = 1u << 3,  // T6b1a
			kFieldCount = 4,
		};
		static constexpr std::array<const char*, kFieldCount> kFieldNames{ "flags", "threshold", "rtti", "controllers" };

		const void* key = nullptr;
		std::uint16_t flags = 0;
		std::uint8_t threshold = 0;
		const void* rtti = nullptr;
		bool controllers = false;

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
		// T6b1c: an attach's capture (CaptureAttached) holds every node and geometry it recorded, ancestors included, taken on the
		// capturing thread while the engine's attach holds them: the scene work makes its references from these (SceneStore::Pinned),
		// never from a key. Released with the batch, on the render thread (ReleaseHandedBack). A probe's or a leaf update's holds none
		// (a leaf update's event holds its own: SceneTracker::Event::pins). With the nodes, each geometry's properties (T6b1b).
		bool pinning = false;
		std::vector<RE::NiPointer<RE::NiRefObject>> pins;
		bool Empty() const { return nodes.empty(); }
	};

	/**
	 * @brief T6b1b: a geometry's records as the classification and the record's writer read them: the mirror's on the scene work
	 * (SceneMirror::Leaf), a live capture's on the render thread (LiveLeaf). Null where the geometry names none (or the mirror holds none).
	 */
	struct LeafView
	{
		const NodeRecord* node = nullptr;          // the geometry's node half
		const NodeRecord* parent = nullptr;        // its parent's
		const GeometryRecord* geometry = nullptr;
		const PropertyRecord* property = nullptr;  // its shader property's
		const PropertyRecord* layer = nullptr;     // the layer the engine draws (GeometryRecord::Layer)
		const AlphaRecord* alpha = nullptr;
		const NodeRecord* fadeNode = nullptr;      // its shader property's fade node's
		const NodeRecord* layerFadeNode = nullptr; // its layer's fade node's
		explicit operator bool() const { return node && geometry; }
		/** @brief a_property's fade node's record (a_property the leaf's property or layer). */
		const NodeRecord* FadeNodeOf(const PropertyRecord* a_property) const { return a_property == property ? fadeNode : a_property == layer ? layerFadeNode : nullptr; }
		std::uint8_t Type() const { return geometry ? geometry->type : 0; }
		bool Skinned() const { return geometry && geometry->skin; }
		// NiAlphaProperty::GetAlphaBlending, GetAlphaTesting.
		bool AlphaBlending() const { return alpha && (alpha->flags & 1u); }
		bool AlphaTesting() const { return alpha && (alpha->flags & (1u << 9)); }
		/** @brief a_property when it is a BSLightingShaderProperty's (netimmerse_cast's answer), else null. */
		static const PropertyRecord* Lighting(const PropertyRecord* a_property) { return a_property && a_property->lighting ? a_property : nullptr; }
	};

	/** @brief A geometry's records captured live and held, with their view: for a reader on the render thread. */
	struct LiveLeaf
	{
		explicit LiveLeaf(const RE::BSGeometry& a_geometry);
		LiveLeaf(const LiveLeaf&) = delete;
		LiveLeaf& operator=(const LiveLeaf&) = delete;
		Records records;
		LeafView view;
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
		// T6b1b: a patched hidden store's (the hidden event rides in its mirror update, one queue: HiddenStoreSiteAt's index), else ~0u.
		std::uint32_t hiddenSite = ~0u;
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
	/** @brief The geometry's records (its node, itself, its properties) into a_out, and with a_pins its properties held there. */
	void CaptureLeaf(const RE::BSGeometry& a_geometry, Records& a_out, std::vector<RE::NiPointer<RE::NiRefObject>>* a_pins = nullptr);

	/** @brief Whether a_object is reached from Main::WorldRootNode by its parent chain. */
	bool InWorld(const RE::NiAVObject* a_object);
	/**
	 * @brief The parentless roots the portal graph draws (alwaysRenderChildren: no parent, under no world root), which InWorld counts as
	 * the world's from now on (T6b1b: the mirror holds what the scene work tracks under them). Render thread (CaptureCategories); the
	 * hooks' threads read the set lock-free. A set replaced is kept (a few dozen pointers a cell's graph): a reader may still hold it.
	 */
	void SetDrawnRoots(std::vector<const RE::NiAVObject*> a_roots);
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
