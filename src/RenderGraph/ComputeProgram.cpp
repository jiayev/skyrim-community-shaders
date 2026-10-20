#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "ComputeProgram.h"

#	include "RenderGraph/RenderGraphRuntime.h"

#	if defined(CS_HAS_ORG_MODULE_SERVICES) && defined(ORG_MODULE_SERVICES_HAS_DXC)
#		include <ORGModuleServices/PipelineRegistry.h>
#		include <ORGModuleServices/ShaderCompiler.h>
#	endif

#	include <cstring>
#	include <filesystem>
#	include <fstream>
#	include <iterator>
#	include <span>

namespace
{
#	if defined(CS_HAS_ORG_MODULE_SERVICES) && defined(ORG_MODULE_SERVICES_HAS_DXC)
	std::uint64_t HashBinary(std::span<const std::byte> a_bytes)
	{
		std::uint64_t hash = 1469598103934665603ull;
		for (const auto byte : a_bytes) {
			hash ^= static_cast<std::uint8_t>(byte);
			hash *= 1099511628211ull;
		}
		return hash;
	}

	std::vector<std::byte> ReadSource(const std::filesystem::path& a_path)
	{
		std::ifstream file(a_path, std::ios::binary);
		const std::vector<char> chars((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
		std::vector<std::byte> bytes(chars.size());
		if (!chars.empty())
			std::memcpy(bytes.data(), chars.data(), chars.size());
		return bytes;
	}

	org::services::ShaderCompileRequest MakeRequest(const ComputeProgram::Desc& a_desc, const std::filesystem::path& a_path, std::span<const std::byte> a_source)
	{
		org::services::ShaderCompileRequest request{};
		request.sourceName = a_path.generic_string();
		request.source = a_source;
		request.entryPoint = a_desc.entry;
		request.target = L"cs_6_6";
		request.format = org::services::ShaderBinaryFormat::Spirv;
		request.warningsAsErrors = false;  // as the build-time compilation these replaced
		// Source-level debug info for Nsight and RenderDoc; drivers ignore it, and optimization is unchanged.
		request.debugInfo = true;
		request.includeDirectories = { RenderGraphRuntime::kShaderDirectory };
		// Every shader file is a potential include; the compiler re-hashes one only when it changes (size or write time), so an edited
		// include gives every program that may read it a new key.
		request.dependencyFiles = RenderGraphRuntime::ShaderSourceFiles();
		request.defines.push_back({ L"BASICRHI_SHADER_API_VULKAN", L"1" });
		for (const auto& [name, value] : a_desc.defines)
			request.defines.push_back({ name, value });
		return request;
	}

	rhi::PipelinePtr CreatePipeline(rhi::Device a_device, rhi::PipelineLayoutHandle a_layout, const std::vector<std::byte>& a_binary, const std::string& a_entry)
	{
		rhi::PipelinePtr pipeline;
		const rhi::SubobjLayout layout{ a_layout };
		// DXC keeps the HLSL entry point's name in the SPIR-V.
		const rhi::SubobjShader shader{ rhi::ShaderStage::Compute, { a_binary.data(), static_cast<std::uint32_t>(a_binary.size()) }, a_entry.c_str() };
		const rhi::PipelineStreamItem items[] = { rhi::Make(layout), rhi::Make(shader) };
		if (a_device.CreatePipeline(items, 2, pipeline) != rhi::Result::Ok)
			return {};
		return pipeline;
	}

	/**
	 * @brief The program's registry rebuild: its source compiled again as it is now (off the owner thread; an unchanged one is a memory
	 * cache hit), and a new SPIR-V's pipeline made under the program's own layout, on the compile's thread. The program is held weakly:
	 * one released meanwhile has nothing to rebuild.
	 */
	org::services::LiveRebuild Rebuild(std::weak_ptr<const ComputeProgram> a_program, rhi::Device a_device, ComputeProgram::Desc a_desc)
	{
		return [a_program = std::move(a_program), a_device, a_desc = std::move(a_desc)](const org::services::LiveRebuildRequest& a_request, org::services::LiveRebuildDone a_done) {
			auto* compiler = RenderGraphRuntime::Get().ShaderCompiler();
			const std::filesystem::path path = std::filesystem::path(RenderGraphRuntime::kShaderDirectory) / a_desc.source;
			const auto source = ReadSource(path);
			if (!compiler || source.empty())
				return a_done({ .error = compiler ? fmt::format("missing shader source {}", path.string()) : std::string("no runtime shader compiler") });
			// The compiler copies the source; a memory-cache hit (nothing changed) completes here, on this thread.
			(void)compiler->CompileAsync(MakeRequest(a_desc, path, source),
				[a_program, a_device, a_request, a_done = std::move(a_done), entry = Util::WStringToString(a_desc.entry)](const org::services::ShaderArtifact& a_artifact) {
					if (!a_artifact)
						return a_done({ .error = fmt::format("SPIR-V build ({}) failed:\n{}", entry, a_artifact.diagnostics.substr(0, 2000)) });
					const std::uint64_t fingerprint = HashBinary(a_artifact.binary);
					if (fingerprint == a_request.fingerprint && !a_request.force)
						return a_done({ .fingerprint = fingerprint });
					const auto program = a_program.lock();
					if (!program)
						return a_done({ .fingerprint = a_request.fingerprint });  // released: nothing binds it
					auto pipeline = CreatePipeline(a_device, program->layout->GetHandle(), a_artifact.binary, entry);
					if (!pipeline)
						return a_done({ .error = fmt::format("the compute pipeline ({}) could not be created", entry) });
					a_done({ .payload = std::make_shared<rhi::PipelinePtr>(std::move(pipeline)), .fingerprint = fingerprint });
				});
		};
	}
#	endif
}

rhi::PipelineHandle ComputeProgram::Pipeline() const
{
#	if defined(CS_HAS_ORG_MODULE_SERVICES) && defined(ORG_MODULE_SERVICES_HAS_DXC)
	// Non-owning: the registry holds the payload it replaced until every submission that may use it has completed.
	return (*static_cast<const rhi::PipelinePtr*>(slot->Peek()))->GetHandle();
#	else
	return {};
#	endif
}

std::shared_ptr<const ComputeProgram> ComputeProgram::Load(rhi::Device a_device, const Desc& a_desc)
{
#	if defined(CS_HAS_ORG_MODULE_SERVICES) && defined(ORG_MODULE_SERVICES_HAS_DXC)
	auto* compiler = RenderGraphRuntime::Get().ShaderCompiler();
	if (!compiler) {
		logger::warn("[ORG] {}: no runtime shader compiler", a_desc.source);
		return {};
	}
	auto& registry = *RenderGraphRuntime::PipelineRegistry();
	// Before the source is read: a reload that starts after rebuilds it (PipelineRegistry::Register).
	const std::uint64_t builtFor = registry.SourcesGeneration();
	const std::filesystem::path path = std::filesystem::path(RenderGraphRuntime::kShaderDirectory) / a_desc.source;
	const auto bytes = ReadSource(path);
	if (bytes.empty()) {
		logger::error("[ORG] Missing shader source {}", path.string());
		return {};
	}
	const std::string entry = Util::WStringToString(a_desc.entry);
	const auto artifact = compiler->Compile(MakeRequest(a_desc, path, bytes));
	if (!artifact) {
		logger::error("[ORG] SPIR-V build of {} ({}) failed:\n{}", a_desc.source, entry, artifact.diagnostics.substr(0, 2000));
		return {};
	}

	auto program = std::make_shared<ComputeProgram>();
	rhi::PushConstantRangeDesc constants{};
	constants.visibility = rhi::ShaderStage::Compute;
	constants.num32BitValues = a_desc.constantWords;
	if (a_device.CreatePipelineLayout(rhi::PipelineLayoutDesc{ .pushConstants = { &constants, 1 }, .flags = rhi::PipelineLayoutFlags::PF_None }, program->layout) !=
		rhi::Result::Ok) {
		logger::error("[ORG] {}: the pipeline layout could not be created", a_desc.source);
		return {};
	}
	auto pipeline = CreatePipeline(a_device, program->layout->GetHandle(), artifact.binary, entry);
	if (!pipeline) {
		logger::error("[ORG] {}: the compute pipeline could not be created", a_desc.source);
		return {};
	}
	// Live while an owner holds the program (the slot is its).
	org::services::LivePipelineDesc live;
	live.id = fmt::format("{} ({})", a_desc.source, entry);
	live.sources = { a_desc.source };
	live.rebuild = Rebuild(program, a_device, a_desc);
	program->slot = registry.Register(std::move(live), std::make_shared<rhi::PipelinePtr>(std::move(pipeline)), HashBinary(artifact.binary), builtFor);
	return program;
#	else
	(void)a_device;
	logger::warn("[ORG] {}: built without ORGModuleServices' compiler", a_desc.source);
	return {};
#	endif
}

#endif
