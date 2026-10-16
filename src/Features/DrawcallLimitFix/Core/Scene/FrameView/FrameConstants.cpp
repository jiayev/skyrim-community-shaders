// FrameView's Prepass constants: the pipelines' PerGeometry blocks from the frame's sample (posted to the coordinator when a
// bindless-read value changes), the frame lighting and fog, the character light's view and the projected textures. Render thread.

#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"
#include "State.h"
#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"
#include "Features/DrawcallLimitFix/Diagnostics/Scene/Events.h"
#include "Features/DrawcallLimitFix/Core/Shading/EngineShaders.h"
#include "Features/DrawcallLimitFix/Core/Shading/FrameGlobals.h"
#include "Features/DrawcallLimitFix/Core/Shading/LightingConstants.h"
#include "Features/DrawcallLimitFix/Core/Shading/MaterialSources.h"

namespace DCLF::Scene
{
	static_assert(std::tuple_size_v<decltype(FrameView::FrameCapture::lighting)> == std::tuple_size_v<FrameLighting>);
	static_assert(std::is_same_v<decltype(FrameView::FrameCapture::fog), FrameFog>);

	void FrameView::RefreshFrameConstants()
	{
		const Tables& view = View();
		ZoneNamedN(frameConstantsZone, "CS.DCLF.FrameConstants", true);
		// The material records' frame components are the scene work's (RefreshMaterialSignatures); the frame keeps the
		// character light's view, from its sample.
		RefreshCharacterLightView();
		Diagnostics::Notify<Diagnostics::ScenePoint::PrepassConstantsBegin>();
		if (!EngineShaders::Lighting())
			return;
		// The frame's pipeline inputs (GeometryPort::PipelineFrame), sampled once, here: the eye is the current accumulator's
		// less the shadow state's posAdjust, the main camera's only from Prepass (LatchAccumulator's point; at EarlyPrepass both are
		// still the shadow-map camera's), and the engine's evaluation the port replaces ran here. The held blocks were made from
		// geometryPortFrame (the last sample with a sun), so the pipeline inputs moved when this sample's differ from it. A sample
		// without a sun makes nothing and is not kept. A sample with one is posted to the coordinator too, which makes
		// its new pipelines' blocks from the newest (MakeNewPipelineConstants); a slot new to the frame's tables is made whole below.
		const GeometryPort::PipelineFrame portFrame = GeometryPort::SamplePipelineFrame();
		++geometryStats.samples;
		const bool pipelineInputsMoved = portFrame.sun && (!geometryPortFrame || !portFrame.SamePipelineInputs(*geometryPortFrame));
		if (portFrame.sun) {
			geometryPortFrame = portFrame;
			geometryPortFrameNumber = frame;
			PostPipelineFrame(portFrame);
		}
		// The frame lighting, the frame's (GeometryPort::MergeFrameLighting: the rows every pipeline that writes them shares) over what
		// was last published (nothing without a sun: it keeps its value); versioned only when it differs.
		FrameLighting frameLighting;
		std::memcpy(frameLighting.data(), frameCapture.lighting.data(), sizeof(frameLighting));
		std::uint32_t lightingWritten = 0;
		GeometryPort::MergeFrameLighting(portFrame, frameLighting, lightingWritten);
		// The frame's fog (FrameFog, what the DCLF_BINDLESS vertex stage reads from VS b13): the frame's sample's, as every technique
		// writes it alike (EvaluateTechnique's fog block depends on no pass descriptor), over the last frame's where it writes none.
		// The technique rows keep the fog they were made with, so the time of day versions none of them. No engine read.
		FrameFog frameFog = frameCapture.fog;
		std::uint32_t fogWritten = 0;
		{
			TechniqueConstants fog;
			EvaluateTechnique(0, fog);
			MergeFrameFog(fog, frameFog, fogWritten);
		}
		Diagnostics::Notify<Diagnostics::ScenePoint::PipelineConstantsBegin>();
		frameTables.SyncPipelines(view.pipelines, view.pipelineBindingVersion, frameTablesGeneration, view.pipelinesStamp);
		TracyCZoneN(pipelinesZone, "CS.DCLF.FrameConstants.Pipelines", true);
		std::int64_t pipelinesVisited = 0;
		for (std::size_t word = 0; word < view.usedPipelineBits.size(); ++word) {
			for (std::uint64_t remaining = view.usedPipelineBits[word]; remaining; remaining &= remaining - 1) {
				const std::uint32_t i = static_cast<std::uint32_t>(word * 64 + std::countr_zero(remaining));
				if (i >= view.pipelines.size())
					continue;
				++pipelinesVisited;
				// A pipeline's PerGeometry block is the port's (GeometryPort::PipelineGeometryConstants) from the frame's sample,
				// no engine code run. Made whole when the slot is new (or keyed again), at the first refresh, or when the pipeline inputs
				// moved (PipelineFrame::SamePipelineInputs: SSRParams.xyz, the world map rows); otherwise what moves every frame is
				// written in place (GeometryPort::RefreshFrameValues: the sun, the ambient, the ambient specular, and EyePosition by the
				// port's rule, True PBR's passes included). Those are kept in the block for the constant-buffer path and the checks, but
				// no DCLF_BINDLESS draw reads them there (DCLFFrameLighting; kPSBindlessGeometryUnread), so only what such a draw reads
				// versions and posts the pipeline (SameBindlessGeometry). No template member or pass is needed: a pipeline whose
				// template pass is missing is covered too. Without a sun nothing is made (the engine had no template pass then either):
				// the held block is kept.
				if (!portFrame.sun)
					continue;
				const std::uint32_t passDescriptor = view.pipelines[i].passDescriptor;
				auto& held = frameTables.geometryConstants[i];
				bool ownChanged = false;
				if (!constantsRefreshed || !frameTables.geometryConstantsValid[i] || pipelineInputsMoved) {
					GeometryConstants constants;
					if (!GeometryPort::PipelineGeometryConstants(passDescriptor, GeometryPort::kMainPassRenderFlags, portFrame, constants))
						continue;
					++geometryStats.full;
					// Its own values: what a bindless draw reads from the block (PackGeometryTemplate's mask).
					ownChanged = !frameTables.geometryConstantsValid[i] || !SameBindlessGeometry(constants, held);
					held = constants;
				} else {
					GeometryPort::RefreshFrameValues(passDescriptor, portFrame, held);
				}
				if (ownChanged) {
					frameTables.pipelineConstantsVersion[i] = frameTables.NextVersion();
					++geometryStats.changed;
					PostPipelineConstants(i);
				}
				frameTables.geometryConstantsValid[i] = 1;
			}
		}
		TracyCZoneEnd(pipelinesZone);
		TracyPlot("CS.DCLF.FrameConstants.Pipelines", pipelinesVisited);
		// The pipeline inputs moved: a slot no member names this frame (not visited) holds a block of the older inputs; it is made
		// again (and posted) at the first Prepass that visits it, so every held block is of the inputs it is refreshed with.
		if (pipelineInputsMoved)
			for (std::size_t p = 0; p < frameTables.geometryConstantsValid.size(); ++p)
				if (p / 64 >= view.usedPipelineBits.size() || !((view.usedPipelineBits[p / 64] >> (p % 64)) & 1))
					frameTables.geometryConstantsValid[p] = 0;
		if (fogWritten)
			frameCapture.fog = frameFog;
		if (lightingWritten && std::memcmp(frameLighting.data(), frameCapture.lighting.data(), sizeof(frameLighting)) != 0) {
			std::memcpy(frameCapture.lighting.data(), frameLighting.data(), sizeof(frameLighting));
			++geometryStats.lightingVersions;
		}
		++geometryStats.frames;

		// The objects' shading is FrameValues' (BindlessShading), sampled at the frame's start for the slots the walk names by their
		// events (NameShadingEvents), and their extras' static rows are written by the same events: nothing of either is sampled here.
		// Every pipeline was made whole once: from the next frame, the frame values in place unless the pipeline inputs move.
		constantsRefreshed = true;
		++shadingFrames;
		Diagnostics::Notify<Diagnostics::ScenePoint::PrepassConstantsEnd>();
	}

	void FrameView::RefreshCharacterLightView()
	{
		// The frame record's t11 (kCharacterLightRegister) for character-light passes: the frame's sample (no engine read); the last one
		// kept while the sample has none (its target is -1 while a cell loads).
		if (auto* view = MaterialSources::FrameCharacterLightView(FrameGlobals::Current().material))
			frameCapture.characterLightView = view;
	}

	void FrameView::NoteProjectedTextures()
	{
		auto& state = globals::game::shadowState->GetRuntimeData();
		ProjectedTextures seen{};
		for (std::size_t i = 0; i < ProjectedTextures::kSlots.size(); ++i) {
			seen.views[i] = reinterpret_cast<ID3D11ShaderResourceView*>(state.PSTexture[ProjectedTextures::kSlots[i]]);
			if (!seen.views[i])
				return;
		}
		seen.valid = true;
		if (projectedTextures.valid && projectedTextures.views == seen.views)
			return;
		projectedTextures = seen;
		// To the scene work, which asks for their bindings (SharedBindings): each view referenced here, where the engine has it
		// bound, so it outlives the scene work's request. The newest capture wins.
		auto capture = std::make_shared<SharedBindings::ProjectedCapture>();
		for (std::size_t i = 0; i < seen.views.size(); ++i)
			capture->views[i].copy_from(seen.views[i]);
		channels.projectedPosted.store(std::move(capture), std::memory_order_release);
	}

	void FrameView::PostPipelineConstants(std::uint32_t a_slot)
	{
		// To the coordinator through a lock-free queue (no hand-over at the frame's start): its next scene pass applies it.
		auto post = std::make_unique<PipelineConstantsPost>();
		post->slot = a_slot;
		post->key = frameTables.pipelineKeys[a_slot];
		post->binding = frameTables.pipelineBindings[a_slot];
		post->generation = frameTables.tablesGeneration;
		post->constants = frameTables.geometryConstants[a_slot];
		channels.pipelineConstantsPosted.Push(std::move(post));
		++pipelinesPosted;
	}
}
