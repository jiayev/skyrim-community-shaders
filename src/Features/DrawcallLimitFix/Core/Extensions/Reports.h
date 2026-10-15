#pragma once

// The report lines contract's Core side (Api/Reports.h): the call the report makes at each of its points.

#include "Features/DrawcallLimitFix/Api/Reports.h"

#include <string>

namespace DCLF::Extensions
{
	/** @brief Render thread (the report, at a_point): every registered feature's lines there, in order. */
	void ReportLinesAt(Api::ReportPoint a_point);
	/** @brief Render thread (the report, at a_point, composing its text): every registered feature's text lines there, in order, onto a_text. */
	void ReportTextAt(Api::ReportPoint a_point, std::string& a_text);
}
