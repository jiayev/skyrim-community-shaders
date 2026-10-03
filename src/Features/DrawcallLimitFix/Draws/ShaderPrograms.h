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
	 * Program publication is render-thread only. The stage cache also accepts CS compilation workers.
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
		 * @brief The key's program once both stages compiled; requests them on first call. Null otherwise.
		 * @param a_onDemand Set to the stages this call requested that no precompile had (kOnDemand*), 0 otherwise.
		 */
		const Program* Find(const PipelineKey& a_key, RE::BSShader& a_lighting, std::uint8_t* a_onDemand = nullptr);

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
		 * @brief The Utility technique's programs, or null while they build. Programs are never freed: the pointer stays
		 * valid for the process (pipeline builds keep it).
		 * @param a_allowRequest False returns null for an unseen technique without starting its builds.
		 */
		const ShadowProgram* FindShadow(std::uint32_t a_technique, RE::BSShader& a_utility, bool a_allowRequest = true, bool* a_requested = nullptr,
			std::uint8_t* a_onDemand = nullptr);

		/** @brief Collects finished compilations (call once per frame). */
		void Update();

		// CS compilation workers request the same stage futures used by runtime programs.
		// Requests before ORG initialization are retained until StartPrecompile.
		void Precompile(const RE::BSShader& a_shader, bool a_pixel, std::uint32_t a_descriptor);
		void StartPrecompile();

		const Stats& GetStats() const { return stats; }

	private:
		ShaderPrograms();
		~ShaderPrograms();

		struct Entry;  // compilation futures; defined with the compiler (ShaderPrograms.cpp)
		struct ShadowEntry;
		struct StageCache;
		std::unique_ptr<StageCache> stages;
		bool LoadSources();

		ankerl::unordered_dense::map<std::uint64_t, std::unique_ptr<Entry>> entries;
		ankerl::unordered_dense::map<std::uint32_t, std::unique_ptr<ShadowEntry>> shadowEntries;
		std::vector<std::byte> source;
		std::vector<std::byte> pulledSource;  // source for the pulled builds (PulledMaterialSource); empty when it could not be made
		std::vector<std::byte> utilitySource;
		std::vector<std::filesystem::path> dependencies;
		bool sourcesLoaded = false;
		std::atomic<bool> sourcesMissing{ false };
		std::uint32_t loggedFailures = 0;
		Stats stats;
	};
}
