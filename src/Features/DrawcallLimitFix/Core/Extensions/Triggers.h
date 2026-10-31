#pragma once

// The active trigger (Api::TriggerDef): the engine point whose hook's DCLF work the render thread is in. Each trigger point marks itself
// with a TriggerScope around that work; the revision submission (IndirectDraws::Impl::SubmitRevisionRecording) compares it with its
// epoch's trigger. One render-thread integer: scopes nest (each restores the one it found), with no allocation and no lock.

#include "Features/DrawcallLimitFix/Api/Epochs.h"

#include <cstdint>
#include <utility>

namespace DCLF::Extensions
{
	/** @brief No trigger point's DCLF work is running (Api::TriggerId's index while none is). */
	inline constexpr std::uint32_t kNoTrigger = ~0u;

	namespace Detail
	{
		// Render thread only: written by TriggerScope, read by the submission's check.
		inline std::uint32_t activeTrigger = kNoTrigger;
	}

	/** @brief Render thread: the active trigger's row in the schedule's triggers (kNoTrigger: none). */
	inline std::uint32_t ActiveTrigger() { return Detail::activeTrigger; }

	/** @brief Render thread: a_trigger active for the scope's life, then the one it found again. */
	class TriggerScope
	{
	public:
		explicit TriggerScope(Api::TriggerId a_trigger) :
			saved(std::exchange(Detail::activeTrigger, a_trigger.index)) {}
		~TriggerScope() { Detail::activeTrigger = saved; }
		TriggerScope(const TriggerScope&) = delete;
		TriggerScope& operator=(const TriggerScope&) = delete;

	private:
		std::uint32_t saved;
	};
}
