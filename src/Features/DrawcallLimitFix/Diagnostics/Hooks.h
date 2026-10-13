#pragma once

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
	 * A hook point is an event type and a Notify call. Notify costs one flag test while nothing observes the event; the observers
	 * register at Install (before the first frame, before DCLF's threads start), and the registry is read-only from then on, so
	 * Notify takes no lock. An observer runs on the thread and at the point of its Notify, and must not change what the main path
	 * does: a change is a Policy question (Policy.h). Area events are declared beside their observers (Diagnostics/<Area>/Events.h).
	 */
	namespace Detail
	{
		template <class Event>
		inline bool armed = false;
		template <class Event>
		inline std::vector<void (*)(const Event&)> observers;
	}

	/** @brief Whether anything observes Event: a hook point whose payload costs something to make tests this first. */
	template <class Event>
	inline bool Armed()
	{
		return Detail::armed<Event>;
	}

	/** @brief A hook point: a_event to every observer of its type, or nothing. */
	template <class Event>
	inline void Notify(const Event& a_event)
	{
		if (Detail::armed<Event>) [[unlikely]]
			for (auto* observer : Detail::observers<Event>)
				observer(a_event);
	}

	/** @brief Install only: a_observer runs at every hook point of Event. */
	template <class Event>
	void Subscribe(void (*a_observer)(const Event&))
	{
		Detail::observers<Event>.push_back(a_observer);
		Detail::armed<Event> = true;
	}

	/**
	 * @brief The observers this run's switches ask for, registered (each area's Install*), and the engine hooks only observers use
	 * installed. PostPostLoad, before DCLF's threads start.
	 */
	void Install();

	// The frame's events (render thread, in frame order).

	/** @brief Main::Draw's early hook, every frame, with DCLF running or not. */
	struct FrameBegin
	{
		bool running;
	};

	/** @brief The frame's claims are installed (or withdrawn), before the culls. */
	struct ClaimsInstalled
	{
		std::uint32_t frame;
	};

	/** @brief The frame's lookups are the installed publication's, before the scene lane's inputs are posted. */
	struct LookupsInstalled
	{
		std::uint32_t frame;
	};

	/** @brief BeforeShadowMaps, before the frame's shadow views are listed. */
	struct ShadowViewsBegin
	{
		std::uint32_t frame;
	};

	/** @brief Prepass, every frame (DCLF running or not), before DCLF's prepass work. */
	struct PrepassBegin
	{
		bool running;
	};

	/** @brief Prepass, DCLF running, after the frame constants are refreshed. */
	struct PrepassEnd
	{
		std::uint32_t frame;
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

	/** @brief A native draw (State::Draw's dirty states applied): its pass (0x100 the depth prepass, 0x101 the deferred main pass, else the accumulator's render mode, 0xFF none), shader type and technique. */
	struct NativeDraw
	{
		std::uint32_t pass;
		std::uint32_t shaderType;
		std::uint32_t technique;
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

	/** @brief DCLF's occlusion epoch drew the maps in a_drawn (bit per map), or none. */
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

	/** @brief Present, every frame (DCLF running or not), first thing. */
	struct Present
	{
	};
}
