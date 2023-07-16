#include "Clustered.h"
#include "State.h"

#include <RE/S/ShaderAccumulator.h>
#include <RE/S/ShadowState.h>

void Clustered::Reset()
{
	rendered = false;
}

void Clustered::Bind(bool a_update)
{
	if (!rendered && a_update) {
		UpdateLights();
		rendered = true;
	}

	if (lights) {
		auto renderer = RE::BSGraphics::Renderer::GetSingleton();
		auto context = renderer->GetRuntimeData().context;
		ID3D11ShaderResourceView* views[1]{};
		views[0] = lights.get()->srv.get();
		context->PSSetShaderResources(17, ARRAYSIZE(views), views);
	}
}

void Clustered::UpdateLights()
{
	std::uint32_t currentLightCount = 0;  // Max number of lights is 4294967295

	auto accumulator = RE::BSGraphics::BSShaderAccumulator::GetCurrentAccumulator();

	//auto shadowSceneNode = RE::BSShaderManager::State::GetSingleton().shadowSceneNode[0];
	auto shadowSceneNode = accumulator->GetRuntimeData().activeShadowSceneNode;
	auto state = RE::BSGraphics::RendererShadowState::GetSingleton();

	std::vector<LightSData> lights_data{};

	auto& runtimeData = shadowSceneNode->GetRuntimeData();
	auto processLights = [&](RE::NiLight* niLight, RE::BSLight* bsLight, RE::BSShadowLight* bsShadowLight) {
		RE::NiPoint3 worldPos = niLight->world.translate;

		float lodDimmer = 0;
		if (bsLight)
			lodDimmer = bsLight->lodDimmer;
		else if (bsShadowLight)
			lodDimmer = bsShadowLight->lodDimmer;
		float dimmer = niLight->GetLightRuntimeData().fade * lodDimmer;
		logger::trace("Found {}light {} at ({} {} {})", bsShadowLight ? "shadow" : "", niLight->name, worldPos.x, worldPos.y, worldPos.z);

		LightSData light{};

		DirectX::XMFLOAT3 color{};
		color.x = dimmer * niLight->GetLightRuntimeData().diffuse.red;
		color.y = dimmer * niLight->GetLightRuntimeData().diffuse.green;
		color.z = dimmer * niLight->GetLightRuntimeData().diffuse.blue;
		light.color = XMLoadFloat3(&color);

		for (int eyeIndex = 0; eyeIndex < (!REL::Module::IsVR() ? 1 : 2); eyeIndex++) {
			DirectX::XMFLOAT3 position{};
			auto eyePos = !REL::Module::IsVR() ?
			                  state->GetRuntimeData().posAdjust.getEye(eyeIndex) :
			                  state->GetVRRuntimeData().posAdjust.getEye(eyeIndex);
			auto adjustedWorldPos = worldPos - eyePos;

			position.x = adjustedWorldPos.x;
			position.y = adjustedWorldPos.y;
			position.z = adjustedWorldPos.z;
			light.positionWS[eyeIndex] = XMLoadFloat3(&position);
		}

		//logger::trace("Set {}light {} at ({} {} {}) because of eye ({} {} {})", bsShadowLight ? "shadow" : "", niLight->name, worldPos.x, worldPos.y, worldPos.z,
		//	eyePosition.x, eyePosition.y, eyePosition.z);

		light.radius = niLight->GetLightRuntimeData().radius.x;

		light.active = true;
		light.shadow = (bool)bsShadowLight;
		light.mask = bsShadowLight ? (float)bsShadowLight->maskSelect : -1;
		lights_data.push_back(light);
		currentLightCount++;
	};
	for (auto& e : runtimeData.activePointLights) {
		if (auto bsLight = e.get()) {
			if (auto niLight = bsLight->light.get()) {
				// See ShadowSceneNode::GetLuminanceAtPoint_1412BC190
				// Not particularly needed.
				if (!bsLight->pointLight) {
					continue;
				}

				// Vanilla is bugged
				//if (!bsLight->affectLand) {
				//	continue;
				//}

				// See BSFadeNode::sub_1412BB090
				// Unlikely to use full portal culling.
				if (bsLight->unk05C == 255) {
					continue;
				}
				if (bsLight->objectNode) {
					continue;
				}
				if (bsLight->Unk_03()) {
					continue;
				}

				// unk018->unk0 maintains a list of BSGeometry nodes where their sorted light lists contain the current light.
				// This is used to control updates and destruction of lights.
				// This checks that the head of the list is not-null, however if the light is too weak to hit an object it may not exist. (untested)
				if (!bsLight->unk018.unk00) {
					continue;
				}

				processLights(niLight, bsLight, nullptr);
			}
		}
	}

	// See ShadowSceneNode::CalculateActiveShadowCasterLights_1412E2F60
	// Whilst there is another shadow caster list, it includes ones that aren't being used due to the shadow caster limit.
	for (auto& e : runtimeData.activeShadowLights) {
		if (auto bsShadowLight = (RE::BSShadowLight*)e.get()) {
			if (auto niLight = bsShadowLight->light.get()) {
				processLights(niLight, nullptr, bsShadowLight);
			}
		}
	}

	if (!currentLightCount) {
		LightSData data{};
		ZeroMemory(&data, sizeof(data));
		lights_data.push_back(data);
		currentLightCount = 1;
	}

	static std::uint32_t lightCount = 0;
	bool lightCountChanged = currentLightCount != lightCount;

	if (!lights || lightCountChanged) {
		lightCount = currentLightCount;
		logger::debug("Found {} lights", lightCount);
		D3D11_BUFFER_DESC sbDesc{};
		sbDesc.Usage = D3D11_USAGE_DYNAMIC;
		sbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		sbDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		sbDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		sbDesc.StructureByteStride = sizeof(LightSData);
		sbDesc.ByteWidth = sizeof(LightSData) * lightCount;
		lights = std::make_unique<Buffer>(sbDesc);

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
		srvDesc.Format = DXGI_FORMAT_UNKNOWN;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
		srvDesc.Buffer.FirstElement = 0;
		srvDesc.Buffer.NumElements = lightCount;
		lights->CreateSRV(srvDesc);
	}

	auto context = RE::BSGraphics::Renderer::GetSingleton()->GetRuntimeData().context;
	D3D11_MAPPED_SUBRESOURCE mapped;
	DX::ThrowIfFailed(context->Map(lights->resource.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
	size_t bytes = sizeof(LightSData) * lightCount;
	memcpy_s(mapped.pData, bytes, lights_data.data(), bytes);
	context->Unmap(lights->resource.get(), 0);
}