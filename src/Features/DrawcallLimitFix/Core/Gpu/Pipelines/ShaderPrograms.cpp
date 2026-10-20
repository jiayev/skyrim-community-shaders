// ShaderPrograms: the stage cache, the compile requests and the pipeline lane's entries (see the header).

#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/ShaderPrograms.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iterator>
#include <mutex>
#include <regex>
#include <span>
#include <sstream>
#include <string_view>
#include <tuple>
#include <unordered_set>

#include <Tracy/Tracy.hpp>

#include "Features/DrawcallLimitFix/Core/Gpu/BuildExecutor.h"
#include <ORGModuleServices/PipelineRegistry.h>
#include "Features/DrawcallLimitFix/Core/Extensions/DrawFamilies.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Materials.h"
#include "RenderGraph/RenderGraphRuntime.h"
#include "ShaderCache.h"
#include "Features/DrawcallLimitFix/Core/Runtime/EventQueue.h"
#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"
#include "Features/DrawcallLimitFix/Core/Shading/ConstantEvaluator.h"

#include <ORGModuleServices/ShaderCompiler.h>
#include "Features/DrawcallLimitFix/Diagnostics/Observation.h"
#include "Features/DrawcallLimitFix/Diagnostics/Policy.h"

namespace DCLF
{
	namespace
	{
		/** @brief A program entry waiting for stages (StageCache::waiting): its kind and its key in that kind's map. */
		enum class StageUser : std::uint8_t
		{
			Lighting,
			Shadow,
			Forward,
			// FindDrawFamily and FindDrawFamilyForward: the first draw family's (Api::DrawFamily 0); a family's is DrawFamily (or
			// DrawFamilyForward) + its index.
			DrawFamily,
			DrawFamilyForward = DrawFamily + kDrawFamilyLanes,
			Family = DrawFamilyForward + kDrawFamilyLanes  // FindFamily: the first family's (Api::ProgramFamily 0); a family's is Family + its index
		};

		StageUser FamilyUser(Api::ProgramFamily a_family) { return static_cast<StageUser>(static_cast<std::uint8_t>(StageUser::Family) + static_cast<std::uint8_t>(a_family)); }
		std::size_t FamilyIndexOf(StageUser a_user) { return static_cast<std::size_t>(a_user) - static_cast<std::size_t>(StageUser::Family); }
		StageUser DrawFamilyUser(Api::DrawFamily a_family, bool a_forward)
		{
			return static_cast<StageUser>(static_cast<std::uint8_t>(a_forward ? StageUser::DrawFamilyForward : StageUser::DrawFamily) + static_cast<std::uint8_t>(a_family));
		}
		// Whether a_user is a draw family's (its programs' or its forward program's), and which.
		bool IsDrawFamilyUser(StageUser a_user) { return a_user >= StageUser::DrawFamily && a_user < StageUser::Family; }
		bool IsDrawFamilyForward(StageUser a_user) { return a_user >= StageUser::DrawFamilyForward && a_user < StageUser::Family; }
		std::size_t DrawFamilyIndexOf(StageUser a_user)
		{
			return static_cast<std::size_t>(a_user) - static_cast<std::size_t>(IsDrawFamilyForward(a_user) ? StageUser::DrawFamilyForward : StageUser::DrawFamily);
		}
	}

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

		// The SPIR-V is built with source-level debug info by default (CS_DCLF_SHADER_DEBUG=0: without), so a GPU crash dump names the
		// shader (Aftermath; and for Nsight and RenderDoc; the source is embedded, and SnapshotSources' copy is there for editing), still
		// optimized. Part of the compile key, so the debug builds are cached beside the release ones. There is deliberately no -Od form:
		// unoptimized code reads per-frame constant buffers (VS b6, PS b7) that the epochs do not supply, so every candidate is skipped
		// and DCLF draws nothing.
		bool ShaderDebug()
		{
			return SwitchValue(Switch::ShaderDebug) != "0";
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

		bool PulledMaterialTexture(std::uint32_t a_register)
		{
			return (a_register < kPixelTextureSlots && a_register != kShadowMaskSlot) || Extensions::FeatureTextureSlotOf(a_register) >= 0;
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
			const std::unordered_set<std::string>* eager = nullptr;  // members kept as statics (EagerRowNames); none when null
		};

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
			const auto source = [&](std::uint32_t a_offset) { return fmt::format("{}({}, {} + {})", PulledRowLoad(type), block.row, block.offset, a_offset); };
			// A member nothing writes or names as a field: a load where it is read (CS_DCLF_LAZY_ROWS), so the stage holds no register for it
			// between its entry and its use. Statics loaded at entry live through the whole stage; a large block (Landscape's) spills.
			if (a_rewrite.eager && !a_match[5].matched && !a_rewrite.eager->contains(name)) {
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
			const std::string index = std::stoul(slot) == kShadowMaskSlot ? std::string("DCLFPipelineTextureIndex()") : "DCLFMaterialTextureIndex(" + slot + ")";
			a_rewrite.out << a_match[1].str() << type << " DCLFRow_" << name << "() { " << type << " r = ResourceDescriptorHeap[" << index << "]; return r; }\n#define " << name
				<< " DCLFRow_" << name << "()" << a_match[5].str() << "\n";
			++a_rewrite.textures;
		}

		// A sampler (s0-s15) likewise.
		void PulledSampler(const std::smatch& a_match, PulledRewrite& a_rewrite)
		{
			const std::string type = a_match[2].str(), name = a_match[3].str(), slot = a_match[4].str();
			const std::string index = std::stoul(slot) == kShadowMaskSlot ? std::string("DCLFPipelineSamplerIndex()") : "DCLFMaterialSamplerIndex(" + slot + ")";
			a_rewrite.out << a_match[1].str() << type << " DCLFRow_" << name << "() { " << type << " r = SamplerDescriptorHeap[" << index << "]; return r; }\n#define " << name
				<< " DCLFRow_" << name << "()" << a_match[5].str() << "\n";
			++a_rewrite.samplers;
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
		 * branch. Empty, logged, when a block's line is not one it knows.
		 */
		std::vector<std::byte> PulledLightingSource(std::span<const std::byte> a_source, const std::unordered_set<std::string>* a_eager)
		{
			const std::string text(reinterpret_cast<const char*>(a_source.data()), a_source.size());
			static const std::regex texture(R"(^(\s*)((?:Texture2D|TextureCube|Texture2DArray|Texture3D)(?:\s*<[^>]*>)?)\s+(\w+)\s*:\s*register\(\s*t(\d+)\s*\)\s*;(.*)$)");
			static const std::regex sampler(R"(^(\s*)(SamplerState|SamplerComparisonState)\s+(\w+)\s*:\s*register\(\s*s(\d+)\s*\)\s*;(.*)$)");
			static const std::regex cbuffer(R"(^\s*cbuffer\s+(PerTechnique|PerMaterial|PerGeometry)\s*:\s*register\(\s*b([012])\s*\).*$)");
			std::istringstream in(text);
			PulledRewrite rewrite;
			rewrite.eager = a_eager;  // CS_DCLF_LAZY_ROWS: the names kept as statics; null: every member is
			auto& out = rewrite.out;
			std::string line;
			auto fail = [&](const std::string& a_line) {
				logger::error("[DCLF] Pulled Lighting source: {} line not understood: {}", rewrite.blockName, a_line);
				return std::vector<std::byte>{};
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
						   (PulledMaterialTexture(static_cast<std::uint32_t>(std::stoul(m[4].str()))) || std::stoul(m[4].str()) == kShadowMaskSlot)) {
					PulledTexture(m, rewrite);
				} else if (std::regex_match(line, m, sampler) && std::stoul(m[4].str()) < 16) {  // s0-s15
					PulledSampler(m, rewrite);
				} else {
					out << line << "\n";
				}
			}
			const std::string result = out.str();
			logger::info("[DCLF] Pulled Lighting source: {} texture, {} sampler and {} block member declarations read from the draw's rows ({} where used)",
				rewrite.textures, rewrite.samplers, rewrite.members, rewrite.lazy);
			std::vector<std::byte> bytes(result.size());
			std::memcpy(bytes.data(), result.data(), result.size());
			return bytes;
		}
	}

	namespace
	{
		/**
		 * @brief One stage's compile (StageCache): its artifact once the compiler's completion callback ran, which then pushes the
		 * stage's key to StageCompletions. Kept for the process, as the programs are.
		 */
		struct Stage
		{
			std::uint64_t key = 0;
			std::shared_ptr<const org::services::ShaderArtifact> artifact;  // written once, before done
			std::atomic<bool> done{ false };
			// A DCLF_PULLED_VERTICES vertex stage's attributes (VertexAttributesOf), written before the stage is shared; 0 otherwise.
			std::uint32_t vertexAttributes = 0;
			std::uint64_t sourcesGeneration = 0;  // the sources it is compiled from (ShaderPrograms::Sources)
		};
		using StagePtr = std::shared_ptr<Stage>;

		/**
		 * @brief The stages whose compile completed, by key: pushed by the completion callbacks (any thread), drained by the one
		 * consumer, the pipeline lane (UpdateLane).
		 */
		EventQueue<std::uint64_t>& StageCompletions()
		{
			// Never destroyed: a compile may complete while the process tears down.
			static auto* queue = new EventQueue<std::uint64_t>;
			return *queue;
		}

		struct StageWaiter
		{
			StageUser user = StageUser::Lighting;
			std::uint64_t id = 0;
		};
	}

	template <class P, std::size_t N>
	struct ShaderPrograms::ProgramEntry
	{
		std::array<StagePtr, N> stages;
		std::uint8_t remaining = 0;  // stages not completed when requested (StageCache::waiting)
		std::shared_ptr<P> program;  // shared with the builds made from it (Find* hand it out const)
		bool failed = false;
		std::uint8_t onDemand = 0;  // kOnDemand*, by stage index: the stages no precompile had asked for
		std::chrono::steady_clock::time_point requested;
	};

	std::string ShaderPrograms::OnDemandStages(std::uint8_t a_stages)
	{
		std::string text;
		for (const auto& [bit, name] : { std::pair{ kOnDemandVertex, "VS" }, std::pair{ kOnDemandPixel, "PS" }, std::pair{ kOnDemandDepthPixel, "depth PS" },
				 std::pair{ kOnDemandPulledVertex, "pulled VS" }, std::pair{ kOnDemandPulledDepthPixel, "pulled depth PS" }, std::pair{ kOnDemandPulledPixel, "pulled PS" },
				 std::pair{ kOnDemandPulledDepthVertex, "pulled depth VS" } })
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

	std::uint64_t ShaderPrograms::CurrentSourcesGeneration()
	{
		const auto* registry = RenderGraphRuntime::PipelineRegistry();
		return registry ? registry->SourcesGeneration() : 0;
	}

	namespace
	{
		std::vector<std::byte> ReadShaderFile(const std::filesystem::path& a_path)
		{
			std::ifstream file(a_path, std::ios::binary);
			const std::vector<char> chars((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
			std::vector<std::byte> bytes(chars.size());
			if (!chars.empty())
				std::memcpy(bytes.data(), chars.data(), chars.size());
			return bytes;
		}
	}

	/**
	 * @brief The shader files of one sources generation, as every stage compiled for it reads them: Lighting.hlsl and its pulled form,
	 * Utility.hlsl, the draw families' and program families' sources (and the latter's pulled forms), every shader file as a dependency,
	 * and each source's ShaderCompiler::FingerprintInputs with them, taken once here: every request's key starts from it instead of
	 * hashing the source and checking the dependency files again (thousands of keys a launch). Immutable once made.
	 */
	struct ShaderPrograms::Sources
	{
		explicit Sources(std::uint64_t a_generation);

		std::uint64_t generation = 0;
		bool missing = false;  // Lighting.hlsl could not be read: no SPIR-V builds of this generation
		std::vector<std::byte> source;        // Lighting.hlsl
		std::vector<std::byte> pulledSource;  // its pulled builds' (PulledLightingSource); empty when it could not be made
		std::vector<std::byte> utilitySource;
		// By draw family: its source (Extensions::DrawFamilies' sourcePath: Data/Shaders/DistantTree.hlsl; empty when missing or not taken).
		std::array<std::vector<std::byte>, kDrawFamilyLanes> drawFamilySources;
		// By program family: its source (Extensions::ProgramFamilies' sourcePath: Data/Shaders/Effect.hlsl) and its pulled form.
		std::array<std::vector<std::byte>, Extensions::kProgramFamilies> familySources, familyPulledSources;
		std::vector<std::filesystem::path> dependencies;
		std::uint64_t sourceFingerprint = 0, pulledFingerprint = 0, utilityFingerprint = 0;
		std::array<std::uint64_t, Extensions::kProgramFamilies> familyFingerprints{}, familyPulledFingerprints{};
		std::array<std::uint64_t, kDrawFamilyLanes> drawFamilyFingerprints{};

		/** @brief Where a stage compiles from: its source, the source's fingerprint and the logs' name of it. */
		struct Resolved
		{
			std::span<const std::byte> source;
			std::uint64_t fingerprint = 0;
			const char* name = nullptr;
			bool pulled = false;  // its pulled build (StageKeyOf's)
			bool family = false;  // a program family's
		};
		/** @brief a_stage of a_shader's source in this generation; false when that source is missing. */
		bool Resolve(const RE::BSShader& a_shader, const ShaderStageSpec& a_stage, Resolved& a_out) const;
	};

	ShaderPrograms::Sources::Sources(std::uint64_t a_generation) :
		generation(a_generation)
	{
		source = ReadShaderFile(kSourcePath);
		if (source.empty()) {
			logger::error("[DCLF] {} is missing; no SPIR-V builds", kSourcePath);
			missing = true;
			return;
		}
		// Every shader file is a potential include; the compiler re-hashes one only when it changes.
		dependencies = RenderGraphRuntime::ShaderSourceFiles();
		static const bool lazyRows = SwitchOnUnlessZero(Switch::LazyRows);
		const auto eager = lazyRows ? EagerRowNames(dependencies) : std::unordered_set<std::string>{};
		pulledSource = PulledLightingSource(source, lazyRows ? &eager : nullptr);
		utilitySource = ReadShaderFile(kUtilitySourcePath);
		if (utilitySource.empty())
			logger::warn("[DCLF] {} is missing; the shadow views stay native", kUtilitySourcePath);
		// The draw families' sources (Extensions::DrawFamilies: tree LOD's DistantTree.hlsl), in family order, before the program families'.
		for (std::size_t f = 0; f < kDrawFamilyLanes; ++f) {
			const auto& family = Extensions::DrawFamilies()[f];
			if (!family.taken)
				continue;
			drawFamilySources[f] = ReadShaderFile(family.sourcePath);
			if (drawFamilySources[f].empty())
				logger::warn("[DCLF] {} is missing; {}", family.sourcePath, family.nativeNote);
		}
		// The program families' sources (Extensions::ProgramFamilies: the Effect port's Effect.hlsl), in family order.
		for (std::size_t f = 0; f < Extensions::kProgramFamilies; ++f) {
			const auto& family = Extensions::ProgramFamilies()[f];
			if (!family.taken)
				continue;
			familySources[f] = ReadShaderFile(family.sourcePath);
			if (!familySources[f].empty())
				// Its plain draws' builds (DCLF_PULLED: the forward epoch's), its rows' declarations read from the draw's rows as Lighting's are.
				familyPulledSources[f] = PulledLightingSource(familySources[f], lazyRows ? &eager : nullptr);
			else
				logger::warn("[DCLF] {} is missing; {}", family.sourcePath, family.nativeNote);
		}
		if (auto* compiler = RenderGraphRuntime::Get().ShaderCompiler()) {
			sourceFingerprint = compiler->FingerprintInputs(source, dependencies);
			for (std::size_t f = 0; f < Extensions::kProgramFamilies; ++f) {
				familyFingerprints[f] = familySources[f].empty() ? 0 : compiler->FingerprintInputs(familySources[f], dependencies);
				familyPulledFingerprints[f] = familyPulledSources[f].empty() ? 0 : compiler->FingerprintInputs(familyPulledSources[f], dependencies);
			}
			pulledFingerprint = pulledSource.empty() ? 0 : compiler->FingerprintInputs(pulledSource, dependencies);
			utilityFingerprint = utilitySource.empty() ? 0 : compiler->FingerprintInputs(utilitySource, dependencies);
			for (std::size_t f = 0; f < kDrawFamilyLanes; ++f)
				drawFamilyFingerprints[f] = drawFamilySources[f].empty() ? 0 : compiler->FingerprintInputs(drawFamilySources[f], dependencies);
		}
		logger::info("[DCLF] SPIR-V builds of {}: {} shader files tracked as dependencies (sources generation {})", kSourcePath, dependencies.size(), generation);
		if (ShaderDebug())
			SnapshotSources(dependencies);
	}

	namespace
	{
		// The logs' name of a source: Lighting, Utility, a program family's (Extensions::ProgramFamilies' name: Effect) or a draw family's
		// (Extensions::DrawFamilies' name: DistantTree), the families' by their owned sourcePath (Request names a source by it).
		const char* SourceLogName(const char* a_sourceName)
		{
			if (a_sourceName == kSourcePath)
				return "Lighting";
			if (a_sourceName == kUtilitySourcePath)
				return "Utility";
			for (const auto& family : Extensions::ProgramFamilies())
				if (family.taken && family.sourcePath.c_str() == a_sourceName)
					return family.name.c_str();
			for (const auto& family : Extensions::DrawFamilies())
				if (family.taken && family.sourcePath.c_str() == a_sourceName)
					return family.name.c_str();
			return a_sourceName;
		}

		// A family's stages' bit in the stage cache's key (its slot's), 0 for none.
		std::uint64_t StageKeyBitOf(Api::ProgramFamily a_family)
		{
			return a_family == Api::ProgramFamily::Count ? 0ull : ProgramFamilySlotOf(a_family).stageKeyBit;
		}
		// A draw family's stages' bit in the stage cache's key (its lane slot's), 0 for none.
		std::uint64_t DrawStageKeyBitOf(Api::DrawFamily a_family)
		{
			return a_family == Api::DrawFamily::Count ? 0ull : DrawFamilyLaneSlotOf(a_family).stageKeyBit;
		}

		/*
		 * The attributes (BSGraphics::Vertex::Attribute, a bit each: VertexInput.h, VertexLayoutAttributes) a DCLF_PULLED_VERTICES vertex
		 * stage reads, from its defines: Lighting.hlsl's (a_lighting) or Effect.hlsl's (the one program family) DCLFPulledInput. Must match
		 * those reads: kept by hand beside them, as the input layout the engine builds from the shader's inputs was (DrawPipelines'
		 * check, MainPipelines.cpp). Lighting.hlsl defines VC itself under TREE_ANIM or LANDSCAPE.
		 */
		std::uint32_t VertexAttributesOf(std::span<const org::services::ShaderDefine> a_defines, bool a_lighting)
		{
			constexpr std::uint32_t kPosition = 1u << 0, kTexcoord0 = 1u << 1, kNormal = 1u << 3, kBinormal = 1u << 4, kColor = 1u << 5,
									kSkinning = 1u << 6, kLandData = 1u << 7, kEyeData = 1u << 8;
			auto has = [&](std::wstring_view a_name) { return std::ranges::any_of(a_defines, [&](const org::services::ShaderDefine& a_define) { return a_define.name == a_name; }); };
			std::uint32_t attributes = 0;
			if (a_lighting) {
				attributes = kPosition | kTexcoord0;
				if (!has(L"MODELSPACENORMALS"))
					attributes |= kNormal | kBinormal;
				if (has(L"VC") || has(L"TREE_ANIM") || has(L"LANDSCAPE"))
					attributes |= kColor | (has(L"LANDSCAPE") ? kLandData : 0u);
				if (has(L"SKINNED"))
					attributes |= kSkinning;
				if (has(L"EYE"))
					attributes |= kEyeData;
				return attributes;
			}
			// A particle system's quads read no stream (DCLF_PARTICLE_PULL: PARTICLES under DCLF_BINDLESS, which every DCLF build has).
			if (has(L"PARTICLES"))
				return 0;
			attributes = kPosition;
			if (has(L"TEXCOORD"))
				attributes |= kTexcoord0;
			if (has(L"NORMALS") || has(L"MOTIONVECTORS_NORMALS"))
				attributes |= kNormal;
			if (has(L"BINORMAL_TANGENT"))
				attributes |= kBinormal;
			if (has(L"VC"))
				attributes |= kColor;
			if (has(L"SKINNED"))
				attributes |= kSkinning;
			return attributes;
		}

		/**
		 * @brief A stage's compile request (the lane's stage cache and the registry's rebuilds both: one request, one compiler key), and
		 * into a_vertexAttributes a DCLF_PULLED_VERTICES vertex stage's attributes (VertexAttributesOf; 0 for any other).
		 */
		org::services::ShaderCompileRequest MakeStageRequest(std::span<const std::byte> a_source, const std::vector<std::filesystem::path>& a_dependencies,
			std::uint64_t a_fingerprint, const RE::BSShader& a_lighting, bool a_pixel, std::uint32_t a_descriptor, bool a_depthOnly, bool a_pulled, bool a_family,
			const char* a_sourceName, std::uint32_t& a_vertexAttributes)
		{
			a_vertexAttributes = 0;
			org::services::ShaderCompileRequest request{};
			request.inputsFingerprint = a_fingerprint;
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
			// The shadow views' Utility stages and the Z-prepass's Lighting ones draw with plain indirect draws (ShadowViewPass,
			// MainOpaquePass) and fetch their own indices, vertices and per-draw words (Utility.hlsl, Lighting.hlsl).
			if (a_pulled)
				request.defines.push_back({ L"DCLF_PULLED", L"1" });
			// A pulled Lighting stage reads its pipeline row's permutation block itself (Permutation.hlsli), as it does the rows'
			// other inputs (PulledLightingSource).
			if (a_pulled && (a_sourceName == kSourcePath || a_family))
				request.defines.push_back({ L"DCLF_PULLED_ROWS", L"1" });
			// The device-generated draws' vertex stages (Program::vertex, both variants'; a program family's) pull their attributes
			// through the geometry's buffers in the draw's push data (Lighting.hlsl, Effect.hlsl): no DCLF pipeline has input assembly,
			// whose vertex input state an execution set's pipelines would have to share. The stage cache's key needs no bit for it:
			// every such stage has it, and the compiler's cache key hashes the defines.
			if (!a_pixel && !a_pulled && (a_sourceName == kSourcePath || a_family)) {
				request.defines.push_back({ L"DCLF_PULLED_VERTICES", L"1" });
				a_vertexAttributes = VertexAttributesOf(request.defines, a_sourceName == kSourcePath);
			}
			// The foliage record (Policy::RecordFoliagePixels): the colour pass's alpha-tested draws write what each pixel shows, and the
			// Z-prepass's which of them owns its depth (Lighting.hlsl); every colour draw records with CS_DCLF_FOLIAGE_PARITY=land.
			if (a_pixel && a_depthOnly == a_pulled && a_sourceName == kSourcePath && Policy::RecordFoliagePixels()) {
				request.defines.push_back({ L"DCLF_FOLIAGE_PARITY", L"1" });
				if (Diagnostics::Observe().foliageLand)
					request.defines.push_back({ L"DCLF_FOLIAGE_PARITY_ALL", L"1" });
			}
			// The first few define sets, to reproduce builds with the DXC command line.
			static std::atomic<std::uint32_t> logged = 0;
			if (logged.fetch_add(1) < 4) {
				std::string text;
				for (const auto& define : request.defines)
					text += fmt::format(" -D {}{}{}", Util::WStringToString(define.name), define.value.empty() ? "" : "=", Util::WStringToString(define.value));
				logger::info("[DCLF] SPIR-V build of {} {}{} {:08X} defines:{}", SourceLogName(a_sourceName), a_pixel ? (a_depthOnly ? "PS (depth)" : "PS") : "VS",
					a_pulled ? " (pulled)" : "", a_descriptor, text);
			}
			const auto shift = [&](const wchar_t* a_flag, std::uint32_t a_value) {
				request.arguments.insert(request.arguments.end(), { a_flag, std::to_wstring(a_value), L"0" });
			};
			shift(L"-fvk-b-shift", kBindingShiftB);
			shift(L"-fvk-t-shift", kBindingShiftT);
			shift(L"-fvk-s-shift", kBindingShiftS);
			shift(L"-fvk-u-shift", kBindingShiftU);
			return request;
		}

		// Requests a_stage's compile; the completion callback fills it and pushes its key to StageCompletions.
		void RequestStage(org::services::ShaderCompiler& a_compiler, const StagePtr& a_stage, org::services::ShaderCompileRequest a_request)
		{
			// The future is not kept: the stage holds the artifact, and nothing waits on the compile but Precompile (on the stage).
			(void)a_compiler.CompileAsync(std::move(a_request), [a_stage](const org::services::ShaderArtifact& a_artifact) {
				a_stage->artifact = std::make_shared<const org::services::ShaderArtifact>(a_artifact);
				a_stage->done.store(true, std::memory_order_release);
				a_stage->done.notify_all();
				// The consumer told, and woken (a stage two kinds share is waited on by both: UpdateLane tells each).
				StageCompletions().Push(a_stage->key);
				WakePipelineLane();
			});
		}

		/**
		 * @brief A stage's key in the stage cache (its descriptor, stage, depth-only, Utility and pulled bits, its draw family's or program
		 * family's bit) and into a_pulled whether it is a pulled build: a Utility stage always is, a draw family's as its lane slot says, a
		 * program family's only when asked.
		 */
		std::uint64_t StageKeyOf(const RE::BSShader& a_shader, const ShaderStageSpec& a_stage, bool& a_pulled)
		{
			const bool utility = a_shader.shaderType.get() == RE::BSShader::Type::Utility;
			const auto drawFamily = Extensions::DrawFamilyOfShader(static_cast<std::uint8_t>(a_shader.shaderType.get()));
			const auto family = Extensions::FamilyOfShader(static_cast<std::uint8_t>(a_shader.shaderType.get()));
			a_pulled = family != Api::ProgramFamily::Count ? a_stage.pulled :
			                                                 (a_stage.pulled || utility || (drawFamily != Api::DrawFamily::Count && DrawFamilyLaneSlotOf(drawFamily).pulled));
			return a_stage.descriptor | (std::uint64_t(a_stage.pixel) << 32) | (std::uint64_t(a_stage.depth) << 33) | (std::uint64_t(utility) << 34) |
			       (std::uint64_t(a_pulled) << 35) | DrawStageKeyBitOf(drawFamily) | StageKeyBitOf(family);
		}

		/** @brief A program's fingerprint: its stages' binaries hashed in their order (the lane's entries and the rebuilds alike). */
		std::uint64_t MixBinary(std::uint64_t a_seed, const std::vector<std::byte>& a_binary)
		{
			return ankerl::unordered_dense::detail::wyhash::mix(a_seed ^ ankerl::unordered_dense::detail::wyhash::hash(a_binary.data(), a_binary.size()),
				0x9E3779B97F4A7C15ull);
		}

		// The programs' stages, in their entries' order (StageCache::RequestAll, CompileProgram): each kind's list, used by the lane's Find*
		// and the registry's Rebuild* alike, so both make the same requests and a program's fingerprint is the same either way.
		std::array<ShaderStageSpec, 7> LightingStages(const PipelineKey& a_key)
		{
			const std::uint32_t v = a_key.vertexDescriptor, p = a_key.pixelDescriptor;
			// vertex, pixel, depth pixel, pulled vertex, pulled depth pixel, pulled pixel, pulled depth vertex (DCLF_PULLED and
			// DCLF_DEPTH_ONLY: a Z-prepass pipeline without a pixel stage).
			return { ShaderStageSpec{ false, v }, ShaderStageSpec{ true, p }, ShaderStageSpec{ true, p, true }, ShaderStageSpec{ false, v, false, true },
				ShaderStageSpec{ true, p, true, true }, ShaderStageSpec{ true, p, false, true }, ShaderStageSpec{ false, v, true, true } };
		}
		std::array<ShaderStageSpec, 6> FamilyStages(const PipelineKey& a_key)
		{
			// Program's order: vertex, pixel, (depth pixel: the pixel again), pulled vertex, (pulled depth pixel: the pulled pixel again),
			// pulled pixel. A stage listed twice is waited for once each (Await), harmlessly.
			const std::uint32_t v = a_key.vertexDescriptor, p = a_key.pixelDescriptor;
			return { ShaderStageSpec{ false, v }, ShaderStageSpec{ true, p }, ShaderStageSpec{ true, p }, ShaderStageSpec{ false, v, false, true },
				ShaderStageSpec{ true, p, false, true }, ShaderStageSpec{ true, p, false, true } };
		}
		std::array<ShaderStageSpec, 2> ShadowStages(std::uint32_t a_technique)
		{
			// Both stages take the same technique: Utility's descriptor is the technique itself, not a pair of vertex and pixel descriptors
			// as the Lighting shader's is.
			return { ShaderStageSpec{ false, a_technique }, ShaderStageSpec{ true, a_technique } };
		}
		std::array<ShaderStageSpec, 4> DrawFamilyStages(Api::DrawFamily a_family)
		{
			const auto& descriptors = Extensions::DrawFamilyOf(a_family).contribution.program.descriptors;
			return { ShaderStageSpec{ false, descriptors.colour }, ShaderStageSpec{ true, descriptors.colour }, ShaderStageSpec{ false, descriptors.depth },
				ShaderStageSpec{ true, descriptors.depth } };
		}
		std::array<ShaderStageSpec, 2> ForwardStages(std::uint32_t a_vertexDescriptor, std::uint32_t a_pixelDescriptor)
		{
			// The pulled vertex stage is the Z-prepass's (the stage cache's key is the descriptor's); the pixel stage the colour one.
			return { ShaderStageSpec{ false, a_vertexDescriptor, false, true }, ShaderStageSpec{ true, a_pixelDescriptor, false, true } };
		}
		std::array<ShaderStageSpec, 2> DrawFamilyForwardStages(Api::DrawFamily a_family)
		{
			const std::uint32_t forward = Extensions::DrawFamilyOf(a_family).contribution.program.descriptors.forward;
			return { ShaderStageSpec{ false, forward }, ShaderStageSpec{ true, forward } };
		}
	}

	std::string ShaderPrograms::SourcePathOf(const RE::BSShader& a_shader)
	{
		if (a_shader.shaderType.get() == RE::BSShader::Type::Utility)
			return kUtilitySourcePath;
		if (const auto drawFamily = Extensions::DrawFamilyOfShader(static_cast<std::uint8_t>(a_shader.shaderType.get())); drawFamily != Api::DrawFamily::Count)
			return Extensions::DrawFamilyOf(drawFamily).sourcePath;
		if (const auto family = Extensions::FamilyOfShader(static_cast<std::uint8_t>(a_shader.shaderType.get())); family != Api::ProgramFamily::Count)
			return Extensions::ProgramFamilyOf(family).sourcePath;
		return kSourcePath;
	}

	bool ShaderPrograms::Sources::Resolve(const RE::BSShader& a_shader, const ShaderStageSpec& a_stage, Resolved& a_out) const
	{
		const bool utility = a_shader.shaderType.get() == RE::BSShader::Type::Utility;
		const auto drawFamily = Extensions::DrawFamilyOfShader(static_cast<std::uint8_t>(a_shader.shaderType.get()));
		const bool drawBuild = drawFamily != Api::DrawFamily::Count;
		const auto d = static_cast<std::size_t>(drawFamily);
		const auto family = Extensions::FamilyOfShader(static_cast<std::uint8_t>(a_shader.shaderType.get()));
		const bool familyBuild = family != Api::ProgramFamily::Count;
		const auto f = static_cast<std::size_t>(family);
		bool pulled = false;
		(void)StageKeyOf(a_shader, a_stage, pulled);
		if (missing || (utility && utilitySource.empty()) || (drawBuild && drawFamilySources[d].empty()) ||
			(familyBuild && (pulled ? familyPulledSources[f] : familySources[f]).empty()) || (pulled && !utility && !drawBuild && !familyBuild && pulledSource.empty()))
			return false;
		a_out.source = utility ? utilitySource : drawBuild ? drawFamilySources[d] : familyBuild ? (pulled ? familyPulledSources[f] : familySources[f]) :
		               pulled  ? pulledSource :
		                         source;
		a_out.fingerprint = utility ? utilityFingerprint : drawBuild ? drawFamilyFingerprints[d] :
		                    familyBuild                      ? (pulled ? familyPulledFingerprints[f] : familyFingerprints[f]) :
		                    pulled                           ? pulledFingerprint :
		                                                       sourceFingerprint;
		a_out.name = utility ? kUtilitySourcePath : drawBuild ? Extensions::DrawFamilyOf(drawFamily).sourcePath.c_str() :
		             familyBuild                  ? Extensions::ProgramFamilyOf(family).sourcePath.c_str() :
		                                            kSourcePath;
		a_out.pulled = pulled;
		a_out.family = familyBuild;
		return true;
	}

	struct ShaderPrograms::StageCache
	{
		std::mutex mutex;
		std::mutex requestMutex;
		bool started = false;
		struct Pending { const RE::BSShader* shader; bool pixel; std::uint32_t descriptor; };
		std::vector<Pending> pending;
		ankerl::unordered_dense::map<std::uint64_t, StagePtr> stages;
		// The compiler Configure pointed at DCLF's build executor (requestMutex): the runtime's, which a restart replaces.
		org::services::ShaderCompiler* configured = nullptr;
		// The consumer's (the pipeline lane's): the program entries waiting for each stage, by stage key, and the entries whose
		// stages have all completed, for UpdateLane to admit.
		ankerl::unordered_dense::map<std::uint64_t, std::vector<StageWaiter>> waiting;
		std::vector<StageWaiter> ready;

		// a_created: set when this call made the request (no earlier request, precompile or runtime, had).
		// pulled: a Lighting stage's pulled build (Program::pulledVertex, pulledDepthPixel); a Utility stage's always is.
		// Null when the stage's source is missing. A stage is of one sources generation: one of an earlier is not found.
		StagePtr Request(ShaderPrograms& owner, const RE::BSShader& shader, bool pixel, std::uint32_t descriptor, bool depth = false, bool* a_created = nullptr,
			bool pulled = false)
		{
			const ShaderStageSpec spec{ pixel, descriptor, depth, pulled };
			bool effectivePulled = false;
			const std::uint64_t key = StageKeyOf(shader, spec, effectivePulled);
			const std::uint64_t generation = CurrentSourcesGeneration();
			{
				std::lock_guard lock(mutex);
				if (const auto found = stages.find(key); found != stages.end() && found->second->sourcesGeneration == generation)
					return found->second;
			}
			// Cache hits must not wait behind another permutation's filesystem key construction.
			std::lock_guard requestLock(requestMutex);
			const auto current = owner.SourcesLocked();
			{
				std::lock_guard lock(mutex);
				if (const auto found = stages.find(key); found != stages.end() && found->second->sourcesGeneration == current->generation)
					return found->second;
			}
			auto* compiler = RenderGraphRuntime::Get().ShaderCompiler();
			Sources::Resolved resolved;
			if (!compiler || !current->Resolve(shader, spec, resolved))
				return {};
			if (configured != compiler) {
				// The compiles run on DCLF's preparation pool (BuildExecutor.h), not a thread each. Every DCLF request takes
				// requestMutex, so none is submitted before this.
				compiler->Configure(BuildSubmitter());
				configured = compiler;
			}
			// Like the program entries, stages live for this source set's lifetime.
			auto stage = std::make_shared<Stage>();
			stage->key = key;
			stage->sourcesGeneration = current->generation;
			RequestStage(*compiler, stage,
				MakeStageRequest(resolved.source, current->dependencies, resolved.fingerprint, shader, pixel, descriptor, depth, resolved.pulled, resolved.family, resolved.name,
					stage->vertexAttributes));
			{
				std::lock_guard lock(mutex);
				stages.insert_or_assign(key, stage);
			}
			if (a_created)
				*a_created = true;
			return stage;
		}

		/** @brief Request for each of a_stages (a program's, in its entry's order), a_created[i] set as Request's. */
		template <std::size_t N>
		std::array<StagePtr, N> RequestAll(ShaderPrograms& owner, const RE::BSShader& shader, const std::array<ShaderStageSpec, N>& a_stages, bool* a_created)
		{
			std::array<StagePtr, N> out;
			for (std::size_t i = 0; i < N; ++i)
				out[i] = Request(owner, shader, a_stages[i].pixel, a_stages[i].descriptor, a_stages[i].depth, a_created ? &a_created[i] : nullptr, a_stages[i].pulled);
			return out;
		}

		/**
		 * @brief The pipeline lane: registers a new entry's stages, counting in a_remaining those not completed yet. Ready at once (all
		 * completed) goes straight to ready. False when a stage could not be requested (a missing source): the entry fails. A stage's
		 * done is set before its key is pushed, so a stage read as not done has a completion still to drain.
		 */
		template <std::size_t N>
		bool Await(StageUser a_user, std::uint64_t a_id, const std::array<StagePtr, N>& a_stages, std::uint8_t& a_remaining)
		{
			for (const auto& stage : a_stages)
				if (!stage)
					return false;
			for (const auto& stage : a_stages) {
				if (stage->done.load(std::memory_order_acquire))
					continue;
				waiting[stage->key].push_back({ a_user, a_id });
				++a_remaining;
			}
			if (!a_remaining)
				ready.push_back({ a_user, a_id });
			return true;
		}
	};

	ShaderPrograms::ShaderPrograms() : stages(std::make_unique<StageCache>()) {}
	ShaderPrograms::~ShaderPrograms() = default;

	std::shared_ptr<const ShaderPrograms::Sources> ShaderPrograms::SourcesLocked()
	{
		const std::uint64_t generation = CurrentSourcesGeneration();
		if (!sources || sources->generation != generation) {
			sources = std::make_shared<const Sources>(generation);
			sourcesMissing.store(sources->missing, std::memory_order_relaxed);
			// The stages of the files before: no request finds them again (each stays with the entries and builds that hold it).
			std::lock_guard lock(stages->mutex);
			stages->stages.clear();
		}
		return sources;
	}

	std::shared_ptr<const ShaderPrograms::Sources> ShaderPrograms::CurrentSources()
	{
		std::lock_guard lock(stages->requestMutex);
		return SourcesLocked();
	}

	void ShaderPrograms::Precompile(const RE::BSShader& a_shader, bool a_pixel, std::uint32_t a_descriptor)
	{
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
		const bool lighting = type == RE::BSShader::Type::Lighting;
		auto depth = a_pixel && lighting ? stages->Request(*this, a_shader, true, a_descriptor, true) : StagePtr{};
		// The Z-prepass's pulled builds: the vertex stage, and the depth pixel stage; the plain colour draws' pixel stage; and the depth-only
		// vertex stage of a Z-prepass pipeline without a pixel stage (DCLF_DEPTH_ONLY: Program::pulledDepthVertex).
		auto pulled = lighting ? stages->Request(*this, a_shader, a_pixel, a_descriptor, a_pixel, nullptr, true) : StagePtr{};
		auto pulledColour = lighting && a_pixel ? stages->Request(*this, a_shader, true, a_descriptor, false, nullptr, true) : StagePtr{};
		auto pulledDepthVertex = lighting && !a_pixel ? stages->Request(*this, a_shader, false, a_descriptor, true, nullptr, true) : StagePtr{};
		// A shadow technique's canonical form, which the shadow views' merged pipelines draw with (CanonicalShadowTechnique).
		static const bool shadowMerge = SwitchOnUnlessZero(Switch::ShadowMerge);
		const std::uint32_t canonical = !lighting && shadowMerge ? CanonicalShadowTechnique(a_descriptor) : a_descriptor;
		auto merged = canonical != a_descriptor ? stages->Request(*this, a_shader, a_pixel, canonical) : StagePtr{};
		// Stay inside CS's bounded compilation workers until this task is done, including cache hits. CS's workers, not
		// DCLF's: the compiles run on DCLF's pool, which never waits on these.
		for (const auto* requested : { &stage, &depth, &pulled, &pulledColour, &pulledDepthVertex, &merged }) {
			if (!*requested) continue;
			(*requested)->done.wait(false, std::memory_order_acquire);
			const auto& artifact = *(*requested)->artifact;
			if (!artifact) {
				static std::atomic<std::uint32_t> failures{ 0 };
				if (failures.fetch_add(1) < kMaxLoggedFailures)
					logger::warn("[DCLF] ORG precompile {} {} {:08X} failed: {}", type == RE::BSShader::Type::Lighting ? "Lighting" : "Utility",
						a_pixel ? "PS" : "VS", a_descriptor, artifact.diagnostics.substr(0, 1500));
			}
		}
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

	bool ShaderPrograms::FollowSources()
	{
		const std::uint64_t generation = CurrentSourcesGeneration();
		if (generation == entriesGeneration)
			return false;
		entriesGeneration = generation;
		// Every entry of the files before, and the stages it waited for: a program held by a build or a pipeline stays with it. A stage
		// still compiling completes into its own object; no entry waits for it (FanOut).
		const std::size_t dropped = entries.size() + shadowEntries.size() + forwardEntries.size();
		stages->waiting.clear();
		stages->ready.clear();
		entries.clear();
		shadowEntries.clear();
		forwardEntries.clear();
		for (auto& family : familyEntries)
			family.clear();
		for (auto& entry : drawFamilyEntries)
			entry.reset();
		for (auto& entry : drawFamilyForwardEntries)
			entry.reset();
		loggedFailures = 0;
		logger::info("[DCLF] shader programs: sources generation {}; {} program entries of the files before dropped, made again as they are asked for", generation, dropped);
		return true;
	}

	template <class E, class MakeStages>
	E& ShaderPrograms::MakeEntry(std::unique_ptr<E>& a_slot, [[maybe_unused]] std::uint8_t a_user, [[maybe_unused]] std::uint64_t a_id, std::atomic<std::uint32_t>* a_requested,
		[[maybe_unused]] std::atomic<std::uint32_t>& a_failed, [[maybe_unused]] MakeStages&& a_makeStages)
	{
		a_slot = std::make_unique<E>();
		auto& entry = *a_slot;
		std::array<bool, std::tuple_size_v<decltype(entry.stages)>> created{};
		entry.stages = a_makeStages(created.data());
		if (!stages->Await(static_cast<StageUser>(a_user), a_id, entry.stages, entry.remaining)) {
			entry.failed = true;
			a_failed.fetch_add(1, std::memory_order_relaxed);
		}
		// After a shader reload (FollowSources) every stage is requested here, none by a precompile: no on-demand warning then.
		for (std::size_t i = 0; i < created.size() && !entriesGeneration; ++i)
			entry.onDemand |= created[i] ? static_cast<std::uint8_t>(1u << i) : std::uint8_t(0);
		entry.requested = std::chrono::steady_clock::now();
		if (a_requested)
			a_requested->fetch_add(1, std::memory_order_relaxed);
		return entry;
	}

	namespace
	{
		/** @brief An entry's program, and whether it failed for good (a Find's a_failed). */
		template <class E>
		auto Found(const E* a_entry, bool* a_failed) -> std::shared_ptr<const typename decltype(E::program)::element_type>
		{
			if (a_failed)
				*a_failed = a_entry && a_entry->failed;
			return a_entry ? a_entry->program : nullptr;
		}
	}

	std::shared_ptr<const ShaderPrograms::Program> ShaderPrograms::Find(const PipelineKey& a_key, RE::BSShader& a_lighting, std::uint8_t* a_onDemand, bool* a_failed)
	{
		if (a_onDemand)
			*a_onDemand = 0;
		if (!Enabled())
			return Found<Entry>(nullptr, a_failed);
		const std::uint64_t id = LightingProgramId(a_key);
		auto& slot = entries[id];
		if (!slot) {
			MakeEntry(slot, static_cast<std::uint8_t>(StageUser::Lighting), id, &laneStats.requested, laneStats.failed,
				[&](bool* a_created) { return stages->RequestAll(*this, a_lighting, LightingStages(a_key), a_created); });
			if (a_onDemand)
				*a_onDemand = slot->onDemand;
		}
		return Found(slot.get(), a_failed);
	}

	std::shared_ptr<const ShaderPrograms::Program> ShaderPrograms::FindFamily(Api::ProgramFamily a_family, const PipelineKey& a_key, const RE::BSShader& a_shader, bool* a_failed)
	{
		if (!Enabled())
			return Found<FamilyEntry>(nullptr, a_failed);
		const std::uint64_t id = FamilyProgramId(a_family, a_key);
		auto& slot = familyEntries[static_cast<std::size_t>(a_family)][id];
		if (!slot)
			MakeEntry(slot, static_cast<std::uint8_t>(FamilyUser(a_family)), id, &laneStats.requested, laneStats.failed,
				[&](bool*) { return stages->RequestAll(*this, a_shader, FamilyStages(a_key), nullptr); });
		return Found(slot.get(), a_failed);
	}

	std::shared_ptr<const ShaderPrograms::ShadowProgram> ShaderPrograms::FindShadow(std::uint32_t a_technique, RE::BSShader& a_utility, bool a_allowRequest, bool* a_requested,
		std::uint8_t* a_onDemand, bool* a_failed)
	{
		if (a_requested)
			*a_requested = false;
		if (a_onDemand)
			*a_onDemand = 0;
		if (!Enabled() || (!a_allowRequest && !shadowEntries.contains(a_technique)))
			return Found<ShadowEntry>(nullptr, a_failed);
		auto& slot = shadowEntries[a_technique];
		if (!slot) {
			MakeEntry(slot, static_cast<std::uint8_t>(StageUser::Shadow), a_technique, &laneStats.shadowRequested, laneStats.shadowFailed,
				[&](bool* a_created) { return stages->RequestAll(*this, a_utility, ShadowStages(a_technique), a_created); });
			if (a_requested)
				*a_requested = true;
			if (a_onDemand)
				*a_onDemand = slot->onDemand;
		}
		return Found(slot.get(), a_failed);
	}

	std::shared_ptr<const ShaderPrograms::DrawProgram> ShaderPrograms::FindDrawFamily(Api::DrawFamily a_family, RE::BSShader& a_shader, bool* a_failed)
	{
		if (!Enabled())
			return Found<DrawFamilyEntry>(nullptr, a_failed);
		auto& slot = drawFamilyEntries[static_cast<std::size_t>(a_family)];
		if (!slot)
			MakeEntry(slot, static_cast<std::uint8_t>(DrawFamilyUser(a_family, false)), 0, nullptr, laneStats.failed,
				[&](bool*) { return stages->RequestAll(*this, a_shader, DrawFamilyStages(a_family), nullptr); });
		return Found(slot.get(), a_failed);
	}

	std::shared_ptr<const ShaderPrograms::ForwardProgram> ShaderPrograms::FindForward(std::uint32_t a_vertexDescriptor, std::uint32_t a_pixelDescriptor, RE::BSShader& a_lighting,
		bool* a_failed)
	{
		if (!Enabled())
			return Found<ForwardEntry>(nullptr, a_failed);
		const std::uint64_t id = ForwardProgramId(a_vertexDescriptor, a_pixelDescriptor);
		auto& slot = forwardEntries[id];
		if (!slot)
			MakeEntry(slot, static_cast<std::uint8_t>(StageUser::Forward), id, nullptr, laneStats.failed,
				[&](bool*) { return stages->RequestAll(*this, a_lighting, ForwardStages(a_vertexDescriptor, a_pixelDescriptor), nullptr); });
		return Found(slot.get(), a_failed);
	}

	std::shared_ptr<const ShaderPrograms::ForwardProgram> ShaderPrograms::FindDrawFamilyForward(Api::DrawFamily a_family, RE::BSShader& a_shader, bool* a_failed)
	{
		if (!Enabled())
			return Found<ForwardEntry>(nullptr, a_failed);
		auto& slot = drawFamilyForwardEntries[static_cast<std::size_t>(a_family)];
		if (!slot)
			MakeEntry(slot, static_cast<std::uint8_t>(DrawFamilyUser(a_family, true)), 0, nullptr, laneStats.failed,
				[&](bool*) { return stages->RequestAll(*this, a_shader, DrawFamilyForwardStages(a_family), nullptr); });
		return Found(slot.get(), a_failed);
	}

	namespace
	{
		const org::services::ShaderArtifact& ArtifactOf(const StagePtr& a_stage) { return *a_stage->artifact; }

		// The outcome of a stage no precompile had asked for (logged with its reason when it was requested): compiled, or
		// found in the disk cache, which a precompile run before would also have filled.
		void OnDemandOutcome(const char* a_what, std::uint8_t a_stages, std::chrono::steady_clock::time_point a_requested,
			std::initializer_list<std::pair<std::uint8_t, const org::services::ShaderArtifact*>> a_artifacts)
		{
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
				ShaderPrograms::OnDemandStages(compiled), ShaderPrograms::OnDemandStages(cached), ShaderPrograms::OnDemandStages(failed));
		}

		/**
		 * @brief The completed stages (the queue), fanned out to the entries waiting for them: an entry whose last stage completed is
		 * ready. a_arrive(waiter, key) counts the waiter's stage of that key when that stage (its own: one of an earlier sources generation
		 * may complete after the cache moved on) is done: -1 not yet (it stays waiting), 0 counted, 1 counted and it was the last.
		 */
		template <class Arrive>
		void FanOut(ankerl::unordered_dense::map<std::uint64_t, std::vector<StageWaiter>>& a_waiting, std::vector<StageWaiter>& a_ready, Arrive&& a_arrive)
		{
			StageCompletions().Drain([&](std::uint64_t&& a_key) {
				const auto found = a_waiting.find(a_key);
				if (found == a_waiting.end())
					return;  // no entry waits (a precompile's stage, or one found completed)
				std::erase_if(found->second, [&](const StageWaiter& a_waiter) {
					const int arrived = a_arrive(a_waiter, a_key);
					if (arrived == 1)
						a_ready.push_back(a_waiter);
					return arrived >= 0;
				});
				if (found->second.empty())
					a_waiting.erase(found);
			});
		}

		/** @brief A program's stages' fingerprint (MixBinary in their order) and the oldest sources generation among them. */
		template <std::size_t N>
		std::pair<std::uint64_t, std::uint64_t> FingerprintOf(const std::array<StagePtr, N>& a_stages)
		{
			std::uint64_t fingerprint = 0xcbf29ce484222325ull, generation = ~0ull;
			for (const auto& stage : a_stages) {
				fingerprint = MixBinary(fingerprint, stage->artifact->binary);
				generation = std::min(generation, stage->sourcesGeneration);
			}
			return { fingerprint, generation };
		}
	}

	template <class E, class LogFailure>
	void ShaderPrograms::FinishEntry(E& a_entry, std::atomic<std::uint32_t>* a_ready, std::atomic<std::uint32_t>& a_failed, LogFailure&& a_logFailure)
	{
		std::uint32_t cached = 0;
		for (std::size_t i = 0; i < a_entry.stages.size(); ++i) {
			const auto& artifact = ArtifactOf(a_entry.stages[i]);
			if (!artifact) {
				a_entry.failed = true;
				a_failed.fetch_add(1, std::memory_order_relaxed);
				a_logFailure(i, artifact);
				return;
			}
			cached += artifact.fromCache ? 1u : 0u;
		}
		[&]<std::size_t... I>(std::index_sequence<I...>) {
			a_entry.program = std::make_shared<std::remove_reference_t<decltype(*a_entry.program)>>(
				std::remove_reference_t<decltype(*a_entry.program)>{ ArtifactOf(a_entry.stages[I]).binary... });
		}(std::make_index_sequence<std::tuple_size_v<decltype(a_entry.stages)>>{});
		std::tie(a_entry.program->fingerprint, a_entry.program->sourcesGeneration) = FingerprintOf(a_entry.stages);
		if (a_ready)
			a_ready->fetch_add(1, std::memory_order_relaxed);
		laneStats.fromCache.fetch_add(cached, std::memory_order_relaxed);
	}

	void ShaderPrograms::FinishDrawFamily(std::uint8_t a_user, Finished& a_finished)
	{
		const auto user = static_cast<StageUser>(a_user);
		const auto f = DrawFamilyIndexOf(user);
		const auto& family = Extensions::DrawFamilies()[f];
		if (IsDrawFamilyForward(user)) {
			// A forward view's program of the family's (tree LOD's: DistantTree's forward descriptor).
			FinishEntry(*drawFamilyForwardEntries[f], nullptr, laneStats.failed, [&](std::size_t a_stage, const org::services::ShaderArtifact& a_artifact) {
				if (loggedFailures++ < kMaxLoggedFailures)
					logger::warn("[DCLF] SPIR-V build of the forward {} {} failed:\n{}", family.name, a_stage ? "PS" : "VS", a_artifact.diagnostics.substr(0, 1500));
			});
			a_finished.drawFamilyForward[f] = true;
			return;
		}
		// The family's programs: its four stages (tree LOD's: DistantTree's).
		auto& entry = *drawFamilyEntries[f];
		FinishEntry(entry, nullptr, laneStats.failed, [&](std::size_t a_stage, const org::services::ShaderArtifact& a_artifact) {
			static constexpr const char* kStages[] = { "VS", "PS", "VS (depth)", "PS (depth)" };
			logger::warn("[DCLF] SPIR-V build of {} {} failed; {}:\n{}", family.name, kStages[a_stage], family.nativeNote, a_artifact.diagnostics.substr(0, 1500));
		});
		if (entry.program)
			logger::info("[DCLF] {} programs ready ({}, pulled)", family.label, family.name);
		a_finished.drawFamily[f] = true;
	}

	void ShaderPrograms::UpdateLane(Finished& a_finished)
	{
		// The entry a waiter is: its kind's map, by its id.
		const auto visit = [&](const StageWaiter& a_waiter, auto&& a_visit) {
			switch (a_waiter.user) {
			case StageUser::Shadow: return a_visit(*shadowEntries.find(static_cast<std::uint32_t>(a_waiter.id))->second);
			case StageUser::Forward: return a_visit(*forwardEntries.find(a_waiter.id)->second);
			case StageUser::Lighting: return a_visit(*entries.find(a_waiter.id)->second);
			default: break;
			}
			if (IsDrawFamilyUser(a_waiter.user))
				return IsDrawFamilyForward(a_waiter.user) ? a_visit(*drawFamilyForwardEntries[DrawFamilyIndexOf(a_waiter.user)]) :
				                                            a_visit(*drawFamilyEntries[DrawFamilyIndexOf(a_waiter.user)]);
			return a_visit(*familyEntries[FamilyIndexOf(a_waiter.user)].find(a_waiter.id)->second);
		};
		FanOut(stages->waiting, stages->ready, [&](const StageWaiter& a_waiter, std::uint64_t a_key) {
			return visit(a_waiter, [&](auto& a_entry) -> int {
				const bool done = std::ranges::any_of(a_entry.stages, [&](const StagePtr& a_stage) {
					return a_stage->key == a_key && a_stage->done.load(std::memory_order_acquire);
				});
				if (!done)
					return -1;
				return --a_entry.remaining == 0 ? 1 : 0;
			});
		});
		for (const auto& waiter : std::exchange(stages->ready, {})) {
			const std::uint64_t id = waiter.id;
			const auto vertex = static_cast<std::uint32_t>(id >> 32), pixel = static_cast<std::uint32_t>(id);
			switch (waiter.user) {
			case StageUser::Shadow:
				{
					// The shadow views' programs: a Utility technique's two stages.
					const auto technique = static_cast<std::uint32_t>(id);
					auto& entry = *shadowEntries.find(technique)->second;
					OnDemandOutcome(fmt::format("Utility technique {:08X}", technique).c_str(), entry.onDemand, entry.requested,
						{ { kOnDemandVertex, &ArtifactOf(entry.stages[0]) }, { kOnDemandPixel, &ArtifactOf(entry.stages[1]) } });
					FinishEntry(entry, &laneStats.shadowReady, laneStats.shadowFailed, [&](std::size_t a_stage, const org::services::ShaderArtifact& a_artifact) {
						if (loggedFailures++ < kMaxLoggedFailures)
							logger::warn("[DCLF] SPIR-V build of Utility {} {:08X} failed:\n{}", a_stage ? "PS" : "VS", technique, a_artifact.diagnostics.substr(0, 1500));
					});
					a_finished.shadow.push_back(technique);
				}
				continue;
			case StageUser::Forward:
				// The forward views' programs: a Lighting pair (a draw family's: FinishDrawFamily).
				FinishEntry(*forwardEntries.find(id)->second, nullptr, laneStats.failed, [&](std::size_t a_stage, const org::services::ShaderArtifact& a_artifact) {
					if (loggedFailures++ < kMaxLoggedFailures)
						logger::warn("[DCLF] SPIR-V build of the forward {} {} failed:\n{}", fmt::format("Lighting VS {:08X} PS {:08X}", vertex, pixel), a_stage ? "PS" : "VS",
							a_artifact.diagnostics.substr(0, 1500));
				});
				a_finished.forward.push_back(id);
				continue;
			case StageUser::Lighting:
				break;
			default:
				if (IsDrawFamilyUser(waiter.user)) {
					FinishDrawFamily(static_cast<std::uint8_t>(waiter.user), a_finished);
					continue;
				}
				{
					// A program family's vertex and pixel stages (the Effect port's Effect.hlsl), a Program of those two alone; its id's
					// vertex part without the family's program-id bit.
					const auto f = FamilyIndexOf(waiter.user);
					const auto& family = Extensions::ProgramFamilies()[f];
					const auto familyVertex = static_cast<std::uint32_t>((id & ~kProgramFamilySlots[f].programIdBit) >> 32);
					auto& entry = *familyEntries[f].find(id)->second;
					FinishEntry(entry, &laneStats.ready, laneStats.failed, [&](std::size_t a_stage, const org::services::ShaderArtifact& a_artifact) {
						if (loggedFailures++ < kMaxLoggedFailures)
							logger::warn("[DCLF] SPIR-V build of {} {}{} {:08X} failed; its {}:\n{}", family.name, a_stage == 0 || a_stage == 3 ? "VS" : "PS",
								a_stage >= 3 ? " (pulled)" : "", a_stage == 0 || a_stage == 3 ? familyVertex : pixel,
								family.nativeNote, a_artifact.diagnostics.substr(0, 1500));
					});
					if (entry.program)
						entry.program->vertexAttributes = entry.stages[0]->vertexAttributes;
					a_finished.families[f].push_back(id);
				}
				continue;
			}
			auto& entry = *entries.find(id)->second;
			a_finished.lighting.push_back(id);
			OnDemandOutcome(fmt::format("Lighting VS {:08X} PS {:08X}", vertex, pixel).c_str(), entry.onDemand, entry.requested,
				{ { kOnDemandVertex, &ArtifactOf(entry.stages[0]) }, { kOnDemandPixel, &ArtifactOf(entry.stages[1]) }, { kOnDemandDepthPixel, &ArtifactOf(entry.stages[2]) },
					{ kOnDemandPulledVertex, &ArtifactOf(entry.stages[3]) }, { kOnDemandPulledDepthPixel, &ArtifactOf(entry.stages[4]) },
					{ kOnDemandPulledPixel, &ArtifactOf(entry.stages[5]) }, { kOnDemandPulledDepthVertex, &ArtifactOf(entry.stages[6]) } });
			FinishEntry(entry, &laneStats.ready, laneStats.failed, [&](std::size_t a_stage, const org::services::ShaderArtifact& a_artifact) {
				static constexpr const char* kStages[] = { "VS", "PS", "PS (depth)", "VS (pulled)", "PS (pulled depth)", "PS (pulled)", "VS (pulled depth)" };
				if (loggedFailures++ < kMaxLoggedFailures)
					logger::warn("[DCLF] SPIR-V build of Lighting {} {:08X} failed:\n{}", kStages[a_stage], a_stage == 0 || a_stage == 3 || a_stage == 6 ? vertex : pixel,
						a_artifact.diagnostics.substr(0, 1500));
			});
			// The vertex stage's attributes (its defines'): DrawPipelines checks the key's vertex layout has them.
			if (entry.program)
				entry.program->vertexAttributes = entry.stages[0]->vertexAttributes;
		}
	}

	template <class P, std::size_t N>
	void ShaderPrograms::CompileProgram(const RE::BSShader& a_shader, const std::array<ShaderStageSpec, N>& a_stages, Rebuilt<P> a_done)
	{
		auto* compiler = RenderGraphRuntime::Get().ShaderCompiler();
		const auto current = CurrentSources();
		if (!compiler || current->missing)
			return a_done(nullptr, compiler ? fmt::format("{} could not be read", kSourcePath) : std::string("no runtime shader compiler"));
		// Every stage's source first: one that is missing fails the program before anything compiles.
		std::array<Sources::Resolved, N> resolved;
		for (std::size_t i = 0; i < N; ++i)
			if (!current->Resolve(a_shader, a_stages[i], resolved[i]))
				return a_done(nullptr, fmt::format("the source of {} is missing", SourcePathOf(a_shader)));
		// Written by the compiles' completions, each its own stage; the last one to complete makes the program.
		struct Compiles
		{
			std::array<std::shared_ptr<const org::services::ShaderArtifact>, N> artifacts;
			std::array<std::uint32_t, N> attributes{};
			std::atomic<std::uint32_t> remaining{ static_cast<std::uint32_t>(N) };
			std::uint64_t generation = 0;
			Rebuilt<P> done;
		};
		auto compiles = std::make_shared<Compiles>();
		compiles->generation = current->generation;
		compiles->done = std::move(a_done);
		std::array<org::services::ShaderCompileRequest, N> requests;
		for (std::size_t i = 0; i < N; ++i) {
			const auto& stage = a_stages[i];
			requests[i] = MakeStageRequest(resolved[i].source, current->dependencies, resolved[i].fingerprint, a_shader, stage.pixel, stage.descriptor, stage.depth,
				resolved[i].pulled, resolved[i].family, resolved[i].name, compiles->attributes[i]);
		}
		for (std::size_t i = 0; i < N; ++i) {
			// The compiler copies the source (the request's span is current's, held until here). A memory-cache hit completes on this thread.
			(void)compiler->CompileAsync(std::move(requests[i]), [compiles, i](const org::services::ShaderArtifact& a_artifact) {
				compiles->artifacts[i] = std::make_shared<const org::services::ShaderArtifact>(a_artifact);
				if (compiles->remaining.fetch_sub(1, std::memory_order_acq_rel) != 1)
					return;
				for (std::size_t s = 0; s < N; ++s)
					if (!*compiles->artifacts[s])
						return compiles->done(nullptr, fmt::format("its stage {} failed to compile:\n{}", s, compiles->artifacts[s]->diagnostics.substr(0, 1500)));
				auto program = [&]<std::size_t... I>(std::index_sequence<I...>) {
					return std::make_shared<P>(P{ compiles->artifacts[I]->binary... });
				}(std::make_index_sequence<N>{});
				std::uint64_t fingerprint = 0xcbf29ce484222325ull;
				for (const auto& artifact : compiles->artifacts)
					fingerprint = MixBinary(fingerprint, artifact->binary);
				program->fingerprint = fingerprint;
				program->sourcesGeneration = compiles->generation;
				// The vertex stage's attributes, as UpdateLane sets them (a Lighting or program family program's first stage).
				if constexpr (std::is_same_v<P, Program>)
					program->vertexAttributes = compiles->attributes[0];
				compiles->done(std::move(program), {});
			});
		}
	}

	void ShaderPrograms::RebuildLighting(const PipelineKey& a_key, const RE::BSShader& a_lighting, Rebuilt<Program> a_done)
	{
		CompileProgram<Program>(a_lighting, LightingStages(a_key), std::move(a_done));
	}

	void ShaderPrograms::RebuildFamily(Api::ProgramFamily, const PipelineKey& a_key, const RE::BSShader& a_shader, Rebuilt<Program> a_done)
	{
		CompileProgram<Program>(a_shader, FamilyStages(a_key), std::move(a_done));
	}

	void ShaderPrograms::RebuildShadow(std::uint32_t a_technique, const RE::BSShader& a_utility, Rebuilt<ShadowProgram> a_done)
	{
		CompileProgram<ShadowProgram>(a_utility, ShadowStages(a_technique), std::move(a_done));
	}

	void ShaderPrograms::RebuildDrawFamily(Api::DrawFamily a_family, const RE::BSShader& a_shader, Rebuilt<DrawProgram> a_done)
	{
		CompileProgram<DrawProgram>(a_shader, DrawFamilyStages(a_family), std::move(a_done));
	}

	void ShaderPrograms::RebuildForward(std::uint32_t a_vertexDescriptor, std::uint32_t a_pixelDescriptor, const RE::BSShader& a_lighting, Rebuilt<ForwardProgram> a_done)
	{
		CompileProgram<ForwardProgram>(a_lighting, ForwardStages(a_vertexDescriptor, a_pixelDescriptor), std::move(a_done));
	}

	void ShaderPrograms::RebuildDrawFamilyForward(Api::DrawFamily a_family, const RE::BSShader& a_shader, Rebuilt<ForwardProgram> a_done)
	{
		CompileProgram<ForwardProgram>(a_shader, DrawFamilyForwardStages(a_family), std::move(a_done));
	}
}
