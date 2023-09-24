#pragma once

#include <RE/B/BSShader.h>

#include <chrono>
#include <condition_variable>
#include <unordered_map>
#include <unordered_set>

static constexpr REL::Version SHADER_CACHE_VERSION = { 0, 0, 0, 11 };

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
		/// <summary>
		/// Adjust the compiler threads based on the compileThreadCount.
		/// </summary>
		/// This will terminate or generate threads as required to match compileThreadCount.
		void AdjustThreadCount();
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

	private:
		ShaderCache();
		void ProcessCompilationSet(std::stop_token stoken);

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

		eastl::vector<std::jthread> compilationThreads;
		std::stop_source ssource;
		std::mutex vertexShadersMutex;
		std::mutex pixelShadersMutex;
		CompilationSet compilationSet;
		std::unordered_map<std::string, std::pair<ID3DBlob*, ShaderCompilationTask::Status>> shaderMap{};
		std::mutex mapMutex;
	};
}
