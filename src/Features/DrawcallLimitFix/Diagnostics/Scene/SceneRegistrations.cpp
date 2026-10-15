// What the frame registered, observed: the capture's drain read on the render thread (the Lighting shader check, the main camera's
// registrations, the reflection residue's geometries), posted for the next scene pass, which classifies the residue and checks the
// registrations against the residency its walk left (the registration parity). The normal path reads no registration.

#include "SceneObservers.h"
#include "SceneState.h"

#include "Features/DrawcallLimitFix/Scene/Ports/EngineShaders.h"
#include "Features/DrawcallLimitFix/Scene/Coordinator/Internal.h"

namespace DCLF::Diagnostics
{
	namespace
	{
		using V = SceneView;

		bool residueOn = false, parityOn = false;

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
			// The reflection residue's geometries, classified by the next scene pass. Its watch is its own gate (the parities:
			// IndirectDraws::ReflectionRootsOwned); taken every frame so it never accumulates. Their names are read here,
			// where the frame registered them (the pass reading them runs past this frame's Present).
			if (residueOn) {
				const auto residue = PassCapture::Get().TakeReflectionResidueGeometries();
				drain->residue.reserve(residue.size());
				for (const auto* geometry : residue) {
					const auto* parent = geometry ? geometry->parent : nullptr;
					drain->residue.push_back({ geometry, geometry ? fmt::format("'{}' under '{}' ({})", geometry->name.c_str() ? geometry->name.c_str() : "",
																		parent && parent->name.c_str() ? parent->name.c_str() : "",
																		geometry->GetRTTI() && geometry->GetRTTI()->name ? geometry->GetRTTI()->name : "?") :
																	std::string() });
				}
			}
			drain->observed = parityOn;
			const auto entries = a_event.entries;
			if (parityOn && RefreshMainBatchRenderers()) {
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
			// Latest wins for the registrations (the newest frame's alone are checked); a post no scene pass took gives its residue to this
			// one, so none is lost or classified twice. The take between this exchange and the store finds nothing and takes it next pass.
			if (const auto unread = state.drainPosted.exchange(nullptr, std::memory_order_acq_rel))
				drain->residue.insert(drain->residue.begin(), unread->residue.begin(), unread->residue.end());
			state.drainPosted.store(std::shared_ptr<const RegistrationDrain>(std::move(drain)), std::memory_order_release);
		}

		void OnInputsTaken(const On<ScenePoint::AccumulateInputsTaken>&)
		{
			// None posted since the last pass leaves none, so no drain is checked twice.
			auto& state = SceneDiagnostics();
			state.drain = state.drainPosted.exchange(nullptr, std::memory_order_acq_rel);
		}

		void ClassifyResidue(const Scene::Coordinator& a_store)
		{
			// What the engine registered into the reflection faces under the LOD land and objects roots that is no reflection member:
			// untracked, tracked but ineligible (by reason), eligible but not bound, or bound but not in the reflection phase. The drain's
			// (the frames since the last pass took one), by pointer and the names the render thread read (a geometry the engine released
			// since is untracked here, and nothing of it is dereferenced).
			auto& state = SceneDiagnostics();
			if (!state.drain)
				return;
			const auto& tracked = V::tracked(a_store);
			const std::uint32_t sceneFrame = V::sceneFrame(a_store);
			std::scoped_lock lock(state.residueLock);
			auto& classes = state.residue;
			for (const auto& [geometry, names] : state.drain->residue) {
				const auto it = tracked.find(const_cast<RE::BSGeometry*>(geometry));
				std::size_t kind;
				if (it == tracked.end())
					kind = kResidueUntracked;
				else if (it->second.candidateReason != Ineligible::None)
					kind = static_cast<std::size_t>(it->second.candidateReason);
				else if (const std::int32_t slot = a_store.FindObject(geometry); !V::ResidentObject(a_store, slot))
					kind = kResidueUnbound;
				else
					kind = (Scene::PhasesIn(V::tables(a_store), static_cast<std::uint32_t>(slot)) & kSetReflection) ? kResidueReflection : kResidueNotReflection;
				++classes.counts[kind];
				// How long a geometry stays in the residue (distinct geometries, and those seen on 10 frames or more this window).
				if (auto& seen = classes.seen[geometry]; seen.frame != sceneFrame) {
					seen.frame = sceneFrame;
					if (++seen.frames == 10)
						++classes.persistent;
				}
				if (classes.first[kind].empty() && it != tracked.end())
					classes.first[kind] = names;
			}
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
			if (residueOn)
				ClassifyResidue(Scene::Coordinator::Get());
			if (parityOn)
				CheckRegistrations(Scene::Coordinator::Get());
		}
	}

	namespace
	{
		void OnMisfiled(const FirstPersonMisfiled& a_event)
		{
			auto& fp = SceneDiagnostics().firstPerson;
			fp.misfiled.fetch_add(1, std::memory_order_relaxed);
			fp.misfiledGeometry.store(a_event.geometry, std::memory_order_relaxed);
			fp.misfiledCategory.store(a_event.category, std::memory_order_relaxed);
		}
	}

	void InstallFirstPersonFiling()
	{
		Subscribe<FirstPersonMisfiled>(OnMisfiled);
	}

	ResidueClasses TakeResidueClasses()
	{
		auto& state = SceneDiagnostics();
		std::scoped_lock lock(state.residueLock);
		return std::exchange(state.residue, {});
	}

	void InstallRegistrations(bool a_residue, bool a_parity)
	{
		residueOn = a_residue;
		parityOn = a_parity;
		if (!residueOn && !parityOn)
			return;
		Subscribe<CaptureDrained>(OnCaptureDrained);
		Subscribe<ScenePoint::AccumulateInputsTaken>(OnInputsTaken);
		Subscribe<ScenePoint::JoinsBegin>(OnJoinsBegin);
	}
}
