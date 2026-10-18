#pragma once

// The particle snapshots (Features/Particles): the writer's engine hooks, the followed records and the expansion the feature's palette
// rows class runs (ParticleRows.cpp). Included by the feature's sources only.

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

struct ID3D11Buffer;

namespace RE
{
	class BSGeometry;
	class NiParticleSystem;
}

namespace DCLF::Api
{
	class InstallContext;
}

namespace DCLF::Features::Particles
{
	/**
	 * @brief The particle systems DCLF draws (CS_DCLF_PARTICLES; dclf-architecture.md, "Particles"; engine notes, "Particle systems"),
	 * their state published by the engine's own writer, so that DCLF never reads a system's particle data afterwards.
	 *
	 * The engine updates a particle system in its update pass (NiParticleSystem::UpdateDownwardPass and UpdateSelectedDownwardPass, vtable
	 * slots 0x2C/0x2D: its controllers run Do_UpdateSystem, whose modifiers write the NiPSysData arrays and the model bound, then its world
	 * transform and world bound), on whatever thread updates its subtree: the cell animations' jobs for a reference's emitters, the
	 * "Particles" job stage for temporary effects and the manager's world-space systems. Its draw builds the quads on the render thread
	 * (FUN_1414f2ad0, type 1: FUN_140e51320 writes four vertices a particle, faced to the drawing camera, sorted back to front, into the
	 * renderer's dynamic ring) from that state. DCLF does the same in two halves:
	 * - the writer: a thunk after each of the two update slots copies the state DCLF needs (positions, size, colour, rotation angle and the
	 *   particle's subtexture rect; the system's world transform, world bound, model bound centre and fade distances) into the system's
	 *   record, a triple buffer: the writer fills its slot and exchanges it into `latest`. A record the producer does not follow is not
	 *   copied (one lookup in an immutable table);
	 * - the reader: FrameValues' producer (one at a time, on DCLF's pool), through the feature's palette rows class (ParticleRows.cpp,
	 *   Api::PaletteRowsContribution: Follow at the run's start, Expand in each chunk), for every particle member of the frame's plan, takes
	 *   the newest snapshot and expands it with the frame's main camera (BeginFrame, the class's frame start at FrameDriver::BeginFrame)
	 *   exactly as FUN_140e51320 does, into the member's block of the frame's palettes (five rows a particle: four corners, then the
	 *   subtexture rect), which Effect.hlsl's DCLF particle permutation pulls by vertex index; and gives the member's placement row the
	 *   snapshot's world (the model bound centre folded into the translation, as BSEffectShader::SetupGeometry does for particles) and world
	 *   bound (the GPU cull's).
	 *
	 * The table of followed records is the producer's: a record is added the first time a plan names its system, and pruned when no plan has
	 * named it for kPruneRuns runs; every change publishes a new immutable table. A writer counts itself in (writersInside) before it reads
	 * the table and out after its capture, so a replaced table or a pruned record is freed only once the count was seen at zero after the
	 * replacement. Neither side waits for the other; no engine reference is held.
	 */
	class ParticleSnapshots
	{
	public:
		static ParticleSnapshots& Get();

		/**
		 * @brief Installs the writer's thunks in NiParticleSystem's vtable (checked first), its move writer reserved through a_context first.
		 * Render thread, at the feature's install (Features/Particles).
		 */
		void Install(Api::InstallContext& a_context);
		bool Installed() const { return installed; }

		// The block layout (Effect.hlsl, DCLFParticleInput): four rows a particle's corners (position relative to the model bound's centre,
		// the packed colour as w's bits), then its subtexture rect (IndexedTexCoord's: u' = y * u + x, v' = w * v + z).
		static constexpr std::uint32_t kRowsPerParticle = 5;
		// The engine draws at most 0x800 particles a system (FUN_1414f2ad0) with a quad index list of as many quads (FUN_140e4d280).
		static constexpr std::uint32_t kMaxQuads = 0x800;
		static constexpr std::uint32_t kMinQuads = 8;
		/**
		 * @brief The particle quads' geometry slot's vertex description: the renderer's static texture-coordinate stream alone (4 bytes a
		 * vertex, half2 at stream 0), which the draw binds and the DCLF particle permutation does not read. NiParticles' own description
		 * (0x0840200004000051) puts the position and colour on stream 1, which DCLF's draws reserve for face streams.
		 */
		static constexpr std::uint64_t kSlotVertexDesc = 0x0000200000000001ull;
		/**
		 * @brief The quads a system of a_capacity particles is drawn with: its capacity rounded up to a power of two, within the engine's
		 * (the palette rows class's rule, which the walk sizes its block by: ParticleRows.cpp).
		 */
		static std::uint32_t QuadsOf(std::uint32_t a_capacity);
		/** @brief The geometry slot key of a_quads quads (one slot a bucket): an address of DCLF's own, never an engine object's. */
		static const void* QuadSlotKey(std::uint32_t a_quads);

		/** @brief The renderer's quad buffers (FUN_140e4d280): its texture-coordinate stream and its quad index list, null before BeginFrame. */
		struct QuadBuffers
		{
			ID3D11Buffer* vertices = nullptr;
			ID3D11Buffer* indices = nullptr;
		};
		QuadBuffers EngineQuads() const;

		/** @brief The main camera a frame's quads face (the renderer's ViewData: viewUp, viewRight, viewForward; posAdjust). */
		struct Camera
		{
			float up[3]{}, right[3]{}, forward[3]{}, eye[3]{};
			bool valid = false;
		};
		/**
		 * @brief Render thread, FrameDriver::BeginFrame (the palette rows class's frame start): the frame's main camera (the world scene
		 * graph's), and the renderer's quads once.
		 */
		void BeginFrame();
		/** @brief Render thread (the palette rows class's kick, at FrameValues::Kick): BeginFrame's camera. */
		Camera FrameCamera() const { return frameCamera; }

		class Record;
		/** @brief The producer (the palette rows class's run start, one at a time): a_geometry's record, followed from now on (added when new). */
		Record* Follow(const void* a_geometry);
		/** @brief What Expand made of a record's newest snapshot: the placement's world (row-major 3x4) and bound, its particles. */
		struct Expanded
		{
			float world[12]{};
			float bound[4]{};
			std::uint32_t particles = 0;
			bool snapshot = false;  // false: no snapshot yet (the block is all degenerate quads, the placement the engine's)
		};
		/**
		 * @brief The producer (the palette rows class's chunks), one thread a record: the record's newest snapshot faced to a_camera into
		 * a_rows (a_quads * kRowsPerParticle float4 rows: the particles back to front, the rest degenerate). FUN_140e51320's port.
		 */
		void Expand(Record& a_record, const Camera& a_camera, std::uint32_t a_quads, float* a_rows, Expanded& a_out);
		/** @brief The producer, after its items (the palette rows class's run end): prunes, publishes a changed table, frees what no writer can hold. */
		void EndRun();

		/**
		 * @brief Whether the producer expanded a snapshot of a_geometry in its last kCoveredRuns runs (a_followed: it has a record). Any
		 * thread (Features/Particles' <- PARTICLES, at the engine's registration of the member's pass).
		 */
		bool Covered(const RE::BSGeometry* a_geometry, bool& a_followed) const;
		static constexpr std::uint32_t kCoveredRuns = 3;
		/** @brief A record whose newest snapshot has not changed for this many runs is frozen (counted once, when it reaches it). */
		static constexpr std::uint32_t kFrozenRuns = 30;

		/** @brief The counters since the last Take (Features/Particles' report lines). */
		struct Counters
		{
			std::uint64_t captures = 0, contended = 0, aligned = 0, expansions = 0, expandedParticles = 0, withoutSnapshot = 0;
			std::uint64_t published = 0, pruned = 0, unchanged = 0, frozen = 0;
			std::uint32_t followed = 0;
		};
		Counters Take();

		~ParticleSnapshots();

	private:
		ParticleSnapshots();
		struct Table;
		struct UpdateHook;
		struct SelectedHook;

		/** @brief The writer, after the engine updated a_system (any thread). */
		void Capture(const RE::NiParticleSystem& a_system);
		const Table* PinTable() const;

		bool installed = false;
		std::atomic<const Table*> current{ nullptr };
		mutable std::atomic<std::uint32_t> writersInside{ 0 };
		std::atomic<ID3D11Buffer*> quadVertices{ nullptr }, quadIndices{ nullptr };
		Camera frameCamera;  // render thread

		// The producer's.
		std::unordered_map<const void*, std::unique_ptr<Record>> records;
		std::vector<std::unique_ptr<const Table>> tables;  // the published one last; the older ones until no writer can hold them
		std::vector<std::unique_ptr<Record>> retired;
		bool dirty = false;
		std::atomic<std::uint32_t> runs{ 0 };

		// Since the last report.
		std::atomic<std::uint64_t> captures{ 0 }, contended{ 0 }, aligned{ 0 }, expansions{ 0 }, expandedParticles{ 0 }, withoutSnapshot{ 0 };
		std::atomic<std::uint64_t> published{ 0 }, pruned{ 0 }, unchanged{ 0 }, frozen{ 0 };
		std::atomic<std::uint32_t> followed{ 0 };  // records.size() at the last run's end (the report's)
	};
}
