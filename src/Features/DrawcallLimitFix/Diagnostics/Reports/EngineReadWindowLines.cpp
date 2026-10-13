#include "Engine.h"

#include "Features/DrawcallLimitFix/Diagnostics/Engine/ReleaseGuard.h"
#include "Features/DrawcallLimitFix/Engine/Access/EngineReadWindow.h"

namespace DCLF::Reports
{
	std::string EngineReadWindowLine()
	{
		// The scene work's accesses without a lease (any is a defect).
		const auto c = EngineReadWindow::Take();
		if (!c.any)
			return {};
		return fmt::format("[DCLF] engine-read window: {} closes, {} of them waited for items in flight ({:.1f} us in all, max {:.1f} us); {} leases refused (work left for the next window); "
						   "scene work engine accesses without a lease {}{}{}",
			c.closes, c.closesWaited, c.waitedUs, c.waitedMaxUs, c.refused, c.unleased, c.unleased ? " <- LANE ENGINE ACCESS; first: " : " <- OK",
			c.unleasedFirst ? c.unleasedFirst : "");
	}

	std::string ReleaseGuardLine()
	{
		if (!ReleaseGuard::Installed())
			return {};
		const auto c = ReleaseGuard::Take();
		return fmt::format("[DCLF] release guard (T6b3e, CS_DCLF_RELEASE_GUARD): engine objects' last releases on DCLF's threads: scene lane {}, executor/pool {} "
						   "({} without a class name){}{}{}{}",
			c.lane, c.pool, c.unnamed, c.lane || c.pool ? " <- LANE ENGINE RELEASE; first: " : " <- OK", c.first ? c.first : "", c.lane || c.pool ? ", last: " : "",
			(c.lane || c.pool) && c.last ? c.last : "");
	}
}
