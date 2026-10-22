// CS_DCLF_MIRROR_PARITY, the records: the render thread captures a slice of the tracked objects live at the frame's start (the probe, asked
// by the coordinator once a frame), and the next pass compares it with the mirror after its events. A field that differs is
// - evented: an event without values named the object since it was last captured (a hook that carries no value yet);
// - late: the next batch names it (an update of the field, or an event without values: pushed after the drain);
// - missed: nothing named it (a writer no hook sees).
// An update names only its fields: a field it did not carry that differs is late or missed. CS_DCLF_MIRROR_WATCH=parity arms its
// watchpoints on what the check found stale.

#include "SceneObservers.h"
#include "SceneState.h"

#include "Features/DrawcallLimitFix/Diagnostics/Scene/MirrorWatch.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Registry.h"
#include "Features/DrawcallLimitFix/Core/Scene/Coordinator/Internal.h"

#include <fmt/ranges.h>

namespace DCLF::Diagnostics
{
	namespace
	{
		using V = SceneView;

		/**
		 * @brief The last probe's pending differences resolved by this batch's event keys (late or missed), then this probe against the mirror (a
		 * difference whose object an event of the probe's batch named is evented, else pending).
		 */
		void Check(const SceneMirror& a_mirror, const SceneCapture::Records& a_probe, const SceneMirror::KeySet& a_eventKeys, const SceneMirror::FieldMap& a_eventFields)
		{
			auto& m = SceneDiagnostics().mirror;
			auto& t = m.tally;
			auto& pending = m.pending;
			auto& named = m.named;
			const auto& geometries = V::mirrorGeometries(a_mirror);
			// The last probe's: an event without values since names the object, or updates its fields (late), or nothing does (missed).
			for (auto& [pendingKey, p] : pending) {
				const void* key = reinterpret_cast<const void*>(pendingKey & ~std::uintptr_t(7));
				const auto fields = a_eventFields.find(pendingKey);
				const bool late = a_eventKeys.contains(key) || (fields != a_eventFields.end() && (fields->second & p.fields) == p.fields);
				auto& into = late ? t.late[p.type] : t.missed[p.type];
				for (std::uint32_t f = 0; f < SceneMirror::kMaxFields; ++f)
					if (p.fields & (1u << f)) {
						++into[f];
						if (!late && t.firstMissedBy[p.type][f].empty()) {
							t.firstMissedBy[p.type][f] = p.what;
							// A property has no name: a geometry naming it (once per field and report).
							if (p.type == 2)
								for (const auto& [geometryKey, geometry] : geometries)
									if (geometry.property == key || geometry.layerProperty == key) {
										const auto* node = a_mirror.Node(geometryKey);
										t.firstMissedBy[p.type][f] += fmt::format(" (geometry {} '{}')", geometryKey, node && node->name ? node->name : "");
										break;
									}
						}
					}
				if (!late)
					for (std::uint32_t b = 0; b < 32; ++b)
						t.flagBitsMissed[b] += (p.flagBits >> b) & 1u;
				if (!late && p.type == 2 && (p.fields & MirrorWatch::PropertyFields()))
					m.missedProperty = key;
				if (!late && p.type == 3)
					m.missedAlpha = key;
				if (!late && t.firstMissed.empty())
					t.firstMissed = std::move(p.what);
			}
			pending.clear();
			// Only what the mirror holds (an event names many objects out of the world).
			for (const void* key : a_eventKeys)
				if (V::mirrorHolds(a_mirror, key))
					named.insert(key);
			++t.probes;
			auto compare = [&](std::uint8_t a_type, const auto& a_live, const auto* a_mirror, auto a_fields, const char* a_name, std::uint32_t a_flagBits) {
				++t.checked;
				if (!a_mirror) {
					if (t.absent++ == 0)
						t.firstAbsent = fmt::format("{} {} '{}'", SceneMirror::kTypeNames[a_type], a_live.key, a_name ? a_name : "");
					return;
				}
				const std::uint32_t differ = a_mirror->Differ(a_live);
				if (!differ)
					return;
				std::string what = fmt::format("{} {} '{}': {}", SceneMirror::kTypeNames[a_type], a_live.key, a_name ? a_name : "", a_fields(differ));
				// The values, mirror -> live, of the fields worth seeing.
				if constexpr (std::is_same_v<std::decay_t<decltype(a_live)>, SceneCapture::NodeRecord>) {
					if (differ & (SceneCapture::NodeRecord::kFadeNear | SceneCapture::NodeRecord::kFadeFar | SceneCapture::NodeRecord::kFadeType))
						what += fmt::format(" (near {} -> {}, far {} -> {}, type {} -> {}; captured frame {} thread {}, updated frame {}, probed frame {})", a_mirror->fadeNear,
							a_live.fadeNear, a_mirror->fadeFar, a_live.fadeFar, a_mirror->fadeType, a_live.fadeType, a_mirror->capturedFrame, a_mirror->thread,
							a_mirror->updatedFrame, a_live.capturedFrame);
					if (differ & SceneCapture::NodeRecord::kFlags)
						what += fmt::format(" (flags {:#x} -> {:#x})", a_mirror->flags, a_live.flags);
					if (differ & (SceneCapture::NodeRecord::kFadeCurrent | SceneCapture::NodeRecord::kFadeLevel | SceneCapture::NodeRecord::kFadeDoor))
						what += fmt::format(" (current fade {} -> {}, LOD level {} -> {}, screen door {} -> {}; captured frame {} thread {}, updated frame {}, probed frame {})",
							a_mirror->currentFade, a_live.currentFade, a_mirror->fadeLevel, a_live.fadeLevel, a_mirror->fadeDoor, a_live.fadeDoor, a_mirror->capturedFrame,
							a_mirror->thread, a_mirror->updatedFrame, a_live.capturedFrame);
					if (differ & SceneCapture::NodeRecord::kBody) {
						// The live chain NonFixedBody reads, from the probe's key (alive: the probe just read it).
						const auto* node = static_cast<const RE::NiAVObject*>(a_live.key);
						const auto* collision = node->collisionObject.get();
						const auto* ni = collision ? const_cast<RE::NiCollisionObject*>(collision)->AsBhkNiCollisionObject() : nullptr;
						const auto* body = ni ? ni->body.get() : nullptr;
						const auto* rtti = body ? const_cast<RE::bhkWorldObject*>(body)->GetRTTI() : nullptr;
						const auto* entity = body ? reinterpret_cast<const std::byte*>(body->referencedObject.get()) : nullptr;
						what += fmt::format(" (live: collision {} body {} '{}' entity {} motion {})", static_cast<const void*>(collision), static_cast<const void*>(body),
							rtti && rtti->name ? rtti->name : "", static_cast<const void*>(entity), entity ? static_cast<int>(*reinterpret_cast<const std::uint8_t*>(entity + 0x160)) : -1);
					}
					if (differ & (SceneCapture::NodeRecord::kBody | SceneCapture::NodeRecord::kFade109))
						what += fmt::format(" (body {} -> {}, +0x109 {:#x} -> {:#x}; captured frame {} thread {}, updated frame {})", a_mirror->body, a_live.body, a_mirror->fade109,
							a_live.fade109, a_mirror->capturedFrame, a_mirror->thread, a_mirror->updatedFrame);
				}
				if constexpr (std::is_same_v<std::decay_t<decltype(a_live)>, SceneCapture::AlphaRecord>)
					what += fmt::format(" (flags {:#x} -> {:#x}, threshold {} -> {})", a_mirror->flags, a_live.flags, a_mirror->threshold, a_live.threshold);
				if constexpr (std::is_same_v<std::decay_t<decltype(a_live)>, SceneCapture::PropertyRecord>) {
					if (differ & SceneCapture::PropertyRecord::kFlags)
						what += fmt::format(" (flags {:#x} -> {:#x}, bits {:#x}; captured frame {} thread {} sequence {}, last written by {} at frame {}; now frame {} sequence {})",
							a_mirror->flags, a_live.flags, a_mirror->flags ^ a_live.flags, a_mirror->capturedFrame, a_mirror->thread, a_mirror->sequence,
							a_mirror->writer == 1 ? "a capture" : a_mirror->writer == 2 ? "an update" : "?", a_mirror->writtenFrame, SceneCapture::Frame(),
							SceneCapture::NextSequence());
					if (differ & (SceneCapture::PropertyRecord::kAlphaValue | SceneCapture::PropertyRecord::kProjected | SceneCapture::PropertyRecord::kLandBlend))
						what += fmt::format(" (alpha {} -> {}, projected [{}] -> [{}], land blend [{}] -> [{}]; captured frame {} thread {}, last written by {} at frame {}; now frame {})",
							a_mirror->alpha, a_live.alpha, fmt::join(a_mirror->projected, " "), fmt::join(a_live.projected, " "), fmt::join(a_mirror->landBlend, " "),
							fmt::join(a_live.landBlend, " "), a_mirror->capturedFrame, a_mirror->thread, a_mirror->writer == 1 ? "a capture" : a_mirror->writer == 2 ? "an update" : "?",
							a_mirror->writtenFrame, SceneCapture::Frame());
				}
				if (a_type == 0 && (differ & SceneCapture::NodeRecord::kFadeCurrent) && MirrorWatch::CurrentFromParity())
					m.missedNode = a_live.key;
				if (named.contains(a_live.key)) {
					for (std::uint32_t b = 0; b < 32; ++b)
						t.flagBitsEvented[b] += (a_flagBits >> b) & 1u;
					for (std::uint32_t f = 0; f < SceneMirror::kMaxFields; ++f)
						if (differ & (1u << f))
							++t.evented[a_type][f];
					if (t.firstEvented.empty())
						t.firstEvented = what;
					return;
				}
				auto& p = pending[reinterpret_cast<std::uintptr_t>(a_live.key) | a_type];
				p.type = a_type;
				p.fields |= differ;
				p.flagBits |= a_flagBits;
				if (p.what.empty())
					p.what = what;
			};
			for (const auto& n : a_probe.nodes) {
				const auto* mirrored = a_mirror.Node(n.key);
				compare(0, n, mirrored, SceneMirror::Fields<SceneCapture::NodeRecord>, n.name, mirrored ? (mirrored->flags ^ n.flags) : 0u);
			}
			for (const auto& g : a_probe.geometries) {
				const auto* node = a_mirror.Node(g.key);
				compare(1, g, a_mirror.Geometry(g.key), SceneMirror::Fields<SceneCapture::GeometryRecord>, node ? node->name : nullptr, 0u);
			}
			for (const auto& p : a_probe.properties)
				compare(2, p, a_mirror.Property(p.key), SceneMirror::Fields<SceneCapture::PropertyRecord>, nullptr, 0u);
			for (const auto& a : a_probe.alphas)
				compare(3, a, a_mirror.Alpha(a.key), SceneMirror::Fields<SceneCapture::AlphaRecord>, nullptr, 0u);
		}

		/** @brief A line of the tally: the counts by record type and field, named. */
		std::string TallyLine(const char* a_label, const std::array<std::array<std::uint64_t, SceneMirror::kMaxFields>, SceneMirror::kTypes>& a_counts)
		{
			std::string out;
			for (std::size_t type = 0; type < SceneMirror::kTypes; ++type) {
				for (std::uint32_t f = 0; f < SceneMirror::kMaxFields; ++f) {
					if (!a_counts[type][f])
						continue;
					const char* name = type == 0 ? (f < SceneCapture::NodeRecord::kFieldCount ? SceneCapture::NodeRecord::kFieldNames[f] : "?") :
					                   type == 1 ? (f < SceneCapture::GeometryRecord::kFieldCount ? SceneCapture::GeometryRecord::kFieldNames[f] : "?") :
					                   type == 2 ? (f < SceneCapture::PropertyRecord::kFieldCount ? SceneCapture::PropertyRecord::kFieldNames[f] : "?") :
					                               (f < SceneCapture::AlphaRecord::kFieldCount ? SceneCapture::AlphaRecord::kFieldNames[f] : "?");
					out += fmt::format("{}{} {} {}", out.empty() ? "" : ", ", SceneMirror::kTypeNames[type], name, a_counts[type][f]);
				}
			}
			return fmt::format("{} {}", a_label, out.empty() ? "0" : out);
		}

		void OnCaptureApplied(const MirrorCaptureApplied& a_event)
		{
			// The objects a capture took are named by no event since.
			auto& named = SceneDiagnostics().mirror.named;
			for (const auto& n : a_event.records.nodes)
				named.erase(n.key);
			for (const auto& g : a_event.records.geometries)
				named.erase(g.key);
			for (const auto& p : a_event.records.properties)
				named.erase(p.key);
			for (const auto& a : a_event.records.alphas)
				named.erase(a.key);
		}

		void OnDetached(const MirrorDetached& a_event)
		{
			auto& named = SceneDiagnostics().mirror.named;
			for (const auto* geometry : a_event.geometries)
				named.erase(geometry);
			for (const void* node : a_event.nodes)
				named.erase(node);
		}

		void OnCleared(const MirrorCleared&)
		{
			auto& m = SceneDiagnostics().mirror;
			m.pending.clear();
			m.named.clear();
		}

		void OnUpdateApplied(const MirrorUpdateApplied& a_event)
		{
			if (a_event.key)
				SceneDiagnostics().mirror.eventFields[SceneMirror::Key(a_event.key, a_event.type)] |= a_event.fields;
		}

		void OnEventsCollecting(const EventsCollecting&)
		{
			// The probe the render thread posted, taken before the drains (so every event older than its capture is in this batch or an
			// earlier one).
			auto& m = SceneDiagnostics().mirror;
			m.batchProbe = m.probePosted.Take();
		}

		void OnEventsApplied(const MirrorEventsApplied& a_event)
		{
			// The objects the batch's events without values named: a field the probe finds different on one of them has a hook that carries
			// no value. The fade, property, node and hidden events' values are updates.
			auto& m = SceneDiagnostics().mirror;
			const auto engineChannels = Extensions::RegisteredEngineChannels();
			for (std::size_t k = 0; k < a_event.channelKeys.size(); ++k)
				if (engineChannels[k].namesKeys)
					for (const void* key : a_event.channelKeys[k])
						m.eventKeys.insert(key);
			if (m.batchProbe)
				m.probe = std::move(m.batchProbe);
		}

		void PostProbeRequest(const Scene::Coordinator& a_store)
		{
			// The next slice of the coordinator's tracked set, each geometry held by the request (a copy of its entry's reference: the entry
			// may let go before the render thread captures it), asked of the render thread once a frame and one at a time. The render thread
			// drops the references.
			auto& m = SceneDiagnostics().mirror;
			const auto& tracked = V::tracked(a_store);
			const std::uint32_t sceneFrame = V::sceneFrame(a_store);
			if (tracked.empty() || m.probeFrame == sceneFrame || m.probeAsked.load(std::memory_order_acquire))
				return;
			m.probeFrame = sceneFrame;
			constexpr std::size_t kSlice = 256;
			auto request = std::make_shared<MirrorProbeRequest>();
			request->geometries.reserve(std::min(kSlice, tracked.size()));
			auto it = tracked.begin() + static_cast<std::ptrdiff_t>(std::min(m.probeCursor, tracked.size()));
			for (std::size_t n = 0; n < std::min(kSlice, tracked.size()); ++n, ++it) {
				if (it == tracked.end())
					it = tracked.begin();
				if (it->second.geometry)
					request->geometries.push_back(it->second.geometry);
			}
			m.probeCursor = static_cast<std::size_t>(it - tracked.begin());
			m.probeAsked.store(true, std::memory_order_release);
			m.probeRequests.Push(std::move(request));
		}

		void OnCheckPoint(const On<ScenePoint::MirrorCheckPoint>&)
		{
			// With the pass's events all drained: the applied batch's probe against the mirror; the next probe asked of the render thread.
			auto& m = SceneDiagnostics().mirror;
			if (m.probe) {
				ZoneScopedN("CS.DCLF.Scene.MirrorParity");
				Check(V::mirror(Scene::Coordinator::Get()), *m.probe, m.eventKeys, m.eventFields);
				if (const void* property = std::exchange(m.missedProperty, nullptr))
					m.watchProperty.store(property, std::memory_order_release);
				if (const void* alpha = std::exchange(m.missedAlpha, nullptr))
					m.watchAlpha.store(alpha, std::memory_order_release);
				if (const void* node = std::exchange(m.missedNode, nullptr))
					m.watchNode.store(node, std::memory_order_release);
				m.probe.reset();
			}
			m.eventKeys.clear();
			m.eventFields.clear();
			PostProbeRequest(Scene::Coordinator::Get());
		}

		void OnProbePoint(const On<ScenePoint::MirrorProbePoint>&)
		{
			// Render thread, the frame's start. CS_DCLF_MIRROR_WATCH=parity: what the last check found stale, watched from here.
			auto& m = SceneDiagnostics().mirror;
			if (const void* property = m.watchProperty.exchange(nullptr, std::memory_order_acq_rel))
				MirrorWatch::ArmProperty(property);
			if (const void* alpha = m.watchAlpha.exchange(nullptr, std::memory_order_acq_rel))
				MirrorWatch::ArmAlpha(alpha);
			if (const void* node = m.watchNode.exchange(nullptr, std::memory_order_acq_rel))
				MirrorWatch::ArmCurrent(node);
			// The slice the coordinator asked for (the newest: an older one is dropped here, with its references).
			std::shared_ptr<MirrorProbeRequest> request;
			m.probeRequests.Drain([&request](std::shared_ptr<MirrorProbeRequest>&& a_request) { request = std::move(a_request); });
			if (!request)
				return;
			ZoneScopedN("CS.DCLF.Ingest.ProbeMirror");
			// Live (the update done, before the culls): each geometry's records and its ancestors' up to the world's root, each object once.
			// Each geometry is held by the request.
			auto probe = std::make_unique<SceneCapture::Records>();
			ankerl::unordered_dense::set<const void*> taken;
			for (const auto& held : request->geometries) {
				const auto* geometry = held.get();
				if (!geometry || !SceneCapture::InWorld(geometry) || !taken.insert(geometry).second)
					continue;
				SceneCapture::CaptureLeaf(*geometry, *probe);
				for (const auto* ancestor = geometry->parent; ancestor && taken.insert(ancestor).second; ancestor = ancestor->parent)
					probe->nodes.push_back(SceneCapture::CaptureNode(*ancestor));
			}
			// To the next pass, which takes it before its drains; the next request may follow.
			m.probePosted.Post(std::move(probe));
			m.probeAsked.store(false, std::memory_order_release);
			WakeScenePass(SceneWake::Capture);
		}
	}

	std::string TakeMirrorParityLines()
	{
		auto& t = SceneDiagnostics().mirror.tally;
		std::string text;
		if (t.probes) {
			std::uint64_t missed = 0;
			for (const auto& type : t.missed)
				for (const auto count : type)
					missed += count;
			text += fmt::format("[DCLF] mirror parity: {} probes, {} records compared, {} absent from the mirror; {}; {}; {}{}{}{}{}\n", t.probes, t.checked, t.absent,
				TallyLine("evented (an event without values):", t.evented), TallyLine("late:", t.late), TallyLine("missed (no event):", t.missed),
				t.absent || missed ? " <- MIRROR" : " <- OK", t.firstAbsent.empty() ? "" : "; first absent: " + t.firstAbsent,
				t.firstMissed.empty() ? "" : "; first missed: " + t.firstMissed, t.firstEvented.empty() ? "" : "; first evented: " + t.firstEvented);
			std::string bits;
			for (std::uint32_t b = 0; b < 32; ++b)
				if (t.flagBitsMissed[b] || t.flagBitsEvented[b])
					bits += fmt::format("{}bit {} {}/{}", bits.empty() ? "" : ", ", b, t.flagBitsMissed[b], t.flagBitsEvented[b]);
			if (!bits.empty())
				text += fmt::format("[DCLF] mirror parity, node flag bits that differed (missed/evented): {}\n", bits);
			for (std::size_t type = 0; type < SceneMirror::kTypes; ++type)
				for (std::uint32_t f = 0; f < SceneMirror::kMaxFields; ++f)
					if (!t.firstMissedBy[type][f].empty())
						text += fmt::format("[DCLF] mirror parity, first missed {} field {}: {}\n", SceneMirror::kTypeNames[type], f, t.firstMissedBy[type][f]);
			t = {};
		}
		return text;
	}

	namespace
	{
		void OnFadeStaticsPlaced(const FadeStaticsPlaced& a_event)
		{
			// CS_DCLF_MIRROR_WATCH: a placed node in the world, its statics just set, is watched for the next writer.
			if (SceneCapture::InWorld(a_event.fade))
				MirrorWatch::Arm(a_event.fade);
		}
	}

	void InstallMirrorWatchPlacement()
	{
		Subscribe<FadeStaticsPlaced>(OnFadeStaticsPlaced);
	}

	void InstallMirrorParity()
	{
		Subscribe<MirrorCaptureApplied>(OnCaptureApplied);
		Subscribe<MirrorDetached>(OnDetached);
		Subscribe<MirrorCleared>(OnCleared);
		Subscribe<MirrorUpdateApplied>(OnUpdateApplied);
		Subscribe<EventsCollecting>(OnEventsCollecting);
		Subscribe<MirrorEventsApplied>(OnEventsApplied);
		Subscribe<ScenePoint::MirrorCheckPoint>(OnCheckPoint);
		Subscribe<ScenePoint::MirrorProbePoint>(OnProbePoint);
	}
}
