#include "Engine.h"

#include "Features/DrawcallLimitFix/Diagnostics/Engine/ShadowViewTally.h"

namespace DCLF::Reports
{
	void SunViewsLines()
	{
		auto& p = Diagnostics::ShadowViewTally(Diagnostics::ShadowView::Sun);
		const std::uint64_t differ = p.viewProj + p.eye + p.viewport + p.casters + p.cascadePlanes + p.cascadeCasters + p.fullFrustumPlanes + p.missing;
		logger::info("[DCLF] sun views (T2a: DCLF's, from the cameras UpdateCamera set up, against the engine's): {} views ({} view-projection, {} eye, {} viewport, "
					 "{} caster volume differ), {} cascades ({} planes, {} caster planes differ), {} full-frustum processes ({} differ), {} without DCLF's{}{}",
			p.views, p.viewProj, p.eye, p.viewport, p.casters, p.cascades, p.cascadePlanes, p.cascadeCasters, p.fullFrusta, p.fullFrustumPlanes, p.missing,
			differ ? " <- SUN VIEW" : " <- OK", p.first.empty() ? "" : "; first: " + p.first);
		p = {};
	}

	void LightViewsLines()
	{
		auto& p = Diagnostics::ShadowViewTally(Diagnostics::ShadowView::Light);
		const std::uint64_t differ = p.viewProj + p.eye + p.viewport + p.casters + p.missing;
		logger::info("[DCLF] light views (T3a: DCLF's, from the cameras the local lights' UpdateCamera set up, against the engine's): {} views ({} matrices, {} eye, "
					 "{} viewport, {} caster volume differ), {} without DCLF's{}{}",
			p.views, p.viewProj, p.eye, p.viewport, p.casters, p.missing, differ ? " <- LIGHT VIEW" : " <- OK", p.first.empty() ? "" : "; first: " + p.first);
		p = {};
	}

	void FocusViewsLines()
	{
		auto& p = Diagnostics::ShadowViewTally(Diagnostics::ShadowView::Focus);
		const std::uint64_t differ = p.viewProj + p.eye + p.viewport + p.casters + p.missing;
		logger::info("[DCLF] focus views (T3c: DCLF's, from the cameras BSShadowDirectionalLight::sub aimed, against the engine's): {} views ({} matrices, {} eye, "
					 "{} viewport, {} caster volume differ), {} without DCLF's{}{}",
			p.views, p.viewProj, p.eye, p.viewport, p.casters, p.missing, differ ? " <- FOCUS VIEW" : " <- OK", p.first.empty() ? "" : "; first: " + p.first);
		p = {};
		// The membership check.
		const auto members = Diagnostics::TakeFocusMembers();
		logger::info("[DCLF] focus members (T3c: the engine's registrations into DCLF's focus views against their targets' fade roots): {} registrations, {} under "
					 "another root{}{}",
			members.registrations, members.outside, members.outside ? " <- FOCUS MEMBERS" : " <- OK", members.first.empty() ? std::string() : "; first: " + members.first);
	}
}
