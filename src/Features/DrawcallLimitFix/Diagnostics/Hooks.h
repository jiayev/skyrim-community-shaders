#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

struct ID3D11Texture2D;
namespace RE
{
	class BSRenderPass;
}

namespace DCLF::Diagnostics
{
	/**
	 * @brief The hook points: where the main path announces what happened, for the observers (parity checks, probes, dumps) to look.
	 *
	 * The main path reaches the diagnostics only three ways: Observe() (Observation.h: which subjects this run watches), Policy::
	 * (Policy.h: the questions that change its work, whose normal-path answer is the switches-off one), and the hook points here
	 * (Notify, Armed; observers Subscribe at Install). It never reads a parity switch itself.
	 *
	 * A hook point is an event type and a Notify call. Notify costs one flag test while nothing observes the event; the observers
	 * register at Install (before the first frame, before DCLF's threads start), and the registry is read-only from then on, so
	 * Notify takes no lock. An observer runs on the thread and at the point of its Notify, and must not change what the main path
	 * does: a change is a Policy question (Policy.h). Area events are declared beside their observers (Diagnostics/<Area>/Events.h).
	 *
	 * Switchable observers (SubscribeSwitchable: the compatibility monitor's, PlayerParity.h) are registered at Install too, but run
	 * only while SetSwitchableArmed has them on: the render thread flips them at the frame's start, and an event with none on and no
	 * other observer costs its one flag test again.
	 */
	namespace Detail
	{
		template <class Event>
		inline std::atomic<bool> armed{ false };
		template <class Event>
		inline std::vector<void (*)(const Event&)> observers;
		template <class Event>
		inline std::atomic<bool> switchableArmed{ false };
		template <class Event>
		inline std::vector<void (*)(const Event&)> switchableObservers;
		// Per event type with switchable observers: sets its switchable flag and its armed one.
		inline std::vector<void (*)(bool)> switchableArms;
	}

	/** @brief Whether anything observes Event: a hook point whose payload costs something to make tests this first. */
	template <class Event>
	inline bool Armed()
	{
		return Detail::armed<Event>.load(std::memory_order_relaxed);
	}

	/** @brief A hook point: a_event to every observer of its type, or nothing. */
	template <class Event>
	inline void Notify(const Event& a_event)
	{
		if (Detail::armed<Event>.load(std::memory_order_relaxed)) [[unlikely]] {
			for (auto* observer : Detail::observers<Event>)
				observer(a_event);
			if (Detail::switchableArmed<Event>.load(std::memory_order_relaxed))
				for (auto* observer : Detail::switchableObservers<Event>)
					observer(a_event);
		}
	}

	/**
	 * @brief A hook point that carries nothing but which point it is: Point, an enumerator of a Point enum (FramePoint below, the areas'
	 * in their Events.h), is the event type. Its observers read what they look at from the modules (the frame from FrameView, the
	 * coordinator's state through Diagnostics/Scene's SceneView).
	 */
	template <auto Point>
	struct On
	{
	};

	/** @brief A hook point of an On event: Notify<FramePoint::Present>(). */
	template <auto Point>
	inline void Notify()
	{
		Notify(On<Point>{});
	}

	/** @brief Install only: a_observer runs at every hook point of Event. */
	template <class Event>
	void Subscribe(void (*a_observer)(const Event&))
	{
		Detail::observers<Event>.push_back(a_observer);
		Detail::armed<Event>.store(true, std::memory_order_relaxed);
	}

	/** @brief Install only: a_observer runs at every hook point of Event while SetSwitchableArmed has the switchable observers on. */
	template <class Event>
	void SubscribeSwitchable(void (*a_observer)(const Event&))
	{
		if (Detail::switchableObservers<Event>.empty())
			Detail::switchableArms.push_back([](bool a_on) {
				Detail::switchableArmed<Event>.store(a_on, std::memory_order_relaxed);
				Detail::armed<Event>.store(a_on || !Detail::observers<Event>.empty(), std::memory_order_relaxed);
			});
		Detail::switchableObservers<Event>.push_back(a_observer);
	}

	/** @brief Install only: a_observer runs at every On<Point> hook point while the switchable observers are on. */
	template <auto Point>
	void SubscribeSwitchable(void (*a_observer)(const On<Point>&))
	{
		SubscribeSwitchable<On<Point>>(a_observer);
	}

	/** @brief Render thread, the frame's start: every switchable observer on or off (PlayerParity::BeginFrame). */
	inline void SetSwitchableArmed(bool a_on)
	{
		for (auto* arm : Detail::switchableArms)
			arm(a_on);
	}

	/** @brief Install only: a_observer runs at every On<Point> hook point. */
	template <auto Point>
	void Subscribe(void (*a_observer)(const On<Point>&))
	{
		Subscribe<On<Point>>(a_observer);
	}

	/**
	 * @brief The observers this run's switches ask for, registered (each area's Install*), and the engine hooks only observers use
	 * installed. PostPostLoad, before DCLF's threads start.
	 */
	void Install();

	// The frame's events (render thread, in frame order).

	/** @brief The frame's points (On<FramePoint::...>), in frame order. */
	enum class FramePoint : std::uint8_t
	{
		LookupsInstalled,   // the frame's lookups are the installed publication's, before the scene lane's inputs are posted
		ClaimsInstalled,    // the frame's claims are installed (none before DCLF's first covered frame), before the culls
		ShadowViewsBegin,   // BeforeShadowMaps (the frame's shadow views already listed, when CalculateAndDrawShadowCasterLights returned)
		RegistrationsDone,  // EarlyPrepass: the frame's registrations drained (the registration jobs done)
		PrepassEnd,         // Prepass, DCLF running, after the frame constants are refreshed
		Present,            // Present, every frame (DCLF running or not), first thing
	};

	/** @brief Main::Draw's early hook, every frame, with DCLF running or not. */
	struct FrameBegin
	{
		bool running;
	};

	/** @brief Prepass, every frame (DCLF running or not), before DCLF's prepass work. */
	struct PrepassBegin
	{
		bool running;
	};

	/** @brief A native Lighting draw, after its SetupGeometry (DCLF running). */
	struct NativeLightingDraw
	{
		RE::BSRenderPass* pass;
		std::uint32_t renderFlags;
		std::uint32_t frame;
		bool deferred;          // inside the deferred pass
		bool mainPassCaptured;  // the colour epoch's bindings were captured this frame (BeforeOpaquePass)
	};

	/** @brief A native pass the batch renderers are about to draw while DCLF runs: a set member's in the main views is a leak. */
	struct NativePassOffered
	{
		const RE::BSRenderPass* pass;
		std::uint32_t technique;
		bool inDepthPass;
	};

	/** @brief A native pass the batch renderers drew (BSBatchRenderer::RenderPassImmediately returned; DCLF running or not). */
	struct NativePassDrawn
	{
		const RE::BSRenderPass* pass;
		bool inDepthPass;
	};

	/** @brief An occlusion map's reference render by the engine (a_map 0 Skylighting's, 1 the precipitation mask), before the map is cleared for DCLF's. */
	struct OcclusionReferenceRendered
	{
		std::uint32_t map;
	};

	/** @brief DCLF's shadow epoch drew the occlusion maps in a_drawn (bit per map), or none: every frame it runs, after it. */
	struct OcclusionMapsDrawn
	{
		std::uint32_t drawn;
	};

	/** @brief A named point of the frame where the bound targets may be looked at (a_extra: another texture to keep). */
	struct TargetPoint
	{
		const char* label;
		ID3D11Texture2D* extra = nullptr;
	};

}
