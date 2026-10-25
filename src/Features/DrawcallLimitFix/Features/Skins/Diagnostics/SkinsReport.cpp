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
			// The palettes' blocks (skins' and every palette rows class's: particle quads), what FrameValues sizes them by (twice
			// boneCapacity rows) and how much below boneTop is free.
			const auto& tables = V::tables(Scene::Coordinator::Get());
			std::uint64_t held = 0, blocks = 0, free = 0;
			for (const std::uint32_t rows : tables.boneRows)
				if (rows) {
					held += rows;
					++blocks;
				}
			for (const auto& [offset, rows] : tables.boneFree)
				free += rows;
			if (blocks)
				logger::info("[DCLF] palette blocks: {} blocks hold {} rows; {} rows handed out (boneTop), {} free in {} runs below it; capacity {} rows",
					blocks, held, tables.boneTop, free, tables.boneFree.size(), tables.BoneCapacity());
		}
	}

	void RegisterReport(Api::InstallContext& a_context)
	{
		a_context.RegisterReportLines({ .name = "skinned", .point = Api::ReportPoint::SceneCoordinatorLastPass, .lines = &Lines });
	}
}
