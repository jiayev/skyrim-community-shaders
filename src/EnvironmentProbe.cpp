#include "EnvironmentProbe.h"

#include "Deferred.h"
#include "Features/Effects11/D3D11StateBackup.h"
#include "Features/LightLimitFix.h"
#include "Features/Skin.h"
#include "Features/TerrainBlending.h"
#include "Globals.h"
#include "ShaderCache.h"
#include "State.h"
#include "Util.h"

#include <atomic>
#include <cmath>

namespace EnvironmentProbe
{
	namespace
	{
		constexpr auto targetSlot = RE::RENDER_TARGETS::kWATER_REFLECTIONS;
		constexpr auto depthSlot = RE::RENDER_TARGETS_DEPTHSTENCIL::kCUBEMAP_REFLECTIONS;

		struct alignas(16) CaptureData
		{
			float3 faceSign;
			uint32_t faceMask;
			float nearDistance;
			float farDistance;
			uint32_t lightCount;
			uint32_t textureSize;
			float3 worldOriginDelta;
			float padding2{};
			float3 lightOriginDelta;
			float padding3{};
		};
		static_assert(sizeof(CaptureData) == 64);

		template <class T>
		struct RestoreValue
		{
			T& value;
			T saved = value;
			~RestoreValue() { value = saved; }
		};

		struct RestoreRenderMode
		{
			RE::BSShaderAccumulator::RENDER_MODE mode;
			~RestoreRenderMode() { RE::BSShaderManager::SetRenderMode(mode); }
		};

		struct Resources
		{
			std::array<RE::NiPointer<RE::NiCamera>, 8> cameras;
			RE::NiPointer<RE::BSShaderAccumulator> accumulator;
			winrt::com_ptr<ID3D11Texture2D> depth;
			winrt::com_ptr<ID3D11DepthStencilView> dsv;
			winrt::com_ptr<ID3D11VertexShader> clearVS;
			winrt::com_ptr<ID3D11PixelShader> clearPS;
			winrt::com_ptr<ID3D11RasterizerState> clearRaster;
			winrt::com_ptr<ID3D11DepthStencilState> clearDepth;
			winrt::com_ptr<ID3D11BlendState> clearBlend;
			std::unique_ptr<ConstantBuffer> captureCB;
			std::unique_ptr<ConstantBuffer> sharedCB;
			std::vector<winrt::com_ptr<ID3D11Buffer>> bufferCopies;
			struct BlendOverride
			{
				winrt::com_ptr<ID3D11BlendState> original;
				winrt::com_ptr<ID3D11BlendState> withDepth;
				winrt::com_ptr<ID3D11BlendState> withoutDepth;
			};
			std::vector<BlendOverride> blends;
			uint32_t size = 0;

			bool Ensure(uint32_t a_size)
			{
				auto* device = globals::d3d::device;
				if (!captureCB) {
					captureCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<CaptureData>(), "EnvironmentProbe::Capture");
					sharedCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<SharedDataCB>(), "EnvironmentProbe::SharedData");
				}
				if (!clearVS)
					clearVS.attach(static_cast<ID3D11VertexShader*>(Util::CompileShader(L"Data/Shaders/EnvironmentProbeClear.hlsl", { { "VSHADER", "1" } }, "vs_5_0")));
				if (!clearPS)
					clearPS.attach(static_cast<ID3D11PixelShader*>(Util::CompileShader(L"Data/Shaders/EnvironmentProbeClear.hlsl", {}, "ps_5_0")));
				if (!clearVS || !clearPS)
					return false;
				if (!clearRaster) {
					Util::SetResourceName(clearVS.get(), "EnvironmentProbe::Clear VS");
					Util::SetResourceName(clearPS.get(), "EnvironmentProbe::Clear PS");
					D3D11_RASTERIZER_DESC raster{};
					raster.FillMode = D3D11_FILL_SOLID;
					raster.CullMode = D3D11_CULL_NONE;
					raster.DepthClipEnable = true;
					DX::ThrowIfFailed(device->CreateRasterizerState(&raster, clearRaster.put()));
					Util::SetResourceName(clearRaster.get(), "EnvironmentProbe::Clear Rasterizer");
					D3D11_DEPTH_STENCIL_DESC ds{};
					ds.DepthFunc = D3D11_COMPARISON_ALWAYS;
					DX::ThrowIfFailed(device->CreateDepthStencilState(&ds, clearDepth.put()));
					Util::SetResourceName(clearDepth.get(), "EnvironmentProbe::Clear DepthState");
					D3D11_BLEND_DESC blend{};
					blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
					DX::ThrowIfFailed(device->CreateBlendState(&blend, clearBlend.put()));
					Util::SetResourceName(clearBlend.get(), "EnvironmentProbe::Clear Blend");
				}
				if (size != a_size) {
					dsv = nullptr;
					depth = nullptr;
					D3D11_TEXTURE2D_DESC desc{};
					desc.Width = desc.Height = a_size;
					desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
					desc.Format = DXGI_FORMAT_D32_FLOAT;
					desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
					DX::ThrowIfFailed(device->CreateTexture2D(&desc, nullptr, depth.put()));
					Util::SetResourceName(depth.get(), "EnvironmentProbe::Radial Z");
					DX::ThrowIfFailed(device->CreateDepthStencilView(depth.get(), nullptr, dsv.put()));
					Util::SetResourceName(dsv.get(), "EnvironmentProbe::Radial Z DSV");
					size = a_size;
				}
				if (!accumulator) {
					// Verified constructors: SE 1.5.97 / AE 1.6.1170. CommonLib's cross-runtime
					// base sizes omit the runtime tail; allocate the verified flat object sizes.
					static REL::Relocation<RE::BSShaderAccumulator*(void*, uint32_t)> ctor{ REL::RelocationID(99920, 106564) };
					auto* memory = RE::malloc(0x180);
					if (!memory)
						throw std::bad_alloc();
					accumulator.reset(ctor(memory, 99));
				}
				return true;
			}

			RE::NiCamera* Camera(uint32_t a_face, const Request& a_request)
			{
				if (!cameras[a_face]) {
					static REL::Relocation<RE::NiCamera*(void*)> ctor{ REL::RelocationID(69258, 70624) };
					auto* memory = RE::malloc(0x188);
					if (!memory)
						throw std::bad_alloc();
					cameras[a_face].reset(ctor(memory));
				}
				auto* camera = cameras[a_face].get();
				RE::NiPoint3 forward{ a_face & 1 ? -1.f : 1.f, a_face & 2 ? -1.f : 1.f, a_face & 4 ? -1.f : 1.f };
				forward.Unitize();
				auto right = forward.Cross({ 0, 0, 1 });
				right.Unitize();
				const auto up = right.Cross(forward);
				// NiCamera columns are forward, up, right (verified in UpdateWorldData).
				for (uint32_t row = 0; row < 3; ++row) {
					camera->local.rotate.entry[row][0] = forward[row];
					camera->local.rotate.entry[row][1] = up[row];
					camera->local.rotate.entry[row][2] = right[row];
				}
				camera->local.translate = a_request.position;
				camera->local.scale = 1;
				auto& data = camera->GetRuntimeData2();
				// Conservative frustum enclosing one octant. The Lighting VS performs the
				// exact three-plane clip and the octahedral projection into the final target.
				constexpr float extent = 1.414214f;
				data.viewFrustum = { -extent, extent, extent, -extent, a_request.nearDistance / 1.732051f, a_request.farDistance, false };
				data.port = { 0, 1, 1, 0 };
				RE::NiUpdateData update{};
				camera->UpdateWorldData(&update);
				return camera;
			}
		};

		struct CaptureState
		{
			const Request& request;
			RE::NiCamera* camera = nullptr;
			DWORD thread = GetCurrentThreadId();
			bool shaderFailed = false;
		};
		std::atomic<CaptureState*> active = nullptr;
		Resources resources;

		bool IncludeGeometry(RE::BSGeometry& a_geometry, bool a_cullDynamic)
		{
			const auto& data = a_geometry.GetGeometryRuntimeData();
			if (!netimmerse_cast<RE::BSLightingShaderProperty*>(data.shaderProperty.get()))
				return false;
			if (!a_cullDynamic)
				return true;
			if (data.skinInstance || data.shaderProperty->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kSkinned))
				return false;
			if (const auto* reference = a_geometry.GetUserData()) {
				const auto* base = reference->GetBaseObject();
				return base && (base->Is(RE::FormType::Static) || base->Is(RE::FormType::Tree));
			}
			for (auto* node = static_cast<RE::NiAVObject*>(&a_geometry); node; node = node->parent)
				if (node->controllers)
					return false;
			return true;  // Unowned terrain / static scene geometry.
		}

		struct AppendGeometry
		{
			static void thunk(RE::BSCullingProcess* a_process, RE::BSGeometry& a_geometry, int32_t a_group)
			{
				const auto* capture = active.load();
				if (capture && a_process->camera == capture->camera && !IncludeGeometry(a_geometry, capture->request.cullDynamic))
					return;
				func(a_process, a_geometry, a_group);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct SavedPipeline
		{
			Effects11Util::D3D11FullStateBackup bindings;
			std::vector<std::pair<ID3D11Buffer*, ID3D11Buffer*>> copies;

			SavedPipeline()
			{
				auto* context = globals::d3d::context;
				bindings.Save(context);
				auto save = [&](ID3D11Buffer* buffer) {
					if (!buffer || std::ranges::any_of(copies, [&](auto entry) { return entry.first == buffer; }))
						return;
					D3D11_BUFFER_DESC desc{};
					buffer->GetDesc(&desc);
					if (desc.Usage == D3D11_USAGE_IMMUTABLE)
						return;
					const auto index = copies.size();
					if (resources.bufferCopies.size() <= index)
						resources.bufferCopies.emplace_back();
					auto& backup = resources.bufferCopies[index];
					D3D11_BUFFER_DESC oldDesc{};
					if (backup)
						backup->GetDesc(&oldDesc);
					if (!backup || oldDesc.ByteWidth != desc.ByteWidth) {
						backup = nullptr;
						desc.Usage = D3D11_USAGE_DEFAULT;
						desc.CPUAccessFlags = 0;
						DX::ThrowIfFailed(globals::d3d::device->CreateBuffer(&desc, nullptr, backup.put()));
						Util::SetResourceName(backup.get(), "EnvironmentProbe::Saved Constants %zu", index);
					}
					context->CopyResource(backup.get(), buffer);
					copies.emplace_back(buffer, backup.get());
				};
				for (auto* buffer : bindings.vsCBs) save(buffer);
				for (auto* buffer : bindings.psCBs) save(buffer);
				for (auto* buffer : bindings.csCBs) save(buffer);
				save(*globals::game::perFrame);
				save(globals::state->permutationCB->CB());
				if (globals::features::lightLimitFix.strictLightDataCB)
					save(globals::features::lightLimitFix.strictLightDataCB->CB());
				if (globals::features::skin.PerGeometryCB)
					save(globals::features::skin.PerGeometryCB->CB());
			}

			~SavedPipeline()
			{
				auto* context = globals::d3d::context;
				context->OMSetRenderTargets(0, nullptr, nullptr);
				for (auto [original, backup] : copies)
					context->CopyResource(original, backup);
				bindings.Restore(context);
			}
		};

		bool ValidateTarget(ID3D11RenderTargetView* a_view, bool a_depth, uint32_t& a_size, ID3D11Resource*& a_resource)
		{
			if (!a_view)
				return true;
			D3D11_RENDER_TARGET_VIEW_DESC view{};
			a_view->GetDesc(&view);
			if (view.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2D ||
				(a_depth ? view.Format != DXGI_FORMAT_R32_FLOAT :
						   (view.Format != DXGI_FORMAT_R16G16B16A16_FLOAT && view.Format != DXGI_FORMAT_R32G32B32A32_FLOAT)))
				return false;
			winrt::com_ptr<ID3D11Resource> resource;
			a_view->GetResource(resource.put());
			const auto texture = resource.try_as<ID3D11Texture2D>();
			if (!texture)
				return false;
			winrt::com_ptr<ID3D11Device> device;
			resource->GetDevice(device.put());
			D3D11_TEXTURE2D_DESC desc{};
			texture->GetDesc(&desc);
			const auto width = std::max(1u, desc.Width >> view.Texture2D.MipSlice);
			const auto height = std::max(1u, desc.Height >> view.Texture2D.MipSlice);
			if (device.get() != globals::d3d::device || desc.SampleDesc.Count != 1 || width != height || (a_size && a_size != width))
				return false;
			a_size = width;
			a_resource = resource.get();  // The caller's RTV owns it throughout Render.
			return true;
		}

		void Clear(const Request& a_request)
		{
			auto* context = globals::d3d::context;
			context->ClearDepthStencilView(resources.dsv.get(), D3D11_CLEAR_DEPTH, 1, 0);
			ID3D11RenderTargetView* targets[] = { a_request.radiance, a_request.depth };
			context->OMSetRenderTargets(2, targets, nullptr);
			context->OMSetBlendState(resources.clearBlend.get(), nullptr, UINT_MAX);
			context->OMSetDepthStencilState(resources.clearDepth.get(), 0);
			context->RSSetState(resources.clearRaster.get());
			D3D11_VIEWPORT viewport{ 0, 0, float(resources.size), float(resources.size), 0, 1 };
			context->RSSetViewports(1, &viewport);
			context->IASetInputLayout(nullptr);
			context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			context->VSSetShader(resources.clearVS.get(), nullptr, 0);
			context->PSSetShader(resources.clearPS.get(), nullptr, 0);
			auto* capture = resources.captureCB->CB();
			context->PSSetConstantBuffers(8, 1, &capture);
			context->Draw(3, 0);
		}
	}

	void InstallHooks()
	{
		stl::write_vfunc<0x18, AppendGeometry>(RE::VTABLE_BSCullingProcess[0]);
	}

	bool IsRendering()
	{
		const auto* capture = active.load();
		return capture && capture->thread == GetCurrentThreadId();
	}

	void ShaderFailed()
	{
		if (IsRendering())
			active.load()->shaderFailed = true;
	}

	void BeforeDraw()
	{
		if (!IsRendering())
			return;
		auto& state = globals::game::shadowState->GetRuntimeData();
		const bool targetsChanged = state.renderTargets[0] != targetSlot || state.depthStencil != depthSlot ||
		                            state.depthStencilSlice != 0 || state.cubeMapRenderTarget != UINT_MAX ||
		                            std::any_of(std::begin(state.renderTargets) + 1, std::end(state.renderTargets), [](uint32_t target) { return target != RE::RENDER_TARGET::kNONE; });
		if (targetsChanged)
			state.stateUpdateFlags.set(RE::BSGraphics::DIRTY_RENDERTARGET);
		if (state.viewPort.TopLeftX != 0 || state.viewPort.TopLeftY != 0 || state.viewPort.Width != resources.size ||
			state.viewPort.Height != resources.size || state.viewPort.MinDepth != 0 || state.viewPort.MaxDepth != 1)
			state.stateUpdateFlags.set(RE::BSGraphics::DIRTY_VIEWPORT);
		std::fill(std::begin(state.renderTargets), std::end(state.renderTargets), RE::RENDER_TARGET::kNONE);
		std::fill(std::begin(state.setRenderTargetMode), std::end(state.setRenderTargetMode), RE::BSGraphics::SRTM_NO_CLEAR);
		state.renderTargets[0] = targetSlot;
		state.depthStencil = depthSlot;
		state.depthStencilSlice = 0;
		state.setDepthStencilMode = RE::BSGraphics::SRTM_NO_CLEAR;
		state.cubeMapRenderTarget = UINT_MAX;
		state.viewPort = { 0, 0, float(resources.size), float(resources.size), 0, 1 };
	}

	void AfterDraw()
	{
		if (!IsRendering())
			return;
		const auto& request = active.load()->request;
		auto* context = globals::d3d::context;
		ID3D11RenderTargetView* targets[] = { request.radiance, request.depth };
		context->OMSetRenderTargets(2, targets, resources.dsv.get());
		D3D11_RECT scissor{ 0, 0, LONG(resources.size), LONG(resources.size) };
		context->RSSetScissorRects(1, &scissor);
		auto* capture = resources.captureCB->CB();
		auto* shared = resources.sharedCB->CB();
		context->VSSetConstantBuffers(8, 1, &capture);
		context->PSSetConstantBuffers(8, 1, &capture);
		context->VSSetConstantBuffers(5, 1, &shared);
		context->PSSetConstantBuffers(5, 1, &shared);

		// Keep material alpha blending on radiance, but never blend or overwrite the radial
		// depth output in a transparent pass that does not write the hardware depth buffer.
		winrt::com_ptr<ID3D11BlendState> blend;
		float factor[4];
		UINT mask;
		context->OMGetBlendState(blend.put(), factor, &mask);
		winrt::com_ptr<ID3D11DepthStencilState> depth;
		UINT reference;
		context->OMGetDepthStencilState(depth.put(), &reference);
		D3D11_DEPTH_STENCIL_DESC depthDesc{};
		if (depth)
			depth->GetDesc(&depthDesc);
		const bool writesDepth = !depth || (depthDesc.DepthEnable && depthDesc.DepthWriteMask == D3D11_DEPTH_WRITE_MASK_ALL);
		auto it = std::ranges::find_if(resources.blends, [&](const auto& entry) {
			return entry.original == blend || entry.withDepth == blend || entry.withoutDepth == blend;
		});
		if (it == resources.blends.end()) {
			Resources::BlendOverride entry;
			entry.original = blend;
			D3D11_BLEND_DESC desc{};
			if (blend)
				blend->GetDesc(&desc);
			else
				desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
			desc.IndependentBlendEnable = true;
			desc.RenderTarget[1] = desc.RenderTarget[0];
			desc.RenderTarget[1].BlendEnable = false;
			desc.RenderTarget[1].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED;
			DX::ThrowIfFailed(globals::d3d::device->CreateBlendState(&desc, entry.withDepth.put()));
			Util::SetResourceName(entry.withDepth.get(), "EnvironmentProbe::Radiance and Depth Blend");
			desc.RenderTarget[1].RenderTargetWriteMask = 0;
			DX::ThrowIfFailed(globals::d3d::device->CreateBlendState(&desc, entry.withoutDepth.put()));
			Util::SetResourceName(entry.withoutDepth.get(), "EnvironmentProbe::Radiance Blend");
			resources.blends.push_back(std::move(entry));
			it = std::prev(resources.blends.end());
		}
		context->OMSetBlendState(writesDepth ? it->withDepth.get() : it->withoutDepth.get(), factor, mask);
	}

	Result Render(const Request& a_request)
	{
		if (active.load() || REL::Module::IsVR() || !globals::d3d::context || !globals::state->sharedDataCB || globals::state->sharedDataSnapshot.BufferDim.x <= 0 ||
			!globals::shaderCache->IsEnabled() || globals::game::mappedFrameBuffer ||
			!globals::state->enableVShaders || !globals::state->enablePShaders ||
			!globals::state->ShaderEnabled(RE::BSShader::Type::Lighting))
			return Result::Unavailable;
		uint32_t size = 0;
		ID3D11Resource* radianceResource = nullptr;
		ID3D11Resource* depthResource = nullptr;
		if ((!a_request.radiance && !a_request.depth) || (a_request.faces & ~0xFFu) ||
			!std::isfinite(a_request.position.x) || !std::isfinite(a_request.position.y) || !std::isfinite(a_request.position.z) ||
			!std::isfinite(a_request.nearDistance) || !std::isfinite(a_request.farDistance) ||
			a_request.nearDistance <= 0 || a_request.farDistance <= a_request.nearDistance ||
			!ValidateTarget(a_request.radiance, false, size, radianceResource) ||
			!ValidateTarget(a_request.depth, true, size, depthResource) ||
			(radianceResource && radianceResource == depthResource))
			return Result::InvalidRequest;
		if (!a_request.faces)
			return Result::Success;
		auto* scene = globals::game::smState->shadowSceneNode[0];
		if (!scene || globals::features::terrainBlending.renderDepth)
			return Result::Unavailable;

		try {
			if (!resources.Ensure(size))
				return Result::ShaderUnavailable;
			SavedPipeline pipeline;
			auto& state = *globals::state;
			auto* graphics = globals::game::graphicsState;
			auto& renderer = globals::game::renderer->GetRuntimeData();
			auto& depthTargets = globals::game::renderer->GetDepthStencilData().depthStencils;
			RestoreValue shadow{ globals::game::shadowState->GetRuntimeData() };
			RestoreValue frame{ globals::game::frameBufferCached };
			RestoreValue accumulator{ *globals::game::currentAccumulator };
			RestoreRenderMode renderMode{ accumulator.saved ?
											  static_cast<RE::BSShaderAccumulator::RENDER_MODE>(accumulator.saved->GetRuntimeData().renderMode) :
											  RE::BSShaderAccumulator::RENDER_MODE::kNormal };
			RestoreValue target{ renderer.renderTargets[targetSlot] };
			RestoreValue depthTarget{ depthTargets[depthSlot] };
			RestoreValue inWorld{ state.inWorld };
			RestoreValue deferred{ globals::deferred->deferredPass };
			RestoreValue permutation{ state.permutationData };
			RestoreValue previousPermutation{ state.permutationDataPrevious };
			RestoreValue currentShader{ state.currentShader };
			RestoreValue vertex{ state.currentVertexDescriptor };
			RestoreValue pixel{ state.currentPixelDescriptor };
			RestoreValue modifiedVertex{ state.modifiedVertexDescriptor };
			RestoreValue modifiedPixel{ state.modifiedPixelDescriptor };
			RestoreValue lastVertex{ state.lastModifiedVertexDescriptor };
			RestoreValue lastPixel{ state.lastModifiedPixelDescriptor };
			RestoreValue customVertex{ state.customVertexShader };
			RestoreValue customPixel{ state.customPixelShader };
			RestoreValue engineVertex{ *globals::game::currentVertexShader };
			RestoreValue enginePixel{ *globals::game::currentPixelShader };
			RestoreValue updateShader{ state.updateShader };
			RestoreValue nearPlane{ *globals::game::cameraNear };
			RestoreValue farPlane{ *globals::game::cameraFar };
			RestoreValue jitterX{ graphics->projectionPosScaleX };
			RestoreValue jitterY{ graphics->projectionPosScaleY };
			RestoreValue width{ graphics->screenWidth };
			RestoreValue height{ graphics->screenHeight };
			RestoreValue dynamicWidth{ graphics->GetRuntimeData().dynamicResolutionWidthRatio };
			RestoreValue dynamicHeight{ graphics->GetRuntimeData().dynamicResolutionHeightRatio };
			RestoreValue technique{ globals::game::smState->currentShaderTechnique };
			auto& llf = globals::features::lightLimitFix;
			RestoreValue strictData{ llf.strictLightDataTemp };
			RestoreValue wasEmpty{ llf.wasEmpty };
			RestoreValue wasWorld{ llf.wasWorld };
			RestoreValue wasFirstPerson{ llf.wasFirstPerson };
			RestoreValue previousEye{ llf.previousWorldEyePosition };
			RestoreValue previousRoom{ llf.previousRoomIndex };
			RestoreValue previousMask{ llf.previousShadowBitMask };
			RestoreValue firstPersonPass{ llf.localShadowFirstPersonPass };
			RestoreValue skinWetness{ globals::features::skin.currentWetness };

			CaptureState capture{ a_request };
			struct ActiveScope
			{
				explicit ActiveScope(CaptureState& a_capture) { active.store(&a_capture); }
				~ActiveScope() { active.store(nullptr); }
			} activeScope(capture);
			state.inWorld = false;  // Do not start the deferred pipeline or queue Terrain Blending geometry.
			globals::deferred->deferredPass = false;
			llf.localShadowFirstPersonPass = false;
			state.permutationData.ExtraShaderDescriptor = uint32_t(State::ExtraShaderDescriptors::InWorld);
			graphics->projectionPosScaleX = graphics->projectionPosScaleY = 0;
			graphics->screenWidth = graphics->screenHeight = size;
			graphics->GetRuntimeData().dynamicResolutionWidthRatio = graphics->GetRuntimeData().dynamicResolutionHeightRatio = 1;
			*globals::game::cameraNear = a_request.nearDistance;
			*globals::game::cameraFar = a_request.farDistance;
			renderer.renderTargets[targetSlot] = {};
			renderer.renderTargets[targetSlot].RTV = a_request.radiance;
			depthTargets[depthSlot] = {};
			depthTargets[depthSlot].texture = resources.depth.get();
			depthTargets[depthSlot].views[0] = resources.dsv.get();

			const auto worldEye = frame.saved.GetCameraPosAdjust();
			CaptureData data{};
			data.faceMask = a_request.faces;
			data.textureSize = size;
			data.nearDistance = a_request.nearDistance;
			data.farDistance = a_request.farDistance;
			data.lightCount = llf.loaded ? llf.lightCount : 0;
			data.worldOriginDelta = float3{ a_request.position.x - worldEye.x, a_request.position.y - worldEye.y, a_request.position.z - worldEye.z };
			const auto lightDelta = a_request.position - llf.eyePositionCached;
			data.lightOriginDelta = { lightDelta.x, lightDelta.y, lightDelta.z };
			resources.captureCB->Update(data);
			Clear(a_request);
			// The clear pass changed raw D3D state. Rebind the original feature resources and
			// mark engine state dirty before drawing any material.
			pipeline.bindings.Restore(globals::d3d::context);
			auto& shadowData = globals::game::shadowState->GetRuntimeData();
			shadowData.stateUpdateFlags = static_cast<RE::BSGraphics::ShaderFlags>(UINT_MAX);
			shadowData.PSResourceModifiedBits = shadowData.PSSamplerModifiedBits = 0xFFFF;
			if (llf.loaded && llf.lights) {
				auto* lights = llf.lights->srv.get();
				globals::d3d::context->PSSetShaderResources(35, 1, &lights);
				auto* strict = llf.strictLightDataCB->CB();
				globals::d3d::context->PSSetConstantBuffers(3, 1, &strict);
			}

			// Reuse the engine's cull -> accumulate -> opaque/alpha/decal Forward dispatch.
			// These are the same SE/AE functions used by BSCubeMapCamera, without its cameras,
			// cubemap render targets, reflection flags, or face loop.
			static REL::Relocation<void(RE::CullJobDescriptor*, uint32_t, uint32_t)> cull{ REL::RelocationID(100213, 106921) };
			for (uint32_t face = 0; face < 8; ++face) {
				if (!(a_request.faces & (1u << face)))
					continue;
				capture.camera = resources.Camera(face, a_request);
				data.faceSign = { face & 1 ? -1.f : 1.f, face & 2 ? -1.f : 1.f, face & 4 ? -1.f : 1.f };
				resources.captureCB->Update(data);
				auto* draw = resources.accumulator.get();
				auto* runtime = draw->GetRuntimeData();
				runtime->activeShadowSceneNode = scene;
				runtime->renderMode = RE::BSShaderAccumulator::RENDER_MODE::kNormal;
				runtime->eyePosition = a_request.position;
				runtime->unkB8 = true;
				runtime->unk178[0] = 0;
				draw->StartAccumulating(capture.camera);
				RE::CullJobDescriptor job{};
				job.shaderAccumulator = resources.accumulator;
				job.camera.reset(capture.camera);
				job.frustum = &capture.camera->GetRuntimeData2().viewFrustum;
				job.scene = scene;
				job.cullMode = 3;  // Ignore the main view's portal visibility.
				job.unk58 = UINT_MAX;
				job.ignorePreprocess = true;
				job.cameraRelatedUpdates = false;
				job.updateAccumulateFlag = false;
				cull(&job, 0, 0);
				graphics->SetCameraData(capture.camera, 8);
				shadowData.depthStencilDepthMode = RE::BSGraphics::DepthStencilDepthMode::kTestWrite;
				shadowData.stateUpdateFlags.set(RE::BSGraphics::DIRTY_DEPTH_MODE);
				BeforeDraw();
				*globals::game::currentAccumulator = reinterpret_cast<RE::BSGraphics::BSShaderAccumulator*>(draw);
				auto shared = state.sharedDataSnapshot;
				shared.CameraData = { a_request.farDistance, a_request.nearDistance, a_request.farDistance - a_request.nearDistance, a_request.farDistance * a_request.nearDistance };
				shared.BufferDim = { float(size), float(size), 1.f / size, 1.f / size };
				shared.MipBias = 0;
				shared.WaterSystemHeight -= data.worldOriginDelta.z;
				for (int x = -2; x <= 2; ++x)
					for (int y = -2; y <= 2; ++y)
						shared.WaterData[(x + 2) + (y + 2) * 5] = Util::TryGetWaterData(float(x) * 4096, float(y) * 4096);
				resources.sharedCB->Update(shared);
				draw->FinishAccumulatingPreResolveDepth(8);
				draw->FinishAccumulatingPostResolveDepth(8);
			}
			return capture.shaderFailed ? Result::ShaderUnavailable : Result::Success;
		} catch (const std::exception& error) {
			resources.accumulator.reset();
			logger::error("Community Shaders environment probe: {}", error.what());
			return Result::Unavailable;
		}
	}
}
