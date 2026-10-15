#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Scene/FrameView/FrameView.h"
#include "Features/DrawcallLimitFix/Diagnostics/Draws/DrawsState.h"
#include "Features/DrawcallLimitFix/Diagnostics/Engine/Registrations.h"
#include "Engine.h"

// The views' epochs' report lines (taken by IndirectDraws::AsyncReport): the forward epoch and the effects, the accumulated visibility,
// the shadow capability. (The reflection's and the first-person view's: Features/Reflection, Features/FirstPerson.)

namespace DCLF
{
	std::string IndirectDraws::ForwardReport()
	{
		if (!impl)
			return {};
		auto& f = impl->forwardEpoch;
		if (!f.reached && !ActiveToggles().forward)
			return {};
		std::string text = fmt::format("[DCLF] forward epoch (T7d, CS_DCLF_FORWARD {}): reached {} frames, {} epochs; not submitted: {} without this frame's colour epoch, "
									   "{} without a forward recording{}, {} failed{}\n",
			ActiveToggles().forward ? "on" : "off", f.reached, f.epochs, f.noColour, f.noRecording, f.noRecording ? " <- NO FORWARD RECORDING" : "", f.failed,
			f.failed ? " <- EPOCH FAILED" : "");
		if (f.refractionDrawn || f.refractionNotReady || ActiveToggles().refraction)
			text += fmt::format("[DCLF] refraction (T7d, CS_DCLF_REFRACTION {}): {} forward epochs drew the refraction normals, {} with refraction slots could not "
								"(no refraction pipeline yet, or no refraction normals){}\n",
				ActiveToggles().refraction ? "on" : "off", f.refractionDrawn, f.refractionNotReady, f.refractionNotReady && !f.refractionDrawn ? " <- NO REFRACTION DRAW" : "");
		// The Effect port: its members in the installed tables (objects of an effect key: forward ones, decals in the decal groups, and
		// skinned ones), its material records, its pipelines.
		{
			const auto& tables = Scene::FrameView::Get().GetTables();
			std::uint32_t members = 0, forwardMembers = 0, materials = 0, lit = 0, decals = 0, skinned = 0, opaque = 0;
			std::string firstOpaque;
			for (std::size_t o = 0; o < tables.objects.size(); ++o) {
				const auto& object = tables.objects[o];
				if ((object.flags & (kObjectFree | kObjectNoBindings)) || !(object.flags & kObjectMember))
					continue;
				forwardMembers += (object.flags & kObjectForward) ? 1u : 0u;
				if (object.pipelineIndex < tables.pipelines.size() && RasterEffect(tables.pipelines[object.pipelineIndex].rasterFlags)) {
					++members;
					lit += (tables.pipelines[object.pipelineIndex].passDescriptor & EffectFlag::Lighting) ? 1u : 0u;  // the Lighting variant
					decals += ObjectDecalGroup(object.flags) ? 1u : 0u;
					// An opaque effect (CS_DCLF_OPAQUE_EFFECTS) is decal group 1's with the opaque write mode, 1 (a decal's is 10 or 11).
					const auto rasterFlags = tables.pipelines[object.pipelineIndex].rasterFlags;
					if (RasterDecalGroup(rasterFlags) == 1 && RasterWriteMode(rasterFlags) == 1 && opaque++ == 0 && o < tables.objectGeometry.size() && tables.objectGeometry[o]) {
						const auto* geometry = tables.objectGeometry[o];
						firstOpaque = fmt::format("; first opaque: '{}' at ({:.0f}, {:.0f}, {:.0f}), raster flags {:#x}, pass {:#x}", geometry->name.c_str() ? geometry->name.c_str() : "",
							geometry->world.translate.x, geometry->world.translate.y, geometry->world.translate.z, rasterFlags,
							tables.pipelines[object.pipelineIndex].passDescriptor);
					}
					// A particle member's block is its quads', not a skin's palette.
					skinned += (object.flags & kObjectSkinned) && !(tables.pipelines[object.pipelineIndex].passDescriptor & EffectFlag::Particles) ? 1u : 0u;
				}
			}
			for (std::uint32_t m = 0; m < tables.materials.size(); ++m)
				materials += tables.MaterialUsed(m) && tables.materials[m].kind == kMaterialKindEffect ? 1u : 0u;
			const auto pipelines = DrawPipelines::Get().GetStats();
			if (members || materials || pipelines.effectRequested || ActiveToggles().effects)
				text += fmt::format("[DCLF] effects (T7d's Effect port, CS_DCLF_EFFECTS {}): {} members bound ({} the Lighting variant, {} decals, {} skinned; {} forward "
									"members in all; {} opaque, hint 0, in decal group 1), {} effect materials in use; pipelines {} requested, {} ready, {} failed{}{}\n",
					ActiveToggles().effects ? "on" : "off", members, lit, decals, skinned, forwardMembers, opaque, materials, pipelines.effectRequested, pipelines.effectReady,
					pipelines.effectFailed, pipelines.effectFailed ? " <- EFFECT PIPELINE FAILED" : "", firstOpaque);
			// CS_DCLF_SET_PARITY: the members' Lighting-variant passes the engine registered, against the sun light the port reads as their light 0.
			if (const auto [checked, other] = Diagnostics::Registrations::TakeEffectLight(); checked)
				text += fmt::format("[DCLF] effects' Lighting variant (CS_DCLF_SET_PARITY): {} engine passes of members checked, {} with a light 0 other than the sun "
									"light{}\n",
					checked, other, other ? " <- EFFECT LIGHT" : " <- OK");
		}
		const std::uint32_t submitted = f.submittedFrame;
		const std::uint32_t refractionFrame = f.refractionFrame;
		f = {};
		f.submittedFrame = submitted;
		f.refractionFrame = refractionFrame;
		return text;
	}

	std::string IndirectDraws::VisibilityReport()
	{
		using Readback = VisibilityReadback;
		if (!impl->scene || !impl->scene->visibilityReadback)
			return {};
		auto& r = *impl->scene->visibilityReadback;
		const auto take = [](std::atomic<std::uint64_t>& a_counter) { return a_counter.exchange(0, std::memory_order_relaxed); };
		// The writes (the normal path).
		std::string text = fmt::format(
			"[DCLF] accumulated writes (T7b2): {} write lists made at Main::Update's entry ({} full), {} set, {} cleared, {} already so, "
			"{} clears held by a camera cut ({} cuts, {} roots set by them); {:.1f} us on the main thread\n",
			take(r.writeBatches), take(r.fullWrites), take(r.writesSet), take(r.writesCleared), take(r.writesUnchanged), take(r.writesHeldByCut),
			take(r.cuts), take(r.cutWrites), double(take(r.writeNs)) / 1000.0);
		// Every listed root.
		{
			text += fmt::format(
				"[DCLF] accumulated, every listed root (T7f checklist item 1): wide writes {} set, {} cleared, {} already so; {} nodes between "
				"set ({} geometries whose walk missed their root); cell roots {} seen, {} set\n",
				take(r.wideSet), take(r.wideCleared), take(r.wideUnchanged), take(r.wideNodesSet), take(r.wideNoChain), take(r.cellRootsSeen), take(r.cellRootsSet));
			if (!r.wideNoChainFirst.empty())
				text += fmt::format("[DCLF] accumulated, geometries whose walk missed their root (T7f): {}\n", r.wideNoChainFirst);
			r.wideNoChainFirst.clear();
		}
		// The actors' animation stamp.
		text += Reports::ActorRateLine();
		if (!Diagnostics::Observe().accumulated)
			return text;
		text += Reports::AnimRateLine();
		struct Taken
		{
			std::uint64_t both, dclfOnly, engineOnly, neither;
		};
		const auto takeCounts = [&](Readback::Counts& a_counts) { return Taken{ take(a_counts.both), take(a_counts.dclfOnly), take(a_counts.engineOnly), take(a_counts.neither) }; };
		const auto roots = takeCounts(r.rootCounts), geometry = takeCounts(r.geometryCounts);
		const auto stoodRoots = takeCounts(r.stoodInRootCounts), stoodGeometry = takeCounts(r.stoodInGeometryCounts);
		const auto frames = take(r.framesCompared), incomplete = take(r.framesIncomplete), unmatched = take(r.samplesUnmatched), moved = take(r.moved);
		const auto staleGeometry = take(r.staleGeometry), noVerdictEngine = take(r.noVerdictEngine), noVerdictNeither = take(r.noVerdictNeither);
		const auto hidden = take(r.hiddenDclfOnly);
		const auto batches = take(r.batches), events = take(r.events), stale = take(r.stale), overflowed = take(r.overflowed), missed = take(r.missed);
		const auto setsAdded = take(r.setsAdded), busy = take(r.busy), decodeNs = take(r.decodeNs);
		std::uint32_t sets = 0, listEvents = 0;
		for (const auto* set = r.sets.load(std::memory_order_acquire); set; set = set->next) {
			++sets;
			listEvents = std::max(listEvents, set->events);
		}
		std::string firsts;
		static constexpr std::array<const char*, Readback::kCategories> kCategoryNames{ "roots DCLF's alone", "roots the engine's alone", "geometry DCLF's alone",
			"geometry the engine's alone" };
		for (std::size_t c = 0; c < Readback::kCategories; ++c) {
			if (!r.firstNames[c].empty())
				firsts += fmt::format("; first {}: {}", kCategoryNames[c], r.firstNames[c]);
			r.firstNames[c].clear();
		}
		const bool differ = roots.dclfOnly || roots.engineOnly || geometry.dclfOnly || geometry.engineOnly;
		return text + fmt::format(
			"[DCLF] accumulated (T7b1): {} frames compared ({} left out: a batch past its list; {} samples no batch reached; {} slots moved), engine-culled roots "
			"{} both / {} DCLF's alone / {} the engine's alone / {} neither, geometry {} / {} / {} / {} ({} of the engine's alone stale under a failed root); stood-in roots "
			"{} / {} / {} / {}, geometry {} / {} / {} / {}; aside: roots with no DCLF verdict {} set by the engine / {} not, {} DCLF's alone on an app-culled root; "
			"readback {} batches, {} events ({} stale), {} past their list, {} arms missed, {} staging sets (+{}), event list {}, {} wakes busy, decode {:.1f} us{}{}\n",
			frames, incomplete, unmatched, moved, roots.both, roots.dclfOnly, roots.engineOnly, roots.neither, geometry.both, geometry.dclfOnly, geometry.engineOnly, geometry.neither,
			staleGeometry, stoodRoots.both, stoodRoots.dclfOnly, stoodRoots.engineOnly, stoodRoots.neither, stoodGeometry.both, stoodGeometry.dclfOnly, stoodGeometry.engineOnly,
			stoodGeometry.neither, noVerdictEngine, noVerdictNeither, hidden, batches, events, stale, overflowed, missed, sets, setsAdded, listEvents, busy,
			double(decodeNs) / 1000.0, !frames ? " <- NOT COMPARED" : differ ? " <- ACCUMULATED DIFFER" : " <- OK", firsts);
	}

	std::string IndirectDraws::ShadowCapabilityReport() const
	{
		const auto& c = impl->capabilityStats;
		return fmt::format("[DCLF] shadow capability: phases {:#x}, {} rasterizer states in the catalog; {} changes since startup, {} of them views it did not foresee{}{}",
			ShadowCapability(), DrawPipelines::Get().ShadowRasterStateCount(), c.changes, c.defects, c.defects ? " <- PHASES" : " <- OK",
			c.lastCause.empty() ? "" : " (last: " + c.lastCause + ")");
	}
}
