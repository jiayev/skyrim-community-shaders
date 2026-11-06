#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

// What decides a texture's window, as pure functions (Streamer's worker calls them; nothing here touches the engine or the GPU).
namespace TextureStreaming::Policy
{
	inline constexpr std::uint32_t kNoWindow = ~0u;

	/**
	 * @brief The window top a footprint asks for. A footprint is log2 of the UV one screen pixel covers, so it needs no texture's size: the
	 * mip that gives one texel per pixel is log2(largest side) + footprint. Rounded to the finer mip, and never coarser than a_baseTop.
	 */
	inline std::uint32_t TopForFootprint(std::uint32_t a_largestSide, float a_footprintLog2, std::uint32_t a_baseTop)
	{
		const float mip = std::log2(static_cast<float>(std::max(a_largestSide, 1u))) + a_footprintLog2;
		if (!(mip > 0.0f))
			return 0;
		return std::min(static_cast<std::uint32_t>(mip), a_baseTop);
	}

	/**
	 * @brief The footprint a distance gives, assuming one UV repeat across the object's diameter: the floor (feedback corrects surfaces that
	 * repeat more). a_pixelsPerRadian is screen height / (2 tan(vertical fov / 2)). The object's near side counts, never closer than a tenth of
	 * its radius. a_scale above 1 asks for finer mips.
	 */
	inline float FootprintForDistance(float a_distance, float a_radius, float a_pixelsPerRadian, float a_scale)
	{
		const float radius = std::max(a_radius, 1.0f);
		const float nearSide = std::max(a_distance - radius, radius * 0.1f);
		return std::log2(nearSide / (a_pixelsPerRadian * 2.0f * radius * std::max(a_scale, 0.01f)));
	}

	inline constexpr std::uint32_t kSettleTicks = 1;         // an upgrade's demand holds this long first
	inline constexpr std::uint32_t kUrgentMips = 3;          // this far short of what is wanted: at once
	inline constexpr std::uint32_t kDowngradeSlackMips = 2;  // this much finer than wanted before a downgrade is considered
	inline constexpr std::uint32_t kDowngradeTicks = 20;     // and for this long (about 5 s at 4 Hz)
	inline constexpr std::uint32_t kNativeTicks = 4;         // native binds on this many ticks in a row (about 1 s): the engine's draws want full resolution. First person is one, and cannot report

	// The budget's controller. Everything else in the process comes off the budget first, and what is left is the windows' allowance.
	inline constexpr float kBudgetTarget = 0.92f;       // windows are fitted to this share of the budget, leaving room for the frame's other growth
	inline constexpr float kPullbackStep = 0.25f;       // in mips: a texture crosses a whole mip at its own point, so small steps change the total smoothly
	inline constexpr std::uint32_t kPullbackSteps = 4;  // steps a tick while over the allowance (a mip a tick)
	inline constexpr float kMaxVisibleOffset = 6.0f;    // the setting's limit: about 1/4096 of a texture's bytes
	inline constexpr float kMaxPullback = 12.0f;        // more than any texture's chain: everything at its base
	inline constexpr float kRelaxMargin = 0.95f;        // a step back must fit this share of the allowance, or it would be taken again at once
	inline constexpr std::uint32_t kRelaxTicks = 20;    // and fit for this long (about 5 s) before the first step back

	/** @brief How far demand is pulled back, in mips. Added to the footprint, so coarser. */
	struct Pullback
	{
		float distance = 0.0f;  // of what only distance asks for: surfaces out of view
		float visible = 0.0f;   // of what is on screen: only once every distance demand is at its base, and within the setting's offset
		std::uint32_t relaxTicks = 0;
	};

	/**
	 * @brief One tick of the controller. a_cost(distance, visible) is the bytes the windows take at that pull-back, which never rises as either
	 * does. Over a_allowance, it steps until the windows fit: distance first. Once no distance demand is left to give, what is on screen goes,
	 * by a_maxVisible mips at most: 0 loads what it asks for, however far over that is. Under the allowance, it steps back after a wait, as long
	 * as the step would still fit with a margin. The windows follow at their own pace, so it reads what they would take rather than measured
	 * VRAM, and does not wind up while they catch up.
	 */
	template <class Cost>
	void StepPullback(Pullback& a_pullback, std::uint64_t a_allowance, float a_maxVisible, const Cost& a_cost)
	{
		a_pullback.visible = std::min(a_pullback.visible, a_maxVisible);
		if (a_cost(a_pullback.distance, a_pullback.visible) > a_allowance) {
			a_pullback.relaxTicks = 0;
			// No distance demand can make it fit: no need to walk through the steps that change nothing.
			if (a_cost(kMaxPullback, a_pullback.visible) > a_allowance)
				a_pullback.distance = kMaxPullback;
			for (std::uint32_t step = 0; step < kPullbackSteps && a_cost(a_pullback.distance, a_pullback.visible) > a_allowance; ++step) {
				if (a_pullback.distance < kMaxPullback)
					a_pullback.distance += kPullbackStep;
				else if (a_pullback.visible < a_maxVisible)
					a_pullback.visible = std::min(a_pullback.visible + kPullbackStep, a_maxVisible);
				else
					break;
			}
			return;
		}
		auto next = a_pullback;
		if (next.visible > 0.0f)
			next.visible = std::max(next.visible - kPullbackStep, 0.0f);
		else if (next.distance > 0.0f)
			next.distance -= kPullbackStep;
		else
			return;
		if (static_cast<float>(a_cost(next.distance, next.visible)) > static_cast<float>(a_allowance) * kRelaxMargin) {
			a_pullback.relaxTicks = 0;
			return;
		}
		// After the wait, a step a tick: most steps back through a large pull-back change nothing.
		if (++a_pullback.relaxTicks > kRelaxTicks) {
			next.relaxTicks = a_pullback.relaxTicks;
			a_pullback = next;
		}
	}

	enum class Action : std::uint8_t
	{
		None,
		Upgrade,
		Downgrade
	};

	/** @brief One texture's counters between ticks. */
	struct Pace
	{
		std::uint32_t upgradeTicks = 0;    // ticks the wanted window has been finer than the resident one
		std::uint32_t downgradeTicks = 0;  // ticks the resident window has been finer than wanted by the slack
	};

	/**
	 * @brief What to do this tick, given the resident top (kNoWindow before the first window) and the wanted one. Advances a_pace. A texture
	 * without a window waits for a base request: only a window's upgrade or downgrade is decided here.
	 */
	inline Action Decide(std::uint32_t a_residentTop, std::uint32_t a_wantedTop, Pace& a_pace)
	{
		if (a_residentTop == kNoWindow)
			return Action::None;
		if (a_wantedTop < a_residentTop) {
			a_pace.downgradeTicks = 0;
			const bool urgent = a_residentTop - a_wantedTop >= kUrgentMips;
			if (urgent || a_pace.upgradeTicks >= kSettleTicks) {
				a_pace.upgradeTicks = 0;
				return Action::Upgrade;
			}
			++a_pace.upgradeTicks;
			return Action::None;
		}
		a_pace.upgradeTicks = 0;
		if (a_wantedTop >= a_residentTop + kDowngradeSlackMips) {
			if (++a_pace.downgradeTicks >= kDowngradeTicks) {
				a_pace.downgradeTicks = 0;
				return Action::Downgrade;
			}
			return Action::None;
		}
		a_pace.downgradeTicks = 0;
		return Action::None;
	}
}
