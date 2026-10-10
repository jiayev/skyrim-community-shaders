#pragma once

#include <cstddef>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "Features/DrawcallLimitFix/Scene/Records.h"

namespace DCLF
{
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
	 * source, every shader file under Data/Shaders and the defines, and cached on disk.
	 *
	 * The compiles run on DCLF's build executor (BuildExecutor.h). Each stage's completion callback pushes its key to a
	 * lock-free queue, which the consumer drains: the entries waiting for the stage are told, and an entry whose stages have
	 * all completed gets its program. Nothing polls a compile.
	 *
	 * One consumer owns the programs (T6b2c steps 4 and 9): the pipeline lane (BuildExecutor.h, WakePipelineLane), which a completion
	 * wakes: every kind's entries (Find, FindShadow, FindTreeLod, FindForward, FindForwardTreeLod) are made and finished on it
	 * (UpdateLane). The stage cache also accepts CS compilation workers.
	 */
	class ShaderPrograms
	{
	public:
		struct Program
		{
			std::vector<std::byte> vertex;  // SPIR-V
			std::vector<std::byte> pixel;
			std::vector<std::byte> depthPixel;  // the same permutation built with DCLF_DEPTH_ONLY
			// The Z-prepass's plain draws' stages (DCLF_PULLED; MainOpaquePass): the vertex stage fetching its draw's vertices and
			// rows itself, and the depth pixel stage reading its material row's inputs itself (PulledMaterialSource).
			std::vector<std::byte> pulledVertex;
			std::vector<std::byte> pulledDepthPixel;
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
		 * @brief Tree LOD's programs (dclf-lod.md, "Tree LOD: the draws"): `Data/Shaders/DistantTree.hlsl`'s pulled builds
		 * (DCLF_PULLED), the colour technique (deferred) and the depth technique, each a vertex and a pixel stage. Requested on the
		 * first call; null until all four are compiled. Never freed. The pipeline lane's, as the other Find* below; a_failed as Find's.
		 */
		struct TreeLodProgram
		{
			std::vector<std::byte> vertex, pixel;            // DistantTreeBlock, deferred
			std::vector<std::byte> depthVertex, depthPixel;  // Depth (RENDER_DEPTH)
		};
		const TreeLodProgram* FindTreeLod(RE::BSShader& a_distantTree, bool* a_failed = nullptr);

		/**
		 * @brief A forward view's program (the water reflection's cube map faces: dclf-lod.md, "Water reflections"): one forward
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
		/** @brief Tree LOD's forward program: DistantTree's DistantTreeBlock technique with AlphaTest and without Deferred, pulled. */
		const ForwardProgram* FindForwardTreeLod(RE::BSShader& a_distantTree, bool* a_failed = nullptr);

		/** @brief The programs one UpdateLane made or failed, by kind, so the lane retries the keys that waited on them. */
		struct Finished
		{
			std::vector<std::uint64_t> lighting;  // LightingProgramId
			std::vector<std::uint32_t> shadow;    // Utility techniques
			std::vector<std::uint64_t> forward;   // ForwardProgramId
			bool treeLod = false, forwardTreeLod = false;

			bool Empty() const { return lighting.empty() && shadow.empty() && forward.empty() && !treeLod && !forwardTreeLod; }
			void Clear()
			{
				lighting.clear();
				shadow.clear();
				forward.clear();
				treeLod = forwardTreeLod = false;
			}
		};
		/** @brief The pipeline lane: admits the compilations that completed since the last call, and appends the programs now made or failed to a_finished. */
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

		struct Entry;  // the program's stages; defined with the compiler (ShaderPrograms.cpp)
		struct ShadowEntry;
		struct StageCache;
		std::unique_ptr<StageCache> stages;
		bool LoadSources();

		ankerl::unordered_dense::map<std::uint64_t, std::unique_ptr<Entry>> entries;
		ankerl::unordered_dense::map<std::uint32_t, std::unique_ptr<ShadowEntry>> shadowEntries;
		struct TreeLodEntry;
		std::unique_ptr<TreeLodEntry> treeLodEntry;
		struct ForwardEntry;
		ankerl::unordered_dense::map<std::uint64_t, std::unique_ptr<ForwardEntry>> forwardEntries;
		std::unique_ptr<ForwardEntry> forwardTreeLodEntry;
		std::vector<std::byte> distantTreeSource;
		std::vector<std::byte> source;
		std::vector<std::byte> pulledSource;  // source for the pulled builds (PulledMaterialSource); empty when it could not be made
		std::vector<std::byte> utilitySource;
		std::vector<std::filesystem::path> dependencies;
		// Each source's ShaderCompiler::FingerprintInputs with the dependencies, taken once when they load: every request's key
		// starts from it instead of hashing the source and checking the dependency files again (2,000 keys a launch).
		std::uint64_t sourceFingerprint = 0, pulledFingerprint = 0, utilityFingerprint = 0, distantTreeFingerprint = 0;
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
