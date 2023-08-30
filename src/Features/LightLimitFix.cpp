#include "LightLimitFix.h"

#include <numbers>

#include <PerlinNoise.hpp>

#include "State.h"
#include "Util.h"

constexpr std::uint32_t CLUSTER_SIZE_X = 32;
constexpr std::uint32_t CLUSTER_SIZE_Y = 16;
constexpr std::uint32_t CLUSTER_SIZE_Z = 16;
constexpr std::uint32_t CLUSTER_MAX_LIGHTS = 1024;

constexpr std::uint32_t CLUSTER_COUNT = CLUSTER_SIZE_X * CLUSTER_SIZE_Y * CLUSTER_SIZE_Z;

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	LightLimitFix::Settings,
	EnableFirstPersonShadows,
	EnableParticleLights,
	EnableParticleLightsShadows,
	EnableParticleLightsCulling,
	EnableParticleLightsDetection,
	ParticleLightsBrightness,
	ParticleLightsRadius,
	ParticleLightsRadiusBillboards,
	EnableParticleLightsOptimization,
	ParticleLightsOptimisationClusterRadius)

void LightLimitFix::DrawSettings()
{
	if (ImGui::TreeNodeEx("Miscellaneous", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Checkbox("Extend First-Person Shadows", &settings.EnableFirstPersonShadows);

		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx("Particle Lights", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Checkbox("Enable Particle Lights", &settings.EnableParticleLights);
		ImGui::Checkbox("Enable Shadows", &settings.EnableParticleLightsShadows);

		ImGui::Checkbox("Enable Culling", &settings.EnableParticleLightsCulling);
		ImGui::Checkbox("Enable Detection", &settings.EnableParticleLightsDetection);

		ImGui::SliderFloat("Brightness", &settings.ParticleLightsBrightness, 0.0, 1.0, "%.2f");
		ImGui::SliderFloat("Saturation", &settings.ParticleLightsSaturation, 1.0, 2.0, "%.2f");

		ImGui::SliderFloat("Radius", &settings.ParticleLightsRadius, 0.0, 100.0, "%.0f");
		ImGui::SliderFloat("Billboard Radius", &settings.ParticleLightsRadiusBillboards, 0.0, 1.0, "%.2f");

		ImGui::Checkbox("Enable Optimization", &settings.EnableParticleLightsOptimization);
		ImGui::SliderInt("Optimisation Cluster Radius", (int*)&settings.ParticleLightsOptimisationClusterRadius, 1, 48);

		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx("Statistics", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Text(std::format("Clustered Light Count : {}", lightCount).c_str());
		ImGui::Text(std::format("Particle Lights Detection Count : {}", particleLightsDetectionHits).c_str());
		ImGui::Text(std::format("Strict Lights Count : {}", strictLightsCount).c_str());

		ImGui::TreePop();
	}
}

void LightLimitFix::SetupResources()
{
	{
		D3D11_BUFFER_DESC sbDesc{};
		sbDesc.Usage = D3D11_USAGE_DYNAMIC;
		sbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		sbDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		sbDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		sbDesc.StructureByteStride = sizeof(PerPass);
		sbDesc.ByteWidth = sizeof(PerPass);
		perPass = std::make_unique<Buffer>(sbDesc);

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
		srvDesc.Format = DXGI_FORMAT_UNKNOWN;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
		srvDesc.Buffer.FirstElement = 0;
		srvDesc.Buffer.NumElements = 1;
		perPass->CreateSRV(srvDesc);
	}

	{
		D3D11_BUFFER_DESC sbDesc{};
		sbDesc.Usage = D3D11_USAGE_DYNAMIC;
		sbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		sbDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		sbDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		sbDesc.StructureByteStride = sizeof(LightData);
		sbDesc.ByteWidth = sizeof(LightData) * 1024;
		strictLights = eastl::make_unique<Buffer>(sbDesc);

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
		srvDesc.Format = DXGI_FORMAT_UNKNOWN;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
		srvDesc.Buffer.FirstElement = 0;
		srvDesc.Buffer.NumElements = 15;
		strictLights->CreateSRV(srvDesc);
	}

	clusterBuildingCS = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\LightLimitFix\\ClusterBuildingCS.hlsl", {}, "cs_5_0");
	clusterCullingCS = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\LightLimitFix\\ClusterCullingCS.hlsl", {}, "cs_5_0");

	perFrameLightCulling = new ConstantBuffer(ConstantBufferDesc<PerFrameLightCulling>());

	D3D11_BUFFER_DESC sbDesc{};
	sbDesc.Usage = D3D11_USAGE_DEFAULT;
	sbDesc.CPUAccessFlags = 0;
	sbDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
	sbDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;

	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
	srvDesc.Format = DXGI_FORMAT_UNKNOWN;
	srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
	srvDesc.Buffer.FirstElement = 0;

	D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
	uavDesc.Format = DXGI_FORMAT_UNKNOWN;
	uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
	uavDesc.Buffer.FirstElement = 0;
	uavDesc.Buffer.Flags = 0;

	std::uint32_t numElements = CLUSTER_COUNT;

	sbDesc.StructureByteStride = sizeof(ClusterAABB);
	sbDesc.ByteWidth = sizeof(ClusterAABB) * numElements;
	clusters = eastl::make_unique<Buffer>(sbDesc);
	srvDesc.Buffer.NumElements = numElements;
	clusters->CreateSRV(srvDesc);
	uavDesc.Buffer.NumElements = numElements;
	clusters->CreateUAV(uavDesc);

	numElements = 1;
	sbDesc.StructureByteStride = sizeof(uint32_t);
	sbDesc.ByteWidth = sizeof(uint32_t) * numElements;
	lightCounter = eastl::make_unique<Buffer>(sbDesc);
	srvDesc.Buffer.NumElements = numElements;
	lightCounter->CreateSRV(srvDesc);
	uavDesc.Buffer.NumElements = numElements;
	lightCounter->CreateUAV(uavDesc);

	numElements = CLUSTER_COUNT * CLUSTER_MAX_LIGHTS;
	sbDesc.StructureByteStride = sizeof(uint32_t);
	sbDesc.ByteWidth = sizeof(uint32_t) * numElements;
	lightList = eastl::make_unique<Buffer>(sbDesc);
	srvDesc.Buffer.NumElements = numElements;
	lightList->CreateSRV(srvDesc);
	uavDesc.Buffer.NumElements = numElements;
	lightList->CreateUAV(uavDesc);

	numElements = CLUSTER_COUNT;
	sbDesc.StructureByteStride = sizeof(LightGrid);
	sbDesc.ByteWidth = sizeof(LightGrid) * numElements;
	lightGrid = eastl::make_unique<Buffer>(sbDesc);
	srvDesc.Buffer.NumElements = numElements;
	lightGrid->CreateSRV(srvDesc);
	uavDesc.Buffer.NumElements = numElements;
	lightGrid->CreateUAV(uavDesc);
}

void LightLimitFix::Reset()
{
	rendered = false;
	for (auto& particleLight : particleLights) {
		if (const auto particleSystem = netimmerse_cast<RE::NiParticleSystem*>(particleLight.first)) {
			if (auto particleData = particleSystem->GetParticleRuntimeData().particleData.get()) {
				particleData->DecRefCount();
			}
		}
		particleLight.first->DecRefCount();
	}
	particleLights.clear();
	std::swap(particleLights, queuedParticleLights);
}

void LightLimitFix::Load(json& o_json)
{
	if (o_json[GetName()].is_object())
		settings = o_json[GetName()];
	Feature::Load(o_json);
}

void LightLimitFix::Save(json& o_json)
{
	o_json[GetName()] = settings;
}

void LightLimitFix::BSLightingShader_SetupGeometry_Before(RE::BSRenderPass*)
{
	strictLightsCount = 0;
	strictLightsData.clear();
}

void LightLimitFix::SetLightPosition(LightLimitFix::LightData& a_light, RE::NiPoint3& a_initialPosition)
{
	auto state = RE::BSGraphics::RendererShadowState::GetSingleton();
	for (int eyeIndex = 0; eyeIndex < eyeCount; eyeIndex++) {
		auto eyePosition = eyeCount == 1 ?
		                       state->GetRuntimeData().posAdjust.getEye(eyeIndex) :
                               state->GetVRRuntimeData().posAdjust.getEye(eyeIndex);
		auto viewMatrix = eyeCount == 1 ?
		                      state->GetRuntimeData().cameraData.getEye(eyeIndex).viewMat :
                              state->GetVRRuntimeData().cameraData.getEye(eyeIndex).viewMat;
		auto worldPos = a_initialPosition - eyePosition;
		a_light.positionWS[eyeIndex].x = worldPos.x;
		a_light.positionWS[eyeIndex].y = worldPos.y;
		a_light.positionWS[eyeIndex].z = worldPos.z;
		a_light.positionVS[eyeIndex] = DirectX::SimpleMath::Vector3::Transform(a_light.positionWS[eyeIndex], viewMatrix);
	}
}

void LightLimitFix::BSLightingShader_SetupGeometry_GeometrySetupConstantPointLights(RE::BSRenderPass* Pass, uint32_t NumLights, uint32_t ShadowLightCount)
{
	NumLights = (uint8_t)std::min(7, Pass->numLights - 1);

	for (uint8_t i = 0; i < NumLights; i++) {
		auto bsLight = Pass->sceneLights[i + 1];
		auto niLight = bsLight->light.get();

		LightData light{};

		float dimmer = niLight->GetLightRuntimeData().fade * bsLight->lodDimmer;
		dimmer = 1;
		light.color.x = niLight->GetLightRuntimeData().diffuse.red * dimmer;
		light.color.y = niLight->GetLightRuntimeData().diffuse.green * dimmer;
		light.color.z = niLight->GetLightRuntimeData().diffuse.blue * dimmer;
		light.radius = niLight->GetLightRuntimeData().radius.x;

		SetLightPosition(light, niLight->world.translate);

		//light.shadow = false;
		//light.mask = -1;

		if (i < ShadowLightCount) {
			//light.shadow = true;
			//light.mask = (float)static_cast<RE::BSShadowLight*>(bsLight)->maskSelect;
		}

		light.shadowMode = 0;

		strictLightsData.push_back(light);
		strictLightsCount++;
	}
}

float LightLimitFix::CalculateLuminance(CachedParticleLight& light, RE::NiPoint3& point)
{
	// See BSLight::CalculateLuminance_14131D3D0
	// Performs lighting on the CPU which is identical to GPU code

	auto lightDirection = light.position - point;
	float lightDist = lightDirection.Length();
	float intensityFactor = std::clamp(lightDist / light.radius, 0.0f, 1.0f);
	float intensityMultiplier = 1 - intensityFactor * intensityFactor;

	float luminance = (light.color.red + light.color.blue + light.color.green) / 3.0f;
	return luminance * intensityMultiplier;
}

void LightLimitFix::AddParticleLightLuminance(RE::NiPoint3& targetPosition, int& numHits, float& lightLevel)
{
	std::lock_guard<std::shared_mutex> lk{ cachedParticleLightsMutex };
	particleLightsDetectionHits = 0;
	if (settings.EnableParticleLightsDetection) {
		for (auto& light : cachedParticleLights[0]) {  // left eye only
			auto luminance = CalculateLuminance(light, targetPosition);
			lightLevel += luminance;
			if (luminance > 0.0)
				particleLightsDetectionHits++;
		}
	}
	numHits += particleLightsDetectionHits;
}

void LightLimitFix::BSLightingShader_SetupGeometry_After(RE::BSRenderPass*)
{
	auto context = RE::BSGraphics::Renderer::GetSingleton()->GetRuntimeData().context;
	D3D11_MAPPED_SUBRESOURCE mapped;
	DX::ThrowIfFailed(context->Map(strictLights->resource.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
	size_t bytes = sizeof(LightData) * strictLightsCount;
	memcpy_s(mapped.pData, bytes, strictLightsData.data(), bytes);
	context->Unmap(strictLights->resource.get(), 0);
}

void LightLimitFix::BSEffectShader_SetupGeometry_Before(RE::BSRenderPass* a_pass)
{
	if (auto shaderProperty = netimmerse_cast<RE::BSEffectShaderProperty*>(a_pass->shaderProperty)) {
		if (auto material = shaderProperty->material) {
			if (material->greyscaleTexturePath.size() > 1) {
				std::string textureName = material->greyscaleTexturePath.c_str();
				auto lastSeparatorPos = textureName.find_last_of("\\/");
				if (lastSeparatorPos != std::string::npos) {
					std::string filename = textureName.substr(lastSeparatorPos + 1);
#pragma warning(push)
#pragma warning(disable: 4244)
					std::transform(filename.begin(), filename.end(), filename.begin(), ::tolower);
#pragma warning(pop)
					;
					bool shouldCull = filename.contains("ench");
					if (shouldCull) {
						logger::info("cull");
					}
				}
			}
		}
	}
}

void LightLimitFix::Bind()
{
	auto context = RE::BSGraphics::Renderer::GetSingleton()->GetRuntimeData().context;
	auto accumulator = RE::BSGraphics::BSShaderAccumulator::GetCurrentAccumulator();

	auto reflections = (!REL::Module::IsVR() ?
							   RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData().cubeMapRenderTarget :
                               RE::BSGraphics::RendererShadowState::GetSingleton()->GetVRRuntimeData().cubeMapRenderTarget) == RE::RENDER_TARGETS_CUBEMAP::kREFLECTIONS;

	if (reflections || accumulator->GetRuntimeData().activeShadowSceneNode != RE::BSShaderManager::State::GetSingleton().shadowSceneNode[0]) {
		PerPass perPassData{};
		perPassData.EnableGlobalLights = false;

		D3D11_MAPPED_SUBRESOURCE mapped;
		DX::ThrowIfFailed(context->Map(perPass->resource.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
		size_t bytes = sizeof(PerPass);
		memcpy_s(mapped.pData, bytes, &perPassData, bytes);
		context->Unmap(perPass->resource.get(), 0);
	} else {
		if (!rendered) {
			UpdateLights();
			rendered = true;
		}

		{
			ID3D11ShaderResourceView* views[4]{};
			views[0] = lights->srv.get();
			views[1] = lightList->srv.get();
			views[2] = lightGrid->srv.get();
			views[3] = RE::BSGraphics::Renderer::GetSingleton()->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY].depthSRV;
			context->PSSetShaderResources(17, ARRAYSIZE(views), views);
		}

		{
			PerPass perPassData{};
			perPassData.CameraData.x = accumulator->kCamera->GetRuntimeData2().viewFrustum.fFar;
			perPassData.CameraData.y = accumulator->kCamera->GetRuntimeData2().viewFrustum.fNear;
			perPassData.CameraData.z = accumulator->kCamera->GetRuntimeData2().viewFrustum.fFar - accumulator->kCamera->GetRuntimeData2().viewFrustum.fNear;
			perPassData.CameraData.w = accumulator->kCamera->GetRuntimeData2().viewFrustum.fFar * accumulator->kCamera->GetRuntimeData2().viewFrustum.fNear;

			auto viewport = RE::BSGraphics::State::GetSingleton();
			if (!screenSpaceShadowsTexture) {
				auto shadowMask = RE::BSGraphics::Renderer::GetSingleton()->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kSHADOW_MASK];
				D3D11_TEXTURE2D_DESC texDesc{};
				shadowMask.texture->GetDesc(&texDesc);
				texDesc.Format = DXGI_FORMAT_R16_FLOAT;
				texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_RENDER_TARGET;
				screenSpaceShadowsTexture = new Texture2D(texDesc);
			}
			float resolutionX = screenSpaceShadowsTexture->desc.Width * viewport->GetRuntimeData().dynamicResolutionCurrentWidthScale;
			float resolutionY = screenSpaceShadowsTexture->desc.Height * viewport->GetRuntimeData().dynamicResolutionCurrentHeightScale;

			perPassData.LightsNear = lightsNear;
			perPassData.LightsFar = lightsFar;

			perPassData.BufferDim = { resolutionX, resolutionY };
			perPassData.FrameCount = viewport->uiFrameCount;
			perPassData.EnableGlobalLights = true;

			D3D11_MAPPED_SUBRESOURCE mapped;
			DX::ThrowIfFailed(context->Map(perPass->resource.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
			size_t bytes = sizeof(PerPass);
			memcpy_s(mapped.pData, bytes, &perPassData, bytes);
			context->Unmap(perPass->resource.get(), 0);
		}
	}

	{
		ID3D11ShaderResourceView* views[1]{};
		views[0] = perPass->srv.get();
		context->PSSetShaderResources(32, ARRAYSIZE(views), views);
	}
}

bool LightLimitFix::IsValidLight(RE::BSLight* a_light)
{
	return a_light && !a_light->light->GetFlags().any(RE::NiAVObject::Flag::kHidden);
}

bool LightLimitFix::IsGlobalLight(RE::BSLight* a_light)
{
	return !(a_light->portalStrict || !a_light->portalGraph);
}

struct VertexColor
{
	std::uint8_t data[4];
};

bool LightLimitFix::CheckParticleLights(RE::BSRenderPass* a_pass, uint32_t)
{
	// See https://www.nexusmods.com/skyrimspecialedition/articles/1391
	if (settings.EnableParticleLights) {
		if (auto shaderProperty = netimmerse_cast<RE::BSEffectShaderProperty*>(a_pass->shaderProperty)) {
			if (!shaderProperty->lightData) {
				if (auto material = shaderProperty->material) {
					if (!material->sourceTexturePath.empty()) {
						std::string textureName = material->sourceTexturePath.c_str();

						{
							if (textureName.size() < 1)
								return false;

							auto lastSeparatorPos = textureName.find_last_of("\\/");
							if (lastSeparatorPos == std::string::npos)
								return false;

							textureName = textureName.substr(lastSeparatorPos + 1);
							if (textureName.size() < 4)
								return false;

							textureName.erase(textureName.length() - 4);  // Remove ".dds"
#pragma warning(push)
#pragma warning(disable: 4244)
							std::transform(textureName.begin(), textureName.end(), textureName.begin(), ::tolower);
#pragma warning(pop)
						}

						auto& configs = ParticleLights::GetSingleton()->particleLightConfigs;
						auto it = configs.find(textureName);
						if (it == configs.end())
							return false;

						ParticleLights::Config* config = &it->second;
						ParticleLights::GradientConfig* gradientConfig = nullptr;
						if (!material->greyscaleTexturePath.empty()) {
							textureName = material->greyscaleTexturePath.c_str();
							{
								if (textureName.size() < 1)
									return false;

								auto lastSeparatorPos = textureName.find_last_of("\\/");
								if (lastSeparatorPos == std::string::npos)
									return false;

								textureName = textureName.substr(lastSeparatorPos + 1);
								if (textureName.size() < 4)
									return false;

								textureName.erase(textureName.length() - 4);  // Remove ".dds"
#pragma warning(push)
#pragma warning(disable: 4244)
								std::transform(textureName.begin(), textureName.end(), textureName.begin(), ::tolower);
#pragma warning(pop)
							}

							auto& gradientConfigs = ParticleLights::GetSingleton()->particleLightGradientConfigs;
							auto itGradient = gradientConfigs.find(textureName);
							if (itGradient == gradientConfigs.end())
								return false;
							gradientConfig = &itGradient->second;
						}

						a_pass->geometry->IncRefCount();
						if (const auto particleSystem = netimmerse_cast<RE::NiParticleSystem*>(a_pass->geometry)) {
							if (auto particleData = particleSystem->GetParticleRuntimeData().particleData.get()) {
								particleData->IncRefCount();
							}
						}

						RE::NiColorA color;
						color.red = material->baseColor.red * material->baseColorScale * settings.ParticleLightsBrightness;
						color.green = material->baseColor.green * material->baseColorScale * settings.ParticleLightsBrightness;
						color.blue = material->baseColor.blue * material->baseColorScale * settings.ParticleLightsBrightness;
						color.alpha = material->baseColor.alpha * shaderProperty->alpha;

						if (auto emittance = shaderProperty->unk88) {
							color.red *= emittance->red;
							color.green *= emittance->green;
							color.blue *= emittance->blue;
						}

						if (auto rendererData = a_pass->geometry->GetGeometryRuntimeData().rendererData) {
							if (rendererData->vertexDesc.HasFlag(RE::BSGraphics::Vertex::Flags::VF_COLORS)) {
								if (auto triShape = a_pass->geometry->AsTriShape()) {
									uint32_t vertexSize = rendererData->vertexDesc.GetSize();
									uint32_t offset = rendererData->vertexDesc.GetAttributeOffset(RE::BSGraphics::Vertex::Attribute::VA_COLOR);
									RE::NiColorA vertexColor{};
									for (int v = 0; v < triShape->GetTrishapeRuntimeData().vertexCount; v++) {
										if (VertexColor* vertex = reinterpret_cast<VertexColor*>(&rendererData->rawVertexData[vertexSize * v + offset])) {
											RE::NiColorA niColor{ (float)vertex->data[0] / 255.0f, (float)vertex->data[1] / 255.0f, (float)vertex->data[2] / 255.0f, (float)vertex->data[3] / 255.0f };
											if (niColor.alpha > vertexColor.alpha)
												vertexColor = niColor;
										}
									}
									color.red *= vertexColor.red;
									color.green *= vertexColor.green;
									color.blue *= vertexColor.blue;
									if (shaderProperty->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kVertexAlpha)) {
										color.alpha *= vertexColor.alpha;
									}
								}
							}
						}

						color.red *= config->colorMult.red;
						color.green *= config->colorMult.green;
						color.blue *= config->colorMult.blue;

						if (gradientConfig) {
							color.red *= gradientConfig->color.red;
							color.green *= gradientConfig->color.green;
							color.blue *= gradientConfig->color.blue;
						}

						queuedParticleLights.insert({ a_pass->geometry, { color, *config } });

						return settings.EnableParticleLightsCulling && config->cull;
					}
				}
			}
		}
	}
	return false;
}

void LightLimitFix::BSEffectShader_SetupGeometry(RE::BSRenderPass* a_pass)
{
	if (auto shaderProperty = netimmerse_cast<RE::BSEffectShaderProperty*>(a_pass->shaderProperty)) {
		if (auto material = shaderProperty->material) {
			logger::info("{}", material->greyscaleTexturePath.c_str());
		}
	}
}

enum class GrassShaderTechniques
{
	RenderDepth = 8,
};

void LightLimitFix::Draw(const RE::BSShader* shader, const uint32_t descriptor)
{
	switch (shader->shaderType.get()) {
	case RE::BSShader::Type::Lighting:
		{
			Bind();
		}
		break;
	case RE::BSShader::Type::Grass:
		{
			const auto technique = descriptor & 0b1111;
			if (technique != static_cast<uint32_t>(GrassShaderTechniques::RenderDepth)) {
				Bind();
			}
		}
		break;
	}
}

void LightLimitFix::DataLoaded()
{
	auto iMagicLightMaxCount = RE::GameSettingCollection::GetSingleton()->GetSetting("iMagicLightMaxCount");
	iMagicLightMaxCount->data.i = MAXINT32;
	logger::info("[LLF] Unlocked magic light limit");
}

float LightLimitFix::CalculateLightDistance(float3 a_lightPosition, float a_radius)
{
	return (a_lightPosition.x * a_lightPosition.x) + (a_lightPosition.y * a_lightPosition.y) + (a_lightPosition.z * a_lightPosition.z) - (a_radius * a_radius);
}

bool LightLimitFix::AddCachedParticleLights(eastl::vector<LightData>& lightsData, LightLimitFix::LightData& light, int a_eyeIndex, ParticleLights::Config* a_config, RE::BSGeometry* a_geometry, double a_timer)
{
	static float& lightFadeStart = (*(float*)RELOCATION_ID(527668, 414582).address());
	static float& lightFadeEnd = (*(float*)RELOCATION_ID(527669, 414583).address());

	float distance = CalculateLightDistance(light.positionWS[0], light.radius);

	float dimmer = 0.0f;

	if (distance < lightFadeStart || lightFadeEnd == 0.0f) {
		dimmer = 1.0f;
	} else if (distance <= lightFadeEnd) {
		dimmer = 1.0f - ((distance - lightFadeStart) / (lightFadeEnd - lightFadeStart));
	} else {
		dimmer = 0.0f;
	}

	light.color *= dimmer;

	float distantLightFadeStart = lightsFar * lightsFar * (lightFadeStart / lightFadeEnd);
	float distantLightFadeEnd = lightsFar * lightsFar;

	if (distance < distantLightFadeStart || distantLightFadeEnd == 0.0f) {
		dimmer = 1.0f;
	} else if (distance <= distantLightFadeEnd) {
		dimmer = 1.0f - ((distance - distantLightFadeStart) / (distantLightFadeEnd - distantLightFadeStart));
	} else {
		dimmer = 0.0f;
	}

	light.color *= dimmer;

	if ((light.color.x + light.color.y + light.color.z) > 1e-4 && light.radius > 1e-4) {
		if (a_geometry && a_config && a_config->flicker) {
			auto seed = (std::uint32_t)std::hash<void*>{}(a_geometry);

			siv::PerlinNoise perlin1{ seed };
			siv::PerlinNoise perlin2{ seed + 1 };
			siv::PerlinNoise perlin3{ seed + 2 };
			siv::PerlinNoise perlin4{ seed + 3 };

			auto scaledTimer = a_timer * a_config->flickerSpeed;

			for (int eyeIndex = 0; eyeIndex < eyeCount; eyeIndex++) {
				light.positionWS[eyeIndex].x += (float)perlin1.noise1D(scaledTimer) * a_config->flickerMovement;
				light.positionWS[eyeIndex].y += (float)perlin2.noise1D(scaledTimer) * a_config->flickerMovement;
				light.positionWS[eyeIndex].z += (float)perlin3.noise1D(scaledTimer) * a_config->flickerMovement;
			}
			dimmer = std::max(0.0f, dimmer - ((float)perlin4.noise1D_01(scaledTimer) * a_config->flickerIntensity));
		}

		CachedParticleLight cachedParticleLight{};
		cachedParticleLight.color = { light.color.x, light.color.y, light.color.z };
		cachedParticleLight.radius = light.radius;

		light.shadowMode = 2 * settings.EnableParticleLightsShadows;
		auto state = RE::BSGraphics::RendererShadowState::GetSingleton();
		auto eyePosition = eyeCount == 1 ?
		                       state->GetRuntimeData().posAdjust.getEye(a_eyeIndex) :
                               state->GetVRRuntimeData().posAdjust.getEye(a_eyeIndex);
		cachedParticleLight.position = { light.positionWS[a_eyeIndex].x + eyePosition.x, light.positionWS[a_eyeIndex].y + eyePosition.y, light.positionWS[a_eyeIndex].z + eyePosition.z };
		for (int eyeIndex = 0; eyeIndex < eyeCount; eyeIndex++) {
			auto viewMatrix = eyeCount == 1 ?
			                      state->GetRuntimeData().cameraData.getEye(eyeIndex).viewMat :
                                  state->GetVRRuntimeData().cameraData.getEye(eyeIndex).viewMat;
			light.positionVS[eyeIndex] = DirectX::SimpleMath::Vector3::Transform(light.positionWS[eyeIndex], viewMatrix);
		}
		for (int eyeIndex = 0; eyeIndex < eyeCount; eyeIndex++) {
			cachedParticleLights[eyeIndex].push_back(cachedParticleLight);
		}
		lightsData.push_back(light);
		return true;
	}
	return false;
}

float3 LightLimitFix::Saturation(float3 color, float saturation)
{
	auto grey = color.Dot(float3(0.3f, 0.59f, 0.11f));
	return float3::Lerp(float3(grey), color, saturation);
}

void LightLimitFix::UpdateLights()
{
	bool interior = true;

	if (auto playerCharacter = RE::PlayerCharacter::GetSingleton()) {
		if (auto parentCell = playerCharacter->GetParentCell()) {
			interior = parentCell->IsInteriorCell();
		}
	}

	auto accumulator = RE::BSGraphics::BSShaderAccumulator::GetCurrentAccumulator();

	lightsNear = std::max(0.0f, accumulator->kCamera->GetRuntimeData2().viewFrustum.fNear);
	lightsFar = std::min(interior ? 8192.0f : 16384.0f, accumulator->kCamera->GetRuntimeData2().viewFrustum.fFar);

	std::uint32_t currentLightCount = 0;  // Max number of lights is 4294967295

	auto shadowSceneNode = RE::BSShaderManager::State::GetSingleton().shadowSceneNode[0];
	auto state = RE::BSGraphics::RendererShadowState::GetSingleton();

	RE::NiLight* refLight = nullptr;
	RE::NiLight* magicLight = nullptr;
	RE::BSLight* firstPersonLight = nullptr;
	RE::BSLight* thirdPersonLight = nullptr;

	if (settings.EnableFirstPersonShadows) {
		if (auto playerCamera = RE::PlayerCamera::GetSingleton()) {
			if (playerCamera->IsInFirstPerson()) {
				if (auto player = RE::PlayerCharacter::GetSingleton()) {
					firstPersonLight = player->GetInfoRuntimeData().firstPersonLight.get();
					thirdPersonLight = player->GetInfoRuntimeData().thirdPersonLight.get();
					if (auto light = player->extraList.GetByType<RE::ExtraLight>()) {
						refLight = light->lightData->light.get();
					}
					if (auto light = player->extraList.GetByType<RE::ExtraMagicLight>()) {
						magicLight = light->lightData->light.get();
					}
				}
			}
		}
	}

	eastl::vector<LightData> lightsData{};

	static float* g_deltaTime = (float*)RELOCATION_ID(523660, 410199).address();  // 2F6B948, 30064C8
	static double timer = 0;
	if (!RE::UI::GetSingleton()->GameIsPaused())
		timer += *g_deltaTime;

	//process point lights
	for (auto& e : shadowSceneNode->GetRuntimeData().activePointLights) {
		if (auto bsLight = e.get()) {
			if (auto niLight = bsLight->light.get()) {
				if (IsValidLight(bsLight) && IsGlobalLight(bsLight)) {
					auto& runtimeData = niLight->GetLightRuntimeData();

					LightData light{};
					light.color = { runtimeData.diffuse.red, runtimeData.diffuse.green, runtimeData.diffuse.blue };
					light.color *= runtimeData.fade;
					light.color *= bsLight->lodDimmer;

					light.radius = runtimeData.radius.x;

					SetLightPosition(light, niLight->world.translate);

					static float& lightFadeStart = (*(float*)RELOCATION_ID(527668, 414582).address());
					static float& lightFadeEnd = (*(float*)RELOCATION_ID(527669, 414583).address());

					float distance = CalculateLightDistance(light.positionWS[0], light.radius);

					float distantLightFadeStart = lightsFar * lightsFar * (lightFadeStart / lightFadeEnd);
					float distantLightFadeEnd = lightsFar * lightsFar;

					float dimmer;

					if (distance < distantLightFadeStart || distantLightFadeEnd == 0.0f) {
						dimmer = 1.0f;
					} else if (distance <= distantLightFadeEnd) {
						dimmer = 1.0f - ((distance - distantLightFadeStart) / (distantLightFadeEnd - distantLightFadeStart));
					} else {
						dimmer = 0.0f;
					}

					light.color *= dimmer;

					if ((light.color.x + light.color.y + light.color.z) > 1e-4 && light.radius > 1e-4) {
						light.shadowMode = bsLight == firstPersonLight || bsLight == thirdPersonLight || niLight == refLight || niLight == magicLight;
						lightsData.push_back(light);
						currentLightCount++;
					}
				}
			}
		}
	}

	{
		std::lock_guard<std::shared_mutex> lk{ cachedParticleLightsMutex };
		cachedParticleLights[0].clear();
		cachedParticleLights[1].clear();

		LightData clusteredLight{};
		uint32_t clusteredLights = 0;

		auto eyeIndex = 0;  // only calculate for left eye
		auto eyePosition = eyeCount == 1 ?
		                       state->GetRuntimeData().posAdjust.getEye(eyeIndex) :
                               state->GetVRRuntimeData().posAdjust.getEye(eyeIndex);

		for (const auto& particleLight : particleLights) {
			if (const auto particleSystem = netimmerse_cast<RE::NiParticleSystem*>(particleLight.first);
				particleSystem && particleSystem->GetParticleRuntimeData().particleData.get()) {
				// process BSGeometry
				auto particleData = particleSystem->GetParticleRuntimeData().particleData.get();

				auto numVertices = particleData->GetActiveVertexCount();
				for (std::uint32_t p = 0; p < numVertices; p++) {
					float radius = particleData->GetParticlesRuntimeData().sizes[p] * settings.ParticleLightsRadius;

					auto initialPosition = particleData->GetParticlesRuntimeData().positions[p] + (particleSystem->GetParticleSystemRuntimeData().isWorldspace ? RE::NiPoint3{} : (particleLight.first->worldBound.center));

					RE::NiPoint3 positionWS = initialPosition - eyePosition;

					if (clusteredLights) {
						auto averageRadius = clusteredLight.radius / (float)clusteredLights;
						float radiusDiff = abs(averageRadius - radius);

						auto averagePosition = clusteredLight.positionWS[eyeIndex] / (float)clusteredLights;
						float positionDiff = positionWS.GetDistance({ averagePosition.x, averagePosition.y, averagePosition.z });

						if ((radiusDiff + positionDiff) > settings.ParticleLightsOptimisationClusterRadius || !settings.EnableParticleLightsOptimization) {
							clusteredLight.radius /= (float)clusteredLights;
							clusteredLight.positionWS[eyeIndex] /= (float)clusteredLights;
							clusteredLight.positionWS[1] = clusteredLight.positionWS[0];
							if (eyeCount == 2) {
								auto offset = eyePosition - state->GetVRRuntimeData().posAdjust.getEye(1);
								clusteredLight.positionWS[1].x += offset.x / (float)clusteredLights;
								clusteredLight.positionWS[1].y += offset.y / (float)clusteredLights;
								clusteredLight.positionWS[1].z += offset.z / (float)clusteredLights;
							}

							currentLightCount += AddCachedParticleLights(lightsData, clusteredLight, eyeIndex);

							clusteredLights = 0;
							clusteredLight.color = { 0, 0, 0 };
							clusteredLight.radius = 0;
							clusteredLight.positionWS[eyeIndex] = { 0, 0, 0 };
						}
					}

					float alpha = particleLight.second.first.alpha * particleData->GetParticlesRuntimeData().color[p].alpha;

					float3 color;
					color.x = particleLight.second.first.red * particleData->GetParticlesRuntimeData().color[p].red;
					color.y = particleLight.second.first.green * particleData->GetParticlesRuntimeData().color[p].green;
					color.z = particleLight.second.first.blue * particleData->GetParticlesRuntimeData().color[p].blue;

					clusteredLight.color += Saturation(color, settings.ParticleLightsSaturation) * alpha * std::numbers::pi_v<float>;

					clusteredLight.radius += radius * particleLight.second.second.radiusMult;
					clusteredLight.positionWS[eyeIndex].x += positionWS.x;
					clusteredLight.positionWS[eyeIndex].y += positionWS.y;
					clusteredLight.positionWS[eyeIndex].z += positionWS.z;

					clusteredLights++;
				}
			} else {
				// process billboard
				LightData light{};

				light.color.x = particleLight.second.first.red;
				light.color.y = particleLight.second.first.green;
				light.color.z = particleLight.second.first.blue;

				light.color = Saturation(light.color, settings.ParticleLightsSaturation);

				light.color *= particleLight.second.first.alpha;

				float radius = particleLight.first->GetModelData().modelBound.radius * particleLight.first->world.scale;

				light.radius = radius * settings.ParticleLightsRadiusBillboards;

				SetLightPosition(light, particleLight.first->worldBound.center);  //light is complete for both eyes by now

				currentLightCount += AddCachedParticleLights(lightsData, light, eyeIndex, &particleLight.second.second, particleLight.first, timer);
			}
		}

		if (clusteredLights) {
			clusteredLight.radius /= (float)clusteredLights;
			clusteredLight.positionWS[0] /= (float)clusteredLights;
			clusteredLight.positionWS[1] = clusteredLight.positionWS[0];
			if (eyeCount == 2) {
				auto offset = eyePosition - state->GetVRRuntimeData().posAdjust.getEye(1);
				clusteredLight.positionWS[1].x += offset.x / (float)clusteredLights;
				clusteredLight.positionWS[1].y += offset.y / (float)clusteredLights;
				clusteredLight.positionWS[1].z += offset.z / (float)clusteredLights;
			}
			currentLightCount += AddCachedParticleLights(lightsData, clusteredLight, eyeIndex);
		}
	}

	auto context = RE::BSGraphics::Renderer::GetSingleton()->GetRuntimeData().context;

	{
		if (!currentLightCount) {
			LightData data{};
			lightsData.push_back(data);
			currentLightCount = 1;
		}

		bool lightCountChanged = currentLightCount != lightCount;

		if (!lights || lightCountChanged) {
			lightCount = currentLightCount;

			D3D11_BUFFER_DESC sbDesc{};
			sbDesc.Usage = D3D11_USAGE_DYNAMIC;
			sbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
			sbDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			sbDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
			sbDesc.StructureByteStride = sizeof(LightData);
			sbDesc.ByteWidth = sizeof(LightData) * lightCount;
			lights = eastl::make_unique<Buffer>(sbDesc);

			D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
			srvDesc.Format = DXGI_FORMAT_UNKNOWN;
			srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
			srvDesc.Buffer.FirstElement = 0;
			srvDesc.Buffer.NumElements = lightCount;
			lights->CreateSRV(srvDesc);
		}

		D3D11_MAPPED_SUBRESOURCE mapped;
		DX::ThrowIfFailed(context->Map(lights->resource.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
		size_t bytes = sizeof(LightData) * lightCount;
		memcpy_s(mapped.pData, bytes, lightsData.data(), bytes);
		context->Unmap(lights->resource.get(), 0);
	}

	{
		auto projMatrixUnjittered = eyeCount == 1 ? state->GetRuntimeData().cameraData.getEye().projMatrixUnjittered : state->GetVRRuntimeData().cameraData.getEye().projMatrixUnjittered;
		float fov = atan(1.0f / static_cast<float4x4>(projMatrixUnjittered).m[0][0]) * 2.0f * (180.0f / 3.14159265359f);

		static float _near = 0.0f, _far = 0.0f, _fov = 0.0f, _lightsNear = 0.0f, _lightsFar = 0.0f;
		if (fabs(_near - accumulator->kCamera->GetRuntimeData2().viewFrustum.fNear) > 1e-4 || fabs(_far - accumulator->kCamera->GetRuntimeData2().viewFrustum.fFar) > 1e-4 || fabs(_fov - fov) > 1e-4 || fabs(_lightsNear - lightsNear) > 1e-4 || fabs(_lightsFar - lightsFar) > 1e-4) {
			PerFrameLightCulling perFrameData{};
			perFrameData.InvProjMatrix[0] = DirectX::XMMatrixInverse(nullptr, projMatrixUnjittered);
			if (eyeCount == 1)
				perFrameData.InvProjMatrix[1] = perFrameData.InvProjMatrix[0];
			else
				perFrameData.InvProjMatrix[1] = DirectX::XMMatrixInverse(nullptr, state->GetVRRuntimeData().cameraData.getEye(1).projMatrixUnjittered);
			perFrameData.LightsNear = lightsNear;
			perFrameData.LightsFar = lightsFar;

			perFrameLightCulling->Update(perFrameData);

			ID3D11Buffer* perframe_cb = perFrameLightCulling->CB();
			context->CSSetConstantBuffers(0, 1, &perframe_cb);

			ID3D11UnorderedAccessView* clusters_uav = clusters->uav.get();
			context->CSSetUnorderedAccessViews(0, 1, &clusters_uav, nullptr);

			context->CSSetShader(clusterBuildingCS, nullptr, 0);
			context->Dispatch(CLUSTER_SIZE_X, CLUSTER_SIZE_Y, CLUSTER_SIZE_Z);
			context->CSSetShader(nullptr, nullptr, 0);

			ID3D11UnorderedAccessView* null_uav = nullptr;
			context->CSSetUnorderedAccessViews(0, 1, &null_uav, nullptr);

			_near = accumulator->kCamera->GetRuntimeData2().viewFrustum.fNear;
			_far = accumulator->kCamera->GetRuntimeData2().viewFrustum.fFar;
			_fov = fov;
			_lightsNear = lightsNear;
			_lightsFar = lightsFar;
		}
	}

	{
		ID3D11ShaderResourceView* srvs[] = { clusters->srv.get(), lights->srv.get() };
		context->CSSetShaderResources(0, ARRAYSIZE(srvs), srvs);
		ID3D11UnorderedAccessView* uavs[] = { lightCounter->uav.get(), lightList->uav.get(), lightGrid->uav.get() };
		context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);

		context->CSSetShader(clusterCullingCS, nullptr, 0);
		context->Dispatch(CLUSTER_SIZE_X / 32, CLUSTER_SIZE_Y / 16, CLUSTER_SIZE_Z / 2);
		context->CSSetShader(nullptr, nullptr, 0);
	}

	ID3D11ShaderResourceView* null_srvs[2] = { nullptr };
	context->CSSetShaderResources(0, ARRAYSIZE(null_srvs), null_srvs);
	ID3D11UnorderedAccessView* null_uavs[3] = { nullptr };
	context->CSSetUnorderedAccessViews(0, ARRAYSIZE(null_uavs), null_uavs, nullptr);
}