#pragma once

// The frame's schedule (Frame.cpp): each epoch DCLF adds passes to and its passes in order. Only the shell includes it, handing it to
// Extensions::InstallFeatures; Core never includes Schedule/.

#include "Features/DrawcallLimitFix/Api/Epochs.h"

namespace DCLF::Schedule
{
	/** @brief The one table: every epoch, its shape, and its passes' keys in order. */
	const Api::FrameSchedule& Frame();
}
