#include "Rendering/EnvironmentProbe/CameraPass.h"

#include "Globals.h"
#include "State.h"
#include "Util.h"

namespace EnvironmentProbe
{
	CameraPass& GetCameraPass()
	{
		static CameraPass pass;
		return pass;
	}

	void CameraPass::Render()
	{
		if (!enabled || lastFrame == globals::state->frameCount)
			return;
		lastFrame = globals::state->frameCount;
		if (resetPending.exchange(false)) {
			validFaces = 0;
			anchor.Reset();
		}
		valid = false;
		result = Result::Unavailable;
		if (REL::Module::IsVR() || !globals::d3d::device || !globals::game::smState->shadowSceneNode[0])
			return;

		try {
			auto* context = globals::d3d::context;
			if (statisticsPending) {
				D3D11_QUERY_DATA_PIPELINE_STATISTICS counts{};
				if (context->GetData(statistics.get(), &counts, sizeof(counts), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK) {
					logger::info("[EnvironmentProbeStats] frame={} triangles={} VS={} GS={} emitted={} PS={} CS={}", statisticsFrame,
						counts.IAPrimitives, counts.VSInvocations, counts.GSInvocations, counts.GSPrimitives, counts.PSInvocations, counts.CSInvocations);
					statisticsPending = false;
				}
			}
			if (!radiance || !depth || !normals) {
				auto create = [](DXGI_FORMAT format, const char* name) {
					D3D11_TEXTURE2D_DESC desc{};
					desc.Width = desc.Height = Resolution;
					desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
					desc.Format = format;
					desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
					auto texture = std::make_unique<Texture2D>(desc, name);
					texture->CreateRTV({ .Format = format, .ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D });
					texture->CreateSRV({ .Format = format, .ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D, .Texture2D = { .MostDetailedMip = 0, .MipLevels = 1 } });
					return texture;
				};
				radiance = create(DXGI_FORMAT_R16G16B16A16_FLOAT, "EnvironmentProbe::Camera Radiance");
				depth = create(DXGI_FORMAT_R32_FLOAT, "EnvironmentProbe::Camera Depth");
				normals = create(DXGI_FORMAT_R16G16_UNORM, "EnvironmentProbe::Camera World Normals");
				const float black[4]{};
				const float emptyDepth[4]{ FarDistance, FarDistance, FarDistance, FarDistance };
				context->ClearRenderTargetView(radiance->rtv.get(), black);
				context->ClearRenderTargetView(depth->rtv.get(), emptyDepth);
				context->ClearRenderTargetView(normals->rtv.get(), black);
			}
			// Image-space draws can replace the cached framebuffer with an orthographic
			// camera. The world camera retains the actual capture position in both POVs.
			const auto* camera = RE::Main::WorldRootCamera();
			if (!camera)
				return;
			bool discontinuity;
			const auto anchorStart = std::chrono::steady_clock::now();
			const auto& rotation = camera->world.rotate;
			const RE::NiPoint3 forward{ rotation.entry[0][0], rotation.entry[1][0], rotation.entry[2][0] };
			if (!anchor.Update(camera->world.translate, forward, anchorDistance, *globals::game::deltaTime, lastFrame, discontinuity)) {
				validFaces = 0;
				return;
			}
			if (discontinuity)
				validFaces = 0;
			const auto position = anchor.Position();
			if (globals::state->IsDeveloperMode() && lastFrame % 127 == 0) {
				const auto offset = position - camera->world.translate;
				logger::info("[EnvironmentProbeAnchor] frame={} offset=({:.2f},{:.2f},{:.2f}) cpu_us={:.1f} reset={}",
					lastFrame, offset.x, offset.y, offset.z,
					std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - anchorStart).count(), discontinuity);
			}
			Request request;
			request.position = { position.x, position.y, position.z };
			request.faces = mode == Mode::AllFaces ? 0xFFu : 1u << (lastFrame & 7u);
			request.cullDynamic = false;
			request.supplementalSunShadows = supplementalSunShadows;
			request.asyncShaders = true;
			request.farDistance = FarDistance;
			request.radiance = radiance->rtv.get();
			request.depth = depth->rtv.get();
			request.normals = normals->rtv.get();
			validFaces &= ~request.faces;
			const bool sample = globals::state->IsDeveloperMode() && !statisticsPending && lastFrame % 127 == 0;
			if (sample && !statistics) {
				const D3D11_QUERY_DESC desc{ D3D11_QUERY_PIPELINE_STATISTICS, 0 };
				if (SUCCEEDED(globals::d3d::device->CreateQuery(&desc, statistics.put())))
					Util::SetResourceName(statistics.get(), "EnvironmentProbe::Pipeline Statistics");
			}
			if (sample && statistics)
				context->Begin(statistics.get());
			globals::state->BeginPerfEvent("Environment Probe Camera Pass");
			result = EnvironmentProbe::Render(request);
			globals::state->EndPerfEvent();
			if (sample && statistics) {
				context->End(statistics.get());
				statisticsFrame = lastFrame;
				statisticsPending = true;
			}
			valid = result == Result::Success;
			if (valid) {
				validFaces |= request.faces;
				for (uint32_t face = 0; face < 8; ++face)
					if (request.faces & (1u << face)) {
						origins[face] = request.position;
						captureFrames[face] = lastFrame;
					}
			}

			if (result == Result::ShaderUnavailable || result == Result::InvalidRequest) {
				enabled = false;
				logger::error("Environment probe camera pass stopped: {}", static_cast<int>(result));
			}
		} catch (const std::exception& error) {
			enabled = false;
			logger::error("Environment probe camera pass: {}", error.what());
		}
	}
}
