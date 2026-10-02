#include "ShaderPrograms.h"

#include <atomic>
#include <chrono>
#include <fstream>
#include <iterator>
#include <mutex>

#include <Tracy/Tracy.hpp>

#include "RenderGraph/RenderGraphRuntime.h"
#include "ShaderCache.h"
#include "Features/DrawcallLimitFix/Common/Switches.h"

#if defined(CS_HAS_ORG_MODULE_SERVICES) && defined(ORG_MODULE_SERVICES_HAS_DXC)
#	include <ORGModuleServices/ShaderCompiler.h>
#	define DCLF_HAS_SHADER_COMPILER 1
#endif

namespace DCLF
{
	namespace
	{
		constexpr const char* kSourcePath = "Data/Shaders/Lighting.hlsl";
		constexpr const char* kUtilitySourcePath = "Data/Shaders/Utility.hlsl";
		constexpr const char* kShaderDirectory = RenderGraphRuntime::kShaderDirectory;
		constexpr std::size_t kMaxLoggedFailures = 8;

		std::wstring Widen(const std::string& a_value)
		{
			return std::wstring(a_value.begin(), a_value.end());
		}

		// CS_DCLF_SHADER_DEBUG=1 builds the SPIR-V with source-level debug info (for Nsight and
		// RenderDoc; the source is embedded, and SnapshotSources' copy is there for editing), still optimized. Part of the
		// compile key, so the debug builds are cached beside the release ones. There is deliberately no
		// -Od form: unoptimized code reads per-frame constant buffers (VS b6, PS b7) that the epochs do
		// not supply, so every candidate is skipped and DCLF draws nothing.
		bool ShaderDebug()
		{
			return SwitchEnabled(Switch::ShaderDebug);
		}

		// The shader tree as the game sees it through MO2's virtual file system, copied to a real
		// directory a debugger outside the VFS can use as a source search path. The debug info names
		// files relative to the game directory (Data/Shaders/...), so the copy keeps that layout.
		void SnapshotSources(const std::vector<std::filesystem::path>& a_files)
		{
			auto root = std::filesystem::path(SwitchValue(Switch::ShaderSourceDir));
			if (root.empty()) {
				root = SwitchesDirectory();
				if (root.empty())
					return;
				root /= L"CommunityShaders-ShaderSource";
			}
			std::size_t copied = 0;
			for (const auto& file : a_files) {
				std::error_code ec;
				const auto destination = root / file.lexically_normal();
				std::filesystem::create_directories(destination.parent_path(), ec);
				if (!ec && std::filesystem::copy_file(file, destination, std::filesystem::copy_options::overwrite_existing, ec))
					++copied;
			}
			logger::info("[DCLF] Shader debug info on; {} of {} shader files copied to {}", copied, a_files.size(), root.string());
		}
	}

	struct ShaderPrograms::ShadowEntry
	{
#if defined(DCLF_HAS_SHADER_COMPILER)
		std::shared_future<org::services::ShaderArtifact> vertex;
		std::shared_future<org::services::ShaderArtifact> pixel;
#endif
		std::unique_ptr<ShadowProgram> program;
		bool failed = false;
		std::uint8_t onDemand = 0;  // kOnDemand*: the stages no precompile had asked for
		std::chrono::steady_clock::time_point requested;
	};

	struct ShaderPrograms::Entry
	{
#if defined(DCLF_HAS_SHADER_COMPILER)
		std::shared_future<org::services::ShaderArtifact> vertex;
		std::shared_future<org::services::ShaderArtifact> pixel;
		std::shared_future<org::services::ShaderArtifact> depthPixel;
#endif
		std::unique_ptr<Program> program;
		bool failed = false;
		std::uint8_t onDemand = 0;  // kOnDemand*: the stages no precompile had asked for
		std::chrono::steady_clock::time_point requested;
	};

	std::string ShaderPrograms::OnDemandStages(std::uint8_t a_stages)
	{
		std::string text;
		for (const auto& [bit, name] : { std::pair{ kOnDemandVertex, "VS" }, std::pair{ kOnDemandPixel, "PS" }, std::pair{ kOnDemandDepthPixel, "depth PS" } })
			if (a_stages & bit)
				text += fmt::format("{}{}", text.empty() ? "" : ", ", name);
		return text;
	}

	ShaderPrograms& ShaderPrograms::Get()
	{
		static ShaderPrograms programs;
		return programs;
	}

	bool ShaderPrograms::Enabled() const
	{
		return !sourcesMissing && RenderGraphRuntime::Get().ShaderCompiler() != nullptr;
	}

	bool ShaderPrograms::LoadSources()
	{
		if (sourcesLoaded)
			return true;
		std::ifstream file(kSourcePath, std::ios::binary);
		std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
		if (bytes.empty()) {
			logger::error("[DCLF] {} is missing; no SPIR-V builds", kSourcePath);
			sourcesMissing = true;
			return false;
		}
		source.resize(bytes.size());
		std::memcpy(source.data(), bytes.data(), bytes.size());
		// Every shader file is a potential include; the compiler re-hashes one only when it changes.
		dependencies = RenderGraphRuntime::ShaderSourceFiles();
		{
			std::ifstream utility(kUtilitySourcePath, std::ios::binary);
			std::vector<char> utilityBytes((std::istreambuf_iterator<char>(utility)), std::istreambuf_iterator<char>());
			utilitySource.resize(utilityBytes.size());
			if (!utilityBytes.empty())
				std::memcpy(utilitySource.data(), utilityBytes.data(), utilityBytes.size());
			else
				logger::warn("[DCLF] {} is missing; the shadow views stay native", kUtilitySourcePath);
		}
		sourcesLoaded = true;
		logger::info("[DCLF] SPIR-V builds of {}: {} shader files tracked as dependencies", kSourcePath, dependencies.size());
		if (ShaderDebug())
			SnapshotSources(dependencies);
		return true;
	}

#if defined(DCLF_HAS_SHADER_COMPILER)
	namespace
	{
		std::shared_future<org::services::ShaderArtifact> RequestStage(std::span<const std::byte> a_source, const std::vector<std::filesystem::path>& a_dependencies,
			const RE::BSShader& a_lighting, bool a_pixel, std::uint32_t a_descriptor, bool a_depthOnly = false, const char* a_sourceName = kSourcePath)
		{
			org::services::ShaderCompileRequest request{};
			ZoneScopedN("CS.DCLF.Shaders.RequestStage");
			request.sourceName = a_sourceName;
			request.source = a_source;
			request.entryPoint = L"main";
			request.target = a_pixel ? L"ps_6_6" : L"vs_6_6";
			request.format = org::services::ShaderBinaryFormat::Spirv;
			request.warningsAsErrors = false;
			request.debugInfo = ShaderDebug();
			request.languageVersion = L"2018";  // CS's shaders are written for FXC's semantics
			request.includeDirectories = { kShaderDirectory };
			request.dependencyFiles = a_dependencies;
			for (const auto& [name, value] : SIE::ShaderCache::GetCompileDefines(a_lighting, a_pixel ? SIE::ShaderClass::Pixel : SIE::ShaderClass::Vertex, a_descriptor))
				request.defines.push_back({ Widen(name), Widen(value) });
			request.defines.push_back({ L"DCLF_ORG", L"1" });
			// The Z-prepass build of the pixel stage: everything past the alpha test is compiled away, so it
			// reads only what the discard needs and none of the per-frame pixel bindings.
			if (a_depthOnly)
				request.defines.push_back({ L"DCLF_DEPTH_ONLY", L"1" });
			// The five per-object PerGeometry variables come from a GPU-resident table indexed by the draw's
			// own object index instead of from its constant buffer (DCLF_BINDLESS), and so do the three registers
			// that would otherwise make the binding record per-object (DCLF_BINDLESS_DRAW): the alpha test
			// reference, the emissive multiplier, and Light Limit Fix's room index and shadow bit mask.
			request.defines.push_back({ L"DCLF_BINDLESS", L"1" });
			request.defines.push_back({ L"DCLF_BINDLESS_DRAW", L"1" });
			// The shadow views' Utility stages draw with plain indirect draws (ShadowViewPass) and fetch their own indices,
			// vertices and per-draw words (Utility.hlsl).
			if (a_sourceName != kSourcePath)
				request.defines.push_back({ L"DCLF_PULLED", L"1" });
			// The first few define sets, to reproduce builds with the DXC command line.
			static std::atomic<std::uint32_t> logged = 0;
			if (logged.fetch_add(1) < 4) {
				std::string text;
				for (const auto& define : request.defines)
					text += fmt::format(" -D {}{}{}", Util::WStringToString(define.name), define.value.empty() ? "" : "=", Util::WStringToString(define.value));
				logger::info("[DCLF] SPIR-V build of {} {} {:08X} defines:{}", a_sourceName == kSourcePath ? "Lighting" : "Utility",
					a_pixel ? (a_depthOnly ? "PS (depth)" : "PS") : "VS", a_descriptor, text);
			}
			const auto shift = [&](const wchar_t* a_flag, std::uint32_t a_value) {
				request.arguments.insert(request.arguments.end(), { a_flag, std::to_wstring(a_value), L"0" });
			};
			shift(L"-fvk-b-shift", kBindingShiftB);
			shift(L"-fvk-t-shift", kBindingShiftT);
			shift(L"-fvk-s-shift", kBindingShiftS);
			shift(L"-fvk-u-shift", kBindingShiftU);
			return RenderGraphRuntime::Get().ShaderCompiler()->CompileAsync(std::move(request));
		}
	}
#endif

	struct ShaderPrograms::StageCache
	{
		std::mutex mutex;
		std::mutex requestMutex;
		bool started = false;
		struct Pending { const RE::BSShader* shader; bool pixel; std::uint32_t descriptor; };
		std::vector<Pending> pending;
#if defined(DCLF_HAS_SHADER_COMPILER)
		ankerl::unordered_dense::map<std::uint64_t, std::shared_future<org::services::ShaderArtifact>> futures;
		// a_created: set when this call made the request (no earlier request, precompile or runtime, had).
		std::shared_future<org::services::ShaderArtifact> Request(ShaderPrograms& owner, const RE::BSShader& shader,
			bool pixel, std::uint32_t descriptor, bool depth = false, bool* a_created = nullptr)
		{
			const bool utility = shader.shaderType.get() == RE::BSShader::Type::Utility;
			const std::uint64_t key = descriptor | (std::uint64_t(pixel) << 32) | (std::uint64_t(depth) << 33) | (std::uint64_t(utility) << 34);
			{
				std::lock_guard lock(mutex);
				if (const auto found = futures.find(key); found != futures.end())
					return found->second;
			}
			// Cache hits must not wait behind another permutation's filesystem key construction.
			std::lock_guard requestLock(requestMutex);
			{
				std::lock_guard lock(mutex);
				if (const auto found = futures.find(key); found != futures.end())
					return found->second;
			}
			if (!owner.Enabled() || !owner.LoadSources() || (utility && owner.utilitySource.empty()))
				return {};
			// Like the program entries, stage futures live for this source set's lifetime.
			// A VS shared by several PS permutations must not rescan the shader tree each time.
			auto future = RequestStage(utility ? owner.utilitySource : owner.source, owner.dependencies, shader,
				pixel, descriptor, depth, utility ? kUtilitySourcePath : kSourcePath);
			{
				std::lock_guard lock(mutex);
				futures.emplace(key, future);
			}
			if (a_created)
				*a_created = future.valid();
			return future;
		}
#endif
	};

	ShaderPrograms::ShaderPrograms() : stages(std::make_unique<StageCache>()) {}
	ShaderPrograms::~ShaderPrograms() = default;

	void ShaderPrograms::Precompile(const RE::BSShader& a_shader, bool a_pixel, std::uint32_t a_descriptor)
	{
#if defined(DCLF_HAS_SHADER_COMPILER)
		const bool enabled = SwitchValue(Switch::Precompile) != "0";
		if (!enabled)
			return;
		const auto type = a_shader.shaderType.get();
		if (type != RE::BSShader::Type::Lighting && type != RE::BSShader::Type::Utility)
			return;
		{
			std::lock_guard lock(stages->mutex);
			if (!stages->started) {
				stages->pending.push_back({ &a_shader, a_pixel, a_descriptor });
				return;
			}
		}
		ZoneScopedN("CS.DCLF.Shaders.Precompile");
		auto stage = stages->Request(*this, a_shader, a_pixel, a_descriptor);
		auto depth = a_pixel && type == RE::BSShader::Type::Lighting ? stages->Request(*this, a_shader, true, a_descriptor, true) : decltype(stage){};
		// Stay inside CS's bounded compilation workers until this task is done, including cache hits.
		for (const auto* future : { &stage, &depth }) {
			if (!future->valid()) continue;
			const auto& artifact = future->get();
			if (!artifact) {
				static std::atomic<std::uint32_t> failures{ 0 };
				if (failures.fetch_add(1) < kMaxLoggedFailures)
					logger::warn("[DCLF] ORG precompile {} {} {:08X} failed: {}", type == RE::BSShader::Type::Lighting ? "Lighting" : "Utility",
						a_pixel ? "PS" : "VS", a_descriptor, artifact.diagnostics.substr(0, 1500));
			}
		}
#endif
	}

	void ShaderPrograms::StartPrecompile()
	{
		std::vector<StageCache::Pending> pending;
		{
			std::lock_guard lock(stages->mutex);
			if (stages->started) return;
			stages->started = true;
			pending.swap(stages->pending);
		}
		for (const auto request : pending)
			SIE::ShaderCache::Instance().compilationPool.detach_task([this, request] {
				try { Precompile(*request.shader, request.pixel, request.descriptor); }
				catch (const std::exception& e) { logger::warn("[DCLF] ORG shader precompile failed: {}", e.what()); }
				SIE::ShaderCache::Instance().NotifyPoolProgress();
			});
	}

	const ShaderPrograms::Program* ShaderPrograms::Find(const PipelineKey& a_key, RE::BSShader& a_lighting, std::uint8_t* a_onDemand)
	{
		if (a_onDemand)
			*a_onDemand = 0;
		if (!Enabled())
			return nullptr;
		const std::uint64_t id = (static_cast<std::uint64_t>(a_key.vertexDescriptor) << 32) | a_key.pixelDescriptor;
		auto [it, inserted] = entries.try_emplace(id);
		if (inserted) {
			it->second = std::make_unique<Entry>();
#if defined(DCLF_HAS_SHADER_COMPILER)
			bool created[3]{};
			it->second->vertex = stages->Request(*this, a_lighting, false, a_key.vertexDescriptor, false, &created[0]);
			it->second->pixel = stages->Request(*this, a_lighting, true, a_key.pixelDescriptor, false, &created[1]);
			it->second->depthPixel = stages->Request(*this, a_lighting, true, a_key.pixelDescriptor, true, &created[2]);
			it->second->onDemand = static_cast<std::uint8_t>((created[0] ? kOnDemandVertex : 0) | (created[1] ? kOnDemandPixel : 0) | (created[2] ? kOnDemandDepthPixel : 0));
			it->second->requested = std::chrono::steady_clock::now();
			if (a_onDemand)
				*a_onDemand = it->second->onDemand;
#else
			(void)a_lighting;
			it->second->failed = true;
#endif
			++stats.requested;
		}
		return it->second->program.get();
	}

	const ShaderPrograms::ShadowProgram* ShaderPrograms::FindShadow(std::uint32_t a_technique, RE::BSShader& a_utility, bool a_allowRequest, bool* a_requested,
		std::uint8_t* a_onDemand)
	{
		if (a_requested)
			*a_requested = false;
		if (a_onDemand)
			*a_onDemand = 0;
		if (!Enabled())
			return nullptr;
		auto it = shadowEntries.find(a_technique);
		if (it == shadowEntries.end() && !a_allowRequest)
			return nullptr;
		const bool inserted = it == shadowEntries.end();
		if (inserted) {
			it = shadowEntries.try_emplace(a_technique).first;
			if (a_requested)
				*a_requested = true;
			it->second = std::make_unique<ShadowEntry>();
#if defined(DCLF_HAS_SHADER_COMPILER)
			// Both stages take the same technique: Utility's descriptor is the technique itself, not a
			// pair of vertex and pixel descriptors as the Lighting shader's is.
			bool created[2]{};
			it->second->vertex = stages->Request(*this, a_utility, false, a_technique, false, &created[0]);
			it->second->pixel = stages->Request(*this, a_utility, true, a_technique, false, &created[1]);
			it->second->onDemand = static_cast<std::uint8_t>((created[0] ? kOnDemandVertex : 0) | (created[1] ? kOnDemandPixel : 0));
			it->second->requested = std::chrono::steady_clock::now();
			if (a_onDemand)
				*a_onDemand = it->second->onDemand;
#else
			(void)a_utility;
			it->second->failed = true;
#endif
			++stats.shadowRequested;
		}
		return it->second->program.get();
	}

	void ShaderPrograms::Update()
	{
#if defined(DCLF_HAS_SHADER_COMPILER)
		const auto ready = [](const auto& a_future) {
			return a_future.valid() && a_future.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
		};
		// The outcome of a stage no precompile had asked for (logged with its reason when it was requested): compiled now, or
		// found in the disk cache, which a precompile run before would also have filled.
		const auto onDemandOutcome = [](const char* a_what, std::uint8_t a_stages, std::chrono::steady_clock::time_point a_requested,
										 std::initializer_list<std::pair<std::uint8_t, const org::services::ShaderArtifact*>> a_artifacts) {
			if (!a_stages)
				return;
			std::uint8_t compiled = 0, cached = 0, failed = 0;
			for (const auto& [bit, artifact] : a_artifacts) {
				if (!(a_stages & bit))
					continue;
				(!*artifact ? failed : artifact->fromCache ? cached : compiled) |= bit;
			}
			const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a_requested).count();
			logger::warn("[DCLF] on-demand SPIR-V {} done after {:.0f} ms: compiled now [{}], from the disk cache [{}], failed [{}]", a_what, ms,
				OnDemandStages(compiled), OnDemandStages(cached), OnDemandStages(failed));
		};
		for (auto& [id, entryPointer] : entries) {
			auto& entry = *entryPointer;
			if (entry.program || entry.failed || !ready(entry.vertex) || !ready(entry.pixel) || !ready(entry.depthPixel))
				continue;
			const auto& vertex = entry.vertex.get();
			const auto& pixel = entry.pixel.get();
			const auto& depthPixel = entry.depthPixel.get();
			onDemandOutcome(fmt::format("Lighting VS {:08X} PS {:08X}", static_cast<std::uint32_t>(id >> 32), static_cast<std::uint32_t>(id)).c_str(), entry.onDemand,
				entry.requested, { { kOnDemandVertex, &vertex }, { kOnDemandPixel, &pixel }, { kOnDemandDepthPixel, &depthPixel } });
			if (!vertex || !pixel || !depthPixel) {
				entry.failed = true;
				++stats.failed;
				if (loggedFailures++ < kMaxLoggedFailures) {
					const auto& failed = !vertex ? vertex : pixel;
					std::string diagnostics = failed.diagnostics.substr(0, 1500);
					logger::warn("[DCLF] SPIR-V build of Lighting {} {:08X} failed:\n{}", !vertex ? "VS" : "PS",
						static_cast<std::uint32_t>(!vertex ? (id >> 32) : id), diagnostics);
				}
				continue;
			}
			entry.program = std::make_unique<Program>(Program{ vertex.binary, pixel.binary, depthPixel.binary });
			++stats.ready;
			stats.fromCache += (vertex.fromCache ? 1 : 0) + (pixel.fromCache ? 1 : 0) + (depthPixel.fromCache ? 1 : 0);
		}
		for (auto& [technique, entryPointer] : shadowEntries) {
			auto& entry = *entryPointer;
			if (entry.program || entry.failed || !ready(entry.vertex) || !ready(entry.pixel))
				continue;
			const auto& vertex = entry.vertex.get();
			const auto& pixel = entry.pixel.get();
			onDemandOutcome(fmt::format("Utility technique {:08X}", technique).c_str(), entry.onDemand, entry.requested,
				{ { kOnDemandVertex, &vertex }, { kOnDemandPixel, &pixel } });
			if (!vertex || !pixel) {
				entry.failed = true;
				++stats.shadowFailed;
				if (loggedFailures++ < kMaxLoggedFailures) {
					const auto& failed = !vertex ? vertex : pixel;
					logger::warn("[DCLF] SPIR-V build of Utility {} {:08X} failed:\n{}", !vertex ? "VS" : "PS", technique, failed.diagnostics.substr(0, 1500));
				}
				continue;
			}
			entry.program = std::make_unique<ShadowProgram>(ShadowProgram{ vertex.binary, pixel.binary });
			++stats.shadowReady;
			stats.fromCache += (vertex.fromCache ? 1 : 0) + (pixel.fromCache ? 1 : 0);
		}
#endif
	}
}
