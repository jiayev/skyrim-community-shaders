// DrawPipelines: the render thread's requests and frame inputs, the pipeline lane (passes on the coordinator executor: programs,
// builds, set versions, the published catalog), and the indirect states the epochs bind.


// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "Features/DrawcallLimitFix/Core/Gpu/BuildExecutor.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelines.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelinesRhi.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/EngineStates.h"
#include "Features/DrawcallLimitFix/Core/Extensions/MaterialPorts.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ProgramFamilies.h"
#include "Features/DrawcallLimitFix/Core/Runtime/EventQueue.h"
#include "Features/DrawcallLimitFix/Core/Runtime/LatestSlot.h"
#include "Features/DrawcallLimitFix/Core/Scene/SceneWake.h"
#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"

#include "RenderGraph/RenderGraphRuntime.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/SpirvReflection.h"
#include "Features/DrawcallLimitFix/Scene/Ports/LightingConstants.h"
#include "Features/DrawcallLimitFix/Scene/Ports/EngineShaders.h"
#include "Features/DrawcallLimitFix/Scene/Ports/LightingDescriptors.h"
#include "Features/DrawcallLimitFix/Scene/Model/Lookups.h"
#include "Features/DrawcallLimitFix/Scene/Ports/VertexInput.h"
#include "ShaderCache.h"

#include <Tracy/Tracy.hpp>

#include <OpenRenderGraph/PersistentGraphHost.h>
#include <ORGModuleServices/PipelineService.h>
#include <rhi_helpers.h>

#include <algorithm>
#include <atomic>
#include <limits>
#include <stdexcept>
#include <string_view>
#include "Features/DrawcallLimitFix/Diagnostics/Observation.h"
#include "Features/DrawcallLimitFix/Diagnostics/Policy.h"

namespace DCLF
{
	namespace
	{
		/**
		 * @brief Whether a fragment program defeats early depth testing: it discards, writes depth or the sample mask, or writes memory.
		 * Such a pipeline is in the shadow views' discarding class (kShadowDiscards), drawn after the others.
		 */
		bool FragmentDefersDepth(std::span<const std::byte> a_spirv)
		{
			const std::size_t words = a_spirv.size() / 4;
			if (words < 5)
				return true;
			auto word = [&](std::size_t i) {
				std::uint32_t w;
				std::memcpy(&w, a_spirv.data() + i * 4, 4);
				return w;
			};
			constexpr std::uint32_t kOpDecorate = 71, kOpMemberDecorate = 72, kOpImageWrite = 99, kOpAtomicFirst = 227, kOpAtomicLast = 242,
									kOpKill = 252, kOpTerminateInvocation = 4416, kOpDemoteToHelperInvocation = 5380, kDecorationBuiltIn = 11,
									kBuiltInSampleMask = 20, kBuiltInFragDepth = 22;
			auto depthOrMask = [](std::uint32_t a_builtIn) { return a_builtIn == kBuiltInFragDepth || a_builtIn == kBuiltInSampleMask; };
			for (std::size_t i = 5; i < words;) {
				const std::uint32_t first = word(i);
				const std::uint32_t count = first >> 16, op = first & 0xFFFF;
				if (count == 0 || i + count > words)
					return true;
				if (op == kOpKill || op == kOpTerminateInvocation || op == kOpDemoteToHelperInvocation || op == kOpImageWrite ||
					(op >= kOpAtomicFirst && op <= kOpAtomicLast))
					return true;
				if (op == kOpDecorate && count >= 4 && word(i + 2) == kDecorationBuiltIn && depthOrMask(word(i + 3)))
					return true;
				if (op == kOpMemberDecorate && count >= 5 && word(i + 3) == kDecorationBuiltIn && depthOrMask(word(i + 4)))
					return true;
				i += count;
			}
			return false;
		}
		constexpr std::uint32_t kMaxLoggedFailures = 8;

		// Register classes as shifted by the SPIR-V builds (ShaderPrograms.h), set 0. DCLF_BINDLESS builds declare a cbuffer at the draw's
		// push binding for the object index; the address words beside it feed the layout's indirect ranges.
		constexpr std::uint32_t kRecordAddressBinding = kDrawPushBinding;
		// The texture registers the vertex stage may declare: t121 (shading) to t127 (records); t125 is pixel-only but in the run.
		constexpr std::uint32_t kObjectBufferBinding = kBindingShiftT + kObjectBufferRegister;
		constexpr std::uint32_t kVertexTextureBinding = kBindingShiftT + kShadingBufferRegister;
		constexpr std::uint32_t kVertexTextureCount = kObjectBufferRegister - kShadingBufferRegister + 1;
		constexpr std::uint32_t kDescriptorHeapBindings = 1000000;  // DXC's ResourceDescriptorHeap bindings (BasicRHI maps them)

		bool InRange(std::uint32_t a_binding, std::uint32_t a_first, std::uint32_t a_count)
		{
			return a_binding >= a_first && a_binding < a_first + a_count;
		}

		const char* KindName(SpirvReflection::BindingKind a_kind)
		{
			switch (a_kind) {
			case SpirvReflection::BindingKind::ConstantBuffer:
				return "constant buffer";
			case SpirvReflection::BindingKind::Texture:
				return "texture";
			case SpirvReflection::BindingKind::Sampler:
				return "sampler";
			case SpirvReflection::BindingKind::StorageBuffer:
				return "storage buffer";
			default:
				return "resource";
			}
		}

		// Every binding a stage declares must be one the layout maps for that stage.
		void CheckBindings(const SpirvReflection& a_module, bool a_pixel)
		{
			using Kind = SpirvReflection::BindingKind;
			for (const auto& binding : a_module.bindings) {
				if (binding.binding >= kDescriptorHeapBindings)
					continue;
				bool mapped = binding.set == 0;
				switch (binding.kind) {
				case Kind::ConstantBuffer:
					mapped = mapped && (InRange(binding.binding, kBindingShiftB, kConstantBufferRegisters) || binding.binding == kRecordAddressBinding);
					break;
				case Kind::Texture:
				case Kind::StorageBuffer:
					// The pixel stage maps the whole t range; the vertex stage only the object-buffer run.
					mapped = mapped && (a_pixel ? InRange(binding.binding, kBindingShiftT, kTextureRegisters) : InRange(binding.binding, kVertexTextureBinding, kVertexTextureCount));
					break;
				case Kind::Sampler:
					mapped = mapped && a_pixel && InRange(binding.binding, kBindingShiftS, kSamplerRegisters);
					break;
				default:
					mapped = false;
					break;
				}
				if (!mapped)
					throw std::runtime_error(fmt::format("{} {} at set {} binding {} is outside the DCLF layout", a_pixel ? "pixel" : "vertex", KindName(binding.kind), binding.set,
						binding.binding));
			}
		}

		// A pulled Lighting stage (DCLF_PULLED) reads its rows itself: it may declare none of a row's bindings (b0-b2, b4, material
		// textures and samplers, shadow mask), which the Z-prepass's layout leaves out.
		void CheckPulledBindings(const SpirvReflection& a_module, bool a_pixel)
		{
			CheckBindings(a_module, a_pixel);
			using Kind = SpirvReflection::BindingKind;
			for (const auto& binding : a_module.bindings) {
				if (binding.binding >= kDescriptorHeapBindings)
					continue;
				const std::uint32_t b = binding.binding - kBindingShiftB, t = binding.binding - kBindingShiftT;
				const bool row = (binding.kind == Kind::ConstantBuffer && (b <= 2 || b == 4)) ||
				                 ((binding.kind == Kind::Texture || binding.kind == Kind::StorageBuffer) && InRange(binding.binding, kBindingShiftT, kTextureRegisters) &&
									 (t < kPixelTextureSlots || FeatureMaterialSlot(t) >= 0)) ||
				                 binding.kind == Kind::Sampler;
				if (row)
					throw std::runtime_error(fmt::format("pulled {} {} at binding {} is a row's", a_pixel ? "pixel" : "vertex", KindName(binding.kind), binding.binding));
			}
		}

		/**
		 * @brief Identity of a main pipeline's Z-prepass pipeline (Built::zKey): its pulled stages' SPIR-V and depth state. Slots with
		 * equal keys share a depth pipeline, Z-prepass bucket and draw call (IndirectState::zGroups); rows and vertex layout are read per
		 * draw. No pixel stage unless it defers the depth test (FragmentDefersDepth): it only runs the alpha test.
		 */
		struct ZPipelineKey
		{
			std::uint64_t vertex = 0, pixel = 0;  // wyhash of the modules; pixel 0 without a pixel stage
			std::uint32_t vertexBytes = 0, pixelBytes = 0;
			std::uint32_t cull = 0, depthWrite = 0, depthFunc = 0;
			std::uint32_t depthBias = 0, depthBiasClamp = 0, slopeScaledDepthBias = 0;  // the floats' bits
			// 1 for terrain group 1 (RasterTerrainGroup), whose bucket has a near-block twin (PlanZBuckets); it holds no other slot.
			std::uint32_t terrainNear = 0, padding = 0;

			bool operator==(const ZPipelineKey&) const = default;
		};
		static_assert(std::has_unique_object_representations_v<ZPipelineKey>);
		struct ZPipelineKeyHash
		{
			using is_avalanching = void;
			std::uint64_t operator()(const ZPipelineKey& a_key) const noexcept { return ankerl::unordered_dense::detail::wyhash::hash(&a_key, sizeof(a_key)); }
		};

		void AddUsage(const SpirvReflection& a_module, bool a_pixel, RegisterUsage& a_usage)
		{
			using Kind = SpirvReflection::BindingKind;
			for (const auto& binding : a_module.bindings) {
				if (binding.binding >= kDescriptorHeapBindings)
					continue;
				if (binding.binding == kRecordAddressBinding)
					continue;  // push data, not a DrawBindings entry (and 1u << 190 is not a shift)
				switch (binding.kind) {
				case Kind::ConstantBuffer:
					(a_pixel ? a_usage.pixelConstants : a_usage.vertexConstants) |= 1u << (binding.binding - kBindingShiftB);
					break;
				case Kind::Texture:
				case Kind::StorageBuffer:
					{
						const std::uint32_t t = binding.binding - kBindingShiftT;
						a_usage.textures[t / 64] |= 1ull << (t % 64);
					}
					break;
				case Kind::Sampler:
					a_usage.samplers |= 1u << (binding.binding - kBindingShiftS);
					break;
				default:
					break;
				}
			}
		}

		// The Lighting variables by index (ShaderConstants::LightingVS, LightingPS), named as ShaderCache's GetVariableIndices matches them.
		constexpr std::string_view kLightingVSNames[] = { "World", "PreviousWorld", "EyePosition", "LandBlendParams", "TreeParams", "WindTimers", "TextureProj",
			"IndexScale", "WorldMapOverlayParameters", "LeftEyeCenter", "RightEyeCenter", "TexcoordOffset", "HighDetailRange", "FogParam", "FogNearColor", "FogFarColor",
			"Bones" };
		constexpr std::string_view kLightingPSNames[] = { "NumLightNumShadowLight", "PointLightPosition", "PointLightColor", "DirLightDirection", "DirLightColor",
			"DirectionalAmbient", "AmbientSpecularTintAndFresnelPower", "MaterialData", "EmitColor", "AlphaTestRef", "ShadowLightMaskSelect", "VPOSOffset",
			"ProjectedUVParams", "ProjectedUVParams2", "ProjectedUVParams3", "SplitDistance", "SSRParams", "WorldMapOverlayParametersPS", "AmbientColor", "FogColor",
			"ColourOutputClamp", "EnvmapData", "ParallaxOccData", "TintColor", "LODTexParams", "SpecularColor", "SparkleParams", "MultiLayerParallaxData",
			"LightingEffectParams", "IBLParams", "LandscapeTexture1to4IsSnow", "LandscapeTexture5to6IsSnow", "LandscapeTexture1to4IsSpecPower",
			"LandscapeTexture5to6IsSpecPower", "SnowRimLightParameters", "CharacterLightParams", "PBRFlags", "PBRParams1", "LandscapeTexture2PBRParams",
			"LandscapeTexture3PBRParams", "LandscapeTexture4PBRParams", "LandscapeTexture5PBRParams", "LandscapeTexture6PBRParams", "PBRParams2",
			"LandscapeTexture1GlintParameters", "LandscapeTexture2GlintParameters", "LandscapeTexture3GlintParameters", "LandscapeTexture4GlintParameters",
			"LandscapeTexture5GlintParameters", "LandscapeTexture6GlintParameters", "MaterialObjectRGBScale" };
		static_assert(std::size(kLightingVSNames) == kLightingVSVariables && std::size(kLightingPSNames) == kLightingPSVariables);

		std::span<const std::string_view> LightingNames(bool a_pixel)
		{
			return a_pixel ? std::span<const std::string_view>(kLightingPSNames) : std::span<const std::string_view>(kLightingVSNames);
		}

		/**
		 * @brief A key's variable names: its family's port space's (Extensions::MaterialSpaceOf: the Effect port's EffectConstants.h names
		 * for an effect key), else Lighting's (kind 0's space names none: these).
		 */
		std::span<const std::string_view> VariableNames(const PipelineKey& a_key, bool a_pixel)
		{
			const auto& space = Extensions::MaterialSpaceOf(Extensions::FamilyRules(a_key.rasterFlags).kind);
			const auto names = a_pixel ? space.ps.names : space.vs.names;
			return names.empty() ? LightingNames(a_pixel) : names;
		}

		/**
		 * @brief A stage's constant table from its modules, as ShaderCache's ReflectConstantBuffers: each variable's float offset in its
		 * PerTechnique/PerMaterial/PerGeometry block, 0 where absent. The modules are one permutation's builds of one stage, and
		 * Lighting.hlsl places every member with packoffset, so a variable has one offset. Pulled stages declare no blocks.
		 */
		std::vector<std::uint8_t> ConstantTableOf(std::initializer_list<const SpirvReflection*> a_modules, bool a_pixel, const PipelineKey& a_key)
		{
			static std::atomic<std::uint32_t> logged{ 0 };
			const auto names = VariableNames(a_key, a_pixel);
			std::vector<std::uint8_t> table(names.size(), 0);
			std::vector<std::uint8_t> found(names.size(), 0);
			for (const auto* module : a_modules) {
				for (const auto& member : module->blockMembers) {
					if (member.block != "PerTechnique" && member.block != "PerMaterial" && member.block != "PerGeometry")
						continue;
					const auto it = std::find(names.begin(), names.end(), member.name);
					if (it == names.end()) {
						if (logged.fetch_add(1, std::memory_order_relaxed) < kMaxLoggedFailures)
							logger::warn("[DCLF] Constant tables: {} {} member {} is no Lighting variable (VS {:08X} PS {:08X}); left out", a_pixel ? "PS" : "VS",
								member.block, member.name, a_key.vertexDescriptor, a_key.pixelDescriptor);
						continue;
					}
					const auto variable = static_cast<std::size_t>(it - names.begin());
					const std::uint32_t offset = member.offset / 4;
					if (offset > std::numeric_limits<std::uint8_t>::max() || (found[variable] && table[variable] != offset)) {
						if (logged.fetch_add(1, std::memory_order_relaxed) < kMaxLoggedFailures)
							logger::error("[DCLF] Constant tables: {} {} at float {} ({}) cannot be a table entry (VS {:08X} PS {:08X})", a_pixel ? "PS" : "VS", member.name,
								offset, found[variable] ? fmt::format("another stage has it at {}", table[variable]) : std::string("beyond 255"), a_key.vertexDescriptor,
								a_key.pixelDescriptor);
						continue;
					}
					table[variable] = static_cast<std::uint8_t>(offset);
					found[variable] = 1;
				}
			}
			return table;
		}

		bool SameSemantic(std::string_view a_left, std::string_view a_right)
		{
			return a_left.size() == a_right.size() && std::equal(a_left.begin(), a_left.end(), a_right.begin(), [](char a, char b) { return std::toupper(static_cast<unsigned char>(a)) == std::toupper(static_cast<unsigned char>(b)); });
		}

		// The vertex shader's input mask, as ShaderCache derives BSGraphics::VertexShader::vertexDesc.
		std::uint64_t InputMask(const SpirvReflection& a_vertex)
		{
			std::uint64_t mask = 0b1111;
			bool texcoord2 = false, texcoord3 = false;
			for (const auto& input : a_vertex.inputs) {
				const auto& name = input.semantic;
				const auto index = input.semanticIndex;
				if (SameSemantic(name, "POSITION") && index == 0)
					AddVertexAttribute(mask, 0);
				else if (SameSemantic(name, "TEXCOORD") && index <= 1)
					AddVertexAttribute(mask, 1 + index);
				else if (SameSemantic(name, "NORMAL") && index == 0)
					AddVertexAttribute(mask, 3);
				else if (SameSemantic(name, "BINORMAL") && index == 0)
					AddVertexAttribute(mask, 4);
				else if (SameSemantic(name, "COLOR") && index == 0)
					AddVertexAttribute(mask, 5);
				else if (SameSemantic(name, "BLENDWEIGHT") && index == 0)
					AddVertexAttribute(mask, 6);
				else if (SameSemantic(name, "TEXCOORD") && index >= 4 && index <= 7)
					AddVertexAttribute(mask, 9);
				else if (SameSemantic(name, "TEXCOORD") && index == 2)
					texcoord2 = true;
				else if (SameSemantic(name, "TEXCOORD") && index == 3)
					texcoord3 = true;
			}
			if (texcoord2)
				AddVertexAttribute(mask, texcoord3 ? 7 : 8);
			return mask;
		}

		rhi::FinalizedInputLayout BuildInputLayout(const SpirvReflection& a_vertex, std::uint64_t a_vertexLayout)
		{
			const auto elements = BuildVertexElements(InputMask(a_vertex) & a_vertexLayout);
			rhi::FinalizedInputLayout layout;
			// The stride is dynamic (the VertexBuffer argument).
			layout.bindings.push_back(rhi::InputBindingDesc{ 0, 16, rhi::InputRate::PerVertex, 1 });
			bool secondStream = false;
			for (const auto& input : a_vertex.inputs) {
				const VertexElement* element = nullptr;
				for (std::uint32_t i = 0; i < elements.count && !element; ++i) {
					if (SameSemantic(elements.elements[i].semantic, input.semantic) && elements.elements[i].semanticIndex == input.semanticIndex)
						element = &elements.elements[i];
				}
				if (!element)
					throw std::runtime_error(fmt::format("vertex input {}{} is not in the geometry's layout {:016X}", input.semantic, input.semanticIndex, a_vertexLayout));
				// Stream 1 is a dynamic shape's positions (BSDynamicTriShape::dynamicData, float4 per vertex): every draw binds a second
				// vertex buffer (DrawSequence), the face shape's positions or its own buffer again.
				if (element->slot > 1 || element->perInstance)
					throw std::runtime_error(fmt::format("vertex input {}{} comes from stream {} (only the geometry's stream and a dynamic shape's positions are supported)",
						input.semantic, input.semanticIndex, element->slot));
				secondStream |= element->slot == 1;
				layout.attributes.push_back(rhi::InputAttributeDesc{ element->slot, element->offset, rhi::helpers::ToRHI(element->format), element->semantic, element->semanticIndex, input.location });
			}
			if (secondStream)
				layout.bindings.push_back(rhi::InputBindingDesc{ 1, 16, rhi::InputRate::PerVertex, 1 });
			return layout;
		}

		// Initialize's pipeline layout ranges.
		rhi::LayoutBindingRange LayoutRange(std::uint32_t a_binding, std::uint32_t a_count, rhi::ShaderStage a_stage, rhi::LayoutRangeSource a_source, std::size_t a_offset,
			bool a_samplers = false)
		{
			rhi::LayoutBindingRange range{};
			range.set = 0;
			range.binding = a_binding;
			range.count = a_count;
			range.visibility = a_stage;
			range.source = a_source;
			range.recordOffset = static_cast<std::uint32_t>(a_offset);
			range.samplers = a_samplers;
			return range;
		}
		// A range whose record (or pushed address) is at a_word of push range a_root.
		rhi::LayoutBindingRange RangeFrom(rhi::LayoutBindingRange a_range, std::uint32_t a_root, std::uint32_t a_word)
		{
			a_range.addressRootIndex = a_root;
			a_range.addressOffset32 = a_word;
			return a_range;
		}
		rhi::LayoutBindingRange PushedRange(std::uint32_t a_binding, rhi::ShaderStage a_stage, std::uint32_t a_root, std::uint32_t a_word)
		{
			return RangeFrom(LayoutRange(a_binding, 1, a_stage, rhi::LayoutRangeSource::PushAddress, 0), a_root, a_word);
		}
		rhi::LayoutBindingRange FromMaterialRow(rhi::LayoutBindingRange a_range) { return RangeFrom(a_range, 0, kDrawPushMaterialRow); }
		rhi::LayoutBindingRange FromFrameRecord(rhi::LayoutBindingRange a_range) { return RangeFrom(a_range, 1, kFramePushRecord); }

		// The main pass: pass-wide blocks by push address, b0/b1/b2/b4 from the draw's rows, the rest from the frame record.
		std::vector<rhi::LayoutBindingRange> MainLayoutRanges(bool a_pulled)
		{
			// A pulled draw (Z-prepass plain draws) reads its rows itself (DCLF_PULLED): no row ranges.
			auto fromRow = [](rhi::LayoutBindingRange a_range) { return RangeFrom(a_range, 0, kDrawPushPipelineRow); };
			std::vector<rhi::LayoutBindingRange> mainRanges;
			std::uint32_t word = kFramePushRegisters;
			for (const bool pixel : { false, true }) {
				const std::uint32_t mask = pixel ? kFramePushPS : kFramePushVS;
				const auto stage = pixel ? rhi::ShaderStage::Pixel : rhi::ShaderStage::Vertex;
				const std::size_t frame = pixel ? offsetof(DrawBindings, pixelConstants) : offsetof(DrawBindings, vertexConstants);
				for (std::uint32_t r = 0; r < kConstantBufferRegisters; ++r) {
					const std::uint32_t b = kBindingShiftB + r;
					const auto address = rhi::LayoutRangeSource::IndirectAddress;
					if ((mask >> r) & 1) {
						mainRanges.push_back(PushedRange(b, stage, 1, word));
						word += 2;
					} else if (a_pulled && (r <= 2 || r == 4)) {
						continue;
					} else if (r == 0) {
						mainRanges.push_back(fromRow(LayoutRange(b, 1, stage, address,
							kPipelineRowHeader + (pixel ? offsetof(PipelineRowHeader, psTechnique) : offsetof(PipelineRowHeader, vsTechnique)))));
					} else if (r == 1) {
						mainRanges.push_back(FromMaterialRow(LayoutRange(b, 1, stage, address,
							kMaterialRowHeader + (pixel ? offsetof(MaterialRowHeader, psMaterial) : offsetof(MaterialRowHeader, vsMaterial)))));
					} else if (r == 2) {
						mainRanges.push_back(fromRow(LayoutRange(b, 1, stage, address,
							kPipelineRowHeader + (pixel ? offsetof(PipelineRowHeader, psGeometry) : offsetof(PipelineRowHeader, vsGeometry)))));
					} else if (r == 4) {
						mainRanges.push_back(fromRow(LayoutRange(b, 1, stage, address,
							kPipelineRowHeader + (pixel ? offsetof(PipelineRowHeader, psPermutation) : offsetof(PipelineRowHeader, vsPermutation)))));
					} else {
						mainRanges.push_back(FromFrameRecord(LayoutRange(b, 1, stage, address, frame + 8 * std::size_t{ r })));
					}
				}
			}
			// Textures and samplers: t0-t15 and the features' from the material row, except the shadow mask (t14, pipeline row); the rest
			// from the frame record.
			const auto index = rhi::LayoutRangeSource::IndirectIndex;
			const auto pixelStage = rhi::ShaderStage::Pixel;
			for (std::uint32_t t = 0; t < kPixelTextureSlots; ++t) {
				if (a_pulled) {
					continue;
				} else if (t == kShadowMaskSlot) {
					mainRanges.push_back(fromRow(LayoutRange(kBindingShiftT + t, 1, pixelStage, index, kPipelineRowHeader + offsetof(PipelineRowHeader, shadowMask))));
					mainRanges.push_back(fromRow(LayoutRange(kBindingShiftS + t, 1, pixelStage, index, kPipelineRowHeader + offsetof(PipelineRowHeader, shadowMaskSampler), true)));
				} else {
					mainRanges.push_back(FromMaterialRow(LayoutRange(kBindingShiftT + t, 1, pixelStage, index, kMaterialRowHeader + offsetof(MaterialRowHeader, textures) + 4 * std::size_t{ t })));
					mainRanges.push_back(FromMaterialRow(LayoutRange(kBindingShiftS + t, 1, pixelStage, index, kMaterialRowHeader + offsetof(MaterialRowHeader, samplers) + 4 * std::size_t{ t }, true)));
				}
			}
			std::uint32_t first = kPixelTextureSlots;
			for (std::uint32_t f = 0; f < kFeatureMaterialTextures; ++f) {
				const std::uint32_t t = kFeatureMaterialRegisters[f];
				if (t > first)
					mainRanges.push_back(FromFrameRecord(LayoutRange(kBindingShiftT + first, t - first, pixelStage, index, offsetof(DrawBindings, textures) + 4 * std::size_t{ first })));
				if (!a_pulled)
					mainRanges.push_back(FromMaterialRow(LayoutRange(kBindingShiftT + t, 1, pixelStage, index, kMaterialRowHeader + offsetof(MaterialRowHeader, features) + 4 * std::size_t{ f })));
				first = t + 1;
			}
			mainRanges.push_back(FromFrameRecord(LayoutRange(kBindingShiftT + first, kTextureRegisters - first, pixelStage, index, offsetof(DrawBindings, textures) + 4 * std::size_t{ first })));
			// The vertex stage's buffers (t121-t127), from the frame record.
			mainRanges.push_back(FromFrameRecord(LayoutRange(kVertexTextureBinding, kVertexTextureCount, rhi::ShaderStage::Vertex, index,
				offsetof(DrawBindings, textures) + 4 * std::size_t{ kShadingBufferRegister })));
			return mainRanges;
		}

		// The shadow views': the draw's words name its material row, the view's push words and frame record the rest.
		std::vector<rhi::LayoutBindingRange> ShadowLayoutRanges()
		{
			const auto index = rhi::LayoutRangeSource::IndirectIndex;
			const auto pixelStage = rhi::ShaderStage::Pixel;
			auto fromShadowFrameRecord = [](rhi::LayoutBindingRange a_range) { return RangeFrom(a_range, 1, kShadowPushFrameRecord); };
			std::vector<rhi::LayoutBindingRange> shadowRanges;
			for (const bool pixel : { false, true }) {
				const auto stage = pixel ? rhi::ShaderStage::Pixel : rhi::ShaderStage::Vertex;
				for (std::uint32_t r = 0; r < kConstantBufferRegisters; ++r) {
					const std::uint32_t b = kBindingShiftB + r;
					if (!pixel && r == 1)
						shadowRanges.push_back(PushedRange(b, stage, 0, kDrawPushMaterialRow));  // the material row itself
					else if (r == 0)
						shadowRanges.push_back(PushedRange(b, stage, 1, kShadowPushViewBlock));
					else if (r == kPerFrameVertexRegister)
						shadowRanges.push_back(PushedRange(b, stage, 1, kShadowPushPerFrame));
					else if (r == kSharedDataRegister)
						shadowRanges.push_back(PushedRange(b, stage, 1, kShadowPushSharedData));
					else if (r == kFeatureDataRegister)
						shadowRanges.push_back(PushedRange(b, stage, 1, kShadowPushFeatureData));
					else
						shadowRanges.push_back(PushedRange(b, stage, 1, kShadowPushZeros));
				}
			}
			// The diffuse (t0) from the draw's row; t1 and up, the samplers and the vertex stage's buffers from the frame record.
			shadowRanges.push_back(FromMaterialRow(LayoutRange(kBindingShiftT, 1, pixelStage, index, kShadowRowDiffuseOffset)));
			shadowRanges.push_back(fromShadowFrameRecord(LayoutRange(kBindingShiftT + 1, kTextureRegisters - 1, pixelStage, index, offsetof(DrawBindings, textures) + 4)));
			shadowRanges.push_back(fromShadowFrameRecord(LayoutRange(kBindingShiftS, kSamplerRegisters, pixelStage, index, offsetof(DrawBindings, samplers), true)));
			shadowRanges.push_back(fromShadowFrameRecord(LayoutRange(kVertexTextureBinding, kVertexTextureCount, rhi::ShaderStage::Vertex, index,
				offsetof(DrawBindings, textures) + 4 * std::size_t{ kShadingBufferRegister })));
			return shadowRanges;
		}
	}

	struct DrawPipelines::Impl
	{
		struct Entry
		{
			std::uint32_t index = kNotReady;  // handed out once a published set version holds the pipeline
			std::uint32_t slot = kNotReady;   // its index in the sets, from its admission
			bool failed = false;
		};
		/** @brief A RequestLighting key, with the pipeline slot that asked. */
		struct LightingRequest
		{
			PipelineKey key{};
			std::uint32_t slot = ~0u;
		};
		/** @brief A RequestShadow key, with the casters' key (no mode bits or view state) and occlusion view, for the warnings. */
		struct ShadowRequest
		{
			ShadowPipelineKey viewKey{};
			ShadowPipelineKey casterKey{};
			std::uint32_t occlusion = ~0u;
		};

		/**
		 * @brief A version of an indirect pipeline set group and its signatures, holding the admitted pipelines [0, applied).
		 *
		 * Frames keep the version they record with (IndirectState::version). New pipelines are written only into a version nothing else
		 * references, at indices it never held, then published. So a set is never written while a recording reads it or sizes preprocess
		 * memory against it (VUID-VkGeneratedCommandsInfoEXT-preprocessSize-11071), and no entry submitted work may use is rewritten
		 * (VUID-VkWriteIndirectExecutionSetPipelineEXT-index-11029).
		 */
		struct SetVersion
		{
			std::array<rhi::IndirectPipelineSetPtr, kVariantCount> sets;  // one per variant; a key has the same index in both
			std::array<rhi::CommandSignaturePtr, kVariantCount> signatures;
			// [0, applied), held while a recording holds the version.
			std::vector<org::services::PipelinePayload> pipelines;
			// Each pipeline's Z-prepass group, and each group's first pipeline (whose pulled depth pipeline is the group's).
			std::vector<std::uint32_t> zGroupOf, zGroupFirst;
			std::uint32_t applied = 0;
		};
		// The shadow views' pipelines, versioned the same way; their plain draws bind each by index (ShadowIndirectState).
		struct ShadowSetVersion
		{
			// [0, applied), held while a recording holds the version, with their vertex layouts and classes.
			std::vector<org::services::PipelinePayload> pipelines;
			std::vector<std::uint64_t> layouts;
			std::vector<std::uint8_t> discards;
			std::uint32_t applied = 0;
		};
		// As many versions as are held at once: the published one, those of frames being recorded, and those of kept recordings (ORG's
		// recording reuse holds the version a recording bound while it may be resubmitted). A new one is made when none is free, so a
		// new pipeline never waits for a recording.
		template <class Version>
		struct Versions
		{
			std::vector<std::shared_ptr<Version>> owned;
			std::atomic<std::shared_ptr<Version>> published;
			// Previous generations' versions, dropped once nothing holds them.
			std::vector<std::shared_ptr<Version>> retired;
			std::uint32_t handedOut = 0;  // the pipelines whose Entry::index is set
			bool failureLogged = false;

			void Retire()
			{
				for (auto& version : owned)
					if (version)
						retired.push_back(std::move(version));
				owned.clear();
				published.store(nullptr);
				handedOut = 0;
			}

			enum class PublishResult
			{
				Current,    // the published version already holds every admitted pipeline
				Published,  // a version was brought up to them and published
				Waiting,    // every other version is still held: the admissions wait for a later frame
				Failed,     // a set operation failed
			};

			/** @brief Brings an unreferenced version up to a_admitted (a_apply writes [applied, a_admitted)) and publishes it. */
			template <class Apply>
			PublishResult Publish(std::uint32_t a_admitted, Apply&& a_apply)
			{
				std::erase_if(retired, [](const std::shared_ptr<Version>& a_version) { return a_version.use_count() == 1; });
				const auto current = published.load();
				if (current && current->applied == a_admitted)
					return PublishResult::Current;
				// A free one (unreferenced, and unreachable until published), or a new one.
				auto free = std::ranges::find_if(owned, [&](const std::shared_ptr<Version>& a_version) {
					return !a_version || (a_version != current && a_version.use_count() == 1);
				});
				if (free == owned.end())
					free = owned.insert(owned.end(), nullptr);
				{
					auto& version = *free;
					// Every other owner has let go: whatever a recording did with it happened before this.
					std::atomic_thread_fence(std::memory_order_acquire);
					if (!version)
						version = std::make_shared<Version>();
					if (!a_apply(*version)) {
						// Start that version over: what it holds may be partial.
						version.reset();
						return PublishResult::Failed;
					}
					version->applied = a_admitted;
					published.store(version);
					return PublishResult::Published;
				}
			}
		};

		rhi::Device device{};
		rhi::PipelineLayoutPtr layout;
		// The shadow views' layout (kShadowPushWords): pass-wide blocks pushed per view.
		rhi::PipelineLayoutPtr shadowLayout;
		// The Z-prepass's plain draws: their layout and a DrawSequence tail's signature.
		rhi::PipelineLayoutPtr zLayout;
		rhi::CommandSignaturePtr zDrawSignature;
		// Tree LOD's draw signature: a plain DrawInstanced under zLayout. Its pipelines are the lane's (Lane::TreeLod).
		rhi::CommandSignaturePtr treeLodDrawSignature;
		struct TreeLodBuilt
		{
			rhi::PipelinePtr depth, colour;
		};
		// Render thread: what it has asked of the lane already, so it asks each once.
		bool treeLodAsked = false;
		bool refractionAsked = false;  // the refraction pipeline (RequestRefraction)
		ankerl::unordered_dense::set<ForwardPipelineKey, ForwardPipelineKeyHash> forwardAsked;
		rhi::PipelineLayoutHandle ShadowLayout() const { return shadowLayout->GetHandle(); }
		// The shadow views' plain indirect draw: a DrawSequence's tail as an indexed draw from the index pool (StoreShadowSequence).
		rhi::CommandSignaturePtr shadowDrawSignature;
		bool supported = false;
		bool attempted = false;
		org::services::PipelineService service;
		/*
		 * Build completions: each callback pushes its artifact (on the building thread, or inline on the requesting thread for a cached
		 * pipeline) and wakes the lane, which drains them. Each carries its request's generation; one from before a target or format
		 * change is dropped.
		 */
		struct MainDone
		{
			PipelineKey key;
			std::uint32_t generation = 0;
			org::services::PipelineArtifact artifact;
		};
		struct ShadowDone
		{
			ShadowPipelineKey key;
			std::uint32_t generation = 0;
			org::services::PipelineArtifact artifact;
		};
		struct TreeLodDone
		{
			std::uint32_t generation = 0;
			org::services::PipelineArtifact artifact;
		};
		struct ForwardDone
		{
			ForwardPipelineKey key;
			std::uint32_t generation = 0;
			org::services::PipelineArtifact artifact;
		};
		struct RefractionDone
		{
			std::uint32_t generation = 0;              // the main set's (the main depth's format)
			DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;  // the refraction normals' it was built for
			org::services::PipelineArtifact artifact;
		};
		struct Completions
		{
			EventQueue<MainDone, 1024> main;
			EventQueue<ShadowDone, 1024> shadow;
			EventQueue<TreeLodDone, 16> treeLod;
			EventQueue<ForwardDone, 256> forward;
			EventQueue<RefractionDone, 16> refraction;
			// Requests to the lane, and the lane's engine-state requests to the render thread (CaptureEngineStates replies).
			EventQueue<LightingRequest, 1024> requests;
			EventQueue<ShadowRequest, 1024> shadowRequests;
			EventQueue<ForwardPipelineKey, 256> forwardRequests;
			std::atomic<bool> treeLodRequest{ false };
			std::atomic<bool> refractionRequest{ false };
			EventQueue<std::uint32_t, 64> stateRequests;
		};
		// Never freed: a build may complete while the process tears down.
		Completions* completions = new Completions;

		// The builds run on DCLF's preparation pool (BuildExecutor.h), not a thread each.
		Impl() { service.Configure(BuildSubmitter()); }

		// Render thread: the shadow view rasterizer states by id - 1; the lane reads the posted copy (FrameInput::shadowStates).
		struct ShadowRasterState
		{
			float depthBias = 0.0f;
			float depthBiasClamp = 0.0f;
			float slopeScaledDepthBias = 0.0f;
			rhi::CullMode cull = rhi::CullMode::Back;
			bool frontCCW = false;

			bool operator==(const ShadowRasterState&) const = default;
		};
		std::vector<ShadowRasterState> shadowRasterStates;

		/**
		 * @brief Render thread: the engine's FrontCounterClockwise, read once from its table; empty until the table holds a state. Every
		 * entry has the same winding, and Community Shaders' per-cascade copies are copies of it.
		 */
		std::optional<bool> EngineFrontCCW()
		{
			if (!engineFrontCCW) {
				if (auto* state = EngineRasterStates()[0][1][0][0]) {
					D3D11_RASTERIZER_DESC desc{};
					state->GetDesc(&desc);
					engineFrontCCW = desc.FrontCounterClockwise != FALSE;
					logger::info("[DCLF] engine rasterizer winding: front face {}", *engineFrontCCW ? "counter-clockwise" : "clockwise");
				}
			}
			return engineFrontCCW;
		}
		std::optional<bool> engineFrontCCW;
		std::uint32_t loggedShadowRasterFailures = 0;
		// A build's result: both variants' pipelines and the registers the shaders read.
		struct Built
		{
			std::array<rhi::PipelinePtr, kVariantCount> pipelines;
			// Per variant: the Z-prepass pixel stage reads far less than the colour one.
			std::array<RegisterUsage, kVariantCount> usage;
			bool discards = false;  // a shadow pipeline whose pixel stage defers the depth test (FragmentDefersDepth)
			// A main pipeline's Z-prepass plain-draw pipeline (pulled stages) and its identity, shared by equal keys.
			rhi::PipelinePtr pulledDepth;
			ZPipelineKey zKey;
			std::shared_ptr<const ConstantTables> tables;  // a main pipeline's stages' (ConstantTableOf)
		};

		/** @brief The engine's state behind a key's RasterStateBits, read once and kept in RHI terms so builds never touch D3D11. */
		struct EngineState
		{
			float depthBias = 0.0f;
			float depthBiasClamp = 0.0f;
			float slopeScaledDepthBias = 0.0f;
			rhi::BlendState blend{};
			bool valid = false;  // false: a state object was missing or used something the RHI cannot express
			// An opaque key's: only the blend (its write masks) is the engine's; depth and bias stay the opaque pass's.
			bool blendOnly = false;
		};
		// The opaque groups' write modes (alphaBlendWriteMode), as the engine draws them: alpha-tested (accumulation hint 2) writes
		// every channel (mode 10); plain opaque (hint 0) leaves RT0's alpha alone (mode 1), so a shader alpha below 1 never reaches
		// the G-buffer.
		static constexpr std::uint32_t kOpaqueWriteMode = 1, kAlphaTestedWriteMode = 10;
		ankerl::unordered_dense::map<std::uint32_t, EngineState> engineStates;  // by RasterStateBits
		std::uint32_t loggedStateFailures = 0;

		static bool ToRHI(D3D11_BLEND a_factor, rhi::BlendFactor& a_out)
		{
			switch (a_factor) {
			case D3D11_BLEND_ONE: a_out = rhi::BlendFactor::One; return true;
			case D3D11_BLEND_ZERO: a_out = rhi::BlendFactor::Zero; return true;
			case D3D11_BLEND_SRC_COLOR: a_out = rhi::BlendFactor::SrcColor; return true;
			case D3D11_BLEND_INV_SRC_COLOR: a_out = rhi::BlendFactor::InvSrcColor; return true;
			case D3D11_BLEND_SRC_ALPHA: a_out = rhi::BlendFactor::SrcAlpha; return true;
			case D3D11_BLEND_INV_SRC_ALPHA: a_out = rhi::BlendFactor::InvSrcAlpha; return true;
			case D3D11_BLEND_DEST_COLOR: a_out = rhi::BlendFactor::DstColor; return true;
			case D3D11_BLEND_INV_DEST_COLOR: a_out = rhi::BlendFactor::InvDstColor; return true;
			case D3D11_BLEND_DEST_ALPHA: a_out = rhi::BlendFactor::DstAlpha; return true;
			case D3D11_BLEND_INV_DEST_ALPHA: a_out = rhi::BlendFactor::InvDstAlpha; return true;
			default: return false;  // SRC_ALPHA_SAT, BLEND_FACTOR and the SRC1 family: not in the RHI
			}
		}

		static bool ToRHI(D3D11_BLEND_OP a_op, rhi::BlendOp& a_out)
		{
			switch (a_op) {
			case D3D11_BLEND_OP_ADD: a_out = rhi::BlendOp::Add; return true;
			case D3D11_BLEND_OP_SUBTRACT: a_out = rhi::BlendOp::Sub; return true;
			case D3D11_BLEND_OP_REV_SUBTRACT: a_out = rhi::BlendOp::RevSub; return true;
			case D3D11_BLEND_OP_MIN: a_out = rhi::BlendOp::Min; return true;
			case D3D11_BLEND_OP_MAX: a_out = rhi::BlendOp::Max; return true;
			default: return false;
			}
		}

		/** @brief The engine's state objects for these state bits, in RHI terms. */
		EngineState ReadEngineState(std::uint32_t a_stateBits, std::string& a_error)
		{
			EngineState state{};
			const std::uint32_t bias = RasterDepthBiasMode(a_stateBits);
			const std::uint32_t blendMode = RasterBlendMode(a_stateBits);
			const std::uint32_t writeMode = RasterWriteMode(a_stateBits);
			const std::uint32_t alphaToCoverage = (a_stateBits & kRasterAlphaToCoverage) ? 1u : 0u;
			const std::uint32_t extra = (a_stateBits & kRasterBlendExtra) ? 1u : 0u;
			if (bias >= 12 || blendMode >= 7 || writeMode >= 13) {
				a_error = fmt::format("state indices out of range (bias {}, blend {}, write {})", bias, blendMode, writeMode);
				return state;
			}
			// Fill solid, cull back, no scissor: the cull mode comes from the key, and bias is the same across the table's cull modes.
			auto* raster = EngineRasterStates()[0][1][bias][0];
			// The deferred variant: DCLF's main-pass draws are G-buffer draws.
			auto* blend = DeferredBlendState(blendMode, alphaToCoverage, writeMode, extra);
			if (!raster || !blend) {
				a_error = fmt::format("no engine state object at bias {} / blend [{}][{}][{}][{}] (the deferred blend states are made on the first deferred pass)",
					bias, blendMode, alphaToCoverage, writeMode, extra);
				return state;
			}
			D3D11_RASTERIZER_DESC rasterDesc{};
			raster->GetDesc(&rasterDesc);
			state.depthBias = static_cast<float>(rasterDesc.DepthBias);
			state.depthBiasClamp = rasterDesc.DepthBiasClamp;
			state.slopeScaledDepthBias = rasterDesc.SlopeScaledDepthBias;
			D3D11_BLEND_DESC blendDesc{};
			blend->GetDesc(&blendDesc);
			state.blend.alphaToCoverage = blendDesc.AlphaToCoverageEnable != FALSE;
			state.blend.independentBlend = true;
			state.blend.numAttachments = 8;
			for (std::uint32_t i = 0; i < 8; ++i) {
				const auto& source = blendDesc.RenderTarget[blendDesc.IndependentBlendEnable ? i : 0];
				auto& target = state.blend.attachments[i];
				target.enable = source.BlendEnable != FALSE;
				target.writeMask = static_cast<rhi::ColorWriteEnable>(source.RenderTargetWriteMask & 0xF);
				if (!ToRHI(source.SrcBlend, target.srcColor) || !ToRHI(source.DestBlend, target.dstColor) || !ToRHI(source.BlendOp, target.colorOp) ||
					!ToRHI(source.SrcBlendAlpha, target.srcAlpha) || !ToRHI(source.DestBlendAlpha, target.dstAlpha) || !ToRHI(source.BlendOpAlpha, target.alphaOp)) {
					a_error = fmt::format("blend [{}][{}][{}][{}] target {} uses a blend factor or op the RHI does not express", blendMode, alphaToCoverage, writeMode, extra, i);
					return state;
				}
			}
			state.valid = true;
			return state;
		}

		/** @brief Render thread -> lane, latest-wins, posted whole (PostInput): what the builds need that only the render thread can read. */
		struct FrameInput
		{
			TargetFormats targets;
			std::uint32_t targetsGeneration = 0;  // DrawPipelines::generation, whose targets these are
			// Engine states by RasterStateBits (CaptureEngineStates): every one asked for, valid or not.
			std::shared_ptr<const ankerl::unordered_dense::map<std::uint32_t, EngineState>> states;
			std::optional<bool> frontCCW;      // EngineFrontCCW, once the engine's table holds a state
			RE::BSShader* lighting = nullptr;  // the Lighting shader the programs' defines are of (EngineShaders::Lighting)
			// By program family: the engine shader its programs' defines are of (its programSource: the Effect port's BSEffectShader).
			std::array<const void*, Extensions::kProgramFamilies> familySource{};
			// Rasterizer states by id - 1, append-only: a newer list holds every id an older one did.
			RE::BSShader* utility = nullptr;
			DXGI_FORMAT shadowFormat = DXGI_FORMAT_UNKNOWN;
			std::shared_ptr<const std::vector<ShadowRasterState>> shadowStates;
			// SetTreeLodInputs (DistantTree: tree LOD's and the forward tree program), SetForwardTargets.
			RE::BSShader* distantTree = nullptr;
			ForwardTargets forwardTargets;
			ForwardTargets firstPersonTargets;  // SetFirstPersonTargets: the first-person keys'
			// SetRefractionInputs.
			RE::BSShader* refractionUtility = nullptr;
			DXGI_FORMAT refractionFormat = DXGI_FORMAT_UNKNOWN;
			std::uint8_t refractionWriteMask = 0xF;
		};
		LatestSlot<FrameInput> inputSlot;
		FrameInput renderInput;                       // render thread: what it posts, as last posted
		std::vector<std::uint32_t> pendingStateBits;  // render thread: asked for by the lane, not readable yet (no deferred pass yet)
		LatestSlot<PipelineCatalog> catalogSlot;      // lane -> the scene lane (TakeCatalog)

		/** @brief Render thread: posts the current frame inputs and wakes the lane. */
		void PostInput(const DrawPipelines& a_owner)
		{
			renderInput.targets = a_owner.targets;
			renderInput.targetsGeneration = a_owner.generation;
			inputSlot.Post(std::make_unique<FrameInput>(renderInput));
			WakePipelineLane();
		}

		/** @brief A family's counters (read by the report, relaxed). */
		struct FamilyCounters
		{
			std::atomic<std::uint32_t> requested{ 0 }, ready{ 0 }, failed{ 0 }, setPublishes{ 0 }, setWaits{ 0 }, zPipelines{ 0 }, zDepthOnly{ 0 };
			void Zero()
			{
				for (auto* counter : { &requested, &ready, &failed, &zPipelines, &zDepthOnly })
					counter->store(0, std::memory_order_relaxed);
			}
		};
		/** @brief A forward view's pipeline: built, its build queued or done, or failed for good (this generation). */
		struct ForwardEntry
		{
			org::services::PipelinePayload built;
			bool requested = false;
			bool failed = false;
		};
		/**
		 * @brief One keyed family of the lane's pipelines (main set, shadow set, forward): entries, every key asked for, keys parked on
		 * their program or an input, counters. A change of what they are built for starts a new generation (StartOver): queued builds
		 * finish and their completions are dropped.
		 */
		template <class Key, class Hash, class E, class Asked>
		struct PipelineFamily
		{
			std::uint32_t generation = 0;
			ankerl::unordered_dense::map<Key, E, Hash> entries;
			std::uint32_t failedEntries = 0;
			Asked asked;
			// Keys waiting on their program (by id), retried when UpdateLane finishes it; keys waiting on an input, retried on input change.
			ankerl::unordered_dense::map<std::uint64_t, std::vector<Key>> programWaiters;
			std::vector<Key> blocked;
			std::uint32_t loggedFailures = 0;
			FamilyCounters counters;

			/** @brief At capacity, counting queued builds (every unfailed entry takes a slot). */
			bool Full() const { return entries.size() - failedEntries >= kMaxPipelines; }
			/** @brief a_key's program can never be made: its objects stay native. */
			void FailForGood(const Key& a_key)
			{
				entries.try_emplace(a_key).first->second.failed = true;
				++failedEntries;
			}
			template <class Request>
			void RetryProgram(std::uint64_t a_program, Request&& a_request)
			{
				const auto waiting = programWaiters.find(a_program);
				if (waiting == programWaiters.end())
					return;
				const auto keys = std::move(waiting->second);
				programWaiters.erase(waiting);
				for (const auto& key : keys)
					a_request(key);
			}
			template <class Request>
			void RetryBlocked(Request&& a_request)
			{
				for (const auto& key : std::exchange(blocked, {}))
					a_request(key);
			}
			void StartOver()
			{
				entries.clear();
				failedEntries = 0;
				++generation;
				counters.Zero();
				// Every key asked for is requested again once the new inputs are in (DrainLane retries blocked).
				programWaiters.clear();
				blocked.clear();
				for (const auto& request : asked) {
					if constexpr (requires { request.first; })
						blocked.push_back(request.first);
					else
						blocked.push_back(request);
				}
			}
		};

		/**
		 * @brief The lane's state: touched only by its passes (DrainLane), one at a time on the coordinator, except the counters (read by
		 * the report). Its own family is the main set, keyed by PipelineKey with the slot that first asked.
		 */
		struct Lane : PipelineFamily<PipelineKey, PipelineKeyHash, Entry, ankerl::unordered_dense::map<PipelineKey, std::uint32_t, PipelineKeyHash>>
		{
			std::shared_ptr<const FrameInput> input;  // the newest taken
			ankerl::unordered_dense::set<std::uint32_t> statesAsked;  // engine state bits asked of the render thread
			ShaderPrograms::Finished finishedPrograms;               // UpdateLane's, per pass (every kind's)
			Versions<SetVersion> versions;
			std::vector<org::services::PipelinePayload> setPipelines;  // index -> Built, admitted (not necessarily published)
			// Z-prepass groups: each pipeline's (numbered as first admitted), and each group's first pipeline.
			std::vector<std::uint32_t> zGroupOf, zGroupFirst;
			ankerl::unordered_dense::map<ZPipelineKey, std::uint32_t, ZPipelineKeyHash> zGroups;
			std::vector<std::array<RegisterUsage, kVariantCount>> usage;  // by set index, then variant
			std::vector<std::shared_ptr<const ConstantTables>> constantTables;  // by set index
			std::string recreatedBy;  // the set's last recreation, for NearestKey's warnings
			std::uint64_t revision = 0;
			bool dirty = false;  // the catalog to publish at the end of the pass
			// Stats::families, by program family.
			struct ProgramFamilyCounters
			{
				std::atomic<std::uint32_t> requested{ 0 }, ready{ 0 }, failed{ 0 };
			};
			std::array<ProgramFamilyCounters, Extensions::kProgramFamilies> programFamilyCounters;
			/**
			 * @brief The shadow views' set, kept as the main set: keyed by view key (with mode bits and view state), built for the shadow map
			 * format, its own versions. Its generation is PipelineCatalog::shadowGeneration.
			 */
			struct Shadow : PipelineFamily<ShadowPipelineKey, ShadowPipelineKeyHash, Entry, ankerl::unordered_dense::map<ShadowPipelineKey, ShadowRequest, ShadowPipelineKeyHash>>
			{
				Versions<ShadowSetVersion> versions;
				// By index, admitted: the pipeline, its vertex layout and its class (kShadowDiscards).
				std::vector<org::services::PipelinePayload> setPipelines;
				std::vector<std::uint64_t> layouts;
				std::vector<std::uint8_t> discards;
				std::string recreatedBy;  // the set's last recreation, for NearestShadowKey's warnings
			};
			Shadow shadow;
			/** @brief Tree LOD's two pipelines, rebuilt with each new main set (RecreateMainSet), under the main set's generation. */
			struct TreeLod
			{
				bool asked = false;          // RequestTreeLod has arrived: built from here on, for every generation
				bool requested = false;      // this generation's build is queued
				bool failed = false;         // this generation's build failed, or the program did
				bool programFailed = false;  // for good
				org::services::PipelinePayload built;
				// Earlier generations' builds, kept for the process: a recording may still draw with them.
				std::vector<org::services::PipelinePayload> retired;
				struct Counters
				{
					std::atomic<std::uint32_t> requested{ 0 }, ready{ 0 }, failed{ 0 };
				};
				Counters counters;
			};
			TreeLod treeLod;
			/** @brief The refraction pipeline, rebuilt with each new main set (RecreateMainSet) or new normals format / write mask. */
			struct Refraction
			{
				bool asked = false;          // RequestRefraction has arrived
				bool requested = false;      // this build is queued
				bool failed = false;         // this build failed, or the program did
				bool programFailed = false;  // for good
				DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;  // the format requested or built for
				org::services::PipelinePayload built;
				// Earlier builds, kept for the process: a recording may still draw with them.
				std::vector<org::services::PipelinePayload> retired;
				struct Counters
				{
					std::atomic<std::uint32_t> requested{ 0 }, ready{ 0 }, failed{ 0 };
				};
				Counters counters;
				/** @brief Starts over: the build is retired (kept for the process), the program's verdict kept. */
				void Reset()
				{
					if (built)
						retired.push_back(std::move(built));
					built = {};
					requested = false;
					failed = programFailed;
					format = DXGI_FORMAT_UNKNOWN;
				}
			};
			Refraction refraction;
			/**
			 * @brief The forward pipelines, built for the forward targets (rebuilt by RecreateForward). No set: recordings bind each by
			 * handle, so every build is kept for the process. Programs by ShaderPrograms::ForwardProgramId, the forward tree's by kTreeProgram.
			 */
			struct Forward : PipelineFamily<ForwardPipelineKey, ForwardPipelineKeyHash, ForwardEntry, ankerl::unordered_dense::set<ForwardPipelineKey, ForwardPipelineKeyHash>>
			{
				static constexpr std::uint64_t kTreeProgram = ~0ull;
				std::vector<org::services::PipelinePayload> retired;  // earlier targets' builds, kept for the process
			};
			Forward forward;
		};
		Lane lane;

		/** @brief Publishes a family's admitted pipelines (Versions::Publish) and hands out the published indices (a_onReady per key). */
		template <class Family, class Version, class Publish, class OnReady>
		void PublishAdmitted(Family& a_family, Versions<Version>& a_versions, std::uint32_t a_admitted, const char* a_what, Publish&& a_publish, OnReady&& a_onReady)
		{
			using Result = typename Versions<Version>::PublishResult;
			const auto result = a_versions.Publish(a_admitted, a_publish);
			if (result == Result::Published)
				a_family.counters.setPublishes.fetch_add(1, std::memory_order_relaxed);
			if (result == Result::Waiting)
				a_family.counters.setWaits.fetch_add(1, std::memory_order_relaxed);
			if ((result == Result::Current || result == Result::Published) && a_versions.handedOut != a_admitted) {
				a_versions.handedOut = a_admitted;
				for (auto& [key, entry] : a_family.entries)
					if (entry.index == kNotReady && entry.slot != kNotReady) {
						entry.index = entry.slot;
						a_family.counters.ready.fetch_add(1, std::memory_order_relaxed);
						a_onReady(key);
					}
				lane.dirty = true;
			} else if (result == Result::Failed && !a_versions.failureLogged) {
				a_versions.failureLogged = true;
				logger::warn("[DCLF] Could not publish the {} ({} pipelines admitted)", a_what, a_admitted);
			}
		}

		/** @brief The lane's pass (SetPipelineLaneDrain). */
		static void DrainLaneThunk();
		void DrainLane();
		/** @brief The lane: requests a_key's program, then its pipeline, or parks it until what it lacks arrives. */
		void TryRequest(const PipelineKey& a_key);
		/** @brief The lane: the main pass's targets changed; every key is built again into a new set. */
		void RecreateMainSet(const TargetFormats& a_from, const TargetFormats& a_to);
		/** @brief The lane: admits completed builds, publishes a set version, hands out their indices. */
		void AdmitLane();
		/** @brief The lane: TryRequest for a shadow view key. */
		void TryRequestShadow(const ShadowPipelineKey& a_key);
		/** @brief The lane: the shadow map format changed; every shadow key is built again into a new set. */
		void RecreateShadowSet(DXGI_FORMAT a_from, DXGI_FORMAT a_to);
		/** @brief The lane: AdmitLane for the shadow views' set. */
		void AdmitShadow();
		/** @brief The lane: TryRequest for tree LOD, once asked for. */
		void TryRequestTreeLod();
		/** @brief The lane: TryRequest for a forward key. */
		void TryRequestForward(const ForwardPipelineKey& a_key);
		/** @brief The lane: TryRequest for the refraction pipeline, once asked for. */
		void TryRequestRefraction();
		/** @brief The lane: the forward targets changed; every forward key is built again. */
		void RecreateForward(const ForwardTargets& a_from, const ForwardTargets& a_to);
		/** @brief The lane: admits completed tree LOD and forward builds. */
		void AdmitTreeLodAndForward();
		void PublishCatalog();
		/** @brief The lane: NearestKey for a shadow view key, among the shadow keys requested. */
		std::string NearestShadowKey(const ShadowPipelineKey& a_key) const;
		/** @brief The lane, for the on-demand warnings: the requested key nearest a_key (fewest differing bits) and what differs. */
		std::string NearestKey(const PipelineKey& a_key) const;
		static std::string DescribeKey(const PipelineKey& a_key)
		{
			// A family's key is named by its key name ("effect"), a Lighting key by its technique.
			const auto family = KeyFamily(a_key.rasterFlags);
			return fmt::format("VS {:08X} PS {:08X} pass {:08X} ({}), raster {:X}, vertex layout {:016X}", a_key.vertexDescriptor, a_key.pixelDescriptor,
				a_key.passDescriptor,
				family != Api::ProgramFamily::Count ? std::string_view(Extensions::ProgramFamilyOf(family).keyName) : LightingTechniqueName((a_key.passDescriptor >> 24) & 0x3f),
				a_key.rasterFlags, a_key.vertexLayout);
		}

		bool Initialize()
		{
			if (layout)
				return true;
			if (attempted || !RenderGraphRuntime::Get().IsActive() || !RenderGraphRuntime::Get().Host())
				return false;
			attempted = true;
			device = RenderGraphRuntime::Get().Host()->GetDesc().device;

			::IndirectCommandsFeatureInfo indirect{};
			supported = device.QueryFeatureInfo(&indirect.header) == rhi::Result::Ok && indirect.pipelineSets && indirect.indexBufferArguments &&
			            indirect.vertexBufferArguments && indirect.indirectBindings && indirect.maxPipelineSetCount >= kMaxPipelines;
			if (!supported) {
				logger::warn("[DCLF] Indirect pipelines unavailable: pipeline sets {}, index buffers {}, vertex buffers {}, indirect bindings {}, up to {} pipelines",
					indirect.pipelineSets, indirect.indexBufferArguments, indirect.vertexBufferArguments, indirect.indirectBindings, indirect.maxPipelineSetCount);
				return false;
			}

			// A draw's push data (kDrawPushWords).
			rhi::PushConstantRangeDesc drawPush{};
			drawPush.visibility = rhi::ShaderStage::AllGraphics;
			drawPush.num32BitValues = kDrawPushWords;
			drawPush.set = 0;
			drawPush.binding = kRecordAddressBinding;
			rhi::PushConstantRangeDesc pushConstants[2] = { drawPush, {} };
			pushConstants[1].visibility = rhi::ShaderStage::AllGraphics;
			pushConstants[1].num32BitValues = kFramePushWords;
			pushConstants[1].set = 0;
			pushConstants[1].binding = kFramePushBinding;

			const auto mainRanges = MainLayoutRanges(false);
			const auto zRanges = MainLayoutRanges(true);
			const rhi::PipelineLayoutDesc desc{ .ranges = rhi::Span<rhi::LayoutBindingRange>{ mainRanges.data(), static_cast<std::uint32_t>(mainRanges.size()) },
				.pushConstants = { pushConstants, 2u }, .staticSamplers = {}, .flags = rhi::PF_AllowInputAssembler };
			logger::info("[DCLF] main layout: {} ranges, {} frame push words", mainRanges.size(), kFramePushWords);
			if (device.CreatePipelineLayout(desc, layout) != rhi::Result::Ok) {
				supported = false;
				logger::error("[DCLF] Could not create the indirect draw pipeline layout");
				return false;
			}
			// The Z-prepass's plain draws: the main ranges minus the rows', no input assembler.
			const rhi::PipelineLayoutDesc zDesc{ .ranges = rhi::Span<rhi::LayoutBindingRange>{ zRanges.data(), static_cast<std::uint32_t>(zRanges.size()) },
				.pushConstants = { pushConstants, 2u }, .staticSamplers = {}, .flags = rhi::PF_None };
			if (device.CreatePipelineLayout(zDesc, zLayout) != rhi::Result::Ok) {
				supported = false;
				logger::error("[DCLF] Could not create the Z-prepass's pipeline layout");
				return false;
			}
			// The shadow views (kShadowPushWords): the draw's words name its material row (pipeline row words unused), the view's the rest.
			rhi::PushConstantRangeDesc shadowPush[2] = { drawPush, pushConstants[1] };
			shadowPush[1].num32BitValues = kShadowPushWords;
			const auto shadowRanges = ShadowLayoutRanges();
			const rhi::PipelineLayoutDesc shadowDesc{ .ranges = rhi::Span<rhi::LayoutBindingRange>{ shadowRanges.data(), static_cast<std::uint32_t>(shadowRanges.size()) },
				.pushConstants = { shadowPush, 2u }, .staticSamplers = {}, .flags = rhi::PF_AllowInputAssembler };
			if (device.CreatePipelineLayout(shadowDesc, shadowLayout) != rhi::Result::Ok) {
				supported = false;
				logger::error("[DCLF] Could not create the shadow views' pipeline layout");
				return false;
			}
			return CreateDrawSignatures();
		}

		/** @brief Initialize's command signatures (false when a required one could not be made). */
		bool CreateDrawSignatures()
		{
			rhi::IndirectArg draw{};
			draw.kind = rhi::IndirectArgKind::DrawIndexed;
			rhi::CommandSignatureDesc drawDesc{};
			drawDesc.args = { &draw, 1 };
			drawDesc.byteStride = sizeof(DrawSequence);
			if (device.CreateCommandSignature(drawDesc, ShadowLayout(), shadowDrawSignature) != rhi::Result::Ok) {
				supported = false;
				logger::error("[DCLF] Could not create the shadow views' draw signature");
				return false;
			}
			if (device.CreateCommandSignature(drawDesc, zLayout->GetHandle(), zDrawSignature) != rhi::Result::Ok) {
				supported = false;
				logger::error("[DCLF] Could not create the Z-prepass's draw signature");
				return false;
			}
			rhi::IndirectArg treeDraw{};
			treeDraw.kind = rhi::IndirectArgKind::Draw;
			rhi::CommandSignatureDesc treeDesc{};
			treeDesc.args = { &treeDraw, 1 };
			treeDesc.byteStride = 4 * sizeof(std::uint32_t);
			if (device.CreateCommandSignature(treeDesc, zLayout->GetHandle(), treeLodDrawSignature) != rhi::Result::Ok)
				logger::warn("[DCLF] Could not create tree LOD's draw signature; tree LOD stays native");
			return true;
		}

		/** @brief A triangle-list graphics pipeline description (no pixel stage when a_pixel is empty); Create throws on failure. */
		struct PipelineDesc
		{
			PipelineDesc(rhi::PipelineLayoutHandle a_layout, std::span<const std::byte> a_vertex, std::span<const std::byte> a_pixel, DXGI_FORMAT a_depthFormat) :
				layout{ a_layout }, vertex{ rhi::ShaderStage::Vertex, { a_vertex.data(), static_cast<std::uint32_t>(a_vertex.size()) }, "main" },
				pixel{ rhi::ShaderStage::Pixel, { a_pixel.data(), static_cast<std::uint32_t>(a_pixel.size()) }, "main" }, hasPixel(!a_pixel.empty()),
				depthFormat{ rhi::helpers::ToRHI(a_depthFormat) }
			{
				blend.bs.numAttachments = 0;
			}
			rhi::SubobjLayout layout;
			rhi::SubobjShader vertex, pixel;
			bool hasPixel;
			rhi::SubobjRaster raster{};
			rhi::SubobjDepth depth{};
			rhi::SubobjBlend blend{};
			rhi::SubobjRTVs targets{};
			rhi::SubobjDSV depthFormat;
			rhi::SubobjInputLayout input{};
			rhi::SubobjFlags flags{};

			/** @brief Adds a colour target, blended per its index's blend attachment. */
			void Colour(DXGI_FORMAT a_format)
			{
				targets.rt.formats[targets.rt.count++] = rhi::helpers::ToRHI(a_format);
				blend.bs.numAttachments = targets.rt.count;
			}
			void Colours(const TargetFormats& a_targets)
			{
				for (std::uint32_t i = 0; i < a_targets.colorCount; ++i)
					Colour(a_targets.colors[i]);
			}
			void Create(rhi::Device a_device, rhi::PipelinePtr& a_out, std::string_view a_what) const
			{
				const rhi::SubobjPrimitiveTopology topology{ rhi::PrimitiveTopology::TriangleList };
				const rhi::PipelineStreamItem items[] = {
					rhi::Make(layout), rhi::Make(vertex), rhi::Make(raster), rhi::Make(depth), rhi::Make(blend), rhi::Make(targets), rhi::Make(depthFormat),
					rhi::Make(topology), rhi::Make(input), rhi::Make(flags), rhi::Make(pixel),
				};
				if (const auto result = a_device.CreatePipeline(items, static_cast<std::uint32_t>(std::size(items)) - (hasPixel ? 0u : 1u), a_out); result != rhi::Result::Ok)
					throw std::runtime_error(fmt::format("CreatePipeline ({}) failed ({})", a_what, static_cast<int>(result)));
			}
		};

		/** @brief A SPIR-V module's hash (pipeline service recipes, Z-prepass groups). */
		static std::uint64_t ModuleHash(const std::vector<std::byte>& a_module) { return ankerl::unordered_dense::detail::wyhash::hash(a_module.data(), a_module.size()); }

		/** @brief Parses SPIR-V modules, or throws. */
		template <std::size_t N>
		static std::array<SpirvReflection, N> Parse(const std::array<const std::vector<std::byte>*, N>& a_modules)
		{
			std::array<SpirvReflection, N> out;
			for (std::size_t i = 0; i < N; ++i)
				if (!out[i].Parse(*a_modules[i]))
					throw std::runtime_error("not SPIR-V");
			return out;
		}

		/** @brief Tree LOD's pipelines (TryRequestTreeLod). */
		static org::services::PipelinePayload BuildTreeLod(rhi::Device a_device, rhi::PipelineLayoutHandle a_zLayout, const ShaderPrograms::TreeLodProgram* a_program,
			TargetFormats a_targets, rhi::BlendState a_blend, bool a_frontCCW)
		{
			auto stages = Parse<4>({ &a_program->vertex, &a_program->pixel, &a_program->depthVertex, &a_program->depthPixel });
			for (std::size_t i = 0; i < stages.size(); ++i)
				CheckPulledBindings(stages[i], i % 2 == 1);
			auto built = std::make_shared<TreeLodBuilt>();
			for (const bool colour : { false, true }) {
				PipelineDesc desc(a_zLayout, colour ? a_program->vertex : a_program->depthVertex, colour ? a_program->pixel : a_program->depthPixel, a_targets.depth);
				desc.raster.rs.cull = rhi::CullMode::None;  // the engine draws tree LOD with culling off (cull mode 0) in both passes
				desc.raster.rs.frontCCW = a_frontCCW;
				desc.depth.ds.depthEnable = true;
				desc.depth.ds.depthWrite = !colour;
				desc.depth.ds.depthFunc = colour ? rhi::CompareOp::Equal : rhi::CompareOp::Less;
				if (colour) {
					desc.blend.bs = a_blend;
					desc.Colours(a_targets);
				}
				desc.Create(a_device, colour ? built->colour : built->depth, colour ? "tree LOD colour" : "tree LOD depth");
			}
			return built;
		}

		/**
		 * @brief The refraction pipeline: one attachment (FUN_1414b4860 binds kREFRACTION_NORMALS alone as RT0), main depth LESS_EQUAL
		 * without writes (its depth mode 1), write mode 2's mask, no culling: the pixel stage discards one-sided back faces itself.
		 */
		static org::services::PipelinePayload BuildRefraction(rhi::Device a_device, rhi::PipelineLayoutHandle a_layout, const ShaderPrograms::ShadowProgram* a_program,
			DXGI_FORMAT a_format, DXGI_FORMAT a_depthFormat, std::uint8_t a_writeMask, bool a_frontCCW)
		{
			auto stages = Parse<2>({ &a_program->vertex, &a_program->pixel });
			CheckBindings(stages[0], false);
			CheckBindings(stages[1], true);
			auto built = std::make_shared<rhi::PipelinePtr>();
			PipelineDesc desc(a_layout, a_program->vertex, a_program->pixel, a_depthFormat);
			desc.raster.rs.cull = rhi::CullMode::None;
			desc.raster.rs.frontCCW = a_frontCCW;
			desc.depth.ds.depthEnable = true;
			desc.depth.ds.depthWrite = false;
			desc.depth.ds.depthFunc = rhi::CompareOp::LessEqual;
			desc.blend.bs.attachments[0].enable = false;
			desc.blend.bs.attachments[0].writeMask = static_cast<rhi::ColorWriteEnable>(a_writeMask & 0xF);
			desc.Colour(a_format);
			desc.Create(a_device, *built, "refraction");
			return built;
		}

		/** @brief A forward pipeline. A second target (first-person motion vectors) is written unblended: those members are opaque. */
		static org::services::PipelinePayload BuildForward(rhi::Device a_device, rhi::PipelineLayoutHandle a_zLayout, const ShaderPrograms::ForwardProgram* a_program,
			ForwardTargets a_targets, rhi::CullMode a_cull, bool a_frontCCW)
		{
			auto stages = Parse<2>({ &a_program->vertex, &a_program->pixel });
			CheckPulledBindings(stages[0], false);
			CheckPulledBindings(stages[1], true);
			auto built = std::make_shared<rhi::PipelinePtr>();
			PipelineDesc desc(a_zLayout, a_program->vertex, a_program->pixel, a_targets.depth);
			desc.raster.rs.cull = a_cull;
			desc.raster.rs.frontCCW = a_frontCCW;
			// The engine's depth mode 3 for the face's LOD: test and write, LESS_EQUAL.
			desc.depth.ds.depthEnable = true;
			desc.depth.ds.depthWrite = true;
			desc.depth.ds.depthFunc = rhi::CompareOp::LessEqual;
			// No blending, every channel written (the engine's write mask there is stale state; the cube's alpha is not read).
			desc.Colour(a_targets.colour);
			if (a_targets.colour2 != DXGI_FORMAT_UNKNOWN)
				desc.Colour(a_targets.colour2);
			desc.Create(a_device, *built, desc.targets.rt.count > 1 ? "first-person forward" : "forward");
			return built;
		}

		/**
		 * @brief A ForwardOrDecal family key's pipelines (Api::ProgramBuild::ForwardOrDecal: the Effect port's keys), following Build's
		 * forward or decal rules (a decal uses the DEFERRED pixel stage). An attachment the pixel stage does not output keeps its contents
		 * (write mask 0), as DXVK builds the engine's pipeline: DEFERRED without Normals/MotionVectorsNormals writes neither. Its Z key is
		 * never shared (a_slot's salt); the depth variant of a forward key is never drawn (no Z-prepass bucket: PlanZBuckets). Named by
		 * a_keyName ("effect color", "effect depth").
		 */
		static org::services::PipelinePayload BuildForwardOrDecal(const ProgramFamilySlot& a_slot, std::string_view a_keyName, rhi::Device a_device,
			rhi::PipelineLayoutHandle a_layout, PipelineKey a_key, const ShaderPrograms::Program* a_program, TargetFormats a_targets, EngineState a_state, bool a_frontCCW)
		{
			auto [vertex, pixel] = Parse<2>({ &a_program->vertex, &a_program->pixel });
			const std::uint32_t decalGroup = RasterDecalGroup(a_key.rasterFlags);
			if (!a_state.valid || (!RasterForward(a_key.rasterFlags) && decalGroup != 1 && decalGroup != 2))
				throw std::runtime_error(fmt::format("an {} key without the forward or a decal group's state", a_keyName));
			CheckBindings(vertex, false);
			CheckBindings(pixel, true);
			auto built = std::make_shared<Built>();
			for (const auto variant : { kColorVariant, kDepthVariant }) {
				AddUsage(vertex, false, built->usage[variant]);
				AddUsage(pixel, true, built->usage[variant]);
			}
			built->tables = std::make_shared<const ConstantTables>(ConstantTables{ ConstantTableOf({ &vertex }, false, a_key), ConstantTableOf({ &pixel }, true, a_key) });
			built->zKey.vertex = ModuleHash(a_program->vertex) ^ a_slot.zSalt;  // no Lighting pipeline's
			built->zKey.vertexBytes = static_cast<std::uint32_t>(a_program->vertex.size());
			built->zKey.pixel = ModuleHash(a_program->pixel);
			built->zKey.pixelBytes = static_cast<std::uint32_t>(a_program->pixel.size());
			const bool test = (a_key.rasterFlags & kRasterForwardDepthTest) != 0, write = (a_key.rasterFlags & kRasterForwardDepthWrite) != 0;
			for (std::uint32_t variant = 0; variant < kVariantCount; ++variant) {
				const bool depthOnly = variant == kDepthVariant;
				PipelineDesc desc(a_layout, a_program->vertex, a_program->pixel, a_targets.depth);
				desc.input.il = BuildInputLayout(vertex, a_key.vertexLayout);
				desc.flags.flags = rhi::PipelineFlags_IndirectBindable;
				desc.raster.rs.cull = (a_key.rasterFlags & kRasterTwoSided) ? rhi::CullMode::None : rhi::CullMode::Back;
				desc.raster.rs.frontCCW = a_frontCCW;
				auto& depth = desc.depth.ds;
				depth.depthWrite = !depthOnly && write;
				depth.depthEnable = depthOnly || test || write;
				depth.depthFunc = (depthOnly || test) ? rhi::CompareOp::LessEqual : rhi::CompareOp::Always;
				// Decal rules: bias on the colour variant and on the opaque group's depth variant; LESS_EQUAL; only the opaque group's depth
				// variant writes depth. A first-person effect decal drawn forward (Coordinator::ViewDecalForward) keeps its group's bias.
				if (decalGroup ? (!depthOnly || decalGroup == 1) : (!depthOnly && RasterDepthBiasMode(a_key.rasterFlags))) {
					desc.raster.rs.depthBias = a_state.depthBias;
					desc.raster.rs.depthBiasClamp = a_state.depthBiasClamp;
					desc.raster.rs.slopeScaledDepthBias = a_state.slopeScaledDepthBias;
				}
				if (decalGroup) {
					depth.depthEnable = true;
					depth.depthWrite = depthOnly && decalGroup == 1;
					depth.depthFunc = rhi::CompareOp::LessEqual;
				}
				if (!depthOnly) {
					desc.blend.bs = a_state.blend;
					desc.Colours(a_targets);
					for (std::uint32_t i = decalGroup ? 0u : 1u; i < a_targets.colorCount && i < 8; ++i) {
						if (decalGroup && ((pixel.outputLocations >> i) & 1))
							continue;
						desc.blend.bs.attachments[i].enable = false;
						desc.blend.bs.attachments[i].writeMask = static_cast<rhi::ColorWriteEnable>(0);
					}
				}
				desc.Create(a_device, built->pipelines[variant], fmt::format("{} {}", a_keyName, depthOnly ? "depth" : "color"));
			}
			return built;
		}

		static org::services::PipelinePayload Build(rhi::Device a_device, rhi::PipelineLayoutHandle a_layout, rhi::PipelineLayoutHandle a_zLayout, PipelineKey a_key,
			const ShaderPrograms::Program* a_program, TargetFormats a_targets, EngineState a_state, bool a_frontCCW)
		{
			// A program family's key: its build rule (Api::ProgramBuild; ForwardOrDecal, the one there is).
			if (const auto family = KeyFamily(a_key.rasterFlags); family != Api::ProgramFamily::Count)
				return BuildForwardOrDecal(ProgramFamilySlotOf(family), Extensions::ProgramFamilyOf(family).keyName, a_device, a_layout, a_key, a_program, a_targets, a_state,
					a_frontCCW);
			auto [vertex, pixel, depthPixel, pulledVertex, pulledDepthPixel] =
				Parse<5>({ &a_program->vertex, &a_program->pixel, &a_program->depthPixel, &a_program->pulledVertex, &a_program->pulledDepthPixel });
			CheckBindings(vertex, false);
			CheckBindings(pixel, true);
			CheckBindings(depthPixel, true);
			CheckPulledBindings(pulledVertex, false);
			CheckPulledBindings(pulledDepthPixel, true);
			auto built = std::make_shared<Built>();
			// CS_DCLF_FOLIAGE_PARITY: terrain LOD's depth stage records its pixels (DCLF_OWNED_PARITY), so it keeps its pixel stage.
			const std::uint32_t technique = (a_key.passDescriptor >> 24) & 0x3f;
			const bool pulledPixel = FragmentDefersDepth(a_program->pulledDepthPixel) || (Policy::RecordFoliagePixels() && (technique == 9 || technique == 18));
			AddUsage(vertex, false, built->usage[kColorVariant]);
			AddUsage(pixel, true, built->usage[kColorVariant]);
			AddUsage(vertex, false, built->usage[kDepthVariant]);
			AddUsage(depthPixel, true, built->usage[kDepthVariant]);
			built->tables = std::make_shared<const ConstantTables>(ConstantTables{ ConstantTableOf({ &vertex }, false, a_key), ConstantTableOf({ &pixel, &depthPixel }, true, a_key) });
			// Decal groups (engine notes, "Decals"): an opaque-group decal (hint 2) writes depth in the decal depth pass, before the colour
			// pass, with the colour variant's bias and test. A blended decal writes none (depth mode 1). A multi-index layer (group 3, the
			// engine's group 2: depth mode 3) writes depth from its colour draw after its host is shaded; its depth variant is never drawn.
			const std::uint32_t decalGroup = a_state.valid ? RasterDecalGroup(a_key.rasterFlags) : 0u;
			// A terrain group's key carries the engine's blend (PackTerrainRasterFlags) but no decal depth: an opaque depth variant without
			// bias; the colour variant tests as Terrain Blending's replay: group 1 LESS_EQUAL with writes (terrainDepthStencilState),
			// group 2 EQUAL.
			const std::uint32_t terrainGroup = RasterTerrainGroup(a_key.rasterFlags);
			const bool engineDepth = a_state.valid && terrainGroup == 0;
			// A forward key (kRasterForward) draws after the deferred composite on the colour pass's eight attachments (so it lives in the
			// main set): the main target alone, blended per the engine's state, the G-buffer masked off (unbound in the engine); depth per
			// the property (SetupGeometry: kZBufferTest, kZBufferWrite). Its depth variant is never drawn.
			const bool forward = a_state.valid && RasterForward(a_key.rasterFlags);
			for (std::uint32_t variant = 0; variant < kVariantCount; ++variant) {
				const bool depthOnly = variant == kDepthVariant;
				PipelineDesc desc(a_layout, a_program->vertex, depthOnly ? a_program->depthPixel : a_program->pixel, a_targets.depth);
				desc.input.il = BuildInputLayout(vertex, a_key.vertexLayout);
				desc.flags.flags = rhi::PipelineFlags_IndirectBindable;
				auto& raster = desc.raster.rs;
				raster.cull = (a_key.rasterFlags & kRasterTwoSided) ? rhi::CullMode::None : rhi::CullMode::Back;
				// A decal's engine depth bias. The integer DepthBias passes through as the constant factor: for D24S8 Vulkan's default
				// representation is the format's least representable value, which is what D3D11 (and DXVK, for UNORM formats) means by it.
				if (engineDepth && (!depthOnly || decalGroup == 1)) {
					raster.depthBias = a_state.depthBias;
					raster.depthBiasClamp = a_state.depthBiasClamp;
					raster.slopeScaledDepthBias = a_state.slopeScaledDepthBias;
				}
				// The engine's winding (EngineFrontCCW); RasterState::frontCCW means the same on every backend.
				raster.frontCCW = a_frontCCW;
				auto& depth = desc.depth.ds;
				depth.depthEnable = true;
				// The colour variant tests EQUAL against DCLF's Z-prepass: where the prepass's alpha test discarded (foliage cards), LESS_EQUAL
				// would pass those fragments and run full lighting before they discard. A decal keeps LESS_EQUAL: its depth bias never passes EQUAL.
				depth.depthWrite = depthOnly ? decalGroup != 2 : (decalGroup == 3 || terrainGroup == 1);
				depth.depthFunc = depthOnly ? (engineDepth ? rhi::CompareOp::LessEqual : rhi::CompareOp::Less) :
				                              ((engineDepth || terrainGroup == 1) ? rhi::CompareOp::LessEqual : rhi::CompareOp::Equal);
				if (forward) {
					const bool test = (a_key.rasterFlags & kRasterForwardDepthTest) != 0, write = (a_key.rasterFlags & kRasterForwardDepthWrite) != 0;
					depth.depthWrite = !depthOnly && write;
					depth.depthEnable = depthOnly || test || write;
					depth.depthFunc = (depthOnly || test) ? rhi::CompareOp::LessEqual : rhi::CompareOp::Always;
				}
				if (!depthOnly) {
					// A key with engine state blends exactly as the engine's state says, write masks included; others don't blend.
					if (a_state.valid || a_state.blendOnly)
						desc.blend.bs = a_state.blend;
					desc.Colours(a_targets);
					if (forward)
						for (std::uint32_t i = 1; i < a_targets.colorCount && i < 8; ++i) {
							desc.blend.bs.attachments[i].enable = false;
							desc.blend.bs.attachments[i].writeMask = static_cast<rhi::ColorWriteEnable>(0);
						}
				}
				desc.Create(a_device, built->pipelines[variant], depthOnly ? "depth" : "color");
				if (!depthOnly)
					continue;
				// The Z-prepass plain-draw pipeline: the depth variant's state, pulled stages, no vertex input, bound per call.
				auto& zKey = built->zKey;
				zKey.vertex = ModuleHash(a_program->pulledVertex);
				zKey.vertexBytes = static_cast<std::uint32_t>(a_program->pulledVertex.size());
				zKey.pixel = pulledPixel ? ModuleHash(a_program->pulledDepthPixel) : 0;
				zKey.pixelBytes = pulledPixel ? static_cast<std::uint32_t>(a_program->pulledDepthPixel.size()) : 0;
				zKey.cull = static_cast<std::uint32_t>(raster.cull);
				zKey.depthWrite = depth.depthWrite ? 1u : 0u;
				zKey.depthFunc = static_cast<std::uint32_t>(depth.depthFunc);
				zKey.depthBias = std::bit_cast<std::uint32_t>(static_cast<float>(raster.depthBias));
				zKey.depthBiasClamp = std::bit_cast<std::uint32_t>(raster.depthBiasClamp);
				zKey.slopeScaledDepthBias = std::bit_cast<std::uint32_t>(raster.slopeScaledDepthBias);
				zKey.terrainNear = terrainGroup == 1 ? 1u : 0u;
				PipelineDesc pulled(a_zLayout, a_program->pulledVertex, pulledPixel ? std::span<const std::byte>(a_program->pulledDepthPixel) : std::span<const std::byte>{},
					a_targets.depth);
				pulled.raster = desc.raster;
				pulled.depth = desc.depth;
				pulled.blend = desc.blend;
				pulled.targets = desc.targets;
				pulled.Create(a_device, built->pulledDepth, "pulled depth");
			}
			return built;
		}

		/**
		 * @brief One shadow pipeline: the Utility permutation, depth only (write, LESS, as the engine's shadow pass), with the view's
		 * bias, cull and winding. A two-sided caster draws without culling, as the engine's Utility pass does.
		 */
		static org::services::PipelinePayload BuildShadow(rhi::Device a_device, rhi::PipelineLayoutHandle a_layout, ShadowPipelineKey a_key,
			const ShaderPrograms::ShadowProgram* a_program, DXGI_FORMAT a_depthFormat, ShadowRasterState a_state)
		{
			auto [vertex, pixel] = Parse<2>({ &a_program->vertex, &a_program->pixel });
			CheckBindings(vertex, false);
			CheckBindings(pixel, true);
			auto built = std::make_shared<Built>();
			AddUsage(vertex, false, built->usage[kColorVariant]);
			AddUsage(pixel, true, built->usage[kColorVariant]);
			PipelineDesc desc(a_layout, a_program->vertex, a_program->pixel, a_depthFormat);
			// The pulling vertex stage (DCLF_PULLED) has no vertex inputs; the pipeline is bound by its draws, not from an execution set.
			desc.input.il = BuildInputLayout(vertex, a_key.vertexLayout);
			desc.raster.rs.cull = (a_key.rasterFlags & kRasterTwoSided) ? rhi::CullMode::None : a_state.cull;
			desc.raster.rs.frontCCW = a_state.frontCCW;
			// The integer DepthBias passes through as the constant factor: the shadow maps are D16_UNORM, where Vulkan's default
			// representation (the least representable value) is what D3D11 and DXVK mean by it.
			desc.raster.rs.depthBias = a_state.depthBias;
			desc.raster.rs.depthBiasClamp = a_state.depthBiasClamp;
			desc.raster.rs.slopeScaledDepthBias = a_state.slopeScaledDepthBias;
			desc.depth.ds.depthEnable = true;
			desc.depth.ds.depthWrite = true;
			desc.depth.ds.depthFunc = rhi::CompareOp::Less;
			desc.Create(a_device, built->pipelines[kColorVariant], "shadow");
			built->discards = FragmentDefersDepth(a_program->pixel);
			return built;
		}

		// DrawSequence's command signature (Records.h), created with the set it selects from.
		bool CreateSignature(SetVersion& a_version, std::uint32_t a_variant)
		{
			rhi::IndirectArg args[6]{};
			args[0].kind = rhi::IndirectArgKind::PipelineIndex;
			args[1].kind = rhi::IndirectArgKind::Constant;
			args[1].u.rootConstants = { 0, 0, kDrawPushArgumentWords };  // the rows' addresses and the object word -> the layout's push data
			args[2].kind = rhi::IndirectArgKind::VertexBuffer;
			args[2].u.vertexBuffer.slot = 0;
			args[3].kind = rhi::IndirectArgKind::VertexBuffer;  // a face shape's positions (DrawSequence::streamBuffer*)
			args[3].u.vertexBuffer.slot = 1;
			args[4].kind = rhi::IndirectArgKind::IndexBuffer;
			args[5].kind = rhi::IndirectArgKind::DrawIndexed;
			rhi::CommandSignatureDesc desc{};
			desc.args = { args, 6 };
			desc.byteStride = sizeof(DrawSequence);
			desc.pipelineSet = a_version.sets[a_variant]->GetHandle();
			desc.explicitPreprocess = true;
			return device.CreateCommandSignature(desc, layout->GetHandle(), a_version.signatures[a_variant]) == rhi::Result::Ok;
		}

		/** @brief Writes [a_applied, a_admitted) into a_set, creating it (initial pipeline 0) and its signatures if absent. */
		template <class GetPipeline, class CreateSignatures>
		bool ApplyToSet(rhi::IndirectPipelineSetPtr& a_set, std::uint32_t a_applied, std::uint32_t a_admitted, const char* a_name, GetPipeline&& a_pipeline,
			CreateSignatures&& a_createSignatures)
		{
			std::uint32_t from = a_applied;
			if (!a_set) {
				if (device.CreateIndirectPipelineSet(rhi::IndirectPipelineSetDesc{ a_pipeline(0), kMaxPipelines }, a_set) != rhi::Result::Ok)
					return false;
				a_set->SetName(a_name);
				if (!a_createSignatures())
					return false;
				from = 1;
			}
			if (from >= a_admitted)
				return true;
			std::vector<rhi::PipelineHandle> pipelines;
			pipelines.reserve(a_admitted - from);
			for (std::uint32_t i = from; i < a_admitted; ++i)
				pipelines.push_back(a_pipeline(i));
			return device.UpdateIndirectPipelineSet(a_set->GetHandle(), from, { pipelines.data(), static_cast<std::uint32_t>(pipelines.size()) }) ==
			       rhi::Result::Ok;
		}
	};

	DrawPipelines::DrawPipelines() :
		impl(std::make_unique<Impl>())
	{
		// Named once this is whole (a pass that runs meanwhile waits for Get's construction).
		SetPipelineLaneDrain(&Impl::DrainLaneThunk);
	}

	DrawPipelines::~DrawPipelines() = default;

	DrawPipelines& DrawPipelines::Get()
	{
		static DrawPipelines pipelines;
		return pipelines;
	}

	bool DrawPipelines::Enabled() const
	{
		return impl->Initialize() && impl->supported;
	}

	void DrawPipelines::SetTargetFormats(const TargetFormats& a_formats)
	{
		// A draw with nothing bound (seen while the game shuts down) is not the main pass.
		if (a_formats == targets || a_formats.colorCount == 0 || a_formats.depth == DXGI_FORMAT_UNKNOWN)
			return;
		if (HasTargetFormats()) {
			auto describe = [](const TargetFormats& a_formats) {
				std::string text;
				for (std::uint32_t i = 0; i < a_formats.colorCount; ++i)
					text += fmt::format("{} ", static_cast<int>(a_formats.colors[i]));
				return text + fmt::format("depth {}", static_cast<int>(a_formats.depth));
			};
			// The lane rebuilds (RecreateMainSet); until the frame's catalog catches up, GetIndirectState and TreeLodPipelinesOf give none.
			logger::info("[DCLF] Main pass targets changed ({} -> {}); the indirect pipelines and tree LOD's are built again", describe(targets), describe(a_formats));
			++stats.targetChanges;
			++generation;
		}
		targets = a_formats;
		if (Enabled())
			impl->PostInput(*this);
	}

	void DrawPipelines::RequestLighting(const PipelineKey& a_key, std::uint32_t a_slot)
	{
		impl->completions->requests.Push({ a_key, a_slot });
		WakePipelineLane();
	}

	void DrawPipelines::RequestTreeLod()
	{
		if (impl->treeLodAsked)
			return;
		impl->treeLodAsked = true;
		impl->completions->treeLodRequest.store(true, std::memory_order_release);
		WakePipelineLane();
	}

	void DrawPipelines::SetTreeLodInputs(RE::BSShader& a_distantTree)
	{
		auto& input = impl->renderInput;
		if (input.distantTree == &a_distantTree || !Enabled())
			return;
		input.distantTree = &a_distantTree;
		impl->PostInput(*this);
	}

	void DrawPipelines::RequestRefraction()
	{
		if (impl->refractionAsked)
			return;
		impl->refractionAsked = true;
		impl->completions->refractionRequest.store(true, std::memory_order_release);
		WakePipelineLane();
	}

	void DrawPipelines::SetRefractionInputs(RE::BSShader& a_utility, DXGI_FORMAT a_format, std::uint8_t a_writeMask)
	{
		auto& input = impl->renderInput;
		if ((input.refractionUtility == &a_utility && input.refractionFormat == a_format && input.refractionWriteMask == a_writeMask) || !Enabled())
			return;
		input.refractionUtility = &a_utility;
		input.refractionFormat = a_format;
		input.refractionWriteMask = a_writeMask;
		impl->PostInput(*this);
	}

	void DrawPipelines::RequestForward(const ForwardPipelineKey& a_key)
	{
		if (!impl->forwardAsked.insert(a_key).second)
			return;
		impl->completions->forwardRequests.Push(a_key);
		WakePipelineLane();
	}

	void DrawPipelines::SetForwardTargets(const ForwardTargets& a_targets)
	{
		auto& input = impl->renderInput;
		if (a_targets.colour == DXGI_FORMAT_UNKNOWN || a_targets.depth == DXGI_FORMAT_UNKNOWN || input.forwardTargets == a_targets || !Enabled())
			return;
		input.forwardTargets = a_targets;
		impl->PostInput(*this);
	}

	void DrawPipelines::SetFirstPersonTargets(const ForwardTargets& a_targets)
	{
		auto& input = impl->renderInput;
		if (a_targets.colour == DXGI_FORMAT_UNKNOWN || a_targets.depth == DXGI_FORMAT_UNKNOWN || input.firstPersonTargets == a_targets || !Enabled())
			return;
		input.firstPersonTargets = a_targets;
		impl->PostInput(*this);
	}

	std::shared_ptr<const PipelineCatalog> DrawPipelines::TakeCatalog()
	{
		return std::shared_ptr<const PipelineCatalog>(impl->catalogSlot.Take());
	}

	DrawPipelines::Stats DrawPipelines::GetStats() const
	{
		Stats result = stats;
		const auto& counters = impl->lane.counters;
		result.requested = counters.requested.load(std::memory_order_relaxed);
		result.ready = counters.ready.load(std::memory_order_relaxed);
		result.failed = counters.failed.load(std::memory_order_relaxed);
		result.zPipelines = counters.zPipelines.load(std::memory_order_relaxed);
		result.zDepthOnly = counters.zDepthOnly.load(std::memory_order_relaxed);
		result.setPublishes = counters.setPublishes.load(std::memory_order_relaxed);
		result.setWaits = counters.setWaits.load(std::memory_order_relaxed);
		for (std::size_t f = 0; f < Extensions::kProgramFamilies; ++f) {
			const auto& programCounters = impl->lane.programFamilyCounters[f];
			result.families[f] = { programCounters.requested.load(std::memory_order_relaxed), programCounters.ready.load(std::memory_order_relaxed),
				programCounters.failed.load(std::memory_order_relaxed) };
		}
		const auto& shadowCounters = impl->lane.shadow.counters;
		result.shadowRequested = shadowCounters.requested.load(std::memory_order_relaxed);
		result.shadowReady = shadowCounters.ready.load(std::memory_order_relaxed);
		result.shadowFailed = shadowCounters.failed.load(std::memory_order_relaxed);
		result.shadowSetPublishes = shadowCounters.setPublishes.load(std::memory_order_relaxed);
		result.shadowSetWaits = shadowCounters.setWaits.load(std::memory_order_relaxed);
		const auto& treeCounters = impl->lane.treeLod.counters;
		result.treeLodRequested = treeCounters.requested.load(std::memory_order_relaxed);
		result.treeLodReady = treeCounters.ready.load(std::memory_order_relaxed);
		result.treeLodFailed = treeCounters.failed.load(std::memory_order_relaxed);
		const auto& forwardCounters = impl->lane.forward.counters;
		result.forwardRequested = forwardCounters.requested.load(std::memory_order_relaxed);
		result.forwardReady = forwardCounters.ready.load(std::memory_order_relaxed);
		result.forwardFailed = forwardCounters.failed.load(std::memory_order_relaxed);
		return result;
	}

	std::span<const std::string_view> DrawPipelines::LightingVariableNames(bool a_pixel)
	{
		return LightingNames(a_pixel);
	}

	void DrawPipelines::RequestShadow(const ShadowPipelineKey& a_viewKey, const ShadowPipelineKey& a_casterKey, std::uint32_t a_occlusion)
	{
		impl->completions->shadowRequests.Push({ a_viewKey, a_casterKey, a_occlusion });
		WakePipelineLane();
	}

	void DrawPipelines::SetShadowInputs(DXGI_FORMAT a_format, RE::BSShader& a_utility)
	{
		auto& input = impl->renderInput;
		// The view format of a shadow map the engine has drawn: unknown until then, and not posted.
		if (a_format == DXGI_FORMAT_UNKNOWN || (input.shadowFormat == a_format && input.utility == &a_utility) || !Enabled())
			return;
		input.shadowFormat = a_format;
		input.utility = &a_utility;
		impl->PostInput(*this);
	}

	namespace
	{
		// Two keys' differing fields ("name a -> b") and differing bit count.
		struct KeyDifference
		{
			std::uint32_t bits = 0;
			std::string fields;
			void Field(const char* a_name, std::uint64_t a_from, std::uint64_t a_to)
			{
				if (a_from == a_to)
					return;
				bits += static_cast<std::uint32_t>(std::popcount(a_from ^ a_to));
				fields += fmt::format("{}{} {:X} -> {:X} (bits {:X})", fields.empty() ? "" : ", ", a_name, a_from, a_to, a_from ^ a_to);
			}
		};
	}

	std::string DrawPipelines::Impl::NearestKey(const PipelineKey& a_key) const
	{
		const PipelineKey* nearest = nullptr;
		KeyDifference best;
		for (const auto& [key, entry] : lane.entries) {
			if (key == a_key)
				continue;
			KeyDifference difference;
			difference.Field("vertex", key.vertexDescriptor, a_key.vertexDescriptor);
			difference.Field("pixel", key.pixelDescriptor, a_key.pixelDescriptor);
			difference.Field("pass", key.passDescriptor, a_key.passDescriptor);
			difference.Field("raster", key.rasterFlags, a_key.rasterFlags);
			difference.Field("vertex layout", key.vertexLayout, a_key.vertexLayout);
			if (!nearest || difference.bits < best.bits) {
				nearest = &key;
				best = std::move(difference);
			}
		}
		if (!nearest)
			return lane.recreatedBy.empty() ? std::string("the first pipeline") : fmt::format("no other pipeline since the set was recreated ({})", lane.recreatedBy);
		return fmt::format("nearest requested pipeline differs in {}", best.fields);
	}

	std::string DrawPipelines::Impl::NearestShadowKey(const ShadowPipelineKey& a_key) const
	{
		const ShadowPipelineKey* nearest = nullptr;
		KeyDifference best;
		for (const auto& [key, entry] : lane.shadow.entries) {
			if (key == a_key)
				continue;
			KeyDifference difference;
			difference.Field("technique", key.technique, a_key.technique);
			difference.Field("raster", key.rasterFlags, a_key.rasterFlags);
			difference.Field("vertex layout", key.vertexLayout, a_key.vertexLayout);
			difference.Field("view state", key.viewState, a_key.viewState);
			if (!nearest || difference.bits < best.bits) {
				nearest = &key;
				best = std::move(difference);
			}
		}
		if (!nearest)
			return lane.shadow.recreatedBy.empty() ? std::string("the first shadow pipeline") :
			                                         fmt::format("no other shadow pipeline since the set was recreated ({})", lane.shadow.recreatedBy);
		return fmt::format("nearest requested shadow pipeline differs in {}", best.fields);
	}

	namespace
	{
		/** @brief A shadow request's casters, for the on-demand warnings. */
		std::string DescribeShadowCasters(const ShadowPipelineKey& a_casterKey, std::uint32_t a_occlusion)
		{
			return fmt::format("for the casters of key technique {:08X}, raster {:X}, vertex layout {:016X} ({})", a_casterKey.technique, a_casterKey.rasterFlags,
				a_casterKey.vertexLayout, a_occlusion == ~0u ? std::string("the shadow views") : fmt::format("occlusion view {}", a_occlusion));
		}
	}

	void DrawPipelines::Impl::DrainLaneThunk()
	{
		DrawPipelines::Get().impl->DrainLane();
	}

	void DrawPipelines::Impl::DrainLane()
	{
		ZoneScopedN("CS.DCLF.PipelineLane");
		auto& l = lane;
		auto& s = l.shadow;
		auto& f = l.forward;
		// New inputs: new targets restart the main set and tree LOD, a new shadow format the shadow set, new forward targets the forward
		// pipelines; anything new may unpark keys waiting on an input.
		bool inputsChanged = false;
		if (auto input = inputSlot.Take()) {
			if (l.input && l.input->targets.colorCount != 0 && !(l.input->targets == input->targets))
				RecreateMainSet(l.input->targets, input->targets);
			if (l.input && l.input->shadowFormat != DXGI_FORMAT_UNKNOWN && input->shadowFormat != l.input->shadowFormat)
				RecreateShadowSet(l.input->shadowFormat, input->shadowFormat);
			if (l.input && l.input->forwardTargets.colour != DXGI_FORMAT_UNKNOWN && !(l.input->forwardTargets == input->forwardTargets))
				RecreateForward(l.input->forwardTargets, input->forwardTargets);
			// New first-person targets (rare) rebuild every forward pipeline too.
			else if (l.input && l.input->firstPersonTargets.colour != DXGI_FORMAT_UNKNOWN && !(l.input->firstPersonTargets == input->firstPersonTargets))
				RecreateForward(l.input->firstPersonTargets, input->firstPersonTargets);
			if (l.input && (l.input->refractionFormat != input->refractionFormat || l.input->refractionWriteMask != input->refractionWriteMask))
				l.refraction.Reset();
			l.input = std::shared_ptr<const FrameInput>(std::move(input));
			inputsChanged = true;
			l.dirty = true;  // the catalog names the targets' generation, the shadow format and the forward targets
		}
		// Each key once (the lane keeps them and rebuilds them after a target or format change).
		completions->requests.Drain([&](LightingRequest&& a_request) {
			if (l.asked.try_emplace(a_request.key, a_request.slot).second)
				TryRequest(a_request.key);
		});
		completions->shadowRequests.Drain([&](ShadowRequest&& a_request) {
			if (s.asked.try_emplace(a_request.viewKey, a_request).second)
				TryRequestShadow(a_request.viewKey);
		});
		completions->forwardRequests.Drain([&](ForwardPipelineKey&& a_key) {
			if (f.asked.insert(a_key).second)
				TryRequestForward(a_key);
		});
		if (completions->treeLodRequest.exchange(false, std::memory_order_acq_rel))
			l.treeLod.asked = true;
		if (completions->refractionRequest.exchange(false, std::memory_order_acq_rel))
			l.refraction.asked = true;
		if (inputsChanged) {
			l.RetryBlocked([&](const auto& a_key) { TryRequest(a_key); });
			s.RetryBlocked([&](const auto& a_key) { TryRequestShadow(a_key); });
			f.RetryBlocked([&](const auto& a_key) { TryRequestForward(a_key); });
		}
		// Tree LOD and refraction wait on nothing a list holds: tried every pass (cheap once requested, built or failed).
		TryRequestTreeLod();
		TryRequestRefraction();
		// Programs finished since the last pass: their keys go on to their pipelines. Loop until none: a program found complete at once
		// (every stage cached) finishes with no completion to wake the lane.
		auto& finished = l.finishedPrograms;
		for (;;) {
			finished.Clear();
			ShaderPrograms::Get().UpdateLane(finished);
			if (finished.Empty())
				break;
			// A family program's id is distinct from every Lighting one (FamilyProgramId); its keys wait in the same map.
			for (const auto& ids : finished.families)
				finished.lighting.insert(finished.lighting.end(), ids.begin(), ids.end());
			for (const std::uint64_t id : finished.lighting)
				l.RetryProgram(id, [&](const auto& a_key) { TryRequest(a_key); });
			for (const std::uint32_t technique : finished.shadow)
				s.RetryProgram(technique, [&](const auto& a_key) { TryRequestShadow(a_key); });
			for (const std::uint64_t id : finished.forward)
				f.RetryProgram(id, [&](const auto& a_key) { TryRequestForward(a_key); });
			if (finished.forwardTreeLod)
				f.RetryProgram(Lane::Forward::kTreeProgram, [&](const auto& a_key) { TryRequestForward(a_key); });
			if (finished.treeLod)
				TryRequestTreeLod();
			if (!finished.shadow.empty())
				TryRequestRefraction();
		}
		AdmitLane();
		AdmitShadow();
		AdmitTreeLodAndForward();
		if (l.dirty)
			PublishCatalog();
		// Completed builds become the service's active pipelines (a later request completes at once).
		service.PublishReady(0);
	}

	void DrawPipelines::Impl::TryRequest(const PipelineKey& a_key)
	{
		auto& l = lane;
		if (l.entries.contains(a_key))
			return;  // requested, or failed for good
		const auto* input = l.input.get();
		// A program family's key (Api::ProgramFamily: the Effect port's): its programs are its source's, of its family's shader.
		const auto family = KeyFamily(a_key.rasterFlags);
		const bool member = family != Api::ProgramFamily::Count;
		const auto f = static_cast<std::size_t>(family);
		if (!input || !input->lighting || (member && !input->familySource[f]) || input->targets.colorCount == 0 || input->targets.depth == DXGI_FORMAT_UNKNOWN ||
			!input->frontCCW || !input->states || !layout || !zLayout) {
			l.blocked.push_back(a_key);
			return;
		}
		const auto asked = l.asked.find(a_key);
		const std::uint32_t slot = asked != l.asked.end() ? asked->second : ~0u;
		// A build here is one the precompile missed: each is logged with its key, slot and nearest relative to find the gap.
		std::uint8_t onDemand = 0;
		bool programFailed = false;
		// A family key's program is its source's (Effect.hlsl for an effect key).
		const auto* program = member ? ShaderPrograms::Get().FindFamily(family, a_key, *static_cast<const RE::BSShader*>(input->familySource[f]), &programFailed) :
		                               ShaderPrograms::Get().Find(a_key, *input->lighting, &onDemand, &programFailed);
		if (onDemand)
			logger::warn("[DCLF] on-demand SPIR-V compile: Lighting {} that no precompile requested ({}), for pipeline slot {}: {}", ShaderPrograms::OnDemandStages(onDemand),
				SIE::ShaderCache::Instance().IsCompiling() ? "Community Shaders' compile workers still busy" : "Community Shaders' compile workers idle", slot,
				DescribeKey(a_key));
		if (programFailed) {
			// Its objects stay native (no set index ever comes).
			l.FailForGood(a_key);
			l.dirty = true;
			if (member)
				l.programFamilyCounters[f].failed.fetch_add(1, std::memory_order_relaxed);
			return;
		}
		if (!program) {
			l.programWaiters[member ? ShaderPrograms::FamilyProgramId(family, a_key) : ShaderPrograms::LightingProgramId(a_key)].push_back(a_key);
			return;
		}
		if (l.Full()) {
			l.blocked.push_back(a_key);
			return;
		}
		// The engine state behind the key's bits (CaptureEngineStates): asked for once, the key parked until the reply. An inexpressible
		// state keeps its objects native.
		const auto& states = *input->states;
		auto stateOf = [&](std::uint32_t a_bits) -> const EngineState* {
			if (const auto it = states.find(a_bits); it != states.end())
				return &it->second;
			if (l.statesAsked.insert(a_bits).second)
				completions->stateRequests.Push(a_bits);
			l.blocked.push_back(a_key);
			return nullptr;
		};
		EngineState state{};
		if (const auto bits = RasterStateBits(a_key.rasterFlags)) {
			const auto* found = stateOf(bits);
			if (!found || !found->valid)
				return;
			state = *found;
		} else {
			// An opaque key: the engine's blend state for its group's write mode.
			constexpr std::uint32_t kDoAlphaTest = 1u << 20;
			// LOD (LODLand 9, LODObjects 13, LODObjectHD 15, LODLandNoise 18) draws in write mode 1 even alpha-tested, as the engine's
			// main pass does (dclf-lod.md), so the G-buffer alphas keep what lies beneath.
			const std::uint32_t technique = (a_key.passDescriptor >> 24) & 0x3f;
			const bool lod = technique == 9 || technique == 13 || technique == 15 || technique == 18;
			const std::uint32_t writeMode = (a_key.pixelDescriptor & kDoAlphaTest) && !lod ? kAlphaTestedWriteMode : kOpaqueWriteMode;
			const auto* found = stateOf(writeMode << kRasterWriteModeShift);
			if (!found || !found->valid)
				return;
			state.blend = found->blend;
			state.blendOnly = true;
		}
		const auto& inputTargets = input->targets;
		org::services::PipelineRecipe recipe;
		recipe.id = fmt::format("dclf.{}.{:08X}.{:08X}.{:08X}.{:X}.{:016X}", member ? std::string_view(Extensions::ProgramFamilyOf(family).keyName) : std::string_view("lighting"),
			a_key.vertexDescriptor, a_key.pixelDescriptor, a_key.passDescriptor, a_key.rasterFlags, a_key.vertexLayout);
		recipe.shaderKey = PipelineKeyHash{}(a_key);
		recipe.fixedFunctionKey = ankerl::unordered_dense::detail::wyhash::hash(&inputTargets, sizeof(inputTargets));
		// The blend state too: its write masks are the engine's, so other masks are another pipeline.
		recipe.fixedFunctionKey ^= ankerl::unordered_dense::detail::wyhash::hash(&state.blend, sizeof(state.blend)) * 0x9E3779B97F4A7C15ull;
		recipe.build = [device = device, layout = layout->GetHandle(), zLayout = zLayout->GetHandle(), key = a_key, program, formats = inputTargets, state,
							frontCCW = *input->frontCCW] {
			return Build(device, layout, zLayout, key, program, formats, state, frontCCW);
		};
		l.entries.try_emplace(a_key);
		(void)service.Request(std::move(recipe), [completions = completions, key = a_key, generation = l.generation](const org::services::PipelineArtifact& a_artifact) {
			completions->main.Push({ key, generation, a_artifact });
			WakePipelineLane();
		});
		l.counters.requested.fetch_add(1, std::memory_order_relaxed);
		if (member)
			l.programFamilyCounters[f].requested.fetch_add(1, std::memory_order_relaxed);
		logger::warn("[DCLF] on-demand pipeline build: {} pipeline slot {}: {}; {}", member ? std::string_view(Extensions::ProgramFamilyOf(family).name) : std::string_view("Lighting"),
			slot, DescribeKey(a_key), NearestKey(a_key));
	}

	void DrawPipelines::Impl::RecreateMainSet(const TargetFormats& a_from, const TargetFormats& a_to)
	{
		auto& l = lane;
		auto describe = [](const TargetFormats& a_formats) {
			std::string text;
			for (std::uint32_t i = 0; i < a_formats.colorCount; ++i)
				text += fmt::format("{} ", static_cast<int>(a_formats.colors[i]));
			return text + fmt::format("depth {}", static_cast<int>(a_formats.depth));
		};
		// Every pipeline depends on the targets: start over (the new generation drops queued builds' completions).
		logger::info("[DCLF] pipeline lane: main pass targets {} -> {}; rebuilding {} indirect pipelines", describe(a_from), describe(a_to), l.setPipelines.size());
		l.StartOver();
		l.versions.Retire();
		l.setPipelines.clear();
		l.zGroupOf.clear();
		l.zGroupFirst.clear();
		l.zGroups.clear();
		l.usage.clear();
		l.constantTables.clear();
		for (auto& programCounters : l.programFamilyCounters)
			for (auto* counter : { &programCounters.requested, &programCounters.ready, &programCounters.failed })
				counter->store(0, std::memory_order_relaxed);
		l.recreatedBy = fmt::format("main pass targets {} -> {}", describe(a_from), describe(a_to));
		// Tree LOD depends on the targets too: rebuilt (TryRequestTreeLod), the last kept for the process.
		auto& t = l.treeLod;
		if (t.built)
			t.retired.push_back(std::move(t.built));
		t.built = {};
		t.requested = false;
		t.failed = t.programFailed;
		for (auto* counter : { &t.counters.requested, &t.counters.ready, &t.counters.failed })
			counter->store(0, std::memory_order_relaxed);
		t.counters.failed.store(t.programFailed ? 1u : 0u, std::memory_order_relaxed);
		// The refraction pipeline tests the main depth.
		l.refraction.Reset();
		l.dirty = true;
	}

	void DrawPipelines::Impl::AdmitLane()
	{
		auto& l = lane;
		// Completed builds, admitted in completion order; one from an earlier generation is dropped (its entry went with the set).
		completions->main.Drain([&](MainDone&& a_done) {
			if (a_done.generation != l.generation)
				return;
			const auto& key = a_done.key;
			const auto& artifact = a_done.artifact;
			const auto found = l.entries.find(key);
			if (found == l.entries.end())
				return;
			auto& entry = found->second;
			l.dirty = true;
			if (!artifact) {
				entry.failed = true;
				++l.failedEntries;
				l.counters.failed.fetch_add(1, std::memory_order_relaxed);
				if (const auto family = KeyFamily(key.rasterFlags); family != Api::ProgramFamily::Count)
					l.programFamilyCounters[static_cast<std::size_t>(family)].failed.fetch_add(1, std::memory_order_relaxed);
				if (l.loggedFailures++ < kMaxLoggedFailures)
					logger::warn("[DCLF] Indirect pipeline VS {:08X} PS {:08X} layout {:016X} failed: {}", key.vertexDescriptor, key.pixelDescriptor, key.vertexLayout, artifact.error);
				return;
			}
			const auto* built = static_cast<const Built*>(artifact.payload.get());
			entry.slot = static_cast<std::uint32_t>(l.setPipelines.size());
			const auto [group, added] = l.zGroups.try_emplace(built->zKey, static_cast<std::uint32_t>(l.zGroupFirst.size()));
			if (added) {
				l.zGroupFirst.push_back(entry.slot);
				l.counters.zPipelines.fetch_add(1, std::memory_order_relaxed);
				if (built->zKey.pixel == 0)
					l.counters.zDepthOnly.fetch_add(1, std::memory_order_relaxed);
			}
			l.zGroupOf.push_back(group->second);
			l.setPipelines.push_back(artifact.payload);
			l.usage.push_back(built->usage);
			l.constantTables.push_back(built->tables);
		});
		const auto admitted = static_cast<std::uint32_t>(l.setPipelines.size());
		if (!admitted)
			return;
		auto pipelineAt = [&](std::uint32_t a_variant) {
			return [&, a_variant](std::uint32_t a_index) {
				return static_cast<const Built*>(l.setPipelines[a_index].get())->pipelines[a_variant]->GetHandle();
			};
		};
		PublishAdmitted(l, l.versions, admitted, "indirect pipeline sets", [&](SetVersion& a_version) {
			for (std::uint32_t variant = 0; variant < kVariantCount; ++variant) {
				if (!ApplyToSet(a_version.sets[variant], a_version.applied, admitted, variant == kDepthVariant ? "DCLF indirect pipelines (depth)" : "DCLF indirect pipelines",
						pipelineAt(variant), [&] { return CreateSignature(a_version, variant); }))
					return false;
			}
			a_version.pipelines.assign(l.setPipelines.begin(), l.setPipelines.begin() + admitted);
			a_version.zGroupOf.assign(l.zGroupOf.begin(), l.zGroupOf.begin() + admitted);
			// A group's first pipeline is admitted before any other of it: the groups of [0, admitted) are a prefix.
			const auto groups = static_cast<std::size_t>(std::ranges::lower_bound(l.zGroupFirst, admitted) - l.zGroupFirst.begin());
			a_version.zGroupFirst.assign(l.zGroupFirst.begin(), l.zGroupFirst.begin() + static_cast<std::ptrdiff_t>(groups));
			return true;
		}, [&](const PipelineKey& a_key) {
			if (const auto family = KeyFamily(a_key.rasterFlags); family != Api::ProgramFamily::Count)
				l.programFamilyCounters[static_cast<std::size_t>(family)].ready.fetch_add(1, std::memory_order_relaxed);
		});
	}

	void DrawPipelines::Impl::TryRequestShadow(const ShadowPipelineKey& a_key)
	{
		auto& s = lane.shadow;
		if (s.entries.contains(a_key))
			return;  // requested, or failed for good
		// An inexpressible view rasterizer state has id 0: its views stay native.
		if (a_key.viewState == 0) {
			s.FailForGood(a_key);
			lane.dirty = true;
			return;
		}
		// The view's rasterizer state reaches the lane with the inputs after the render thread registers it.
		const auto* input = lane.input.get();
		if (!input || !input->utility || input->shadowFormat == DXGI_FORMAT_UNKNOWN || !input->shadowStates || a_key.viewState > input->shadowStates->size() ||
			!shadowLayout || !shadowDrawSignature) {
			s.blocked.push_back(a_key);
			return;
		}
		const auto asked = s.asked.find(a_key);
		const std::string casters = asked != s.asked.end() ? DescribeShadowCasters(asked->second.casterKey, asked->second.occlusion) : std::string("for no recorded caster");
		// As TryRequest: each build is logged with its casters and nearest relative.
		std::uint8_t onDemand = 0;
		bool programFailed = false;
		const auto* program = ShaderPrograms::Get().FindShadow(a_key.technique, *input->utility, true, nullptr, &onDemand, &programFailed);
		if (onDemand)
			logger::warn("[DCLF] on-demand SPIR-V compile: Utility {} of technique {:08X} that no precompile requested ({}), {}", ShaderPrograms::OnDemandStages(onDemand),
				a_key.technique, SIE::ShaderCache::Instance().IsCompiling() ? "Community Shaders' compile workers still busy" : "Community Shaders' compile workers idle",
				casters);
		if (programFailed) {
			// Its casters stay native in this key's views.
			s.FailForGood(a_key);
			lane.dirty = true;
			return;
		}
		if (!program) {
			s.programWaiters[a_key.technique].push_back(a_key);
			return;
		}
		if (s.Full()) {
			s.blocked.push_back(a_key);
			return;
		}
		const ShadowRasterState rasterState = (*input->shadowStates)[a_key.viewState - 1];
		const DXGI_FORMAT depthFormat = input->shadowFormat;
		org::services::PipelineRecipe recipe;
		recipe.id = fmt::format("dclf.shadow.{:08X}.{:X}.{:016X}.{}", a_key.technique, a_key.rasterFlags, a_key.vertexLayout, a_key.viewState);
		recipe.shaderKey = ShadowPipelineKeyHash{}(a_key);
		recipe.fixedFunctionKey = static_cast<std::uint64_t>(depthFormat);
		recipe.build = [device = device, layout = ShadowLayout(), key = a_key, program, depthFormat, rasterState] {
			return BuildShadow(device, layout, key, program, depthFormat, rasterState);
		};
		s.entries.try_emplace(a_key);
		(void)service.Request(std::move(recipe), [completions = completions, key = a_key, generation = s.generation](const org::services::PipelineArtifact& a_artifact) {
			completions->shadow.Push({ key, generation, a_artifact });
			WakePipelineLane();
		});
		s.counters.requested.fetch_add(1, std::memory_order_relaxed);
		logger::warn("[DCLF] on-demand pipeline build: shadow technique {:08X}, raster {:X}, vertex layout {:016X}, view state {}, format {}, {}; {}", a_key.technique,
			a_key.rasterFlags, a_key.vertexLayout, a_key.viewState, static_cast<int>(depthFormat), casters, NearestShadowKey(a_key));
	}

	void DrawPipelines::Impl::RecreateShadowSet(DXGI_FORMAT a_from, DXGI_FORMAT a_to)
	{
		auto& s = lane.shadow;
		// Every shadow pipeline depends on the format: start over (the new generation drops queued builds' completions).
		logger::info("[DCLF] pipeline lane: shadow map format {} -> {}; rebuilding {} shadow pipelines", static_cast<int>(a_from), static_cast<int>(a_to),
			s.setPipelines.size());
		s.StartOver();
		s.versions.Retire();
		s.setPipelines.clear();
		s.layouts.clear();
		s.discards.clear();
		s.recreatedBy = fmt::format("shadow map format {} -> {}", static_cast<int>(a_from), static_cast<int>(a_to));
		lane.dirty = true;
	}

	void DrawPipelines::Impl::AdmitShadow()
	{
		auto& s = lane.shadow;
		// As AdmitLane.
		completions->shadow.Drain([&](ShadowDone&& a_done) {
			if (a_done.generation != s.generation)
				return;
			const auto& key = a_done.key;
			const auto& artifact = a_done.artifact;
			const auto found = s.entries.find(key);
			if (found == s.entries.end())
				return;
			auto& entry = found->second;
			lane.dirty = true;
			if (!artifact) {
				entry.failed = true;
				++s.failedEntries;
				s.counters.failed.fetch_add(1, std::memory_order_relaxed);
				if (s.loggedFailures++ < kMaxLoggedFailures)
					logger::warn("[DCLF] shadow pipeline technique {:08X} layout {:016X} failed: {}", key.technique, key.vertexLayout, artifact.error);
				return;
			}
			const auto* built = static_cast<const Built*>(artifact.payload.get());
			entry.slot = static_cast<std::uint32_t>(s.setPipelines.size());
			s.setPipelines.push_back(artifact.payload);
			s.layouts.push_back(key.vertexLayout);
			s.discards.push_back(built->discards ? 1 : 0);
		});
		const auto admitted = static_cast<std::uint32_t>(s.setPipelines.size());
		if (!admitted)
			return;
		PublishAdmitted(s, s.versions, admitted, "shadow pipeline set", [&](ShadowSetVersion& a_version) {
			a_version.pipelines.assign(s.setPipelines.begin(), s.setPipelines.begin() + admitted);
			a_version.layouts.assign(s.layouts.begin(), s.layouts.begin() + admitted);
			a_version.discards.assign(s.discards.begin(), s.discards.begin() + admitted);
			return true;
		}, [](const ShadowPipelineKey&) {});
	}

	void DrawPipelines::Impl::TryRequestTreeLod()
	{
		auto& t = lane.treeLod;
		if (!t.asked || t.requested || t.built || t.failed)
			return;
		// What it lacks arrives with the frame's inputs or its program (Finished::treeLod); DrainLane retries each pass.
		const auto* input = lane.input.get();
		if (!input || !input->distantTree || input->targets.colorCount == 0 || input->targets.depth == DXGI_FORMAT_UNKNOWN || !input->frontCCW || !input->states ||
			!zLayout || !treeLodDrawSignature)
			return;
		// The engine draws tree LOD colour in write mode 1: its blend, read unasked by CaptureEngineStates. If inexpressible, tree
		// LOD stays native.
		const auto state = input->states->find(kOpaqueWriteMode << kRasterWriteModeShift);
		if (state == input->states->end())
			return;
		if (!state->second.valid) {
			t.failed = true;
			t.counters.failed.fetch_add(1, std::memory_order_relaxed);
			lane.dirty = true;
			logger::warn("[DCLF] tree LOD's pipelines cannot be built: the opaque write mode's state cannot be read; tree LOD stays native");
			return;
		}
		bool programFailed = false;
		const auto* program = ShaderPrograms::Get().FindTreeLod(*input->distantTree, &programFailed);
		if (programFailed) {
			t.failed = t.programFailed = true;
			t.counters.failed.fetch_add(1, std::memory_order_relaxed);
			lane.dirty = true;
			return;
		}
		if (!program)
			return;
		const auto& inputTargets = input->targets;
		const auto blend = state->second.blend;
		org::services::PipelineRecipe recipe;
		recipe.id = "dclf.tree-lod";
		recipe.shaderKey = ModuleHash(program->vertex) ^ ModuleHash(program->pixel) * 0x9E3779B97F4A7C15ull ^ ModuleHash(program->depthPixel);
		recipe.fixedFunctionKey = ankerl::unordered_dense::detail::wyhash::hash(&inputTargets, sizeof(inputTargets)) ^
		                          ankerl::unordered_dense::detail::wyhash::hash(&blend, sizeof(blend)) * 0x9E3779B97F4A7C15ull;
		recipe.build = [device = device, zLayout = zLayout->GetHandle(), program, formats = inputTargets, blend, frontCCW = *input->frontCCW] {
			return BuildTreeLod(device, zLayout, program, formats, blend, frontCCW);
		};
		t.requested = true;
		(void)service.Request(std::move(recipe), [completions = completions, generation = lane.generation](const org::services::PipelineArtifact& a_artifact) {
			completions->treeLod.Push({ generation, a_artifact });
			WakePipelineLane();
		});
		t.counters.requested.fetch_add(1, std::memory_order_relaxed);
	}

	void DrawPipelines::Impl::TryRequestRefraction()
	{
		auto& r = lane.refraction;
		if (!r.asked || r.requested || r.built || r.failed)
			return;
		// What it lacks arrives with the frame's inputs or its program (Finished::shadow); DrainLane retries each pass.
		const auto* input = lane.input.get();
		if (!input || !input->refractionUtility || input->refractionFormat == DXGI_FORMAT_UNKNOWN || input->targets.depth == DXGI_FORMAT_UNKNOWN || !input->frontCCW ||
			!shadowLayout)
			return;
		bool programFailed = false;
		const auto* program = ShaderPrograms::Get().FindShadow(kRefractionTechnique, *input->refractionUtility, true, nullptr, nullptr, &programFailed);
		if (programFailed) {
			r.failed = r.programFailed = true;
			r.counters.failed.fetch_add(1, std::memory_order_relaxed);
			lane.dirty = true;
			logger::warn("[DCLF] the refraction program (Utility {:08X}, pulled) failed; refraction draws nothing", kRefractionTechnique);
			return;
		}
		if (!program)
			return;
		const DXGI_FORMAT format = input->refractionFormat, depthFormat = input->targets.depth;
		const std::uint8_t writeMask = input->refractionWriteMask;
		const bool frontCCW = *input->frontCCW;
		org::services::PipelineRecipe recipe;
		recipe.id = "dclf.refraction";
		recipe.shaderKey = ModuleHash(program->vertex) ^ ModuleHash(program->pixel) * 0x9E3779B97F4A7C15ull;
		const std::uint64_t fixedFunction[] = { static_cast<std::uint64_t>(format), static_cast<std::uint64_t>(depthFormat), writeMask, frontCCW ? 1ull : 0ull };
		recipe.fixedFunctionKey = ankerl::unordered_dense::detail::wyhash::hash(fixedFunction, sizeof(fixedFunction));
		recipe.build = [device = device, layout = ShadowLayout(), program, format, depthFormat, writeMask, frontCCW] {
			return BuildRefraction(device, layout, program, format, depthFormat, writeMask, frontCCW);
		};
		r.requested = true;
		r.format = format;
		(void)service.Request(std::move(recipe), [completions = completions, generation = lane.generation, format](const org::services::PipelineArtifact& a_artifact) {
			completions->refraction.Push({ generation, format, a_artifact });
			WakePipelineLane();
		});
		r.counters.requested.fetch_add(1, std::memory_order_relaxed);
	}

	void DrawPipelines::Impl::TryRequestForward(const ForwardPipelineKey& a_key)
	{
		auto& f = lane.forward;
		if (f.entries.contains(a_key))
			return;  // requested, or failed for good
		const bool tree = (a_key.flags & ForwardPipelineKey::kForwardTreeLod) != 0;
		// A first-person key draws into the first-person targets, back faces culled (no mirror there).
		const bool firstPerson = (a_key.flags & ForwardPipelineKey::kForwardFirstPerson) != 0;
		const auto* input = lane.input.get();
		const ForwardTargets* keyTargets = input ? (firstPerson ? &input->firstPersonTargets : &input->forwardTargets) : nullptr;
		if (!input || keyTargets->colour == DXGI_FORMAT_UNKNOWN || keyTargets->depth == DXGI_FORMAT_UNKNOWN || !input->frontCCW ||
			!(tree ? input->distantTree : input->lighting) || !zLayout) {
			f.blocked.push_back(a_key);
			return;
		}
		bool programFailed = false;
		const auto* program = tree ? ShaderPrograms::Get().FindForwardTreeLod(*input->distantTree, &programFailed) :
		                             ShaderPrograms::Get().FindForward(a_key.vertexDescriptor, a_key.pixelDescriptor, *input->lighting, &programFailed);
		if (programFailed) {
			// Its objects stay out of the reflection phase (the engine draws them).
			f.FailForGood(a_key);
			f.counters.failed.fetch_add(1, std::memory_order_relaxed);
			lane.dirty = true;
			return;
		}
		if (!program) {
			f.programWaiters[tree ? Lane::Forward::kTreeProgram : ShaderPrograms::ForwardProgramId(a_key.vertexDescriptor, a_key.pixelDescriptor)].push_back(a_key);
			return;
		}
		const ForwardTargets forwardTargets = *keyTargets;
		const auto cull = (a_key.flags & ForwardPipelineKey::kForwardTwoSided) ? rhi::CullMode::None : firstPerson ? rhi::CullMode::Back : rhi::CullMode::Front;
		org::services::PipelineRecipe recipe;
		recipe.id = tree ? std::string("dclf.forward.tree-lod") : fmt::format("dclf.forward.{:08X}.{:08X}.{:X}", a_key.vertexDescriptor, a_key.pixelDescriptor, a_key.flags);
		recipe.shaderKey = ModuleHash(program->vertex) ^ ModuleHash(program->pixel) * 0x9E3779B97F4A7C15ull;
		recipe.fixedFunctionKey = ankerl::unordered_dense::detail::wyhash::hash(&forwardTargets, sizeof(forwardTargets)) ^
		                          static_cast<std::uint64_t>(cull) * 0x9E3779B97F4A7C15ull;
		recipe.build = [device = device, zLayout = zLayout->GetHandle(), program, forwardTargets, cull, frontCCW = *input->frontCCW] {
			return BuildForward(device, zLayout, program, forwardTargets, cull, frontCCW);
		};
		f.entries.try_emplace(a_key).first->second.requested = true;
		(void)service.Request(std::move(recipe), [completions = completions, key = a_key, generation = f.generation](const org::services::PipelineArtifact& a_artifact) {
			completions->forward.Push({ key, generation, a_artifact });
			WakePipelineLane();
		});
		f.counters.requested.fetch_add(1, std::memory_order_relaxed);
		lane.dirty = true;  // the catalog reports its program
	}

	void DrawPipelines::Impl::RecreateForward(const ForwardTargets& a_from, const ForwardTargets& a_to)
	{
		auto& f = lane.forward;
		// Start over (the new generation drops queued builds' completions); built ones are kept for the process: a recording may still
		// draw with them.
		logger::info("[DCLF] pipeline lane: forward targets colour {} depth {} -> colour {} depth {}; rebuilding {} forward pipelines", static_cast<int>(a_from.colour),
			static_cast<int>(a_from.depth), static_cast<int>(a_to.colour), static_cast<int>(a_to.depth), f.entries.size());
		for (auto& entry : f.entries)
			if (entry.second.built)
				f.retired.push_back(std::move(entry.second.built));
		f.StartOver();
		lane.dirty = true;
	}

	void DrawPipelines::Impl::AdmitTreeLodAndForward()
	{
		auto& t = lane.treeLod;
		completions->treeLod.Drain([&](TreeLodDone&& a_done) {
			if (a_done.generation != lane.generation)
				return;  // from before a target change (RecreateMainSet)
			lane.dirty = true;
			if (!a_done.artifact) {
				t.failed = true;
				t.counters.failed.fetch_add(1, std::memory_order_relaxed);
				logger::warn("[DCLF] tree LOD's pipelines failed: {}; tree LOD stays native", a_done.artifact.error);
				return;
			}
			t.built = a_done.artifact.payload;
			t.counters.ready.fetch_add(1, std::memory_order_relaxed);
			logger::info("[DCLF] tree LOD pipelines ready");
		});
		auto& r = lane.refraction;
		completions->refraction.Drain([&](RefractionDone&& a_done) {
			if (a_done.generation != lane.generation || a_done.format != r.format || !r.requested || r.built)
				return;  // from before a target or format change (RecreateMainSet, Refraction::Reset)
			lane.dirty = true;
			if (!a_done.artifact) {
				r.failed = true;
				r.counters.failed.fetch_add(1, std::memory_order_relaxed);
				logger::warn("[DCLF] the refraction pipeline failed: {}; refraction draws nothing", a_done.artifact.error);
				return;
			}
			r.built = a_done.artifact.payload;
			r.counters.ready.fetch_add(1, std::memory_order_relaxed);
			logger::info("[DCLF] refraction pipeline ready (Utility {:08X}, refraction normals format {})", kRefractionTechnique, static_cast<int>(a_done.format));
		});
		auto& f = lane.forward;
		completions->forward.Drain([&](ForwardDone&& a_done) {
			if (a_done.generation != f.generation)
				return;  // from before a forward targets change (RecreateForward)
			const auto found = f.entries.find(a_done.key);
			if (found == f.entries.end())
				return;
			auto& entry = found->second;
			lane.dirty = true;
			if (!a_done.artifact) {
				entry.failed = true;
				f.counters.failed.fetch_add(1, std::memory_order_relaxed);
				if (f.loggedFailures++ < kMaxLoggedFailures)
					logger::warn("[DCLF] forward pipeline VS {:08X} PS {:08X} flags {:X} failed: {}", a_done.key.vertexDescriptor, a_done.key.pixelDescriptor, a_done.key.flags,
						a_done.artifact.error);
				return;
			}
			entry.built = a_done.artifact.payload;
			f.counters.ready.fetch_add(1, std::memory_order_relaxed);
		});
	}

	void DrawPipelines::Impl::PublishCatalog()
	{
		auto& l = lane;
		auto catalog = std::make_unique<PipelineCatalog>();
		catalog->entries.reserve(l.entries.size());
		for (const auto& [key, entry] : l.entries) {
			PipelineCatalog::Entry out;
			out.failed = entry.failed;
			if (entry.index != kNotReady) {
				out.setIndex = entry.index;
				out.usage = l.usage[entry.index];
				out.tables = l.constantTables[entry.index];
			}
			catalog->entries.emplace(key, out);
		}
		catalog->generation = l.generation;
		catalog->targetsGeneration = l.input ? l.input->targetsGeneration : 0;
		catalog->revision = ++l.revision;
		// The version the indices were handed out of, or a later one (published only grows within a generation).
		catalog->setVersion = l.versions.published.load();
		const auto& s = l.shadow;
		catalog->shadowEntries.reserve(s.entries.size());
		for (const auto& [key, entry] : s.entries) {
			PipelineCatalog::ShadowEntry out;
			out.failed = entry.failed;
			out.setIndex = entry.index != kNotReady ? entry.index : PipelineCatalog::kNone;
			catalog->shadowEntries.emplace(key, out);
		}
		catalog->shadowGeneration = s.generation;
		catalog->shadowFormat = l.input ? l.input->shadowFormat : DXGI_FORMAT_UNKNOWN;
		catalog->shadowSetVersion = s.versions.published.load();
		catalog->treeLod.pipelines = l.treeLod.built;
		catalog->treeLod.failed = l.treeLod.failed;
		catalog->refraction.pipeline = l.refraction.built;
		catalog->refraction.format = l.refraction.built ? l.refraction.format : DXGI_FORMAT_UNKNOWN;
		catalog->refraction.failed = l.refraction.failed;
		const auto& f = l.forward;
		catalog->forwardEntries.reserve(f.entries.size());
		for (const auto& [key, entry] : f.entries) {
			PipelineCatalog::ForwardEntry out;
			out.pipeline = entry.built;
			out.program = entry.requested;
			out.failed = entry.failed;
			catalog->forwardEntries.emplace(key, out);
		}
		catalog->forwardTargets = l.input ? l.input->forwardTargets : ForwardTargets{};
		catalog->firstPersonTargets = l.input ? l.input->firstPersonTargets : ForwardTargets{};
		catalogSlot.Post(std::move(catalog));
		// The scene lane takes it at its next pass (ResolveLookups).
		WakeScenePass(SceneWake::Catalog);
		l.dirty = false;
	}

	struct ForwardPipelineAccess
	{
		static bool TreeLod(const PipelineCatalog* a_catalog, std::uint32_t a_targetsGeneration, TreeLodPipelines& a_out)
		{
			auto& drawPipelines = DrawPipelines::Get();
			const auto& impl = *drawPipelines.impl;
			// None once the targets changed after the catalog was built (as GetIndirectState).
			if (!a_catalog || !a_catalog->treeLod.pipelines || a_catalog->targetsGeneration != a_targetsGeneration || !impl.treeLodDrawSignature)
				return false;
			const auto* built = static_cast<const DrawPipelines::Impl::TreeLodBuilt*>(a_catalog->treeLod.pipelines.get());
			a_out = TreeLodPipelines{ built->depth->GetHandle(), built->colour->GetHandle(), impl.treeLodDrawSignature->GetHandle() };
			return true;
		}
	};

	bool TreeLodPipelinesOf(const PipelineCatalog* a_catalog, TreeLodPipelines& a_out)
	{
		return ForwardPipelineAccess::TreeLod(a_catalog, DrawPipelines::Get().Generation(), a_out);
	}

	bool TreeLodPipelinesOf(const PipelineCatalog* a_catalog, std::uint32_t a_targetsGeneration, TreeLodPipelines& a_out)
	{
		return ForwardPipelineAccess::TreeLod(a_catalog, a_targetsGeneration, a_out);
	}

	rhi::PipelineHandle ForwardPipelineOf(const PipelineCatalog* a_catalog, const ForwardPipelineKey& a_key, const ForwardTargets& a_targets)
	{
		if (!a_catalog)
			return {};
		const auto& built = (a_key.flags & ForwardPipelineKey::kForwardFirstPerson) ? a_catalog->firstPersonTargets : a_catalog->forwardTargets;
		if (!(built == a_targets))
			return {};
		const auto* entry = a_catalog->FindForward(a_key);
		if (!entry || !entry->pipeline)
			return {};
		return (*static_cast<const rhi::PipelinePtr*>(entry->pipeline.get()))->GetHandle();
	}

	std::uint32_t DrawPipelines::ShadowRasterStateId(const D3D11_RASTERIZER_DESC& a_desc, std::uint32_t a_renderMode)
	{
		// The scissor is not part of it: the epoch's pass is bounded by the view's viewport.
		if (a_desc.FillMode != D3D11_FILL_SOLID || !a_desc.DepthClipEnable) {
			if (impl->loggedShadowRasterFailures++ < kMaxLoggedFailures)
				logger::warn("[DCLF] shadow view rasterizer state not expressible (fill {}, depth clip {}); the view stays native",
					static_cast<int>(a_desc.FillMode), a_desc.DepthClipEnable);
			return 0;
		}
		Impl::ShadowRasterState state;
		state.depthBias = static_cast<float>(a_desc.DepthBias);
		state.depthBiasClamp = a_desc.DepthBiasClamp;
		state.slopeScaledDepthBias = a_desc.SlopeScaledDepthBias;
		state.cull = a_desc.CullMode == D3D11_CULL_NONE ? rhi::CullMode::None : a_desc.CullMode == D3D11_CULL_FRONT ? rhi::CullMode::Front : rhi::CullMode::Back;
		state.frontCCW = a_desc.FrontCounterClockwise != FALSE;
		auto& states = impl->shadowRasterStates;
		for (std::size_t i = 0; i < states.size(); ++i)
			if (states[i] == state)
				return static_cast<std::uint32_t>(i + 1);
		states.push_back(state);
		logger::info("[DCLF] shadow view rasterizer state {} (mode {:#x}): depth bias {} (clamp {}, slope {}), cull {}, front {}", states.size(), a_renderMode,
			state.depthBias, state.depthBiasClamp, state.slopeScaledDepthBias, static_cast<int>(a_desc.CullMode), a_desc.FrontCounterClockwise ? "CCW" : "CW");
		// Keys of this id wait in the lane until it arrives with the frame's inputs.
		impl->renderInput.shadowStates = std::make_shared<const std::vector<Impl::ShadowRasterState>>(states);
		if (Enabled())
			impl->PostInput(*this);
		return static_cast<std::uint32_t>(states.size());
	}

	std::uint32_t DrawPipelines::ShadowRasterStateCount() const
	{
		return static_cast<std::uint32_t>(impl->shadowRasterStates.size());
	}

	void DrawPipelines::CaptureEngineStates()
	{
		if (!Enabled())
			return;
		// State bits the lane asked for: read here (the deferred pass), or kept for a later one.
		impl->completions->stateRequests.Drain([&](std::uint32_t&& a_bits) { impl->pendingStateBits.push_back(a_bits); });
		bool changed = false;
		auto& input = impl->renderInput;
		if (!input.frontCCW)
			if (const auto frontCCW = impl->EngineFrontCCW()) {
				input.frontCCW = frontCCW;
				changed = true;
			}
		if (auto* lighting = EngineShaders::Lighting(); lighting != input.lighting) {
			input.lighting = lighting;
			changed = true;
		}
		// The program families' shaders (their programSource: the Effect port's BSEffectShader).
		for (std::size_t f = 0; f < Extensions::kProgramFamilies; ++f) {
			const auto& family = Extensions::ProgramFamilies()[f];
			if (const void* source = family.taken ? family.contribution.programSource() : nullptr; source != input.familySource[f]) {
				input.familySource[f] = source;
				changed = true;
			}
		}
		// The deferred blend states exist from the first deferred pass; read nothing before, so no state is recorded unreadable for good.
		if (DeferredBlendState(0, 0, 10, 0)) {
			auto read = [&](std::uint32_t a_bits, bool a_opaque) {
				if (impl->engineStates.contains(a_bits))
					return;
				std::string error;
				auto state = impl->ReadEngineState(a_bits, error);
				if (a_opaque) {
					const std::uint32_t writeMode = RasterWriteMode(a_bits);
					if (!state.valid && impl->loggedStateFailures++ < kMaxLoggedFailures)
						logger::warn("[DCLF] opaque write mode {} state cannot be read: {}; its objects stay native", writeMode, error);
					else if (state.valid)
						logger::info("[DCLF] opaque write mode {}: rt0 mask {:X}, rt1 mask {:X}", writeMode, static_cast<unsigned>(state.blend.attachments[0].writeMask),
							static_cast<unsigned>(state.blend.attachments[1].writeMask));
				} else if (!state.valid && impl->loggedStateFailures++ < kMaxLoggedFailures) {
					logger::warn("[DCLF] pipeline state {:05X} cannot be built: {}; its objects stay native", a_bits, error);
				} else if (state.valid) {
					logger::info("[DCLF] pipeline state {:05X}: depth bias {} (clamp {}, slope {}), blend rt0 {} mask {:X}, rt1 {} mask {:X}",
						a_bits, state.depthBias, state.depthBiasClamp, state.slopeScaledDepthBias, state.blend.attachments[0].enable ? "on" : "off",
						static_cast<unsigned>(state.blend.attachments[0].writeMask), state.blend.attachments[1].enable ? "on" : "off",
						static_cast<unsigned>(state.blend.attachments[1].writeMask));
				}
				impl->engineStates.emplace(a_bits, state);
				changed = true;
			};
			// The opaque write modes (opaque Lighting keys and tree LOD), unasked; then what the lane asked for.
			for (const std::uint32_t writeMode : { Impl::kOpaqueWriteMode, Impl::kAlphaTestedWriteMode })
				read(writeMode << kRasterWriteModeShift, true);
			for (const std::uint32_t bits : std::exchange(impl->pendingStateBits, {}))
				read(bits, false);  // the opaque modes are read above
		}
		if (changed) {
			input.states = std::make_shared<const decltype(impl->engineStates)>(impl->engineStates);
			impl->PostInput(*this);
		}
	}

	ShadowIndirectState GetShadowIndirectState(const PipelineCatalog& a_catalog, DXGI_FORMAT a_shadowFormat)
	{
		// a_catalog's shadow set version (its lookups' indices are of it); none once the shadow map format differs from the set's.
		const auto& pipelines = DrawPipelines::Get();
		const auto& impl = *pipelines.impl;
		ShadowIndirectState state;
		if (!a_catalog.shadowSetVersion || a_catalog.shadowFormat != a_shadowFormat || !impl.layout || !impl.shadowLayout)
			return state;
		auto version = std::static_pointer_cast<const DrawPipelines::Impl::ShadowSetVersion>(a_catalog.shadowSetVersion);
		state.layout = impl.ShadowLayout();
		state.pipelines.reserve(version->pipelines.size());
		for (const auto& payload : version->pipelines)
			state.pipelines.push_back(static_cast<const DrawPipelines::Impl::Built*>(payload.get())->pipelines[kColorVariant]->GetHandle());
		state.vertexLayouts = version->layouts;
		state.discards = version->discards;
		state.drawSignature = impl.shadowDrawSignature->GetHandle();
		state.version = std::move(version);
		state.valid = true;
		return state;
	}

	IndirectState GetIndirectState(const PipelineCatalog& a_catalog, std::uint32_t a_targetsGeneration)
	{
		// a_catalog's set version (its lookups' indices are of it); none once the targets differ from the set's.
		const auto& pipelines = DrawPipelines::Get();
		const auto& impl = *pipelines.impl;
		IndirectState state;
		if (!a_catalog.setVersion || a_catalog.targetsGeneration != a_targetsGeneration || !impl.layout)
			return state;
		auto version = std::static_pointer_cast<const DrawPipelines::Impl::SetVersion>(a_catalog.setVersion);
		for (std::uint32_t variant = 0; variant < kVariantCount; ++variant) {
			state.sets[variant] = version->sets[variant]->GetHandle();
			state.signatures[variant] = version->signatures[variant]->GetHandle();
		}
		state.zGroups = version->zGroupOf;
		state.zPipelines.reserve(version->zGroupFirst.size());
		for (const auto first : version->zGroupFirst) {
			// An effect pipeline's group has no Z-prepass pipeline, and no bucket names it (PlanZBuckets).
			const auto& pulledDepth = static_cast<const DrawPipelines::Impl::Built*>(version->pipelines[first].get())->pulledDepth;
			state.zPipelines.push_back(pulledDepth ? pulledDepth->GetHandle() : decltype(pulledDepth->GetHandle()){});
		}
		state.zLayout = impl.zLayout->GetHandle();
		state.zDrawSignature = impl.zDrawSignature->GetHandle();
		// The refraction draw, under the shadow views' layout.
		if (a_catalog.refraction.pipeline && impl.shadowLayout && impl.shadowDrawSignature) {
			state.refraction = (*static_cast<const rhi::PipelinePtr*>(a_catalog.refraction.pipeline.get()))->GetHandle();
			state.refractionFormat = a_catalog.refraction.format;
			state.refractionLayout = impl.shadowLayout->GetHandle();
			state.refractionDrawSignature = impl.shadowDrawSignature->GetHandle();
		}
		state.version = std::move(version);
		state.layout = impl.layout->GetHandle();
		state.valid = true;
		return state;
	}

	IndirectState FrameIndirectState()
	{
		const auto* catalog = FrameCatalog();
		return catalog ? GetIndirectState(*catalog, DrawPipelines::Get().Generation()) : IndirectState{};
	}

	ShadowIndirectState FrameShadowIndirectState()
	{
		const auto* catalog = FrameCatalog();
		return catalog ? GetShadowIndirectState(*catalog, DrawPipelines::Get().ShadowFormat()) : ShadowIndirectState{};
	}

	DXGI_FORMAT DrawPipelines::ShadowFormat() const
	{
		return impl->renderInput.shadowFormat;
	}
}

