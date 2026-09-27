#pragma once

#if defined(CS_HAS_RENDER_GRAPH)

#	include <rhi.h>

#	include <cstdint>
#	include <memory>
#	include <string>
#	include <utility>
#	include <vector>

/**
 * @brief A render-graph compute shader: the pipeline and the layout its push constants are bound through.
 *
 * Every feature's compute passes create theirs here, from the HLSL under Data/Shaders, compiled to SPIR-V at runtime by
 * RenderGraphRuntime's ORGModuleServices compiler (content-addressed, disk-cached under Data/ShaderCache/ORG), with
 * BasicRHI's descriptor-heap ABI. The shaders fetch everything else from the descriptor heap by index.
 */
struct ComputeProgram
{
	rhi::PipelineLayoutPtr layout;
	rhi::PipelinePtr pipeline;

	struct Desc
	{
		std::string source;  // relative to Data/Shaders, e.g. "DrawcallLimitFix/BuildDrawsCS.hlsl"
		std::wstring entry = L"main";
		std::uint32_t constantWords = 0;  // the push-constant block, in 32-bit words
		std::vector<std::pair<std::wstring, std::wstring>> defines;
	};

	/**
	 * @brief Compiles (or finds in the cache) and creates the program; null, logged, when the runtime has no compiler or
	 * the build fails. Blocks on a compile the cache does not have, so call it at setup, not per frame.
	 */
	static std::shared_ptr<const ComputeProgram> Load(rhi::Device a_device, const Desc& a_desc);
};

#endif
