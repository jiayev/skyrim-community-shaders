// The pulled builds' source rewrite (DCLF_PULLED): PulledSource.h. Standard library only, so tools/shader-compare's dclf-shadertool
// builds it as the game does.
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/PulledSource.h"

#include <cctype>
#include <cstring>
#include <format>
#include <fstream>
#include <iterator>
#include <regex>
#include <sstream>

namespace DCLF::PulledSource
{
	namespace
	{
		// A block's row and its stage's place in it (DCLFObjects.hlsli).
		struct PulledBlock
		{
			const char* row = nullptr;
			const char* offset = nullptr;
		};

		PulledBlock PulledBlockOf(const std::string& a_name)
		{
			return a_name == "PerTechnique" ? PulledBlock{ "DCLFPipelineRowAddress", "kDCLFTechniqueBlock" } :
			       a_name == "PerMaterial"  ? PulledBlock{ "DCLFMaterialRowAddress", "kDCLFMaterialBlock" } :
			                                  PulledBlock{ "DCLFPipelineRowAddress", "kDCLFGeometryBlock" };
		}

		const char* PulledRowLoad(const std::string& a_type)
		{
			return a_type == "float4"   ? "DCLFRowFloat4" :
			       a_type == "float3"   ? "DCLFRowFloat3" :
			       a_type == "float2"   ? "DCLFRowFloat2" :
			       a_type == "float"    ? "DCLFRowFloat" :
			       a_type == "uint"     ? "DCLFRowUint" :
			       a_type == "int"      ? "(int)DCLFRowUint" :
			       a_type == "float3x3" ? "DCLFRowFloat3x3" :
			       a_type == "float3x4" ? "DCLFRowFloat3x4" :
			                              "DCLFRowFloat4x4";
		}

		// PulledLightingSource's output, and the block it is in.
		struct PulledRewrite
		{
			std::ostringstream out;
			std::string blockName;
			PulledBlock block{};
			bool inBlock = false, opened = false;
			std::ostringstream assignments;  // the block's DCLFPer<Block>Statics body, its #if lines with it
			std::uint32_t textures = 0, samplers = 0, members = 0, lazy = 0;
			const Options* options = nullptr;
		};

		bool MaterialTexture(const Options& a_options, std::size_t a_register)
		{
			return a_register < a_options.materialTexture.size() && a_options.materialTexture[a_register];
		}

		// A block member: a static, and its assignment from the row; false when it is not one the rows can hold.
		bool PulledMember(const std::smatch& a_match, PulledRewrite& a_rewrite)
		{
			auto& out = a_rewrite.out;
			auto& assignments = a_rewrite.assignments;
			const auto& block = a_rewrite.block;
			const std::string type = a_match[3].str(), name = a_match[4].str();
			const bool matrix = type.find('x') != std::string::npos;
			// A column-major matrix's registers are its columns, and an array of matrices is none the shader has.
			if (matrix && (!a_match[2].matched || a_match[5].matched))
				return false;
			const std::uint32_t component = a_match[7].matched ? static_cast<std::uint32_t>(std::string("xyzw").find(a_match[7].str()[0])) : 0u;
			const std::uint32_t offset = static_cast<std::uint32_t>(std::stoul(a_match[6].str())) * 16 + component * 4;
			const auto source = [&](std::uint32_t a_offset) { return std::format("{}({}, {} + {})", PulledRowLoad(type), block.row, block.offset, a_offset); };
			// A member nothing writes or names as a field: a load where it is read (CS_DCLF_LAZY_ROWS), so the stage holds no register for it
			// between its entry and its use. Statics loaded at entry live through the whole stage; a large block (Landscape's) spills.
			if (a_rewrite.options->eager && !a_match[5].matched && !a_rewrite.options->eager->contains(name)) {
				out << a_match[1].str() << "#define " << name << " (" << source(offset) << ")" << a_match[8].str() << "\n";
				++a_rewrite.members;
				++a_rewrite.lazy;
				return true;
			}
			out << a_match[1].str() << "static " << a_match[2].str() << type << " " << name << (a_match[5].matched ? "[" + a_match[5].str() + "]" : std::string{}) << ";" << a_match[8].str()
				<< "\n";
			if (a_match[5].matched) {
				// An array's elements: a register each.
				const auto count = static_cast<std::uint32_t>(std::stoul(a_match[5].str()));
				for (std::uint32_t e = 0; e < count; ++e)
					assignments << "\t" << name << "[" << e << "] = " << source(offset + 16 * e) << ";\n";
			} else {
				assignments << "\t" << name << " = " << source(offset) << ";\n";
			}
			++a_rewrite.members;
			return true;
		}

		// A line inside a row's cbuffer; false when it is not one it knows.
		bool PulledBlockLine(const std::string& a_line, PulledRewrite& a_rewrite)
		{
			static const std::regex member(
				R"(^(\s*)(row_major\s+)?(float|float2|float3|float4|uint|int|float3x3|float3x4|float4x4)\s+(\w+)\s*(?:\[\s*(\d+)\s*\])?\s*:\s*packoffset\(\s*c(\d+)(?:\.([xyzw]))?\s*\)\s*;(.*)$)");
			auto& out = a_rewrite.out;
			std::smatch m;
			const auto first = a_line.find_first_not_of(" \t");
			const std::string trimmed = first == std::string::npos ? std::string{} : a_line.substr(first);
			if (!a_rewrite.opened && trimmed == "{") {
				a_rewrite.opened = true;
				out << "\n";
			} else if (trimmed.rfind("};", 0) == 0 || trimmed == "}") {  // Effect.hlsl closes a block without the semicolon
				// The block's members as statics, assigned from the row by the pulled entry points (Lighting.hlsl,
				// DCLFRowStatics) once the row's address is known: a member's name is one other code reuses (a struct
				// field), so not a macro.
				a_rewrite.inBlock = false;
				out << "void DCLF" << a_rewrite.blockName << "Statics()\n{\n" << a_rewrite.assignments.str() << "}\n";
			} else if (trimmed.empty() || trimmed[0] == '#' || trimmed.rfind("//", 0) == 0) {
				out << a_line << "\n";
				if (!trimmed.empty() && trimmed[0] == '#')
					a_rewrite.assignments << a_line << "\n";
			} else if (std::regex_match(a_line, m, member)) {
				return PulledMember(m, a_rewrite);
			} else {
				return false;
			}
			return true;
		}

		// A material (or shadow mask) texture: a function reading it at the row's index, behind a macro of its name.
		void PulledTexture(const std::smatch& a_match, PulledRewrite& a_rewrite)
		{
			const std::string type = a_match[2].str(), name = a_match[3].str(), slot = a_match[4].str();
			const std::string index = std::stoul(slot) == a_rewrite.options->shadowMaskSlot ? std::string("DCLFPipelineTextureIndex()") : "DCLFMaterialTextureIndex(" + slot + ")";
			a_rewrite.out << a_match[1].str() << type << " DCLFRow_" << name << "() { " << type << " r = ResourceDescriptorHeap[" << index << "]; return r; }\n#define " << name
				<< " DCLFRow_" << name << "()" << a_match[5].str() << "\n";
			++a_rewrite.textures;
		}

		// A sampler (s0-s15) likewise.
		void PulledSampler(const std::smatch& a_match, PulledRewrite& a_rewrite)
		{
			const std::string type = a_match[2].str(), name = a_match[3].str(), slot = a_match[4].str();
			const std::string index = std::stoul(slot) == a_rewrite.options->shadowMaskSlot ? std::string("DCLFPipelineSamplerIndex()") : "DCLFMaterialSamplerIndex(" + slot + ")";
			a_rewrite.out << a_match[1].str() << type << " DCLFRow_" << name << "() { " << type << " r = SamplerDescriptorHeap[" << index << "]; return r; }\n#define " << name
				<< " DCLFRow_" << name << "()" << a_match[5].str() << "\n";
			++a_rewrite.samplers;
		}

	}

	/**
	 * @brief The names a block member may not be a macro of: every identifier the shader sources name as a field (".name") or
	 * assign or declare ("name =", "name +=", "name[i] ="), so a macro would rewrite a struct member, a local or a write.
	 * Read from every shader file (a_files: RenderGraphRuntime::ShaderSourceFiles) with each sources generation.
	 */
	std::unordered_set<std::string> EagerRowNames(const std::vector<std::filesystem::path>& a_files)
	{
		const auto names = [&] {
			std::unordered_set<std::string> out;
			const auto word = [](char a_c) { return std::isalnum(static_cast<unsigned char>(a_c)) || a_c == '_'; };
			for (const auto& path : a_files) {
				std::ifstream file(path, std::ios::binary);
				const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
				for (std::size_t i = 0; i < text.size(); ++i) {
					if (text[i] == '.' && i + 1 < text.size() && (std::isalpha(static_cast<unsigned char>(text[i + 1])) || text[i + 1] == '_')) {
						std::size_t end = i + 1;
						while (end < text.size() && word(text[end]))
							++end;
						out.emplace(text.substr(i + 1, end - i - 1));
					} else if (text[i] == '=' && (i + 1 >= text.size() || text[i + 1] != '=') && i > 0 && text[i - 1] != '=' && text[i - 1] != '!' &&
							   text[i - 1] != '<' && text[i - 1] != '>') {
						std::size_t end = i;
						if (std::strchr("+-*/|&^%", text[end - 1]))
							--end;
						while (end > 0 && std::isspace(static_cast<unsigned char>(text[end - 1])))
							--end;
						if (end > 0 && text[end - 1] == ']') {  // an element's write
							while (end > 0 && text[end - 1] != '[')
								--end;
							if (end > 0)
								--end;
						}
						std::size_t begin = end;
						while (begin > 0 && word(text[begin - 1]))
							--begin;
						if (begin < end)
							out.emplace(text.substr(begin, end - begin));
					}
				}
			}
			return out;
		}();
		return names;
	}

	/**
	 * @brief Lighting.hlsl for a pulled build (DCLF_PULLED): its rows' inputs, which a device-generated draw's layout maps from
	 * the rows' addresses in its push data (MainPipelines.cpp, the main layout), read from the rows its sequence names instead
	 * (Common/DCLFObjects.hlsli, DCLFMaterialRowAddress and DCLFPipelineRowAddress). A plain draw call binds once for draws
	 * of many pipeline slots (the Z-prepass's buckets are its depth pipelines'), so nothing it binds may be a row's:
	 * - the material row's: the PerMaterial blocks (b1), the material textures (t0-t15 but t14, and the features' t71, t74
	 *   and t80-t91) and their samplers (s0-s15 but s14);
	 * - the pipeline row's: the PerTechnique blocks (b0), the PerGeometry template (b2) and the shadow mask (t14, s14); its
	 *   permutation block (b4) is Permutation.hlsli's (DCLF_PULLED_ROWS).
	 * A block's members become statics, which its function (DCLFPer<Block>Statics) assigns from the row; a texture or sampler
	 * a function of its own, behind a macro of its name. Line by line, so each declaration's replacement stays in its #if
	 * branch. An error, no source, when a block's line is not one it knows.
	 */
	Result Rewrite(std::string_view a_source, const Options& a_options)
	{
		const std::string text(a_source);
		static const std::regex texture(R"(^(\s*)((?:Texture2D|TextureCube|Texture2DArray|Texture3D)(?:\s*<[^>]*>)?)\s+(\w+)\s*:\s*register\(\s*t(\d+)\s*\)\s*;(.*)$)");
		static const std::regex sampler(R"(^(\s*)(SamplerState|SamplerComparisonState)\s+(\w+)\s*:\s*register\(\s*s(\d+)\s*\)\s*;(.*)$)");
		static const std::regex cbuffer(R"(^\s*cbuffer\s+(PerTechnique|PerMaterial|PerGeometry)\s*:\s*register\(\s*b([012])\s*\).*$)");
		std::istringstream in(text);
		PulledRewrite rewrite;
		rewrite.options = &a_options;
		auto& out = rewrite.out;
		std::string line;
		auto fail = [&](const std::string& a_line) {
			Result failed;
			failed.error = std::format("{} line not understood: {}", rewrite.blockName, a_line);
			return failed;
		};
		while (std::getline(in, line)) {
			if (!line.empty() && line.back() == '\r')
				line.pop_back();
			std::smatch m;
			if (rewrite.inBlock) {
				if (!PulledBlockLine(line, rewrite))
					return fail(line);
				continue;
			}
			if (std::regex_match(line, m, cbuffer)) {
				rewrite.inBlock = true;
				rewrite.opened = false;
				rewrite.blockName = m[1].str();
				rewrite.block = PulledBlockOf(rewrite.blockName);
				rewrite.assignments.str({});
				out << "// " << rewrite.blockName << ": the draw's " << (rewrite.blockName == "PerMaterial" ? "material" : "pipeline") << " row's (DCLF_PULLED)\n";
			} else if (std::regex_match(line, m, texture) &&
					   (MaterialTexture(a_options, std::stoul(m[4].str())) || std::stoul(m[4].str()) == a_options.shadowMaskSlot)) {
				PulledTexture(m, rewrite);
			} else if (std::regex_match(line, m, sampler) && std::stoul(m[4].str()) < a_options.samplerSlots) {  // s0-s15
				PulledSampler(m, rewrite);
			} else {
				out << line << "\n";
			}
		}
		Result result;
		result.source = out.str();
		result.textures = rewrite.textures;
		result.samplers = rewrite.samplers;
		result.members = rewrite.members;
		result.lazy = rewrite.lazy;
		return result;
	}}
