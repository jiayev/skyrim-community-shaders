#if defined(CS_HAS_RENDER_GRAPH) && defined(CS_HAS_ORG_MODULE_SERVICES)
#	include "DrawsState.h"
#include "Features/DrawcallLimitFix/Scene/FrameView/FrameView.h"
#	include "DrawsView.h"
#	include "Events.h"
#	include "Observers.h"
#	include "Features/DrawcallLimitFix/Diagnostics/Engine/Registrations.h"

// The culling's own counters (always on): the colour epoch's count buffer copied every 120th epoch and read three frames later into
// the stats, with the CPU's side of the sun test and, under CS_DCLF_SET_PARITY, of the terrain near test and the forward and
// refraction registrations.

namespace DCLF::Diagnostics
{
	void NoteCullLatch(const CullLatchWritten& a_written)
	{
		auto& state = DrawsState::Get();
		if (a_written.depthOnly)
			std::copy(std::begin(a_written.latch.terrainNear), std::end(a_written.latch.terrainNear), state.terrainNearUpload.begin());
		else
			state.sunUpload = a_written.latch;
	}

	void ReadCullCounters(const MainEpochDone& a_epoch)
	{
		if (a_epoch.depthOnly)
			return;
		const auto& a_resources = a_epoch.resources;
		auto& a_stats = a_epoch.stats;
		const MainPayload& a_payload = a_epoch.payload;
		const auto& impl = DrawsView::Get();
		const auto& scene = impl.scene;
		const auto& sunCascades = impl.sunCascades;
		auto& state = DrawsState::Get();
		auto& cullReadback = state.cullReadback;
		auto& cullEpochs = state.cullEpochs;
		const auto& sunUpload = state.sunUpload;
		const auto& terrainNearUpload = state.terrainNearUpload;
		using CullReadback = DrawsState::CullReadback;
		auto* context = globals::d3d::context;
		// This frame's engine blended registrations of members (taken every colour epoch, so each copy has its own frame's).
		const std::uint32_t forwardEngine = Registrations::TakeForward();
		const std::uint32_t refractionEngine = Registrations::TakeRefraction();  // the refraction's, likewise
		if (cullReadback) {
			if (--cullReadback->framesLeft)
				return;
			D3D11_MAPPED_SUBRESOURCE mapped{};
			if (SUCCEEDED(context->Map(cullReadback->count.get(), 0, D3D11_MAP_READ, 0, &mapped))) {
				const auto* words = static_cast<const std::uint32_t*>(mapped.pData);
				a_stats.cullDrawn = words[0];
				a_stats.cullRejected = words[1];
				a_stats.cullTested = words[2];
				a_stats.cullOccluded = words[6];
				a_stats.hzbNear = words[7];
				a_stats.hzbFar = words[8];
				a_stats.hzbSampled = words[9];
				if (words[10]) {
					a_stats.hzbSample.valid = true;
					std::memcpy(&a_stats.hzbSample.farthest, &words[11], sizeof(float));
					std::memcpy(&a_stats.hzbSample.nearestZ, &words[12], sizeof(float));
					a_stats.hzbSample.uvMin[0] = (words[13] & 0xFFFF) / 65535.0f;
					a_stats.hzbSample.uvMin[1] = (words[13] >> 16) / 65535.0f;
					a_stats.hzbSample.uvMax[0] = (words[14] & 0xFFFF) / 65535.0f;
					a_stats.hzbSample.uvMax[1] = (words[14] >> 16) / 65535.0f;
					a_stats.hzbSample.mip = words[15];
				} else {
					a_stats.hzbSample.valid = false;
				}
				a_stats.cullDrawnPhaseTwo = words[17];
				a_stats.cullRescuedByPhaseTwo = words[18];
				a_stats.decalsCulled = words[kCountDecalsCulledWord];
				a_stats.decalsTested = words[kCountDecalsTestedWord];
				a_stats.sunTested = words[kCountSunTestedWord];
				a_stats.sunMissed = words[kCountSunMissedWord];
				a_stats.fadeTested = words[kCountFadeTestedWord];
				a_stats.fadeHidden = words[kCountFadeHiddenWord];
				if (scene)
					a_stats.fadeRoots = scene->fadeRootCount;
				a_stats.sunCpuTested = cullReadback->sunCpuTested;
				a_stats.sunCpuMissed = cullReadback->sunCpuMissed;
				a_stats.terrainNearTested = words[kCountTerrainNearTestedWord];
				a_stats.terrainNear = words[kCountTerrainNearWord];
				a_stats.terrainCpu = cullReadback->terrainCpu;
				a_stats.terrainCpuTested = cullReadback->terrainCpuTested;
				a_stats.terrainCpuNear = cullReadback->terrainCpuNear;
				a_stats.forwardDrawn = words[kCountForwardWord];
				a_stats.forwardTested = words[kCountForwardTestedWord];
				a_stats.forwardInView = words[kCountForwardInViewWord];
				a_stats.forwardOccluded = words[kCountForwardOccludedWord];
				a_stats.forwardEngine = cullReadback->forwardEngine;
				a_stats.forwardObserved = cullReadback->forwardObserved;
				a_stats.refractionDrawn = words[kCountRefractionWord];
				a_stats.refractionTested = words[kCountRefractionTestedWord];
				a_stats.refractionInView = words[kCountRefractionInViewWord];
				a_stats.refractionOccluded = words[kCountRefractionOccludedWord];
				a_stats.refractionEngine = cullReadback->refractionEngine;
				a_stats.refractionObserved = cullReadback->refractionObserved;
				context->Unmap(cullReadback->count.get(), 0);
			}
			cullReadback.reset();
			return;
		}
		if ((cullEpochs++ % 120) != 0 || !a_resources->countD3D11)
			return;
		D3D11_BUFFER_DESC desc{};
		a_resources->countD3D11->GetDesc(&desc);
		desc.Usage = D3D11_USAGE_STAGING;
		desc.BindFlags = 0;
		desc.MiscFlags = 0;
		desc.StructureByteStride = 0;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		CullReadback readback;
		if (FAILED(globals::d3d::device->CreateBuffer(&desc, nullptr, readback.count.put())))
			return;
		ScopedPerfEvent event("CS DCLF: culling readback");
		context->CopyResource(readback.count.get(), a_resources->countD3D11.get());
		readback.framesLeft = 3;
		readback.forwardEngine = forwardEngine;
		readback.forwardObserved = Diagnostics::Observe().claims && ActiveToggles().forward;
		readback.refractionEngine = refractionEngine;
		readback.refractionObserved = Diagnostics::Observe().claims && ActiveToggles().forward && ActiveToggles().refraction;
		// The CPU's side of the sun test, over the same frame's colour inputs and the same planes.
		auto sunTest = [&](const DrawInput& input) {
			// The colour segment's inputs of the one main list (its view bits, as BuildDrawsCS tests them first).
			if (!(input.view.mask & kColourViewBits) || !(input.flags & kObjectSunTest) || !(sunUpload.sunState & kSunTestOn))
				return;
			++readback.sunCpuTested;
			bool inside = false;
			const auto& tablesNow = Scene::FrameView::Get().GetTables();
			const auto* geometry = input.objectIndex < tablesNow.objectGeometry.size() ? tablesNow.objectGeometry[input.objectIndex] : nullptr;
			if (!geometry)
				return;
			const auto& bound = geometry->worldBound;
			const float center[3]{ bound.center.x, bound.center.y, bound.center.z };
			for (std::size_t c = 0; c < sunCascades.size() && !inside; ++c)
				inside = InSunCascade(sunCascades[c], center, bound.radius);
			readback.sunCpuMissed += inside ? 0 : 1;
		};
		// CS_DCLF_SET_PARITY (<- TB NEAR): Terrain Blending's near test over the same frame's terrain group 1 inputs of the depth
		// segment's first phase, with the eye and distance its latch had (terrainNearUpload), as TerrainBlending.cpp tests a pass.
		readback.terrainCpu = Diagnostics::Observe().claims && terrainNearUpload[3] > 0.0f;
		auto terrainTest = [&](const DrawInput& input) {
			if (!readback.terrainCpu || !(input.view.mask & kDepthViewBits) || ObjectTerrainGroup(input.flags) != 1)
				return;
			++readback.terrainCpuTested;
			const auto& tablesNow = Scene::FrameView::Get().GetTables();
			const auto* geometry = input.objectIndex < tablesNow.objectGeometry.size() ? tablesNow.objectGeometry[input.objectIndex] : nullptr;
			if (!geometry)
				return;
			const auto& bound = geometry->worldBound;
			const RE::NiPoint3 eye{ terrainNearUpload[0], terrainNearUpload[1], terrainNearUpload[2] };
			readback.terrainCpuNear += bound.center.GetDistance(eye) - bound.radius <= terrainNearUpload[3] ? 1 : 0;
		};
		if (a_payload.resident.elements)
			for (const auto& input : *a_payload.resident.elements) {
				sunTest(input);
				terrainTest(input);
			}
		for (const auto& input : a_payload.inputList) {
			sunTest(input);
			terrainTest(input);
		}
		cullReadback = std::move(readback);
	}
}

#endif
