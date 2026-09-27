#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "ComputeProgram.h"

#	include "RenderGraph/RenderGraphRuntime.h"

#	if defined(CS_HAS_ORG_MODULE_SERVICES) && defined(ORG_MODULE_SERVICES_HAS_DXC)
#		include <ORGModuleServices/ShaderCompiler.h>
#	endif

#	include <filesystem>
#	include <fstream>
#	include <iterator>

std::shared_ptr<const ComputeProgram> ComputeProgram::Load(rhi::Device a_device, const Desc& a_desc)
{
#	if defined(CS_HAS_ORG_MODULE_SERVICES) && defined(ORG_MODULE_SERVICES_HAS_DXC)
	auto* compiler = RenderGraphRuntime::Get().ShaderCompiler();
	if (!compiler) {
		logger::warn("[ORG] {}: no runtime shader compiler", a_desc.source);
		return {};
	}
	const std::filesystem::path path = std::filesystem::path(RenderGraphRuntime::kShaderDirectory) / a_desc.source;
	std::ifstream file(path, std::ios::binary);
	std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
	if (bytes.empty()) {
		logger::error("[ORG] Missing shader source {}", path.string());
		return {};
	}
	org::services::ShaderCompileRequest request{};
	request.sourceName = path.generic_string();
	request.source = std::as_bytes(std::span(bytes));
	request.entryPoint = a_desc.entry;
	request.target = L"cs_6_6";
	request.format = org::services::ShaderBinaryFormat::Spirv;
	request.warningsAsErrors = false;  // as the build-time compilation these replaced
	// Source-level debug info for Nsight and RenderDoc; drivers ignore it, and optimization is unchanged.
	request.debugInfo = true;
	request.includeDirectories = { RenderGraphRuntime::kShaderDirectory };
	// Every shader file is a potential include; the compiler re-hashes one only when it changes.
	request.dependencyFiles = RenderGraphRuntime::ShaderSourceFiles();
	request.defines.push_back({ L"BASICRHI_SHADER_API_VULKAN", L"1" });
	for (const auto& [name, value] : a_desc.defines)
		request.defines.push_back({ name, value });
	// DXC keeps the HLSL entry point's name in the SPIR-V.
	const std::string entry = Util::WStringToString(a_desc.entry);
	const auto artifact = compiler->Compile(std::move(request));
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
	const rhi::SubobjLayout layout{ program->layout->GetHandle() };
	const rhi::SubobjShader shader{ rhi::ShaderStage::Compute, { artifact.binary.data(), static_cast<std::uint32_t>(artifact.binary.size()) }, entry.c_str() };
	const rhi::PipelineStreamItem items[] = { rhi::Make(layout), rhi::Make(shader) };
	if (a_device.CreatePipeline(items, 2, program->pipeline) != rhi::Result::Ok) {
		logger::error("[ORG] {}: the compute pipeline could not be created", a_desc.source);
		return {};
	}
	return program;
#	else
	(void)a_device;
	logger::warn("[ORG] {}: built without ORGModuleServices' compiler", a_desc.source);
	return {};
#	endif
}

#endif
