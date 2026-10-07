#include "FadeState.h"

#include "Features/DrawcallLimitFix/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Engine/PrimaryCull.h"
#include "Features/DrawcallLimitFix/Scene/FrameGlobals.h"
#include "Features/DrawcallLimitFix/Scene/SceneStore.h"

#include <cmath>
#include <cstring>

namespace DCLF::FadeState
{
	namespace
	{
		using Engine::At;
		using Engine::Global;

		// AE 1.6.1170 module offsets (skyrim-engine-notes.md, "Fade state").
		constexpr std::uintptr_t kFadeOnVisible = 0x1479f50;      // BSFadeNode::OnVisible
		constexpr std::uintptr_t kLeafOnVisible = 0x147c9c0;      // BSLeafAnimNode::OnVisible
		constexpr std::uintptr_t kTreeOnVisible = 0x147d3c0;      // BSTreeNode::OnVisible
		constexpr std::uintptr_t kTreeRtti = 0x332a2d8;           // NiRTTI of BSTreeNode
		constexpr std::uintptr_t kFadeValue = 0x147b110;          // FUN_14147b110(node, camera, &distance)
		constexpr std::uintptr_t kFadeUpdate = 0x147a160;         // FUN_14147a160(node, fadeAmount, camera)
		constexpr std::uintptr_t kLodStep = 0x147a430;            // FUN_14147a430(node, distance)
		constexpr std::uintptr_t kLogf = 0x153d086;               // the engine's CRT logf
		constexpr std::uintptr_t kPowf = 0x153d014;               // ... and powf
		constexpr std::size_t kOnVisibleSlot = 0x34;

		/** @brief MINSS a, b: a when a < b, else b (b when either is NaN). */
		float MinSS(float a_a, float a_b) { return a_a < a_b ? a_a : a_b; }
		/** @brief MAXSS a, b: a when a > b, else b. */
		float MaxSS(float a_a, float a_b) { return a_a > a_b ? a_a : a_b; }
		/** @brief UCOMISS's ZF: equal, or unordered. */
		bool UnorderedEqual(float a_a, float a_b) { return !(a_a < a_b) && !(a_a > a_b); }

		std::uint32_t TypeOf(const FadeNodeState& a_state) { return (a_state.levels >> 8) & 0xFu; }

		/** @brief The fade frame counter's distance from the last visible frame, as the engine subtracts it (32-bit). */
		std::int32_t SinceVisible(const FadeNodeState& a_state, const FadeFrame& a_frame)
		{
			return static_cast<std::int32_t>(static_cast<std::uint32_t>(a_frame.counter) - static_cast<std::uint32_t>(a_state.lastVisible));
		}
		bool LongUnseen(const FadeNodeState& a_state, const FadeFrame& a_frame)
		{
			return a_state.lastVisible < static_cast<std::int32_t>(static_cast<std::uint32_t>(a_frame.counter) - 20u);
		}

		/** @brief FUN_14147b110: the fade value; writes the LOD metric and the unscaled distance. */
		float FadeValue(FadeNodeState& a_state, const FadeRootStatic& a_root, const float a_centre[3], const FadeFrame& a_frame, float& a_distance)
		{
			const std::uint32_t type = TypeOf(a_state);
			const float divisor = a_frame.divisors[type];
			float scale;
			if (divisor > 0.0f) {
				scale = a_frame.lodAdjust / divisor;
			} else {
				scale = a_frame.defaultScale;
				if (type != 6) {
					a_state.flags |= kFadeFlagFadedIn;
					a_state.currentFade = 1.0f;
				}
			}
			const float dy = a_centre[1] - a_frame.eye[1];
			const float dx = a_centre[0] - a_frame.eye[0];
			const float dz = a_centre[2] - a_frame.eye[2];
			const float sum = (dy * dy + dx * dx) + dz * dz;
			const float distance = std::sqrt(sum);
			a_distance = distance;
			const float x = distance * scale;
			const float nearDistance = a_frame.distanceMult * a_root.nearDistance;
			float value = a_frame.one;
			if (x > nearDistance && a_frame.lodUpdates)
				value = a_frame.one - (x - nearDistance) / (a_frame.distanceMult * a_root.farDistance - nearDistance);
			a_state.previousMetric = a_state.metric;
			a_state.metric = x * a_frame.metricScale;
			if (a_frame.overridden)
				a_state.metric = a_frame.metricOverride;
			return value;
		}

		/** @brief FUN_14147a430: the LOD level and its cross-fade. */
		void LodStep(FadeNodeState& a_state, const FadeRootStatic& a_root, float a_distance, const FadeFrame& a_frame)
		{
			std::uint8_t l152 = static_cast<std::uint8_t>(a_state.levels);
			std::uint8_t l153 = static_cast<std::uint8_t>(a_state.levels >> 8);
			std::uint8_t level = l152 & 0xF;
			l152 = static_cast<std::uint8_t>((l152 & 0xF) | (l152 << 4));
			l153 &= 0x7F;
			if (((l153 & 0xF0) == 0x20 && UnorderedEqual(a_frame.one, a_state.currentFade)) || !a_frame.lodUpdates) {
				const float scale = a_root.lodScale;
				const bool tree = (l153 & 0xF) == 4 && (a_root.bits & kFadeRootTreeThresholds);
				const float farThreshold = tree ? a_frame.treeLodFar : a_frame.lodFar;
				const float nearThreshold = tree ? a_frame.treeLodNear : a_frame.lodNear;
				float farDistance = farThreshold * scale;
				if (farThreshold > 0.0f)
					farDistance = MaxSS(a_frame.lodMinimum, farDistance);
				float nearDistance = nearThreshold * scale;
				if (nearThreshold > 0.0f)
					nearDistance = MaxSS(a_frame.lodMinimum, nearDistance);
				level = a_distance > farDistance ? 1 : (a_distance <= nearDistance ? 3 : 2);
			}
			if (LongUnseen(a_state, a_frame) || !a_frame.lodUpdates) {
				l152 = static_cast<std::uint8_t>((l152 & 0xF0) | level);
				l153 = static_cast<std::uint8_t>((l153 & 0xAF) | 0x20);
			} else {
				std::uint8_t state = l153;
				const std::uint8_t transition = state & 0x70;
				if (transition > 0x20) {
					state = static_cast<std::uint8_t>((state & 0xBF) | 0x30);
					a_state.blend = MinSS(a_frame.deltaTime / a_frame.blendTime + a_state.blend, a_frame.one);
					if (UnorderedEqual(a_state.blend, a_frame.one)) {
						l152 = static_cast<std::uint8_t>((l152 & 0xF0) | ((l152 + 1) & 0xF));
						state = static_cast<std::uint8_t>((state & 0x2F) | 0x80);
					}
				} else if (transition < 0x20) {
					state = static_cast<std::uint8_t>((state & 0x9F) | 0x10);
					a_state.blend = MaxSS(a_state.blend - a_frame.deltaTime / a_frame.blendTime, 0.0f);
					if (UnorderedEqual(a_state.blend, 0.0f))
						state = static_cast<std::uint8_t>((state & 0x0F) | 0xA0);
				} else {
					const std::uint8_t previous = l152 >> 4;
					if (previous < level) {
						a_state.blend = 0.0f;
						state = static_cast<std::uint8_t>((state & 0x0F) | 0xC0);
					} else if (previous > level) {
						a_state.blend = 1.0f;
						state = static_cast<std::uint8_t>((state & 0x0F) | 0x80);
						l152 = static_cast<std::uint8_t>((l152 & 0xF0) | level);
					}
				}
				l153 = state;
			}
			a_state.levels = (a_state.levels & 0xFFFF0000u) | l152 | (std::uint32_t(l153) << 8);
		}

		/** @brief FUN_14147a160: the fade state machine. */
		void FadeUpdate(FadeNodeState& a_state, const FadeRootStatic& a_root, const float a_centre[3], float a_amount, const FadeFrame& a_frame)
		{
			float distance = 0.0f;
			const float value = FadeValue(a_state, a_root, a_centre, a_frame, distance);
			if (a_state.flags & kFadeFlagLodInUpdate)
				LodStep(a_state, a_root, distance, a_frame);
			const std::uint32_t type = TypeOf(a_state);
			if (type != 8 && LongUnseen(a_state, a_frame) && a_frame.snapRadiusLimit > a_state.snapRadius) {
				if (value > a_frame.fadeInAbove || ((a_state.flags & kFadeFlagFadedIn) && value > a_frame.fadeOutBelow)) {
					a_state.flags |= kFadeFlagFadedIn;
					a_state.currentFade = 1.0f;
				} else {
					a_state.currentFade = 0.0f;
					a_state.flags &= ~kFadeFlagFadedIn;
				}
			}
			const bool alwaysOut = ((a_root.bits >> kFadeRootBitsShift) & 1u) != 0;
			const float one = a_frame.one;
			bool fadeOut = false;
			if (!alwaysOut) {
				const bool fadedIn = (a_state.flags & kFadeFlagFadedIn) != 0;
				if (fadedIn && (!UnorderedEqual(a_amount, one) || !UnorderedEqual(one, a_state.amountFade))) {
					// Toward fadeAmount (a script's fade), or past the snap threshold.
					const float step = MinSS(a_frame.deltaTime / a_frame.amountTime, a_frame.stepMax);
					float fade;
					if (a_amount > a_frame.amountSnapAbove) {
						fade = a_amount - a_frame.amountSnapOffset;
					} else {
						const float last = a_state.amountFade;
						fade = a_amount >= last ? MinSS(last + step, a_amount) : MaxSS(last - step, a_amount);
					}
					a_state.currentFade = fade;
					a_state.amountFade = fade;
					return;
				}
				const float current = a_state.currentFade;
				const bool fadeIn = (!fadedIn && value > a_frame.fadeInAbove) || (current > 0.0f && current < one && value > a_frame.fadeOutBelow);
				if (fadeIn) {
					if (SinceVisible(a_state, a_frame) > 1 && type != 8) {
						a_state.currentFade = 1.0f;
						a_state.flags |= kFadeFlagFadedIn;
						return;
					}
					const float fade = MinSS(MinSS(a_frame.deltaTime / a_frame.fadeInTime, a_frame.stepMax) + a_state.currentFade, one);
					a_state.currentFade = fade;
					if (fade < one)
						return;
					a_state.flags |= kFadeFlagFadedIn;
					return;
				}
			}
			// 0x14147a36a: fading out, or nothing.
			if ((a_state.flags & kFadeFlagFadedIn) && (alwaysOut || value < a_frame.fadeOutBelow)) {
				fadeOut = true;
			} else {
				const float current = a_state.currentFade;
				if (!(current > 0.0f) || !(current < one))
					return;
				if (!alwaysOut && !(value < a_frame.fadeInAbove))
					return;
				fadeOut = true;
			}
			if (fadeOut) {
				if (SinceVisible(a_state, a_frame) > 1 && type != 8) {
					a_state.currentFade = 0.0f;
				} else {
					const float fade = MaxSS(a_state.currentFade - MinSS(a_frame.deltaTime / a_frame.fadeOutTime, a_frame.stepMax), 0.0f);
					a_state.currentFade = fade;
					if (fade > 0.0f)
						return;
				}
				a_state.flags &= ~kFadeFlagFadedIn;
			}
		}

		/** @brief BSFadeNode::OnVisible for a process with cameraRelatedUpdates: whether it goes on into the children. */
		bool FadeNodeOnVisible(FadeNodeState& a_state, const FadeRootStatic& a_root, const float a_centre[3], const FadeFrame& a_frame)
		{
			const float amount = a_root.fadeAmount;
			if (!a_frame.fadesOn)
				return true;
			if ((a_state.flags & kFadeFlagSettled) && UnorderedEqual(a_frame.one, amount) && UnorderedEqual(a_frame.one, a_state.currentFade))
				return true;
			if (TypeOf(a_state) == 6 && !a_frame.overridden) {
				float distance = 0.0f;
				FadeValue(a_state, a_root, a_centre, a_frame, distance);
				if (!a_frame.lodUpdates) {
					a_state.flags |= kFadeFlagFadedIn;
					a_state.currentFade = 1.0f;
				}
				if (!(a_state.flags & kFadeFlagFadedIn)) {
					if (SinceVisible(a_state, a_frame) > 1) {
						a_state.currentFade = 1.0f;
						a_state.flags |= kFadeFlagFadedIn;
					} else {
						const float fade = MinSS(MinSS(a_frame.deltaTime / a_frame.fadeInTime, a_frame.stepMax) + a_state.currentFade, a_frame.one);
						a_state.currentFade = fade;
						if (!(fade < a_frame.one))
							a_state.flags |= kFadeFlagFadedIn;
					}
				}
				a_state.lastVisible = a_frame.counter;
				return true;
			}
			FadeUpdate(a_state, a_root, a_centre, amount, a_frame);
			a_state.lastVisible = a_frame.counter;
			return a_state.currentFade > 0.0f && !UnorderedEqual(amount, 0.0f);
		}

		std::uint32_t TreeLodBits(const RE::NiAVObject& a_node)
		{
			return TreeLodSelected(a_node) ? kFadeRootTreeLod : 0u;
		}
	}

	void AnimatedUpdate(FadeNodeState& a_state, const FadeRootStatic& a_root, const float a_centre[3], const FadeFrame& a_frame, std::uint32_t a_updates)
	{
		FadeFrame frame = a_frame;
		std::copy_n(a_frame.animEye, 3, frame.eye);
		frame.lodAdjust = a_frame.animLodAdjust;
		frame.counter = a_frame.animCounter;
		frame.deltaTime = a_frame.animDeltaTime;
		for (std::uint32_t u = 0; u < a_updates; ++u)
			FadeUpdate(a_state, a_root, a_centre, a_root.fadeAmount, frame);
	}

	std::uint32_t OnVisible(FadeNodeState& a_state, const FadeRootStatic& a_root, const float a_centre[3], const FadeFrame& a_frame)
	{
		std::uint32_t verdict = kFadeVerdictInView;
		const std::uint32_t plan = a_root.bits & kFadeRootPlanMask;
		if (plan == kFadeRootOther)
			return verdict | kFadeVerdictDrawn;
		// BSTreeNode::OnVisible: nothing at all above the height limit.
		if (plan == kFadeRootTree && a_centre[2] - a_frame.treeHeightBase > a_frame.treeHeightLimit)
			return verdict | kFadeVerdictAboveLimit;
		verdict |= kFadeVerdictServiced;
		// BSLeafAnimNode::OnVisible: its LOD step first.
		if ((plan == kFadeRootLeaf || plan == kFadeRootTree) && a_frame.lodUpdates) {
			float distance = 0.0f;
			FadeValue(a_state, a_root, a_centre, a_frame, distance);
			LodStep(a_state, a_root, distance, a_frame);
			a_state.lastVisible = a_frame.counter;
		}
		const bool drawn = FadeNodeOnVisible(a_state, a_root, a_centre, a_frame);
		// BSTreeNode::OnVisible's LOD fix-up.
		if (plan == kFadeRootTree && (a_root.bits & kFadeRootTreeLod)) {
			a_state.levels = (a_state.levels & ~0xFF00u) | ((((a_state.levels >> 8) & 0xAFu) | 0x20u) << 8);
			a_state.blend = 0.0f;
		}
		return verdict | (drawn ? kFadeVerdictDrawn : 0u);
	}

	FadeFrame SampleFrame(const RE::NiCamera* a_camera)
	{
		FadeFrame frame;
		if (a_camera) {
			frame.eye[0] = a_camera->world.translate.x;
			frame.eye[1] = a_camera->world.translate.y;
			frame.eye[2] = a_camera->world.translate.z;
			frame.lodAdjust = At<float>(a_camera, 0x184);
		}
		frame.counter = Global<std::int32_t>(0x2032e50);
		frame.deltaTime = Global<float>(0x2033084);
		frame.fadesOn = Global<std::uint8_t>(0x2032dfd) ? 1u : 0u;
		frame.lodUpdates = Global<std::uint8_t>(0x2032dfc) ? 1u : 0u;
		frame.fadeInTime = Global<float>(0x2032e2c);
		frame.fadeOutTime = Global<float>(0x2032e30);
		frame.fadeInAbove = Global<float>(0x2032e34);
		frame.fadeOutBelow = Global<float>(0x2032e38);
		frame.blendTime = Global<float>(0x2032e3c);
		frame.distanceMult = Global<float>(0x2032e48);
		frame.stepMax = Global<float>(0x2032e4c);
		frame.amountTime = Global<float>(0x332a214);
		frame.metricScale = Global<float>(0x1aa6300);
		frame.defaultScale = Global<float>(0x1ad2840);
		frame.metricOverride = Global<float>(0x332a254);
		frame.overridden = UnorderedEqual(Global<float>(0x332a254), Global<float>(0x1769578)) ? 0u : 1u;
		frame.lodFar = Global<float>(0x332a22c);
		frame.lodNear = Global<float>(0x332a25c);
		frame.treeLodFar = Global<float>(0x332a238);
		frame.treeLodNear = Global<float>(0x332a268);
		frame.lodMinimum = Global<float>(0x33dcfa8);
		frame.one = Global<float>(0x1ad2870);
		frame.amountSnapAbove = Global<float>(0x1ad2874);
		frame.amountSnapOffset = Global<float>(0x1ad288c);
		frame.snapRadiusLimit = Global<float>(0x1ad29f4);
		// BSTreeNode::OnVisible's height test as the list processes have it this frame (+infinity: off).
		const auto treeHeight = PrimaryCull::Get().TreeHeightTest();
		frame.treeHeightBase = treeHeight[0];
		frame.treeHeightLimit = treeHeight[1];
		for (std::uint32_t type = 0; type < 16; ++type)
			frame.divisors[type] = Global<float>(0x2032e00 + type * 4);
		return frame;
	}

	FadeNodeState ReadNode(const RE::NiAVObject& a_node)
	{
		FadeNodeState state;
		state.flags = At<std::uint32_t>(&a_node, 0xF4) & kFadeFlagMask;
		state.currentFade = At<float>(&a_node, 0x130);
		state.snapRadius = At<float>(&a_node, 0x134);
		state.lastVisible = At<std::int32_t>(&a_node, 0x13C);
		state.amountFade = At<float>(&a_node, 0x140);
		state.metric = At<float>(&a_node, 0x144);
		state.previousMetric = At<float>(&a_node, 0x148);
		state.blend = At<float>(&a_node, 0x14C);
		state.levels = At<std::uint8_t>(&a_node, 0x152) | (std::uint32_t(At<std::uint8_t>(&a_node, 0x153)) << 8);
		return state;
	}

	bool WriteNode(RE::NiAVObject& a_node, const FadeNodeState& a_state)
	{
		const float fadeBefore = At<float>(&a_node, 0x130);
		const std::uint8_t levelBefore = At<std::uint8_t>(&a_node, 0x152) & 0xF;
		std::atomic_ref<std::uint32_t> flags(At<std::uint32_t>(&a_node, 0xF4));
		if (a_state.flags & kFadeFlagFadedIn)
			flags.fetch_or(kFadeFlagFadedIn, std::memory_order_relaxed);
		else
			flags.fetch_and(~kFadeFlagFadedIn, std::memory_order_relaxed);
		At<float>(&a_node, 0x130) = a_state.currentFade;
		At<float>(&a_node, 0x134) = a_state.snapRadius;
		At<std::int32_t>(&a_node, 0x13C) = a_state.lastVisible;
		At<float>(&a_node, 0x140) = a_state.amountFade;
		At<float>(&a_node, 0x144) = a_state.metric;
		At<float>(&a_node, 0x148) = a_state.previousMetric;
		At<float>(&a_node, 0x14C) = a_state.blend;
		At<std::uint8_t>(&a_node, 0x152) = static_cast<std::uint8_t>(a_state.levels);
		At<std::uint8_t>(&a_node, 0x153) = static_cast<std::uint8_t>(a_state.levels >> 8);
		return a_state.currentFade != fadeBefore || (a_state.levels & 0xF) != levelBefore;
	}

	std::size_t NodeBytes(std::uint32_t a_plan)
	{
		return a_plan == kFadeRootTree ? 0x1B8 : a_plan == kFadeRootLeaf ? 0x170 : 0x158;
	}

	std::uint32_t PlanOf(const RE::NiAVObject& a_node)
	{
		const auto base = REL::Module::get().base();
		const auto onVisible = (*reinterpret_cast<const std::uintptr_t* const*>(&a_node))[kOnVisibleSlot];
		return onVisible == base + kFadeOnVisible ? kFadeRootFade :
		       onVisible == base + kLeafOnVisible ? kFadeRootLeaf :
		       onVisible == base + kTreeOnVisible ? kFadeRootTree : kFadeRootOther;
	}

	const RE::NiAVObject* TreeLodSwitch(const RE::NiAVObject& a_node)
	{
		if (PlanOf(a_node) != kFadeRootTree)
			return nullptr;
		return At<const RE::NiAVObject*>(&a_node, 0x180);
	}

	bool TreeLodSelected(const RE::NiAVObject& a_node)
	{
		const auto* lod = TreeLodSwitch(a_node);
		return lod && At<std::int32_t>(lod, 0x12C) > 0;
	}

	FadeRootStatic StaticOf(const RE::NiAVObject& a_node)
	{
		using Logf = float (*)(float);
		using Powf = float (*)(float, float);
		const auto base = REL::Module::get().base();
		FadeRootStatic row;
		row.initial = ReadNode(a_node);
		row.radius = At<float>(&a_node, 0xF0);
		row.fadeAmount = At<float>(&a_node, 0x100);
		row.nearDistance = At<float>(&a_node, 0x128);
		row.farDistance = At<float>(&a_node, 0x12C);
		row.object = 0;
		const std::uint8_t flags109 = At<std::uint8_t>(&a_node, 0x109);
		const bool tree = a_node.GetRTTI() == reinterpret_cast<const RE::NiRTTI*>(base + kTreeRtti);
		row.bits = PlanOf(a_node) | (std::uint32_t(flags109) << kFadeRootBitsShift) | TreeLodBits(a_node) | (tree ? kFadeRootTreeThresholds : 0u);
		const std::uint32_t objectFlags = At<std::uint32_t>(&a_node, 0xF4);
		row.bits |= ((objectFlags & (1u << 11)) ? kFadeRootAlwaysDraw : 0u) | ((objectFlags & (1u << 12)) ? kFadeRootPreprocessed : 0u) |
		            ((objectFlags & (1u << 20)) ? kFadeRootPreprocessHidden : 0u);
		// FUN_14147a430's scale, with the engine's own CRT, in its order.
		row.lodScale = 1.0f;
		if (flags109 & 2) {
			// The globals as the frame captured them (FrameGlobals: 0x2032e40, 0x2032e54, 0x2032e44).
			const auto& g = FrameGlobals::Current();
			const float exponent = reinterpret_cast<Logf>(base + kLogf)(row.radius / g.lodRadiusBase) * g.lodExponentScale;
			row.lodScale = reinterpret_cast<Powf>(base + kPowf)(g.lodPowBase, exponent);
		}
		return row;
	}

	std::string Differences(const FadeNodeState& a_expected, const FadeNodeState& a_actual)
	{
		std::string text;
		const auto field = [&](const char* a_name, auto a_e, auto a_a) {
			if (std::memcmp(&a_e, &a_a, sizeof(a_e)) != 0)
				text += fmt::format("{}{} {} (expected {})", text.empty() ? "" : ", ", a_name, a_a, a_e);
		};
		field("flags", a_expected.flags, a_actual.flags);
		field("currentFade", a_expected.currentFade, a_actual.currentFade);
		field("snapRadius", a_expected.snapRadius, a_actual.snapRadius);
		field("lastVisible", a_expected.lastVisible, a_actual.lastVisible);
		field("amountFade", a_expected.amountFade, a_actual.amountFade);
		field("metric", a_expected.metric, a_actual.metric);
		field("previousMetric", a_expected.previousMetric, a_actual.previousMetric);
		field("blend", a_expected.blend, a_actual.blend);
		field("levels", a_expected.levels & 0xFFFFu, a_actual.levels & 0xFFFFu);
		return text;
	}

	void CheckPort(const RE::NiAVObject& a_node, const RE::NiCamera& a_camera, PortCheck& a_check)
	{
		using FadeValueFn = float (*)(void*, const float*, float*);
		using FadeUpdateFn = void (*)(void*, float, const float*);
		using LodStepFn = void (*)(void*, float);
		const std::uint32_t plan = PlanOf(a_node);
		if (plan == kFadeRootOther)
			return;
		const auto base = REL::Module::get().base();
		const FadeFrame frame = SampleFrame(&a_camera);
		const FadeRootStatic root = StaticOf(a_node);
		const float centre[3] = { a_node.worldBound.center.x, a_node.worldBound.center.y, a_node.worldBound.center.z };
		if (plan == kFadeRootTree && centre[2] - frame.treeHeightBase > frame.treeHeightLimit)
			return;
		// The engine's functions on a copy: they read and write only the node (the LOD step's type test reads its vtable,
		// which the copy keeps) and the globals.
		const std::size_t size = NodeBytes(plan);
		alignas(16) std::byte scratch[kMaxNodeBytes];
		std::memcpy(scratch, &a_node, size);
		const float camera[4] = { frame.eye[0], frame.eye[1], frame.eye[2], frame.lodAdjust };
		SceneStore::MuteFadeEvents(true);
		{
			void* node = scratch;
			if ((plan == kFadeRootLeaf || plan == kFadeRootTree) && Global<std::uint8_t>(0x2032dfc)) {
				float distance = 0.0f;
				reinterpret_cast<FadeValueFn>(base + kFadeValue)(node, camera, &distance);
				reinterpret_cast<LodStepFn>(base + kLodStep)(node, distance);
				At<std::int32_t>(node, 0x13C) = frame.counter;
			}
			// BSFadeNode::OnVisible's body (0x141479f50); its type-6 branch is inline in the engine, so the port's own.
			const bool settled = !frame.fadesOn || ((At<std::uint32_t>(node, 0xF4) & kFadeFlagSettled) && UnorderedEqual(1.0f, At<float>(node, 0x100)) &&
																   UnorderedEqual(1.0f, At<float>(node, 0x130)));
			if (!settled) {
				if (TypeOf(ReadNode(*static_cast<const RE::NiAVObject*>(node))) == 6 && !frame.overridden) {
					FadeNodeState state = ReadNode(*static_cast<const RE::NiAVObject*>(node));
					FadeNodeOnVisible(state, root, centre, frame);
					At<float>(node, 0x130) = state.currentFade;
					At<std::uint32_t>(node, 0xF4) = (At<std::uint32_t>(node, 0xF4) & ~kFadeFlagMask) | state.flags;
					At<std::int32_t>(node, 0x13C) = state.lastVisible;
					At<float>(node, 0x144) = state.metric;
					At<float>(node, 0x148) = state.previousMetric;
				} else {
					reinterpret_cast<FadeUpdateFn>(base + kFadeUpdate)(node, At<float>(node, 0x100), camera);
					At<std::int32_t>(node, 0x13C) = frame.counter;
				}
			}
			if (plan == kFadeRootTree && (root.bits & kFadeRootTreeLod)) {
				At<std::uint8_t>(node, 0x153) = static_cast<std::uint8_t>((At<std::uint8_t>(node, 0x153) & 0xAF) | 0x20);
				At<float>(node, 0x14C) = 0.0f;
			}
		}
		SceneStore::MuteFadeEvents(false);
		const FadeNodeState engine = ReadNode(*reinterpret_cast<const RE::NiAVObject*>(scratch));
		FadeNodeState port = root.initial;
		OnVisible(port, root, centre, frame);
		++a_check.checked;
		if (const auto differences = Differences(engine, port); !differences.empty()) {
			++a_check.differ;
			if (a_check.first.empty())
				a_check.first = fmt::format("{} (plan {}): {}", static_cast<const void*>(&a_node), plan, differences);
		}
	}
}
