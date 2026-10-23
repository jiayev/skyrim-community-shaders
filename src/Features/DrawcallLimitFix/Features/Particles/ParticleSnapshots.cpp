// ParticleSnapshots: the update-slot thunks (the writer, on whichever thread updates the system), the followed-record table and the
// producer's expansion into the frame's palettes (FrameValues, on DCLF's pool, through the feature's palette rows class), FUN_140e51320's
// port.

#include "Features/DrawcallLimitFix/Features/Particles/ParticleSnapshots.h"

#include "Features/DrawcallLimitFix/Api/Feature.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Extensions/EngineEvents.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ObjectContributors.h"
#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <numeric>

namespace DCLF::Features::Particles
{
	using Engine::At;
	using Engine::Global;

	namespace
	{
		// The world scene graph, its camera at +0x128 (PrimaryCull's, PortalViews').
		constexpr std::size_t kSceneGraphCamera = 0x128;
		// The renderer's particle quads (FUN_140e4d280, at its initialisation): the quad index list (R16, 0x800 quads of 0,1,2 0,2,3) and
		// the texture-coordinate stream (half2, the corners (0,1) (1,1) (1,0) (0,0) of each quad), both immutable from then on.
		constexpr Engine::Offset kQuadIndices{ 0x3286a40, 0x3025f38 };
		constexpr Engine::Offset kQuadTexcoords{ 0x3286a48, 0x3025f40 };
		// NiParticleSystem's vtable slots UpdateDownwardPass and UpdateSelectedDownwardPass, and what they hold (AE 0x140dd48a0, 0x140dd4910).
		constexpr std::size_t kUpdateDownwardSlot = 0x2C;
		constexpr std::size_t kUpdateSelectedSlot = 0x2D;
		constexpr Engine::Offset kUpdateDownward{ 0xdd48a0, 0xd034a0 };
		constexpr Engine::Offset kUpdateSelected{ 0xdd4910, 0xd03510 };
		// NiParticles +0x160: four distances (ushort) of the draw's per-particle fade by the camera's distance (FUN_140d40ba0).
		constexpr std::size_t kParticlesFadeDistances = 0x160;
		// FUN_140e51320's constants: a rotated particle whose faded alpha is at most this is drawn with no size (DAT_1417743ec); the colour
		// scale of the packed vertex colour (DAT_141ad29a8).
		constexpr float kRotatedAlphaCut = 0.05f;
		constexpr float kColourScale = 255.0f;
		// SetupGeometry's IndexedTexCoord block holds 0x80 rows (FUN_140e4acc0(renderer, 0x80, ...)), zero past the data's offsets.
		constexpr std::uint32_t kSubtextureRows = 0x80;
		// A record no plan named for this many runs leaves the table.
		constexpr std::uint32_t kPruneRuns = 8;
		// latest: the slot index in bits 0-1, and whether it holds a snapshot the reader has not taken.
		constexpr std::uint32_t kSlotMask = 3;
		constexpr std::uint32_t kFresh = 4;

		// One address per quad bucket (QuadSlotKey): DCLF's own, so no engine TriShape can share a geometry slot key with them.
		constexpr std::uint32_t kBuckets = std::countr_zero(ParticleSnapshots::kMaxQuads) + 1;
		alignas(8) std::byte quadSlotKeys[kBuckets]{};

		// The snapshots' move writer ("particle update"), reserved at the install before the update thunks go in.
		Api::MoveWriterId moveWriter;

		std::uint64_t Mix(const void* a_pointer)
		{
			auto value = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(a_pointer));
			value ^= value >> 33;
			value *= 0xff51afd7ed558ccdull;
			value ^= value >> 33;
			return value;
		}

		// FUN_140d40ba0: the particle's alpha factor by its distance from the camera (a_distances: NiParticles +0x160..+0x166).
		float DistanceFade(const float (&a_distances)[4], const float (&a_eye)[3], const float (&a_position)[3])
		{
			const float farStart = a_distances[1];
			const float dy = a_eye[1] - a_position[1], dx = a_eye[0] - a_position[0], dz = a_eye[2] - a_position[2];
			const float distance = std::sqrt(dy * dy + dx * dx + dz * dz);
			if ((distance < a_distances[0] || farStart <= 0.0f) && a_distances[3] < distance) {
				if (distance < farStart || farStart <= 0.0f) {
					if (distance <= a_distances[2])
						return (distance - a_distances[3]) / (a_distances[2] - a_distances[3]);
					return 1.0f;
				}
				return 1.0f - (distance - farStart) / (a_distances[0] - farStart);
			}
			return 0.0f;
		}

		// FUN_140e51320's vertex colour: each channel times 255 truncated (cvttss2si), its low byte; red lowest.
		std::uint32_t PackColour(const float (&a_colour)[4], float a_fade)
		{
			const auto channel = [](float a_value) { return static_cast<std::uint32_t>(static_cast<std::int64_t>(a_value * kColourScale)); };
			return (((channel(a_fade * a_colour[3]) << 8 | (channel(a_colour[2]) & 0xFF)) << 8 | (channel(a_colour[1]) & 0xFF)) << 8) |
			       (channel(a_colour[0]) & 0xFF);
		}

		// FUN_140d4ff00: the engine's quicksort of the particle order by depth, farthest first (the same partitioning, so ties fall as the
		// engine's do).
		void SortByDepth(std::uint16_t* a_order, const float* a_depth, int a_first, int a_last)
		{
			while (true) {
				const std::uint16_t pivot = a_order[a_first];
				const float pivotDepth = a_depth[pivot];
				int low = a_first, high = a_last;
				while (low < high) {
					while (a_depth[a_order[high]] <= pivotDepth && low < high)
						--high;
					if (low != high)
						a_order[low++] = a_order[high];
					while ((pivotDepth < a_depth[a_order[low]] || pivotDepth == a_depth[a_order[low]]) && low < high)
						++low;
					if (low != high)
						a_order[high--] = a_order[low];
				}
				a_order[low] = pivot;
				if (a_first < low)
					SortByDepth(a_order, a_depth, a_first, low - 1);
				if (a_last <= low)
					return;
				a_first = low + 1;
			}
		}
	}

	/** @brief A followed system's snapshots: a triple buffer, one writer at a time (writing) and the producer as its reader. */
	class ParticleSnapshots::Record
	{
	public:
		struct Particle
		{
			float position[3];
			float size;       // radius * size, the draw's
			float colour[4];  // white without a colour array
			float angle;      // the rotation angle, with a rotation array
			float rect[4];    // its subtexture rect (IndexedTexCoord[textureIndex]), the identity without subtextures
			float velocity[3];  // NiPSysData's particle info (+0x90, NiParticleInfo::velocity) for aligned quads, zero otherwise
		};
		struct Slot
		{
			std::vector<Particle> particles;
			RE::NiTransform world;
			float worldBound[4]{};
			float modelCentre[3]{};
			float fadeDistances[4]{};
			bool rotations = false;
			bool aligned = false;
			// The data's aspect flags (FUN_140e51320, without rotations): +0x88 the quads' long axis along the velocity projected on the
			// view plane, +0x89 their aspect by that projected speed; aspect the data's +0x6c (the aspect, and the speed's first aspect),
			// +0x70 (the second aspect), +0x74 and +0x78 (the two speeds).
			bool alignVelocity = false, speedAspect = false;
			float aspect[4]{};
			std::uint64_t sequence = 0;  // 0: never written
		};

		explicit Record(const void* a_key) :
			key(a_key) {}

		const void* key;
		std::array<Slot, 3> slots;
		std::atomic<std::uint32_t> latest{ 1 };
		std::uint32_t writeSlot = 0;  // the writer's
		std::uint32_t readSlot = 2;   // the producer's
		std::atomic<bool> writing{ false };
		std::uint64_t written = 0;    // the writer's sequence
		// The writer's: what its last capture was taken at (the system's lastUpdate and world transform), so an update pass that ran no
		// system update and moved nothing (the engine passes a system twice a frame: the full and the selective downward pass) copies nothing.
		float capturedTime = -1.0f;
		RE::NiTransform capturedWorld;
		bool captured = false;
		// The producer's.
		std::uint32_t listedRun = 0;
		std::vector<std::uint16_t> order;
		std::vector<float> depth;
		// The producer's last run that expanded a snapshot of it (read by the observer on any thread).
		std::atomic<std::uint32_t> expandedRun{ 0 };
		// The producer's: the snapshot sequence it last expanded, and for how many runs in a row it has been the same one (a system the engine
		// stopped updating: frozen, its quads drawn from its last state).
		std::uint64_t expandedSequence = 0;
		std::uint32_t sameRuns = 0;
	};

	/** @brief An immutable open-addressed table of the followed records, by the system's address. */
	struct ParticleSnapshots::Table
	{
		std::vector<std::pair<const void*, Record*>> entries;  // a power of two, at most half full
		Record* Find(const void* a_key) const
		{
			if (entries.empty())
				return nullptr;
			const std::size_t mask = entries.size() - 1;
			for (std::size_t i = Mix(a_key) & mask;; i = (i + 1) & mask) {
				const auto& [key, record] = entries[i];
				if (key == a_key)
					return record;
				if (!key)
					return nullptr;
			}
		}
	};

	struct ParticleSnapshots::UpdateHook
	{
		static void thunk(RE::NiParticleSystem* a_system, RE::NiUpdateData* a_data, std::uint32_t a_arg)
		{
			func(a_system, a_data, a_arg);
			if (a_system)
				Get().Capture(*a_system);
		}
		static inline REL::Relocation<decltype(thunk)> func;
		static constexpr std::size_t size = kUpdateDownwardSlot;
	};

	struct ParticleSnapshots::SelectedHook
	{
		static void thunk(RE::NiParticleSystem* a_system, RE::NiUpdateData* a_data, std::uint32_t a_arg)
		{
			func(a_system, a_data, a_arg);
			if (a_system)
				Get().Capture(*a_system);
		}
		static inline REL::Relocation<decltype(thunk)> func;
		static constexpr std::size_t size = kUpdateSelectedSlot;
	};

	ParticleSnapshots::ParticleSnapshots() = default;
	ParticleSnapshots::~ParticleSnapshots() = default;

	ParticleSnapshots& ParticleSnapshots::Get()
	{
		static ParticleSnapshots snapshots;
		return snapshots;
	}

	std::uint32_t ParticleSnapshots::QuadsOf(std::uint32_t a_capacity)
	{
		return std::bit_ceil(std::clamp(a_capacity, kMinQuads, kMaxQuads));
	}

	const void* ParticleSnapshots::QuadSlotKey(std::uint32_t a_quads)
	{
		return &quadSlotKeys[std::min<std::uint32_t>(std::countr_zero(QuadsOf(a_quads)), kBuckets - 1)];
	}

	void ParticleSnapshots::Install(Api::InstallContext& a_context)
	{
		if (installed)
			return;
		REL::Relocation<std::uintptr_t> vtable{ RE::VTABLE_NiParticleSystem[0] };
		const auto* slots = reinterpret_cast<const std::uintptr_t*>(vtable.address());
		if (slots[kUpdateDownwardSlot] != REL::Offset(kUpdateDownward).address() || slots[kUpdateSelectedSlot] != REL::Offset(kUpdateSelected).address()) {
			logger::warn("[DCLF] particle snapshots: NiParticleSystem's update slots are not the expected functions; particle systems stay the engine's");
			return;
		}
		// Its move writer, the report's "particle update" (the slot after Features/Actors' three, as Core's table had it).
		moveWriter = a_context.ReserveMoveWriter("particle update");
		stl::write_vfunc<kUpdateDownwardSlot, UpdateHook>(RE::VTABLE_NiParticleSystem[0]);
		stl::write_vfunc<kUpdateSelectedSlot, SelectedHook>(RE::VTABLE_NiParticleSystem[0]);
		Extensions::MoveWriterInstalled(moveWriter);
		installed = true;
		logger::info("[DCLF] particle snapshots installed on NiParticleSystem's update");
	}

	ParticleSnapshots::QuadBuffers ParticleSnapshots::EngineQuads() const
	{
		return { quadVertices.load(std::memory_order_acquire), quadIndices.load(std::memory_order_acquire) };
	}

	void ParticleSnapshots::BeginFrame()
	{
		// The renderer's quads, once it made them (render thread: the renderer's own globals, written at its initialisation alone).
		if (!quadVertices.load(std::memory_order_relaxed) || !quadIndices.load(std::memory_order_relaxed)) {
			auto* vertices = Global<ID3D11Buffer*>(kQuadTexcoords);
			auto* indices = Global<ID3D11Buffer*>(kQuadIndices);
			if (vertices && indices) {
				quadVertices.store(vertices, std::memory_order_release);
				quadIndices.store(indices, std::memory_order_release);
			}
		}
		// The main camera, as Main::Draw's camera setup left it (PrimaryCull::BeginFrameCamera's): SetCameraData gives the renderer's view
		// data its world rotation's columns (viewForward the first, viewUp the second, viewRight the third) and posAdjust its translation.
		frameCamera = {};
		const auto* sceneGraph = Global<const std::byte*>(Engine::Address::kWorldSceneGraph);
		const auto* camera = sceneGraph ? At<const RE::NiCamera*>(sceneGraph, kSceneGraphCamera) : nullptr;
		if (!camera)
			return;
		const auto& r = camera->world.rotate.entry;
		for (int i = 0; i < 3; ++i) {
			frameCamera.forward[i] = r[i][0];
			frameCamera.up[i] = r[i][1];
			frameCamera.right[i] = r[i][2];
		}
		frameCamera.eye[0] = camera->world.translate.x;
		frameCamera.eye[1] = camera->world.translate.y;
		frameCamera.eye[2] = camera->world.translate.z;
		frameCamera.valid = true;
	}

	const ParticleSnapshots::Table* ParticleSnapshots::PinTable() const
	{
		// Counted in first, then the table read: a table replaced after this read is not freed until the count is seen at zero (EndRun).
		writersInside.fetch_add(1, std::memory_order_seq_cst);
		return current.load(std::memory_order_seq_cst);
	}

	void ParticleSnapshots::Capture(const RE::NiParticleSystem& a_system)
	{
		const Table* table = PinTable();
		Record* record = table ? table->Find(&a_system) : nullptr;
		if (record) {
			if (record->writing.exchange(true, std::memory_order_acquire)) {
				// Another thread is writing it (the engine updating it twice at once): that capture stands.
				contended.fetch_add(1, std::memory_order_relaxed);
			} else if (const float time = a_system.GetParticleSystemRuntimeData().lastUpdate;
					   record->captured && time == record->capturedTime && std::memcmp(&a_system.world, &record->capturedWorld, sizeof(RE::NiTransform)) == 0) {
				// Nothing new since the last capture (no system update ran, nothing moved): the published snapshot stands.
				record->writing.store(false, std::memory_order_release);
				unchanged.fetch_add(1, std::memory_order_relaxed);
			} else {
				record->captured = true;
				record->capturedTime = time;
				record->capturedWorld = a_system.world;
				auto& slot = record->slots[record->writeSlot];
				const auto& system = static_cast<const RE::NiParticles&>(a_system);
				const auto* data = system.GetParticlesRuntimeData().particleData.get();
				slot.particles.clear();
				slot.rotations = slot.aligned = slot.alignVelocity = slot.speedAspect = false;
				if (data) {
					const auto& p = static_cast<const RE::NiParticlesData*>(data)->GetParticlesRuntimeData();
					// The draw's count (FUN_1414f2ad0: the active vertex count, at most 0x800).
					const std::uint32_t count = std::min<std::uint32_t>(p.numVertices, kMaxQuads);
					if (p.positions && p.radii && p.sizes) {
						slot.particles.resize(count);
						slot.rotations = p.rotationAngles != nullptr;
						slot.aligned = p.unk88 || p.unk89;
						slot.alignVelocity = p.unk88 != 0;
						slot.speedAspect = p.unk89 != 0;
						slot.aspect[0] = p.aspectRatio;
						slot.aspect[1] = p.speedToAspectAspect2;
						slot.aspect[2] = p.speedToAspectAspect1;
						slot.aspect[3] = p.speedToAspectSpeed2;
						// The velocities the aligned quads are built from (NiPSysData's; the mirror's `velocities` keeps any other data the
						// engine's).
						const RE::NiParticleInfo* info = nullptr;
						if (slot.aligned)
							if (const auto* psys = netimmerse_cast<RE::NiPSysData*>(const_cast<RE::NiObject*>(static_cast<const RE::NiObject*>(data))))
								info = psys->GetPSysRuntimeData().particleInfo;
						const std::uint32_t subtextures = p.subtextureOffsets ? std::min(p.subTextureOffsetsCount, kSubtextureRows) : 0u;
						for (std::uint32_t i = 0; i < count; ++i) {
							auto& out = slot.particles[i];
							out.position[0] = p.positions[i].x;
							out.position[1] = p.positions[i].y;
							out.position[2] = p.positions[i].z;
							out.size = p.sizes[i] * p.radii[i];
							if (p.color) {
								out.colour[0] = p.color[i].red;
								out.colour[1] = p.color[i].green;
								out.colour[2] = p.color[i].blue;
								out.colour[3] = p.color[i].alpha;
							} else {
								out.colour[0] = out.colour[1] = out.colour[2] = out.colour[3] = 1.0f;
							}
							out.angle = p.rotationAngles ? p.rotationAngles[i] : 0.0f;
							out.velocity[0] = info ? info[i].velocity.x : 0.0f;
							out.velocity[1] = info ? info[i].velocity.y : 0.0f;
							out.velocity[2] = info ? info[i].velocity.z : 0.0f;
							// The vertex stage's IndexedTexCoord[index] (SetupGeometry's block: the data's offsets, zero past them), applied when
							// the data has subtextures (FUN_1414bde90's TexCoordIndex); the identity otherwise.
							if (p.subTextureOffsetsCount) {
								const std::uint32_t index = p.textureIndices ? p.textureIndices[i] : 0u;
								if (index < subtextures) {
									const auto& offset = p.subtextureOffsets[index];
									out.rect[0] = offset.red;
									out.rect[1] = offset.green;
									out.rect[2] = offset.blue;
									out.rect[3] = offset.alpha;
								} else {
									out.rect[0] = out.rect[1] = out.rect[2] = out.rect[3] = 0.0f;
								}
							} else {
								out.rect[0] = 0.0f;
								out.rect[1] = 1.0f;
								out.rect[2] = 0.0f;
								out.rect[3] = 1.0f;
							}
						}
					}
				}
				slot.world = a_system.world;
				const auto& bound = a_system.worldBound;
				slot.worldBound[0] = bound.center.x;
				slot.worldBound[1] = bound.center.y;
				slot.worldBound[2] = bound.center.z;
				slot.worldBound[3] = bound.radius;
				const auto& model = a_system.GetModelData().modelBound;
				slot.modelCentre[0] = model.center.x;
				slot.modelCentre[1] = model.center.y;
				slot.modelCentre[2] = model.center.z;
				for (std::size_t d = 0; d < 4; ++d)
					slot.fadeDistances[d] = static_cast<float>(At<std::uint16_t>(&a_system, kParticlesFadeDistances + 2 * d));
				slot.sequence = ++record->written;
				const std::uint32_t previous = record->latest.exchange(record->writeSlot | kFresh, std::memory_order_acq_rel);
				record->writeSlot = previous & kSlotMask;
				record->writing.store(false, std::memory_order_release);
				captures.fetch_add(1, std::memory_order_relaxed);
				// The system's move event (MoveEvents, keyed by the geometry): the scene pass's light path takes it then, not every frame.
				if (Extensions::GeometryTypeOn(SceneToggles(), static_cast<std::uint8_t>(RE::BSGeometry::Type::kParticles))) {
					Extensions::PushReservedMove(moveWriter, &a_system, Api::MoveReach::ScenePass);
				}
			}
		}
		writersInside.fetch_sub(1, std::memory_order_seq_cst);
	}

	ParticleSnapshots::Record* ParticleSnapshots::Follow(const void* a_geometry)
	{
		if (!a_geometry)
			return nullptr;
		auto& record = records[a_geometry];
		if (!record) {
			record = std::make_unique<Record>(a_geometry);
			dirty = true;
		}
		record->listedRun = runs.load(std::memory_order_relaxed) + 1;
		return record.get();
	}

	namespace
	{
		using Slot = ParticleSnapshots::Record::Slot;
		using Particle = ParticleSnapshots::Record::Particle;

		/**
		 * @brief The camera in a system's model space and its quads' axes (FUN_140e51320's): the camera's up and right (the world rotation's
		 * transpose times the view data's), the plain quads' corner directions, and for aligned quads (its aspect flags, without rotations)
		 * the quad's axes A (first the camera's up) and B (its right), which the velocity alignment replaces particle by particle and the next
		 * particle starts from (as the engine's registers carry them), the view normal E = B x A, and the aspect, which the speed's aspect
		 * replaces and the next particle keeps likewise.
		 */
		struct QuadFrame
		{
			float up[3], right[3];
			float plainX[3], plainY[3];
			float axisA[3], axisB[3], normal[3];
			float aspect = 0.0f;
		};

		void FrameOf(const Slot& a_slot, const ParticleSnapshots::Camera& a_camera, QuadFrame& a_frame)
		{
			const auto& r = a_slot.world.rotate.entry;
			auto& up = a_frame.up;
			auto& right = a_frame.right;
			for (int j = 0; j < 3; ++j) {
				up[j] = r[1][j] * a_camera.up[1] + a_camera.up[0] * r[0][j] + r[2][j] * a_camera.up[2];
				right[j] = a_camera.right[0] * r[0][j] + a_camera.right[1] * r[1][j] + a_camera.right[2] * r[2][j];
			}
			for (int j = 0; j < 3; ++j) {
				a_frame.plainX[j] = up[j] + right[j];
				a_frame.plainY[j] = up[j] - right[j];
				a_frame.axisA[j] = up[j];
				a_frame.axisB[j] = right[j];
			}
			a_frame.normal[0] = up[2] * right[1] - right[2] * up[1];
			a_frame.normal[1] = up[0] * right[2] - right[0] * up[2];
			a_frame.normal[2] = up[1] * right[0] - right[1] * up[0];
			a_frame.aspect = a_slot.aspect[0];
		}

		// Back to front: the model-space positions' distance along the camera's forward (FUN_140d4fcb0, then FUN_140d4ff00's sort).
		void OrderBackToFront(ParticleSnapshots::Record& a_record, const Slot& a_slot, const ParticleSnapshots::Camera& a_camera, std::uint32_t a_count)
		{
			auto& order = a_record.order;
			auto& depth = a_record.depth;
			order.resize(a_count);
			std::iota(order.begin(), order.end(), std::uint16_t{ 0 });
			if (a_count >= 2) {
				depth.resize(a_count);
				for (std::uint32_t i = 0; i < a_count; ++i) {
					const auto& p = a_slot.particles[i].position;
					depth[i] = p[1] * a_camera.forward[1] + p[0] * a_camera.forward[0] + a_camera.forward[2] * p[2];
				}
				SortByDepth(order.data(), depth.data(), 0, static_cast<int>(a_count) - 1);
			}
		}

		// An aligned quad's half-diagonals, the frame's axes and aspect carried on to the next particle.
		void AlignedAxes(const Slot& a_slot, const Particle& a_particle, QuadFrame& a_frame, float (&a_x)[3], float (&a_y)[3])
		{
			auto& axisA = a_frame.axisA;
			auto& axisB = a_frame.axisB;
			const auto& normal = a_frame.normal;
			const auto& v = a_particle.velocity;
			if (a_slot.speedAspect) {
				// The velocity projected on the view plane; its length clamped to the two speeds picks the aspect between the two.
				const float onB = v[0] * axisB[0] + v[1] * axisB[1] + v[2] * axisB[2];
				const float onA = v[0] * axisA[0] + v[1] * axisA[1] + v[2] * axisA[2];
				const float p0 = onA * axisA[0] + onB * axisB[0], p1 = onA * axisA[1] + onB * axisB[1], p2 = onA * axisA[2] + onB * axisB[2];
				const float speed = std::sqrt(p0 * p0 + p2 * p2 + p1 * p1);
				float clamped = a_slot.aspect[3];
				if (speed <= clamped) {
					clamped = speed;
					if (speed <= a_slot.aspect[2])
						clamped = a_slot.aspect[2];
				}
				a_frame.aspect = ((clamped - a_slot.aspect[2]) / (a_slot.aspect[3] - a_slot.aspect[2])) * (a_slot.aspect[1] - a_slot.aspect[0]) + a_slot.aspect[0];
			}
			if (a_slot.alignVelocity) {
				// A along the projected velocity (XMVector3Normalize: zero for a zero one), B = A x E, both carried to the next particle.
				const float onB = v[0] * axisB[0] + v[1] * axisB[1] + v[2] * axisB[2];
				const float onA = v[0] * axisA[0] + v[1] * axisA[1] + v[2] * axisA[2];
				float a[3]{ axisA[0] * onA + onB * axisB[0], axisA[1] * onA + onB * axisB[1], axisA[2] * onA + onB * axisB[2] };
				const float lengthSq = a[0] * a[0] + a[1] * a[1] + a[2] * a[2];
				const float length = std::sqrt(lengthSq);
				for (int j = 0; j < 3; ++j)
					a[j] = lengthSq == std::numeric_limits<float>::infinity() ? std::numeric_limits<float>::quiet_NaN() : length != 0.0f ? a[j] / length : 0.0f;
				axisA[0] = a[0];
				axisA[1] = a[1];
				axisA[2] = a[2];
				axisB[0] = normal[2] * a[1] - a[2] * normal[1];
				axisB[1] = normal[0] * a[2] - a[0] * normal[2];
				axisB[2] = normal[1] * a[0] - a[1] * normal[0];
			}
			const float stretch = 1.0f / a_frame.aspect;
			for (int j = 0; j < 3; ++j) {
				a_x[j] = (stretch * axisA[j] + axisB[j]) * a_particle.size;
				a_y[j] = (stretch * axisA[j] - axisB[j]) * a_particle.size;
			}
		}

		// A particle's quad half-diagonals: rotated, aligned or plain.
		void QuadAxes(const Slot& a_slot, const Particle& a_particle, float a_fade, bool a_alignedQuads, QuadFrame& a_frame, float (&a_x)[3], float (&a_y)[3])
		{
			if (a_slot.rotations) {
				const float size = a_fade * a_particle.colour[3] <= kRotatedAlphaCut ? 0.0f : a_particle.size;
				const float sine = std::sin(a_particle.angle), cosine = std::cos(a_particle.angle);
				const float a = (cosine + sine) * size, b = (cosine - sine) * size;
				for (int j = 0; j < 3; ++j) {
					a_x[j] = a * a_frame.right[j] + b * a_frame.up[j];
					a_y[j] = a * a_frame.up[j] - b * a_frame.right[j];
				}
			} else if (a_alignedQuads) {
				AlignedAxes(a_slot, a_particle, a_frame, a_x, a_y);
			} else {
				for (int j = 0; j < 3; ++j) {
					a_x[j] = a_frame.plainX[j] * a_particle.size;
					a_y[j] = a_frame.plainY[j] * a_particle.size;
				}
			}
		}

		// A quad's rows: its four corners relative to the model bound's centre, which the placement's translation carries (the engine's
		// vertices and World alike), the packed colour as w's bits; then its subtexture rect.
		void WriteQuad(const Slot& a_slot, const Particle& a_particle, const float (&a_x)[3], const float (&a_y)[3], float a_colour, float* a_out)
		{
			const auto& p = a_particle.position;
			const float centre[3]{ p[0] - a_slot.modelCentre[0], p[1] - a_slot.modelCentre[1], p[2] - a_slot.modelCentre[2] };
			const float signs[4][2]{ { -1.0f, 0.0f }, { 0.0f, -1.0f }, { 1.0f, 0.0f }, { 0.0f, 1.0f } };  // v0 -X, v1 -Y, v2 +X, v3 +Y
			for (int c = 0; c < 4; ++c) {
				for (int j = 0; j < 3; ++j)
					a_out[c * 4 + j] = signs[c][0] != 0.0f ? centre[j] + signs[c][0] * a_x[j] : centre[j] + signs[c][1] * a_y[j];
				a_out[c * 4 + 3] = a_colour;
			}
			std::copy_n(a_particle.rect, 4, a_out + 16);
		}

		// The placement: the world rotation and scale, the model bound's centre folded into the translation (BSEffectShader::SetupGeometry
		// for particles, 0x1414eab10), the previous transform the same; the world bound the update left.
		void PlacementOf(const Slot& a_slot, ParticleSnapshots::Expanded& a_out)
		{
			const auto& r = a_slot.world.rotate.entry;
			const float scale = a_slot.world.scale;
			const float t[3]{ a_slot.world.translate.x, a_slot.world.translate.y, a_slot.world.translate.z };
			const auto& c = a_slot.modelCentre;
			for (int row = 0; row < 3; ++row) {
				a_out.world[row * 4 + 0] = r[row][0] * scale;
				a_out.world[row * 4 + 1] = r[row][1] * scale;
				a_out.world[row * 4 + 2] = r[row][2] * scale;
				a_out.world[row * 4 + 3] = (r[row][0] * c[0] + r[row][1] * c[1] + r[row][2] * c[2]) * scale + t[row];
			}
			std::copy_n(a_slot.worldBound, 4, a_out.bound);
		}
	}

	void ParticleSnapshots::Expand(Record& a_record, const Camera& a_camera, std::uint32_t a_quads, float* a_rows, Expanded& a_out)
	{
		a_out = {};
		const std::size_t rows = std::size_t(a_quads) * kRowsPerParticle;
		// The newest snapshot, when the writer published one since the last take.
		if (a_record.latest.load(std::memory_order_acquire) & kFresh)
			a_record.readSlot = a_record.latest.exchange(a_record.readSlot, std::memory_order_acq_rel) & kSlotMask;
		const auto& slot = a_record.slots[a_record.readSlot];
		if (!slot.sequence || !a_camera.valid) {
			std::fill_n(a_rows, rows * 4, 0.0f);
			withoutSnapshot.fetch_add(1, std::memory_order_relaxed);
			return;
		}
		const std::uint32_t count = std::min<std::uint32_t>(static_cast<std::uint32_t>(slot.particles.size()), a_quads);
		const auto& r = slot.world.rotate.entry;
		const float scale = slot.world.scale;
		const float t[3]{ slot.world.translate.x, slot.world.translate.y, slot.world.translate.z };
		QuadFrame frame;
		FrameOf(slot, a_camera, frame);
		OrderBackToFront(a_record, slot, a_camera, count);
		const bool alignedQuads = !slot.rotations && (slot.alignVelocity || slot.speedAspect);
		for (std::uint32_t k = 0; k < count; ++k) {
			const auto& particle = slot.particles[a_record.order[k]];
			const auto& p = particle.position;
			// The particle's world position, for its distance fade.
			const float world[3]{ (p[0] * r[0][0] + p[1] * r[0][1] + p[2] * r[0][2]) * scale + t[0], (p[0] * r[1][0] + p[1] * r[1][1] + p[2] * r[1][2]) * scale + t[1],
				(p[0] * r[2][0] + p[1] * r[2][1] + p[2] * r[2][2]) * scale + t[2] };
			const float fade = DistanceFade(slot.fadeDistances, a_camera.eye, world);
			const float colour = std::bit_cast<float>(PackColour(particle.colour, fade));
			float x[3], y[3];
			QuadAxes(slot, particle, fade, alignedQuads, frame, x, y);
			WriteQuad(slot, particle, x, y, colour, a_rows + std::size_t(k) * kRowsPerParticle * 4);
		}
		std::fill(a_rows + std::size_t(count) * kRowsPerParticle * 4, a_rows + rows * 4, 0.0f);
		PlacementOf(slot, a_out);
		a_out.particles = count;
		a_out.snapshot = true;
		a_record.expandedRun.store(runs.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
		a_record.sameRuns = slot.sequence == a_record.expandedSequence ? a_record.sameRuns + 1 : 0;
		a_record.expandedSequence = slot.sequence;
		if (a_record.sameRuns == kFrozenRuns)
			frozen.fetch_add(1, std::memory_order_relaxed);
		expansions.fetch_add(1, std::memory_order_relaxed);
		expandedParticles.fetch_add(count, std::memory_order_relaxed);
		if (alignedQuads)
			aligned.fetch_add(1, std::memory_order_relaxed);
	}

	void ParticleSnapshots::EndRun()
	{
		const std::uint32_t run = runs.load(std::memory_order_relaxed) + 1;
		// The records no plan named lately leave the table (a system that left the scene, or stopped being a member).
		for (auto it = records.begin(); it != records.end();) {
			if (run - it->second->listedRun > kPruneRuns) {
				retired.push_back(std::move(it->second));
				it = records.erase(it);
				dirty = true;
				pruned.fetch_add(1, std::memory_order_relaxed);
			} else {
				++it;
			}
		}
		bool replaced = false;
		if (dirty) {
			auto table = std::make_unique<Table>();
			if (!records.empty()) {
				table->entries.resize(std::bit_ceil(records.size() * 2));
				const std::size_t mask = table->entries.size() - 1;
				for (const auto& [key, record] : records) {
					std::size_t i = Mix(key) & mask;
					while (table->entries[i].first)
						i = (i + 1) & mask;
					table->entries[i] = { key, record.get() };
				}
			}
			current.store(table.get(), std::memory_order_seq_cst);
			tables.push_back(std::move(table));
			dirty = false;
			replaced = true;
			published.fetch_add(1, std::memory_order_relaxed);
		}
		// No writer inside after the store: every later one reads the newest table, so the older tables and the records retired before this
		// point are no writer's.
		if ((replaced || tables.size() > 1 || !retired.empty()) && writersInside.load(std::memory_order_seq_cst) == 0) {
			if (tables.size() > 1)
				tables.erase(tables.begin(), tables.end() - 1);
			retired.clear();
		}
		followed.store(static_cast<std::uint32_t>(records.size()), std::memory_order_relaxed);
		runs.store(run, std::memory_order_relaxed);
	}

	bool ParticleSnapshots::Covered(const RE::BSGeometry* a_geometry, bool& a_followed) const
	{
		const Table* table = PinTable();
		const Record* record = table ? table->Find(a_geometry) : nullptr;
		const std::uint32_t expanded = record ? record->expandedRun.load(std::memory_order_relaxed) : 0u;
		writersInside.fetch_sub(1, std::memory_order_seq_cst);
		a_followed = record != nullptr;
		return expanded && runs.load(std::memory_order_relaxed) - expanded <= kCoveredRuns;
	}

	ParticleSnapshots::Counters ParticleSnapshots::Take()
	{
		Counters c;
		c.captures = captures.exchange(0), c.expansions = expansions.exchange(0), c.expandedParticles = expandedParticles.exchange(0);
		c.withoutSnapshot = withoutSnapshot.exchange(0), c.aligned = aligned.exchange(0), c.contended = contended.exchange(0);
		c.published = published.exchange(0), c.pruned = pruned.exchange(0);
		c.unchanged = unchanged.exchange(0), c.frozen = frozen.exchange(0);
		c.followed = followed.load(std::memory_order_relaxed);
		return c;
	}
}
