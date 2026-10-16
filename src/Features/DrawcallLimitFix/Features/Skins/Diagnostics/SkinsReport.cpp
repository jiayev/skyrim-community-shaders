// Skins' report line (CS_DCLF_STATS, among the coordinator's last-pass lines): the last pass's skinned candidates and their palette rows.

#include "Features/DrawcallLimitFix/Features/Skins/Skins.h"
#include "Features/DrawcallLimitFix/Diagnostics/Scene/SceneView.h"

namespace DCLF::Features::Skins
{
	namespace
	{
		using V = Diagnostics::SceneView;

		void Lines(std::uint32_t)
		{
			// The scene lane, the coordinator's report, after the fading line: the walk's counts of its last pass.
			const auto& stats = V::stats(Scene::Coordinator::Get());
			if (stats.skinned)
				logger::info("[DCLF] skinned (the last pass): {} candidates, {} palette rows (FrameValues')", stats.skinned, stats.boneRows);
		}
	}

	void RegisterReport(Api::InstallContext& a_context)
	{
		a_context.RegisterReportLines({ .name = "skinned", .point = Api::ReportPoint::SceneCoordinatorLastPass, .lines = &Lines });
	}
}
