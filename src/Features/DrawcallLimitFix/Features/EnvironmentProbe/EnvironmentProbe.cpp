#include "Features/DrawcallLimitFix/Features/EnvironmentProbe/EnvironmentProbe.h"
#include "Features/DrawcallLimitFix/Core/Engine/Capture/ConstantMirror.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Views.h"
#include "Features/DrawcallLimitFix/Core/Render/Direct/Sites.h"
#include "Features/DrawcallLimitFix/Core/Render/Epochs/ViewEpoch.h"
#include "Features/DrawcallLimitFix/Core/Render/IndirectDraws.h"
#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"
#include "Features/EnvironmentProbeFeature.h"
#include "Globals.h"
#include "State.h"
#include <bitset>

namespace DCLF::Features::EnvironmentProbe
{
	namespace
	{
		constexpr auto kView = Api::EngineView::EnvironmentProbe;
		std::array<std::byte, 64> shadowParameters{};
		std::uint32_t shadowFrame = UINT32_MAX;
		std::uint32_t submittedFrame = UINT32_MAX;
		std::uint32_t importPendingFrame = UINT32_MAX;
		bool loggedSubmission[2]{};
		enum class Pending
		{
			Targets,
			Coverage,
			Claims,
			Sun,
			Inputs,
			Submit,
			Count
		};
		void NotePending(Pending reason)
		{
			static constexpr std::string_view names[]{ "target import", "epoch coverage", "installed members", "sun-shadow parameters", "depth inputs", "epoch submission" };
			static_assert(std::size(names) == static_cast<std::size_t>(Pending::Count));
			static std::bitset<std::size(names)> seen;
			const auto index = static_cast<std::size_t>(reason);
			if (!seen.test(index)) {
				seen.set(index);
				logger::info("[DCLF] environment probe waiting for {}", names[index]);
			}
		}

		void Prepare(const Api::ViewPointContext& frame)
		{
			const auto& feature = globals::features::environmentProbe;
			auto& draws = IndirectDraws::Get();
			if (frame.failed || !draws.ViewState(kView))
				return;
			if (!feature.loaded || !feature.settings.Enabled || !Extensions::ViewOn(ActiveToggles(), kView)) {
				if (draws.SetViewTargets(kView, {})) {
					draws.DropViewPipelines(kView);
					draws.PostViewInputs();
				}
				return;
			}
			// The capture supplies its atlas after the main world draw. Until the
			// import, there is no probe pass for a scene revision to record.
			if (Scene::FrameView::Get().NotDrawingYet() || !draws.SetUpView(kView) || !draws.ViewState(kView)->resources->target)
				return;
			const ForwardTargets formats{ DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_R32_FLOAT };
			if (draws.SetViewTargets(kView, formats))
				draws.PostViewInputs();
			DrawPipelines::Get().SetViewForwardTargets(kView, formats);
			draws.ResolveViewPipelines(kView, *frame.tables, frame.catalog, {}, false);
		}

		bool Covered(const Api::ViewCoverage& view)
		{
			return view.on && view.main && view.epochsCovered && view.targetImported &&
			       view.mainResourcesCurrent && !view.rebuildRequested && !view.rebuildPending;
		}

		void Install(Api::InstallContext& context)
		{
			const auto seed = context.RegisterSwitch({ .name = "CS_DCLF_ENVIRONMENT_PROBE", .kind = Api::SwitchKind::Feature, .reduced = Api::SwitchReduced::UnlessOne, .description = "0: capture the environment probe with the engine" });
			const auto toggle = context.RegisterToggle({ .name = "environment probe", .label = "Draw environment probe geometry", .tooltip = "Draw world Lighting geometry into the octahedral environment probe from the installed scene.", .section = "Reflections", .seed = seed, .entersClassification = true });
			context.RegisterView({ .view = Api::EngineView::EnvironmentProbe, .toggle = toggle, .phase = kPhase, .membership = Api::ViewMembership::WorldLighting, .keys = { .keyFlags = 8, .cullBackFaces = true }, .shape = { .faces = kOctants, .viewBits = kViewBit, .name = "environment probe", .prefix = "cs.dclf.environment-probe", .blocks = "octant-blocks", .target = "radiance", .labels = { "probe bucket map", "probe bucket tables", "probe culling latches", "probe octant data", {} } }, .life = Api::ViewShapeLife::DropWhenOff, .covered = &Covered });
			context.RegisterViewWork({ kView, Api::ViewPoint::FrameStartAfterTreeLod, &Prepare });
		}
	}

	bool SupportsPipeline(std::uint32_t flags)
	{
		return KeyFamily(flags) == Api::ProgramFamily::Count && !RasterForward(flags) && !RasterRefraction(flags) &&
		       !RasterDecalGroup(flags) && !RasterTerrainGroup(flags);
	}

	void CaptureShadowParameters(ID3D11Buffer* buffer, std::uint32_t frame)
	{
		auto& mirror = ConstantMirror::Get();
		mirror.Watch(buffer);
		const auto bytes = mirror.Contents(buffer);
		shadowFrame = UINT32_MAX;
		if (bytes.size() >= shadowParameters.size()) {
			std::memcpy(shadowParameters.data(), bytes.data(), shadowParameters.size());
			shadowFrame = frame;
		}
	}

	std::shared_ptr<const SetSnapshot> Draw(const Capture& capture)
	{
		auto& draws = IndirectDraws::Get();
		const auto frame = globals::state->frameCount;
		if (submittedFrame == frame || importPendingFrame == frame || !Extensions::ViewOn(ActiveToggles(), kView))
			return {};
		if (!draws.ImportProbeTargets(capture.targets)) {
			importPendingFrame = frame;
			NotePending(Pending::Targets);
			return {};
		}
		if (!draws.ViewCovered(kView)) {
			NotePending(Pending::Coverage);
			return {};
		}
		const auto claims = Scene::FrameView::Get().FrameClaims();
		if (!claims || !(claims->drawn & kPhase)) {
			NotePending(Pending::Claims);
			return {};
		}
		if (capture.usesShadowParameters && shadowFrame != frame) {
			NotePending(Pending::Sun);
			return {};
		}
		IndirectDraws::ViewListed listed;
		if (draws.TakeViewInputs(kView, IndirectDraws::ViewInputsFrom::DepthThisFrame, listed) != IndirectDraws::ViewInputsTaken::Taken) {
			NotePending(Pending::Inputs);
			return {};
		}
		listed.probeTextures = capture.textures;
		std::array<Draws::ProbeBlock, kOctants> blocks{};
		std::array<Draws::ViewFace, kOctants> faces{};
		faces.front().shared = capture.shared;
		for (std::uint32_t f = 0; f < kOctants; ++f) {
			if (!capture.cameras[f])
				continue;
			auto& block = blocks[f];
			if (capture.parameters[f].size() != block.capture.size())
				return {};
			std::ranges::copy(capture.parameters[f], block.capture.begin());
			block.shadowParameters = shadowParameters;
			ShadowCameras::Camera camera;
			ShadowCameras::CameraMatrices(*capture.cameras[f], camera);
			Draws::PerFrameBlock(camera, Draws::FrameBlockNow(), false, nullptr, block.camera.data());
			auto& face = faces[f];
			face.drawn = true;
			face.block = std::as_bytes(std::span(&block, 1)).first(offsetof(Draws::ProbeBlock, record));
			std::memcpy(face.viewProj.data(), block.camera.data() + 32 * sizeof(float), sizeof(face.viewProj));
			face.eye = capture.cameras[f]->world.translate;
		}
		std::uint32_t submitted = 0;
		const bool direct = Direct::CpuFrame();
		static bool attempted[2]{};
		if (!std::exchange(attempted[direct], true))
			logger::info("[DCLF] submitting first {} environment probe epoch, frame {}, {} scene inputs", direct ? "CPU" : "GPU", frame, listed.inputs);
		if (draws.SubmitViewEpoch(kView, 0, listed, faces, &submitted, nullptr) != IndirectDraws::ViewSubmit::Submitted || !submitted) {
			NotePending(Pending::Submit);
			return {};
		}
		if (!loggedSubmission[direct])
			logger::info("[DCLF] first {} environment probe epoch submitted, {} octants", direct ? "CPU" : "GPU", submitted);
		loggedSubmission[direct] = true;
		submittedFrame = frame;
		return claims;
	}

	Api::Feature Feature() { return { "environment probe", &Install }; }
}
