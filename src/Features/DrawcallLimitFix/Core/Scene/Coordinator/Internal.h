#pragma once

// Coordinator's implementation, shared by the files of this folder only (Coordinator.h is the interface): helpers, the event batch,
// the shard and timing scopes. Coordinator thread (the scene lane) and the pass's pool workers.

#include "Features/DrawcallLimitFix/Core/Scene/Coordinator/Coordinator.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Extensions/EngineEvents.h"
#include "Features/DrawcallLimitFix/Core/Engine/Walkers.h"
#include "Features/DrawcallLimitFix/Core/Shading/FrameGlobals.h"
#include "Features/DrawcallLimitFix/Core/Runtime/EventQueue.h"
#include "Features/DrawcallLimitFix/Core/Runtime/Hash.h"
#include "Features/DrawcallLimitFix/Core/Shading/LightingConstants.h"
#include "Features/DrawcallLimitFix/Core/Shading/MaterialSources.h"
#include "Features/DrawcallLimitFix/Core/Shading/MaterialWrites.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/EngineStates.h"
#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"
#include "Features/DrawcallLimitFix/Core/Gpu/GpuResources.h"
#include "Features/DrawcallLimitFix/Core/Handoff/PassCapture.h"
#include "Features/DrawcallLimitFix/Core/Engine/Capture/SceneTracker.h"
#include "Features/DrawcallLimitFix/Core/Engine/Capture/SceneEvents.h"
#include "Features/DrawcallLimitFix/Core/Scene/EntrySpace.h"
#include "Features/DrawcallLimitFix/Core/Handoff/PrimaryCull.h"
#include "Features/DrawcallLimitFix/Core/Handoff/ShadowViews.h"
#include "Features/DrawcallLimitFix/Core/Shading/VertexInput.h"
#include <bit>
#include <cassert>
#include <xbyak/xbyak.h>
#include <chrono>
#include <exception>
#include <optional>
#include "Features/DrawcallLimitFix/Core/Engine/EngineReleases.h"
#include "Features/Skylighting.h"
#include "State.h"
#include "Utils/ExternalEmittance.h"
#include "Features/DrawcallLimitFix/Diagnostics/Policy.h"
#include "Features/DrawcallLimitFix/Diagnostics/Observation.h"
#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"
#include "Features/DrawcallLimitFix/Diagnostics/Scene/Events.h"
#include "Features/DrawcallLimitFix/Diagnostics/Reports/Scene.h"

namespace DCLF
{
	namespace Scene
	{
		using Engine::kMaxParentDepth;
		// An event's walk of a subtree (ForEachLive, ForEachMirror): bounded, and whatever it leaves out the frame's walk still covers.
		constexpr std::size_t kMaxEventNodes = 4096;
		constexpr std::size_t kValidationSlice = 256;
		constexpr std::uint32_t kIndexFormatR16 = 57;  // DXGI_FORMAT_R16_UINT
		constexpr std::uint32_t kSpecularBit = 0x200;  // pass descriptor Specular
		constexpr std::uint32_t kTechniqueEnvmap = 1;

		inline void StoreTransform(const RE::NiTransform& a_transform, float (&a_out)[12])
		{
			// Row-major 3x4: rotation scaled, translation in the last column.
			const auto& r = a_transform.rotate.entry;
			const float s = a_transform.scale;
			for (int row = 0; row < 3; ++row) {
				a_out[row * 4 + 0] = r[row][0] * s;
				a_out[row * 4 + 1] = r[row][1] * s;
				a_out[row * 4 + 2] = r[row][2] * s;
			}
			a_out[3] = a_transform.translate.x;
			a_out[7] = a_transform.translate.y;
			a_out[11] = a_transform.translate.z;
		}

		inline bool IsHidden(const RE::NiAVObject* a_object) { return Engine::IsHidden(a_object); }

		/**
		 * @brief A skin's palette rows: three a bone, as the engine's palette update (AE FUN_140e4ff90, which FrameValues runs) sizes
		 * it - numMatrices is the skin data's bone count (+0x58), copied by that update.
		 */
		inline std::uint32_t SkinRowsOf(const RE::NiSkinInstance& a_skin)
		{
			const auto* data = a_skin.skinData.get();
			return data ? data->GetBoneCount() * 3 : 0u;
		}


		/** @brief Takes a geometry off a dependents list; true when others remain under the key. */
		template <class Map, class Key>
		bool Unlist(Map& a_map, Key a_key, RE::BSGeometry* a_geometry)
		{
			const auto it = a_map.find(a_key);
			if (it == a_map.end())
				return false;
			auto& list = it->second;
			if (const auto at = std::find(list.begin(), list.end(), a_geometry); at != list.end()) {
				*at = list.back();
				list.pop_back();
			}
			if (list.empty()) {
				a_map.erase(it);
				return false;
			}
			return true;
		}

		/** @brief Puts a geometry on a dependents list whose positions a_at keeps (Coordinator::DependentsAt). */
		template <class Map, class At, class Key>
		void List(Map& a_map, At& a_at, Key a_key, RE::BSGeometry* a_geometry)
		{
			auto& list = a_map[a_key];
			// One position a (key, geometry): a geometry is listed once under a key (its keys are distinct objects).
			assert(!a_at.contains(std::pair<const void*, const RE::BSGeometry*>{ a_key, a_geometry }));
			a_at.insert_or_assign(std::pair<const void*, const RE::BSGeometry*>{ a_key, a_geometry }, static_cast<std::uint32_t>(list.size()));
			list.push_back(a_geometry);
		}

		/** @brief Unlist by a_at's position (the last swapped into it, its position kept); true when others remain under the key. */
		template <class Map, class At, class Key>
		bool Unlist(Map& a_map, At& a_at, Key a_key, RE::BSGeometry* a_geometry)
		{
			const auto at = a_at.find(std::pair<const void*, const RE::BSGeometry*>{ a_key, a_geometry });
			const auto it = a_map.find(a_key);
			if (it == a_map.end()) {
				if (at != a_at.end())
					a_at.erase(at);
				return false;
			}
			auto& list = it->second;
			if (at != a_at.end()) {
				const std::uint32_t index = at->second;
				a_at.erase(at);
				assert(index < list.size() && list[index] == a_geometry);
				if (index < list.size()) {
					if (index + 1 != list.size()) {
						list[index] = list.back();
						a_at.insert_or_assign(std::pair<const void*, const RE::BSGeometry*>{ a_key, list[index] }, index);
					}
					list.pop_back();
				}
			}
			if (list.empty()) {
				a_map.erase(it);
				return false;
			}
			return true;
		}

		inline bool SameTransform(const RE::NiTransform& a_lhs, const RE::NiTransform& a_rhs)
		{
			return std::memcmp(&a_lhs, &a_rhs, sizeof(RE::NiTransform)) == 0;
		}

		/** @brief A rigid body on this node whose motion type is not kFixed: Havok moves it. */
		inline bool NonFixedBody(const RE::NiAVObject& a_object)
		{
			auto* collision = a_object.collisionObject.get();
			auto* ni = collision ? collision->AsBhkNiCollisionObject() : nullptr;
			if (!ni || !ni->body || !ni->body->GetRTTI() || !std::strstr(ni->body->GetRTTI()->GetName(), "RigidBody"))
				return false;
			auto* entity = static_cast<RE::hkpEntity*>(static_cast<RE::hkReferencedObject*>(ni->body->referencedObject.get()));
			return entity && entity->motion.type.get() != RE::hkpMotion::MotionType::kFixed;
		}
		inline ObjectRecord FreeObjectRecord()
		{
			ObjectRecord record{};
			record.flags = kObjectFree | kObjectNoBindings | kObjectNoShadow;
			return record;
		}

		// The frame's globals (kPSFrameGeometry, kVSEyePosition: LightingConstants.h). EyePosition is written only for some
		// passes (WritesEyePosition); every other pass leaves whatever the constant buffer last held there, which no draw
		// of it reads.
		// What ObjectGeometryConstants writes over the pipeline's block for every object (or every object of the pipeline's
		// kind): World, PreviousWorld, LandBlendParams, TreeParams, WindTimers, TextureProj; the light assignment Light
		// Limit Fix never reads, MaterialData, EmitColor, ShadowLightMaskSelect, ProjectedUVParams 1-3, SSRParams.
		constexpr std::uint64_t kObjectGeometryVS = (1ull << 0) | (1ull << 1) | (1ull << 3) | (1ull << 4) | (1ull << 5) | (1ull << 6);
		constexpr std::uint64_t kObjectGeometryPS = (1ull << 0) | (1ull << 1) | (1ull << 2) | (1ull << 7) | (1ull << 8) | (1ull << 10) | (1ull << 12) |
		                                            (1ull << 13) | (1ull << 14) | (1ull << 16);

		/** @brief The frame variables of a_sample written over a_out's where both write them; true when any differed. */
		inline bool CopyFrameGeometry(const GeometryConstants& a_sample, GeometryConstants& a_out)
		{
			const auto& layout = LightingPSLayout();
			bool changed = false;
			for (const auto v : kPSFrameGeometry) {
				const std::uint32_t offset = layout.offset[v];
				const std::size_t bytes = layout.size[v] * sizeof(float);
				if (!a_out.ps.Written(offset) || !a_sample.ps.Written(offset) || std::memcmp(&a_out.ps.floats[offset], &a_sample.ps.floats[offset], bytes) == 0)
					continue;
				std::memcpy(&a_out.ps.floats[offset], &a_sample.ps.floats[offset], bytes);
				changed = true;
			}
			return changed;
		}

		/** @brief a_sample's EyePosition over a_out's where both write it; true when it differed. */
		inline bool CopyEyePosition(const GeometryConstants& a_sample, GeometryConstants& a_out)
		{
			const auto& layout = LightingVSLayout();
			const std::uint32_t offset = layout.offset[kVSEyePosition];
			const std::size_t bytes = layout.size[kVSEyePosition] * sizeof(float);
			if (!a_out.vs.Written(offset) || !a_sample.vs.Written(offset) || std::memcmp(&a_out.vs.floats[offset], &a_sample.vs.floats[offset], bytes) == 0)
				return false;
			std::memcpy(&a_out.vs.floats[offset], &a_sample.vs.floats[offset], bytes);
			return true;
		}
		inline float GlobalFloatAt(const REL::Relocation<std::uintptr_t>& a_at)
		{
			return *reinterpret_cast<const float*>(a_at.address());
		}
		// Grows each column of a table by one slot (Tables::GeometryColumns and the like).
		inline constexpr auto kGrowColumn = [](auto& a_column, auto&&... a_initial) { a_column.emplace_back(a_initial...); };


		/**
		 * @brief Whether two technique samples feed the technique rows the same (bitwise): everything EvaluateTechnique reads but
		 * the fog, which a row keeps from its making (KeepTechniqueFog; the draws read the frame's, FrameCapture::fog).
		 */
		inline bool SameTechniqueWitness(const TechniqueInputs& a_a, const TechniqueInputs& a_b)
		{
			auto same = [](const auto& a, const auto& b) { return std::memcmp(&a, &b, sizeof(a)) == 0; };
			return same(a_a.highDetailRange, a_b.highDetailRange) && a_a.shadowMask == a_b.shadowMask && a_a.shadowMaskFilter == a_b.shadowMaskFilter &&
			       a_a.shadowMaskSized == a_b.shadowMaskSized && same(a_a.shadowMaskInverseSize, a_b.shadowMaskInverseSize) && same(a_a.colourClamp, a_b.colourClamp);
		}

		inline bool SameTechniqueFloats(const TechniqueConstants& a_a, const TechniqueConstants& a_b) { return a_a.vs.SameBits(a_b.vs) && a_a.ps.SameBits(a_b.ps); }
		inline bool SameTechniqueBindings(const TechniqueConstants& a_a, const TechniqueConstants& a_b)
		{
			return a_a.filterModes == a_b.filterModes && a_a.shadowMask == a_b.shadowMask && a_a.shadowMaskTexture == a_b.shadowMaskTexture;
		}

		/** @brief CS_DCLF_FANOUT, read once: 0 interleaved, 1 evaluated then merged on the lane, 2 (unset or anything else) on the pool. */
		std::uint32_t ConfiguredFanout();


		// Only columns the accumulator patch below can change. It never writes placement, geometry,
		// bones, wetness or shadow inputs. Extras contents the patch rewrites in a kept block are noted by the patch itself
		// (kChangeExtras: an offset change is noted here). Skin-mask changes are noted before the patch.
		struct AccumulateSnapshot
		{
			std::uint32_t flags, material, pipeline, drawPipeline, extras;
			ObjectAncestorValue ancestorValue;
			float fade;
			std::uint8_t resident;

			AccumulateSnapshot(const Coordinator::Tables& a_tables, std::uint32_t a_slot) :
				flags(a_tables.objects[a_slot].flags), material(a_tables.objects[a_slot].materialIndex),
				pipeline(a_tables.objects[a_slot].pipelineIndex), drawPipeline(a_tables.draws[a_slot].pipelineIndex),
				extras(a_tables.extraOffset[a_slot]), ancestorValue(a_tables.ancestorValue[a_slot]), fade(a_tables.fadeDistance[a_slot]),
				resident(a_tables.residentSlot[a_slot])
			{}

			void NoteWrite(Coordinator::Tables& a_tables, std::uint32_t a_slot) const
			{
				auto differs = [](const auto& a, const auto& b) { return std::memcmp(&a, &b, sizeof(a)) != 0; };
				const auto& object = a_tables.objects[a_slot];
				std::uint32_t causes = 0;
				if (flags != object.flags || material != object.materialIndex || pipeline != object.pipelineIndex ||
					drawPipeline != a_tables.draws[a_slot].pipelineIndex || differs(fade, a_tables.fadeDistance[a_slot]))
					causes |= kChangeBindings;
				if (differs(ancestorValue, a_tables.ancestorValue[a_slot]))
					causes |= kChangeAncestorValue;
				if (extras != a_tables.extraOffset[a_slot])
					causes |= kChangeExtras;
				if (resident != a_tables.residentSlot[a_slot])
					causes |= kChangeMembership;
				a_tables.NoteChange(a_slot, causes);
			}
		};
	}

	// The folder's files use Scene's names unqualified.
	using namespace Scene;

	/**
	 * @brief One scene pass's events (Coordinator::CollectEvents, on the coordinator, each queue drained up to where it stood at
	 * the pass's start), oldest first per queue, applied by the same pass (ApplyEvents). Applied, it is handed back whole: its tracker
	 * events hold attached subtrees, released at Present.
	 */
	struct Scene::Coordinator::EventBatch
	{
		SceneTracker::Event* head = nullptr;  // SceneTracker's attach and detach events (and the render thread's mirror-only captures)
		SceneTracker::Event* tail = nullptr;
		std::vector<const void*> properties;
		std::vector<RE::NiPointer<RE::NiAVObject>> nodes;
		std::vector<SwitchEvent> switches;
		// The features' engine channels' keys, by channel (Extensions::RegisteredEngineChannels' order).
		std::array<std::vector<const void*>, Extensions::kEngineChannelSlots> channelKeys;
		// The oldest stamp of its tracker events (SceneTracker::Event::stampNs, stampFrame; 0: none), for the latencies.
		std::uint64_t oldestNs = 0;
		std::uint32_t oldestFrame = 0;

		EventBatch() = default;
		EventBatch(const EventBatch&) = delete;
		EventBatch& operator=(const EventBatch&) = delete;
		~EventBatch()
		{
			SceneTracker::FreeEvents(head);
		}

		void Append(SceneTracker::Event* a_events)
		{
			if (!a_events)
				return;
			(tail ? tail->next : head) = a_events;
			for (tail = a_events; tail->next;)
				tail = tail->next;
		}
	};

	/**
	 * @brief A pass's batch (its tracker events' pins and captures, its nodes' and switches' references) handed to the render thread
	 * (EngineReleases::PushOwner, released at Present) when the pass unwinds while it holds it: a throw never destroys it on the pump, where the
	 * engine's destructors must not run. A batch passed on (retired, posted) leaves it null and the guard does nothing.
	 */
	struct Scene::Coordinator::BatchUnwindGuard
	{
		BatchUnwindGuard(Coordinator& a_store, std::shared_ptr<EventBatch>& a_batch) :
			store(a_store), batch(a_batch) {}
		~BatchUnwindGuard()
		{
			if (batch && std::uncaught_exceptions() > exceptions)
				EngineReleases::PushOwner(std::move(batch));
		}
		BatchUnwindGuard(const BatchUnwindGuard&) = delete;
		BatchUnwindGuard& operator=(const BatchUnwindGuard&) = delete;

	private:
		Coordinator& store;
		std::shared_ptr<EventBatch>& batch;
		int exceptions = std::uncaught_exceptions();
	};


	/** @brief Adds the time from construction to destruction to a scene part's sum and its frame's time (ms). */
	struct ScenePartScope
	{
		double& sum;
		double& frame;
		std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();

		ScenePartScope(double& a_sum, double& a_frame) :
			sum(a_sum), frame(a_frame) {}
		~ScenePartScope()
		{
			const double elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
			sum += elapsed;
			frame += elapsed;
		}
		ScenePartScope(const ScenePartScope&) = delete;
		ScenePartScope& operator=(const ScenePartScope&) = delete;
	};

	/**
	 * @brief What a scene pass's evaluations read on whichever thread runs them. Always the pass's snapshot of the toggles and of
	 * the classify inputs (Coordinator::passSnapshot), and the chunk's mirror-read counters when given. On a pool thread (a_worker) also the
	 * pass's frame globals (FrameGlobals::Current is per thread) and the scene work's flags (sceneWorkThread, EngineReadWindow::sceneWork),
	 * as the sun candidates' verdicts (DirtyVerdicts) and the fan-outs take them: an engine read from there without a lease is counted. One per chunk; the
	 * pass's own thread holds one (not a worker's) over a whole round, its merges included, so both halves read the same snapshot.
	 */
	struct Scene::Coordinator::ShardScope
	{
		ShardScope(const Coordinator& a_store, bool a_worker, MirrorReadStats* a_mirror = nullptr) :
			toggles(&a_store.passSnapshot.toggles),
			inputs(&a_store.passSnapshot.inputs),
			worker(a_worker),
			sink(std::exchange(mirrorReadSink, a_mirror ? a_mirror : mirrorReadSink))
		{
			if (!worker)
				return;
			globalsScope.emplace(a_store.passGlobals);
			marked = std::exchange(sceneWorkThread, true);
			engineMarked = std::exchange(EngineReadWindow::sceneWork, true);
		}
		~ShardScope()
		{
			if (worker) {
				EngineReadWindow::sceneWork = engineMarked;
				sceneWorkThread = marked;
			}
			mirrorReadSink = sink;
		}
		ShardScope(const ShardScope&) = delete;
		ShardScope& operator=(const ShardScope&) = delete;

	private:
		ToggleSnapshotScope toggles;
		Extensions::ClassifyInputsScope inputs;
		std::optional<FrameGlobals::Scope> globalsScope;
		bool worker = false;
		bool marked = false;
		bool engineMarked = false;
		MirrorReadStats* sink = nullptr;
	};

	// One scene sub-zone (ScenePart) for the rest of the enclosing block: a Tracy zone and the part's sum. One per block.
#define DCLF_SCENE_PART(a_part, a_zone) \
	ZoneScopedN(a_zone);                \
	ScenePartScope scenePartScope(stats.scenePartMs[static_cast<std::size_t>(ScenePart::a_part)], \
		stats.scenePartFrameMs[static_cast<std::size_t>(ScenePart::a_part)])

	/**
	 * @brief Attributes the time since the last call to one BuildPart.
	 *
	 * Inert unless CS_DCLF_PROFILE=1: with profiling off `parts` is null and Add() is a null test, so the loop makes no clock calls.
	 * Timing a loop from inside it perturbs what it measures, so the profiled and unprofiled totals are both reported and the
	 * difference is the instrument's own cost.
	 */
	struct PartTimer
	{
		std::array<double, static_cast<std::size_t>(BuildPart::Count)>* parts = nullptr;
		std::array<double, static_cast<std::size_t>(BuildPart::Count)>* phaseParts = nullptr;
		std::chrono::steady_clock::time_point last;

		explicit PartTimer(std::array<double, static_cast<std::size_t>(BuildPart::Count)>& a_parts,
			std::array<double, static_cast<std::size_t>(BuildPart::Count)>* a_phaseParts = nullptr)
		{
			if (Diagnostics::Observe().profile) {
				parts = &a_parts;
				phaseParts = a_phaseParts;
				last = std::chrono::steady_clock::now();
			}
		}

		void Add(BuildPart a_part)
		{
			if (!parts)
				return;
			const auto now = std::chrono::steady_clock::now();
			const double elapsed = std::chrono::duration<double, std::milli>(now - last).count();
			(*parts)[static_cast<std::size_t>(a_part)] += elapsed;
			if (phaseParts)
				(*phaseParts)[static_cast<std::size_t>(a_part)] += elapsed;
			last = now;
		}
	};
}
