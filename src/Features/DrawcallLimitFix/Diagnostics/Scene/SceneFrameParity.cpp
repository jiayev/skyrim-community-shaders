// CS_DCLF_PERSISTENT_PARITY at Prepass (render thread; the scene work runs inline under the parities): the used sets against the installed
// records, the technique rows against the engine's values now, the extras rows against the engine's routines, and every drawn slot's
// shading against the frame's rows.

#include "SceneObservers.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"
#include "SceneState.h"

#include "Features/DrawcallLimitFix/Core/Gpu/FrameValues.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/ProgramFamilySlots.h"
#include "Features/DrawcallLimitFix/Integrations/AdvancedSkin/Diagnostics/WetnessParity.h"
#include "Features/DrawcallLimitFix/Core/Shading/ConstantEvaluator.h"
#include "Features/DrawcallLimitFix/Core/Scene/Coordinator/Internal.h"

namespace DCLF::Diagnostics
{
	namespace
	{
		using V = SceneView;

		void CheckMaterialFrame()
		{
			const auto& view = Scene::FrameView::Get().GetTables();
			const std::uint32_t frame = V::frame();
			// The installed tables' used sets against the slots their bound records name.
			if (!ParityDue(frame, 45))
				return;
			// The used sets are kept by membership changes. On a parity frame compare them against the slots the bound records
			// name, before trusting their absence to skip descriptor/import work.
			{
				std::vector<std::uint64_t> pipelines(view.usedPipelineBits.size()), materials(view.usedMaterialBits.size());
				auto set = [](std::vector<std::uint64_t>& a_bits, std::uint32_t a_slot) {
					if (a_bits.size() <= a_slot / 64)
						a_bits.resize(a_slot / 64 + 1, 0);
					a_bits[a_slot / 64] |= 1ull << (a_slot % 64);
				};
				for (std::uint32_t o = 0; o < view.objects.size(); ++o)
					if (!(view.objects[o].flags & (kObjectNoBindings | kObjectFree))) {
						set(pipelines, view.objects[o].pipelineIndex);
						set(materials, view.objects[o].materialIndex);
					}
				auto compare = [&](std::vector<std::uint64_t> a_named, const std::vector<std::uint64_t>& a_used, const char* a_name) {
					a_named.resize(std::max(a_named.size(), a_used.size()), 0);
					std::uint32_t missing = 0, extra = 0;
					for (std::size_t word = 0; word < a_named.size(); ++word) {
						const std::uint64_t used = word < a_used.size() ? a_used[word] : 0;
						missing += static_cast<std::uint32_t>(std::popcount(a_named[word] & ~used));
						extra += static_cast<std::uint32_t>(std::popcount(used & ~a_named[word]));
					}
					if (missing || extra)
						logger::warn("[DCLF] {} used set differs from the bound records at frame {}: {} named but not used, {} used but not named", a_name, frame,
							missing, extra);
				};
				compare(std::move(pipelines), view.usedPipelineBits, "pipeline");
				compare(std::move(materials), view.usedMaterialBits, "material");
			}
		}

		void CheckTechniqueRows()
		{
			// Each technique row the frame draws (the frame's tables: the coordinator's rows), once, against an evaluation from the engine's
			// values now. A row differs late when the live sample's witness moved since the row's write (a change reaches the draws with the
			// next publication); otherwise it differs. In the order the pipeline loop visits them (the used pipelines).
			const std::uint32_t frame = V::frame();
			auto& g = SceneDiagnostics().techniques;
			const bool parityFrame = ParityDue(frame, 30);
			g.checks += parityFrame ? 1u : 0u;
			if (!parityFrame)
				return;
			const auto& view = Scene::FrameView::Get().GetTables();
			const TechniqueInputs techniqueLive = SampleTechniqueInputs();
			std::vector<std::uint8_t> techniqueChecked(view.techniqueConstants.size(), 0);
			for (std::size_t word = 0; word < view.usedPipelineBits.size(); ++word) {
				for (std::uint64_t remaining = view.usedPipelineBits[word]; remaining; remaining &= remaining - 1) {
					const std::uint32_t i = static_cast<std::uint32_t>(word * 64 + std::countr_zero(remaining));
					if (i >= view.pipelines.size())
						continue;
					// A program family's key (the Effect port's): its technique row is the plain technique's (its family's shader reads none of
					// it: Effect.hlsl, DCLF_BINDLESS): the check leaves it out.
					const bool familyKey = KeyFamily(view.pipelines[i].rasterFlags) != Api::ProgramFamily::Count;
					if (const std::uint32_t r = i < view.pipelineTechnique.size() ? view.pipelineTechnique[i] : ~0u;
						!familyKey &&r < techniqueChecked.size() && !techniqueChecked[r] && view.techniqueConstants[r].valid) {
						techniqueChecked[r] = 1;
						const auto& row = view.techniqueConstants[r];
						TechniqueConstants reference;
						EvaluateTechnique(view.pipelines[i].passDescriptor, techniqueLive, reference);
						KeepTechniqueFog(row.value, reference);
						++g.techniquesChecked;
						if (!SameTechniqueFloats(reference, row.value) || !SameTechniqueBindings(reference, row.value)) {
							if (!SameTechniqueWitness(row.inputs, techniqueLive)) {
								++g.techniquesLate;
							} else if (g.techniquesDiffer++ == 0) {
								g.techniqueFirst = fmt::format("row {} (key {:#x}, pass {:08X}): {}{}", r, view.techniqueKeys[r], view.pipelines[i].passDescriptor,
									SameTechniqueFloats(reference, row.value) ? "" : "floats ", SameTechniqueBindings(reference, row.value) ? "" : "bindings");
							}
						}
					}
				}
			}
		}

		/** @brief The rows complete as the engine's own routines make them now (render thread, under a lease). */
		bool ReferenceExtras(const Scene::Coordinator& a_store, std::uint32_t a_object, float* a_out)
		{
			EngineReadWindow::Touch("Diagnostics::ReferenceExtras");
			const auto& tables = V::tables(a_store);
			const auto& object = tables.objects[a_object];
			const auto* geometry = a_object < tables.objectGeometry.size() ? tables.objectGeometry[a_object] : nullptr;
			const auto* property = static_cast<const RE::BSLightingShaderProperty*>(a_store.SlotPropertyOf(a_object));
			if (!geometry || !property || (object.flags & kObjectNoBindings))
				return false;
			std::fill_n(a_out, kExtraRows * 4, 0.0f);
			const auto eye = globals::game::shadowState->GetRuntimeData().posAdjust.getEye();
			const auto frameInputs = SampleExtrasFrame();
			if (object.flags & kObjectLandBlend) {
				const auto* material = static_cast<const RE::BSLightingShaderMaterialLandscape*>(property->material);
				float* land = a_out + kExtraRowLandBlend * 4;
				land[0] = material ? material->landBlendParams.red : 0.0f;
				land[1] = material ? material->landBlendParams.green : 0.0f;
				land[2] = frameInputs.landBlend[0] - geometry->world.translate.x;
				land[3] = frameInputs.landBlend[1] - geometry->world.translate.y;
			}
			if (object.flags & kObjectProjectedUV) {
				// How the draw makes TextureProj, which a slot without a land blend keeps in its land blend row (WriteObjectExtras): a
				// multi-index shape's own projection, the projection alone (Envmap), or the projection onto the world matrix.
				if (!(object.flags & kObjectLandBlend)) {
					const bool multiIndex = const_cast<RE::BSGeometry*>(geometry)->GetType().get() == RE::BSGeometry::Type::kMultiIndexTriShape;
					const std::uint32_t technique = (tables.pipelines[object.pipelineIndex].passDescriptor >> 24) & 0x3f;
					a_out[kExtraRowLandBlend * 4] = multiIndex ? kTextureProjShape : technique == 1 ? kTextureProjProjection : kTextureProjWorld;
				}
				// The texture matrix as SetupGeometry builds it for a ProjectedUV pass (engine notes), through the engine's own routines:
				// for every technique but Envmap the projection multiplied onto the geometry's world matrix, converted the same way and with
				// posAdjust added back.
				using ToMatrix = void (*)(float*, const RE::NiTransform*);
				using Multiply = void* (*)(float*, const float*, const float*);
				static const REL::Relocation<ToMatrix> toMatrix{ REL::Offset(Engine::Address::kTransformToMatrix) };
				static const REL::Relocation<Multiply> multiply{ REL::Offset(Engine::Address::kMatrixMultiply) };
				const float* p = frameInputs.projection;
				float m[16];
				const std::uint32_t technique = (tables.pipelines[object.pipelineIndex].passDescriptor >> 24) & 0x3f;
				if (technique == 1) {
					std::memcpy(m, p, sizeof(m));
				} else {
					float w[16];
					toMatrix(w, &geometry->world);
					w[12] += eye.x;
					w[13] += eye.y;
					w[14] += eye.z;
					multiply(m, w, p);
				}
				float* proj = a_out + kExtraRowTextureProj * 4;
				const auto* multiIndex = const_cast<RE::BSGeometry*>(geometry)->GetType().get() == RE::BSGeometry::Type::kMultiIndexTriShape ?
				                             &static_cast<const RE::BSMultiIndexTriShape*>(geometry)->GetMultiIndexTrishapeRuntimeData() :
				                             nullptr;
				for (std::uint32_t r = 0; r < 3; ++r)
					for (std::uint32_t c = 0; c < 4; ++c)
						proj[r * 4 + c] = multiIndex ? multiIndex->materialProjection.m[c][r] : m[c * 4 + r];
				const auto& params = multiIndex ? multiIndex->materialParams : property->projectedUVParams;
				const auto& colour = property->projectedUVColor;
				float* out = a_out + kExtraRowProjectedParams * 4;
				const float fade = 1.0f - params.alpha;
				out[0] = fade * params.red;
				out[2] = params.blue;
				out[3] = fade * params.green + params.alpha;
				if (multiIndex) {
					out[4] = multiIndex->normalDampener;
					out[5] = multiIndex->materialScale;
				} else {
					out[4] = colour.red;
					out[5] = colour.green;
					out[6] = colour.blue;
					out[7] = colour.alpha;
				}
				std::memcpy(out + 8, frameInputs.projectedGlobals, sizeof(frameInputs.projectedGlobals));
			}
			return true;
		}

		void CheckExtras(Scene::Coordinator& a_store)
		{
			// The rows complete as the draw completes them against the engine's own routines now (render thread, the parities' inline scene
			// work: the coordinator's tables). A static row a write since would change is stale with no event: the write is undone.
			auto& tables = V::RestorableTables(a_store);
			if (lodFadeEventsInstalled) {
				auto& ep = SceneDiagnostics().extras;
				// The engine's globals and routines under leases, an item each (the scene work may run beside the engine's update).
				const auto frameInputs = [] {
					const EngineReadWindow::Lease lease;
					return lease ? SampleExtrasFrame() : ExtrasFrame{};
				}();
				std::array<float, kExtraRows * 4> reference{}, completed{};
				for (std::uint32_t o = 0; o < tables.objects.size() && o < tables.objectGeometry.size(); ++o) {
					if ((tables.objects[o].flags & (kObjectFree | kObjectNoBindings)) || !(tables.objects[o].flags & (kObjectProjectedUV | kObjectLandBlend)) ||
						!tables.objectGeometry[o] || o >= tables.extraOffset.size() || tables.extraOffset[o] == kNoExtraRows)
						continue;
					float* held = &tables.extraRows[std::size_t(tables.extraOffset[o]) * 4];
					const std::array<float, kExtraRows * 4> before = [&] { std::array<float, kExtraRows * 4> copy; std::memcpy(copy.data(), held, sizeof(copy)); return copy; }();
					if (V::WriteObjectExtras(a_store, o)) {
						++ep.staleStatic;
						std::memcpy(held, before.data(), sizeof(before));
					}
					const EngineReadWindow::Lease lease;
					if (!lease || !ReferenceExtras(a_store, o, reference.data()))
						continue;
					BindlessPlacement placement;
					if (!FrameValues::SampleSlot(tables, o, placement))
						continue;
					CompleteExtras(held, tables.objects[o].flags, placement.world, frameInputs, completed.data());
					++ep.objects;
					float worst = 0.0f;
					std::size_t worstAt = 0;
					for (std::size_t c = 0; c < completed.size(); ++c)
						if (const float d = std::abs(completed[c] - reference[c]); d > worst)
							worst = d, worstAt = c;
					ep.maxDifference = std::max(ep.maxDifference, worst);
					// TextureProj's translation column: the engine subtracts the eye and adds it back, the draw does not (an ulp of the
					// world position).
					if (worst > 0.05f && ep.differ++ == 0) {
						const auto* geometry = tables.objectGeometry[o];
						ep.first = fmt::format("slot {} '{}' (flags {:#x}): differs by {} at component {} (completed {}, reference {}; mode {}, technique {})", o,
							geometry->name.c_str() ? geometry->name.c_str() : "?", tables.objects[o].flags, worst, worstAt, completed[worstAt], reference[worstAt],
							held[kExtraRowLandBlend * 4], (tables.pipelines[tables.objects[o].pipelineIndex].passDescriptor >> 24) & 0x3f);
					}
				}
			}
		}

		void CheckShading(Scene::Coordinator& a_store)
		{
			auto& sp = SceneDiagnostics().shading;
			const auto& tables = V::tables(a_store);
			// A diagnostics frame: waits for the frame's values.
			const auto* rows = FrameValues::Get().ShadingIfDone(true);
			if (!rows || !lodFadeEventsInstalled)
				return;
			++sp.checks;
			// Named for the next frame: by the walk (its events) and the accumulate phase, this frame.
			std::vector<std::uint8_t> named(tables.objects.size(), 0);
			auto items = Scene::FrameView::Get().PeekShadingItems();
			items.insert(items.end(), V::shadingNamed(Scene::Coordinator::Get()).begin(), V::shadingNamed(Scene::Coordinator::Get()).end());
			for (const auto& item : items)
				if (item.slot < named.size())
					named[item.slot] = 1;
			std::vector<std::uint32_t> changed;
			for (std::uint32_t o = 0; o < tables.objects.size() && o < tables.objectGeometry.size() && o < rows->size(); ++o) {
				if ((tables.objects[o].flags & (kObjectFree | kObjectNoBindings)) || !tables.objectGeometry[o])
					continue;
				const auto* property = a_store.SlotPropertyOf(o);
				if (!property)
					continue;
				++sp.slots;
				BindlessShading now = (*rows)[o];
				SampleShading(*property, tables.pipelines[tables.objects[o].pipelineIndex].passDescriptor, V::IsResidentSlot(a_store, o), now);
				// The feature block (the tree's wind) is the row's as the slot was named (a member's until its tree's entry), not kept current:
				// not compared.
				if (std::memcmp(&now, &(*rows)[o], offsetof(BindlessShading, featureBlock)) != 0)
					changed.push_back(o);
			}
			// A write after the walk took the events (the animation job runs the controllers alongside the render thread; the culls'
			// GetRenderPasses) is not missed: its event is queued. Those events are taken now and their dependents named for the next
			// frame, so each changed slot is named, queued or missed.
			std::vector<const void*> late;
			DrainLodFadeEvents(late);
			DrainEmittanceEvents(late);
			MaterialWrites::DrainShadingChanges(late);
			std::sort(late.begin(), late.end());
			late.erase(std::unique(late.begin(), late.end()), late.end());
			std::vector<std::uint32_t> covered;
			V::NameShadingKeys(a_store, late, &covered);
			std::sort(covered.begin(), covered.end());
			for (const std::uint32_t o : changed) {
				if (named[o]) {
					++sp.named;
					continue;
				}
				if (std::binary_search(covered.begin(), covered.end(), o)) {
					++sp.late;
					continue;
				}
				if (sp.missing++ == 0) {
					const auto* geometry = tables.objectGeometry[o];
					const auto& held = (*rows)[o];
					BindlessShading now = held;
					SampleShading(*a_store.SlotPropertyOf(o), tables.pipelines[tables.objects[o].pipelineIndex].passDescriptor,
						V::IsResidentSlot(a_store, o), now);
					sp.first = fmt::format("slot {} '{}' (flags {:#x}): material data ({} {} {}) against ({} {} {}), emit ({} {} {}) against ({} {} {}), mult {} against {}",
						o, geometry->name.c_str() ? geometry->name.c_str() : "", tables.objects[o].flags, now.shading.materialData[0], now.shading.materialData[1],
						now.shading.materialData[2], held.shading.materialData[0], held.shading.materialData[1], held.shading.materialData[2], now.shading.emitColor[0],
						now.shading.emitColor[1], now.shading.emitColor[2], held.shading.emitColor[0], held.shading.emitColor[1], held.shading.emitColor[2], now.emissiveMult,
						held.emissiveMult);
				}
			}
			// The wetness, as the frame's start captured it: Integrations/AdvancedSkin's. Against the frame's tables (the installed
			// publication's), whose membership writes are the ones the capture has: the coordinator's may hold a member a newer
			// publication brings.
			Integrations::AdvancedSkin::CheckRowWetness(Scene::FrameView::Get().GetTables(), *rows, named);
		}

		void OnPrepassBegin(const On<ScenePoint::PrepassConstantsBegin>&)
		{
			CheckMaterialFrame();
		}

		void OnPipelineConstants(const On<ScenePoint::PipelineConstantsBegin>&)
		{
			CheckTechniqueRows();
		}

		void OnPrepassEnd(const On<ScenePoint::PrepassConstantsEnd>&)
		{
			if (!ParityDue(V::frame()))
				return;
			CheckExtras(Scene::Coordinator::Get());
			CheckShading(Scene::Coordinator::Get());
		}
	}

	void InstallFrameParity()
	{
		Subscribe<ScenePoint::PrepassConstantsBegin>(OnPrepassBegin);
		Subscribe<ScenePoint::PipelineConstantsBegin>(OnPipelineConstants);
		Subscribe<ScenePoint::PrepassConstantsEnd>(OnPrepassEnd);
	}
}
