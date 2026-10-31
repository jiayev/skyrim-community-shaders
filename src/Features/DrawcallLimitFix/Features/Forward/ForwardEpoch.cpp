// The forward epoch's submission at the main pass's blended draw (Core's IndirectDraws::SubmitSharedEpoch: the epoch shares the colour
// epoch's shape), with refraction prepared against the selected revision's shape; refraction's counters and the epoch's report lines.

// Main/Resources.h first: volk must precede every Vulkan header of the translation unit.
#include "Features/DrawcallLimitFix/Core/Render/Main/Resources.h"
#include "Features/DrawcallLimitFix/Features/Forward/Forward.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ObjectContributors.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelines.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/EngineStates.h"
#include "Features/DrawcallLimitFix/Core/Render/IndirectDraws.h"
#include "Features/DrawcallLimitFix/Core/Render/Revision/Epochs.h"
#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"
#include "Globals.h"

#include <d3d11.h>

#include <cstdint>
#include <string>

namespace DCLF::Features::Forward
{
	namespace
	{
		// Refraction's counters (render thread): epochs that drew the normals (marked begun for the engine), and those whose members could
		// not, since the last report; the frame whose forward epoch drew them.
		struct RefractionStats
		{
			std::uint64_t drawn = 0, notReady = 0;
			std::uint32_t frame = ~0u;
		};
		RefractionStats refraction;

		/**
		 * @brief The forward epoch (the schedule's "forward", where the feature's forward.* passes are; kNoEpoch: none scheduled), looked up
		 * once: its first read is the render thread's, after InstallFeatures resolved the schedule.
		 */
		std::uint32_t ForwardEpoch()
		{
			static const std::uint32_t epoch = Draws::EpochById("forward");
			return epoch;
		}

		/**
		 * @brief Refraction (SharedEpochSubmit::prepare; a_context: the submission's toggles): the pipeline asked for and its inputs posted
		 * (each once, or when they change), and whether this recording draws the refraction normals (RefractionDrawPass: the colour shape
		 * has refraction slots and the refraction pipeline of the imported normals' format).
		 */
		bool PrepareRefraction(void* a_context, const Draws::Resources* a_resources, const Draws::PassFrame* a_shape)
		{
			const auto& toggles = *static_cast<const ToggleSet*>(a_context);
			bool drawn = false;
			if (Extensions::ObjectClassOn(toggles, Api::ObjectClass::Refraction)) {
				const auto* resources = a_resources;
				auto& pipelines = DrawPipelines::Get();
				pipelines.RequestRefraction();
				if (resources && resources->refractionNormals && globals::game::utilityShader) {
					// The engine's write mask for write mode 2 (FUN_1414b4860), blending off: the forward table's, as it is outside the deferred pass.
					std::uint8_t writeMask = 0xF;
					if (auto* blend = EngineBlendStates()[0][0][2][0]) {
						D3D11_BLEND_DESC desc{};
						blend->GetDesc(&desc);
						writeMask = static_cast<std::uint8_t>(desc.RenderTarget[0].RenderTargetWriteMask & 0xF);
					}
					pipelines.SetRefractionInputs(*globals::game::utilityShader, resources->refractionFormat, writeMask);
				}
				if (a_shape && a_shape->refractionCapacity) {
					drawn = resources && resources->refractionNormals && resources->pool && a_shape->indirect.refraction.valid() &&
					        a_shape->indirect.refractionFormat == resources->refractionFormat;
					if (!drawn)
						++refraction.notReady;
				}
			}
			return drawn;
		}

		/**
		 * @brief ReportPoint::AsyncWorkForward, first (Core prints the point on a report where the epoch reached a frame since the last, or
		 * its toggle is on): the forward epoch's line, taking its counters (IndirectDraws::TakeSharedEpochStats), and refraction's.
		 */
		void ReportLines(std::string& a_text)
		{
			const auto f = IndirectDraws::Get().TakeSharedEpochStats(ForwardEpoch());
			a_text += fmt::format("[DCLF] forward epoch (T7d, CS_DCLF_FORWARD {}): reached {} frames, {} epochs; not submitted: {} without this frame's colour epoch, "
								  "{} without a forward recording{}, {} with nothing to draw (no forward or refraction slot), {} failed{}\n",
				Extensions::ObjectClassOn(ActiveToggles(), Api::ObjectClass::Forward) ? "on" : "off", f.reached, f.epochs, f.noSharedPayload, f.noRecording,
				f.noRecording ? " <- NO FORWARD RECORDING" : "", f.empty, f.failed,
				f.failed ? " <- EPOCH FAILED" : "");
			if (refraction.drawn || refraction.notReady || Extensions::ObjectClassOn(ActiveToggles(), Api::ObjectClass::Refraction))
				a_text += fmt::format("[DCLF] refraction (T7d, CS_DCLF_REFRACTION {}): {} forward epochs drew the refraction normals, {} with refraction slots could not "
									  "(no refraction pipeline yet, or no refraction normals){}\n",
					Extensions::ObjectClassOn(ActiveToggles(), Api::ObjectClass::Refraction) ? "on" : "off", refraction.drawn, refraction.notReady,
					refraction.notReady && !refraction.drawn ? " <- NO REFRACTION DRAW" : "");
			refraction.drawn = refraction.notReady = 0;
		}
	}

	bool SubmitForwardEpoch()
	{
		ZoneScopedN("CS.DCLF.ExecuteForward");
		ToggleSet toggles = ActiveToggles();
		const std::uint32_t epoch = ForwardEpoch();
		if (!Extensions::ObjectClassOn(toggles, Api::ObjectClass::Forward) || epoch == Draws::kNoEpoch)
			return false;
		// A shape without forward and refraction slots records no draw (ForwardDrawPass, RefractionDrawPass): no epoch.
		const auto result = IndirectDraws::Get().SubmitSharedEpoch(epoch, { .prepare = &PrepareRefraction, .context = &toggles,
			.empty = [](const Draws::PassFrame& a_shape) { return !a_shape.forwardCapacity && !a_shape.refractionCapacity; } });
		// Refraction: the epoch cleared and drew the refraction normals, ahead of Main::Draw's own group 5 (FUN_1414b4860). The caller
		// (the blended-draw point) marks them begun and drawn, as that function does, before the engine's blended draws.
		if (!result.submitted || !result.optional)
			return false;
		refraction.frame = Scene::FrameView::Get().GetFrame();
		++refraction.drawn;
		return true;
	}

	bool RefractionDrawnThisFrame()
	{
		return refraction.frame == Scene::FrameView::Get().GetFrame();
	}

	void RegisterReport(Api::InstallContext& a_context)
	{
		// Among the forward epoch's lines, before the features' after it (Features/Effects installs after the feature).
		a_context.RegisterReportLines({ .name = "forward", .point = Api::ReportPoint::AsyncWorkForward, .text = &ReportLines });
	}
}
