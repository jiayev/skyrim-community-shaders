// The actor probe (dclf:actor-probe): at a scene pass's end, per actor in the tracked set, how DCLF binds its geometry - which
// entries hold records, the fade the binding was derived under against the fade now, forward or deferred descriptors, the joins'
// verdicts - and the entries whose binding disagrees with their state. The scene work's own data, read on its lane.

#include "Internal.h"

#include "Features/DrawcallLimitFix/Core/Extensions/RootStates.h"
#include "Features/DrawcallLimitFix/Diagnostics/ShadowMaskProbe.h"

#include <map>
#include <mutex>
#include <string>

namespace DCLF::Scene
{
	namespace
	{
		std::mutex probeMutex;
		std::vector<std::string> probeLabels;
		std::atomic<bool> probeAsked{ false };
	}

	void Coordinator::RequestActorProbe(std::string a_label)
	{
		{
			std::lock_guard lock(probeMutex);
			probeLabels.push_back(std::move(a_label));
		}
		probeAsked.store(true, std::memory_order_release);
		WakeScenePass(SceneWake::FrameInput);
	}

	void Coordinator::RunActorProbeIfAsked()
	{
		if (!probeAsked.exchange(false, std::memory_order_acq_rel))
			return;
		std::vector<std::string> labels;
		{
			std::lock_guard lock(probeMutex);
			labels.swap(probeLabels);
		}
		const bool forwardOn = Extensions::ObjectClassOn(ActiveToggles(), Api::ObjectClass::Forward);
		struct ActorSummary
		{
			std::uint32_t entries = 0, records = 0, noBindings = 0, faces = 0, forward = 0, fadingNow = 0, hint9 = 0, invalid = 0;
			float minFade = 1.0f;
			std::vector<std::string> wrong;
			std::uint32_t armed = 0;
			std::uint32_t badBounds = 0;
			std::vector<std::string> parts;  // every bound entry, listed while the actor fades
		};
		std::vector<std::pair<std::uint32_t, std::string>> shadowProbe;
		std::map<std::uint32_t, ActorSummary> actors;
		std::uint32_t wrongTotal = 0;
		// The root row's fadeAmount (what FadeStateCS steps amountFade toward) against the node's and the mirror's.
		const auto rootList = Extensions::RootListOf(tables);
		for (auto& [geometry, entry] : tracked) {
			const bool face = entry.entryClass == Api::EntryClass::FaceShape;
			if (!face && entry.owner.ownerClass != Api::EntryClass::ActorOwned)
				continue;
			const std::uint32_t formId = entry.ownerReference ? entry.ownerReference->GetFormID() : 0u;
			auto& actor = actors[formId];
			if (++actor.entries == 1 && entry.ownerReference) {
				if (auto* root = entry.ownerReference->Get3D(); root && rootList.index) {
					const float nodeAmount = Engine::At<float>(root, 0x100);
					const auto* rec = mirror.Node(root);
					const auto it = rootList.index->find(root);
					const bool listed = it != rootList.index->end() && it->second < rootList.rows.size();
					const float rowAmount = listed ? rootList.rows[it->second].fadeAmount : -1.0f;
					if ((listed && rowAmount != nodeAmount) || (rec && rec->fadeAmount != nodeAmount) || nodeAmount != 1.0f) {
						++wrongTotal;
						actor.wrong.push_back(std::format("fadeAmount differs: node {:.3f} (+0x140 {:.3f}, +0x130 {:.3f}), mirror {}, root row {} (listed {}, gen {})", nodeAmount,
							Engine::At<float>(root, 0x140), Engine::At<float>(root, 0x130), rec ? std::format("{:.3f}", rec->fadeAmount) : std::string("none"),
							listed ? std::format("{:.3f}", rowAmount) : std::string("-"), listed, listed ? rootList.rows[it->second].generation : 0u));
					}
				}
			}
			actor.faces += face ? 1u : 0u;
			// The live bound (FrameValues samples it into the placement row: the cull's and the shadow lights' test) against the
			// geometry's own transform and model bound and its actor's position (engine reads, diagnostics only).
			{
				const auto& wb = geometry->worldBound;
				const auto& t = geometry->world.translate;
				const auto& mb = geometry->GetModelData().modelBound;
				const RE::NiPoint3 owner = entry.ownerReference ? entry.ownerReference->data.location : t;
				const float fromOwner = wb.center.GetDistance(owner);
				if (!(fromOwner < 400.0f) || !(wb.radius < 400.0f)) {
					++actor.badBounds;
					++wrongTotal;
					if (actor.wrong.size() < 6)
						actor.wrong.push_back(std::format("bound far from its actor '{}' (type {}, face {}, slot {}): world bound ({:.0f} {:.0f} {:.0f}) r {:.0f}, {:.0f} from the actor at ({:.0f} {:.0f} {:.0f}); "
														  "world translate ({:.0f} {:.0f} {:.0f}) scale {:.2f}; model bound ({:.1f} {:.1f} {:.1f}) r {:.1f}; app culled {}",
							geometry->name.c_str() ? geometry->name.c_str() : "", static_cast<std::uint32_t>(geometry->GetType().underlying()), face, entry.slot, wb.center.x,
							wb.center.y, wb.center.z, wb.radius, fromOwner, owner.x, owner.y, owner.z, t.x, t.y, t.z, geometry->world.scale, mb.center.x, mb.center.y,
							mb.center.z, mb.radius, geometry->GetAppCulled()));
				}
			}
			const auto leaf = mirror.Leaf(geometry);
			const auto* record = leaf.property;
			const auto* fadeNode = leaf.fadeNode;
			const float fade = fadeNode ? fadeNode->currentFade : 1.0f;
			actor.minFade = (std::min)(actor.minFade, fade);
			const bool fadingNow = forwardOn && BlendFading(leaf, record);
			actor.fadingNow += fadingNow ? 1u : 0u;
			const bool hasSlot = entry.slot != kNoObjectSlot && entry.slot < tables.objects.size();
			if (!hasSlot)
				continue;
			++actor.records;
			const auto& object = tables.objects[entry.slot];
			const bool bound = !(object.flags & kObjectNoBindings);
			actor.noBindings += bound ? 0u : 1u;
			const auto& derived = entry.derived;
			if (!derived.valid) {
				actor.invalid += bound ? 1u : 0u;
				continue;
			}
			const auto& d = derived.descriptors;
			actor.forward += d.forward ? 1u : 0u;
			actor.hint9 += derived.hint == kFadingHint ? 1u : 0u;
			const char* name = geometry->name.c_str();
			const auto describe = [&](const char* a_what) {
				++wrongTotal;
				if (actor.wrong.size() < 6)
					actor.wrong.push_back(std::format("{} '{}' (slot {}, technique {}, hint {}, pixel {:08X}, forward {}, fade {:.3f} door {}, alpha {:.2f}, flags {:08X}, bound {})",
						a_what, name ? name : "", entry.slot, d.technique, derived.hint, d.pixel, d.forward, fade, fadeNode ? fadeNode->fadeDoor : 0,
						record ? record->materialAlpha : 1.0f, object.flags, bound));
			};
			if (!bound)
				continue;
			actor.parts.push_back(std::format("'{}' face {} technique {} hint {} forward {} pass {:08X} pixel {:08X} property flags {:X} blend {} alpha {:.2f} fading now {}", name ? name : "",
				face, d.technique, derived.hint, d.forward, d.pass, d.pixel, record ? record->flags : 0ull, leaf.AlphaBlending(), record ? record->materialAlpha : 1.0f, fadingNow));
			// Up to twelve bound entries an actor for the CPU cull's shadow light verdicts.
			if (actor.armed < 12) {
				++actor.armed;
				shadowProbe.emplace_back(entry.slot, std::format("actor {:08X} '{}'", formId, name ? name : ""));
			}
			const bool blendedNow = leaf.AlphaBlending() || (record && record->materialAlpha < 1.0f);
			if (d.forward != ((object.flags & kObjectForward) != 0))
				describe(d.forward ? "forward derivation but the record is not forward" : "record forward but the derivation is not");
			else if ((derived.hint == kFadingHint) != fadingNow)
				describe(fadingNow ? "fading but bound opaque" : "bound fading but settled");
			else if (!d.forward && !(d.pixel & kLightingPixelDeferred) && !d.port)
				describe("deferred draw without the Deferred bit");
			else if (d.forward && !fadingNow && !blendedNow && !d.refraction)
				describe("forward but opaque");
			else if (derived.pipelineSlot != object.pipelineIndex || derived.materialSlot != object.materialIndex)
				describe("record's slots differ from the derivation's");
		}
		Diagnostics::ShadowMaskProbe::Arm(std::move(shadowProbe));
		std::uint32_t inFade = 0;
		for (const auto& [id, a] : actors)
			inFade += a.minFade < 1.0f ? 1u : 0u;
		std::string label;
		for (const auto& l : labels)
			label += (label.empty() ? "" : ",") + l;
		logger::info("[DCLF] actor probe '{}' (scene frame {}, {}): {} actors, {} fading, {} entries disagree with their state", label, GetFrame(), frameInterior ? "interior" : "exterior",
			actors.size(), inFade, wrongTotal);
		for (const auto& [id, a] : actors) {
			if (a.wrong.empty() && a.minFade >= 1.0f)
				continue;
			logger::info("[DCLF]   actor {:08X}: {} entries ({} face), {} records ({} without bindings, {} without a derivation), {} forward, {} bound fading, {} fading now, lowest fade {:.3f}, {} bounds far off",
				id, a.entries, a.faces, a.records, a.noBindings, a.invalid, a.forward, a.hint9, a.fadingNow, a.minFade, a.badBounds);
			for (const auto& w : a.wrong)
				logger::info("[DCLF]     {}", w);
			if (a.minFade < 1.0f)
				for (const auto& part : a.parts)
					logger::info("[DCLF]     part {}", part);
		}
	}
}
