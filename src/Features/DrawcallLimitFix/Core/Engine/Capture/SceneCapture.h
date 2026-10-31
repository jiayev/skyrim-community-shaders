#pragma once

// The scene's records: engine values the scene work reads, captured on the writer's thread (hooks) or the render thread (frame
// requests) and carried by SceneTracker's events into the coordinator's mirror (SceneMirror).

#include <array>
#include <d3d11.h>
#include <winrt/base.h>
#include <cstdint>
#include <memory>
#include <variant>
#include <vector>

#include "Features/DrawcallLimitFix/Api/EntryRanges.h"

struct ID3D11Buffer;

namespace DCLF::SceneCapture
{
	/**
	 * @brief Engine scene values as the scene work reads them (dclf-architecture.md, "The scene mirror"). The scene work reads no engine memory:
	 * the hook that changed a value captures it on the writer's thread and the event applies it, in order, to the mirror. The
	 * mirror parity (CS_DCLF_MIRROR_PARITY) probes with the same capture functions, so a field differing with no event names a
	 * missed writer.
	 *
	 * Only what holds still between writer events is a record; per-frame values (transforms, bounds, currentFade, LOD level,
	 * palettes) are FrameValues'. Records hold no references: keys are addresses, and only in-world objects (InWorld) are captured;
	 * their detach drops the records.
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
		kKindCaptured = 1u << 9,  // a node capture contribution's class (its RTTI exactly: Extensions::NodeCaptureOf): its list captured
		// With kKindBillboard: the face mode (NiBillboardNode::userFlags & 7, +0x128, what OnVisible turns it by) in bits 13-15.
		kKindBillboardModeShift = 13,
	};
	/** @brief A billboard node record's face mode (kKindBillboardModeShift). */
	inline constexpr std::uint32_t BillboardModeOf(std::uint16_t a_kind) { return (a_kind >> kKindBillboardModeShift) & 7u; }

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
			kExtra = 1u << 16,       // a fade node's BSX flags; the reference's emittance source; the reference facts (Api::ReferenceFact)
			kFadeCurrent = 1u << 17, // a fade node's currentFade (+0x130): the cull's and the fade update's, every frame while it fades
			kFadeLevel = 1u << 18,   // its LOD level (+0x152 & 0xF): FUN_14147a430's
			kFadeDoor = 1u << 19,    // its screen-door byte (+0x154)
			kFadeAmount = 1u << 20,  // a fade node's fadeAmount (+0x100): Actor::SetAlpha's
			kCaptured = 1u << 21,    // a contributed node's list (Api::kNodeFieldCaptured: its writers' edits, the feature's hooks)
			kFieldCount = 22,
		};
		// kCaptured keeps the parities' name for it ("decals": its one contribution's, Features/Decals).
		static constexpr std::array<const char*, kFieldCount> kFieldNames{ "parent", "rtti", "kind", "hidden", "flags", "user data", "controllers",
			"body", "children", "switch", "fade near", "fade far", "fade +0x109", "fade type", "tree LOD switch", "name", "extra", "current fade", "LOD level",
			"screen door", "fade amount", "decals" };

		// Flag bits recorded besides kHidden: a geometry's kMeshLOD (12, set by its loader). Not recorded (FrameValues'): bits the
		// culls and updates write - kTopFadeNode (14, faded-in), kIgnoreFade (15, settled), kRenderUse (22), kAccumulated (26), 27
		// (LOD step), and on a fade root kAlwaysDraw (11: TESWaterReflections::Update), kPreProcessedNode (12:
		// BSFadeNodeCuller::Process1) and 20 (HighActorCuller::Operate), read by FadeState::StaticOf - and bits never read (17, ...).
		static constexpr std::uint32_t RecordFlags(std::uint16_t a_kind) { return (a_kind & kKindGeometry) ? (1u << 12) : 0u; }

		const void* key = nullptr;
		const void* parent = nullptr;
		const void* rtti = nullptr;
		std::uint16_t kind = 0;
		std::uint32_t flags = 0;
		const void* userData = nullptr;
		std::uint8_t formType = 0;
		bool actor = false;
		/** @brief Whether the record's reference is an ActorCharacter (by form type). */
		bool IsActorCharacter() const { return userData && formType == static_cast<std::uint8_t>(RE::FormType::ActorCharacter); }
		bool controllers = false;
		bool body = false;
		// Children by engine slot (stable; null slots kept, no trailing null): what OnVisible visits, in order.
		std::vector<const void*> children;
		std::int32_t switchIndex = -1;
		std::uint16_t switchFlags = 0;
		bool switchCurrent = false;
		const void* switchChild = nullptr;  // children[switchIndex]
		// kKindCaptured: the list its node capture contribution captures (Api::NodeCaptureContribution: what OnVisible visits in the
		// children's place, in the contribution's order).
		std::vector<const void*> captured;
		// kExtra. bsx: a fade node's BSX value, -1 none. emittance: the reference has an ExtraEmittanceSource with a source. facts:
		// the reference facts, a bit by Api::ReferenceFactId (Extensions::ReferenceFactsOf; each fact's none without a reference).
		std::int32_t bsx = -1;
		bool emittance = false;
		std::uint32_t facts = ~0u;
		float currentFade = 1.0f;
		std::uint8_t fadeLevel = 0, fadeDoor = 0;
		float fadeAmount = 1.0f;
		float fadeNear = 0.0f, fadeFar = 0.0f;
		std::uint8_t fade109 = 0, fadeType = 0;
		const void* treeLodSwitch = nullptr;
		const char* name = nullptr;
		// Diagnostics, not compared: capture and last-update frames, capturing thread.
		std::uint32_t capturedFrame = 0, updatedFrame = 0, thread = 0;

		// NextSequence at the capture's start; not compared.
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
			kSkin = 1u << 2,        // instance, type, partition, data, bone count, partitions' buffers, layouts, counts, LOD bytes
			kDismember = 1u << 3,   // the dismember partitions' shown flags
			kProperty = 1u << 4,
			kAlpha = 1u << 5,
			kLayer = 1u << 6,       // the additional property, the alt index buffer and its count
			kExtra = 1u << 7,       // the declared geometry extra data (Api: DeclareGeometryExtra; Extended Translucency's AnisotropicAlphaMaterial)
			kMultiParams = 1u << 8, // a multi-index shape's material projection, parameters, scale and normal dampener
			kRanges = 1u << 9,      // a ranged type's drawn ranges (Api::GeometryTypeContribution::ranges: object LOD's segments)
			kFieldCount = 10,
		};
		static constexpr std::array<const char*, kFieldCount> kFieldNames{ "type", "renderer", "skin", "dismember", "property", "alpha", "layer", "extra",
			"multi-index parameters", "segments" };

		struct Partition
		{
			const void* rendererData = nullptr;  // its buffData (a geometry slot's key)
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
		const void* altIndexList = nullptr;  // the second index list (its first qword is the buffer: the layer's slot key)
		ID3D11Buffer* altIndexBuffer = nullptr;
		std::uint32_t altPrimCount = 0;
		// kExtra: the extra data's value (an NiIntegerExtraData's), kNoExtra none, kWrongExtra another type.
		static constexpr std::int64_t kNoExtra = -1, kWrongExtra = -2;
		std::int64_t extraValue = kNoExtra;
		// kMultiParams: materialProjection (16), materialParams (4), materialScale, normalDampener.
		std::array<float, 22> multiParams{};
		// kRanges: what the engine's draw of a ranged type draws (compared by triangles: object LOD's rebuild moves run starts, not
		// triangles).
		std::vector<Api::EntryRange> ranges;
		/**
		 * @brief kRenderer: a particle geometry's (NiParticles, types 1 and 2) inputs to PaletteItemClassOf
		 * (Core/Shading/LightingDescriptors.h); the particles are ParticleSnapshots'. vertexDesc is the geometry's own
		 * (+0x148: position and colour on stream 1, quad UVs on stream 0).
		 */
		struct Particles
		{
			bool system = false;            // exactly an NiParticleSystem (not NiMeshParticleSystem, BSStripParticleSystem)
			bool worldSpace = false;        // NiParticleSystem::isWorldspace (BSParticleSystemManager's)
			bool data = false;              // NiParticles::particleData set
			bool aligned = false;           // the data's aspect flags (+0x88, +0x89): velocity-aligned or speed-stretched quads
			bool velocities = false;        // the data is an NiPSysData with particle info (aligned quads' velocities)
			std::uint16_t capacity = 0;     // NiParticlesData::maxNumVertices
			std::uint32_t subtextures = 0;  // NiParticlesData::subTextureOffsetsCount (FUN_1414bde90's TexCoordIndex and IndexedTexture)
			bool operator==(const Particles&) const = default;
		};
		Particles particles;

		// NextSequence at the capture's start; not compared.
		std::uint64_t sequence = 0;

		std::uint32_t Differ(const GeometryRecord& a_other) const;
		void Assign(const GeometryRecord& a_from, std::uint32_t a_fields);
		/** @brief The layer the engine draws (LayerPropertyOf): the additional property with an alt index buffer and count. */
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
			kMaterialOther = 1u << 4,  // the material's feature, the contributor bits, the diffuse view
			kFadeNode = 1u << 5,
			kEmissive = 1u << 6,
			kControllers = 1u << 7,
			kAlphaValue = 1u << 8,  // BSShaderProperty::alpha, a Lighting property's (1 for any other)
			kProjected = 1u << 9,   // projectedUVParams and projectedUVColor
			kLandBlend = 1u << 10,  // a landscape material's landBlendParams
			kShadowPasses = 1u << 11,  // whether a Lighting property's shadow pass list (shadowMapOrMaskPasses) has a head
			kFieldCount = 12,
		};
		static constexpr std::array<const char*, kFieldCount> kFieldNames{ "rtti", "flags", "material", "material alpha", "material other", "fade node",
			"emissive", "controllers", "alpha", "projected UV", "land blend", "shadow passes" };

		const void* key = nullptr;
		const void* rtti = nullptr;
		bool lighting = false;
		// With kRtti, a material port's property (its exact RTTI: Extensions::PortOfRtti), its kind (0: none): emissive and feature are
		// what the port's capture takes (the Effect port's: the emittanceColor, +0x88, null for white; the material's texture paths,
		// Features/Effects' kEffectHasSourcePath and kEffectHasGreyscalePath).
		std::uint8_t port = 0;
		std::uint64_t flags = 0;
		const void* material = nullptr;
		float materialAlpha = 1.0f;
		std::uint32_t feature = 0;
		// A Lighting property's with a material: the property bits' captures (Api::PropertyBits; TruePBR's glints).
		std::uint8_t contributorBits = 0;
		const void* diffuseView = nullptr;
		// A reference on diffuseView, taken while the material holds it: a texture swap may free the view before the scene work
		// binds it (SharedBindings). Not compared; assigned with diffuseView.
		winrt::com_ptr<ID3D11ShaderResourceView> diffuseHeld;
		const void* fadeNode = nullptr;
		const void* emissive = nullptr;
		bool controllers = false;
		float alpha = 1.0f;
		std::array<float, 8> projected{};  // projectedUVParams, projectedUVColor
		std::array<float, 4> landBlend{};
		bool shadowPasses = false;

		// NextSequence at the capture's start; not compared.
		std::uint64_t sequence = 0;
		// Diagnostics, not compared: capture frame and thread; the mirror's last writer (1 capture, 2 update) and its frame.
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
			kRtti = 1u << 2,
			kControllers = 1u << 3,
			kFieldCount = 4,
		};
		static constexpr std::array<const char*, kFieldCount> kFieldNames{ "flags", "threshold", "rtti", "controllers" };

		const void* key = nullptr;
		std::uint16_t flags = 0;
		std::uint8_t threshold = 0;
		const void* rtti = nullptr;
		bool controllers = false;

		// NextSequence at the capture's start; not compared.
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
		// An attach's capture holds every node and geometry it recorded (ancestors included) and their properties, taken while the
		// engine's attach holds them: the scene work's references come from these (Coordinator::Pinned), never from a key. Released
		// with the batch on the render thread (EngineReleases). Probes and leaf updates hold none (SceneTracker::Event::pins).
		bool pinning = false;
		std::vector<RE::NiPointer<RE::NiRefObject>> pins;
		bool Empty() const { return nodes.empty(); }
	};

	/**
	 * @brief A geometry's records: the mirror's on the scene work (SceneMirror::Leaf), a live capture's on the render thread
	 * (LiveLeaf). Null where none.
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
		/** @brief The alpha's flags and threshold in one word (0 without one): what a join's static flags take from it. */
		std::uint32_t AlphaState() const { return alpha ? (static_cast<std::uint32_t>(alpha->flags) << 8) | alpha->threshold : 0u; }
		/** @brief a_property if a BSLightingShaderProperty (netimmerse_cast), else null. */
		static const PropertyRecord* Lighting(const PropertyRecord* a_property) { return a_property && a_property->lighting ? a_property : nullptr; }
		/** @brief a_property if a material port's (PropertyRecord::port: the Effect port's BSEffectShaderProperty), else null. */
		static const PropertyRecord* Port(const PropertyRecord* a_property) { return a_property && a_property->port ? a_property : nullptr; }
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
	 * @brief A hook's values: the fields its writer changed, captured after the write, applied to the mirror's record in push order
	 * when one exists (an object out of the world has none; its attach captures it whole).
	 */
	struct Update
	{
		std::uint32_t fields = 0;
		// A geometry's update may name it by its skin alone (key null, skin set: the dismember setter knows only the skin).
		std::variant<NodeRecord, PropertyRecord, AlphaRecord, GeometryRecord> record;
		// A property or alpha swap: the whole leaf (CaptureLeaf), applied as a capture so the new property's record and uses come
		// with it.
		std::shared_ptr<const Records> leaf;
		// NextSequence after the write (SceneTracker::PushUpdate takes it): an update numbered before a capture's start is in it.
		std::uint64_t sequence = 0;
		// A hidden store's site, the source's index of it (Features/HiddenNodes; the hidden event rides here), else ~0u.
		std::uint32_t hiddenSite = ~0u;
	};

	/**
	 * @brief Orders captures and updates. A loader-thread capture takes milliseconds; another writer can change an object it already
	 * read and push first. The mirror applies a capture, then the updates numbered after its start, and skips updates numbered
	 * before the capture of their record.
	 */
	std::uint64_t NextSequence();

	NodeRecord CaptureNode(const RE::NiAVObject& a_object);
	/** @brief Only a_fields of a node (no children list): a hook's update. A fade field needs a BSFadeNode. */
	NodeRecord CaptureNodeFields(const RE::NiAVObject& a_object, std::uint32_t a_fields);
	GeometryRecord CaptureGeometry(const RE::BSGeometry& a_geometry);
	PropertyRecord CaptureProperty(const RE::BSShaderProperty& a_property);
	AlphaRecord CaptureAlpha(const RE::NiAlphaProperty& a_alpha);
	/** @brief The geometry's records (node, itself, properties) into a_out; with a_pins its properties held there. */
	void CaptureLeaf(const RE::BSGeometry& a_geometry, Records& a_out, std::vector<RE::NiPointer<RE::NiRefObject>>* a_pins = nullptr);

	/** @brief Whether a_object is reached from Main::WorldRootNode by its parent chain. */
	bool InWorld(const RE::NiAVObject* a_object);
	/**
	 * @brief The parentless roots the portal graph draws (alwaysRenderChildren), which InWorld counts as in the world. Written on the
	 * render thread (CaptureCategories), read lock-free by the hooks; replaced sets are kept (a few dozen pointers) for late readers.
	 */
	void SetDrawnRoots(std::vector<const RE::NiAVObject*> a_roots);
	/**
	 * @brief An attach's capture (attaching thread, after the engine's call): everything under a_root and its ancestors to the world
	 * root. Null when a_root is not in the world: a subtree being assembled is captured when attached to the world.
	 */
	std::unique_ptr<Records> CaptureAttached(const RE::NiAVObject& a_root);

	/** @brief Capture counters since the last call: captures, records, time, main-thread share. */
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

	/**
	 * @brief The skin-partition LOD table row a pass draws with (NiSkinPartition::Unk_25, AE 140d43a10): LODMode.index +
	 * LODMode.singleLevel * 4. GetRenderPasses and GetRenderPasses_ShadowMapOrMask agree: a kMeshLOD geometry's fade node LOD
	 * level (+0x152 & 0xF), cumulative; everything else level 3, every LOD byte.
	 */
	std::uint32_t LodRowOf(const RE::BSGeometry& a_geometry, const RE::BSShaderProperty* a_property);
	/** @brief LodRowOf from the records: the geometry's kMeshLOD, a_property's fade node's LOD level. */
	std::uint32_t LodRowOf(const LeafView& a_leaf, const PropertyRecord* a_property);
	std::uint32_t LodRowOf(const RE::BSRenderPass& a_pass);
}
