#if defined(CS_HAS_RENDER_GRAPH) && defined(CS_HAS_ORG_MODULE_SERVICES)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "DrawPipelines.h"
#	include "DrawPipelinesRhi.h"
#	include "Features/DrawcallLimitFix/Engine/EngineStates.h"
#	include "Features/DrawcallLimitFix/Common/Switches.h"

#	include "RenderGraph/RenderGraphRuntime.h"
#	include "SpirvReflection.h"
#	include "Features/DrawcallLimitFix/Scene/VertexInput.h"
#	include "Features/DrawcallLimitFix/Scene/SceneStore.h"
#	include "ShaderCache.h"

#	include <OpenRenderGraph/PersistentGraphHost.h>
#	include <ORGModuleServices/PipelineService.h>
#	include <rhi_helpers.h>

#	include <future>
#	include <stdexcept>

namespace DCLF
{
	namespace
	{
		constexpr std::uint32_t kMaxInFlight = 4;  // pipeline builds running at once (each is a thread)

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
		// The texture registers the vertex stage may declare: the trees' wind, the character light's noise (pixel only, but in the
		// run), the bone palette buffer and the per-object record buffer.
		constexpr std::uint32_t kObjectBufferBinding = kBindingShiftT + kObjectBufferRegister;
		constexpr std::uint32_t kVertexTextureBinding = kBindingShiftT + kTreeWindRegister;
		constexpr std::uint32_t kVertexTextureCount = kObjectBufferRegister - kTreeWindRegister + 1;
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
			std::shared_future<org::services::PipelineArtifact> future;
			std::uint32_t index = kNotReady;  // handed out once a published set version holds the pipeline
			std::uint32_t slot = kNotReady;   // its index in the sets, from its admission
			bool failed = false;
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
			rhi::CommandSignaturePtr depthPassSignature;  // IndirectState::depthPassSignature, when it differs from the depth variant's
			std::uint32_t applied = 0;
		};
		// The shadow views' pipelines, versioned the same way: their plain draws bind each by its index (ShadowIndirectState).
		struct ShadowSetVersion
		{
			// The pipelines of [0, applied), held for as long as a recording holds the version (ShadowIndirectState::pipelines),
			// with their keys' vertex layouts and their classes.
			std::vector<org::services::PipelinePayload> pipelines;
			std::vector<std::uint64_t> layouts;
			std::vector<std::uint8_t> discards;
			std::uint32_t applied = 0;
		};
		// Enough that one is normally free: the published one, and the one the frames still being recorded may hold.
		static constexpr std::size_t kSetVersions = 3;
		template <class Version>
		struct Versions
		{
			std::array<std::shared_ptr<Version>, kSetVersions> owned;
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
				owned = {};
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
				for (auto& version : owned) {
					if (version && (version == current || version.use_count() != 1))
						continue;
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
				return PublishResult::Waiting;
			}
		};

		rhi::Device device{};
		rhi::PipelineLayoutPtr layout;
		// Under frame push, the shadow views' layout: every register from the draw's binding record. Their pass-wide blocks
		// (the view slot's VS_PerFrame at b12, the build's SharedData and FeatureData at b5 and b6) are not the main pass's
		// frame slots. Without frame push the shadow views use the main layout.
		rhi::PipelineLayoutPtr shadowLayout;
		rhi::PipelineLayoutHandle ShadowLayout() const { return shadowLayout->GetHandle(); }
		// The shadow views' plain indirect draw (ShadowIndirectState::drawSignature): a DrawSequence's last words read as an
		// indexed draw from the index pool (BuildDrawsCS, StoreShadowSequence), whose vertex stage pulls its vertices.
		rhi::CommandSignaturePtr shadowDrawSignature;
		bool supported = false;
		bool attempted = false;
		org::services::PipelineService service;
		ankerl::unordered_dense::map<PipelineKey, Entry, PipelineKeyHash> entries;
		std::string recreatedBy, shadowRecreatedBy;  // the last recreation of each set, for NearestKey's warnings
		// The shadow views' own set: one pipeline per (Utility technique, vertex layout, raster state),
		// depth only, into the engine's shadow map format.
		ankerl::unordered_dense::map<ShadowPipelineKey, Entry, ShadowPipelineKeyHash> shadowEntries;
		Versions<ShadowSetVersion> shadowVersions;
		std::vector<org::services::PipelinePayload> shadowSetPipelines;
		DXGI_FORMAT shadowDepthFormat = DXGI_FORMAT_UNKNOWN;
		// The shadow views' rasterizer states, by id - 1 (ShadowRasterStateId), and the render modes each was
		// seen with (bit mode - 0xC).
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
		std::vector<std::uint32_t> shadowRasterStateModes;
		std::uint32_t loggedShadowRasterFailures = 0;
		std::uint32_t shadowInFlight = 0;
		// What a build produces: the pipelines of both variants and the registers the shaders read.
		struct Built
		{
			std::array<rhi::PipelinePtr, kVariantCount> pipelines;
			// Per variant: the Z-prepass build of the pixel stage reads far less than the colour one, and a
			// draw only has to supply what its own variant declares.
			std::array<RegisterUsage, kVariantCount> usage;
			bool discards = false;  // a shadow pipeline whose pixel stage defers the depth test (FragmentDefersDepth)
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
		};
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
			auto* blend = EngineBlendStates()[blendMode][alphaToCoverage][writeMode][extra];
			if (!raster || !blend) {
				a_error = fmt::format("no engine state object at bias {} / blend [{}][{}][{}][{}]", bias, blendMode, alphaToCoverage, writeMode, extra);
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

		Versions<SetVersion> versions;
		std::vector<org::services::PipelinePayload> setPipelines;  // index -> Built, admitted (not necessarily published)
		std::uint32_t inFlight = 0;
		std::uint32_t loggedFailures = 0;

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
			auto fromPipelineRow = [&](rhi::LayoutBindingRange a_range) { return from(a_range, 0, kDrawPushPipelineRow); };
			auto fromMaterialRow = [&](rhi::LayoutBindingRange a_range) { return from(a_range, 0, kDrawPushMaterialRow); };
			auto fromFrameRecord = [&](rhi::LayoutBindingRange a_range) { return from(a_range, 1, kFramePushRecord); };

			// The main pass: per register, the pass-wide blocks by push address, the rows' (b0, b1, b2, b4) from the draw's rows,
			// and everything else from the frame record.
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
					} else if (r == 0) {
						mainRanges.push_back(fromPipelineRow(range(b, 1, stage, address,
							kPipelineRowHeader + (pixel ? offsetof(PipelineRowHeader, psTechnique) : offsetof(PipelineRowHeader, vsTechnique)))));
					} else if (r == 1) {
						mainRanges.push_back(fromMaterialRow(range(b, 1, stage, address,
							kMaterialRowHeader + (pixel ? offsetof(MaterialRowHeader, psMaterial) : offsetof(MaterialRowHeader, vsMaterial)))));
					} else if (r == 2) {
						mainRanges.push_back(fromPipelineRow(range(b, 1, stage, address,
							kPipelineRowHeader + (pixel ? offsetof(PipelineRowHeader, psGeometry) : offsetof(PipelineRowHeader, vsGeometry)))));
					} else if (r == 4) {
						mainRanges.push_back(fromPipelineRow(range(b, 1, stage, address,
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
				if (t == kShadowMaskSlot) {
					mainRanges.push_back(fromPipelineRow(range(kBindingShiftT + t, 1, pixelStage, index, kPipelineRowHeader + offsetof(PipelineRowHeader, shadowMask))));
					mainRanges.push_back(fromPipelineRow(range(kBindingShiftS + t, 1, pixelStage, index, kPipelineRowHeader + offsetof(PipelineRowHeader, shadowMaskSampler), true)));
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
				mainRanges.push_back(fromMaterialRow(range(kBindingShiftT + t, 1, pixelStage, index, kMaterialRowHeader + offsetof(MaterialRowHeader, features) + 4 * std::size_t{ f })));
				first = t + 1;
			}
			mainRanges.push_back(fromFrameRecord(range(kBindingShiftT + first, kTextureRegisters - first, pixelStage, index, offsetof(DrawBindings, textures) + 4 * std::size_t{ first })));
			// The vertex stage's buffers (the object and bone tables), from the frame record.
			mainRanges.push_back(fromFrameRecord(range(kVertexTextureBinding, kVertexTextureCount, rhi::ShaderStage::Vertex, index,
				offsetof(DrawBindings, textures) + 4 * std::size_t{ kTreeWindRegister })));
			const rhi::PipelineLayoutDesc desc{ .ranges = rhi::Span<rhi::LayoutBindingRange>{ mainRanges.data(), static_cast<std::uint32_t>(mainRanges.size()) },
				.pushConstants = { pushConstants, 2u }, .staticSamplers = {}, .flags = rhi::PF_AllowInputAssembler };
			logger::info("[DCLF] main layout: {} ranges, {} frame push words", mainRanges.size(), kFramePushWords);
			if (device.CreatePipelineLayout(desc, layout) != rhi::Result::Ok) {
				supported = false;
				logger::error("[DCLF] Could not create the indirect draw pipeline layout");
				return false;
			}
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
				offsetof(DrawBindings, textures) + 4 * std::size_t{ kTreeWindRegister })));
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
			return true;
		}

		static org::services::PipelinePayload Build(rhi::Device a_device, rhi::PipelineLayoutHandle a_layout, PipelineKey a_key, const ShaderPrograms::Program* a_program,
			TargetFormats a_targets, EngineState a_state, bool a_frontCCW)
		{
			SpirvReflection vertex, pixel, depthPixel;
			if (!vertex.Parse(a_program->vertex) || !pixel.Parse(a_program->pixel) || !depthPixel.Parse(a_program->depthPixel))
				throw std::runtime_error("not SPIR-V");
			CheckBindings(vertex, false);
			CheckBindings(pixel, true);
			CheckBindings(depthPixel, true);
			auto built = std::make_shared<Built>();
			AddUsage(vertex, false, built->usage[kColorVariant]);
			AddUsage(pixel, true, built->usage[kColorVariant]);
			AddUsage(vertex, false, built->usage[kDepthVariant]);
			AddUsage(depthPixel, true, built->usage[kDepthVariant]);

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
					if (a_state.valid)
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
			if (device.CreateCommandSignature(desc, layout->GetHandle(), a_version.signatures[a_variant]) != rhi::Result::Ok)
				return false;
			if (a_variant != kDepthVariant || !desc.explicitPreprocess)
				return true;
			desc.explicitPreprocess = false;
			return device.CreateCommandSignature(desc, layout->GetHandle(), a_version.depthPassSignature) == rhi::Result::Ok;
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
	{}

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
			// Every pipeline depends on the targets: start over (in-flight builds finish and are dropped).
			auto describe = [](const TargetFormats& a_formats) {
				std::string text;
				for (std::uint32_t i = 0; i < a_formats.colorCount; ++i)
					text += fmt::format("{} ", static_cast<int>(a_formats.colors[i]));
				return text + fmt::format("depth {}", static_cast<int>(a_formats.depth));
			};
			logger::info("[DCLF] Main pass targets changed ({} -> {}); rebuilding {} indirect pipelines", describe(targets), describe(a_formats), impl->setPipelines.size());
			++stats.targetChanges;
			impl->entries.clear();
			impl->versions.Retire();
			impl->setPipelines.clear();
			usage.clear();
			++generation;
			impl->inFlight = 0;
			stats.requested = stats.ready = stats.failed = 0;
			impl->recreatedBy = fmt::format("main pass targets {} -> {}", describe(targets), describe(a_formats));
		}
		targets = a_formats;
	}

	const ShaderPrograms::ShadowProgram* RequestShadowProgram(std::uint32_t a_technique, const ShadowPipelineKey& a_casterKey, std::uint32_t a_occlusion,
		RE::BSShader& a_utility, bool a_allowRequest, bool* a_requested)
	{
		std::uint8_t onDemand = 0;
		const auto* program = ShaderPrograms::Get().FindShadow(a_technique, a_utility, a_allowRequest, a_requested, &onDemand);
		if (onDemand)
			logger::warn("[DCLF] on-demand SPIR-V compile: Utility {} of technique {:08X} that no precompile requested ({}): raster {:X}, vertex layout {:016X}; used by {}",
				ShaderPrograms::OnDemandStages(onDemand), a_technique,
				SIE::ShaderCache::Instance().IsCompiling() ? "Community Shaders' compile workers still busy" : "Community Shaders' compile workers idle",
				a_casterKey.rasterFlags, a_casterKey.vertexLayout, SceneStore::Get().DescribeShadowKeyUsers(a_casterKey, a_occlusion));
		return program;
	}

	std::uint32_t RequestShadowPipeline(const ShadowPipelineKey& a_viewKey, const ShaderPrograms::ShadowProgram& a_program, DXGI_FORMAT a_format,
		const ShadowPipelineKey& a_casterKey, std::uint32_t a_occlusion)
	{
		auto& pipelines = DrawPipelines::Get();
		bool requested = false;
		const std::uint32_t index = pipelines.FindShadow(a_viewKey, a_program, a_format, &requested);
		if (requested)
			logger::warn("[DCLF] on-demand pipeline build: shadow technique {:08X}, raster {:X}, vertex layout {:016X}, view state {}, format {}; used by {}; {}",
				a_viewKey.technique, a_viewKey.rasterFlags, a_viewKey.vertexLayout, a_viewKey.viewState, static_cast<int>(a_format),
				SceneStore::Get().DescribeShadowKeyUsers(a_casterKey, a_occlusion), pipelines.NearestShadowKey(a_viewKey));
		return index;
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

	std::string DrawPipelines::NearestKey(const PipelineKey& a_key) const
	{
		const PipelineKey* nearest = nullptr;
		KeyDifference best;
		for (const auto& [key, entry] : impl->entries) {
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
			return impl->recreatedBy.empty() ? std::string("the first pipeline") : fmt::format("no other pipeline since the set was recreated ({})", impl->recreatedBy);
		return fmt::format("nearest requested pipeline differs in {}", best.fields);
	}

	std::string DrawPipelines::NearestShadowKey(const ShadowPipelineKey& a_key) const
	{
		const ShadowPipelineKey* nearest = nullptr;
		KeyDifference best;
		for (const auto& [key, entry] : impl->shadowEntries) {
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
			return impl->shadowRecreatedBy.empty() ? std::string("the first shadow pipeline") :
			                                         fmt::format("no other shadow pipeline since the set was recreated ({})", impl->shadowRecreatedBy);
		return fmt::format("nearest requested shadow pipeline differs in {}", best.fields);
	}

	std::uint32_t DrawPipelines::Find(const PipelineKey& a_key, const ShaderPrograms::Program& a_program, bool* a_requested)
	{
		if (a_requested)
			*a_requested = false;
		if (!HasTargetFormats() || !Enabled())
			return kNotReady;
		if (auto it = impl->entries.find(a_key); it != impl->entries.end())
			return it->second.index;
		if (impl->inFlight >= kMaxInFlight || impl->setPipelines.size() >= kMaxPipelines)
			return kNotReady;  // asked again next frame
		// A key with engine state bits needs that state captured first (CaptureEngineStates); until then
		// it is not ready and nothing is requested, so the object simply stays native.
		Impl::EngineState state{};
		if (const auto bits = RasterStateBits(a_key.rasterFlags)) {
			const auto it = impl->engineStates.find(bits);
			if (it == impl->engineStates.end() || !it->second.valid)
				return kNotReady;
			state = it->second;
		}

		org::services::PipelineRecipe recipe;
		recipe.id = fmt::format("dclf.lighting.{:08X}.{:08X}.{:08X}.{:X}.{:016X}", a_key.vertexDescriptor, a_key.pixelDescriptor, a_key.passDescriptor, a_key.rasterFlags,
			a_key.vertexLayout);
		recipe.shaderKey = PipelineKeyHash{}(a_key);
		recipe.fixedFunctionKey = ankerl::unordered_dense::detail::wyhash::hash(&targets, sizeof(targets));
		const auto frontCCW = impl->EngineFrontCCW();
		if (!frontCCW)
			return kNotReady;
		recipe.build = [device = impl->device, layout = impl->layout->GetHandle(), key = a_key, program = &a_program, formats = targets, state, frontCCW = *frontCCW] {
			return Impl::Build(device, layout, key, program, formats, state, frontCCW);
		};
		auto& entry = impl->entries[a_key];
		entry.future = impl->service.Request(std::move(recipe));
		++impl->inFlight;
		++stats.requested;
		if (a_requested)
			*a_requested = true;
		return kNotReady;
	}

	std::uint32_t DrawPipelines::FindShadow(const ShadowPipelineKey& a_key, const ShaderPrograms::ShadowProgram& a_program, DXGI_FORMAT a_depthFormat,
		bool* a_requested)
	{
		if (a_requested)
			*a_requested = false;
		if (a_depthFormat == DXGI_FORMAT_UNKNOWN || !Enabled())
			return kNotReady;
		if (impl->shadowDepthFormat != a_depthFormat) {
			if (impl->shadowDepthFormat != DXGI_FORMAT_UNKNOWN) {
				// The shadow maps were recreated in another format: every pipeline depended on it.
				logger::info("[DCLF] shadow map format changed ({} -> {}); rebuilding {} shadow pipelines",
					static_cast<int>(impl->shadowDepthFormat), static_cast<int>(a_depthFormat), impl->shadowSetPipelines.size());
				impl->shadowEntries.clear();
				impl->shadowVersions.Retire();
				impl->shadowSetPipelines.clear();
				shadowUsage.clear();
				shadowDiscards.clear();
				shadowLayouts.clear();
				impl->shadowInFlight = 0;
				stats.shadowRequested = stats.shadowReady = stats.shadowFailed = 0;
				impl->shadowRecreatedBy = fmt::format("shadow map format {} -> {}", static_cast<int>(impl->shadowDepthFormat), static_cast<int>(a_depthFormat));
			}
			impl->shadowDepthFormat = a_depthFormat;
		}
		if (auto it = impl->shadowEntries.find(a_key); it != impl->shadowEntries.end())
			return it->second.index;
		if (impl->shadowInFlight >= kMaxInFlight || impl->shadowSetPipelines.size() >= kMaxPipelines)
			return kNotReady;
		// The view's rasterizer state: every shadow pipeline is built for one (ShadowRasterStateId).
		const std::uint32_t stateId = a_key.viewState;
		if (stateId == 0 || stateId > impl->shadowRasterStates.size())
			return kNotReady;
		const Impl::ShadowRasterState state = impl->shadowRasterStates[stateId - 1];
		org::services::PipelineRecipe recipe;
		recipe.id = fmt::format("dclf.shadow.{:08X}.{:X}.{:016X}.{}", a_key.technique, a_key.rasterFlags, a_key.vertexLayout, a_key.viewState);
		recipe.shaderKey = ShadowPipelineKeyHash{}(a_key);
		recipe.fixedFunctionKey = static_cast<std::uint64_t>(a_depthFormat);
		recipe.build = [device = impl->device, layout = impl->ShadowLayout(), key = a_key, program = &a_program, format = a_depthFormat, state] {
			return Impl::BuildShadow(device, layout, key, program, format, state);
		};
		auto& entry = impl->shadowEntries[a_key];
		entry.future = impl->service.Request(std::move(recipe));
		++impl->shadowInFlight;
		++stats.shadowRequested;
		if (a_requested)
			*a_requested = true;
		return kNotReady;
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
		const std::uint32_t modeBit = a_renderMode >= 0xC && a_renderMode < 0xC + 32 ? 1u << (a_renderMode - 0xC) : 0u;
		auto& states = impl->shadowRasterStates;
		for (std::size_t i = 0; i < states.size(); ++i) {
			if (states[i] == state) {
				impl->shadowRasterStateModes[i] |= modeBit;
				return static_cast<std::uint32_t>(i + 1);
			}
		}
		states.push_back(state);
		impl->shadowRasterStateModes.push_back(modeBit);
		logger::info("[DCLF] shadow view rasterizer state {} (mode {:#x}): depth bias {} (clamp {}, slope {}), cull {}, front {}", states.size(), a_renderMode,
			state.depthBias, state.depthBiasClamp, state.slopeScaledDepthBias, static_cast<int>(a_desc.CullMode), a_desc.FrontCounterClockwise ? "CCW" : "CW");
		return static_cast<std::uint32_t>(states.size());
	}

	std::uint32_t DrawPipelines::ShadowRasterStateCount() const
	{
		return static_cast<std::uint32_t>(impl->shadowRasterStates.size());
	}

	std::vector<std::uint32_t> DrawPipelines::ShadowRasterStatesOfMode(std::uint32_t a_renderMode) const
	{
		std::vector<std::uint32_t> ids;
		if (a_renderMode < 0xC || a_renderMode >= 0xC + 32)
			return ids;
		for (std::size_t i = 0; i < impl->shadowRasterStateModes.size(); ++i)
			if (impl->shadowRasterStateModes[i] & (1u << (a_renderMode - 0xC)))
				ids.push_back(static_cast<std::uint32_t>(i + 1));
		return ids;
	}

	void DrawPipelines::CaptureEngineStates(std::span<const PipelineKey> a_keys)
	{
		for (const auto& key : a_keys) {
			const auto bits = RasterStateBits(key.rasterFlags);
			if (!bits || impl->engineStates.contains(bits))
				continue;
			std::string error;
			auto state = impl->ReadEngineState(bits, error);
			if (!state.valid && impl->loggedStateFailures++ < kMaxLoggedFailures)
				logger::warn("[DCLF] pipeline state {:05X} cannot be built: {}; its objects stay native", bits, error);
			else if (state.valid)
				logger::info("[DCLF] pipeline state {:05X}: depth bias {} (clamp {}, slope {}), blend rt0 {} mask {:X}, rt1 {} mask {:X}",
					bits, state.depthBias, state.depthBiasClamp, state.slopeScaledDepthBias, state.blend.attachments[0].enable ? "on" : "off",
					static_cast<unsigned>(state.blend.attachments[0].writeMask), state.blend.attachments[1].enable ? "on" : "off",
					static_cast<unsigned>(state.blend.attachments[1].writeMask));
			impl->engineStates.emplace(bits, state);
		}
	}

	void DrawPipelines::Update()
	{
		if (!impl->supported)
			return;
		for (auto& [key, entry] : impl->entries) {
			if (entry.index != kNotReady || entry.failed || !entry.future.valid() || entry.future.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
				continue;
			--impl->inFlight;
			const auto& artifact = entry.future.get();
			entry.future = {};
			if (!artifact) {
				entry.failed = true;
				++stats.failed;
				if (impl->loggedFailures++ < kMaxLoggedFailures)
					logger::warn("[DCLF] Indirect pipeline VS {:08X} PS {:08X} layout {:016X} failed: {}", key.vertexDescriptor, key.pixelDescriptor, key.vertexLayout, artifact.error);
				continue;
			}
			const auto* built = static_cast<const Impl::Built*>(artifact.payload.get());
			entry.slot = static_cast<std::uint32_t>(impl->setPipelines.size());
			impl->setPipelines.push_back(artifact.payload);
			usage.push_back(built->usage);
		}
		// Publish the admitted pipelines (Impl::SetVersion), then hand out the indices the published version holds.
		const auto admitted = static_cast<std::uint32_t>(impl->setPipelines.size());
		if (admitted) {
			auto pipelineAt = [&](std::uint32_t a_variant) {
				return [&, a_variant](std::uint32_t a_index) {
					return static_cast<const Impl::Built*>(impl->setPipelines[a_index].get())->pipelines[a_variant]->GetHandle();
				};
			};
			const auto result = impl->versions.Publish(admitted, [&](Impl::SetVersion& a_version) {
				for (std::uint32_t variant = 0; variant < kVariantCount; ++variant) {
					if (!impl->ApplyToSet(a_version.sets[variant], a_version.applied, admitted,
							variant == kDepthVariant ? "DCLF indirect pipelines (depth)" : "DCLF indirect pipelines", pipelineAt(variant),
							[&] { return impl->CreateSignature(a_version, variant); }))
						return false;
				}
				return true;
			});
			using Result = decltype(result);
			stats.setPublishes += result == Result::Published;
			stats.setWaits += result == Result::Waiting;
			const bool published = result == Result::Current || result == Result::Published;
			if (published && impl->versions.handedOut != admitted) {
				impl->versions.handedOut = admitted;
				for (auto& [key, entry] : impl->entries) {
					if (entry.index == kNotReady && entry.slot != kNotReady) {
						entry.index = entry.slot;
						++stats.ready;
					}
				}
			} else if (result == Result::Failed && !impl->versions.failureLogged) {
				impl->versions.failureLogged = true;
				logger::warn("[DCLF] Could not publish the indirect pipeline sets ({} pipelines admitted)", admitted);
			}
		}
		for (auto& [key, entry] : impl->shadowEntries) {
			if (entry.index != kNotReady || entry.failed || !entry.future.valid() || entry.future.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
				continue;
			--impl->shadowInFlight;
			const auto& artifact = entry.future.get();
			entry.future = {};
			if (!artifact) {
				entry.failed = true;
				++stats.shadowFailed;
				if (impl->loggedFailures++ < kMaxLoggedFailures)
					logger::warn("[DCLF] shadow pipeline technique {:08X} layout {:016X} failed: {}", key.technique, key.vertexLayout, artifact.error);
				continue;
			}
			const auto* built = static_cast<const Impl::Built*>(artifact.payload.get());
			entry.slot = static_cast<std::uint32_t>(impl->shadowSetPipelines.size());
			impl->shadowSetPipelines.push_back(artifact.payload);
			shadowUsage.push_back(built->usage[kColorVariant]);
			shadowDiscards.push_back(built->discards ? 1 : 0);
			shadowLayouts.push_back(key.vertexLayout);
		}
		const auto shadowAdmitted = static_cast<std::uint32_t>(impl->shadowSetPipelines.size());
		if (shadowAdmitted) {
			const auto result = impl->shadowVersions.Publish(shadowAdmitted, [&](Impl::ShadowSetVersion& a_version) {
				a_version.pipelines.assign(impl->shadowSetPipelines.begin(), impl->shadowSetPipelines.begin() + shadowAdmitted);
				a_version.layouts.assign(shadowLayouts.begin(), shadowLayouts.begin() + shadowAdmitted);
				a_version.discards.assign(shadowDiscards.begin(), shadowDiscards.begin() + shadowAdmitted);
				return true;
			});
			using Result = decltype(result);
			stats.shadowSetPublishes += result == Result::Published;
			stats.shadowSetWaits += result == Result::Waiting;
			const bool published = result == Result::Current || result == Result::Published;
			if (published && impl->shadowVersions.handedOut != shadowAdmitted) {
				impl->shadowVersions.handedOut = shadowAdmitted;
				for (auto& [key, entry] : impl->shadowEntries) {
					if (entry.index == kNotReady && entry.slot != kNotReady) {
						entry.index = entry.slot;
						++stats.shadowReady;
					}
				}
			} else if (result == Result::Failed && !impl->shadowVersions.failureLogged) {
				impl->shadowVersions.failureLogged = true;
				logger::warn("[DCLF] Could not publish the shadow pipeline set ({} pipelines admitted)", shadowAdmitted);
			}
		}
		impl->service.PublishReady(0);
	}

	ShadowIndirectState GetShadowIndirectState()
	{
		const auto& impl = *DrawPipelines::Get().impl;
		ShadowIndirectState state;
		auto version = impl.shadowVersions.published.load();
		if (!impl.layout || !version)
			return state;
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

	IndirectState GetIndirectState()
	{
		const auto& impl = *DrawPipelines::Get().impl;
		IndirectState state;
		auto version = impl.versions.published.load();
		if (!version || !impl.layout)
			return state;
		for (std::uint32_t variant = 0; variant < kVariantCount; ++variant) {
			state.sets[variant] = version->sets[variant]->GetHandle();
			state.signatures[variant] = version->signatures[variant]->GetHandle();
		}
		state.depthPassSignature = version->depthPassSignature ? version->depthPassSignature->GetHandle() : state.signatures[kDepthVariant];
		state.version = std::move(version);
		state.layout = impl.layout->GetHandle();
		state.valid = true;
		return state;
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
	std::uint32_t DrawPipelines::Find(const PipelineKey&, const ShaderPrograms::Program&, bool*) { return kNotReady; }
	std::uint32_t DrawPipelines::FindShadow(const ShadowPipelineKey&, const ShaderPrograms::ShadowProgram&, DXGI_FORMAT, bool*) { return kNotReady; }
	std::string DrawPipelines::NearestKey(const PipelineKey&) const { return {}; }
	const ShaderPrograms::ShadowProgram* RequestShadowProgram(std::uint32_t a_technique, const ShadowPipelineKey&, std::uint32_t, RE::BSShader& a_utility,
		bool a_allowRequest, bool* a_requested)
	{
		return ShaderPrograms::Get().FindShadow(a_technique, a_utility, a_allowRequest, a_requested);
	}
	std::uint32_t RequestShadowPipeline(const ShadowPipelineKey&, const ShaderPrograms::ShadowProgram&, DXGI_FORMAT, const ShadowPipelineKey&, std::uint32_t)
	{
		return DrawPipelines::kNotReady;
	}
	std::string DrawPipelines::NearestShadowKey(const ShadowPipelineKey&) const { return {}; }
	std::uint32_t DrawPipelines::ShadowRasterStateId(const D3D11_RASTERIZER_DESC&, std::uint32_t) { return 0; }
	std::vector<std::uint32_t> DrawPipelines::ShadowRasterStatesOfMode(std::uint32_t) const { return {}; }
	void DrawPipelines::Update() {}
	void DrawPipelines::CaptureEngineStates(std::span<const PipelineKey>) {}
}

#endif
