#pragma once

// Billboards (CS_DCLF_BILLBOARDS; dclf-architecture.md, "Billboards and aligned particles"; engine notes, "Billboards"): the
// BSEffectShaderProperty leaves under an NiBillboardNode DCLF draws, turned to each view's camera by the vertex stage (Effect.hlsl,
// DCLFBillboardWorld), and the particle systems under billboard nodes DCLF's particle path draws (CS_DCLF_BILLBOARD_PARTICLES). The
// feature registers their switches and live toggle, the parent contributions that admit them (Api::ParentContribution: the classification
// reads them as data, BillboardsFeature.cpp), the CPU twin of the engine's turn (Orient), the parity comparing it with the engine's culls
// (Diagnostics/BillboardTurns.cpp: its early hooks, in before the primary cull takes NiBillboardNode's OnVisible slot) and its report lines.
//
// Core keeps what package/Shaders and the engine define: the placement row and the face mode (Core/Gpu/PlacementRows/BillboardRow.h), the parent
// reason (Ineligible::Billboard), the walk up to the one billboard node (Coordinator::BillboardModeOf) and the mode bits the key and the
// record carry (Records.h, kRasterEffectBillboardShift; the Effect family's record bits). Included by the feature's sources only.

#include "Features/DrawcallLimitFix/Api/Feature.h"

#include <cstdint>
#include <string>

namespace DCLF::Features::Billboards
{
	/** @brief A camera as the engine's billboard code reads it: its world rotation's columns (forward, up, right) and translation. */
	struct Camera
	{
		float forward[3]{}, up[3]{}, right[3]{}, eye[3]{};
	};

	/**
	 * @brief The node's world rotation NiBillboardNode::OnVisible makes (0x140d3e650: R0 * M; M from a_mode and a_camera, R0 replaced by
	 * the identity for BSROTATE_ABOUT_UP), row-major. The CPU twin of Effect.hlsl's DCLFBillboardRotation (the parity compares it with the
	 * engine's turn: Diagnostics/BillboardTurns.cpp).
	 */
	void Orient(const float (&a_base)[3][3], const float (&a_pivot)[3], std::uint32_t a_mode, const Camera& a_camera, float (&a_out)[3][3]);

	// BillboardsFeature.cpp: the feature's entry (Features/FeatureList.cpp), and its toggle (the report's).
	Api::Feature Feature();
	Api::ToggleId Toggle();

	// Diagnostics/BillboardTurns.cpp: CS_DCLF_SET_PARITY's observer of the engine's turns (the feature's early hooks).
	/**
	 * @brief CS_DCLF_SET_PARITY: the billboards' observer. A thunk after NiBillboardNode::OnVisible (checked first) compares the world the
	 * engine's cull just turned each effect leaf below to (for the culling camera) with DCLF's row and DCLF's turn of it
	 * (DCLF::PlacementRows::SampleBillboardRow, Orient: the vertex stage's arithmetic on the CPU). Main thread, at the early hooks.
	 */
	void InstallTurns();
	bool TurnsInstalled();
	/** @brief The observer's counts since the last Take (any thread wrote them; the report takes them on the render thread). */
	struct TurnCounts
	{
		std::uint64_t turns = 0, checked = 0, differ = 0;
		float largestRotation = 0.0f, largestTranslation = 0.0f;
		std::string first;  // the first differing leaf, named where it was met
	};
	TurnCounts TakeTurns();

	// Diagnostics/BillboardsReport.cpp: the report lines (the draws' async work).
	void RegisterReport(Api::InstallContext& a_context);
}
