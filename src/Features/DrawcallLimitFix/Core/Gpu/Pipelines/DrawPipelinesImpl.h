// DrawPipelines::Impl, shared by the pipeline translation units: DrawPipelines.cpp (the public face, the lane's pass, the engine
// states), MainPipelines.cpp (Z and colour), ShadowPipelines.cpp, ForwardPipelines.cpp (forward and refraction) and
// FamilyPipelines.cpp (the draw families). Also the helpers more than one of them uses; nothing here is called per draw.

#pragma once

// The including units' block (each starts with it): volk must precede every Vulkan header in a translation unit.
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <rhi_interop_vulkan.h>

#include "Features/DrawcallLimitFix/Core/Gpu/BuildExecutor.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelines.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelinesRhi.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/EngineStates.h"
#include "Features/DrawcallLimitFix/Core/Extensions/DrawFamilies.h"
#include "Features/DrawcallLimitFix/Core/Extensions/MaterialPorts.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Materials.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ProgramFamilies.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Views.h"
#include "Features/DrawcallLimitFix/Core/Runtime/EventQueue.h"
#include "Features/DrawcallLimitFix/Core/Runtime/LatestSlot.h"
#include "Features/DrawcallLimitFix/Core/Scene/SceneWake.h"
#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"

#include "RenderGraph/RenderGraphRuntime.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/SpirvReflection.h"
#include "Features/DrawcallLimitFix/Core/Shading/LightingConstants.h"
#include "Features/DrawcallLimitFix/Core/Shading/EngineShaders.h"
#include "Features/DrawcallLimitFix/Core/Shading/LightingDescriptors.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Lookups.h"
#include "Features/DrawcallLimitFix/Core/Shading/VertexInput.h"
#include "ShaderCache.h"

#include <Tracy/Tracy.hpp>

#include <OpenRenderGraph/PersistentGraphHost.h>
#include <ORGModuleServices/PipelineRegistry.h>
#include <ORGModuleServices/PipelineService.h>
#include <rhi_helpers.h>

#include <algorithm>
#include <atomic>
#include <limits>
#include <stdexcept>
#include <string_view>
#include "Features/DrawcallLimitFix/Diagnostics/Observation.h"
#include "Features/DrawcallLimitFix/Diagnostics/ParityCounter.h"
#include "Features/DrawcallLimitFix/Diagnostics/Policy.h"

namespace DCLF
{
	// Inline: unqualified in DCLF as the one translation unit's anonymous namespace was, without sharing DCLF's linkage names.
	inline namespace DrawPipelinesDetail
	{
		/**
		 * @brief Whether a fragment program defeats early depth testing: it discards, writes depth or the sample mask, or writes memory.
		 * Such a pipeline is in the shadow views' discarding class (kShadowDiscards), drawn after the others.
		 */
		inline bool FragmentDefersDepth(std::span<const std::byte> a_spirv)
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

		/** @brief Which of FragDepth and SampleMask a fragment program writes (their BuiltIn decorations), for its state class. */
		inline void FragmentExports(std::span<const std::byte> a_spirv, bool& a_depth, bool& a_sampleMask)
		{
			a_depth = a_sampleMask = false;
			const std::size_t words = a_spirv.size() / 4;
			auto word = [&](std::size_t i) {
				std::uint32_t w;
				std::memcpy(&w, a_spirv.data() + i * 4, 4);
				return w;
			};
			constexpr std::uint32_t kOpDecorate = 71, kOpMemberDecorate = 72, kDecorationBuiltIn = 11, kBuiltInSampleMask = 20, kBuiltInFragDepth = 22;
			for (std::size_t i = 5; i < words;) {
				const std::uint32_t first = word(i);
				const std::uint32_t count = first >> 16, op = first & 0xFFFF;
				if (count == 0 || i + count > words)
					return;
				std::uint32_t builtIn = ~0u;
				if (op == kOpDecorate && count >= 4 && word(i + 2) == kDecorationBuiltIn)
					builtIn = word(i + 3);
				else if (op == kOpMemberDecorate && count >= 5 && word(i + 3) == kDecorationBuiltIn)
					builtIn = word(i + 4);
				a_depth = a_depth || builtIn == kBuiltInFragDepth;
				a_sampleMask = a_sampleMask || builtIn == kBuiltInSampleMask;
				i += count;
			}
		}
		inline constexpr std::uint32_t kMaxLoggedFailures = 8;

		// Register classes as shifted by the SPIR-V builds (ShaderPrograms.h), set 0. DCLF_BINDLESS builds declare a cbuffer at the draw's
		// push binding for the object index; the address words beside it feed the layout's indirect ranges.
		inline constexpr std::uint32_t kRecordAddressBinding = kDrawPushBinding;
		// The texture registers the vertex stage may declare: t121 (shading) to t127 (records); t125 is pixel-only but in the run.
		inline constexpr std::uint32_t kObjectBufferBinding = kBindingShiftT + kObjectBufferRegister;
		inline constexpr std::uint32_t kVertexTextureBinding = kBindingShiftT + kShadingBufferRegister;
		inline constexpr std::uint32_t kVertexTextureCount = kObjectBufferRegister - kShadingBufferRegister + 1;
		inline constexpr std::uint32_t kDescriptorHeapBindings = 1000000;  // DXC's ResourceDescriptorHeap bindings (BasicRHI maps them)

		inline bool InRange(std::uint32_t a_binding, std::uint32_t a_first, std::uint32_t a_count)
		{
			return a_binding >= a_first && a_binding < a_first + a_count;
		}

		inline const char* KindName(SpirvReflection::BindingKind a_kind)
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
		inline void CheckBindings(const SpirvReflection& a_module, bool a_pixel)
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
		inline void CheckPulledBindings(const SpirvReflection& a_module, bool a_pixel)
		{
			CheckBindings(a_module, a_pixel);
			using Kind = SpirvReflection::BindingKind;
			for (const auto& binding : a_module.bindings) {
				if (binding.binding >= kDescriptorHeapBindings)
					continue;
				const std::uint32_t b = binding.binding - kBindingShiftB, t = binding.binding - kBindingShiftT;
				const bool row = (binding.kind == Kind::ConstantBuffer && (b <= 2 || b == 4)) ||
				                 ((binding.kind == Kind::Texture || binding.kind == Kind::StorageBuffer) && InRange(binding.binding, kBindingShiftT, kTextureRegisters) &&
									 (t < kPixelTextureSlots || Extensions::FeatureTextureSlotOf(t) >= 0)) ||
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

		inline void AddUsage(const SpirvReflection& a_module, bool a_pixel, RegisterUsage& a_usage)
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
		inline constexpr std::string_view kLightingVSNames[] = { "World", "PreviousWorld", "EyePosition", "LandBlendParams", "TreeParams", "WindTimers", "TextureProj",
			"IndexScale", "WorldMapOverlayParameters", "LeftEyeCenter", "RightEyeCenter", "TexcoordOffset", "HighDetailRange", "FogParam", "FogNearColor", "FogFarColor",
			"Bones" };
		inline constexpr std::string_view kLightingPSNames[] = { "NumLightNumShadowLight", "PointLightPosition", "PointLightColor", "DirLightDirection", "DirLightColor",
			"DirectionalAmbient", "AmbientSpecularTintAndFresnelPower", "MaterialData", "EmitColor", "AlphaTestRef", "ShadowLightMaskSelect", "VPOSOffset",
			"ProjectedUVParams", "ProjectedUVParams2", "ProjectedUVParams3", "SplitDistance", "SSRParams", "WorldMapOverlayParametersPS", "AmbientColor", "FogColor",
			"ColourOutputClamp", "EnvmapData", "ParallaxOccData", "TintColor", "LODTexParams", "SpecularColor", "SparkleParams", "MultiLayerParallaxData",
			"LightingEffectParams", "IBLParams", "LandscapeTexture1to4IsSnow", "LandscapeTexture5to6IsSnow", "LandscapeTexture1to4IsSpecPower",
			"LandscapeTexture5to6IsSpecPower", "SnowRimLightParameters", "CharacterLightParams", "PBRFlags", "PBRParams1", "LandscapeTexture2PBRParams",
			"LandscapeTexture3PBRParams", "LandscapeTexture4PBRParams", "LandscapeTexture5PBRParams", "LandscapeTexture6PBRParams", "PBRParams2",
			"LandscapeTexture1GlintParameters", "LandscapeTexture2GlintParameters", "LandscapeTexture3GlintParameters", "LandscapeTexture4GlintParameters",
			"LandscapeTexture5GlintParameters", "LandscapeTexture6GlintParameters", "MaterialObjectRGBScale" };
		static_assert(std::size(kLightingVSNames) == kLightingVSVariables && std::size(kLightingPSNames) == kLightingPSVariables);

		inline std::span<const std::string_view> LightingNames(bool a_pixel)
		{
			return a_pixel ? std::span<const std::string_view>(kLightingPSNames) : std::span<const std::string_view>(kLightingVSNames);
		}

		/*
		 * No DCLF pipeline has input assembly: every vertex stage pulls its attributes (DCLF_PULLED_VERTICES, DCLF_PULLED), so the
		 * pipelines have no vertex input state, which an execution set's pipelines would otherwise have to share. A key's vertex layout
		 * must still hold every attribute its vertex stage reads (ShaderPrograms::Program::vertexAttributes), as the input layout the
		 * engine builds for it had to: else the build fails and its objects stay native.
		 */
		inline void CheckVertexAttributes(std::uint32_t a_attributes, std::uint64_t a_vertexLayout)
		{
			if (const std::uint32_t missing = a_attributes & ~VertexLayoutAttributes(a_vertexLayout))
				throw std::runtime_error(fmt::format("vertex attributes {:03X} (BSGraphics::Vertex::Attribute bits) are not in the geometry's layout {:016X}", missing,
					a_vertexLayout));
		}
		// Mixed into the main and shadow recipes' fixedFunctionKey: a pipeline built with an input layout (before the vertex stages
		// pulled their attributes) is never the artifact a recipe finds.
		inline constexpr std::uint64_t kPulledVerticesSalt = 0x5055'4C4C'5645'5254ull;

		// Initialize's pipeline layout ranges.
		inline rhi::LayoutBindingRange LayoutRange(std::uint32_t a_binding, std::uint32_t a_count, rhi::ShaderStage a_stage, rhi::LayoutRangeSource a_source, std::size_t a_offset,
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
		inline rhi::LayoutBindingRange RangeFrom(rhi::LayoutBindingRange a_range, std::uint32_t a_root, std::uint32_t a_word)
		{
			a_range.addressRootIndex = a_root;
			a_range.addressOffset32 = a_word;
			return a_range;
		}
		inline rhi::LayoutBindingRange PushedRange(std::uint32_t a_binding, rhi::ShaderStage a_stage, std::uint32_t a_root, std::uint32_t a_word)
		{
			return RangeFrom(LayoutRange(a_binding, 1, a_stage, rhi::LayoutRangeSource::PushAddress, 0), a_root, a_word);
		}
		inline rhi::LayoutBindingRange FromMaterialRow(rhi::LayoutBindingRange a_range) { return RangeFrom(a_range, 0, kDrawPushMaterialRow); }
		inline rhi::LayoutBindingRange FromFrameRecord(rhi::LayoutBindingRange a_range) { return RangeFrom(a_range, 1, kFramePushRecord); }

		// Initialize's layouts' ranges (MainPipelines.cpp, ShadowPipelines.cpp).
		std::vector<rhi::LayoutBindingRange> MainLayoutRanges(bool a_pulled);
		std::vector<rhi::LayoutBindingRange> ShadowLayoutRanges();

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

		/**
		 * @brief Everything of one variant's pipeline but its two shader modules (PipelineDesc::State): the fixed-function state as the
		 * build passes it, and the fragment interface of its pixel module. An indirect execution set's pipelines must agree in all of it
		 * (VK_EXT_device_generated_commands: static and dynamic state, fragment outputs, FragDepth and SampleMask), so it is what a
		 * state class is keyed by. No vertex input: no DCLF pipeline has any (CheckVertexAttributes).
		 */
		struct PipelineStateDesc
		{
			rhi::RasterState raster{};
			rhi::DepthStencilState depth{};
			rhi::BlendState blend{};
			rhi::RenderTargets targets{};
			rhi::Format depthFormat = rhi::Format::Unknown;
			rhi::PrimitiveTopology topology = rhi::PrimitiveTopology::TriangleList;
			rhi::PipelineFlags flags = rhi::PipelineFlags_None;
			// The pixel module's: the attachments it writes, whether there is one, FragDepth and SampleMask, a deferred depth test.
			std::uint32_t outputLocations = 0;
			bool hasPixel = false, exportsDepth = false, exportsSampleMask = false, defersDepth = false;
		};
		/** @brief A state class's key: both variants' state, as a set index selects the same entry of both variants' sets. */
		struct StateClassKey
		{
			std::array<PipelineStateDesc, kVariantCount> variants{};
		};

		/**
		 * @brief Each field of a_state, a value apiece, as SameState compares them: the blend attachments as the Vulkan backend expands
		 * them (max(targets, attachments) of them, an index past the last attachment reading the last: rhi_vulkan.cpp), the floats by bits.
		 */
		template <class Field>
		void StateFields(const PipelineStateDesc& a_state, Field&& a_field)
		{
			const auto& r = a_state.raster;
			a_field("fill", static_cast<std::uint64_t>(r.fill));
			a_field("cull", static_cast<std::uint64_t>(r.cull));
			a_field("frontCCW", r.frontCCW);
			a_field("depthBias", std::bit_cast<std::uint32_t>(r.depthBias));
			a_field("depthBiasClamp", std::bit_cast<std::uint32_t>(r.depthBiasClamp));
			a_field("slopeScaledDepthBias", std::bit_cast<std::uint32_t>(r.slopeScaledDepthBias));
			a_field("conservative", r.conservative);
			a_field("depthEnable", a_state.depth.depthEnable);
			a_field("depthWrite", a_state.depth.depthWrite);
			a_field("depthFunc", static_cast<std::uint64_t>(a_state.depth.depthFunc));
			const auto& b = a_state.blend;
			a_field("alphaToCoverage", b.alphaToCoverage);
			a_field("independentBlend", b.independentBlend);
			const std::uint32_t attachments = std::min<std::uint32_t>(std::max(a_state.targets.count, b.numAttachments), 8);
			a_field("blend attachments", attachments);
			for (std::uint32_t i = 0; i < attachments; ++i) {
				const auto& a = b.attachments[std::min(i, b.numAttachments ? b.numAttachments - 1 : 0u)];
				a_field("blend enable", (std::uint64_t(i) << 32) | (a.enable ? 1u : 0u));
				a_field("blend colour", (std::uint64_t(i) << 32) | (std::uint64_t(a.srcColor) << 16) | (std::uint64_t(a.dstColor) << 8) | std::uint64_t(a.colorOp));
				a_field("blend alpha", (std::uint64_t(i) << 32) | (std::uint64_t(a.srcAlpha) << 16) | (std::uint64_t(a.dstAlpha) << 8) | std::uint64_t(a.alphaOp));
				a_field("write mask", (std::uint64_t(i) << 32) | static_cast<std::uint64_t>(a.writeMask));
			}
			a_field("targets", a_state.targets.count);
			for (std::uint32_t i = 0; i < a_state.targets.count && i < 8; ++i)
				a_field("target format", (std::uint64_t(i) << 32) | static_cast<std::uint64_t>(a_state.targets.formats[i]));
			a_field("depth format", static_cast<std::uint64_t>(a_state.depthFormat));
			a_field("topology", static_cast<std::uint64_t>(a_state.topology));
			a_field("flags", static_cast<std::uint64_t>(a_state.flags));
			a_field("output locations", a_state.outputLocations);
			a_field("pixel stage", a_state.hasPixel);
			a_field("FragDepth", a_state.exportsDepth);
			a_field("SampleMask", a_state.exportsSampleMask);
			a_field("defers depth", a_state.defersDepth);
		}

		/**
		 * @brief Whether two keys are one state class: every field of both variants (StateFields). a_fields, when given, gets the fields
		 * that differ ("depth: depthFunc 3 -> 4"), for the set class parity.
		 */
		inline bool SameState(const StateClassKey& a_left, const StateClassKey& a_right, KeyDifference* a_fields = nullptr)
		{
			bool same = true;
			for (std::uint32_t v = 0; v < kVariantCount; ++v) {
				std::vector<std::pair<const char*, std::uint64_t>> left, right;
				StateFields(a_left.variants[v], [&](const char* a_name, std::uint64_t a_value) { left.emplace_back(a_name, a_value); });
				StateFields(a_right.variants[v], [&](const char* a_name, std::uint64_t a_value) { right.emplace_back(a_name, a_value); });
				if (left == right)
					continue;
				same = false;
				if (!a_fields)
					break;
				const std::size_t common = std::min(left.size(), right.size());
				for (std::size_t f = 0; f < common; ++f)
					if (left[f] != right[f])
						a_fields->Field(fmt::format("{}: {}", v == kDepthVariant ? "depth" : "colour", left[f].first).c_str(), left[f].second, right[f].second);
				if (left.size() != right.size())
					a_fields->Field(v == kDepthVariant ? "depth: fields" : "colour: fields", left.size(), right.size());
			}
			return same;
		}
		struct StateClassHash
		{
			using is_avalanching = void;
			std::uint64_t operator()(const StateClassKey& a_key) const noexcept
			{
				std::uint64_t hash = 0xcbf29ce484222325ull;
				for (const auto& variant : a_key.variants)
					StateFields(variant, [&](const char*, std::uint64_t a_value) { hash = ankerl::unordered_dense::detail::wyhash::mix(hash ^ a_value, 0x9E3779B97F4A7C15ull); });
				return hash;
			}
		};
		struct StateClassEqual
		{
			bool operator()(const StateClassKey& a_left, const StateClassKey& a_right) const { return SameState(a_left, a_right); }
		};
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
		 * @brief A version of the state classes' indirect pipeline sets and their signatures, holding the admitted pipelines [0, applied).
		 *
		 * Frames keep the version they record with (IndirectState::version). New pipelines are written only into a version nothing else
		 * references, at indices it never held, then published. So a set is never written while a recording reads it or sizes preprocess
		 * memory against it (VUID-VkGeneratedCommandsInfoEXT-preprocessSize-11071), and no entry submitted work may use is rewritten
		 * (VUID-VkWriteIndirectExecutionSetPipelineEXT-index-11029).
		 */
		struct SetVersion
		{
			/**
			 * @brief A state class's sets (one per variant: a pipeline has the same index in both) and their signatures, holding its first
			 * applied members (Lane::classMembers), and the handles written at each index, which the set class parity reads.
			 */
			struct ClassSets
			{
				std::array<rhi::IndirectPipelineSetPtr, kVariantCount> sets;
				std::array<rhi::CommandSignaturePtr, kVariantCount> signatures;
				std::array<std::vector<rhi::PipelineHandle>, kVariantCount> written;
				std::uint32_t applied = 0;
			};
			std::vector<ClassSets> classes;  // by state class, in creation order
			// By set index in [0, applied): class << kSetClassShift | index in the class (IndirectState::classOf).
			std::vector<std::uint32_t> classOf;
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
		// The draw families' draw signatures, by family (Api::DrawFamilySignature::DrawInstanced: a plain DrawInstanced under zLayout). Their
		// pipelines are the lane's (Lane::families).
		std::array<rhi::CommandSignaturePtr, kDrawFamilyLanes> familySignatures;
		struct FamilyBuilt
		{
			rhi::PipelinePtr depth, colour;
		};
		// Render thread: what it has asked of the lane already, so it asks each once.
		std::array<bool, kDrawFamilyLanes> familyAsked{};  // by draw family (RequestFamily)
		bool refractionAsked = false;  // the refraction pipeline (RequestRefraction)
		ankerl::unordered_dense::set<ForwardPipelineKey, ForwardPipelineKeyHash> forwardAsked;
		rhi::PipelineLayoutHandle ShadowLayout() const { return shadowLayout->GetHandle(); }
		// The shadow views' plain indirect draw: a DrawSequence's tail as an indexed draw from the index pool (StoreShadowSequence).
		rhi::CommandSignaturePtr shadowDrawSignature;
		bool supported = false;
		bool attempted = false;
		org::services::PipelineService service;
		/**
		 * @brief A pipeline's hot reload registration (RenderGraphRuntime::PipelineRegistry; dclf-hot-reload.md), made with its request:
		 * its source, how to build it again (its program from the files as they are now, then the same build with the same inputs), and
		 * the program it is built from now. Registered once admitted; the lane keeps the slot where it keeps the payload.
		 */
		struct Live
		{
			org::services::LivePipelineDesc desc;
			std::uint64_t fingerprint = 0;  // its program's (ShaderPrograms::Program::fingerprint)
			std::uint64_t builtFor = 0;     // its program's sources generation
		};
		/**
		 * @brief A_program's kind's registration: a rebuild takes the program from a_rebuildProgram (a ShaderPrograms::Rebuild* call, any
		 * thread) and, when it differs from the one built with (or the reload is forced), runs a_build on DCLF's build pool, as the
		 * service's build runs a_build on the lane's program.
		 */
		template <class P, class RebuildProgram, class BuildPayload>
		static std::shared_ptr<const Live> MakeLive(std::string a_id, std::string a_source, const P& a_program, RebuildProgram a_rebuildProgram, BuildPayload a_build)
		{
			auto live = std::make_shared<Live>();
			live->desc.id = std::move(a_id);
			live->desc.sources = { std::move(a_source) };
			live->desc.rebuild = [a_rebuildProgram, a_build](const org::services::LiveRebuildRequest& a_request, org::services::LiveRebuildDone a_done) {
				a_rebuildProgram([a_request, a_build, a_done = std::move(a_done)](std::shared_ptr<const P> a_rebuilt, std::string a_error) {
					if (!a_rebuilt)
						return a_done({ .error = std::move(a_error) });
					const std::uint64_t fingerprint = a_rebuilt->fingerprint;
					if (fingerprint == a_request.fingerprint && !a_request.force)
						return a_done({ .fingerprint = fingerprint });
					// The pipeline on DCLF's build pool, as the service's builds (the program's last compile may have completed anywhere).
					auto task = [a_rebuilt, a_build, a_done, fingerprint] {
						try {
							a_done({ .payload = a_build(*a_rebuilt), .fingerprint = fingerprint });
						} catch (const std::exception& e) {
							a_done({ .error = e.what() });
						}
					};
					if (!BuildSubmitter()(task))
						task();
				});
			};
			live->fingerprint = a_program.fingerprint;
			live->builtFor = a_program.sourcesGeneration;
			return live;
		}
		/** @brief The lane, a pipeline admitted: its slot in the registry (null without a registration, or without the registry). */
		static std::shared_ptr<org::services::LivePipelineSlot> RegisterLive(const std::shared_ptr<const Live>& a_live, const org::services::PipelinePayload& a_payload)
		{
			auto* registry = RenderGraphRuntime::PipelineRegistry();
			if (!a_live || !registry)
				return {};
			return registry->Register(a_live->desc, a_payload, a_live->fingerprint, a_live->builtFor);
		}

		/*
		 * Build completions: each callback pushes its artifact (on the building thread, or inline on the requesting thread for a cached
		 * pipeline) and wakes the lane, which drains them. Each carries its request's generation; one from before a target or format
		 * change is dropped. And its registration (Live).
		 */
		struct MainDone
		{
			PipelineKey key;
			std::uint32_t generation = 0;
			org::services::PipelineArtifact artifact;
			std::shared_ptr<const Live> live;
		};
		struct ShadowDone
		{
			ShadowPipelineKey key;
			std::uint32_t generation = 0;
			org::services::PipelineArtifact artifact;
			std::shared_ptr<const Live> live;
		};
		struct FamilyDone
		{
			std::uint32_t generation = 0;
			org::services::PipelineArtifact artifact;
			std::shared_ptr<const Live> live;
		};
		struct ForwardDone
		{
			ForwardPipelineKey key;
			std::uint32_t generation = 0;
			org::services::PipelineArtifact artifact;
			std::shared_ptr<const Live> live;
		};
		struct RefractionDone
		{
			std::uint32_t generation = 0;              // the main set's (the main depth's format)
			DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;  // the refraction normals' it was built for
			org::services::PipelineArtifact artifact;
			std::shared_ptr<const Live> live;
		};
		struct Completions
		{
			EventQueue<MainDone, 1024> main;
			EventQueue<ShadowDone, 1024> shadow;
			std::array<EventQueue<FamilyDone, 16>, kDrawFamilyLanes> family;  // by draw family
			EventQueue<ForwardDone, 256> forward;
			EventQueue<RefractionDone, 16> refraction;
			// Requests to the lane, and the lane's engine-state requests to the render thread (CaptureEngineStates replies).
			EventQueue<LightingRequest, 1024> requests;
			EventQueue<ShadowRequest, 1024> shadowRequests;
			EventQueue<ForwardPipelineKey, 256> forwardRequests;
			std::array<std::atomic<bool>, kDrawFamilyLanes> familyRequest{};  // by draw family
			std::atomic<bool> refractionRequest{ false };
			EventQueue<std::uint32_t, 64> stateRequests;
		};
		// Never freed: a build may complete while the process tears down.
		Completions* completions = new Completions;

		// The builds run on DCLF's preparation pool (BuildExecutor.h), not a thread each. A shader reload's start and its swap wake the lane
		// (FollowSources, AdoptLivePipelines).
		Impl()
		{
			service.Configure(BuildSubmitter());
			if (auto* registry = RenderGraphRuntime::PipelineRegistry())
				registry->AddListener([] { WakePipelineLane(); });
		}

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
			// A forward or main key's plain colour draws' pipeline (the pulled stages, the colour variant's state; ForwardDrawPass, the colour
			// pass): null for any other key, and for one whose pulled stages would not build (its range is then drawn device-generated).
			rhi::PipelinePtr pulledColour;
			// An effect decal key's plain depth draws' pipeline (the pulled stages, the depth variant's state; the decal depth pass's decal
			// buckets): null for any other key (a Lighting key's are its pulledDepth).
			rhi::PipelinePtr pulledDecalDepth;
			ZPipelineKey zKey;
			std::shared_ptr<const ConstantTables> tables;  // a main pipeline's stages' (ConstantTableOf)
			// A main pipeline's both variants' state (PipelineDesc::State): the state class whose execution sets it goes in. Not the pulled
			// depth pipeline's, which plain draws bind.
			StateClassKey stateClass;
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

		static bool ToRHI(D3D11_BLEND a_factor, rhi::BlendFactor& a_out);
		static bool ToRHI(D3D11_BLEND_OP a_op, rhi::BlendOp& a_out);

		/** @brief The engine's state objects for these state bits, in RHI terms. */
		EngineState ReadEngineState(std::uint32_t a_stateBits, std::string& a_error);

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
			// SetFamilyInputs, by draw family (its programs' and its forward program's shader: tree LOD's DistantTree); SetViewForwardTargets,
			// by view (its keys').
			std::array<RE::BSShader*, kDrawFamilyLanes> drawFamilySource{};
			std::array<ForwardTargets, Extensions::kEngineViews> viewTargets{};
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
			std::shared_ptr<org::services::LivePipelineSlot> slot;  // built's (AdoptLivePipelines)
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
			// By index: setPipelines' registry slot (AdoptLivePipelines takes what a reload swapped into it).
			std::vector<std::shared_ptr<org::services::LivePipelineSlot>> setSlots;
			std::uint64_t registryGeneration = 0;  // PipelineRegistry::Generation as last adopted
			// Z-prepass groups: each pipeline's (numbered as first admitted), and each group's first pipeline.
			std::vector<std::uint32_t> zGroupOf, zGroupFirst;
			ankerl::unordered_dense::map<ZPipelineKey, std::uint32_t, ZPipelineKeyHash> zGroups;
			std::vector<std::array<RegisterUsage, kVariantCount>> usage;  // by set index, then variant
			std::vector<std::shared_ptr<const ConstantTables>> constantTables;  // by set index
			// State classes (Built::stateClass), numbered as first admitted: each pipeline's word (by set index: class << kSetClassShift |
			// index in the class), each class's members (set indices, in admission order) and each key's class.
			std::vector<std::uint32_t> classOf;
			std::vector<std::vector<std::uint32_t>> classMembers;
			ankerl::unordered_dense::map<StateClassKey, std::uint32_t, StateClassHash, StateClassEqual> classIndex;
			std::atomic<std::uint32_t> stateClasses{ 0 }, largestClass{ 0 };  // Stats', relaxed
			// CS_DCLF_SET_PARITY: each published version's classes against their members (CheckClasses), and its counts for Stats (relaxed).
			Diagnostics::ParityCounter classParity;
			std::atomic<std::uint64_t> classParityChecks{ 0 }, classParityMismatches{ 0 };
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
				// By index, admitted: the pipeline, its vertex layout and its class (kShadowDiscards), and the pipeline's registry slot.
				std::vector<org::services::PipelinePayload> setPipelines;
				std::vector<std::shared_ptr<org::services::LivePipelineSlot>> setSlots;
				std::vector<std::uint64_t> layouts;
				std::vector<std::uint8_t> discards;
				std::string recreatedBy;  // the set's last recreation, for NearestShadowKey's warnings
			};
			Shadow shadow;
			/** @brief A draw family's two pipelines, rebuilt with each new main set (RecreateMainSet), under the main set's generation. */
			struct FamilyLane
			{
				bool asked = false;          // RequestFamily has arrived: built from here on, for every generation
				bool requested = false;      // this generation's build is queued
				bool failed = false;         // this generation's build failed, or the program did
				bool programFailed = false;  // for good
				org::services::PipelinePayload built;
				std::shared_ptr<org::services::LivePipelineSlot> slot;  // built's (AdoptLivePipelines)
				// Earlier generations' builds, and those a shader reload replaced, kept for the process: a recording may still draw with them.
				std::vector<org::services::PipelinePayload> retired;
				struct Counters
				{
					std::atomic<std::uint32_t> requested{ 0 }, ready{ 0 }, failed{ 0 };
				};
				Counters counters;
			};
			std::array<FamilyLane, kDrawFamilyLanes> families;  // by draw family
			/** @brief The refraction pipeline, rebuilt with each new main set (RecreateMainSet) or new normals format / write mask. */
			struct Refraction
			{
				bool asked = false;          // RequestRefraction has arrived
				bool requested = false;      // this build is queued
				bool failed = false;         // this build failed, or the program did
				bool programFailed = false;  // for good
				DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;  // the format requested or built for
				org::services::PipelinePayload built;
				std::shared_ptr<org::services::LivePipelineSlot> slot;  // built's (AdoptLivePipelines)
				// Earlier builds, and those a shader reload replaced, kept for the process: a recording may still draw with them.
				std::vector<org::services::PipelinePayload> retired;
				struct Counters
				{
					std::atomic<std::uint32_t> requested{ 0 }, ready{ 0 }, failed{ 0 };
				};
				Counters counters;
				/** @brief Starts over: the build is retired (kept for the process), the program's verdict kept. */
				void Reset();
			};
			Refraction refraction;
			/**
			 * @brief The forward pipelines, built for the forward targets (rebuilt by RecreateForward). No set: recordings bind each by
			 * handle, so every build is kept for the process. Programs by ShaderPrograms::ForwardProgramId, a draw family's by its lane slot's
			 * forwardProgramId.
			 */
			struct Forward : PipelineFamily<ForwardPipelineKey, ForwardPipelineKeyHash, ForwardEntry, ankerl::unordered_dense::set<ForwardPipelineKey, ForwardPipelineKeyHash>>
			{
				std::vector<org::services::PipelinePayload> retired;  // earlier targets' builds, kept for the process
			};
			Forward forward;
			std::uint64_t servicePass = 0;  // PublishReady's fence value: the service's displaced payloads retire at once (DrainLane)
		};
		Lane lane;

		/**
		 * @brief Publishes a family's admitted pipelines (Versions::Publish) and hands out the published indices (a_onReady per key).
		 * Returns what the publication did (Published: a new version is the published one).
		 */
		template <class Family, class Version, class Publish, class OnReady>
		typename Versions<Version>::PublishResult PublishAdmitted(Family& a_family, Versions<Version>& a_versions, std::uint32_t a_admitted, const char* a_what, Publish&& a_publish, OnReady&& a_onReady)
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
			return result;
		}

		/** @brief The lane's pass (SetPipelineLaneDrain). */
		static void DrainLaneThunk();
		void DrainLane();
		/** @brief The lane: requests a_key's program, then its pipeline, or parks it until what it lacks arrives. */
		void TryRequest(const PipelineKey& a_key);
		/** @brief TryRequest's warning for a build it requested (a_slot: the pipeline slot that asked), with its nearest requested relative. */
		void LogOnDemandBuild(const PipelineKey& a_key, Api::ProgramFamily a_family, std::uint32_t a_slot) const;
		/** @brief The lane: the main pass's targets changed; every key is built again into a new set. */
		void RecreateMainSet(const TargetFormats& a_from, const TargetFormats& a_to);
		/** @brief RecreateMainSet's: the main set, the draw families' and refraction started over (a_by: why, for the warnings). */
		void ResetMainSet(std::string a_by);
		/** @brief RecreateShadowSet's: the shadow set started over. */
		void ResetShadowSet(std::string a_by);
		/** @brief RecreateForward's: the forward pipelines started over, the built ones kept for the process. */
		void ResetForward();
		/**
		 * @brief The lane, when a shader reload moved the sources (ShaderPrograms::FollowSources dropped its program entries): every key
		 * parked on a program, and every key whose program or build failed, requested again with the inputs (DrainLane's RetryBlocked).
		 */
		void RetryAfterSourcesMoved();
		/**
		 * @brief The lane, each pass before admitting: once PipelineRegistry::Generation moved, every pipeline whose slot a reload swapped
		 * takes the slot's payload where the lane keeps it (the sets' indices, the forward entries, the draw families, refraction). A set
		 * that changed gets new versions (Versions::Retire; the next publication applies every index from the new payloads, its state
		 * classes and Z-prepass groups taken again), the catalog is published again with them, and the scene lane's next revision binds
		 * them. The one reload step for every kind.
		 */
		void AdoptLivePipelines();
		/** @brief AdmitLane's and ReindexMainSet's: a_index's Z-prepass group and state class (AdmitToClass), in admission order. */
		void IndexMainBuilt(std::uint32_t a_index, const Built& a_built);
		/** @brief AdoptLivePipelines': the main set's groups and classes made again from its pipelines, in index order. */
		void ReindexMainSet();
		/** @brief The lane: admits completed builds, publishes a set version, hands out their indices. */
		void AdmitLane();
		/** @brief The lane: TryRequest for a shadow view key. */
		void TryRequestShadow(const ShadowPipelineKey& a_key);
		/** @brief The lane: the shadow map format changed; every shadow key is built again into a new set. */
		void RecreateShadowSet(DXGI_FORMAT a_from, DXGI_FORMAT a_to);
		/** @brief The lane: AdmitLane for the shadow views' set. */
		void AdmitShadow();
		/** @brief The lane: TryRequest for a draw family's pipelines, once asked for. */
		void TryRequestFamily(Api::DrawFamily a_family);
		/** @brief The lane: TryRequestFamily for every draw family. */
		void TryRequestFamilies();
		/** @brief The lane: the draw families RequestFamily has asked for since the last pass, asked from here on. */
		void TakeFamilyRequests();
		/** @brief The lane: the draw families' keys and pipelines that waited on the programs a_finished names, tried again. */
		void RetryFamilyPrograms(const ShaderPrograms::Finished& a_finished);
		/** @brief The lane: TryRequest for a forward key. */
		void TryRequestForward(const ForwardPipelineKey& a_key);
		/** @brief The lane: TryRequest for the refraction pipeline, once asked for. */
		void TryRequestRefraction();
		/** @brief The lane: the forward targets changed; every forward key is built again. */
		void RecreateForward(const ForwardTargets& a_from, const ForwardTargets& a_to);
		/** @brief The lane: admits completed draw family and forward builds. */
		void AdmitFamiliesAndForward();
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

		bool Initialize();

		/** @brief Initialize's command signatures (false when a required one could not be made). */
		bool CreateDrawSignatures();

		/**
		 * @brief A triangle-list graphics pipeline description (no pixel stage when a_pixel is empty), without vertex input; Create throws
		 * on failure.
		 */
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
			rhi::SubobjInputLayout input{};  // always empty: the vertex stages pull their attributes (CheckVertexAttributes)
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
			/**
			 * @brief Its state class's part (PipelineStateDesc): everything Create passes but the two modules, and the fragment interface of
			 * its pixel module (a_pixel, that module reflected).
			 */
			PipelineStateDesc State(const SpirvReflection& a_pixel) const
			{
				PipelineStateDesc state;
				state.raster = raster.rs;
				state.depth = depth.ds;
				state.blend = blend.bs;
				state.targets = targets.rt;
				state.depthFormat = depthFormat.dsv;
				state.topology = rhi::PrimitiveTopology::TriangleList;  // Create's
				state.flags = flags.flags;
				state.hasPixel = hasPixel;
				if (hasPixel) {
					const std::span<const std::byte> module(static_cast<const std::byte*>(pixel.bytecode.data), pixel.bytecode.size);
					state.outputLocations = a_pixel.outputLocations;
					FragmentExports(module, state.exportsDepth, state.exportsSampleMask);
					state.defersDepth = FragmentDefersDepth(module);
				}
				return state;
			}
			void Create(rhi::Device a_device, rhi::PipelinePtr& a_out, std::string_view a_what) const
			{
				const rhi::SubobjPrimitiveTopology topology{ rhi::PrimitiveTopology::TriangleList };
				const rhi::PipelineStreamItem items[] = {
					rhi::Make(layout), rhi::Make(vertex), rhi::Make(raster), rhi::Make(depth), rhi::Make(blend), rhi::Make(targets), rhi::Make(depthFormat),
					rhi::Make(topology), rhi::Make(input), rhi::Make(flags), rhi::Make(pixel),
				};
				// DXVK_PIPELINE_STATS: the pipeline's statistics are logged under its name.
				const std::string label = fmt::format("DCLF {}", a_what);
				rhi::vulkan::set_pipeline_statistics_label(label.c_str());
				const auto result = a_device.CreatePipeline(items, static_cast<std::uint32_t>(std::size(items)) - (hasPixel ? 0u : 1u), a_out);
				rhi::vulkan::set_pipeline_statistics_label(nullptr);
				// DXVK_SHADER_DUMP_PATH: the pixel stage's SPIR-V beside DXVK's dumps, named by the label.
				if (static const char* dump = std::getenv("DXVK_SHADER_DUMP_PATH"); dump && *dump && hasPixel && pixel.bytecode.data) {
					std::string file = label;
					for (auto& c : file)
						if (!std::isalnum(static_cast<unsigned char>(c)))
							c = '_';
					std::ofstream(std::filesystem::path(dump) / (file + ".fs.spv"), std::ios::binary)
						.write(static_cast<const char*>(pixel.bytecode.data), static_cast<std::streamsize>(pixel.bytecode.size));
				}
				if (result != rhi::Result::Ok)
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

		/**
		 * @brief A draw family's pipelines (TryRequestFamily), as Api::DrawFamilyBuild::DepthThenEqualColour builds them; named with its
		 * label ("tree LOD colour", "tree LOD depth").
		 */
		static org::services::PipelinePayload BuildDrawFamily(rhi::Device a_device, rhi::PipelineLayoutHandle a_zLayout, const ShaderPrograms::DrawProgram* a_program,
			TargetFormats a_targets, rhi::BlendState a_blend, bool a_frontCCW, std::string_view a_label);

		/**
		 * @brief The refraction pipeline: one attachment (FUN_1414b4860 binds kREFRACTION_NORMALS alone as RT0), main depth LESS_EQUAL
		 * without writes (its depth mode 1), write mode 2's mask, no culling: the pixel stage discards one-sided back faces itself.
		 */
		static org::services::PipelinePayload BuildRefraction(rhi::Device a_device, rhi::PipelineLayoutHandle a_layout, const ShaderPrograms::ShadowProgram* a_program,
			DXGI_FORMAT a_format, DXGI_FORMAT a_depthFormat, std::uint8_t a_writeMask, bool a_frontCCW);

		/** @brief A forward pipeline. A second target (first-person motion vectors) is written unblended: those members are opaque. */
		static org::services::PipelinePayload BuildForward(rhi::Device a_device, rhi::PipelineLayoutHandle a_zLayout, const ShaderPrograms::ForwardProgram* a_program,
			ForwardTargets a_targets, rhi::CullMode a_cull, bool a_frontCCW);

		/**
		 * @brief A ForwardOrDecal family key's pipelines (Api::ProgramBuild::ForwardOrDecal: the Effect port's keys), following Build's
		 * forward or decal rules (a decal uses the DEFERRED pixel stage). An attachment the pixel stage does not output keeps its contents
		 * (write mask 0), as DXVK builds the engine's pipeline: DEFERRED without Normals/MotionVectorsNormals writes neither. Its Z key is
		 * never shared (a_slot's salt); the depth variant of a forward key is never drawn (no Z-prepass bucket: PlanZBuckets). Named by
		 * a_keyName ("effect color", "effect depth").
		 */
		static org::services::PipelinePayload BuildForwardOrDecal(const ProgramFamilySlot& a_slot, std::string_view a_keyName, rhi::Device a_device,
			rhi::PipelineLayoutHandle a_layout, rhi::PipelineLayoutHandle a_zLayout, PipelineKey a_key, const ShaderPrograms::Program* a_program, TargetFormats a_targets,
			EngineState a_state, bool a_frontCCW);
		/**
		 * @brief A forward key's plain colour draws' pipeline into a_built (Built::pulledColour): a_vertex and a_pixel (pulled) under a_zLayout,
		 * with a_colour's state. A pulled stage that would not build leaves it null, logged (a few): the key's other pipelines stand.
		 */
		static void BuildPulledColour(rhi::Device a_device, rhi::PipelineLayoutHandle a_zLayout, const std::vector<std::byte>& a_vertex, const std::vector<std::byte>& a_pixel,
			const PipelineDesc& a_colour, std::string_view a_name, Built& a_built, rhi::PipelinePtr Built::* a_into = &Built::pulledColour);

		static org::services::PipelinePayload Build(rhi::Device a_device, rhi::PipelineLayoutHandle a_layout, rhi::PipelineLayoutHandle a_zLayout, PipelineKey a_key,
			const ShaderPrograms::Program* a_program, TargetFormats a_targets, EngineState a_state, bool a_frontCCW);

		/**
		 * @brief One shadow pipeline: the Utility permutation, depth only (write, LESS, as the engine's shadow pass), with the view's
		 * bias, cull and winding. A two-sided caster draws without culling, as the engine's Utility pass does.
		 */
		static org::services::PipelinePayload BuildShadow(rhi::Device a_device, rhi::PipelineLayoutHandle a_layout, ShadowPipelineKey a_key,
			const ShaderPrograms::ShadowProgram* a_program, DXGI_FORMAT a_depthFormat, ShadowRasterState a_state);

		// DrawSequence's command signature (Records.h), created with the class's set it selects from.
		bool CreateSignature(SetVersion::ClassSets& a_class, std::uint32_t a_variant);
		/** @brief The lane (AdmitLane): a_index's place in its state class (Built::stateClass), the class made when it is the first of it. */
		void AdmitToClass(const Built& a_built, std::uint32_t a_index);
		/** @brief The lane (AdmitLane's publish): every class's sets of a_version brought up to its members among [0, a_admitted). */
		bool ApplyClasses(SetVersion& a_version, std::uint32_t a_admitted);
		/**
		 * @brief The lane, CS_DCLF_SET_PARITY (AdmitLane, a version just published): every class of a_version against its members. Each
		 * member's class word round-trips, its state equals its class's initial pipeline's (SameState: one that differs is drawn with the
		 * initial's fixed-function state), the handle written at its index is its own, every pipeline is in exactly one class, and no two
		 * classes are one state. Counted in Lane::classParity, each failure logged at once.
		 */
		void CheckClasses(const SetVersion& a_version);
		/**
		 * @brief CheckClasses' members of one class a_class of a_version (a_members: the lane's, in admission order): each one's class word,
		 * its state against the class's initial pipeline's, the handles written at its index; a_seen counts each pipeline's classes.
		 */
		template <class Check>
		static void CheckClassMembers(const SetVersion& a_version, std::uint32_t a_class, const std::vector<std::uint32_t>& a_members,
			std::vector<std::uint8_t>& a_seen, Check&& a_check);

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
}
