// The diagnostic capture's scene-lane sections: the fade census (FadeCensus) and the growth lines (SceneGrowthLines). Both run on the
// scene lane, at a pass's end, only when a capture asks. Core knows no feature: the fade census is whatever the registrations at
// Api::ReportPoint::CaptureCensus write (Features/Fades' "fade roots"), called with their component's context as the report's are.

#include "Internal.h"

#include "Features/DrawcallLimitFix/Core/Extensions/Registry.h"

#include <sstream>
#include <string>
#include <vector>

namespace DCLF::Scene
{
	namespace
	{
		constexpr std::string_view kFadeRoots = "fade roots";
	}

	json Coordinator::FadeCensus()
	{
		json census;
		json other = json::object();
		for (const auto& reportLines : Extensions::RegisteredReportLines()) {
			if (reportLines.point != Api::ReportPoint::CaptureCensus)
				continue;
			std::string text;
			if (reportLines.text)
				reportLines.text(text);
			else if (reportLines.componentText)
				reportLines.componentText({ tables, ComponentState(reportLines.component.index), *this, reportLines.component.index }, text);
			// One JSON document per registration; one that does not parse is kept as its text.
			auto parsed = json::parse(text, nullptr, false);
			if (parsed.is_discarded())
				parsed = text;
			if (reportLines.name == kFadeRoots && census.is_null())
				census = std::move(parsed);
			else
				other[reportLines.name] = std::move(parsed);
		}
		if (!census.is_object())
			census = { { "skipped", "no fade census registered (the Fades feature is not installed)" } };
		if (!other.empty())
			census["other"] = std::move(other);
		census["frame"] = GetFrame();
		return census;
	}

	std::vector<std::string> Coordinator::SceneGrowthLines()
	{
		std::vector<std::string> lines;
		lines.push_back(fmt::format("tracked {}", tracked.size()));
		lines.push_back(fmt::format("walk order capacity {}", order.capacity()));
		lines.push_back(fmt::format("geometry index {}", geometryIndex.size()));
		lines.push_back(fmt::format("pipeline index {}", pipelineIndex.size()));
		lines.push_back(fmt::format("material index {}", materialIndex.size()));
		lines.push_back(fmt::format("material dependents {}", materialDependents.size()));
		lines.push_back(fmt::format("material owners {}", materialOwners.size()));
		lines.push_back(fmt::format("mirror unused records {}", mirror.UnusedRecords()));
		// The registrations' lines ("<name> <value>\n" each).
		for (const auto& reportLines : Extensions::RegisteredReportLines()) {
			if (reportLines.point != Api::ReportPoint::CaptureGrowth)
				continue;
			std::string text;
			if (reportLines.text)
				reportLines.text(text);
			else if (reportLines.componentText)
				reportLines.componentText({ tables, ComponentState(reportLines.component.index), *this, reportLines.component.index }, text);
			std::istringstream stream(text);
			for (std::string line; std::getline(stream, line);)
				if (!line.empty())
					lines.push_back(std::move(line));
		}
		return lines;
	}
}
