// The water reflection's epoch (its passes are Core's, ViewEpochRecord.cpp's, and tree LOD's): the frame's faces committed into the depth
// commit, inside the depth epoch, which hosts the faces' passes (Schedule/Frame.cpp: no epoch of their own), on the view engine's inputs and
// submission; and the view's report lines. Render thread (the report: wherever the draws' async work is composed).

#include "Features/DrawcallLimitFix/Features/Reflection/ReflectionView.h"
#include "Features/DrawcallLimitFix/Features/Reflection/ReflectionFaces.h"

#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelines.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelinesRhi.h"
#include "Features/DrawcallLimitFix/Core/Handoff/PassCapture.h"
#include "Features/DrawcallLimitFix/Core/Render/IndirectDraws.h"
#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/SceneSet.h"

#include <algorithm>
#include <array>
#include <string>

namespace DCLF::Features::Reflection
{
	namespace
	{
		using ViewSubmit = IndirectDraws::ViewSubmit;

		/**
		 * @brief The frame textures (t16 and up) the faces' forward programs read (the LOD Lighting techniques without Deferred, and the
		 * DistantTree forward program; their sources' declarations): Volumetric Shadows' t18 and t98, Exponential Height Fog's t19, Cloud
		 * Shadows' t25, Dynamic Cubemaps' t30 and t31, Light Limit Fix's t35-t37, Skylighting's t50 and t53, Terrain Blending's t55, Terrain
		 * Shadows' t60, Water Effects' t65, IBL's t76-t79 and the character light's t125. Their commit gives each a source of this frame's:
		 * the character light; a registered frame texture (Integrations/VolumetricShadows' t18, its shared shadow map as bound;
		 * ExponentialHeightFog's t19; CloudShadows' t25; DynamicCubemaps' t30, its reflections view as bound, and t31; Skylighting's t50 and
		 * t53; TerrainBlending's t55; TerrainShadows' t60; WaterEffects' t65; IBL's t76-t79), each null where CS may have bound another
		 * there; and t98, Deferred's sun cascades, a frame buffer: the faces' own copy, filled by their commit from this frame's contents.
		 * Only Light Limit Fix's t35-t37 read null, on purpose (the main camera's clusters, culled after the faces); the report names any
		 * register left null.
		 */
		constexpr std::uint32_t kFaceFrameTextures[] = { 18, 19, 25, 30, 31, 35, 36, 37, 50, 53, 55, 60, 65, 76, 77, 78, 79, 98, 125 };

		/** @brief The faces' frame textures left null since the last report, by register ("none": every one had a source). */
		std::string NullFaceTextures(const std::array<std::uint64_t, 2>& a_null)
		{
			std::string text;
			for (const std::uint32_t t : kFaceFrameTextures)
				if ((a_null[t / 64] >> (t % 64)) & 1)
					text += fmt::format("{}t{}", text.empty() ? "" : " ", t);
			return text.empty() ? std::string("none") : text;
		}
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
		// publication whole - and frame constants of their own, which their commit writes from this frame's sources. The cube is imported
		// after (it is known only at the faces' capture).
		IndirectDraws::ViewListed listed;
		if (draws.TakeViewInputs(kView, IndirectDraws::ViewInputsFrom::DepthThisFrameTargetAfter, listed) != IndirectDraws::ViewInputsTaken::Taken)
			return skip(3);
		if (!FrameIndirectState().valid || !draws.SetUpView(kView) || !draws.ImportViewTarget(kView, reflection.cube.get()))
			return skip(4);
		// Not DCLF's this frame (no claims, or the cube was imported since the graph was built): the engine rendered the faces whole.
		if (!draws.ViewCovered(kView))
			return skip(1);
		std::array<Draws::ViewFace, kFaces> views{};
		for (std::uint32_t f = 0; f < kFaces; ++f)
			if (faces[f].captured)
				views[f] = { true, std::span(faces[f].perFrame.data(), faces[f].perFrameBytes), faces[f].viewProj, faces[f].eye };
		// The selected revision's shape (DecideCoverage covered the faces with the depth epoch's recordings, which carry their passes), with
		// tree LOD's row as this depth commit uploaded it when the faces' tree LOD is DCLF's (Core's, IndirectDraws::SubmitViewEpoch): committed
		// into the depth commit (this work runs inside it), which then submits its recording with the faces' work.
		std::uint32_t drawn = 0;
		std::array<std::uint64_t, 2> nullTextures{};
		switch (draws.SubmitViewEpoch(kView, 0, listed, views, &drawn, &nullTextures)) {
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
		reflection.nullTextures[0] |= nullTextures[0];
		reflection.nullTextures[1] |= nullTextures[1];
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
		// The faces' draw families (tree LOD's): ready or DCLF's when any is.
		const bool facesReady = std::ranges::any_of(state->faces, [](const auto& a_faces) { return a_faces.ready; });
		const bool facesOwned = std::ranges::any_of(state->facesOwned, [](bool a_owned) { return a_owned; });
		std::string text = fmt::format("[DCLF] reflection faces: {} captured (targets colour {}, depth {}); {} LOD pipeline slots, {} forward programs and {} forward pipelines ready, "
									   "tree LOD's {}; forward pipelines {} requested, {} built, {} failed{}\n",
			reflection.facesCaptured, static_cast<int>(state->targets.colour), static_cast<int>(state->targets.depth), reflection.lodSlots, reflection.programsReady,
			reflection.pipelinesReady, facesReady ? "ready" : "not ready", forward.forwardRequested, forward.forwardReady, forward.forwardFailed,
			forward.forwardFailed ? " <- FAILED" : "");
		const auto v = static_cast<std::size_t>(kView);
		text += fmt::format("[DCLF] reflection faces drawn: {} frames, {} epochs, {} faces drawn, {} captured after their frame's epoch{}; not drawn: {} not the set's, {} without faces, "
							"{} the engine's (not covered), {} stale inputs, {} without resources, {} failed{}; last frame's registrations: {} member passes and {} tree LOD passes withheld (faces' tree LOD {}); "
							"frame textures without a source: {}\n",
			reflection.updates, reflection.epochs, reflection.facesDrawn, reflection.lateFaces, reflection.lateFaces ? " <- LATE FACES" : "", s[0], s[2], s[1], s[3], s[4], s[5],
			s[5] ? " <- EPOCH FAILED" : "", capture.viewWithheld[v], capture.viewFamilyWithheld[v][static_cast<std::size_t>(Api::DrawFamily::TreeInstances)], facesOwned ? "DCLF's" : "the engine's",
			reflection.epochs ? NullFaceTextures(reflection.nullTextures) : std::string("no epochs"));
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
		reflection.nullTextures = {};
		return text;
	}
}
