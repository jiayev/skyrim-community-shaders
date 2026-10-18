#pragma once

#include <array>
#include <cstddef>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "Features/DrawcallLimitFix/Core/Extensions/ProgramFamilies.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawFamilyLaneSlots.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Records.h"

namespace DCLF
{
	// The Effect family's slot is Records.h's effect key bits.
	static_assert(ProgramFamilySlotOf(Api::ProgramFamily::Effect).keyBit == kRasterEffect &&
	                  ProgramFamilySlotOf(Api::ProgramFamily::Effect).spareShift == kRasterEffectBillboardShift,
		"the Effect family's slot is not Records.h's kRasterEffect and kRasterEffectBillboardShift");
	/**
	 * @brief Register classes of the SPIR-V builds, shifted onto separate binding ranges (DXC
	 * -fvk-{b,t,s,u}-shift, all in set 0) so that the pipeline layout can map each class on its own.
	 */
	inline constexpr std::uint32_t kBindingShiftB = 0;
	inline constexpr std::uint32_t kBindingShiftT = 200;
	inline constexpr std::uint32_t kBindingShiftS = 400;
	inline constexpr std::uint32_t kBindingShiftU = 500;

	/*
	 * DCLF's permutations are built with DCLF_BINDLESS and DCLF_BINDLESS_DRAW. The five PerGeometry variables that
	 * differ between the objects of one pipeline (World, PreviousWorld, MaterialData, EmitColor and the w of SSRParams),
	 * the alpha test reference, the emissive multiplier and Light Limit Fix's room index and shadow bit mask are read
	 * from the per-object record table at t127, indexed by the object index the draw carries in its push data, instead
	 * of from the draw's own constant buffers (PS b11, b8 and b3). That makes the binding record identical for every
	 * draw of a (material, pipeline) pair, so it is deduplicated.
	 */

	/**
	 * @brief SPIR-V builds of the Lighting shader permutations DCLF draws.
	 *
	 * Each pipeline key's vertex and pixel descriptors are compiled once, asynchronously, by the render
	 * graph runtime's ORGModuleServices compiler, from the same source, includes and defines the D3D11
	 * build uses (ShaderCache::GetCompileDefines), plus DCLF_ORG. Artifacts are content-addressed on the
	 * source, every shader file under Data/Shaders and the defines, and cached on disk. Every vertex stage pulls its
	 * attributes: the device-generated draws' (Program::vertex, a family's vertex stage) with DCLF_PULLED_VERTICES, the plain
	 * draws' with DCLF_PULLED.
	 *
	 * The compiles run on DCLF's build executor (BuildExecutor.h). Each stage's completion callback pushes its key to a
	 * lock-free queue, which the consumer drains: the entries waiting for the stage are told, and an entry whose stages have
	 * all completed gets its program. Nothing polls a compile.
	 *
	 * One consumer owns the programs: the pipeline lane (BuildExecutor.h, WakePipelineLane), which a completion
	 * wakes: every kind's entries (Find, FindShadow, FindDrawFamily, FindForward, FindDrawFamilyForward) are made and finished on it
	 * (UpdateLane). The stage cache also accepts CS compilation workers.
	 */
	class ShaderPrograms
	{
	public:
		struct Program
		{
			// SPIR-V. The vertex stage pulls its attributes from the buffers in its draw's push data (DCLF_PULLED_VERTICES): no DCLF
			// pipeline has input assembly. Both variants (colour, depth) draw with it.
			std::vector<std::byte> vertex;
			std::vector<std::byte> pixel;
			std::vector<std::byte> depthPixel;  // the same permutation built with DCLF_DEPTH_ONLY
			// The Z-prepass's plain draws' stages (DCLF_PULLED; MainOpaquePass): the vertex stage fetching its draw's vertices and
			// rows itself, and the depth pixel stage reading its material row's inputs itself (PulledMaterialSource).
			std::vector<std::byte> pulledVertex;
			std::vector<std::byte> pulledDepthPixel;
			// The attributes (BSGraphics::Vertex::Attribute, a bit each) the vertex stages read, from their defines (ShaderPrograms.cpp,
			// VertexAttributesOf): a key whose vertex layout lacks one fails (VertexLayoutAttributes), as the input assembler's layout
			// did. Set once the stages are in (UpdateLane), not by FinishEntry's aggregate.
			std::uint32_t vertexAttributes = 0;
		};

		struct Stats
		{
			std::uint32_t requested = 0;
			std::uint32_t ready = 0;
			std::uint32_t failed = 0;
			std::uint32_t fromCache = 0;  // artifacts found in the memory or disk cache
			std::uint32_t shadowRequested = 0;  // Utility techniques (shadow views)
			std::uint32_t shadowReady = 0;
			std::uint32_t shadowFailed = 0;
		};

		static ShaderPrograms& Get();

		/** @brief Whether SPIR-V compilation is available (render graph active, DXC loaded). */
		bool Enabled() const;

		/**
		 * @brief Stages a runtime request (Find, FindShadow) asked for before any precompile task had (OnDemand*): what the
		 * precompile missed, compiled on demand unless the disk cache has it. Logged when the program is ready (Update).
		 */
		static constexpr std::uint8_t kOnDemandVertex = 1, kOnDemandPixel = 2, kOnDemandDepthPixel = 4, kOnDemandPulledVertex = 8,
									  kOnDemandPulledDepthPixel = 16;
		/** @brief "VS, PS, depth PS" for a set of kOnDemand bits. */
		static std::string OnDemandStages(std::uint8_t a_stages);

		/**
		 * @brief The pipeline lane: the key's program once its stages compiled; requests them on first call. Null otherwise.
		 * @param a_onDemand Set to the stages this call requested that no precompile had (kOnDemand*), 0 otherwise.
		 * @param a_failed Set when the program can never be made (a stage failed, or its source is missing).
		 */
		const Program* Find(const PipelineKey& a_key, RE::BSShader& a_lighting, std::uint8_t* a_onDemand = nullptr, bool* a_failed = nullptr);
		/** @brief Which Lighting program a key draws with (Find's entries, UpdateLane's ids): its vertex and pixel descriptors. */
		static std::uint64_t LightingProgramId(const PipelineKey& a_key)
		{
			return (static_cast<std::uint64_t>(a_key.vertexDescriptor) << 32) | a_key.pixelDescriptor;
		}

		/**
		 * @brief SPIR-V builds of one Utility technique, for a shadow view's draws.
		 *
		 * The same mechanism as the Lighting builds, from `Data/Shaders/Utility.hlsl` with the technique's
		 * own compile defines plus DCLF_BINDLESS: a shadow draw's World, tree parameters, bone palette and
		 * alpha reference come from the per-object record, because one pipeline draws every object of a
		 * view (engine notes: shadow maps). Both stages are compiled; a technique without alpha testing
		 * simply has a pixel stage that writes nothing anyone reads.
		 */
		struct ShadowProgram
		{
			std::vector<std::byte> vertex;
			std::vector<std::byte> pixel;
		};
		/**
		 * @brief The pipeline lane: the Utility technique's programs, or null while they build. Programs are never freed: the
		 * pointer stays valid for the process (pipeline builds keep it).
		 * @param a_allowRequest False returns null for an unseen technique without starting its builds.
		 * @param a_failed Set when the programs can never be made (a stage failed, or its source is missing).
		 */
		const ShadowProgram* FindShadow(std::uint32_t a_technique, RE::BSShader& a_utility, bool a_allowRequest = true, bool* a_requested = nullptr,
			std::uint8_t* a_onDemand = nullptr, bool* a_failed = nullptr);

		/**
		 * @brief A draw family's programs (Api::DrawFamilyProgram; tree LOD's: dclf-architecture.md, "Tree LOD: the draws"): its source's
		 * (Extensions::DrawFamilies' sourcePath: `Data/Shaders/DistantTree.hlsl`) pulled builds (DCLF_PULLED: its lane slot's pulled) of
		 * its colour descriptor (DistantTree's deferred technique) and its depth descriptor (the depth technique), each a vertex and a
		 * pixel stage, a_shader's (its programSource). Requested on the first call; null until all four are compiled. Never freed. The
		 * pipeline lane's, as the other Find* below; a_failed as Find's.
		 */
		struct DrawProgram
		{
			std::vector<std::byte> vertex, pixel;            // colour (DistantTreeBlock, deferred)
			std::vector<std::byte> depthVertex, depthPixel;  // depth (RENDER_DEPTH)
		};
		const DrawProgram* FindDrawFamily(Api::DrawFamily a_family, RE::BSShader& a_shader, bool* a_failed = nullptr);

		/**
		 * @brief A forward view's program (the water reflection's cube map faces: dclf-architecture.md, "Water reflections"): one forward
		 * pass, colour and depth together, drawn with plain draws like the Z-prepass's. So both stages are pulled builds (DCLF_PULLED):
		 * the vertex stage of a main pipeline's vertex descriptor, and the colour pixel stage of its forward pixel descriptor (the
		 * main one without Deferred: LightingShaderDescriptors).
		 */
		struct ForwardProgram
		{
			std::vector<std::byte> vertex, pixel;
		};
		/** @brief The Lighting forward program of these descriptors, requesting it; null until both stages are compiled. Never freed. */
		const ForwardProgram* FindForward(std::uint32_t a_vertexDescriptor, std::uint32_t a_pixelDescriptor, RE::BSShader& a_lighting, bool* a_failed = nullptr);
		/** @brief Which forward program a pair of descriptors draws with (FindForward's entries, UpdateLane's ids). */
		static std::uint64_t ForwardProgramId(std::uint32_t a_vertexDescriptor, std::uint32_t a_pixelDescriptor)
		{
			return (static_cast<std::uint64_t>(a_vertexDescriptor) << 32) | a_pixelDescriptor;
		}
		/**
		 * @brief A program family's key's program (Api::ProgramFamily: its key bit, KeyFamily; the Effect port's Effect.hlsl), its source's
		 * builds (Extensions::ProgramFamilies' sourcePath) of its vertex and pixel descriptors with the same defines as the Lighting builds
		 * (DCLF_BINDLESS and DCLF_BINDLESS_DRAW: the source's DCLF reads), a_shader's (its programSource). Only its vertex and pixel stages
		 * (ProgramStages::VertexPixel: a forward object is no depth segment input, so DrawPipelines builds its depth variant from them and no
		 * Z-prepass pipeline); null until both are compiled. Never freed. The pipeline lane's; a_failed as Find's.
		 */
		const Program* FindFamily(Api::ProgramFamily a_family, const PipelineKey& a_key, const RE::BSShader& a_shader, bool* a_failed = nullptr);
		/**
		 * @brief Which family program a key draws with (FindFamily's entries, UpdateLane's ids): its slot's program-id bit, distinct from every
		 * LightingProgramId.
		 */
		static std::uint64_t FamilyProgramId(Api::ProgramFamily a_family, const PipelineKey& a_key) { return LightingProgramId(a_key) | ProgramFamilySlotOf(a_family).programIdBit; }
		/**
		 * @brief A draw family's forward program: its forward descriptor's pulled builds (tree LOD's: DistantTree's DistantTreeBlock
		 * technique with AlphaTest and without Deferred).
		 */
		const ForwardProgram* FindDrawFamilyForward(Api::DrawFamily a_family, RE::BSShader& a_shader, bool* a_failed = nullptr);

		/** @brief The programs one UpdateLane made or failed, by kind, so the lane retries the keys that waited on them. */
		struct Finished
		{
			std::vector<std::uint64_t> lighting;  // LightingProgramId
			std::vector<std::uint32_t> shadow;    // Utility techniques
			std::vector<std::uint64_t> forward;   // ForwardProgramId
			std::array<std::vector<std::uint64_t>, Extensions::kProgramFamilies> families;  // by family: FamilyProgramId
			// By draw family: its programs (FindDrawFamily) and its forward program (FindDrawFamilyForward).
			std::array<bool, kDrawFamilyLanes> drawFamily{}, drawFamilyForward{};

			bool Empty() const
			{
				for (const auto& ids : families)
					if (!ids.empty())
						return false;
				for (std::size_t f = 0; f < kDrawFamilyLanes; ++f)
					if (drawFamily[f] || drawFamilyForward[f])
						return false;
				return lighting.empty() && shadow.empty() && forward.empty();
			}
			void Clear()
			{
				lighting.clear();
				shadow.clear();
				forward.clear();
				for (auto& ids : families)
					ids.clear();
				drawFamily.fill(false);
				drawFamilyForward.fill(false);
			}
		};
		/** @brief The pipeline lane: admits the compilations that completed since the last call, and appends the programs made or failed to a_finished. */
		void UpdateLane(Finished& a_finished);

		// CS compilation workers request the same stages the runtime programs use, and wait for them (on the stage, not a future).
		// Requests before ORG initialization are retained until StartPrecompile.
		void Precompile(const RE::BSShader& a_shader, bool a_pixel, std::uint32_t a_descriptor);
		void StartPrecompile();

		/** @brief Any thread: the pipeline lane's counters (relaxed reads, for the report). */
		Stats GetStats() const
		{
			Stats result;
			result.requested = laneStats.requested.load(std::memory_order_relaxed);
			result.ready = laneStats.ready.load(std::memory_order_relaxed);
			result.failed = laneStats.failed.load(std::memory_order_relaxed);
			result.fromCache = laneStats.fromCache.load(std::memory_order_relaxed);
			result.shadowRequested = laneStats.shadowRequested.load(std::memory_order_relaxed);
			result.shadowReady = laneStats.shadowReady.load(std::memory_order_relaxed);
			result.shadowFailed = laneStats.shadowFailed.load(std::memory_order_relaxed);
			return result;
		}

	private:
		ShaderPrograms();
		~ShaderPrograms();

		// A program's entry (ShaderPrograms.cpp): its N stages, those not completed yet, and the program once they all are.
		template <class P, std::size_t N>
		struct ProgramEntry;
		using Entry = ProgramEntry<Program, 5>;  // vertex, pixel, depth pixel, pulled vertex, pulled depth pixel
		using ShadowEntry = ProgramEntry<ShadowProgram, 2>;
		using DrawFamilyEntry = ProgramEntry<DrawProgram, 4>;  // vertex, pixel, depth vertex, depth pixel
		using ForwardEntry = ProgramEntry<ForwardProgram, 2>;
		using FamilyEntry = ProgramEntry<Program, 2>;  // vertex and pixel alone (ProgramStages::VertexPixel)
		struct StageCache;
		std::unique_ptr<StageCache> stages;
		bool LoadSources();
		/**
		 * @brief The pipeline lane: a new entry in a_slot (a program's first request), its stages a_makeStages' (registered with the stage
		 * cache, waited for as a_user's a_id), a_requested counted; failed, a_failed counted, when one cannot be requested.
		 */
		template <class E, class MakeStages>
		E& MakeEntry(std::unique_ptr<E>& a_slot, std::uint8_t a_user, std::uint64_t a_id, std::atomic<std::uint32_t>* a_requested, std::atomic<std::uint32_t>& a_failed,
			MakeStages&& a_makeStages);
		/**
		 * @brief The pipeline lane: an entry whose stages all completed gets its program, of its stages' binaries, a_ready counted and the
		 * stages found in a cache counted; or fails at its first failed stage (a_failed counted, a_logFailure(stage index, artifact)).
		 */
		template <class E, class LogFailure>
		void FinishEntry(E& a_entry, std::atomic<std::uint32_t>* a_ready, std::atomic<std::uint32_t>& a_failed, LogFailure&& a_logFailure);
		/** @brief UpdateLane: a draw family's entry whose stages all completed (a_user: its StageUser, its programs' or its forward program's). */
		void FinishDrawFamily(std::uint8_t a_user, Finished& a_finished);

		ankerl::unordered_dense::map<std::uint64_t, std::unique_ptr<Entry>> entries;
		ankerl::unordered_dense::map<std::uint32_t, std::unique_ptr<ShadowEntry>> shadowEntries;
		ankerl::unordered_dense::map<std::uint64_t, std::unique_ptr<ForwardEntry>> forwardEntries;
		// By draw family: its programs' entry and its forward program's.
		std::array<std::unique_ptr<DrawFamilyEntry>, kDrawFamilyLanes> drawFamilyEntries;
		std::array<std::unique_ptr<ForwardEntry>, kDrawFamilyLanes> drawFamilyForwardEntries;
		// By family: its entries, and its source (Extensions::ProgramFamilies' sourcePath: Data/Shaders/Effect.hlsl; empty when missing or
		// not taken).
		std::array<ankerl::unordered_dense::map<std::uint64_t, std::unique_ptr<FamilyEntry>>, Extensions::kProgramFamilies> familyEntries;
		std::array<std::vector<std::byte>, Extensions::kProgramFamilies> familySources;
		// By draw family: its source (Extensions::DrawFamilies' sourcePath: Data/Shaders/DistantTree.hlsl; empty when missing or not taken).
		std::array<std::vector<std::byte>, kDrawFamilyLanes> drawFamilySources;
		std::vector<std::byte> source;
		std::vector<std::byte> pulledSource;  // source for the pulled builds (PulledMaterialSource); empty when it could not be made
		std::vector<std::byte> utilitySource;
		std::vector<std::filesystem::path> dependencies;
		// Each source's ShaderCompiler::FingerprintInputs with the dependencies, taken once when they load: every request's key
		// starts from it instead of hashing the source and checking the dependency files again (thousands of keys a launch).
		std::uint64_t sourceFingerprint = 0, pulledFingerprint = 0, utilityFingerprint = 0;
		std::array<std::uint64_t, Extensions::kProgramFamilies> familyFingerprints{};
		std::array<std::uint64_t, kDrawFamilyLanes> drawFamilyFingerprints{};
		bool sourcesLoaded = false;
		std::atomic<bool> sourcesMissing{ false };
		std::atomic<std::uint32_t> loggedFailures{ 0 };
		// The pipeline lane's (requested and ready: the Lighting programs'; failed and fromCache: every kind's but the Utility programs',
		// which count apart), read by GetStats on any thread.
		struct LaneStats
		{
			std::atomic<std::uint32_t> requested{ 0 }, ready{ 0 }, failed{ 0 }, fromCache{ 0 };
			std::atomic<std::uint32_t> shadowRequested{ 0 }, shadowReady{ 0 }, shadowFailed{ 0 };
		};
		LaneStats laneStats;
	};
}
