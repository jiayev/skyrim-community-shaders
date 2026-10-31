#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

/**
 * @brief The pulled builds' source rewrite (DCLF_PULLED): Lighting.hlsl's (and a program family's) rows' inputs read from the
 * draw's rows instead of bound. Standard library only, with the game's inputs in Options, so the game (ShaderPrograms) and
 * tools/shader-compare's dclf-shadertool make the same source from the same file.
 */
namespace DCLF::PulledSource
{
	struct Options
	{
		// By texture register: read at the material row's index (t0-t15 but the shadow mask, and the features' material slots).
		std::array<bool, 128> materialTexture{};
		std::uint32_t shadowMaskSlot = 14;  // read at the pipeline row's index, as its sampler is
		std::uint32_t samplerSlots = 16;    // s0 to s(n-1) read from the rows
		// CS_DCLF_LAZY_ROWS: the block members kept as statics (EagerRowNames); every other one a load where it is read. Null: every
		// member a static.
		const std::unordered_set<std::string>* eager = nullptr;
	};

	struct Result
	{
		std::string source;  // empty with an error
		std::string error;
		std::uint32_t textures = 0, samplers = 0, members = 0, lazy = 0;
	};

	/**
	 * @brief The names a block member may not be a macro of: every identifier the shader sources name as a field (".name") or
	 * assign or declare ("name =", "name +=", "name[i] ="). Read from every shader file (a_files).
	 */
	std::unordered_set<std::string> EagerRowNames(const std::vector<std::filesystem::path>& a_files);

	/** @brief a_source rewritten for a pulled build. */
	Result Rewrite(std::string_view a_source, const Options& a_options);
}
