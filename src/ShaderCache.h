#pragma once

#include <RE/B/BSShader.h>

#include "BS_thread_pool.hpp"
#include <chrono>
#include <condition_variable>
#include <unordered_map>
#include <unordered_set>

static constexpr REL::Version SHADER_CACHE_VERSION = { 0, 0, 0, 12 };

using namespace std::chrono;

namespace SIE
{
	enum class ShaderClass
	{
		Vertex,
		Pixel,
		Compute,
		Total,
	};

	class ShaderCompilationTask
	{
	public:
		enum Status
		{
			Pending,
			Failed,
			Completed
		};
		ShaderCompilationTask(ShaderClass shaderClass, const RE::BSShader& shader,
			uint32_t descriptor);
		void Perform() const;

		size_t GetId() const;
		std::string GetString() const;

		bool operator==(const ShaderCompilationTask& other) const;

	protected:
		ShaderClass shaderClass;
		const RE::BSShader& shader;
		uint32_t descriptor;
	};
}

template <>
struct std::hash<SIE::ShaderCompilationTask>
{
	std::size_t operator()(const SIE::ShaderCompilationTask& task) const noexcept
	{
		return task.GetId();
	}
};

namespace SIE
{
	class CompilationSet
	{
	public:
		std::optional<ShaderCompilationTask> WaitTake(std::stop_token stoken);
		void Add(const ShaderCompilationTask& task);
		void Complete(const ShaderCompilationTask& task);
		void Clear();
		std::string GetHumanTime(double a_totalms);
		double GetEta();
		std::string GetStatsString(bool a_timeOnly = false);
		std::atomic<uint64_t> completedTasks = 0;
		std::atomic<uint64_t> totalTasks = 0;
		std::atomic<uint64_t> failedTasks = 0;
		std::atomic<uint64_t> cacheHitTasks = 0;  // number of compiles of a previously seen shader combo
		std::mutex compilationMutex;

	private:
		std::unordered_set<ShaderCompilationTask> availableTasks;
		std::unordered_set<ShaderCompilationTask> tasksInProgress;
		std::unordered_set<ShaderCompilationTask> processedTasks;  // completed or failed
		std::condition_variable_any conditionVariable;
		std::chrono::steady_clock::time_point lastReset = high_resolution_clock::now();
		std::chrono::steady_clock::time_point lastCalculation = high_resolution_clock::now();
		double totalMs = (double)duration_cast<std::chrono::milliseconds>(lastReset - lastReset).count();
	};

	class ShaderCache
	{
	public:
		static ShaderCache& Instance()
		{
			static ShaderCache instance;
			return instance;
		}

		inline static bool IsSupportedShader(const RE::BSShader::Type type)
		{
			if (!REL::Module::IsVR())
				return type == RE::BSShader::Type::Lighting ||
				       type == RE::BSShader::Type::BloodSplatter ||
				       type == RE::BSShader::Type::DistantTree ||
				       type == RE::BSShader::Type::Sky ||
				       type == RE::BSShader::Type::Grass ||
				       type == RE::BSShader::Type::Particle ||
				       type == RE::BSShader::Type::Water;
			return type == RE::BSShader::Type::Lighting ||
			       type == RE::BSShader::Type::Grass;
		}

		inline static bool IsSupportedShader(const RE::BSShader& shader)
		{
			return IsSupportedShader(shader.shaderType.get());
		}

		bool IsCompiling();
		bool IsEnabled() const;
		void SetEnabled(bool value);
		bool IsAsync() const;
		void SetAsync(bool value);
		bool IsDump() const;
		void SetDump(bool value);

		bool IsDiskCache() const;
		void SetDiskCache(bool value);
		void DeleteDiskCache();
		void ValidateDiskCache();
		void WriteDiskCacheInfo();
		void Clear();

		bool AddCompletedShader(ShaderClass shaderClass, const RE::BSShader& shader, uint32_t descriptor, ID3DBlob* a_blob);
		ID3DBlob* GetCompletedShader(const std::string a_key);
		ID3DBlob* GetCompletedShader(const SIE::ShaderCompilationTask& a_task);
		ID3DBlob* GetCompletedShader(ShaderClass shaderClass, const RE::BSShader& shader, uint32_t descriptor);
		ShaderCompilationTask::Status GetShaderStatus(const std::string a_key);
		std::string GetShaderStatsString(bool a_timeOnly = false);

		RE::BSGraphics::VertexShader* GetVertexShader(const RE::BSShader& shader, uint32_t descriptor);
		RE::BSGraphics::PixelShader* GetPixelShader(const RE::BSShader& shader,
			uint32_t descriptor);

		RE::BSGraphics::VertexShader* MakeAndAddVertexShader(const RE::BSShader& shader,
			uint32_t descriptor);
		RE::BSGraphics::PixelShader* MakeAndAddPixelShader(const RE::BSShader& shader,
			uint32_t descriptor);

		uint64_t GetCachedHitTasks();
		uint64_t GetCompletedTasks();
		uint64_t GetFailedTasks();
		uint64_t GetTotalTasks();
		void IncCacheHitTasks();
		void ToggleErrorMessages();
		bool IsHideErrors();

		int32_t compilationThreadCount = std::max(static_cast<int32_t>(std::thread::hardware_concurrency()) - 1, 1);
		int32_t backgroundCompilationThreadCount = std::max(static_cast<int32_t>(std::thread::hardware_concurrency()) / 2, 1);
		BS::thread_pool compilationPool{};
		bool backgroundCompilation = false;
		bool menuLoaded = false;

		enum class LightingShaderTechniques
		{
			None = 0,
			Envmap = 1,
			Glowmap = 2,
			Parallax = 3,
			Facegen = 4,
			FacegenRGBTint = 5,
			Hair = 6,
			ParallaxOcc = 7,
			MTLand = 8,
			LODLand = 9,
			Snow = 10,  // unused
			MultilayerParallax = 11,
			TreeAnim = 12,
			LODObjects = 13,
			MultiIndexSparkle = 14,
			LODObjectHD = 15,
			Eye = 16,
			Cloud = 17,  // unused
			LODLandNoise = 18,
			MTLandLODBlend = 19,
			Outline = 20,
		};

		enum class LightingShaderFlags
		{
			VC = 1 << 0,
			Skinned = 1 << 1,
			ModelSpaceNormals = 1 << 2,
			// flags 3 to 8 are unused
			Specular = 1 << 9,
			SoftLighting = 1 << 10,
			RimLighting = 1 << 11,
			BackLighting = 1 << 12,
			ShadowDir = 1 << 13,
			DefShadow = 1 << 14,
			ProjectedUV = 1 << 15,
			AnisoLighting = 1 << 16,
			AmbientSpecular = 1 << 17,
			WorldMap = 1 << 18,
			BaseObjectIsSnow = 1 << 19,
			DoAlphaTest = 1 << 20,
			Snow = 1 << 21,
			CharacterLight = 1 << 22,
			AdditionalAlphaMask = 1 << 23,
		};

		enum class WaterShaderTechniques
		{
			Underwater = 8,
			Lod = 9,
			Stencil = 10,
			Simple = 11,
		};

		enum class WaterShaderFlags
		{
			Vc = 1 << 0,
			NormalTexCoord = 1 << 1,
			Reflections = 1 << 2,
			Refractions = 1 << 3,
			Depth = 1 << 4,
			Interior = 1 << 5,
			Wading = 1 << 6,
			VertexAlphaDepth = 1 << 7,
			Cubemap = 1 << 8,
			Flowmap = 1 << 9,
			BlendNormals = 1 << 10,
		};

	private:
		ShaderCache();
		void ManageCompilationSet(std::stop_token stoken);
		void ProcessCompilationSet(std::stop_token stoken, SIE::ShaderCompilationTask task);

		~ShaderCache();

		std::array<std::map<uint32_t, std::unique_ptr<RE::BSGraphics::VertexShader>>,
			static_cast<size_t>(RE::BSShader::Type::Total)>
			vertexShaders;
		std::array<std::map<uint32_t, std::unique_ptr<RE::BSGraphics::PixelShader>>,
			static_cast<size_t>(RE::BSShader::Type::Total)>
			pixelShaders;

		bool isEnabled = false;
		bool isDiskCache = false;
		bool isAsync = true;
		bool isDump = false;
		bool hideError = false;

		std::stop_source ssource;
		std::mutex vertexShadersMutex;
		std::mutex pixelShadersMutex;
		CompilationSet compilationSet;
		std::unordered_map<std::string, std::pair<ID3DBlob*, ShaderCompilationTask::Status>> shaderMap{};
		std::mutex mapMutex;
	};
}
