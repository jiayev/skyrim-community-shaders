// The water reflection's epoch (its passes are Core's, ViewEpochRecord.cpp's): the frame's faces drawn in one epoch right after the depth
// commit, on the view engine's inputs and submission; and the view's report lines. Render thread (the report: wherever the draws' async work
// is composed).

#include "Features/DrawcallLimitFix/Features/Reflection/ReflectionView.h"
#include "Features/DrawcallLimitFix/Features/Reflection/ReflectionFaces.h"

namespace DCLF::Features::Reflection
{
	namespace
	{
		using ViewSubmit = IndirectDraws::ViewSubmit;
	}

	void Execute(const Api::ViewPointContext&)
	{
		ZoneScopedN("CS.DCLF.ExecuteReflection");
		auto& draws = IndirectDraws::Get();
		if (!draws.ViewState(kView))
			return;
		auto& reflection = FrameState();
		++reflection.updates;
		reflection.executedFrame = Scene::FrameView::Get().GetFrame();
		// The frame's faces, taken whatever happens: the next frame captures its own.
		auto faces = reflection.faces;
		for (auto& face : reflection.faces)
			face.captured = false;
		auto skip = [&](std::uint32_t a_cause) { ++reflection.skipped[a_cause]; };
		const auto set = Scene::FrameView::Get().FrameClaims();
		if (!set || !(set->drawn & kSetReflection))
			return skip(0);
		if (std::none_of(faces.begin(), faces.end(), [](const auto& a_face) { return a_face.captured; }))
			return skip(2);
		// What the faces draw from: the frame's scene list (the installed payload, as the shadow views': a member is the reflection's from
		// the frame it joins) with this frame's depth commit's index pool, depth inputs, object records and geometry rows - the frame's
		// publication whole - and the frame before's colour frame record, in the backing it was written to.
		IndirectDraws::ViewListed listed;
		if (draws.TakeViewInputs(kView, IndirectDraws::ViewInputsFrom::DepthThisFrameColourBefore, listed) != IndirectDraws::ViewInputsTaken::Taken)
			return skip(3);
		if (!FrameIndirectState().valid || !draws.SetUpView(kView) || !draws.ImportViewTarget(kView, reflection.cube.get()))
			return skip(4);
		// Not DCLF's this frame (no claims, or the cube was imported since the graph was built): the engine rendered the faces whole.
		if (!draws.ViewCovered(kView))
			return skip(1);
		std::array<ViewFace, kFaces> views{};
		for (std::uint32_t f = 0; f < kFaces; ++f)
			if (faces[f].captured)
				views[f] = { true, std::span(faces[f].perFrame.data(), faces[f].perFrameBytes), faces[f].viewProj, faces[f].eye };
		// The selected revision's shape and recording (DecideCoverage covered the faces), with tree LOD's row as the last depth commit
		// uploaded it when the faces' tree LOD is DCLF's (Core's, IndirectDraws::SubmitViewEpoch).
		std::uint32_t drawn = 0;
		switch (draws.SubmitViewEpoch(kView, 0, listed, views, &drawn)) {
		case ViewSubmit::NoRevision:
			return skip(1);
		case ViewSubmit::NoOwners:
			return skip(4);
		case ViewSubmit::Failed:
			logger::error("[DCLF] the reflection faces' epoch failed");
			return skip(5);
		case ViewSubmit::Submitted:
			break;
		}
		++reflection.epochs;
		reflection.facesDrawn += drawn;
	}

	std::string Report()
	{
		const auto* state = IndirectDraws::Get().ViewState(kView);
		if (!state)
			return {};
		auto& reflection = FrameState();
		const auto forward = DrawPipelines::Get().GetStats();
		const auto& capture = PassCapture::Get().GetStats();
		const auto& s = reflection.skipped;
		std::string text = fmt::format("[DCLF] reflection faces: {} captured (targets colour {}, depth {}); {} LOD pipeline slots, {} forward programs and {} forward pipelines ready, "
									   "tree LOD's {}; forward pipelines {} requested, {} built, {} failed{}\n",
			reflection.facesCaptured, static_cast<int>(state->targets.colour), static_cast<int>(state->targets.depth), reflection.lodSlots, reflection.programsReady,
			reflection.pipelinesReady, state->treeReady ? "ready" : "not ready", forward.forwardRequested, forward.forwardReady, forward.forwardFailed,
			forward.forwardFailed ? " <- FAILED" : "");
		const auto v = static_cast<std::size_t>(kView);
		text += fmt::format("[DCLF] reflection faces drawn: {} frames, {} epochs, {} faces drawn, {} captured after their frame's epoch{}; not drawn: {} not the set's, {} without faces, "
							"{} the engine's (not covered), {} stale inputs, {} without resources, {} failed{}; last frame's registrations: {} member passes and {} tree LOD passes withheld (faces' tree LOD {})\n",
			reflection.updates, reflection.epochs, reflection.facesDrawn, reflection.lateFaces, reflection.lateFaces ? " <- LATE FACES" : "", s[0], s[2], s[1], s[3], s[4], s[5],
			s[5] ? " <- EPOCH FAILED" : "", capture.viewWithheld[v], capture.viewFamilyWithheld[v][static_cast<std::size_t>(Api::DrawFamily::TreeInstances)], state->treeOwned ? "DCLF's" : "the engine's");
		// The faces' cameras against the engine's at their draws, and the LOD roots the engine did not cull.
		const auto roots = ReflectionFaces::TakeRootStats();
		const auto residue = PassCapture::Get().TakeViewResidue(kView);
		const std::string residueKinds = ResidueKinds();
		auto& parity = FaceCameraCounts();
		text += fmt::format("[DCLF] reflection faces (T4: DCLF's cameras, the engine's tree LOD root skipped; T6b6: its LOD land and objects roots while gated): {} updates, {} without the "
							"tree root, {} without the LOD roots; parity: {} faces, {} blocks "
							"and {} slices differ (registers {:#x}, largest {:.3g}){}{}; residue: {} tree LOD passes{}, {} under the LOD land and objects roots (kept on every "
							"30th update){}{}\n",
			roots.updates, roots.treeSkipped, roots.lodSkipped, parity.faces, parity.blocks, parity.slices, parity.registers, parity.largest,
			parity.blocks || parity.slices ? " <- FACE CAMERA" : (parity.faces ? " <- OK" : ""), parity.first.empty() ? "" : " (first: " + parity.first + ")",
			residue.treePasses, residue.treePasses ? " <- REFLECTION RESIDUE" : "", residue.lodPasses, residue.first.empty() ? "" : " (first: " + residue.first + ")", residueKinds);
		parity = {};
		reflection.facesCaptured = reflection.updates = reflection.epochs = reflection.facesDrawn = reflection.lateFaces = 0;
		reflection.skipped = {};
		return text;
	}
}
