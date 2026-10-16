#pragma once

// Fades' diagnostics, registered by RegisterDiagnostics (FadeDiagnostics.cpp): the report lines (FadeLines.cpp) and, under
// CS_DCLF_FADE_PARITY (FadeParity), the port check (FadePortParity.cpp) and FadeStateCS's log (FadeLog.cpp). Included
// by the feature's diagnostics sources only.

#include "Features/DrawcallLimitFix/Api/Feature.h"
#include "Features/DrawcallLimitFix/Features/Fades/FadeState.h"

#include <cstdint>
#include <vector>

namespace DCLF::Features::Fades
{
	/** @brief FadeLines.cpp: the fade port's line (PrimaryCullParity), the fade roots' (PrimaryCullLast), the write-back's and the writers' (AsyncWorkWriteBack). */
	void RegisterReport(Api::InstallContext& a_context);

	/** @brief FadePortParity.cpp, at install: <- FADE PORT (every 30th frame at the full frustum's end) and the nodes' snapshot (the list jobs' end). */
	void SubscribePortParity();
	/** @brief Render thread: the fade roots' nodes as the list jobs left them in a_frame (null: not taken that frame). */
	const std::vector<FadeNodeState>* NodeSnapshot(std::uint32_t a_frame);
	/** @brief Render thread (the report): the port check's counts since the last call. */
	FadeState::PortCheck TakePortCheck();

	/** @brief FadeLog.cpp, at install: the log window at the depth commit (Diagnostics::FadeLogWindow), its readback after the depth epoch. */
	void SubscribeFadeLog();
}
