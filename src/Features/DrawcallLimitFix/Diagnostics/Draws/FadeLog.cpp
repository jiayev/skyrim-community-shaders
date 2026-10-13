#if defined(CS_HAS_RENDER_GRAPH) && defined(CS_HAS_ORG_MODULE_SERVICES)
#	include "DrawsState.h"
#include "Features/DrawcallLimitFix/Scene/FrameView/FrameView.h"
#	include "Events.h"
#	include "Observers.h"
#	include "Features/DrawcallLimitFix/Diagnostics/Engine/CutObservers.h"

// CS_DCLF_FADE_PARITY: FadeStateCS against the C++ port (Scene/FadeState.h). Every 30th frame the depth commit has the pass log
// kFadeLogEntries roots of a window (NextFadeLog keeps their static rows and the frame's inputs); the log is copied after the depth
// epoch and checked three frames later (ReadFadeLog): each logged update made again by the port from the state it started from, with
// the GPU's frustum verdict. And the root rows' inputs against the nodes (CheckFadeInputs).

namespace DCLF::Diagnostics
{
	namespace
	{
		using FadeReadback = DrawsState::FadeReadback;
	}

	static void CheckFadeInputs(const SceneTables& a_tables)
	{
		auto& state = DrawsState::Get();
		auto& fadeParity = state.fadeParity;
		auto& fadeInputsSeen = state.fadeInputsSeen;
		// Every listed root's row inputs against its node: FadeStateCS reads fadeAmount and +0x109 from the row, which the listing
		// and the events write (Actor::SetAlpha's, the placement snap's). The tables are the last publication's, an event or two
		// behind a value the engine steps each frame, so only a node that held its value since the last check is judged.
		auto& p = fadeParity;
		++p.inputChecks;
		ankerl::unordered_dense::map<const void*, std::pair<float, std::uint8_t>> seen;
		seen.reserve(a_tables.fadeRoots.size());
		// The rows FadeStateCS updates with: +0x109 is the seed's (the node's as the frame's values sampled it), fadeAmount the
		// coordinator's. A root whose seed is not there yet is left out.
		const auto* seeds = FrameValues::Get().FadeSeedsIfDone(true);
		for (std::size_t r = 0; r < a_tables.fadeRoots.size() && r < a_tables.fadeRootNode.size(); ++r) {
			FadeRootStatic row;
			if (!seeds || !MergeFadeSeed(a_tables.fadeRoots[r], seeds->data(), seeds->size(), static_cast<std::uint32_t>(r), row))
				continue;
			const auto* node = static_cast<const RE::NiAVObject*>(a_tables.fadeRootNode[r]);
			if (!node || row.generation == 0)
				continue;
			++p.inputRoots;
			const float amount = Engine::At<float>(node, 0x100);
			const std::uint8_t flags109 = Engine::At<std::uint8_t>(node, 0x109) & 0x3;
			seen.emplace(node, std::make_pair(amount, flags109));
			// +0x109's bits the state reads: 0 (always fade out) and 1 (the LOD thresholds' radius scale).
			const std::uint8_t rowFlags109 = static_cast<std::uint8_t>(row.bits >> kFadeRootBitsShift) & 0x3;
			if (std::memcmp(&amount, &row.fadeAmount, sizeof(amount)) == 0 && flags109 == rowFlags109)
				continue;
			const auto previous = fadeInputsSeen.find(node);
			if (previous == fadeInputsSeen.end() || std::memcmp(&previous->second.first, &amount, sizeof(amount)) != 0 || previous->second.second != flags109) {
				++p.inputMoving;
				continue;
			}
			++p.inputDiffer;
			if (p.inputFirst.empty())
				p.inputFirst = fmt::format("root {} '{}' (plan {}, bits {:#x}): fadeAmount {} (the node's {}), +0x109 {:#x} (the node's {:#x})", r,
					node->name.c_str() ? node->name.c_str() : "", row.bits & kFadeRootPlanMask, row.bits, row.fadeAmount, amount, rowFlags109, flags109);
		}
		fadeInputsSeen = std::move(seen);
		if ((p.inputChecks % 10) == 0) {
			logger::info("[DCLF] fade inputs parity (the root rows' fadeAmount and +0x109 against the nodes): {} checks, {} roots, {} still moving, {} differ{}{}",
				p.inputChecks, p.inputRoots, p.inputMoving, p.inputDiffer, p.inputDiffer ? " <- FADE INPUT" : " <- OK", p.inputFirst.empty() ? "" : "; first: " + p.inputFirst);
			p.inputRoots = p.inputMoving = p.inputDiffer = 0;
			p.inputFirst.clear();
		}
	}

	static std::uint32_t NextFadeLogBase(std::uint32_t a_frame, const SceneTables& a_tables, const FadeFrame& a_inputs)
	{
		auto& state = DrawsState::Get();
		auto& fadeReadback = state.fadeReadback;
		auto& fadeLogCursor = state.fadeLogCursor;
		auto& fadeLogFocus = state.fadeLogFocus;
		auto& turn = state.fadeLogTurn;
		if (!Diagnostics::Observe().fade || (a_frame % 30) != 0 || a_tables.fadeRoots.empty())
			return ~0u;
		CheckFadeInputs(a_tables);
		if (fadeReadback)
			return ~0u;
		const auto count = static_cast<std::uint32_t>(a_tables.fadeRoots.size());
		// The window starts at a live root: the slot table grows and frees rows, and FadeStateCS logs nothing for an empty one (a
		// window of empty rows read back the last log written, and checked nothing). Every other window: the set parity's focus.
		const auto live = [&](std::uint32_t a_row) { return a_row < count && a_tables.fadeRoots[a_row].generation != 0 && a_tables.fadeRoots[a_row].object != ~0u; };
		std::uint32_t base = ~0u;
		if (const std::uint32_t focus = fadeLogFocus.exchange(~0u, std::memory_order_relaxed); (++turn & 1) && live(focus))
			base = focus;
		for (std::uint32_t step = 0, row = fadeLogCursor < count ? fadeLogCursor : 0u; base == ~0u && step < count; ++step, row = row + 1 < count ? row + 1 : 0u)
			if (live(row)) {
				base = row;
				fadeLogCursor = row + kFadeLogEntries;
			}
		if (base == ~0u)
			return ~0u;
		FadeReadback readback;
		readback.frame = a_frame;
		readback.base = base;
		readback.roots.assign(a_tables.fadeRoots.begin() + base, a_tables.fadeRoots.begin() + std::min(count, base + kFadeLogEntries));
		// The rows FadeStateCS updates with: the seeds' node values with the coordinator's inputs (MergeFadeSeed).
		if (const auto* seeds = FrameValues::Get().FadeSeedsIfDone(true))
			for (std::size_t i = 0; i < readback.roots.size(); ++i)
				MergeFadeSeed(readback.roots[i], seeds->data(), seeds->size(), static_cast<std::uint32_t>(base + i), readback.roots[i]);
		readback.inputs = a_inputs;
		readback.nodes.resize(readback.roots.size());
		readback.engine.assign(readback.roots.size(), 0);
		{
			const auto& portals = PortalViews::Get();
			readback.programs = portals.Encoded();
			readback.rootPrograms.assign(readback.roots.size(), kPortalNoProgram);
			readback.radii.assign(readback.roots.size(), 0.0f);
			readback.nodeRadii.assign(readback.roots.size(), 0.0f);
			readback.nodeFlags.assign(readback.roots.size(), 0u);
			for (std::size_t i = 0; i < readback.roots.size(); ++i) {
				if (const auto* root = base + i < a_tables.fadeRootNode.size() ? static_cast<const RE::NiAVObject*>(a_tables.fadeRootNode[base + i]) : nullptr)
					readback.rootPrograms[i] = portals.ProgramOf(root);
				const auto object = readback.roots[i].object;
				const auto* entryNode = object < a_tables.sunEntryNode.size() ? a_tables.sunEntryNode[object] : nullptr;
				const float entry = entryNode ? entryNode->worldBound.radius : -1.0f;
				readback.radii[i] = entry >= 0.0f && entry < 1e30f ? entry : readback.roots[i].radius;
				if (const auto* node = static_cast<const RE::NiAVObject*>(a_tables.fadeRootNode[base + i])) {
					readback.nodeRadii[i] = node->worldBound.radius;
					readback.nodeFlags[i] = node->GetFlags().underlying();
				}
			}
		}
		if (const auto* camera = RE::Main::WorldRootCamera())
			readback.worldCamera = { camera->world.translate.x, camera->world.translate.y, camera->world.translate.z, Engine::At<float>(camera, 0x184) };
		// The engine's nodes as the list jobs left them (PrimaryCull's snapshot): after this cull's OnVisible, before the next
		// animation job's updates, which run beside the render thread.
		const auto* snapshot = CutObservers::NodeSnapshot(a_frame);
		for (std::size_t i = 0; i < readback.roots.size(); ++i) {
			const auto bits = readback.roots[i].bits;
			const auto* node = static_cast<const RE::NiAVObject*>(a_tables.fadeRootNode[base + i]);
			// A root DCLF writes (kFadeRootWritten: every listed root) has no engine OnVisible to compare.
			if (node && snapshot && base + i < snapshot->size() && !(bits & kFadeRootWritten)) {
				readback.nodes[i] = (*snapshot)[base + i];
				readback.engine[i] = 1;
				readback.nodeCentres.resize(readback.roots.size());
				readback.nodeNames.resize(readback.roots.size());
				readback.nodeCentres[i] = { node->worldBound.center.x, node->worldBound.center.y, node->worldBound.center.z };
				readback.nodeNames[i] = node->name.c_str() ? node->name.c_str() : "";
			}
		}
		fadeReadback = std::move(readback);
		return base;
	}

	void NextFadeLog(const FadeLogWindow& a_window)
	{
		a_window.base = NextFadeLogBase(a_window.frame, a_window.tables, a_window.inputs);
	}

	void ReadFadeLog(const MainEpochDone& a_epoch)
	{
		if (!a_epoch.depthOnly)
			return;
		const auto& a_resources = a_epoch.resources;
		auto& state = DrawsState::Get();
		auto& fadeReadback = state.fadeReadback;
		auto& fadeParity = state.fadeParity;
		if (!fadeReadback)
			return;
		auto* context = globals::d3d::context;
		auto& readback = *fadeReadback;
		auto& buffers = *a_resources->scene;
		if (!readback.log) {
			// After the depth epoch that logged: the log into a staging buffer, read three frames later.
			if (!buffers.fadeLog || buffers.fadeLogBase != readback.base || buffers.fadeFrameNumber != readback.frame) {
				fadeReadback.reset();
				return;
			}
			constexpr UINT bytes = kFadeLogEntries * sizeof(FadeLogEntry);
			D3D11_BUFFER_DESC sourceDesc{};
			sourceDesc.ByteWidth = bytes;
			sourceDesc.Usage = D3D11_USAGE_DEFAULT;
			sourceDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			sourceDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
			sourceDesc.StructureByteStride = sizeof(std::uint32_t);
			const auto source = RenderGraphRuntime::Get().WrapBuffer(*buffers.fadeLog, sourceDesc);
			D3D11_BUFFER_DESC desc{};
			desc.ByteWidth = bytes;
			desc.Usage = D3D11_USAGE_STAGING;
			desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			if (!source || FAILED(globals::d3d::device->CreateBuffer(&desc, nullptr, readback.log.put()))) {
				fadeReadback.reset();
				return;
			}
			ScopedPerfEvent event("CS DCLF: fade state readback");
			const D3D11_BOX box{ 0, 0, 0, bytes, 1, 1 };
			context->CopySubresourceRegion(readback.log.get(), 0, 0, 0, 0, source.get(), 0, &box);
			readback.framesLeft = 3;
			return;
		}
		if (--readback.framesLeft)
			return;
		const auto done = std::move(*fadeReadback);
		fadeReadback.reset();
		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (FAILED(context->Map(done.log.get(), 0, D3D11_MAP_READ, 0, &mapped)))
			return;
		const auto* entries = static_cast<const FadeLogEntry*>(mapped.pData);
		auto& p = fadeParity;
		++p.logs;
		for (std::uint32_t i = 0; i < done.roots.size(); ++i) {
			const auto& entry = entries[i];
			const auto& root = done.roots[i];
			// Only what the pass did that frame, for the row it had.
			if (entry.root != done.base + i || entry.after.frame != done.frame || entry.after.generation != root.generation || !root.generation) {
				const bool rootSkip = entry.root != done.base + i, frameSkip = !rootSkip && entry.after.frame != done.frame;
				++(rootSkip ? p.skippedRoot : frameSkip ? p.skippedFrame : p.skippedGeneration);
				if (p.skippedFirst.empty())
					p.skippedFirst = fmt::format("row {}: entry root {}, its update's frame {} (the log's {}), generation {} (the row's {})", done.base + i, entry.root,
						entry.after.frame, done.frame, entry.after.generation, root.generation);
				continue;
			}
			// A root the engine culls too: its OnVisible on the node and FadeStateCS's for the members, from the same state.
			if (i < done.engine.size() && done.engine[i]) {
				++p.engineChecked;
				const auto& n = done.nodes[i];
				const auto& g = entry.after;
				const auto within = [](float a_a, float a_b) { return std::abs(a_a - a_b) <= 1e-5f * std::max(1.0f, std::abs(a_a)); };
				const auto engineDifferences = FadeState::Differences(n, g);
				if (engineDifferences.empty())
					++p.engineExact;
				else if (n.flags == g.flags && n.lastVisible == g.lastVisible && (n.levels & 0xFFFF) == (g.levels & 0xFFFF) && within(n.currentFade, g.currentFade) &&
						 within(n.snapRadius, g.snapRadius) && within(n.amountFade, g.amountFade) && within(n.metric, g.metric) && within(n.previousMetric, g.previousMetric) &&
						 within(n.blend, g.blend))
					++p.engineRounding;
				else {
					++p.engineDiffer;
					if (p.engineFirst.empty()) {
						// Where each metric was measured from: the distance it implies at the frame's scale, against the root's
						// distance from the fade eye and from the world camera.
						const auto& in = done.inputs;
						const auto distanceTo = [&](const float* a_eye) {
							const float dx = entry.centre[0] - a_eye[0], dy = entry.centre[1] - a_eye[1], dz = entry.centre[2] - a_eye[2];
							return std::sqrt(dx * dx + dy * dy + dz * dz);
						};
						const float perMetric = g.metric != 0.0f ? distanceTo(in.eye) / g.metric : 0.0f;
						p.engineFirst = fmt::format("root {} (plan {}, verdict {:#x}): {}; centre ({:.0f} {:.0f} {:.0f}), fade eye ({:.0f} {:.0f} {:.0f}) lodAdjust {} at {:.0f}, "
													"world camera ({:.0f} {:.0f} {:.0f}) lodAdjust {} at {:.0f}; the engine's metric implies {:.0f} at the fade eye's scale",
							entry.root, root.bits & kFadeRootPlanMask, g.verdict, engineDifferences, entry.centre[0], entry.centre[1], entry.centre[2], in.eye[0], in.eye[1],
							in.eye[2], in.lodAdjust, distanceTo(in.eye), done.worldCamera[0], done.worldCamera[1], done.worldCamera[2], done.worldCamera[3],
							distanceTo(done.worldCamera.data()), n.metric * perMetric);
						// The metric at this frame's eye (the port's OnVisible on a copy): which side is current.
						FadeNodeState now = entry.before;
						FadeState::OnVisible(now, root, entry.centre, in);
						p.engineFirst += fmt::format("; at this frame's eye the metric is {}; FadeStateCS's before {} lastVisible {} (after {}), the engine's lastVisible {}",
							now.metric, entry.before.metric, entry.before.lastVisible, g.lastVisible, n.lastVisible);
						{
							// FadeStateCS's test again, on what it was given: the root's portal program, its centre and the radius it read.
							const std::uint32_t list = done.rootPrograms[i];
							std::string why;
							const int port = PortalViews::Visible(done.programs, list, entry.centre, done.radii[i], done.nodeFlags[i], why);
							{
								// How far below its listed entry the root is (the cut lists entries; the engine culls a child after its parents).
								const auto* node = static_cast<const RE::NiAVObject*>(Scene::FrameView::Get().GetTables().fadeRootNode[entry.root]);
								std::uint32_t depth = 0;
								const RE::NiAVObject* at = node;
								while (at && !PrimaryCull::Get().IsEntry(at))
									at = at->parent, ++depth;
								p.engineFirst += fmt::format("; {} below its entry '{}'", at ? fmt::format("{} levels", depth) : std::string("no entry"),
									at && at->name.c_str() ? at->name.c_str() : "");
							}
							p.engineFirst += fmt::format("; its portal program {}, radius read {} (the node's {}), the test on those {} ({})",
								static_cast<std::int32_t>(list), done.radii[i], done.nodeRadii[i], port, why);
						}
						if (i < done.nodeCentres.size())
							p.engineFirst += fmt::format("; the node '{}' centre ({:.0f} {:.0f} {:.0f}) at {:.0f}", done.nodeNames[i], done.nodeCentres[i][0], done.nodeCentres[i][1],
								done.nodeCentres[i][2], distanceTo(done.nodeCentres[i].data()) * 0.0f + [&] {
									const float dx = done.nodeCentres[i][0] - in.eye[0], dy = done.nodeCentres[i][1] - in.eye[1], dz = done.nodeCentres[i][2] - in.eye[2];
									return std::sqrt(dx * dx + dy * dy + dz * dz);
								}());
					}
				}
			}
			++p.updates;
			FadeNodeState port = entry.before;
			std::uint32_t verdict = 0;
			if (entry.after.verdict & kFadeVerdictAnimated) {
				const std::uint32_t updates = (entry.after.verdict >> 5) & kFadeAnimatedCountMask;
				FadeState::AnimatedUpdate(port, root, entry.centre, done.inputs, updates);
				verdict |= kFadeVerdictAnimated | (updates << 5);
			}
			if (entry.after.verdict & kFadeVerdictInView) {
				++p.inView;
				verdict |= FadeState::OnVisible(port, root, entry.centre, done.inputs);
			}
			p.serviced += (verdict & kFadeVerdictServiced) ? 1 : 0;
			const auto differences = FadeState::Differences(port, entry.after);
			if (differences.empty() && verdict == entry.after.verdict) {
				++p.exact;
				continue;
			}
			// Rounding: the same integers and bits, the floats within a few units in the last place (the GPU's division and
			// square root need not round as SSE does).
			const auto close = [](float a_a, float a_b) { return std::abs(a_a - a_b) <= 1e-5f * std::max(1.0f, std::abs(a_a)); };
			const auto& a = entry.after;
			const bool rounding = verdict == a.verdict && port.flags == a.flags && port.lastVisible == a.lastVisible && (port.levels & 0xFFFF) == (a.levels & 0xFFFF) &&
			                      close(port.currentFade, a.currentFade) && close(port.snapRadius, a.snapRadius) && close(port.amountFade, a.amountFade) &&
			                      close(port.metric, a.metric) && close(port.previousMetric, a.previousMetric) && close(port.blend, a.blend);
			++(rounding ? p.rounding : p.differ);
			if (!rounding && p.first.empty())
				p.first = fmt::format("root {} (plan {}, verdict {:#x}, expected {:#x}): {}", entry.root, root.bits & kFadeRootPlanMask, a.verdict, verdict,
					differences.empty() ? "the verdict" : differences);
		}
		context->Unmap(done.log.get(), 0);
		if ((p.logs % 10) == 0) {
			logger::info("[DCLF] fade state parity (FadeStateCS against the port): {} logs, {} root updates ({} in view, {} serviced); {} exact, {} within rounding, {} differ{}{}",
				p.logs, p.updates, p.inView, p.serviced, p.exact, p.rounding, p.differ, p.differ ? " <- FADE STATE" : " <- OK", p.first.empty() ? "" : "; first: " + p.first);
			logger::info("[DCLF] fade state parity: log entries skipped: {} not the row asked for, {} another frame's update, {} another listing's{}", p.skippedRoot,
				p.skippedFrame, p.skippedGeneration, p.skippedFirst.empty() ? "" : "; first: " + p.skippedFirst);
			// Informational: DCLF's state is authoritative for its members, and the engine's node differs where its own cull
			// visits the root differently (an actor's bound, a root its cull never reaches).
			logger::info("[DCLF] fade state of roots with engine-drawn parts (FadeStateCS against the engine's node, informational): {} checked, {} exact, {} within rounding, {} differ{}{}",
				p.engineChecked, p.engineExact, p.engineRounding, p.engineDiffer, p.engineDiffer ? " (the engine's own cull; DCLF's state stands)" : "",
				p.engineFirst.empty() ? "" : "; first: " + p.engineFirst);
			const auto inputs = std::make_tuple(p.inputChecks, p.inputRoots, p.inputMoving, p.inputDiffer, p.inputFirst);
			p = {};
			std::tie(p.inputChecks, p.inputRoots, p.inputMoving, p.inputDiffer, p.inputFirst) = inputs;
		}
	}
}

#endif
