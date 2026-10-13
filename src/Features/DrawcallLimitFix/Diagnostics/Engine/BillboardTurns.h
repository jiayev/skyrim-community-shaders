#pragma once

#include <cstdint>
#include <string>

namespace DCLF::Diagnostics::BillboardTurns
{
	/**
	 * @brief CS_DCLF_SET_PARITY: the billboards' observer. A thunk after NiBillboardNode::OnVisible (checked first) compares the world the
	 * engine's cull just turned each effect leaf below to (for the culling camera) with DCLF's row and DCLF's turn of it
	 * (Billboards::SampleRow, Billboards::Orient: the vertex stage's arithmetic on the CPU). Render thread, at Install.
	 */
	void Install();
	bool Installed();

	/** @brief The observer's counts since the last Take (any thread wrote them; the report takes them on the render thread). */
	struct Counts
	{
		std::uint64_t turns = 0, checked = 0, differ = 0;
		float largestRotation = 0.0f, largestTranslation = 0.0f;
		std::string first;  // the first differing leaf, named where it was met
	};
	Counts Take();
}
