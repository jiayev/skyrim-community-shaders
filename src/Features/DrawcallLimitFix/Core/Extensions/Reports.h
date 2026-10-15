#pragma once

// The report lines contract's Core side (Api/Reports.h): the call the report makes at each of its points.

#include "Features/DrawcallLimitFix/Api/Reports.h"

namespace DCLF::Extensions
{
	/** @brief Render thread (the report, at a_point): every registered feature's lines there, in order. */
	void ReportLinesAt(Api::ReportPoint a_point);
}
