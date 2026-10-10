#if defined(CS_HAS_RENDER_GRAPH) && defined(CS_HAS_ORG_MODULE_SERVICES)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "BuildExecutor.h"
#	include "DrawPipelines.h"
#	include "DrawPipelinesRhi.h"
#	include "Features/DrawcallLimitFix/Engine/EngineStates.h"
#	include "Features/DrawcallLimitFix/Common/EventQueue.h"
#	include "Features/DrawcallLimitFix/Common/LatestSlot.h"
#	include "Features/DrawcallLimitFix/Common/Switches.h"

#	include "RenderGraph/RenderGraphRuntime.h"
#	include "SpirvReflection.h"
#	include "Features/DrawcallLimitFix/Scene/LightingConstants.h"
#	include "Features/DrawcallLimitFix/Scene/LightingDescriptors.h"
#	include "Features/DrawcallLimitFix/Scene/Lookups.h"
#	include "Features/DrawcallLimitFix/Scene/VertexInput.h"
#	include "Features/DrawcallLimitFix/Scene/SceneStore.h"
#	include "ShaderCache.h"

#	include <Tracy/Tracy.hpp>

#	include <OpenRenderGraph/PersistentGraphHost.h>
#	include <ORGModuleServices/PipelineService.h>
#	include <rhi_helpers.h>

#	include <algorithm>
#	include <atomic>
#	include <limits>
#	include <stdexcept>
#	include <string_view>

namespace DCLF
{
	namespace
	{
		// The pipeline lane's frame inputs (render thread -> lane) and its catalogs (lane -> the scene lane) go through a LatestSlot
		// (Common/LatestSlot.h).

		/**
		 * @brief Whether a fragment program can keep a depth-writing draw from testing depth before it shades: it discards
		 * (OpKill, OpTerminateInvocation, OpDemoteToHelperInvocation), writes the depth or the sample mask, or writes memory.
		 * Such a pipeline is of a shadow view's discarding class (DrawPipelines::ShadowDiscards), drawn after the others.
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

		// Register classes as shifted by the SPIR-V builds (ShaderPrograms.h); set 0.
		// Push data. The DCLF_BINDLESS builds declare a cbuffer here to read the object index out of it;
		// the address words beside it are still consumed by the layout's indirect ranges, not by a shader.
		constexpr std::uint32_t kRecordAddressBinding = kDrawPushBinding;
		// The texture registers the vertex stage may declare: the shading rows (a tree's wind until its entry: T6b1a), the palettes, the
		// placement rows, the trees' wind, the character light's noise (pixel only, but in the run), the extras and the per-object record
		// buffer.
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
					// The layout maps the whole t range for the pixel stage, and the object buffer alone for
					// the vertex stage - which is all the vertex half of DCLF_BINDLESS needs.
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

		// A pulled Lighting stage (DCLF_PULLED) reads its rows' inputs itself: none of its bindings may be one the Z-prepass's
		// layout leaves out (a row's: the PerTechnique, PerMaterial, PerGeometry and permutation blocks, the material textures and
		// samplers, the shadow mask).
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
		 * @brief What a main pipeline's Z-prepass pipeline is (Built::zKey): its pulled stages, by their SPIR-V, and its depth
		 * state. Pipeline slots whose keys build the same one share a depth pipeline, and with it a Z-prepass bucket and draw call
		 * (IndirectState::zGroups): everything else a slot's key selects - its rows, its vertex layout - each draw reads itself.
		 * The pixel stage is none where the depth pixel stage cannot defer the depth test (FragmentDefersDepth): it only runs
		 * the alpha test, and without one it has nothing to do.
		 */
		struct ZPipelineKey
		{
			std::uint64_t vertex = 0, pixel = 0;  // wyhash of the modules; pixel 0 without a pixel stage
			std::uint32_t vertexBytes = 0, pixelBytes = 0;
			std::uint32_t cull = 0, depthWrite = 0, depthFunc = 0;
			std::uint32_t depthBias = 0, depthBiasClamp = 0, slopeScaledDepthBias = 0;  // the floats' bits

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

		// The Lighting variables by index (ShaderConstants::LightingVS, LightingPS), named as ShaderCache.cpp's GetVariableIndices
		// matches the game's shaders' cbuffer variables by.
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
		 * @brief A stage's constant table (ConstantTables) from its modules: each Lighting variable their PerTechnique, PerMaterial or
		 * PerGeometry block has, at its offset in floats, as ShaderCache's ReflectConstantBuffers makes it of a game shader's D3D
		 * reflection; 0 where none has it. The modules are one stage's builds of one permutation (the colour and the Z-prepass pixel
		 * stages), compiled from the same declarations (Lighting.hlsl places every member with packoffset), so a variable two of them
		 * have is at one offset. The pulled stages declare no blocks: they read the rows these tables pack (PulledLightingSource).
		 */
		std::vector<std::uint8_t> ConstantTableOf(std::initializer_list<const SpirvReflection*> a_modules, bool a_pixel, const PipelineKey& a_key)
		{
			static std::atomic<std::uint32_t> logged{ 0 };
			const auto names = LightingNames(a_pixel);
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
			// The stride is dynamic (set per draw by the VertexBuffer argument).
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
				// Stream 1 is a dynamic shape's positions (BSDynamicTriShape::dynamicData, float4 a vertex): every draw
				// binds a second vertex buffer (DrawSequence), a face shape's positions or its own buffer again.
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
	}

	struct DrawPipelines::Impl
	{
		struct Entry
		{
			std::uint32_t index = kNotReady;  // handed out once a published set version holds the pipeline
			std::uint32_t slot = kNotReady;   // its index in the sets, from its admission
			bool failed = false;
		};
		/** @brief A key asked of the pipeline lane (DrawPipelines::RequestLighting), with the pipeline slot that asked. */
		struct LightingRequest
		{
			PipelineKey key{};
			std::uint32_t slot = ~0u;
		};
		/**
		 * @brief A shadow view key asked of the pipeline lane (DrawPipelines::RequestShadow), with the casters that asked: their key
		 * slot's key (no mode bits, no view state) and their occlusion view (~0u for a shadow view's), for the on-demand warnings.
		 */
		struct ShadowRequest
		{
			ShadowPipelineKey viewKey{};
			ShadowPipelineKey casterKey{};
			std::uint32_t occlusion = ~0u;
		};

		/**
		 * @brief A version of an indirect pipeline set group and its command signatures, holding the admitted pipelines
		 * [0, applied).
		 *
		 * The frames record with the published version and keep it (IndirectState::version) until they are recorded.
		 * Update writes new pipelines only into a version nothing else references, at indices it never held, and then
		 * publishes it. So a set is never written while a recording reads it or sizes preprocess memory against it
		 * (VUID-VkGeneratedCommandsInfoEXT-preprocessSize-11071: the size depends on the set's pipelines), and no entry
		 * that submitted work may use is ever rewritten (VUID-VkWriteIndirectExecutionSetPipelineEXT-index-11029).
		 */
		struct SetVersion
		{
			std::array<rhi::IndirectPipelineSetPtr, kVariantCount> sets;  // one per variant; a key has the same index in both
			std::array<rhi::CommandSignaturePtr, kVariantCount> signatures;
			// The pipelines of [0, applied), held for as long as a recording holds the version (IndirectState::zPipelines).
			std::vector<org::services::PipelinePayload> pipelines;
			// Each pipeline's Z-prepass group (Impl::zGroupOf), and each group's first pipeline, whose pulled depth pipeline is
			// the group's (IndirectState::zGroups, zPipelines).
			std::vector<std::uint32_t> zGroupOf, zGroupFirst;
			std::uint32_t applied = 0;
		};
		// The shadow views' pipelines, versioned the same way (the lane's too): their plain draws bind each by its index
		// (ShadowIndirectState).
		struct ShadowSetVersion
		{
			// The pipelines of [0, applied), held for as long as a recording holds the version (ShadowIndirectState::pipelines),
			// with their keys' vertex layouts and their classes.
			std::vector<org::services::PipelinePayload> pipelines;
			std::vector<std::uint64_t> layouts;
			std::vector<std::uint8_t> discards;
			std::uint32_t applied = 0;
		};
		// As many versions as are held at once: the published one, the ones frames still being recorded hold, and the ones kept
		// recordings hold (ORG's recording reuse keeps an epoch's recordings, and with them the version they bound, for as long
		// as they may be submitted again). A new one is made when none is free, so a new pipeline never waits for a recording to
		// let go of an older version.
		template <class Version>
		struct Versions
		{
			std::vector<std::shared_ptr<Version>> owned;
			std::atomic<std::shared_ptr<Version>> published;
			// Versions of a previous pipeline generation (a target or format change), dropped here once nothing holds them.
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

			/**
			 * @brief Brings a version nothing references up to a_admitted pipelines (a_apply writes [applied, a_admitted)
			 * into it) and publishes it.
			 */
			template <class Apply>
			PublishResult Publish(std::uint32_t a_admitted, Apply&& a_apply)
			{
				std::erase_if(retired, [](const std::shared_ptr<Version>& a_version) { return a_version.use_count() == 1; });
				const auto current = published.load();
				if (current && current->applied == a_admitted)
					return PublishResult::Current;
				// A free one (its last version is nobody's, and nobody can take it again until it is published), or a new one.
				auto free = std::ranges::find_if(owned, [&](const std::shared_ptr<Version>& a_version) {
					return !a_version || (a_version != current && a_version.use_count() == 1);
				});
				if (free == owned.end())
					free = owned.insert(owned.end(), nullptr);
				{
					auto& version = *free;
					// Every other owner has let go, and none can take it again until it is published: whatever a recording
					// did with it happened before this.
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
		// Under frame push, the shadow views' layout: every register from the draw's binding record. Their pass-wide blocks
		// (the view slot's VS_PerFrame at b12, the build's SharedData and FeatureData at b5 and b6) are not the main pass's
		// frame slots. Without frame push the shadow views use the main layout.
		rhi::PipelineLayoutPtr shadowLayout;
		// The Z-prepass's plain draws (IndirectState::zPipelines): their layout and a DrawSequence tail's signature.
		rhi::PipelineLayoutPtr zLayout;
		rhi::CommandSignaturePtr zDrawSignature;
		// Tree LOD (RequestTreeLod): its draw's signature, a plain DrawInstanced under zLayout. Its pipelines are the lane's (Lane::TreeLod).
		rhi::CommandSignaturePtr treeLodDrawSignature;
		struct TreeLodBuilt
		{
			rhi::PipelinePtr depth, colour;
		};
		// The render thread's: what it has asked of the lane already (RequestTreeLod, RequestForward), so it asks each once.
		bool treeLodAsked = false;
		ankerl::unordered_dense::set<ForwardPipelineKey, ForwardPipelineKeyHash> forwardAsked;
		rhi::PipelineLayoutHandle ShadowLayout() const { return shadowLayout->GetHandle(); }
		// The shadow views' plain indirect draw (ShadowIndirectState::drawSignature): a DrawSequence's last words read as an
		// indexed draw from the index pool (BuildDrawsCS, StoreShadowSequence), whose vertex stage pulls its vertices.
		rhi::CommandSignaturePtr shadowDrawSignature;
		bool supported = false;
		bool attempted = false;
		org::services::PipelineService service;
		/*
		 * The builds' completions: each build's callback pushes its artifact (on the thread that built it, or at once on the
		 * requesting thread for a pipeline the service already holds) and wakes the pipeline lane, which drains them. A completion
		 * carries the generation it was requested in (the main set's for the main set and tree LOD, the shadow set's, the forward
		 * pipelines'): one from before a target or format change is dropped.
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
		struct Completions
		{
			EventQueue<MainDone, 1024> main;
			EventQueue<ShadowDone, 1024> shadow;
			EventQueue<TreeLodDone, 16> treeLod;
			EventQueue<ForwardDone, 256> forward;
			// The pipeline lane's requests (RequestLighting, RequestShadow, RequestForward, RequestTreeLod: a flag, once is enough), and
			// its requests to the render thread: the engine state bits it needs read (CaptureEngineStates replies with the frame's inputs).
			EventQueue<LightingRequest, 1024> requests;
			EventQueue<ShadowRequest, 1024> shadowRequests;
			EventQueue<ForwardPipelineKey, 256> forwardRequests;
			std::atomic<bool> treeLodRequest{ false };
			EventQueue<std::uint32_t, 64> stateRequests;
		};
		// Never freed: a build may complete while the process tears down.
		Completions* completions = new Completions;

		// The builds run on DCLF's preparation pool (BuildExecutor.h), not a thread each.
		Impl() { service.Configure(BuildSubmitter()); }

		// The shadow views' own set (one pipeline per Utility technique, vertex layout and view rasterizer state, depth only, into
		// the engine's shadow map format) is the pipeline lane's: Lane::Shadow. The rasterizer states' registry is the render
		// thread's: by id - 1 (ShadowRasterStateId); the lane builds from the states posted with the frame's inputs
		// (FrameInput::shadowStates).
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
		 * @brief Which face the engine's rasterizer states make the front one (FrontCounterClockwise), read
		 * once from its table on the render thread; empty until the table holds a state. Every entry of the
		 * table has the same winding, and the per-cascade copies Community Shaders swaps in are copies of it.
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
		// What a build produces: the pipelines of both variants and the registers the shaders read.
		struct Built
		{
			std::array<rhi::PipelinePtr, kVariantCount> pipelines;
			// Per variant: the Z-prepass build of the pixel stage reads far less than the colour one, and a
			// draw only has to supply what its own variant declares.
			std::array<RegisterUsage, kVariantCount> usage;
			bool discards = false;  // a shadow pipeline whose pixel stage defers the depth test (FragmentDefersDepth)
			// A main pipeline's: its Z-prepass's plain-draw pipeline (the pulled stages, IndirectState::zPipelines), and what it
			// is (ZPipelineKey), by which the pipelines that build the same one share it.
			rhi::PipelinePtr pulledDepth;
			ZPipelineKey zKey;
			std::shared_ptr<const ConstantTables> tables;  // a main pipeline's stages' (ConstantTableOf)
		};

		/**
		 * @brief The engine's fixed-function state behind a key's state bits (RasterStateBits), read once
		 * from its D3D11 state objects and kept in RHI terms so the asynchronous build never touches D3D11.
		 */
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
		// The main pass's opaque groups' write modes (alphaBlendWriteMode), as the engine draws them: the alpha-tested group
		// (accumulation hint 2) writes every channel (mode 10); the plain opaque group (hint 0) leaves the first target's
		// alpha alone (mode 1), and a shader's alpha below 1 (vertex alpha, say) never reaches the G-buffer.
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
			// Fill solid, cull back, no scissor: the pipeline's own cull mode comes from the key, and the
			// bias values are the same across the cull modes of the engine's table.
			auto* raster = EngineRasterStates()[0][1][bias][0];
			// The deferred pass's variant: DCLF's main-pass draws are G-buffer draws, whenever this is read.
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

		/**
		 * @brief The frame's inputs to the pipeline lane (render thread -> lane, latest-wins, PostInput): what its builds need that
		 * only the render thread can read. Each post is whole, so the lane never sees half of one.
		 */
		struct FrameInput
		{
			TargetFormats targets;
			std::uint32_t targetsGeneration = 0;  // DrawPipelines::generation, whose targets these are
			// The engine states the render thread read (CaptureEngineStates), by RasterStateBits: every one asked for, valid or not.
			std::shared_ptr<const ankerl::unordered_dense::map<std::uint32_t, EngineState>> states;
			std::optional<bool> frontCCW;      // EngineFrontCCW, once the engine's table holds a state
			RE::BSShader* lighting = nullptr;  // the Lighting shader the programs' defines are of (ConstantEvaluator)
			// The shadow views' (SetShadowInputs, ShadowRasterStateId): the Utility shader their programs build, the shadow map format
			// their pipelines draw into, and the registered view rasterizer states (by id - 1; append-only, so a newer list holds every
			// id an older one did).
			RE::BSShader* utility = nullptr;
			DXGI_FORMAT shadowFormat = DXGI_FORMAT_UNKNOWN;
			std::shared_ptr<const std::vector<ShadowRasterState>> shadowStates;
			// Tree LOD's (SetTreeLodInputs): the DistantTree shader its programs and the forward tree program build. The forward views'
			// (SetForwardTargets): the targets their pipelines draw into.
			RE::BSShader* distantTree = nullptr;
			ForwardTargets forwardTargets;
		};
		LatestSlot<FrameInput> inputSlot;
		FrameInput renderInput;                       // render thread: what it posts, as last posted
		std::vector<std::uint32_t> pendingStateBits;  // render thread: asked for by the lane, not readable yet (no deferred pass yet)
		LatestSlot<PipelineCatalog> catalogSlot;      // lane -> the scene lane (TakeCatalog)

		/** @brief Render thread: posts the frame's inputs as they are now (the owner's targets with them) and wakes the lane. */
		void PostInput(const DrawPipelines& a_owner)
		{
			renderInput.targets = a_owner.targets;
			renderInput.targetsGeneration = a_owner.generation;
			inputSlot.Post(std::make_unique<FrameInput>(renderInput));
			WakePipelineLane();
		}

		/**
		 * @brief The pipeline lane's own state (T6b2c step 4): touched only by its passes (DrainLane), one at a time on DCLF's
		 * coordinator, but its counters, which the report reads.
		 */
		struct Lane
		{
			std::shared_ptr<const FrameInput> input;  // the newest taken
			std::uint32_t generation = 0;             // the main set's (PipelineCatalog::generation)
			// The keys requested from the service, or failed for good (their program), and how many failed.
			ankerl::unordered_dense::map<PipelineKey, Entry, PipelineKeyHash> entries;
			std::uint32_t failedEntries = 0;
			// Every key asked for, with the slot that first asked: requested again whole after a target change.
			ankerl::unordered_dense::map<PipelineKey, std::uint32_t, PipelineKeyHash> asked;
			// The keys waiting on their program (by ShaderPrograms::LightingProgramId), retried when UpdateLane finishes it; and the
			// keys waiting on an input (the targets, the winding, an engine state, the set's room), retried when the inputs change.
			ankerl::unordered_dense::map<std::uint64_t, std::vector<PipelineKey>> programWaiters;
			std::vector<PipelineKey> blocked;
			ankerl::unordered_dense::set<std::uint32_t> statesAsked;  // engine state bits asked of the render thread
			ShaderPrograms::Finished finishedPrograms;               // UpdateLane's, per pass (every kind's)
			Versions<SetVersion> versions;
			std::vector<org::services::PipelinePayload> setPipelines;  // index -> Built, admitted (not necessarily published)
			// The admitted pipelines' Z-prepass groups: by index, each pipeline's (its Built::zKey's, numbered as first admitted),
			// and by group, its first pipeline.
			std::vector<std::uint32_t> zGroupOf, zGroupFirst;
			ankerl::unordered_dense::map<ZPipelineKey, std::uint32_t, ZPipelineKeyHash> zGroups;
			std::vector<std::array<RegisterUsage, kVariantCount>> usage;  // by set index, then variant
			std::vector<std::shared_ptr<const ConstantTables>> constantTables;  // by set index
			std::string recreatedBy;  // the set's last recreation, for NearestKey's warnings
			std::uint32_t loggedFailures = 0;
			std::uint64_t revision = 0;
			bool dirty = false;  // the catalog to publish at the end of the pass
			struct Counters
			{
				std::atomic<std::uint32_t> requested{ 0 }, ready{ 0 }, failed{ 0 }, zPipelines{ 0 }, zDepthOnly{ 0 }, setPublishes{ 0 }, setWaits{ 0 };
			};
			Counters counters;
			/**
			 * @brief The shadow views' set (T6b2c step 6), kept as the main set is: keyed by the view key (ShadowPipelineKey with its
			 * mode's bits and its view state), built for the inputs' shadow map format, published in its own set versions.
			 */
			struct Shadow
			{
				std::uint32_t generation = 0;  // PipelineCatalog::shadowGeneration: a format change starts a new set
				ankerl::unordered_dense::map<ShadowPipelineKey, Entry, ShadowPipelineKeyHash> entries;
				std::uint32_t failedEntries = 0;
				// Every key asked for, with the casters that first asked: requested again whole after a format change.
				ankerl::unordered_dense::map<ShadowPipelineKey, ShadowRequest, ShadowPipelineKeyHash> asked;
				// The keys waiting on their program (by Utility technique), retried when UpdateLane finishes it; and the keys waiting on
				// an input (the Utility shader, the format, their view state, the set's room), retried when the inputs change.
				ankerl::unordered_dense::map<std::uint32_t, std::vector<ShadowPipelineKey>> programWaiters;
				std::vector<ShadowPipelineKey> blocked;
				Versions<ShadowSetVersion> versions;
				// By index, admitted (not necessarily published): the pipeline, its key's vertex layout and its class (kShadowDiscards).
				std::vector<org::services::PipelinePayload> setPipelines;
				std::vector<std::uint64_t> layouts;
				std::vector<std::uint8_t> discards;
				std::string recreatedBy;  // the set's last recreation, for NearestShadowKey's warnings
				std::uint32_t loggedFailures = 0;
				struct Counters
				{
					std::atomic<std::uint32_t> requested{ 0 }, ready{ 0 }, failed{ 0 }, setPublishes{ 0 }, setWaits{ 0 };
				};
				Counters counters;
			};
			Shadow shadow;
			/**
			 * @brief Tree LOD's two pipelines (T6b2c step 9), built for the main set's targets: again with each new main set
			 * (RecreateMainSet), their completion dropped by the main set's generation.
			 */
			struct TreeLod
			{
				bool asked = false;          // RequestTreeLod has arrived: built from here on, for every generation
				bool requested = false;      // this generation's build is queued (its completion: Completions::treeLod)
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
			/**
			 * @brief The forward views' pipelines (T6b2c step 9), keyed as asked (ForwardPipelineKey), built for the inputs' forward
			 * targets: again with new ones (RecreateForward). Every built pipeline is kept for the process (no set: a recording binds
			 * each by its handle).
			 */
			struct Forward
			{
				std::uint32_t generation = 0;  // a forward targets change starts them over
				struct Entry
				{
					org::services::PipelinePayload built;
					bool requested = false;  // its build is queued, or done
					bool failed = false;     // its program or its build failed, for good (this generation)
				};
				// The keys whose program the lane has (and so their pipeline requested), or found failed.
				ankerl::unordered_dense::map<ForwardPipelineKey, Entry, ForwardPipelineKeyHash> entries;
				// Every key asked for: requested again whole after a targets change.
				ankerl::unordered_dense::set<ForwardPipelineKey, ForwardPipelineKeyHash> asked;
				// The keys waiting on their program (by ShaderPrograms::ForwardProgramId, the forward tree's by kTreeProgram), retried when
				// UpdateLane finishes it; and the keys waiting on an input (the targets, the winding, the shader), retried when the inputs change.
				static constexpr std::uint64_t kTreeProgram = ~0ull;
				ankerl::unordered_dense::map<std::uint64_t, std::vector<ForwardPipelineKey>> programWaiters;
				std::vector<ForwardPipelineKey> blocked;
				std::vector<org::services::PipelinePayload> retired;  // earlier targets' builds, kept for the process
				std::uint32_t loggedFailures = 0;
				struct Counters
				{
					std::atomic<std::uint32_t> requested{ 0 }, ready{ 0 }, failed{ 0 };
				};
				Counters counters;
			};
			Forward forward;
		};
		Lane lane;

		/** @brief The lane's pass (SetPipelineLaneDrain). */
		static void DrainLaneThunk();
		void DrainLane();
		/** @brief The lane: requests a_key's program and then its pipeline, or parks it until what it lacks arrives. */
		void TryRequest(const PipelineKey& a_key);
		/** @brief The lane: the main pass's targets changed; every key is built again into a new set. */
		void RecreateMainSet(const TargetFormats& a_from, const TargetFormats& a_to);
		/** @brief The lane: the builds that completed, admitted, published in a set version, and their indices handed out. */
		void AdmitLane();
		/** @brief The lane: requests a shadow view key's program and then its pipeline, or parks it until what it lacks arrives. */
		void TryRequestShadow(const ShadowPipelineKey& a_key);
		/** @brief The lane: the shadow map format changed; every shadow key is built again into a new set. */
		void RecreateShadowSet(DXGI_FORMAT a_from, DXGI_FORMAT a_to);
		/** @brief The lane: AdmitLane for the shadow views' set. */
		void AdmitShadow();
		/** @brief The lane: requests tree LOD's program and then its pipelines once asked for, or leaves them until what they lack arrives. */
		void TryRequestTreeLod();
		/** @brief The lane: requests a forward key's program and then its pipeline, or parks it until what it lacks arrives. */
		void TryRequestForward(const ForwardPipelineKey& a_key);
		/** @brief The lane: the forward targets changed; every forward key is built again. */
		void RecreateForward(const ForwardTargets& a_from, const ForwardTargets& a_to);
		/** @brief The lane: tree LOD's and the forward pipelines' builds that completed. */
		void AdmitTreeLodAndForward();
		void PublishCatalog();
		/** @brief The lane: NearestKey for a shadow view key, among the shadow keys requested. */
		std::string NearestShadowKey(const ShadowPipelineKey& a_key) const;
		/**
		 * @brief The lane, for the on-demand build warnings: the pipeline already requested whose key is nearest a_key (fewest
		 * differing bits), with the fields that differ, or why there is none (the set was recreated, and by what).
		 */
		std::string NearestKey(const PipelineKey& a_key) const;
		static std::string DescribeKey(const PipelineKey& a_key)
		{
			return fmt::format("VS {:08X} PS {:08X} pass {:08X} ({}), raster {:X}, vertex layout {:016X}", a_key.vertexDescriptor, a_key.pixelDescriptor,
				a_key.passDescriptor, LightingTechniqueName((a_key.passDescriptor >> 24) & 0x3f), a_key.rasterFlags, a_key.vertexLayout);
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

			// A draw's push data (DrawPipelines.h, kDrawPushWords): its rows' addresses and its object word.
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

			auto range = [](std::uint32_t a_binding, std::uint32_t a_count, rhi::ShaderStage a_stage, rhi::LayoutRangeSource a_source, std::size_t a_offset, bool a_samplers = false) {
				rhi::LayoutBindingRange range{};
				range.set = 0;
				range.binding = a_binding;
				range.count = a_count;
				range.visibility = a_stage;
				range.source = a_source;
				range.recordOffset = static_cast<std::uint32_t>(a_offset);
				range.samplers = a_samplers;
				return range;
			};
			// A range whose record (or, for a push address, whose address) is at a_word of push range a_root.
			auto from = [](rhi::LayoutBindingRange a_range, std::uint32_t a_root, std::uint32_t a_word) {
				a_range.addressRootIndex = a_root;
				a_range.addressOffset32 = a_word;
				return a_range;
			};
			auto pushed = [&](std::uint32_t a_binding, rhi::ShaderStage a_stage, std::uint32_t a_root, std::uint32_t a_word) {
				return from(range(a_binding, 1, a_stage, rhi::LayoutRangeSource::PushAddress, 0), a_root, a_word);
			};
			auto fromMaterialRow = [&](rhi::LayoutBindingRange a_range) { return from(a_range, 0, kDrawPushMaterialRow); };
			auto fromFrameRecord = [&](rhi::LayoutBindingRange a_range) { return from(a_range, 1, kFramePushRecord); };

			// The main pass: per register, the pass-wide blocks by push address, the rows' (b0, b1, b2, b4) from the draw's rows,
			// and everything else from the frame record.
			auto buildRanges = [&](bool a_pulled) {
			// A pulled draw (the Z-prepass's plain draws) reads its rows itself (DCLF_PULLED, ShaderPrograms.cpp PulledLightingSource):
			// no row's range.
			auto fromRow = [&](rhi::LayoutBindingRange a_range) { return from(a_range, 0, kDrawPushPipelineRow); };
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
						mainRanges.push_back(pushed(b, stage, 1, word));
						word += 2;
					} else if (a_pulled && (r <= 2 || r == 4)) {
						continue;
					} else if (r == 0) {
						mainRanges.push_back(fromRow(range(b, 1, stage, address,
							kPipelineRowHeader + (pixel ? offsetof(PipelineRowHeader, psTechnique) : offsetof(PipelineRowHeader, vsTechnique)))));
					} else if (r == 1) {
						mainRanges.push_back(fromMaterialRow(range(b, 1, stage, address,
							kMaterialRowHeader + (pixel ? offsetof(MaterialRowHeader, psMaterial) : offsetof(MaterialRowHeader, vsMaterial)))));
					} else if (r == 2) {
						mainRanges.push_back(fromRow(range(b, 1, stage, address,
							kPipelineRowHeader + (pixel ? offsetof(PipelineRowHeader, psGeometry) : offsetof(PipelineRowHeader, vsGeometry)))));
					} else if (r == 4) {
						mainRanges.push_back(fromRow(range(b, 1, stage, address,
							kPipelineRowHeader + (pixel ? offsetof(PipelineRowHeader, psPermutation) : offsetof(PipelineRowHeader, vsPermutation)))));
					} else {
						mainRanges.push_back(fromFrameRecord(range(b, 1, stage, address, frame + 8 * std::size_t{ r })));
					}
				}
			}
			// Textures: t0-t15 from the material row but the shadow mask (t14, the pipeline row's); the features' (t71, t74) from the
			// material row; every other register from the frame record. Samplers likewise.
			const auto index = rhi::LayoutRangeSource::IndirectIndex;
			const auto pixelStage = rhi::ShaderStage::Pixel;
			for (std::uint32_t t = 0; t < kPixelTextureSlots; ++t) {
				if (a_pulled) {
					continue;
				} else if (t == kShadowMaskSlot) {
					mainRanges.push_back(fromRow(range(kBindingShiftT + t, 1, pixelStage, index, kPipelineRowHeader + offsetof(PipelineRowHeader, shadowMask))));
					mainRanges.push_back(fromRow(range(kBindingShiftS + t, 1, pixelStage, index, kPipelineRowHeader + offsetof(PipelineRowHeader, shadowMaskSampler), true)));
				} else {
					mainRanges.push_back(fromMaterialRow(range(kBindingShiftT + t, 1, pixelStage, index, kMaterialRowHeader + offsetof(MaterialRowHeader, textures) + 4 * std::size_t{ t })));
					mainRanges.push_back(fromMaterialRow(range(kBindingShiftS + t, 1, pixelStage, index, kMaterialRowHeader + offsetof(MaterialRowHeader, samplers) + 4 * std::size_t{ t }, true)));
				}
			}
			std::uint32_t first = kPixelTextureSlots;
			for (std::uint32_t f = 0; f < kFeatureMaterialTextures; ++f) {
				const std::uint32_t t = kFeatureMaterialRegisters[f];
				if (t > first)
					mainRanges.push_back(fromFrameRecord(range(kBindingShiftT + first, t - first, pixelStage, index, offsetof(DrawBindings, textures) + 4 * std::size_t{ first })));
				if (!a_pulled)
					mainRanges.push_back(fromMaterialRow(range(kBindingShiftT + t, 1, pixelStage, index, kMaterialRowHeader + offsetof(MaterialRowHeader, features) + 4 * std::size_t{ f })));
				first = t + 1;
			}
			mainRanges.push_back(fromFrameRecord(range(kBindingShiftT + first, kTextureRegisters - first, pixelStage, index, offsetof(DrawBindings, textures) + 4 * std::size_t{ first })));
			// The vertex stage's buffers (the shading rows up to the object records), from the frame record.
			mainRanges.push_back(fromFrameRecord(range(kVertexTextureBinding, kVertexTextureCount, rhi::ShaderStage::Vertex, index,
				offsetof(DrawBindings, textures) + 4 * std::size_t{ kShadingBufferRegister })));
			return mainRanges;
			};
			const auto mainRanges = buildRanges(false);
			const auto zRanges = buildRanges(true);
			const rhi::PipelineLayoutDesc desc{ .ranges = rhi::Span<rhi::LayoutBindingRange>{ mainRanges.data(), static_cast<std::uint32_t>(mainRanges.size()) },
				.pushConstants = { pushConstants, 2u }, .staticSamplers = {}, .flags = rhi::PF_AllowInputAssembler };
			logger::info("[DCLF] main layout: {} ranges, {} frame push words", mainRanges.size(), kFramePushWords);
			if (device.CreatePipelineLayout(desc, layout) != rhi::Result::Ok) {
				supported = false;
				logger::error("[DCLF] Could not create the indirect draw pipeline layout");
				return false;
			}
			// The Z-prepass's plain draws (MainOpaquePass): the main layout's ranges but the rows', no input assembler.
			const rhi::PipelineLayoutDesc zDesc{ .ranges = rhi::Span<rhi::LayoutBindingRange>{ zRanges.data(), static_cast<std::uint32_t>(zRanges.size()) },
				.pushConstants = { pushConstants, 2u }, .staticSamplers = {}, .flags = rhi::PF_None };
			if (device.CreatePipelineLayout(zDesc, zLayout) != rhi::Result::Ok) {
				supported = false;
				logger::error("[DCLF] Could not create the Z-prepass's pipeline layout");
				return false;
			}
			const auto index = rhi::LayoutRangeSource::IndirectIndex;
			const auto pixelStage = rhi::ShaderStage::Pixel;
			// The shadow views (DrawPipelines.h, kShadowPushWords): the draw's words name its material row (the pipeline row's words
			// are unused), the view's the rest.
			rhi::PushConstantRangeDesc shadowPush[2] = { drawPush, pushConstants[1] };
			shadowPush[1].num32BitValues = kShadowPushWords;
			auto fromShadowFrameRecord = [&](rhi::LayoutBindingRange a_range) { return from(a_range, 1, kShadowPushFrameRecord); };
			std::vector<rhi::LayoutBindingRange> shadowRanges;
			for (const bool pixel : { false, true }) {
				const auto stage = pixel ? rhi::ShaderStage::Pixel : rhi::ShaderStage::Vertex;
				for (std::uint32_t r = 0; r < kConstantBufferRegisters; ++r) {
					const std::uint32_t b = kBindingShiftB + r;
					if (!pixel && r == 1)
						shadowRanges.push_back(pushed(b, stage, 0, kDrawPushMaterialRow));  // the material row itself
					else if (r == 0)
						shadowRanges.push_back(pushed(b, stage, 1, kShadowPushViewBlock));
					else if (r == kPerFrameVertexRegister)
						shadowRanges.push_back(pushed(b, stage, 1, kShadowPushPerFrame));
					else if (r == kSharedDataRegister)
						shadowRanges.push_back(pushed(b, stage, 1, kShadowPushSharedData));
					else if (r == kFeatureDataRegister)
						shadowRanges.push_back(pushed(b, stage, 1, kShadowPushFeatureData));
					else
						shadowRanges.push_back(pushed(b, stage, 1, kShadowPushZeros));
				}
			}
			// The diffuse (t0) from the draw's row; t1 and up, the samplers and the vertex stage's buffers from the frame record.
			shadowRanges.push_back(fromMaterialRow(range(kBindingShiftT, 1, pixelStage, index, kShadowRowDiffuseOffset)));
			shadowRanges.push_back(fromShadowFrameRecord(range(kBindingShiftT + 1, kTextureRegisters - 1, pixelStage, index, offsetof(DrawBindings, textures) + 4)));
			shadowRanges.push_back(fromShadowFrameRecord(range(kBindingShiftS, kSamplerRegisters, pixelStage, index, offsetof(DrawBindings, samplers), true)));
			shadowRanges.push_back(fromShadowFrameRecord(range(kVertexTextureBinding, kVertexTextureCount, rhi::ShaderStage::Vertex, index,
				offsetof(DrawBindings, textures) + 4 * std::size_t{ kShadingBufferRegister })));
			const rhi::PipelineLayoutDesc shadowDesc{ .ranges = rhi::Span<rhi::LayoutBindingRange>{ shadowRanges.data(), static_cast<std::uint32_t>(shadowRanges.size()) },
				.pushConstants = { shadowPush, 2u }, .staticSamplers = {}, .flags = rhi::PF_AllowInputAssembler };
			if (device.CreatePipelineLayout(shadowDesc, shadowLayout) != rhi::Result::Ok) {
				supported = false;
				logger::error("[DCLF] Could not create the shadow views' pipeline layout");
				return false;
			}
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

		/** @brief Tree LOD's pipelines (TryRequestTreeLod). */
		static org::services::PipelinePayload BuildTreeLod(rhi::Device a_device, rhi::PipelineLayoutHandle a_zLayout, const ShaderPrograms::TreeLodProgram* a_program,
			TargetFormats a_targets, rhi::BlendState a_blend, bool a_frontCCW)
		{
			SpirvReflection vertex, pixel, depthVertex, depthPixel;
			if (!vertex.Parse(a_program->vertex) || !pixel.Parse(a_program->pixel) || !depthVertex.Parse(a_program->depthVertex) || !depthPixel.Parse(a_program->depthPixel))
				throw std::runtime_error("not SPIR-V");
			CheckPulledBindings(vertex, false);
			CheckPulledBindings(pixel, true);
			CheckPulledBindings(depthVertex, false);
			CheckPulledBindings(depthPixel, true);
			auto built = std::make_shared<TreeLodBuilt>();
			const rhi::SubobjLayout layout{ a_zLayout };
			const rhi::SubobjDSV depthFormat{ rhi::helpers::ToRHI(a_targets.depth) };
			const rhi::SubobjPrimitiveTopology topology{ rhi::PrimitiveTopology::TriangleList };
			const rhi::SubobjInputLayout noInput{};
			const rhi::SubobjFlags plain{};
			for (const bool colour : { false, true }) {
				const auto& vs = colour ? a_program->vertex : a_program->depthVertex;
				const auto& ps = colour ? a_program->pixel : a_program->depthPixel;
				const rhi::SubobjShader vertexShader{ rhi::ShaderStage::Vertex, { vs.data(), static_cast<std::uint32_t>(vs.size()) }, "main" };
				const rhi::SubobjShader pixelShader{ rhi::ShaderStage::Pixel, { ps.data(), static_cast<std::uint32_t>(ps.size()) }, "main" };
				rhi::SubobjRaster raster{};
				raster.rs.cull = rhi::CullMode::None;  // the engine draws tree LOD with culling off (cull mode 0) in both passes
				raster.rs.frontCCW = a_frontCCW;
				rhi::SubobjDepth depth{};
				depth.ds.depthEnable = true;
				depth.ds.depthWrite = !colour;
				depth.ds.depthFunc = colour ? rhi::CompareOp::Equal : rhi::CompareOp::Less;
				rhi::SubobjBlend blend{};
				rhi::SubobjRTVs targets{};
				if (colour) {
					blend.bs = a_blend;
					blend.bs.numAttachments = a_targets.colorCount;
					targets.rt.count = a_targets.colorCount;
					for (std::uint32_t i = 0; i < a_targets.colorCount; ++i)
						targets.rt.formats[i] = rhi::helpers::ToRHI(a_targets.colors[i]);
				} else {
					blend.bs.numAttachments = 0;
				}
				const rhi::PipelineStreamItem items[] = {
					rhi::Make(layout), rhi::Make(vertexShader), rhi::Make(pixelShader), rhi::Make(raster), rhi::Make(depth), rhi::Make(blend),
					rhi::Make(targets), rhi::Make(depthFormat), rhi::Make(topology), rhi::Make(noInput), rhi::Make(plain),
				};
				auto& out = colour ? built->colour : built->depth;
				if (const auto result = a_device.CreatePipeline(items, static_cast<std::uint32_t>(std::size(items)), out); result != rhi::Result::Ok)
					throw std::runtime_error(fmt::format("CreatePipeline (tree LOD {}) failed ({})", colour ? "colour" : "depth", static_cast<int>(result)));
			}
			return built;
		}

		/** @brief A forward view's pipeline (TryRequestForward). */
		static org::services::PipelinePayload BuildForward(rhi::Device a_device, rhi::PipelineLayoutHandle a_zLayout, const ShaderPrograms::ForwardProgram* a_program,
			ForwardTargets a_targets, rhi::CullMode a_cull, bool a_frontCCW)
		{
			SpirvReflection vertex, pixel;
			if (!vertex.Parse(a_program->vertex) || !pixel.Parse(a_program->pixel))
				throw std::runtime_error("not SPIR-V");
			CheckPulledBindings(vertex, false);
			CheckPulledBindings(pixel, true);
			auto built = std::make_shared<rhi::PipelinePtr>();
			const rhi::SubobjLayout layout{ a_zLayout };
			const rhi::SubobjShader vertexShader{ rhi::ShaderStage::Vertex, { a_program->vertex.data(), static_cast<std::uint32_t>(a_program->vertex.size()) }, "main" };
			const rhi::SubobjShader pixelShader{ rhi::ShaderStage::Pixel, { a_program->pixel.data(), static_cast<std::uint32_t>(a_program->pixel.size()) }, "main" };
			rhi::SubobjRaster raster{};
			raster.rs.cull = a_cull;
			raster.rs.frontCCW = a_frontCCW;
			// The engine's depth mode 3 for the face's LOD: test and write, LESS_EQUAL.
			rhi::SubobjDepth depth{};
			depth.ds.depthEnable = true;
			depth.ds.depthWrite = true;
			depth.ds.depthFunc = rhi::CompareOp::LessEqual;
			// No blending, every channel written (the engine's write mask there is stale state, and the cube's alpha is not read).
			rhi::SubobjBlend blend{};
			blend.bs.numAttachments = 1;
			rhi::SubobjRTVs targets{};
			targets.rt.count = 1;
			targets.rt.formats[0] = rhi::helpers::ToRHI(a_targets.colour);
			const rhi::SubobjDSV depthFormat{ rhi::helpers::ToRHI(a_targets.depth) };
			const rhi::SubobjPrimitiveTopology topology{ rhi::PrimitiveTopology::TriangleList };
			const rhi::SubobjInputLayout noInput{};
			const rhi::SubobjFlags plain{};
			const rhi::PipelineStreamItem items[] = {
				rhi::Make(layout), rhi::Make(vertexShader), rhi::Make(pixelShader), rhi::Make(raster), rhi::Make(depth), rhi::Make(blend),
				rhi::Make(targets), rhi::Make(depthFormat), rhi::Make(topology), rhi::Make(noInput), rhi::Make(plain),
			};
			if (const auto result = a_device.CreatePipeline(items, static_cast<std::uint32_t>(std::size(items)), *built); result != rhi::Result::Ok)
				throw std::runtime_error(fmt::format("CreatePipeline (forward) failed ({})", static_cast<int>(result)));
			return built;
		}

		static org::services::PipelinePayload Build(rhi::Device a_device, rhi::PipelineLayoutHandle a_layout, rhi::PipelineLayoutHandle a_zLayout, PipelineKey a_key,
			const ShaderPrograms::Program* a_program, TargetFormats a_targets, EngineState a_state, bool a_frontCCW)
		{
			SpirvReflection vertex, pixel, depthPixel, pulledVertex, pulledDepthPixel;
			if (!vertex.Parse(a_program->vertex) || !pixel.Parse(a_program->pixel) || !depthPixel.Parse(a_program->depthPixel) ||
				!pulledVertex.Parse(a_program->pulledVertex) || !pulledDepthPixel.Parse(a_program->pulledDepthPixel))
				throw std::runtime_error("not SPIR-V");
			CheckBindings(vertex, false);
			CheckBindings(pixel, true);
			CheckBindings(depthPixel, true);
			CheckPulledBindings(pulledVertex, false);
			CheckPulledBindings(pulledDepthPixel, true);
			auto built = std::make_shared<Built>();
			// CS_DCLF_FOLIAGE_PARITY: terrain LOD's depth stage records the pixels it owns (Lighting.hlsl, DCLF_OWNED_PARITY), so it
			// keeps its pixel stage.
			const std::uint32_t technique = (a_key.passDescriptor >> 24) & 0x3f;
			const bool pulledPixel = FragmentDefersDepth(a_program->pulledDepthPixel) || (FoliageParityOn() && (technique == 9 || technique == 18));
			AddUsage(vertex, false, built->usage[kColorVariant]);
			AddUsage(pixel, true, built->usage[kColorVariant]);
			AddUsage(vertex, false, built->usage[kDepthVariant]);
			AddUsage(depthPixel, true, built->usage[kDepthVariant]);
			// The tables its constant groups are packed by (T6b2c): its own stages', fixed with it.
			built->tables = std::make_shared<const ConstantTables>(ConstantTables{ ConstantTableOf({ &vertex }, false, a_key), ConstantTableOf({ &pixel, &depthPixel }, true, a_key) });

			const rhi::SubobjLayout layout{ a_layout };
			const rhi::SubobjShader vertexShader{ rhi::ShaderStage::Vertex, { a_program->vertex.data(), static_cast<std::uint32_t>(a_program->vertex.size()) }, "main" };
			const rhi::SubobjShader pixelShader{ rhi::ShaderStage::Pixel, { a_program->pixel.data(), static_cast<std::uint32_t>(a_program->pixel.size()) }, "main" };
			const rhi::SubobjShader depthPixelShader{ rhi::ShaderStage::Pixel, { a_program->depthPixel.data(), static_cast<std::uint32_t>(a_program->depthPixel.size()) }, "main" };
			const rhi::SubobjDSV depthFormat{ rhi::helpers::ToRHI(a_targets.depth) };
			const rhi::SubobjPrimitiveTopology topology{ rhi::PrimitiveTopology::TriangleList };
			const rhi::SubobjInputLayout input{ BuildInputLayout(vertex, a_key.vertexLayout) };
			const rhi::SubobjFlags flags{ rhi::PipelineFlags_IndirectBindable };
			// An opaque-group decal (accumulation hint 2) writes depth where the engine's main pass does: its depth variant is
			// the decal depth pass's, which draws it before the colour pass with the colour variant's bias and test (engine
			// notes, "Decals"). A blended decal writes none (depth mode 1), so its depth variant writes nothing either. A
			// multi-index layer (group 3, the engine's group 2: depth mode 3) writes its depth from its colour draw, after its
			// host is shaded, as the engine does: no depth variant of it is drawn (the decal depth pass is group 1's alone).
			const std::uint32_t decalGroup = a_state.valid ? RasterDecalGroup(a_key.rasterFlags) : 0u;
			for (std::uint32_t variant = 0; variant < kVariantCount; ++variant) {
				const bool depthOnly = variant == kDepthVariant;
				rhi::SubobjRaster raster{};
				raster.rs.cull = (a_key.rasterFlags & kRasterTwoSided) ? rhi::CullMode::None : rhi::CullMode::Back;
				// A decal's depth bias, as the engine's rasterizer state for its bias mode holds it. The
				// integer DepthBias goes through as the constant factor: the main depth is D24S8, for which
				// Vulkan's default representation is the least representable value of the format, which is
				// exactly what D3D11 (and DXVK, which forces it for UNORM formats) means by it.
				if (a_state.valid && (!depthOnly || decalGroup == 1)) {
					raster.rs.depthBias = a_state.depthBias;
					raster.rs.depthBiasClamp = a_state.depthBiasClamp;
					raster.rs.slopeScaledDepthBias = a_state.slopeScaledDepthBias;
				}
				// The engine's winding (EngineFrontCCW): its rasterizer states say which face is the front one,
				// and RasterState::frontCCW means the same thing on every backend.
				raster.rs.frontCCW = a_frontCCW;
				rhi::SubobjDepth depth{};
				depth.ds.depthEnable = true;
				// The depth variant is DCLF's own Z-prepass. The colour variant tests EQUAL against it, as the
				// engine's opaque pass does. Where the prepass's alpha test discarded a texel (foliage cards), the
				// stored depth is whatever lies behind it, so LESS_EQUAL would pass those fragments and run the full
				// lighting shader before they discard; EQUAL rejects them. A decal (a key with engine state) keeps
				// LESS_EQUAL: it draws with a depth bias over the surface beneath, which EQUAL never passes.
				depth.ds.depthWrite = depthOnly ? decalGroup != 2 : decalGroup == 3;
				depth.ds.depthFunc = depthOnly ? (a_state.valid ? rhi::CompareOp::LessEqual : rhi::CompareOp::Less) :
				                                 (a_state.valid ? rhi::CompareOp::LessEqual : rhi::CompareOp::Equal);
				rhi::SubobjBlend blend{};
				rhi::SubobjRTVs targets{};
				if (!depthOnly) {
					// A decal blends (or not) exactly as the engine's blend state for its indices says, per
					// target, including the write masks. Everything else keeps the RHI default: no blending.
					if (a_state.valid || a_state.blendOnly)
						blend.bs = a_state.blend;
					blend.bs.numAttachments = a_targets.colorCount;
					targets.rt.count = a_targets.colorCount;
					for (std::uint32_t i = 0; i < a_targets.colorCount; ++i)
						targets.rt.formats[i] = rhi::helpers::ToRHI(a_targets.colors[i]);
				} else {
					blend.bs.numAttachments = 0;
				}
				const rhi::PipelineStreamItem items[] = {
					rhi::Make(layout), rhi::Make(vertexShader), rhi::Make(depthOnly ? depthPixelShader : pixelShader), rhi::Make(raster), rhi::Make(depth), rhi::Make(blend),
					rhi::Make(targets), rhi::Make(depthFormat), rhi::Make(topology), rhi::Make(input), rhi::Make(flags),
				};
				if (const auto result = a_device.CreatePipeline(items, static_cast<std::uint32_t>(std::size(items)), built->pipelines[variant]); result != rhi::Result::Ok)
					throw std::runtime_error(fmt::format("CreatePipeline ({}) failed ({})", depthOnly ? "depth" : "color", static_cast<int>(result)));
				if (!depthOnly)
					continue;
				// The Z-prepass's plain-draw pipeline: the depth variant's state, the pulled stages, no vertex input, bound per call.
				auto hash = [](const std::vector<std::byte>& a_module) { return ankerl::unordered_dense::detail::wyhash::hash(a_module.data(), a_module.size()); };
				auto& zKey = built->zKey;
				zKey.vertex = hash(a_program->pulledVertex);
				zKey.vertexBytes = static_cast<std::uint32_t>(a_program->pulledVertex.size());
				zKey.pixel = pulledPixel ? hash(a_program->pulledDepthPixel) : 0;
				zKey.pixelBytes = pulledPixel ? static_cast<std::uint32_t>(a_program->pulledDepthPixel.size()) : 0;
				zKey.cull = static_cast<std::uint32_t>(raster.rs.cull);
				zKey.depthWrite = depth.ds.depthWrite ? 1u : 0u;
				zKey.depthFunc = static_cast<std::uint32_t>(depth.ds.depthFunc);
				zKey.depthBias = std::bit_cast<std::uint32_t>(static_cast<float>(raster.rs.depthBias));
				zKey.depthBiasClamp = std::bit_cast<std::uint32_t>(raster.rs.depthBiasClamp);
				zKey.slopeScaledDepthBias = std::bit_cast<std::uint32_t>(raster.rs.slopeScaledDepthBias);
				const rhi::SubobjLayout zLayout{ a_zLayout };
				const rhi::SubobjShader pulledVertexShader{ rhi::ShaderStage::Vertex,
					{ a_program->pulledVertex.data(), static_cast<std::uint32_t>(a_program->pulledVertex.size()) }, "main" };
				const rhi::SubobjShader pulledPixelShader{ rhi::ShaderStage::Pixel,
					{ a_program->pulledDepthPixel.data(), static_cast<std::uint32_t>(a_program->pulledDepthPixel.size()) }, "main" };
				const rhi::SubobjInputLayout noInput{};
				const rhi::SubobjFlags plain{};
				const rhi::PipelineStreamItem pulledItems[] = {
					rhi::Make(zLayout), rhi::Make(pulledVertexShader), rhi::Make(raster), rhi::Make(depth), rhi::Make(blend),
					rhi::Make(targets), rhi::Make(depthFormat), rhi::Make(topology), rhi::Make(noInput), rhi::Make(plain), rhi::Make(pulledPixelShader),
				};
				const auto pulledCount = static_cast<std::uint32_t>(std::size(pulledItems)) - (pulledPixel ? 0u : 1u);
				if (const auto result = a_device.CreatePipeline(pulledItems, pulledCount, built->pulledDepth); result != rhi::Result::Ok)
					throw std::runtime_error(fmt::format("CreatePipeline (pulled depth) failed ({})", static_cast<int>(result)));
			}
			return built;
		}

		/**
		 * @brief One shadow pipeline: the Utility permutation, depth only, with the view's rasterizer state.
		 *
		 * No colour attachment and no blending - a shadow map holds depth alone - and the depth test is
		 * the engine's own for a shadow pass: write, compare LESS. Depth bias, cull mode and winding are the
		 * view's (ShadowRasterStateId); a two-sided caster draws without culling whatever the view's mode,
		 * as the engine's Utility pass switches culling off for one.
		 */
		static org::services::PipelinePayload BuildShadow(rhi::Device a_device, rhi::PipelineLayoutHandle a_layout, ShadowPipelineKey a_key,
			const ShaderPrograms::ShadowProgram* a_program, DXGI_FORMAT a_depthFormat, ShadowRasterState a_state)
		{
			SpirvReflection vertex, pixel;
			if (!vertex.Parse(a_program->vertex) || !pixel.Parse(a_program->pixel))
				throw std::runtime_error("not SPIR-V");
			CheckBindings(vertex, false);
			CheckBindings(pixel, true);
			auto built = std::make_shared<Built>();
			AddUsage(vertex, false, built->usage[kColorVariant]);
			AddUsage(pixel, true, built->usage[kColorVariant]);

			const rhi::SubobjLayout layout{ a_layout };
			const rhi::SubobjShader vertexShader{ rhi::ShaderStage::Vertex, { a_program->vertex.data(), static_cast<std::uint32_t>(a_program->vertex.size()) }, "main" };
			const rhi::SubobjShader pixelShader{ rhi::ShaderStage::Pixel, { a_program->pixel.data(), static_cast<std::uint32_t>(a_program->pixel.size()) }, "main" };
			const rhi::SubobjDSV depthFormat{ rhi::helpers::ToRHI(a_depthFormat) };
			const rhi::SubobjPrimitiveTopology topology{ rhi::PrimitiveTopology::TriangleList };
			// The pulling vertex stage (Utility.hlsl, DCLF_PULLED) has no vertex inputs; the pipeline is bound by its draws, not
			// from an execution set.
			const rhi::SubobjInputLayout input{ BuildInputLayout(vertex, a_key.vertexLayout) };
			const rhi::SubobjFlags flags{};
			rhi::SubobjRaster raster{};
			raster.rs.cull = (a_key.rasterFlags & kRasterTwoSided) ? rhi::CullMode::None : a_state.cull;
			raster.rs.frontCCW = a_state.frontCCW;
			// The integer DepthBias goes through as the constant factor. The shadow maps are D16_UNORM, for
			// which Vulkan's default representation is the least representable value of the format - what
			// D3D11 means by it, and what DXVK uses for the engine's own draws.
			raster.rs.depthBias = a_state.depthBias;
			raster.rs.depthBiasClamp = a_state.depthBiasClamp;
			raster.rs.slopeScaledDepthBias = a_state.slopeScaledDepthBias;
			rhi::SubobjDepth depth{};
			depth.ds.depthEnable = true;
			depth.ds.depthWrite = true;
			depth.ds.depthFunc = rhi::CompareOp::Less;
			rhi::SubobjBlend blend{};
			blend.bs.numAttachments = 0;
			rhi::SubobjRTVs targets{};
			targets.rt.count = 0;
			const rhi::PipelineStreamItem items[] = {
				rhi::Make(layout), rhi::Make(vertexShader), rhi::Make(pixelShader), rhi::Make(raster), rhi::Make(depth), rhi::Make(blend),
				rhi::Make(targets), rhi::Make(depthFormat), rhi::Make(topology), rhi::Make(input), rhi::Make(flags),
			};
			if (const auto result = a_device.CreatePipeline(items, static_cast<std::uint32_t>(std::size(items)), built->pipelines[kColorVariant]); result != rhi::Result::Ok)
				throw std::runtime_error(fmt::format("CreatePipeline (shadow) failed ({})", static_cast<int>(result)));
			built->discards = FragmentDefersDepth(a_program->pixel);
			return built;
		}

		// The command signature of DrawSequence (Records.h), created with the set it selects from.
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

		/**
		 * @brief Writes a_pipelines' [a_version's applied, a_admitted) into a_version's set(s), creating each set (with
		 * pipeline 0, its initial pipeline) and its signature(s) when the version has none yet.
		 */
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
		// The pipeline lane's pass, named once this is whole (a pass that runs meanwhile waits for Get's construction).
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
			// Every pipeline depends on the targets: start over (queued builds finish, and the new generation drops their completions).
			auto describe = [](const TargetFormats& a_formats) {
				std::string text;
				for (std::uint32_t i = 0; i < a_formats.colorCount; ++i)
					text += fmt::format("{} ", static_cast<int>(a_formats.colors[i]));
				return text + fmt::format("depth {}", static_cast<int>(a_formats.depth));
			};
			// The main set and tree LOD's pipelines are the pipeline lane's: it rebuilds them from the inputs posted below
			// (RecreateMainSet). Until the frame's catalog is of the new targets, GetIndirectState has no set and TreeLodPipelinesOf no
			// pipelines (its generation is behind this one).
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

	std::shared_ptr<const PipelineCatalog> DrawPipelines::TakeCatalog()
	{
		// The scene lane's (T6b2c step 5): what the lane published since the last call, null when nothing newer.
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

	void DrawPipelines::CheckConstantTables(const RE::BSShader& a_lighting, const Lookups& a_lookups)
	{
		auto& parity = tableParity;
		++parity.checks;
		auto& cache = SIE::ShaderCache::Instance();
		// A table entry as a byte (Community Shaders' are int8_t, read unsigned by the engine), 0 past its end.
		auto at = [](const auto& a_table, std::uint32_t a_variable) -> std::uint32_t {
			return a_variable < a_table.size() ? static_cast<std::uint8_t>(a_table[a_variable]) : 0u;
		};
		for (const auto& entry : a_lookups.pipelines) {
			if (entry.setIndex == Lookups::kNone)
				continue;
			++parity.entries;
			const auto* vs = cache.GetVertexShader(a_lighting, entry.key.vertexDescriptor);
			const auto* ps = cache.GetPixelShader(a_lighting, entry.key.pixelDescriptor);
			if (!vs || !ps) {
				++parity.uncached;
				continue;
			}
			for (const bool pixel : { false, true }) {
				const auto names = LightingNames(pixel);
				const auto* groups = pixel ? kPSGroups : kVSGroups;
				const auto* firsts = pixel ? kPSFirstVariable : kVSFirstVariable;
				for (std::uint32_t v = 0; v < names.size(); ++v) {
					// Offset 0 is also what a table holds for a variable the stage lacks: it counts only for its group's first.
					std::uint32_t first = ~0u;
					for (std::uint32_t group = 0; group < 3; ++group)
						if ((groups[group] >> v) & 1)
							first = firsts[group];
					const std::uint32_t ours = at(pixel ? entry.psTable : entry.vsTable, v);
					const std::uint32_t theirs = pixel ? at(ps->constantTable, v) : at(vs->constantTable, v);
					const bool inOurs = ours != 0 || v == first, inTheirs = theirs != 0 || v == first;
					auto describe = [&] {
						return fmt::format("VS {:08X} PS {:08X}: {} {} at {}, ShaderCache {}", entry.key.vertexDescriptor, entry.key.pixelDescriptor, pixel ? "PS" : "VS",
							names[v], ours, theirs);
					};
					if (inOurs && inTheirs) {
						++parity.variables;
						if (ours != theirs && parity.differ++ == 0)
							parity.first = describe();
					} else if (inOurs) {
						if (parity.catalogOnly++ == 0)
							parity.catalogOnlyFirst = describe();
					} else if (inTheirs) {
						++parity.cacheOnly;
					}
				}
			}
		}
	}

	void DrawPipelines::RequestShadow(const ShadowPipelineKey& a_viewKey, const ShadowPipelineKey& a_casterKey, std::uint32_t a_occlusion)
	{
		impl->completions->shadowRequests.Push({ a_viewKey, a_casterKey, a_occlusion });
		WakePipelineLane();
	}

	void DrawPipelines::SetShadowInputs(DXGI_FORMAT a_format, RE::BSShader& a_utility)
	{
		auto& input = impl->renderInput;
		// The format of a view the engine has drawn (a typeless resource's view format): unknown until then, and not posted.
		if (a_format == DXGI_FORMAT_UNKNOWN || (input.shadowFormat == a_format && input.utility == &a_utility) || !Enabled())
			return;
		input.shadowFormat = a_format;
		input.utility = &a_utility;
		impl->PostInput(*this);
	}

	namespace
	{
		// The fields of two keys that differ, as "name a -> b", and how many bits differ in all.
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
		/** @brief The casters a shadow request names (Impl::ShadowRequest), for the on-demand warnings. */
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
		// The frame's inputs: new targets start the main set (and tree LOD's) over, a new shadow map format the shadow set, new forward
		// targets the forward pipelines; anything new may unpark the keys waiting on an input.
		bool inputsChanged = false;
		if (auto input = inputSlot.Take()) {
			if (l.input && l.input->targets.colorCount != 0 && !(l.input->targets == input->targets))
				RecreateMainSet(l.input->targets, input->targets);
			if (l.input && l.input->shadowFormat != DXGI_FORMAT_UNKNOWN && input->shadowFormat != l.input->shadowFormat)
				RecreateShadowSet(l.input->shadowFormat, input->shadowFormat);
			if (l.input && l.input->forwardTargets.colour != DXGI_FORMAT_UNKNOWN && !(l.input->forwardTargets == input->forwardTargets))
				RecreateForward(l.input->forwardTargets, input->forwardTargets);
			l.input = std::shared_ptr<const FrameInput>(std::move(input));
			inputsChanged = true;
			l.dirty = true;  // the catalog names the targets' generation, the shadow format and the forward targets
		}
		// The keys asked for: each once (the lane keeps them, and builds them again after a target or format change).
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
		if (inputsChanged) {
			for (const auto& key : std::exchange(l.blocked, {}))
				TryRequest(key);
			for (const auto& key : std::exchange(s.blocked, {}))
				TryRequestShadow(key);
			for (const auto& key : std::exchange(f.blocked, {}))
				TryRequestForward(key);
		}
		// Tree LOD's waits on nothing a list holds: tried every pass (it returns at once when it is requested, built or failed).
		TryRequestTreeLod();
		// The programs finished since the last pass: their keys go on to their pipelines (or fail with them). Again until none is: a
		// program a request above found complete at once (every stage cached) is finished with no completion to wake the lane.
		auto& finished = l.finishedPrograms;
		for (;;) {
			finished.Clear();
			ShaderPrograms::Get().UpdateLane(finished);
			if (finished.Empty())
				break;
			for (const std::uint64_t id : finished.lighting) {
				const auto waiting = l.programWaiters.find(id);
				if (waiting == l.programWaiters.end())
					continue;
				const auto keys = std::move(waiting->second);
				l.programWaiters.erase(waiting);
				for (const auto& key : keys)
					TryRequest(key);
			}
			for (const std::uint32_t technique : finished.shadow) {
				const auto waiting = s.programWaiters.find(technique);
				if (waiting == s.programWaiters.end())
					continue;
				const auto keys = std::move(waiting->second);
				s.programWaiters.erase(waiting);
				for (const auto& key : keys)
					TryRequestShadow(key);
			}
			auto retryForward = [&](std::uint64_t a_program) {
				const auto waiting = f.programWaiters.find(a_program);
				if (waiting == f.programWaiters.end())
					return;
				const auto keys = std::move(waiting->second);
				f.programWaiters.erase(waiting);
				for (const auto& key : keys)
					TryRequestForward(key);
			};
			for (const std::uint64_t id : finished.forward)
				retryForward(id);
			if (finished.forwardTreeLod)
				retryForward(Lane::Forward::kTreeProgram);
			if (finished.treeLod)
				TryRequestTreeLod();
		}
		AdmitLane();
		AdmitShadow();
		AdmitTreeLodAndForward();
		if (l.dirty)
			PublishCatalog();
		// The service's own bookkeeping: completed builds become its active pipelines (a later request for one completes at once).
		service.PublishReady(0);
	}

	void DrawPipelines::Impl::TryRequest(const PipelineKey& a_key)
	{
		auto& l = lane;
		if (l.entries.contains(a_key))
			return;  // requested, or failed for good
		const auto* input = l.input.get();
		if (!input || !input->lighting || input->targets.colorCount == 0 || input->targets.depth == DXGI_FORMAT_UNKNOWN || !input->frontCCW || !input->states ||
			!layout || !zLayout) {
			l.blocked.push_back(a_key);
			return;
		}
		const auto asked = l.asked.find(a_key);
		const std::uint32_t slot = asked != l.asked.end() ? asked->second : ~0u;
		// Everything built here is built at runtime, which a complete precompile and cache would avoid: each build a request starts is
		// logged with the key, the slot that asked for it and its nearest relative, to find what the precompile misses.
		std::uint8_t onDemand = 0;
		bool programFailed = false;
		const auto* program = ShaderPrograms::Get().Find(a_key, *input->lighting, &onDemand, &programFailed);
		if (onDemand)
			logger::warn("[DCLF] on-demand SPIR-V compile: Lighting {} that no precompile requested ({}), for pipeline slot {}: {}", ShaderPrograms::OnDemandStages(onDemand),
				SIE::ShaderCache::Instance().IsCompiling() ? "Community Shaders' compile workers still busy" : "Community Shaders' compile workers idle", slot,
				DescribeKey(a_key));
		if (programFailed) {
			// Its objects stay native (they wait for a set index that never comes).
			l.entries.try_emplace(a_key).first->second.failed = true;
			++l.failedEntries;
			l.dirty = true;
			return;
		}
		if (!program) {
			l.programWaiters[ShaderPrograms::LightingProgramId(a_key)].push_back(a_key);
			return;
		}
		// The set's capacity, counting the builds still queued (every requested entry that has not failed takes a slot).
		if (l.entries.size() - l.failedEntries >= kMaxPipelines) {
			l.blocked.push_back(a_key);
			return;
		}
		// The engine's state behind the key's state bits, as the render thread read it in the deferred pass (CaptureEngineStates): asked
		// for once, and the key parked until the reply. A state that cannot be expressed keeps its objects native.
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
			// An opaque key: the blend state of its group's write mode, from the engine's own state object.
			constexpr std::uint32_t kDoAlphaTest = 1u << 20;
			// LOD (LODLand 9, LODObjects 13, LODObjectHD 15, LODLandNoise 18) is drawn in write mode 1 alpha-tested or not: the
			// engine's main-pass draws of it all take it (dclf-lod.md, the census), so the G-buffer's alphas keep what lies beneath,
			// as the native draw leaves them.
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
		recipe.id = fmt::format("dclf.lighting.{:08X}.{:08X}.{:08X}.{:X}.{:016X}", a_key.vertexDescriptor, a_key.pixelDescriptor, a_key.passDescriptor, a_key.rasterFlags,
			a_key.vertexLayout);
		recipe.shaderKey = PipelineKeyHash{}(a_key);
		recipe.fixedFunctionKey = ankerl::unordered_dense::detail::wyhash::hash(&inputTargets, sizeof(inputTargets));
		// And the blend state, whose write masks are the engine's (an opaque key's too): a build cached with other masks is another pipeline.
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
		logger::warn("[DCLF] on-demand pipeline build: Lighting pipeline slot {}: {}; {}", slot, DescribeKey(a_key), NearestKey(a_key));
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
		// Every pipeline depends on the targets: start over (queued builds finish, and the new generation drops their completions).
		logger::info("[DCLF] pipeline lane: main pass targets {} -> {}; rebuilding {} indirect pipelines", describe(a_from), describe(a_to), l.setPipelines.size());
		l.entries.clear();
		l.failedEntries = 0;
		l.versions.Retire();
		l.setPipelines.clear();
		l.zGroupOf.clear();
		l.zGroupFirst.clear();
		l.zGroups.clear();
		l.usage.clear();
		l.constantTables.clear();
		++l.generation;
		for (auto* counter : { &l.counters.requested, &l.counters.ready, &l.counters.failed, &l.counters.zPipelines, &l.counters.zDepthOnly })
			counter->store(0, std::memory_order_relaxed);
		l.recreatedBy = fmt::format("main pass targets {} -> {}", describe(a_from), describe(a_to));
		// Every key asked for, requested again once the new inputs are in (DrainLane retries the parked keys).
		l.programWaiters.clear();
		l.blocked.clear();
		l.blocked.reserve(l.asked.size());
		for (const auto& [key, slot] : l.asked)
			l.blocked.push_back(key);
		// Tree LOD's pipelines are of the targets too: built again (TryRequestTreeLod, every pass), the last kept for the process.
		auto& t = l.treeLod;
		if (t.built)
			t.retired.push_back(std::move(t.built));
		t.built = {};
		t.requested = false;
		t.failed = t.programFailed;
		for (auto* counter : { &t.counters.requested, &t.counters.ready, &t.counters.failed })
			counter->store(0, std::memory_order_relaxed);
		t.counters.failed.store(t.programFailed ? 1u : 0u, std::memory_order_relaxed);
		l.dirty = true;
	}

	void DrawPipelines::Impl::AdmitLane()
	{
		auto& l = lane;
		// The builds that completed since the last pass, admitted in completion order. One from an earlier generation (before a target
		// change) is dropped: its entry went with the set.
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
		// Publish the admitted pipelines (SetVersion), then hand out the indices the published version holds.
		const auto admitted = static_cast<std::uint32_t>(l.setPipelines.size());
		if (!admitted)
			return;
		auto pipelineAt = [&](std::uint32_t a_variant) {
			return [&, a_variant](std::uint32_t a_index) {
				return static_cast<const Built*>(l.setPipelines[a_index].get())->pipelines[a_variant]->GetHandle();
			};
		};
		const auto result = l.versions.Publish(admitted, [&](SetVersion& a_version) {
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
		});
		using Result = decltype(result);
		if (result == Result::Published)
			l.counters.setPublishes.fetch_add(1, std::memory_order_relaxed);
		if (result == Result::Waiting)
			l.counters.setWaits.fetch_add(1, std::memory_order_relaxed);
		const bool published = result == Result::Current || result == Result::Published;
		if (published && l.versions.handedOut != admitted) {
			l.versions.handedOut = admitted;
			for (auto& [key, entry] : l.entries) {
				if (entry.index == kNotReady && entry.slot != kNotReady) {
					entry.index = entry.slot;
					l.counters.ready.fetch_add(1, std::memory_order_relaxed);
				}
			}
			l.dirty = true;
		} else if (result == Result::Failed && !l.versions.failureLogged) {
			l.versions.failureLogged = true;
			logger::warn("[DCLF] Could not publish the indirect pipeline sets ({} pipelines admitted)", admitted);
		}
	}

	void DrawPipelines::Impl::TryRequestShadow(const ShadowPipelineKey& a_key)
	{
		auto& s = lane.shadow;
		if (s.entries.contains(a_key))
			return;  // requested, or failed for good
		// A view rasterizer state a pipeline cannot express has no id (ShadowRasterStateId's 0): its views stay native.
		if (a_key.viewState == 0) {
			s.entries.try_emplace(a_key).first->second.failed = true;
			++s.failedEntries;
			lane.dirty = true;
			return;
		}
		// The view's rasterizer state reaches the lane with the inputs after the render thread registered it (ShadowRasterStateId).
		const auto* input = lane.input.get();
		if (!input || !input->utility || input->shadowFormat == DXGI_FORMAT_UNKNOWN || !input->shadowStates || a_key.viewState > input->shadowStates->size() ||
			!shadowLayout || !shadowDrawSignature) {
			s.blocked.push_back(a_key);
			return;
		}
		const auto asked = s.asked.find(a_key);
		const std::string casters = asked != s.asked.end() ? DescribeShadowCasters(asked->second.casterKey, asked->second.occlusion) : std::string("for no recorded caster");
		// As TryRequest: each build a request starts is logged with the casters that asked for it and its nearest relative.
		std::uint8_t onDemand = 0;
		bool programFailed = false;
		const auto* program = ShaderPrograms::Get().FindShadow(a_key.technique, *input->utility, true, nullptr, &onDemand, &programFailed);
		if (onDemand)
			logger::warn("[DCLF] on-demand SPIR-V compile: Utility {} of technique {:08X} that no precompile requested ({}), {}", ShaderPrograms::OnDemandStages(onDemand),
				a_key.technique, SIE::ShaderCache::Instance().IsCompiling() ? "Community Shaders' compile workers still busy" : "Community Shaders' compile workers idle",
				casters);
		if (programFailed) {
			// Its casters stay native in the views of this key (they wait for a set index that never comes).
			s.entries.try_emplace(a_key).first->second.failed = true;
			++s.failedEntries;
			lane.dirty = true;
			return;
		}
		if (!program) {
			s.programWaiters[a_key.technique].push_back(a_key);
			return;
		}
		// The set's capacity, counting the builds still queued.
		if (s.entries.size() - s.failedEntries >= kMaxPipelines) {
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
		// The shadow maps were recreated in another format: every shadow pipeline depended on it (queued builds finish, and the new
		// generation drops their completions).
		logger::info("[DCLF] pipeline lane: shadow map format {} -> {}; rebuilding {} shadow pipelines", static_cast<int>(a_from), static_cast<int>(a_to),
			s.setPipelines.size());
		s.entries.clear();
		s.failedEntries = 0;
		s.versions.Retire();
		s.setPipelines.clear();
		s.layouts.clear();
		s.discards.clear();
		++s.generation;
		for (auto* counter : { &s.counters.requested, &s.counters.ready, &s.counters.failed })
			counter->store(0, std::memory_order_relaxed);
		s.recreatedBy = fmt::format("shadow map format {} -> {}", static_cast<int>(a_from), static_cast<int>(a_to));
		// Every key asked for, requested again once the new inputs are in (DrainLane retries the parked keys).
		s.programWaiters.clear();
		s.blocked.clear();
		s.blocked.reserve(s.asked.size());
		for (const auto& [key, request] : s.asked)
			s.blocked.push_back(key);
		lane.dirty = true;
	}

	void DrawPipelines::Impl::AdmitShadow()
	{
		auto& s = lane.shadow;
		// As AdmitLane: the builds that completed, admitted in completion order; one from before a format change is dropped.
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
		// Publish the admitted pipelines (ShadowSetVersion), then hand out the indices the published version holds.
		const auto admitted = static_cast<std::uint32_t>(s.setPipelines.size());
		if (!admitted)
			return;
		const auto result = s.versions.Publish(admitted, [&](ShadowSetVersion& a_version) {
			a_version.pipelines.assign(s.setPipelines.begin(), s.setPipelines.begin() + admitted);
			a_version.layouts.assign(s.layouts.begin(), s.layouts.begin() + admitted);
			a_version.discards.assign(s.discards.begin(), s.discards.begin() + admitted);
			return true;
		});
		using Result = decltype(result);
		if (result == Result::Published)
			s.counters.setPublishes.fetch_add(1, std::memory_order_relaxed);
		if (result == Result::Waiting)
			s.counters.setWaits.fetch_add(1, std::memory_order_relaxed);
		const bool published = result == Result::Current || result == Result::Published;
		if (published && s.versions.handedOut != admitted) {
			s.versions.handedOut = admitted;
			for (auto& [key, entry] : s.entries) {
				if (entry.index == kNotReady && entry.slot != kNotReady) {
					entry.index = entry.slot;
					s.counters.ready.fetch_add(1, std::memory_order_relaxed);
				}
			}
			lane.dirty = true;
		} else if (result == Result::Failed && !s.versions.failureLogged) {
			s.versions.failureLogged = true;
			logger::warn("[DCLF] Could not publish the shadow pipeline set ({} pipelines admitted)", admitted);
		}
	}

	void DrawPipelines::Impl::TryRequestTreeLod()
	{
		auto& t = lane.treeLod;
		if (!t.asked || t.requested || t.built || t.failed)
			return;
		// What it lacks arrives with the frame's inputs (DrainLane tries again each pass) or with its program (Finished::treeLod).
		const auto* input = lane.input.get();
		if (!input || !input->distantTree || input->targets.colorCount == 0 || input->targets.depth == DXGI_FORMAT_UNKNOWN || !input->frontCCW || !input->states ||
			!zLayout || !treeLodDrawSignature)
			return;
		// The opaque write mode's blend (the engine's tree LOD colour draws are in write mode 1), as an opaque Lighting key's: read
		// unasked in the deferred pass (CaptureEngineStates). One that cannot be expressed keeps tree LOD native.
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
		recipe.shaderKey = ankerl::unordered_dense::detail::wyhash::hash(program->vertex.data(), program->vertex.size()) ^
		                   ankerl::unordered_dense::detail::wyhash::hash(program->pixel.data(), program->pixel.size()) * 0x9E3779B97F4A7C15ull ^
		                   ankerl::unordered_dense::detail::wyhash::hash(program->depthPixel.data(), program->depthPixel.size());
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

	void DrawPipelines::Impl::TryRequestForward(const ForwardPipelineKey& a_key)
	{
		auto& f = lane.forward;
		if (f.entries.contains(a_key))
			return;  // requested, or failed for good
		const bool tree = (a_key.flags & ForwardPipelineKey::kForwardTreeLod) != 0;
		const auto* input = lane.input.get();
		if (!input || input->forwardTargets.colour == DXGI_FORMAT_UNKNOWN || input->forwardTargets.depth == DXGI_FORMAT_UNKNOWN || !input->frontCCW ||
			!(tree ? input->distantTree : input->lighting) || !zLayout) {
			f.blocked.push_back(a_key);
			return;
		}
		bool programFailed = false;
		const auto* program = tree ? ShaderPrograms::Get().FindForwardTreeLod(*input->distantTree, &programFailed) :
		                             ShaderPrograms::Get().FindForward(a_key.vertexDescriptor, a_key.pixelDescriptor, *input->lighting, &programFailed);
		if (programFailed) {
			// Its objects stay out of the reflection phase (the engine draws them in the faces).
			f.entries.try_emplace(a_key).first->second.failed = true;
			f.counters.failed.fetch_add(1, std::memory_order_relaxed);
			lane.dirty = true;
			return;
		}
		if (!program) {
			f.programWaiters[tree ? Lane::Forward::kTreeProgram : ShaderPrograms::ForwardProgramId(a_key.vertexDescriptor, a_key.pixelDescriptor)].push_back(a_key);
			return;
		}
		const ForwardTargets forwardTargets = input->forwardTargets;
		const auto cull = (a_key.flags & ForwardPipelineKey::kForwardTwoSided) ? rhi::CullMode::None : rhi::CullMode::Front;
		auto hash = [](const std::vector<std::byte>& a_module) { return ankerl::unordered_dense::detail::wyhash::hash(a_module.data(), a_module.size()); };
		org::services::PipelineRecipe recipe;
		recipe.id = tree ? std::string("dclf.forward.tree-lod") : fmt::format("dclf.forward.{:08X}.{:08X}.{:X}", a_key.vertexDescriptor, a_key.pixelDescriptor, a_key.flags);
		recipe.shaderKey = hash(program->vertex) ^ hash(program->pixel) * 0x9E3779B97F4A7C15ull;
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
		lane.dirty = true;  // its program is the catalog's now
	}

	void DrawPipelines::Impl::RecreateForward(const ForwardTargets& a_from, const ForwardTargets& a_to)
	{
		auto& f = lane.forward;
		// Every forward pipeline depends on the targets: start over (queued builds finish, and the new generation drops their
		// completions). The built ones are kept for the process: a recording may still draw with them.
		logger::info("[DCLF] pipeline lane: forward targets colour {} depth {} -> colour {} depth {}; rebuilding {} forward pipelines", static_cast<int>(a_from.colour),
			static_cast<int>(a_from.depth), static_cast<int>(a_to.colour), static_cast<int>(a_to.depth), f.entries.size());
		for (auto& entry : f.entries)
			if (entry.second.built)
				f.retired.push_back(std::move(entry.second.built));
		f.entries.clear();
		++f.generation;
		for (auto* counter : { &f.counters.requested, &f.counters.ready, &f.counters.failed })
			counter->store(0, std::memory_order_relaxed);
		// Every key asked for, requested again once the new inputs are in (DrainLane retries the parked keys).
		f.programWaiters.clear();
		f.blocked.assign(f.asked.begin(), f.asked.end());
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
		// The version the indices above were handed out of, or a later one of the generation (published only grows within one).
		catalog->setVersion = l.versions.published.load();
		// The shadow views' set, likewise.
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
		// Tree LOD's pipelines (of the targets of targetsGeneration) and the forward views' (of forwardTargets): each kept by the lane.
		catalog->treeLod.pipelines = l.treeLod.built;
		catalog->treeLod.failed = l.treeLod.failed;
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
		catalogSlot.Post(std::move(catalog));
		l.dirty = false;
	}

	struct ForwardPipelineAccess
	{
		static bool TreeLod(const PipelineCatalog* a_catalog, std::uint32_t a_targetsGeneration, TreeLodPipelines& a_out)
		{
			auto& drawPipelines = DrawPipelines::Get();
			const auto& impl = *drawPipelines.impl;
			// Built for the targets drawn into: none once they changed after the catalog's were built for them (as GetIndirectState:
			// its generation is behind the caller's).
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
		if (!a_catalog || !(a_catalog->forwardTargets == a_targets))
			return {};
		const auto* entry = a_catalog->FindForward(a_key);
		if (!entry || !entry->pipeline)
			return {};
		return (*static_cast<const rhi::PipelinePtr*>(entry->pipeline.get()))->GetHandle();
	}

	std::uint32_t DrawPipelines::ShadowRasterStateId(const D3D11_RASTERIZER_DESC& a_desc, std::uint32_t a_renderMode)
	{
		// The scissor is not part of it: the epoch's pass is bounded by the view's viewport (its render area).
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
		// The pipeline lane builds a key of this id once the id reaches it with the frame's inputs (its keys wait for it meanwhile).
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
		// The state bits the pipeline lane asked for since the last call: read here (the deferred pass), or kept for a later one.
		impl->completions->stateRequests.Drain([&](std::uint32_t&& a_bits) { impl->pendingStateBits.push_back(a_bits); });
		bool changed = false;
		// The engine's winding and the Lighting shader, once each is known.
		auto& input = impl->renderInput;
		if (!input.frontCCW)
			if (const auto frontCCW = impl->EngineFrontCCW()) {
				input.frontCCW = frontCCW;
				changed = true;
			}
		if (auto* lighting = ConstantEvaluator::Get().GetLightingShader(); lighting != input.lighting) {
			input.lighting = lighting;
			changed = true;
		}
		// The deferred pass's blend states (DeferredBlendState) exist from its first frame: until then nothing is read, so no
		// state is recorded as unreadable for good.
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
			// The opaque groups' write modes (every opaque Lighting key's blend, and tree LOD's), unasked; then what the lane asked for.
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
		// The shadow set version of a_catalog, the one its shadow lookups' indices are of (the scene lane resolved them from it). None once
		// the shadow map format is not the one that catalog's set was built for (SetShadowInputs): its pipelines draw into the old one.
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
		// The set version of a_catalog, the one its lookups' indices are of. None once the targets are not the ones that catalog's set was
		// built for (SetTargetFormats): its pipelines draw into the old ones.
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
		for (const auto first : version->zGroupFirst)
			state.zPipelines.push_back(static_cast<const DrawPipelines::Impl::Built*>(version->pipelines[first].get())->pulledDepth->GetHandle());
		state.zLayout = impl.zLayout->GetHandle();
		state.zDrawSignature = impl.zDrawSignature->GetHandle();
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

#else  // no render graph

#	include "DrawPipelines.h"

namespace DCLF
{
	struct DrawPipelines::Impl
	{};

	DrawPipelines::DrawPipelines() = default;
	DrawPipelines::~DrawPipelines() = default;

	DrawPipelines& DrawPipelines::Get()
	{
		static DrawPipelines pipelines;
		return pipelines;
	}

	bool DrawPipelines::Enabled() const { return false; }
	void DrawPipelines::SetTargetFormats(const TargetFormats& a_formats) { targets = a_formats; }
	void DrawPipelines::RequestLighting(const PipelineKey&, std::uint32_t) {}
	std::shared_ptr<const PipelineCatalog> DrawPipelines::TakeCatalog() { return nullptr; }
	DrawPipelines::Stats DrawPipelines::GetStats() const { return stats; }
	void DrawPipelines::RequestShadow(const ShadowPipelineKey&, const ShadowPipelineKey&, std::uint32_t) {}
	void DrawPipelines::SetShadowInputs(DXGI_FORMAT, RE::BSShader&) {}
	std::uint32_t DrawPipelines::ShadowRasterStateId(const D3D11_RASTERIZER_DESC&, std::uint32_t) { return 0; }
	void DrawPipelines::RequestTreeLod() {}
	void DrawPipelines::SetTreeLodInputs(RE::BSShader&) {}
	void DrawPipelines::RequestForward(const ForwardPipelineKey&) {}
	void DrawPipelines::SetForwardTargets(const ForwardTargets&) {}
	void DrawPipelines::CaptureEngineStates() {}
	void DrawPipelines::CheckConstantTables(const RE::BSShader&, const Lookups&) {}
}

#endif
