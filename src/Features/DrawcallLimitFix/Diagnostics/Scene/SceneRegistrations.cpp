// What the frame registered, observed: the capture's drain read on the render thread (the Lighting shader check, the main camera's
// registrations), posted for the next scene pass, which checks the registrations against the residency its walk left (the registration
// parity). The normal path reads no registration. (The reflection residue's classification is Features/Reflection's, Diagnostics/Residue.cpp.)

#include "SceneObservers.h"
#include "SceneState.h"

#include "Features/DrawcallLimitFix/Scene/Ports/EngineShaders.h"
#include "Features/DrawcallLimitFix/Scene/Coordinator/Internal.h"

namespace DCLF::Diagnostics
{
	namespace
	{
		using V = SceneView;

		bool parityOn = false;

		/** @brief The main camera's batch renderers, for the filter of the capture; false before the first latch (the first frame). */
		bool RefreshMainBatchRenderers()
		{
			auto* accumulator = V::latchedAccumulator() ? V::latchedAccumulator() : *globals::game::currentAccumulator.get();
			auto* batch = accumulator ? accumulator->GetRuntimeData().batchRenderer : nullptr;
			if (!batch)
				return false;
			auto& renderers = SceneDiagnostics().mainBatchRenderers;
			renderers.clear();
			renderers.insert(batch);
			for (auto* group : batch->geometryGroups) {
				if (group && group->batchRenderer)
					renderers.insert(group->batchRenderer);
			}
			return true;
		}

		void OnCaptureDrained(const CaptureDrained& a_event)
		{
			auto& state = SceneDiagnostics();
			auto drain = std::make_shared<RegistrationDrain>();
			drain->observed = parityOn;
			const auto entries = a_event.entries;
			if (RefreshMainBatchRenderers()) {
				// Any Lighting pass the frame registered names the engine's instance DCLF's programs are of (CaptureLightingShader).
				if (auto* lighting = EngineShaders::Lighting())
					for (const auto& entry : entries)
						if (entry.pass && entry.pass->shader && entry.pass->shader->shaderType.get() == RE::BSShader::Type::Lighting) {
							if (entry.pass->shader != lighting && state.registrations.lightingShaderDiffers++ == 0)
								logger::warn("[DCLF] the Lighting shader the registrations use ({}) is not the engine's instance DCLF's programs are of ({}) <- LIGHTING SHADER",
									static_cast<const void*>(entry.pass->shader), static_cast<const void*>(lighting));
							break;
						}
				// Eligible objects DCLF has not bound that the engine registered: a scene event DCLF missed (a record not written again,
				// a verdict not taken again). The tracked set is the coordinator's: checked by the next scene pass.
				for (const auto& entry : entries)
					if (entry.geometry && state.mainBatchRenderers.contains(entry.batch))
						drain->registrations.push_back({ entry.geometry, entry.hint });
			} else {
				drain->observed = false;
			}
			// Latest wins (the newest frame's registrations alone are checked).
			state.drainPosted.store(std::shared_ptr<const RegistrationDrain>(std::move(drain)), std::memory_order_release);
		}

		void OnInputsTaken(const On<ScenePoint::AccumulateInputsTaken>&)
		{
			// None posted since the last pass leaves none, so no drain is checked twice.
			auto& state = SceneDiagnostics();
			state.drain = state.drainPosted.exchange(nullptr, std::memory_order_acq_rel);
		}

		void CheckRegistrations(const Scene::Coordinator& a_store)
		{
			// The registration parity: only while the render thread observed the frame's registrations; the normal path reads none of them.
			// The newest frame's drain posted since the last pass (the frame before this pass's), judged against the residency this pass's
			// walk left, before its joins: a record bound by the last pass counts as bound.
			auto& state = SceneDiagnostics();
			const auto& drain = state.drain;
			if (!drain || !drain->observed)
				return;
			auto& r = state.registrations;
			const auto& tracked = V::tracked(a_store);
			++r.registrationFrames;
			r.registrationsChecked += drain->registrations.size();
			for (const auto& entry : drain->registrations) {
				const auto it = tracked.find(const_cast<RE::BSGeometry*>(entry.geometry));
				if (it == tracked.end() || it->second.candidateReason != Ineligible::None || V::ResidentObject(a_store, a_store.FindObject(entry.geometry)))
					continue;
				++r.registeredUnbound;
				// The names are the engine's: read under a lease (the scene lane), skipped without one.
				if (r.registeredUnboundFirst.empty())
					if (const EngineReadWindow::Lease lease; lease)
						r.registeredUnboundFirst = fmt::format("'{}' under '{}' (hint {})", entry.geometry->name.c_str() ? entry.geometry->name.c_str() : "",
							entry.geometry->parent && entry.geometry->parent->name.c_str() ? entry.geometry->parent->name.c_str() : "", entry.hint);
			}
		}

		void OnJoinsBegin(const On<ScenePoint::JoinsBegin>&)
		{
			CheckRegistrations(Scene::Coordinator::Get());
		}
	}

	void InstallRegistrations(bool a_parity)
	{
		parityOn = a_parity;
		if (!parityOn)
			return;
		Subscribe<CaptureDrained>(OnCaptureDrained);
		Subscribe<ScenePoint::AccumulateInputsTaken>(OnInputsTaken);
		Subscribe<ScenePoint::JoinsBegin>(OnJoinsBegin);
	}
}
