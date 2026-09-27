#include "TreeTrace.h"

#include "LightingDescriptors.h"
#include "PassCapture.h"
#include "Records.h"
#include "SceneStore.h"
#include "Switches.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

#include <ankerl/unordered_dense.h>

namespace DCLF
{
	namespace
	{
		// The state of a traced geometry at one of the frame's two sample points.
		struct Sample
		{
			RE::NiPoint3 world;
			float boneSum = 0.0f;  // the skin's bone world translations, summed: moves when any bone does
			std::uint32_t frameID = 0;  // NiSkinInstance::frameID, the palette update's idempotence key
			std::int32_t switchIndex = -2;  // the nearest switch node's index, -2 without one
			std::uint32_t switchRev = 0, childRev = 0;
			std::uint8_t lod152 = 0, lod153 = 0;
			float fade = 1.0f;
			bool hidden = false;
			bool valid = false;
		};

		const RE::NiSwitchNode* NearestSwitch(const RE::BSGeometry* a_geometry, const RE::NiAVObject** a_child)
		{
			const RE::NiAVObject* child = a_geometry;
			for (auto* node = a_geometry->parent; node; child = node, node = node->parent) {
				if (auto* switchNode = node->AsSwitchNode()) {
					*a_child = child;
					return switchNode;
				}
			}
			return nullptr;
		}

		Sample Take(RE::BSGeometry* a_geometry)
		{
			Sample s;
			s.valid = true;
			s.world = a_geometry->world.translate;
			auto& data = a_geometry->GetGeometryRuntimeData();
			if (auto* skin = data.skinInstance.get()) {
				s.frameID = skin->frameID;
				if (skin->boneWorldTransforms)
					for (std::uint32_t i = 0; i < std::min<std::uint32_t>(skin->numMatrices, 80); ++i)
						if (const auto* t = skin->boneWorldTransforms[i])
							s.boneSum += t->translate.x + t->translate.y + t->translate.z;
			}
			if (auto* property = data.shaderProperty.get(); property && property->fadeNode) {
				const auto& fade = property->fadeNode->GetRuntimeData();
				s.lod152 = fade.unk152;
				s.lod153 = fade.unk153;
				s.fade = fade.currentFade;
			}
			const RE::NiAVObject* child = nullptr;
			if (const auto* switchNode = NearestSwitch(a_geometry, &child)) {
				SceneStore::SwitchState state;
				if (SceneStore::ReadSwitch(*switchNode, state)) {
					s.switchIndex = state.index;
					s.switchRev = state.revID;
					s.childRev = state.childRevID && state.index >= 0 && static_cast<std::uint32_t>(state.index) < state.childRevCapacity ?
					                 state.childRevID[state.index] :
					                 ~0u;
				}
			}
			for (const RE::NiAVObject* object = a_geometry; object; object = object->parent)
				if (object->GetFlags().any(RE::NiAVObject::Flag::kHidden))
					s.hidden = true;
			return s;
		}

		std::string Describe(const Sample& a_sample)
		{
			return fmt::format("pos ({:.1f} {:.1f} {:.1f}) bones {:.2f} fid {} switch {}/{}/{} lod {:x}/{:x} fade {:.2f}{}", a_sample.world.x, a_sample.world.y,
				a_sample.world.z, a_sample.boneSum, a_sample.frameID, a_sample.switchIndex, a_sample.switchRev, a_sample.childRev, a_sample.lod152, a_sample.lod153,
				a_sample.fade, a_sample.hidden ? " HIDDEN" : "");
		}

		float Distance(const RE::NiPoint3& a, const RE::NiPoint3& b)
		{
			return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
		}
	}

	struct TreeTrace::Impl
	{
		struct Entry
		{
			std::string name;
			bool skinned = false;
			bool underSwitch = false;
			std::uint32_t lastSeen = 0;  // the last frame it was accumulated or drawn natively
			Sample pre, post;
			// This frame, from AfterAccumulate.
			bool accumulated = false, fading = false;
			std::uint8_t hint = 0, lodRow = 0;
			std::int32_t object = -1;
			std::uint32_t flags = 0;
			std::uint8_t mask = 0;
			RE::NiPoint3 recordWorld;
			bool dclfDrew = false, withheld = false, handedBack = false;
			// Its registrations this frame (PassCapture): how many, how many withheld and fading then, the fade
			// node's LOD state then, and the hints (bit per hint).
			std::uint32_t registrations = 0, registeredWithheld = 0, registeredFading = 0, registeredHints = 0;
			std::uint8_t registered153 = 0xFF;
			Ineligible reason = Ineligible::None;
			bool reasonAccumulate = false;
			// This frame, from the native draws.
			std::uint32_t nativeDraws = 0;
			std::uint32_t nativeCopies = 0;  // hint-10 draws (a LOD cross-fade's copy of the old level): always native
			std::uint8_t nativeLodRow = 0xFF;
			// Who drew it in the last frames, newest in bit 0: 1 native, 2 DCLF (two bits a frame), and whether
			// the engine accumulated it.
			std::uint32_t drawnHistory = 0;
			std::uint32_t accumulatedHistory = 0;
			std::string lastLine;  // the previous frame's judged line, for a gap's context
		};

		ankerl::unordered_dense::map<const RE::BSGeometry*, Entry> entries;
		// Geometries already found not to belong to a tree, so the ancestor walk is taken once.
		ankerl::unordered_dense::set<const RE::BSGeometry*> notTrees;
		std::uint32_t frame = 0;
		bool postTaken = false;

		struct Counts
		{
			std::uint32_t frames = 0, judged = 0;
			std::uint32_t native = 0, dclf = 0, both = 0, nobody = 0;
			std::uint32_t staleWorld = 0, bonesMovedDclf = 0, bonesMovedNative = 0, movedInCull = 0;
			std::uint32_t switchChangedInCull = 0, lodChangedInCull = 0;
			std::uint32_t gaps = 0, engineGaps = 0;  // drawn, not drawn, drawn: by nobody / not accumulated
			std::uint32_t handovers = 0;  // the drawer changed between native and DCLF
			std::uint32_t copies = 0, copiesWithDclf = 0;  // object-frames with native cross-fade copies; of them, DCLF drawing the object
			std::uint32_t lines = 0;
		} counts;

		Entry* Find(RE::BSGeometry* a_geometry)
		{
			if (const auto it = entries.find(a_geometry); it != entries.end())
				return &it->second;
			if (notTrees.contains(a_geometry))
				return nullptr;
			const RE::TESObjectREFR* owner = nullptr;
			for (const RE::NiAVObject* node = a_geometry; node && !owner; node = node->parent)
				owner = node->GetUserData();
			const auto* base = owner ? owner->GetBaseObject() : nullptr;
			if (!base || base->GetFormType() != RE::FormType::Tree) {
				notTrees.insert(a_geometry);
				return nullptr;
			}
			auto& entry = entries[a_geometry];
			entry.name = fmt::format("{}:{}", base->GetFormEditorID() && *base->GetFormEditorID() ? base->GetFormEditorID() : fmt::format("{:08X}", base->GetFormID()),
				a_geometry->name.c_str() ? a_geometry->name.c_str() : "?");
			entry.skinned = a_geometry->GetGeometryRuntimeData().skinInstance != nullptr;
			const RE::NiAVObject* child = nullptr;
			entry.underSwitch = NearestSwitch(a_geometry, &child) != nullptr;
			return &entry;
		}

		// Twelve lines per kind and report interval, so the common kinds do not crowd out the rare ones.
		std::array<std::uint32_t, 8> linesByKind{};
		void Log(std::uint32_t a_kind, const std::string& a_line)
		{
			if (linesByKind[a_kind]++ < 12)
				logger::info("[DCLF] tree trace, frame {}: {}", frame, a_line);
		}

		// The last frame, now that its native draws are all in.
		void Judge()
		{
			if (!postTaken)
				return;
			for (auto& [geometry, e] : entries) {
				if (e.lastSeen + 4 < frame)
					continue;  // gone; a frame it was merely not seen in is still judged, which is what shows a gap
				const bool native = e.nativeDraws > 0;
				const std::uint32_t drawn = (native ? 1u : 0u) | (e.dclfDrew ? 2u : 0u);
				const std::uint32_t previous = e.drawnHistory & 3;
				e.drawnHistory = (e.drawnHistory << 2) | drawn;
				e.accumulatedHistory = (e.accumulatedHistory << 1) | (e.accumulated ? 1u : 0u);
				const std::string line = fmt::format(
					"'{}'{}{} acc {} hint {} row {}{} | obj {} flags {:x} mask {:x} reason {}{} | withheld {} handed-back {} | reg {} withheld {} fading {} 153 {:x} hints {:x} | native {} (row {}) copies {} dclf {} | pre {} | post {}",
					e.name, e.skinned ? " skinned" : "", e.underSwitch ? " switch" : "", e.accumulated, e.hint, e.lodRow, e.fading ? " fading" : "", e.object, e.flags,
					e.mask, kIneligibleNames[static_cast<std::size_t>(e.reason)], e.reasonAccumulate ? "(acc)" : "", e.withheld, e.handedBack, e.registrations, e.registeredWithheld, e.registeredFading, e.registered153, e.registeredHints, e.nativeDraws,
					e.nativeLodRow, e.nativeCopies, e.dclfDrew, Describe(e.pre), Describe(e.post));
				if (e.nativeCopies) {
					++counts.copies;
					counts.copiesWithDclf += e.dclfDrew;
				}
				if (e.accumulated) {
					++counts.judged;
					switch (drawn) {
					case 0:
						++counts.nobody;
						Log(0, "NOBODY DREW an accumulated object: " + line);
						break;
					case 1:
						++counts.native;
						break;
					case 2:
						++counts.dclf;
						break;
					default:
						++counts.both;
						Log(1, "drawn TWICE: " + line);
						break;
					}
				}
				if (e.dclfDrew && Distance(e.recordWorld, e.post.world) > 0.01f) {
					++counts.staleWorld;
					Log(2, fmt::format("DCLF drew a stale world translation ({:.1f} {:.1f} {:.1f}): ", e.recordWorld.x, e.recordWorld.y, e.recordWorld.z) + line);
				}
				if (e.pre.valid && e.post.valid) {
					if (std::abs(e.pre.boneSum - e.post.boneSum) > 0.01f) {
						// The walk ran the palette update at "pre": whoever drew the skin drew that palette.
						++(e.dclfDrew ? counts.bonesMovedDclf : counts.bonesMovedNative);
						Log(3, "bones moved between the walk and the draw: " + line);
					}
					if (Distance(e.pre.world, e.post.world) > 0.01f) {
						++counts.movedInCull;
						Log(3, "moved between the walk and the draw: " + line);
					}
					if (e.pre.switchIndex != e.post.switchIndex || e.pre.childRev != e.post.childRev) {
						++counts.switchChangedInCull;
						Log(4, "switch changed between the walk and the draw: " + line);
					}
					if (e.pre.lod152 != e.post.lod152 || e.pre.lod153 != e.post.lod153) {
						++counts.lodChangedInCull;
						Log(5, "LOD state changed between the walk and the draw: " + line);
					}
				}
				if (previous && drawn && ((previous & 1) != (drawn & 1) || (previous & 2) != (drawn & 2))) {
					++counts.handovers;
					Log(6, fmt::format("drawer {} -> {}: ", previous, drawn) + line);
				}
				// Drawn two frames ago, not last frame, drawn now.
				if (drawn && (e.drawnHistory & 0xC) == 0 && (e.drawnHistory & 0x30)) {
					const bool accumulatedInGap = e.accumulatedHistory & 2;
					++(accumulatedInGap ? counts.gaps : counts.engineGaps);
					Log(7, fmt::format("one-frame gap ({}): gap frame was: {} || now: ", accumulatedInGap ? "accumulated, nobody drew" : "the engine did not accumulate it",
							e.lastLine) +
						line);
				}
				e.lastLine = line;
			}
			postTaken = false;
		}
	};

	bool TreeTrace::Enabled()
	{
		static const bool enabled = SwitchEnabled("CS_DCLF_TREE_TRACE");
		return enabled;
	}

	TreeTrace& TreeTrace::Get()
	{
		static TreeTrace trace;
		return trace;
	}

	TreeTrace::TreeTrace() :
		impl(std::make_unique<Impl>()) {}

	TreeTrace::~TreeTrace() = default;

	void TreeTrace::BeforeScene()
	{
		impl->Judge();
		++impl->frame;
		++impl->counts.frames;
		auto& store = SceneStore::Get();
		// Forget what has not been seen for a while; what is left is dereferenced only while still tracked
		// (detached geometry leaves the tracked set at Present).
		std::erase_if(impl->entries, [&](const auto& a_entry) { return a_entry.second.lastSeen + 8 < impl->frame; });
		for (auto& [geometry, e] : impl->entries) {
			e.pre = store.IsTracked(geometry) ? Take(const_cast<RE::BSGeometry*>(geometry)) : Sample{};
			e.post = {};
			e.accumulated = e.fading = e.dclfDrew = e.withheld = e.handedBack = e.reasonAccumulate = false;
			e.registrations = e.registeredWithheld = e.registeredFading = e.registeredHints = 0;
			e.registered153 = 0xFF;
			e.object = -1;
			e.flags = 0;
			e.mask = 0;
			e.nativeDraws = 0;
			e.nativeCopies = 0;
			e.nativeLodRow = 0xFF;
			e.reason = Ineligible::None;
		}
		if (impl->notTrees.size() > 200000)
			impl->notTrees.clear();
	}

	void TreeTrace::AfterAccumulate()
	{
		auto& store = SceneStore::Get();
		const auto& tables = store.GetTables();
		const auto& lookups = store.GetLookups();
		auto& capture = PassCapture::Get();
		// The accumulated trees join the trace (a new one has no "pre" this frame).
		for (const auto& [geometry, pass] : store.GetAccumulatedPasses()) {
			auto* e = impl->Find(const_cast<RE::BSGeometry*>(geometry));
			if (!e)
				continue;
			e->accumulated = true;
			e->hint = static_cast<std::uint8_t>(pass.hint);
			e->lodRow = static_cast<std::uint8_t>(pass.lodRow);
			e->fading = pass.fading;
			e->lastSeen = impl->frame;
		}
		for (const auto& entry : capture.LastDrain()) {
			if (!entry.geometry || !store.GetMainBatchRenderers().contains(entry.batch))
				continue;
			const auto it = impl->entries.find(entry.geometry);
			if (it == impl->entries.end())
				continue;
			auto& e = it->second;
			++e.registrations;
			e.registeredWithheld += entry.withheld;
			e.registeredFading += entry.fading;
			e.registered153 = entry.fadeState;
			e.registeredHints |= 1u << (entry.hint & 31);
		}
		for (auto& [geometry, e] : impl->entries) {
			if (e.lastSeen + 4 < impl->frame || !store.IsTracked(geometry))
				continue;
			e.post = Take(const_cast<RE::BSGeometry*>(geometry));
			e.reason = store.ReasonThisFrame(geometry, &e.reasonAccumulate);
			e.object = store.FindObject(geometry);
			e.withheld = capture.WithheldThisFrame(geometry);
			e.handedBack = capture.HandedBack(geometry);
			if (e.object >= 0 && static_cast<std::size_t>(e.object) < tables.objects.size()) {
				const auto& record = tables.objects[e.object];
				e.flags = record.flags;
				e.recordWorld = { record.world[3], record.world[7], record.world[11] };
				e.mask = static_cast<std::size_t>(e.object) < tables.skinPartitions.size() ? tables.skinPartitions[e.object] : 0;
				// What HandBackUndrawable counts as drawable, for an object the engine kept (culling off: drawn).
				e.dclfDrew = !(record.flags & kObjectNoBindings) && (record.flags & kObjectNativeVisible) && record.pipelineIndex < lookups.pipelines.size() &&
				             lookups.pipelines[record.pipelineIndex].setIndex != Lookups::kNone;
			}
		}
		impl->postTaken = true;
	}

	void TreeTrace::OnNativeLightingDraw(const RE::BSRenderPass* a_pass)
	{
		if (!a_pass || !a_pass->geometry)
			return;
		auto* e = impl->Find(a_pass->geometry);
		if (!e)
			return;
		if (a_pass->accumulationHint == 10) {
			++e->nativeCopies;
			e->lastSeen = impl->frame;
			return;
		}
		++e->nativeDraws;
		e->nativeLodRow = static_cast<std::uint8_t>(SceneStore::LodRowOf(*a_pass));
		e->lastSeen = impl->frame;
	}

	void TreeTrace::Report(std::uint32_t a_frame, std::uint32_t a_interval)
	{
		if ((a_frame % a_interval) != 0)
			return;
		const auto& c = impl->counts;
		logger::info(
			"[DCLF] tree trace over {} frames: {} accumulated object-frames: {} native, {} DCLF, {} both, {} nobody; one-frame gaps {} (accumulated) + {} (not accumulated); "
			"{} drawer changes; {} with native cross-fade copies ({} of them DCLF's otherwise); DCLF stale world {}; bones moved after the walk {} (DCLF) / {} (native); moved in the cull {}; switch changed in the cull {}; LOD state changed in the cull {}",
			c.frames, c.judged, c.native, c.dclf, c.both, c.nobody, c.gaps, c.engineGaps, c.handovers, c.copies, c.copiesWithDclf, c.staleWorld, c.bonesMovedDclf, c.bonesMovedNative, c.movedInCull,
			c.switchChangedInCull, c.lodChangedInCull);
		impl->counts = {};
		impl->linesByKind = {};
	}
}
