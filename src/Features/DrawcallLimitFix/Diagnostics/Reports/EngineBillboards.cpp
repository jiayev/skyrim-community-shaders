#include "Engine.h"

#include "Features/DrawcallLimitFix/Runtime/Toggles.h"
#include "Features/DrawcallLimitFix/Diagnostics/Engine/BillboardTurns.h"
#include "Features/DrawcallLimitFix/Engine/Capture/Billboards.h"
#include "Features/DrawcallLimitFix/Scene/Ports/LightingConstants.h"
#include "Features/DrawcallLimitFix/Scene/FrameView/FrameView.h"

namespace DCLF::Reports
{
	std::string BillboardsLines()
	{
		// The billboard members in the installed tables: objects of an effect key with a billboard mode.
		std::uint32_t members = 0;
		{
			const auto& tables = Scene::FrameView::Get().GetTables();
			for (const auto& object : tables.objects) {
				if ((object.flags & (kObjectFree | kObjectNoBindings)) || !(object.flags & kObjectMember) || object.pipelineIndex >= tables.pipelines.size())
					continue;
				members += RasterEffectBillboard(tables.pipelines[object.pipelineIndex].rasterFlags) ? 1u : 0u;
			}
		}
		const bool on = ActiveToggles().billboards;
		std::string text;
		if (on || members)
			text += fmt::format("[DCLF] billboards (CS_DCLF_BILLBOARDS {}): {} members bound, turned to each view's camera by the vertex stage\n", on ? "on" : "off", members);
		if (Diagnostics::BillboardTurns::Installed()) {
			const auto c = Diagnostics::BillboardTurns::Take();
			const std::string firstText = c.first.empty() ? std::string() : fmt::format("; first {}", c.first);
			text += fmt::format("[DCLF] billboards' engine turns (CS_DCLF_SET_PARITY): {} nodes turned by the engine's culls, {} effect leaves below them checked "
								"against DCLF's row and turn, {} differ (largest rotation {:.2e}, translation {:.2e}){}{}\n",
				c.turns, c.checked, c.differ, c.largestRotation, c.largestTranslation, c.differ ? " <- BILLBOARD" : " <- OK", firstText);
		}
		return text;
	}
}
