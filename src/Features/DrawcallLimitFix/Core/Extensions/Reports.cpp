// The registered report lines' calls (see the header).

#include "Features/DrawcallLimitFix/Core/Extensions/Reports.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Registry.h"

namespace DCLF::Extensions
{
	void ReportLinesAt(Api::ReportPoint a_point)
	{
		for (const auto& lines : RegisteredReportLines())
			if (lines.point == a_point && lines.lines)
				lines.lines();
	}

	void ReportTextAt(Api::ReportPoint a_point, std::string& a_text)
	{
		for (const auto& lines : RegisteredReportLines())
			if (lines.point == a_point && lines.text)
				lines.text(a_text);
	}
}
