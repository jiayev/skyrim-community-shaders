#include "Rendering/EnvironmentProbe/Shadows.h"
#include "Globals.h"
#include "Rendering/EnvironmentProbe/Capture.h"
#include "Rendering/EnvironmentProbe/Scene.h"
#include "State.h"
#include "Util.h"

#include <atomic>
#include <deque>
#include <mutex>

namespace EnvironmentProbe::Shadows
{
	namespace
	{
		constexpr uint32_t resolution = 512;
		constexpr auto depthSlot = RE::RENDER_TARGETS_DEPTHSTENCIL::kCUBEMAP_REFLECTIONS;
		std::unique_ptr<Texture2D> texture;
		Parameters parameters{};
		RE::NiPointer<RE::NiCamera> camera;
		std::atomic<RE::BSShaderAccumulator*> active = nullptr;
		std::deque<RE::BSRenderPass> passes;
		std::mutex passMutex;
		winrt::com_ptr<ID3D11DepthStencilState> depthState;
		winrt::com_ptr<ID3D11RasterizerState> rasterState;

		struct ShadowPasses
		{
			static RE::BSShaderProperty::RenderPassArray* thunk(RE::BSLightingShaderProperty* property, RE::BSGeometry* geometry,
				uint32_t mode, RE::BSShaderAccumulator* accumulator)
			{
				if (accumulator != active.load())
					return func(property, geometry, mode, accumulator);
				// Private nodes: native shadow pass queues also belong to the unfinished world view.
				// Never regenerate or relink those queues for this additional camera.
				thread_local RE::BSShaderProperty::RenderPassArray result{};
				result.head = nullptr;
				using enum RE::BSShaderProperty::EShaderPropertyFlag;
				if (property->flags.none(kCastShadows) || property->flags.none(kZBufferWrite) ||
					property->flags.any(kRefraction, kTempRefraction, kDecal, kDynamicDecal))
					return &result;
				auto* alpha = static_cast<RE::NiAlphaProperty*>(geometry->GetGeometryRuntimeData().alphaProperty.get());
				if (alpha && alpha->GetAlphaBlending() && !alpha->GetAlphaTesting())
					return &result;
				uint32_t technique = uint32_t(RE::BSUtilityShader::Flags::RenderShadowmap);
				if (property->flags.any(kVertexColors))
					technique |= uint32_t(RE::BSUtilityShader::Flags::Vc);
				if (property->flags.any(kTreeAnim))
					technique |= uint32_t(RE::BSUtilityShader::Flags::TreeAnim);
				if (alpha && alpha->GetAlphaTesting())
					technique |= uint32_t(RE::BSUtilityShader::Flags::AlphaTest) | uint32_t(RE::BSUtilityShader::Flags::Texture);
				if (property->flags.any(kLODObjects, kHDLODObjects))
					technique |= uint32_t(RE::BSUtilityShader::Flags::LodObject);
				std::scoped_lock lock(passMutex);
				auto& pass = passes.emplace_back();
				pass.shader = globals::game::utilityShader;
				pass.shaderProperty = property;
				pass.geometry = geometry;
				pass.passEnum = technique + 0x2B;
				pass.LODMode.index = 3;
				result.head = &pass;
				return &result;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	void InstallHooks() { stl::write_vfunc<0x2B, ShadowPasses>(RE::VTABLE_BSLightingShaderProperty[0]); }
	ID3D11ShaderResourceView* Map() { return texture ? texture->srv.get() : nullptr; }
	Parameters Data() { return parameters; }
	void Reset() { parameters.enabled = 0; }

	void BeforeDraw()
	{
		if (!active.load())
			return;
		auto& state = globals::game::shadowState->GetRuntimeData();
		if (state.depthStencil != depthSlot || state.depthStencilSlice != 0 || state.cubeMapRenderTarget != UINT_MAX ||
			std::any_of(std::begin(state.renderTargets), std::end(state.renderTargets), [](uint32_t target) { return target != RE::RENDER_TARGET::kNONE; }))
			state.stateUpdateFlags.set(RE::BSGraphics::DIRTY_RENDERTARGET);
		if (state.viewPort.TopLeftX != 0 || state.viewPort.TopLeftY != 0 || state.viewPort.Width != resolution ||
			state.viewPort.Height != resolution || state.viewPort.MinDepth != 0 || state.viewPort.MaxDepth != 1)
			state.stateUpdateFlags.set(RE::BSGraphics::DIRTY_VIEWPORT);
		std::fill(std::begin(state.renderTargets), std::end(state.renderTargets), RE::RENDER_TARGET::kNONE);
		state.cubeMapRenderTarget = UINT_MAX;
		state.depthStencil = depthSlot;
		state.depthStencilSlice = 0;
		state.setDepthStencilMode = RE::BSGraphics::SRTM_NO_CLEAR;
		state.viewPort = { 0, 0, float(resolution), float(resolution), 0, 1 };
	}

	void AfterDraw()
	{
		if (!active.load())
			return;
		auto* context = globals::d3d::context;
		context->GSSetShader(nullptr, nullptr, 0);
		context->OMSetRenderTargets(0, nullptr, texture->dsv.get());
		context->OMSetDepthStencilState(depthState.get(), 0);
		context->RSSetState(rasterState.get());
	}

	bool RejectGeometry(RE::BSCullingProcess* process, RE::BSGeometry& geometry)
	{
		if (!active.load() || process->camera != camera.get())
			return false;
		auto* property = netimmerse_cast<RE::BSLightingShaderProperty*>(geometry.GetGeometryRuntimeData().shaderProperty.get());
		using enum RE::BSShaderProperty::EShaderPropertyFlag;
		if (!property || geometry.worldBound.radius <= 32.f || property->flags.none(kZBufferWrite, kCastShadows) ||
			property->flags.any(kRefraction, kTempRefraction, kLODLandscape, kEyeReflect, kDecal, kDynamicDecal) ||
			(property->flags.any(kSkinned) && property->flags.none(kTreeAnim)))
			return true;
		if (auto* reference = geometry.GetUserData()) {
			auto* base = reference->GetBaseObject();
			if (!base || (!base->Is(RE::FormType::Static) && !base->Is(RE::FormType::Tree)))
				return true;
		}
		static const RE::BSFixedString bsxKey{ "BSX" };
		for (auto* node = static_cast<RE::NiAVObject*>(&geometry); node; node = node->parent) {
			if (auto* extra = node->GetExtraData(bsxKey)) {
				using F = RE::BSXFlags::Flag;
				constexpr uint32_t dynamicFlags = uint32_t(F::kRagdoll) | uint32_t(F::kDynamic) | uint32_t(F::kAddon) |
				                                  uint32_t(F::kNeedsTransformUpdate) | uint32_t(F::kMagicShaderParticles) | uint32_t(F::kLights) |
				                                  uint32_t(F::kBreakable) | uint32_t(F::kSearchedBreakable);
				if (uint32_t(static_cast<RE::BSXFlags*>(extra)->value) & dynamicFlags)
					return true;
			}
		}
		return false;
	}

	void Render(const Request& request)
	{
		parameters = {};
		if (!request.supplementalSunShadows)
			return;
		auto* scene = globals::game::smState->shadowSceneNode[0];
		auto* sun = scene->GetRuntimeData().sunShadowDirLight;
		if (!sun || !globals::state->HasDirectionalShadows())
			return;
		auto forward = sun->GetShadowDirectionalLightRuntimeData().sunVector;
		if (forward.SqrLength() < 0.5f)
			return;
		forward.Unitize();
		if (!texture) {
			D3D11_TEXTURE2D_DESC desc{};
			desc.Width = desc.Height = resolution;
			desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
			desc.Format = DXGI_FORMAT_R32_TYPELESS;
			desc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
			texture = std::make_unique<Texture2D>(desc, "EnvironmentProbe::Coarse Sun Shadow Map");
			texture->CreateDSV({ .Format = DXGI_FORMAT_D32_FLOAT, .ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D });
			texture->CreateSRV({ .Format = DXGI_FORMAT_R32_FLOAT, .ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D, .Texture2D = { .MostDetailedMip = 0, .MipLevels = 1 } });
			D3D11_DEPTH_STENCIL_DESC depth{};
			depth.DepthEnable = true;
			depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
			depth.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
			DX::ThrowIfFailed(globals::d3d::device->CreateDepthStencilState(&depth, depthState.put()));
			Util::SetResourceName(depthState.get(), "EnvironmentProbe::Sun Shadow Depth");
			D3D11_RASTERIZER_DESC raster{};
			raster.FillMode = D3D11_FILL_SOLID;
			raster.CullMode = D3D11_CULL_NONE;  // Thin walls and foliage cast from both sides.
			raster.DepthClipEnable = false;
			raster.SlopeScaledDepthBias = 1.5f;
			DX::ThrowIfFailed(globals::d3d::device->CreateRasterizerState(&raster, rasterState.put()));
			Util::SetResourceName(rasterState.get(), "EnvironmentProbe::Sun Shadow Rasterizer");
		}
		if (!camera) {
			static REL::Relocation<RE::NiCamera*(void*)> ctor{ REL::RelocationID(69258, 70624) };
			auto* memory = RE::malloc(0x188);
			if (!memory)
				throw std::bad_alloc();
			camera.reset(ctor(memory));
		}
		auto right = forward.Cross(std::abs(forward.z) < 0.99f ? RE::NiPoint3{ 0, 0, 1 } : RE::NiPoint3{ 0, 1, 0 });
		right.Unitize();
		const auto up = right.Cross(forward);
		// Support function of each selected spherical octant. This fits RECEIVERS;
		// casters remain free to lie outside those octants on the ray toward the sun.
		const float radius = std::min(request.farDistance, 5000.f);
		const auto bounds = [&](const RE::NiPoint3& axis) {
			float low = 0, high = 0;
			for (uint32_t face = 0; face < 8; ++face) {
				if (!(request.faces & (1u << face)))
					continue;
				float positive = 0, negative = 0;
				for (uint32_t i = 0; i < 3; ++i) {
					const float value = axis[i] * ((face & (1u << i)) ? -1.f : 1.f);
					(value > 0 ? positive : negative) += value * value;
				}
				low = std::min(low, -radius * std::sqrt(negative));
				high = std::max(high, radius * std::sqrt(positive));
			}
			return std::pair{ low, high };
		};
		const auto [left, rightEdge] = bounds(right);
		const auto [bottom, top] = bounds(up);
		const auto [back, front] = bounds(forward);
		const float halfWidth = (rightEdge - left) * .5f * resolution / (resolution - 4.f);
		const float halfHeight = (top - bottom) * .5f * resolution / (resolution - 4.f);
		auto center = request.position + right * ((left + rightEdge) * .5f) + up * ((bottom + top) * .5f);
		const float texelX = 2 * halfWidth / resolution, texelY = 2 * halfHeight / resolution;
		center += right * (std::round(center.Dot(right) / texelX) * texelX - center.Dot(right));
		center += up * (std::round(center.Dot(up) / texelY) * texelY - center.Dot(up));
		const float upstream = std::max(-back, (center - scene->worldBound.center).Dot(forward) + scene->worldBound.radius);
		const float nearPlane = 1;
		const float farPlane = upstream + front + nearPlane;
		const auto origin = center - forward * (upstream + nearPlane);
		for (uint32_t row = 0; row < 3; ++row) {
			camera->local.rotate.entry[row][0] = forward[row];
			camera->local.rotate.entry[row][1] = up[row];
			camera->local.rotate.entry[row][2] = right[row];
		}
		camera->local.translate = origin;
		camera->local.scale = 1;
		camera->GetRuntimeData2().viewFrustum = { -halfWidth, halfWidth, halfHeight, -halfHeight, nearPlane, farPlane, true };
		camera->GetRuntimeData2().port = { 0, 1, 1, 0 };
		RE::NiUpdateData update{};
		camera->UpdateWorldData(&update);
		// Column-major HLSL storage. Coordinates are relative to the probe, avoiding
		// cancellation between large absolute coordinates in the receiver shader.
		const auto delta = request.position - origin;
		const float scaleX = 1 / (2 * halfWidth), scaleY = 1 / (2 * halfHeight), depthScale = 1 / (farPlane - nearPlane);
		parameters.projection = {
			right.x * scaleX, -up.x * scaleY, forward.x * depthScale, 0,
			right.y * scaleX, -up.y * scaleY, forward.y * depthScale, 0,
			right.z * scaleX, -up.z * scaleY, forward.z * depthScale, 0,
			0.5f + delta.Dot(right) * scaleX, 0.5f - delta.Dot(up) * scaleY, (delta.Dot(forward) - nearPlane) * depthScale, 1
		};
		parameters.depthBias = depthScale;  // One game unit of receiver bias.
		parameters.enabled = 1;

		static REL::Relocation<RE::BSShaderAccumulator*(void*, uint32_t)> ctor{ REL::RelocationID(99920, 106564) };
		auto* memory = RE::malloc(0x180);
		if (!memory)
			throw std::bad_alloc();
		RE::NiPointer<RE::BSShaderAccumulator> accumulator{ ctor(memory, 99) };
		passes.clear();
		struct Scope
		{
			uint32_t utilityTechnique = globals::game::utilityShader->currentTechnique;
			uint32_t utilityMask = globals::game::utilityShader->currentTechniqueMask;
			RE::BSGraphics::BSShaderAccumulator* previous = *globals::game::currentAccumulator;
			RE::BSShaderAccumulator::RENDER_MODE mode = previous ?
			                                                static_cast<RE::BSShaderAccumulator::RENDER_MODE>(previous->GetRuntimeData().renderMode) :
			                                                RE::BSShaderAccumulator::RENDER_MODE::kNormal;
			~Scope()
			{
				active.store(nullptr);
				*globals::game::currentAccumulator = previous;
				RE::BSShaderManager::SetRenderMode(mode);
				globals::game::utilityShader->currentTechnique = utilityTechnique;
				globals::game::utilityShader->currentTechniqueMask = utilityMask;
			}
		} scope;
		active.store(accumulator.get());
		auto* runtime = accumulator->GetRuntimeData();
		runtime->activeShadowSceneNode = scene;
		runtime->renderMode = RE::BSShaderAccumulator::RENDER_MODE::kShadowMapPlain;
		runtime->eyePosition = request.position;
		accumulator->StartAccumulating(camera.get());
		RE::CullJobDescriptor job{};
		job.shaderAccumulator = accumulator;
		job.camera = camera;
		job.frustum = &camera->GetRuntimeData2().viewFrustum;
		job.scene = scene;
		job.cullMode = 3;
		job.unk58 = UINT_MAX;
		job.ignorePreprocess = true;
		job.cameraRelatedUpdates = false;
		job.updateAccumulateFlag = false;
		auto roots = Scene::Roots(scene);
		job.cullingObjects = &roots;
		job.unk61 = true;
		static REL::Relocation<void(RE::CullJobDescriptor*, uint32_t, uint32_t)> cull{ REL::RelocationID(100213, 106921) };
		cull(&job, roots.empty() ? 0 : 1, 0);
		auto& depth = globals::game::renderer->GetDepthStencilData().depthStencils[depthSlot];
		depth = {};
		depth.texture = texture->resource.get();
		depth.views[0] = texture->dsv.get();
		globals::d3d::context->ClearDepthStencilView(texture->dsv.get(), D3D11_CLEAR_DEPTH, 1, 0);
		globals::game::graphicsState->SetCameraData(camera.get(), 0);
		*globals::game::currentAccumulator = reinterpret_cast<RE::BSGraphics::BSShaderAccumulator*>(accumulator.get());
		BeforeDraw();
		accumulator->FinishAccumulatingPreResolveDepth(0);
		accumulator->FinishAccumulatingPostResolveDepth(0);
		globals::d3d::context->OMSetRenderTargets(0, nullptr, nullptr);
	}
}
