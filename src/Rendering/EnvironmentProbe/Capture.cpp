#include "Rendering/EnvironmentProbe/Capture.h"
#include "Rendering/EnvironmentProbe/Geometry.h"
#include "Rendering/EnvironmentProbe/Scene.h"
#include "Rendering/EnvironmentProbe/Shadows.h"

#include "Deferred.h"
#include "Features/Effects11/D3D11StateBackup.h"
#include "Features/LightLimitFix.h"
#include "Features/TerrainBlending.h"
#include "Globals.h"
#include "Profiler.h"
#include "ShaderCache.h"
#include "State.h"
#include "Util.h"

#include <atomic>
#include <bit>
#include <cmath>
#include <deque>

namespace EnvironmentProbe
{
	namespace
	{
		std::atomic_bool invalidateShaders{ false };
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
			uint32_t cullBackface = 0;
			float3 lightOriginDelta;
			uint32_t receiveSunShadows = 1;
			DirectX::XMFLOAT4X4 sunProjection[3]{};
			float4 sunOrigin{};
			float4 sunViewDepth{};
			float sunEndSplits[3]{};
			uint32_t sunCascadeCount = 0;
			float sunStartSplits[4]{};
			Shadows::Parameters coarseSun{};
		};
		static_assert(sizeof(CaptureData) == 400);

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
			GeometryShaders geometryShaders;
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
			template <class T>
			struct ReversedState
			{
				winrt::com_ptr<T> original;
				winrt::com_ptr<T> reversed;
			};
			std::vector<ReversedState<ID3D11DepthStencilState>> depthStates;
			std::vector<ReversedState<ID3D11RasterizerState>> rasterStates;
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
					Util::SetResourceName(depth.get(), "EnvironmentProbe::Reversed Projective Z");
					D3D11_DEPTH_STENCIL_VIEW_DESC depthView{};
					depthView.Format = DXGI_FORMAT_D32_FLOAT;
					depthView.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
					DX::ThrowIfFailed(device->CreateDepthStencilView(depth.get(), &depthView, dsv.put()));
					Util::SetResourceName(dsv.get(), "EnvironmentProbe::Reversed Projective Z DSV");
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
				// Supplies the engine's camera constants. Custom cull planes below replace
				// this rectangular frustum for scene traversal.
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
			bool shadersPending = false;
			uint32_t draws = 0;
			uint32_t twoSided = 0;
			uint32_t depthWrites = 0;
			struct GeometryWork
			{
				uint32_t triangles;
				uint32_t draws = 0;
				uint32_t faces = 0;
			};
			std::unordered_map<RE::BSGeometry*, GeometryWork> geometryWork;
			CaptureData* data = nullptr;
			uint32_t geometryMask = 0;
			uint32_t cullBackface = 0;
			uint32_t reusedPasses = 0;
			uint32_t nativeDirty = UINT32_MAX;
			bool bindingsReady = false;
			bool writesDepth = true;
			uint32_t receiveSunShadows = 1;
			ID3D11ShaderResourceView* sunShadowMap = nullptr;
			ID3D11Buffer* sunShadowParameters = nullptr;
			RE::BSLightingShader* lightingShader = nullptr;
			uint32_t lightingTechnique = 0;
			struct MaterialValues
			{
				float alpha;
				float specularLODFade;
				float envmapLODFade;
			};
			std::unordered_map<RE::BSLightingShaderProperty*, MaterialValues> materialValues;
			std::deque<RE::BSShaderProperty::RenderPassArray> generatedPasses;
			~CaptureState()
			{
				if (lightingShader)
					lightingShader->currentRawTechnique = lightingTechnique;
				for (const auto& [property, values] : materialValues) {
					property->alpha = values.alpha;
					property->specularLODFade = values.specularLODFade;
					property->envmapLODFade = values.envmapLODFade;
				}
				// Copied probe passes borrow their light arrays from these native nodes.
				// ActiveScope has already released the accumulator's batch lists.
				for (auto& generated : generatedPasses)
					generated.Clear();
			}
			ID3D11GeometryShader* boundGeometryShader = nullptr;
			winrt::com_ptr<ID3D11BlendState> nativeBlend;
			float blendFactor[4]{};
			UINT sampleMask = UINT_MAX;
			std::array<RE::NiFrustumPlanes, 8> facePlanes;
			std::unordered_map<RE::BSGeometry*, uint32_t> geometryFaces;
			std::deque<RE::BSRenderPass> passes;
			RE::BSShaderProperty::RenderPassArray passArray{};
		};
		std::atomic<CaptureState*> active = nullptr;
		Resources resources;

		RE::NiFrustumPlanes CullPlanes(const Request& a_request, const float3& a_sign, uint32_t a_size)
		{
			RE::NiFrustumPlanes result{};
			const RE::NiPoint3 sign{ a_sign.x, a_sign.y, a_sign.z };
			for (uint32_t axis = 0; axis < 3; ++axis) {
				// Match the VS's one-texel seam overlap. Ni planes use dot(n, p) - c >= 0.
				auto normal = sign / float(std::max(a_size, 4u));
				normal[axis] += sign[axis];
				normal.Unitize();
				result.cullingPlanes[axis] = { normal, normal.Dot(a_request.position) };
				// Custom planes replace the camera frustum, including its far plane.
				// Bound the octant by a box containing the radial capture sphere.
				RE::NiPoint3 farNormal{};
				farNormal[axis] = -sign[axis];
				result.cullingPlanes[axis + 3] = { farNormal, farNormal.Dot(a_request.position) - a_request.farDistance };
			}
			result.activePlanes = static_cast<RE::NiFrustumPlanes::ActivePlane>(0x3F);
			result.basePlaneStates = 0x3F;
			return result;
		}

		struct LightingPasses
		{
			static RE::BSShaderProperty::RenderPassArray* thunk(RE::BSLightingShaderProperty* property, RE::BSGeometry* geometry, uint32_t flags, RE::BSShaderAccumulator* accumulator)
			{
				auto* capture = active.load();
				if (!capture || accumulator != resources.accumulator.get()) {
					auto* passes = func(property, geometry, flags, accumulator);
					Scene::RememberPasses(property, geometry, flags, accumulator, passes);
					return passes;
				}
				const auto first = capture->passes.size();
				if (Scene::CopyPasses(property, geometry, flags, capture->passes)) {
					++capture->reusedPasses;
				} else {
					// GetRenderPasses writes the cache key also used by GetRenderPasses_ShadowMapOrMask.
					// Publishing a probe build there makes the next world cull reuse stale CSM masks.
					// Its native pass nodes may also still belong to the unfinished world accumulator.
					RestoreValue passList{ property->renderPassList };
					RestoreValue passState{ property->lastRenderPassState };
					RestoreValue lightChanged{ property->lightData->lightListChanged };
					capture->materialValues.try_emplace(property, CaptureState::MaterialValues{
																	  property->alpha, property->specularLODFade, property->envmapLODFade });
					property->renderPassList = {};
					property->lastRenderPassState = INT32_MAX;
					auto& generated = capture->generatedPasses.emplace_back();
					RestoreValue materialEye{ accumulator->GetRuntimeData()->eyePosition };
					accumulator->GetRuntimeData()->eyePosition = RE::Main::WorldRootCamera()->world.translate;
					func(property, geometry, flags, accumulator);
					generated = property->renderPassList;
					for (auto* pass = generated.head; pass; pass = pass->next)
						if (pass->shader->shaderType.get() == RE::BSShader::Type::Lighting)
							capture->passes.push_back(*pass);
				}

				// Cached passes also belong to the unfinished world view. Registering them again
				// rewrites passGroupNext, creating cycles (notably in the refraction mask).
				// Give the native accumulator private Lighting nodes, excluding Utility passes
				// before registration rather than merely rejecting their shader at draw time.
				capture->passArray.head = nullptr;
				auto** tail = &capture->passArray.head;
				for (auto i = first; i < capture->passes.size(); ++i) {
					auto& copy = capture->passes[i];
					copy.next = copy.passGroupNext = nullptr;
					*tail = &copy;
					tail = &copy.next;
				}
				return &capture->passArray;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

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
				if (Shadows::RejectGeometry(a_process, a_geometry))
					return;
				auto* capture = active.load();
				if (capture && a_process->camera == capture->camera) {
					const auto& request = capture->request;
					const auto& bound = a_geometry.worldBound;
					const float distance = (bound.center - request.position).Length();
					if (distance - bound.radius > request.farDistance || distance + bound.radius < request.nearDistance ||
						!IncludeGeometry(a_geometry, request.cullDynamic))
						return;
					uint32_t mask = 0;
					for (uint32_t face = 0; face < 8; ++face) {
						if (!(request.faces & (1u << face)))
							continue;
						bool visible = true;
						if (!a_geometry.GetFlags().any(RE::NiAVObject::Flag::kAlwaysDraw)) {
							for (uint32_t axis = 0; axis < 3; ++axis) {
								const auto& plane = capture->facePlanes[face].cullingPlanes[axis];
								if (plane.normal.Dot(bound.center) - plane.constant < -bound.radius)
									visible = false;
							}
						}
						if (visible)
							mask |= 1u << face;
					}
					if (!mask || !capture->geometryFaces.try_emplace(&a_geometry, mask).second)
						return;
				}
				func(a_process, a_geometry, a_group);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct SetupTechnique
		{
			static bool thunk(RE::BSLightingShader* shader, uint32_t technique)
			{
				if (IsRendering()) {
					auto& capture = *active.load();
					if (!capture.lightingShader) {
						capture.lightingShader = shader;
						capture.lightingTechnique = shader->currentRawTechnique;
					}
				}
				return func(shader, technique);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct SetupGeometry
		{
			static void thunk(RE::BSShader* shader, RE::BSRenderPass* pass, uint32_t flags)
			{
				func(shader, pass, flags);
				if (!IsRendering())
					return;
				auto& capture = *active.load();
				capture.receiveSunShadows = pass->shaderProperty->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kReceiveShadows);
				capture.geometryMask = capture.geometryFaces.at(pass->geometry);
				const bool twoSided = pass->shaderProperty->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kTwoSided);
				// Capture flag 8 suppresses the native batcher's material cull-mode selection.
				// Restore it explicitly rather than inheriting the last main-view material.
				auto& state = globals::game::shadowState->GetRuntimeData();
				const uint32_t mode = twoSided ? 0 : RE::BSGraphics::RasterStateCullMode::RASTER_STATE_CULL_MODE_BACK;
				if (state.rasterStateCullMode != mode) {
					state.rasterStateCullMode = mode;
					state.stateUpdateFlags.set(RE::BSGraphics::DIRTY_RASTER_CULL_MODE);
				}
				++capture.draws;
				capture.twoSided += twoSided;
				if (globals::state->IsDeveloperMode() && globals::state->frameCount % 127 == 0) {
					if (auto* shape = pass->geometry->AsTriShape()) {
						auto& work = capture.geometryWork.try_emplace(pass->geometry, CaptureState::GeometryWork{ shape->GetTrishapeRuntimeData().triangleCount }).first->second;
						++work.draws;
						work.faces |= capture.geometryMask;
					}
				}
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct GroupAlphas
		{
			static RE::BSBatchRenderer::GeometryGroup* thunk(RE::BSShaderAccumulator* accumulator, RE::NiBound* bound)
			{
				auto* group = func(accumulator, bound);
				if (group && IsRendering() && accumulator == resources.accumulator.get()) {
					// A single camera-forward depth cannot order the back hemisphere.
					group->depth = (bound->center - active.load()->request.position).Length();
					if (accumulator->sortByClosestPoint)
						group->depth -= bound->radius;
				}
				return group;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct SavedPipeline
		{
			Effects11Util::D3D11FullStateBackup bindings;
			winrt::com_ptr<ID3D11GeometryShader> geometryShader;
			winrt::com_ptr<ID3D11Buffer> geometryConstants;
			std::vector<std::pair<ID3D11Buffer*, ID3D11Buffer*>> copies;

			SavedPipeline()
			{
				auto* context = globals::d3d::context;
				bindings.Save(context);
				context->GSGetShader(geometryShader.put(), nullptr, nullptr);
				context->GSGetConstantBuffers(8, 1, geometryConstants.put());
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
			}

			~SavedPipeline()
			{
				auto* context = globals::d3d::context;
				context->OMSetRenderTargets(0, nullptr, nullptr);
				for (auto [original, backup] : copies)
					context->CopyResource(original, backup);
				bindings.Restore(context);
				context->GSSetShader(geometryShader.get(), nullptr, 0);
				auto* buffer = geometryConstants.get();
				context->GSSetConstantBuffers(8, 1, &buffer);
			}
		};

		bool ValidateTarget(ID3D11RenderTargetView* a_view, DXGI_FORMAT a_format, uint32_t& a_size, ID3D11Resource*& a_resource)
		{
			if (!a_view)
				return true;
			D3D11_RENDER_TARGET_VIEW_DESC view{};
			a_view->GetDesc(&view);
			if (view.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2D ||
				(a_format != DXGI_FORMAT_UNKNOWN ? view.Format != a_format :
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
			context->ClearDepthStencilView(resources.dsv.get(), D3D11_CLEAR_DEPTH, 0, 0);
			ID3D11RenderTargetView* targets[] = { a_request.radiance, a_request.depth, a_request.normals };
			context->OMSetRenderTargets(3, targets, nullptr);
			context->OMSetBlendState(resources.clearBlend.get(), nullptr, UINT_MAX);
			context->OMSetDepthStencilState(resources.clearDepth.get(), 0);
			context->RSSetState(resources.clearRaster.get());
			D3D11_VIEWPORT viewport{ 0, 0, float(resources.size), float(resources.size), 0, 1 };
			context->RSSetViewports(1, &viewport);
			context->IASetInputLayout(nullptr);
			context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			context->GSSetShader(nullptr, nullptr, 0);
			context->VSSetShader(resources.clearVS.get(), nullptr, 0);
			context->PSSetShader(resources.clearPS.get(), nullptr, 0);
			auto* capture = resources.captureCB->CB();
			context->PSSetConstantBuffers(8, 1, &capture);
			context->Draw(3, 0);
		}
	}

	void InstallHooks()
	{
		Scene::InstallHooks();
		Shadows::InstallHooks();
		stl::write_vfunc<0x18, AppendGeometry>(RE::VTABLE_BSCullingProcess[0]);
		stl::write_vfunc<0x2A, LightingPasses>(RE::VTABLE_BSLightingShaderProperty[0]);
		stl::write_vfunc<0x2, SetupTechnique>(RE::VTABLE_BSLightingShader[0]);
		stl::write_vfunc<0x6, SetupGeometry>(RE::VTABLE_BSLightingShader[0]);
		stl::write_vfunc<0x28, GroupAlphas>(RE::VTABLE_BSShaderAccumulator[0]);
	}

	void ClearShaderCache() { invalidateShaders.store(true); }

	bool IsRendering()
	{
		const auto* capture = active.load();
		return capture && capture->thread == GetCurrentThreadId();
	}

	bool UsesAsyncShaders()
	{
		return IsRendering() && active.load()->request.asyncShaders;
	}

	void ShaderUnavailable(bool a_pending)
	{
		if (IsRendering()) {
			if (a_pending)
				active.load()->shadersPending = true;
			else
				active.load()->shaderFailed = true;
		}
	}

	void BeforeDraw()
	{
		Shadows::BeforeDraw();
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
		active.load()->nativeDirty |= state.stateUpdateFlags.underlying();
	}

	void AfterDraw()
	{
		Shadows::AfterDraw();
		if (!IsRendering())
			return;
		auto& state = *active.load();
		const auto& request = state.request;
		auto* context = globals::d3d::context;
		const uint32_t mask = state.geometryMask;
		ID3D11GeometryShader* geometryShader = nullptr;
		if (std::popcount(mask) > 1) {
			auto* vertex = globals::state->customVertexShader;
			if (!vertex)
				throw std::runtime_error("Environment probe requires its Lighting VS");
			geometryShader = resources.geometryShaders.Get(*vertex);
		}
		if (!state.bindingsReady || state.boundGeometryShader != geometryShader) {
			context->GSSetShader(geometryShader, nullptr, 0);
			state.boundGeometryShader = geometryShader;
		}
		if (state.nativeDirty & RE::BSGraphics::DIRTY_RENDERTARGET) {
			ID3D11RenderTargetView* targets[] = { request.radiance, request.depth, request.normals };
			context->OMSetRenderTargets(3, targets, resources.dsv.get());
			D3D11_RECT scissor{ 0, 0, LONG(resources.size), LONG(resources.size) };
			context->RSSetScissorRects(1, &scissor);
		}
		if (!state.bindingsReady) {
			auto* capture = resources.captureCB->CB();
			auto* shared = resources.sharedCB->CB();
			context->GSSetConstantBuffers(8, 1, &capture);
			context->VSSetConstantBuffers(8, 1, &capture);
			context->PSSetConstantBuffers(8, 1, &capture);
			context->VSSetConstantBuffers(5, 1, &shared);
			context->PSSetConstantBuffers(5, 1, &shared);
			context->PSSetShaderResources(104, 1, &state.sunShadowMap);
			auto* coarseMap = Shadows::Map();
			context->PSSetShaderResources(105, 1, &coarseMap);
			context->PSSetConstantBuffers(13, 1, &state.sunShadowParameters);
			auto& llf = globals::features::lightLimitFix;
			if (llf.loaded && llf.probeLightMasks) {
				auto* masks = llf.probeLightMasks->srv.get();
				context->PSSetShaderResources(36, 1, &masks);
			}
			state.bindingsReady = true;
		}

		// Keep material alpha blending on radiance, but never blend or overwrite the radial
		// depth output in a transparent pass that does not write the hardware depth buffer.
		const bool depthChanged = (state.nativeDirty & (RE::BSGraphics::DIRTY_DEPTH_MODE | RE::BSGraphics::DIRTY_DEPTH_STENCILREF_MODE)) != 0;
		if (depthChanged) {
			winrt::com_ptr<ID3D11DepthStencilState> depth;
			UINT reference;
			context->OMGetDepthStencilState(depth.put(), &reference);
			CD3D11_DEPTH_STENCIL_DESC depthDesc(D3D11_DEFAULT);
			if (depth)
				depth->GetDesc(&depthDesc);
			state.writesDepth = !depth || (depthDesc.DepthEnable && depthDesc.DepthWriteMask == D3D11_DEPTH_WRITE_MASK_ALL);
			auto depthIt = std::ranges::find_if(resources.depthStates, [&](const auto& entry) {
				return entry.original == depth;
			});
			if (depthIt == resources.depthStates.end()) {
				Resources::ReversedState<ID3D11DepthStencilState> entry{ depth };
				switch (depthDesc.DepthFunc) {
				case D3D11_COMPARISON_LESS:
					depthDesc.DepthFunc = D3D11_COMPARISON_GREATER;
					break;
				case D3D11_COMPARISON_LESS_EQUAL:
					depthDesc.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;
					break;
				case D3D11_COMPARISON_GREATER:
					depthDesc.DepthFunc = D3D11_COMPARISON_LESS;
					break;
				case D3D11_COMPARISON_GREATER_EQUAL:
					depthDesc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
					break;
				default:
					break;
				}
				DX::ThrowIfFailed(globals::d3d::device->CreateDepthStencilState(&depthDesc, entry.reversed.put()));
				Util::SetResourceName(entry.reversed.get(), "EnvironmentProbe::Reversed Depth State");
				resources.depthStates.push_back(std::move(entry));
				depthIt = std::prev(resources.depthStates.end());
			}
			context->OMSetDepthStencilState(depthIt->reversed.get(), reference);
		}
		state.depthWrites += state.writesDepth;

		// Native decal bias must move in the opposite direction under reversed Z.
		if (state.nativeDirty & (RE::BSGraphics::DIRTY_RASTER_CULL_MODE | RE::BSGraphics::DIRTY_RASTER_DEPTH_BIAS | RE::BSGraphics::DIRTY_UNKNOWN1)) {
			winrt::com_ptr<ID3D11RasterizerState> raster;
			context->RSGetState(raster.put());
			state.cullBackface = 0;
			if (raster) {
				D3D11_RASTERIZER_DESC desc;
				raster->GetDesc(&desc);
				if (desc.CullMode != D3D11_CULL_NONE)
					state.cullBackface = (desc.CullMode == D3D11_CULL_BACK) != bool(desc.FrontCounterClockwise) ? 1u : 2u;
				if (desc.DepthBias != 0 || desc.SlopeScaledDepthBias != 0) {
					auto rasterIt = std::ranges::find_if(resources.rasterStates, [&](const auto& entry) {
						return entry.original == raster;
					});
					if (rasterIt == resources.rasterStates.end()) {
						Resources::ReversedState<ID3D11RasterizerState> entry{ raster };
						desc.DepthBias = -desc.DepthBias;
						desc.SlopeScaledDepthBias = -desc.SlopeScaledDepthBias;
						desc.DepthBiasClamp = -desc.DepthBiasClamp;
						DX::ThrowIfFailed(globals::d3d::device->CreateRasterizerState(&desc, entry.reversed.put()));
						Util::SetResourceName(entry.reversed.get(), "EnvironmentProbe::Reversed Depth Bias");
						resources.rasterStates.push_back(std::move(entry));
						rasterIt = std::prev(resources.rasterStates.end());
					}
					context->RSSetState(rasterIt->reversed.get());
				}
			}
		}
		if (state.nativeDirty & RE::BSGraphics::DIRTY_ALPHA_BLEND) {
			state.nativeBlend = nullptr;
			context->OMGetBlendState(state.nativeBlend.put(), state.blendFactor, &state.sampleMask);
		}
		if (depthChanged || (state.nativeDirty & RE::BSGraphics::DIRTY_ALPHA_BLEND)) {
			const auto& blend = state.nativeBlend;
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
				desc.RenderTarget[2] = desc.RenderTarget[1];
				desc.RenderTarget[2].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN;
				DX::ThrowIfFailed(globals::d3d::device->CreateBlendState(&desc, entry.withDepth.put()));
				Util::SetResourceName(entry.withDepth.get(), "EnvironmentProbe::Radiance and Depth Blend");
				desc.RenderTarget[1].RenderTargetWriteMask = 0;
				desc.RenderTarget[2].RenderTargetWriteMask = 0;
				DX::ThrowIfFailed(globals::d3d::device->CreateBlendState(&desc, entry.withoutDepth.put()));
				Util::SetResourceName(entry.withoutDepth.get(), "EnvironmentProbe::Radiance Blend");
				resources.blends.push_back(std::move(entry));
				it = std::prev(resources.blends.end());
			}
			context->OMSetBlendState(state.writesDepth ? it->withDepth.get() : it->withoutDepth.get(), state.blendFactor, state.sampleMask);
		}
		if (state.data->faceMask != mask || state.data->cullBackface != state.cullBackface || state.data->receiveSunShadows != state.receiveSunShadows) {
			state.data->faceMask = mask;
			state.data->cullBackface = state.cullBackface;
			state.data->receiveSunShadows = state.receiveSunShadows;
			const auto face = std::countr_zero(mask);
			state.data->faceSign = { face & 1 ? -1.f : 1.f, face & 2 ? -1.f : 1.f, face & 4 ? -1.f : 1.f };
			resources.captureCB->Update(*state.data);
		}
		state.nativeDirty = 0;
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
		ID3D11Resource* normalResource = nullptr;
		if ((!a_request.radiance && !a_request.depth && !a_request.normals) || (a_request.faces & ~0xFFu) ||
			!std::isfinite(a_request.position.x) || !std::isfinite(a_request.position.y) || !std::isfinite(a_request.position.z) ||
			!std::isfinite(a_request.nearDistance) || !std::isfinite(a_request.farDistance) ||
			a_request.nearDistance <= 0 || a_request.farDistance <= a_request.nearDistance ||
			!ValidateTarget(a_request.radiance, DXGI_FORMAT_UNKNOWN, size, radianceResource) ||
			!ValidateTarget(a_request.depth, DXGI_FORMAT_R32_FLOAT, size, depthResource) ||
			!ValidateTarget(a_request.normals, DXGI_FORMAT_R16G16_UNORM, size, normalResource) ||
			(radianceResource && (radianceResource == depthResource || radianceResource == normalResource)) ||
			(depthResource && depthResource == normalResource))
			return Result::InvalidRequest;
		if (!a_request.faces)
			return Result::Success;
		auto* scene = globals::game::smState->shadowSceneNode[0];
		const auto* worldCamera = RE::Main::WorldRootCamera();
		if (!scene || !worldCamera || globals::features::terrainBlending.renderDepth)
			return Result::Unavailable;

		try {
			if (invalidateShaders.exchange(false)) {
				resources.geometryShaders.Clear();
				resources.clearVS = nullptr;
				resources.clearPS = nullptr;
			}
			// Sequential phases: Profiler does not support nested passes. Keep this guard
			// outside the pipeline guards so Restore includes all state restoration.
			struct Timing
			{
				Timing() { globals::profiler->BeginPass("EnvironmentProbe::Setup"); }
				~Timing() { globals::profiler->EndPass(); }
				void Next(const char* name)
				{
					globals::profiler->EndPass();
					globals::profiler->BeginPass(name);
				}
			} timing;
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

			state.inWorld = false;
			globals::deferred->deferredPass = false;
			graphics->projectionPosScaleX = graphics->projectionPosScaleY = 0;
			if (a_request.supplementalSunShadows) {
				timing.Next("EnvironmentProbe::CoarseSunShadow");
				Shadows::Render(a_request);
				timing.Next("EnvironmentProbe::Setup");
			} else {
				Shadows::Reset();
			}
			Scene::RequestCapture();
			CaptureState capture{ a_request };
			struct ActiveScope
			{
				explicit ActiveScope(CaptureState& a_capture) { active.store(&a_capture); }
				~ActiveScope()
				{
					// Drop all native batch lists before their borrowed private pass nodes die.
					resources.accumulator.reset();
					active.store(nullptr);
				}
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

			// Framebuffer constants may already describe an image-space camera here.
			// Cached world material features still use the world camera's origin.
			const auto worldEye = worldCamera->world.translate;
			CaptureData data{};
			capture.data = &data;
			data.faceMask = a_request.faces;
			const auto initialFace = std::countr_zero(a_request.faces);
			data.faceSign = { initialFace & 1 ? -1.f : 1.f, initialFace & 2 ? -1.f : 1.f, initialFace & 4 ? -1.f : 1.f };
			data.textureSize = size;
			data.coarseSun = Shadows::Data();
			data.nearDistance = a_request.nearDistance;
			data.farDistance = a_request.farDistance;
			data.lightCount = llf.loaded ? llf.lightCount : 0;
			data.worldOriginDelta = float3{ a_request.position.x - worldEye.x, a_request.position.y - worldEye.y, a_request.position.z - worldEye.z };
			const auto lightDelta = a_request.position - llf.eyePositionCached;
			data.lightOriginDelta = { lightDelta.x, lightDelta.y, lightDelta.z };
			capture.sunShadowMap = globals::deferred->GetDirectionalShadowMap();
			capture.sunShadowParameters = globals::deferred->GetDirectionalShadowParameters();
			if (capture.sunShadowMap && capture.sunShadowParameters) {
				const auto* sun = scene->GetRuntimeData().sunShadowDirLight;
				const auto& sunRuntime = sun->GetRuntimeData();
				const auto& directional = sun->GetShadowDirectionalLightRuntimeData();
				data.sunCascadeCount = std::min(sunRuntime.shadowmapDescriptors.size(), 3u);
				for (uint32_t cascade = 0; cascade < data.sunCascadeCount; ++cascade) {
					memcpy(&data.sunProjection[cascade], &sunRuntime.shadowmapDescriptors[cascade].lightTransform, sizeof(DirectX::XMFLOAT4X4));
					data.sunEndSplits[cascade] = directional.endSplitDistances[cascade];
					data.sunStartSplits[cascade] = directional.startSplitDistances[cascade];
				}
				const auto& rotation = worldCamera->world.rotate;
				const float3 forward{ rotation.entry[0][0], rotation.entry[1][0], rotation.entry[2][0] };
				data.sunOrigin = { a_request.position.x, a_request.position.y, a_request.position.z, 0 };
				data.sunViewDepth = { forward.x, forward.y, forward.z, forward.x * data.worldOriginDelta.x + forward.y * data.worldOriginDelta.y + forward.z * data.worldOriginDelta.z };
			}
			resources.captureCB->Update(data);
			Clear(a_request);
			if (llf.loaded && llf.lights) {
				timing.Next("EnvironmentProbe::LightCull");
				if (!llf.BuildProbeLightGrid(resources.captureCB->CB()))
					return Result::ShaderUnavailable;
			}
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
			for (uint32_t i = 0; i < 8; ++i)
				capture.facePlanes[i] = CullPlanes(a_request, { i & 1 ? -1.f : 1.f, i & 2 ? -1.f : 1.f, i & 4 ? -1.f : 1.f }, size);
			const uint32_t face = std::countr_zero(a_request.faces);
			timing.Next("EnvironmentProbe::Cull");
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
			auto planes = capture.facePlanes[face];
			if (std::popcount(a_request.faces) > 1) {
				for (uint32_t axis = 0; axis < 3; ++axis) {
					RE::NiPoint3 normal{};
					normal[axis] = 1;
					planes.cullingPlanes[axis] = { normal, a_request.position[axis] - a_request.farDistance };
					planes.cullingPlanes[axis + 3] = { -normal, -a_request.position[axis] - a_request.farDistance };
				}
			}
			job.customCullPlanes = &planes;
			job.doCustomCullPlanes = true;
			job.scene = scene;
			job.cullMode = 3;  // Ignore the main view's portal visibility.
			job.unk58 = UINT_MAX;
			job.ignorePreprocess = true;
			// BSFadeNode::OnVisible only updates distance/LOD state when this is
			// true. Keep the main world's policy even for unvisited probe subtrees;
			// the custom planes and per-geometry octant tests use the probe origin.
			job.cameraRelatedUpdates = false;
			job.updateAccumulateFlag = false;
			auto roots = Scene::Roots(scene);
			job.cullingObjects = &roots;
			job.unk61 = true;  // Test hidden flags on reused roots, as NiAVObject::Cull does.
			cull(&job, roots.empty() ? 0 : 1, 0);
			timing.Next("EnvironmentProbe::Draw");
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
			if (state.IsDeveloperMode() && state.frameCount % 127 == 0)
				logger::info("[EnvironmentProbeReuse] roots={} geometry={} materials={} draws={}", roots.size(), capture.geometryFaces.size(), capture.reusedPasses, capture.draws);

			if (!capture.geometryWork.empty()) {
				uint64_t uniqueTriangles = 0, submittedTriangles = 0;
				std::vector<std::pair<RE::BSGeometry*, CaptureState::GeometryWork>> ranked(capture.geometryWork.begin(), capture.geometryWork.end());
				for (const auto& [geometry, work] : ranked) {
					uniqueTriangles += work.triangles;
					submittedTriangles += uint64_t(work.triangles) * work.draws;
				}
				std::ranges::sort(ranked, [](const auto& a, const auto& b) { return uint64_t(a.second.triangles) * a.second.draws > uint64_t(b.second.triangles) * b.second.draws; });
				logger::info("[EnvironmentProbeGeometry] unique={} triangles={} submitted={}", ranked.size(), uniqueTriangles, submittedTriangles);
				for (uint32_t i = 0; i < std::min<size_t>(10, ranked.size()); ++i) {
					const auto& [geometry, work] = ranked[i];
					logger::info("[EnvironmentProbeMesh] name={} triangles={} draws={} faces={:02x}", geometry->name.c_str(), work.triangles, work.draws, work.faces);
				}
			}
			timing.Next("EnvironmentProbe::Restore");
			return capture.shaderFailed ? Result::ShaderUnavailable : capture.shadersPending ? Result::ShadersPending :
			                                                                                   Result::Success;
		} catch (const std::exception& error) {
			resources.accumulator.reset();
			logger::error("Community Shaders environment probe: {}", error.what());
			return Result::Unavailable;
		}
	}
}
