#pragma once

#include <array>
#include <cstdint>

namespace DCLF
{
	/**
	 * @brief What woke the coordinator's scene pump (its passes run on the scene lane), by source; the report counts the passes each
	 * source woke and those that changed nothing. A pass runs only for an input whose effect is wanted before the frame's own pass
	 * (FrameInput, once a frame): a world attach, a detach, a hidden bit, a leaf's property or alpha swapped, property or alpha values,
	 * a property event, a switch, an answer a waiting binding needs, a new catalog, a load.
	 *
	 * The hidden, leaf, value and property sources wake only outside the frame's render (the EngineReadWindow, Main::Draw to Present):
	 * inside it the engine sets and undoes them for its own views (GetRenderPasses leaves each camera's alpha on the property, the
	 * reflection faces hide the water, Main::Draw hides the first-person skeleton), and the frame's next pass takes them. What the
	 * frame's pass takes anyway does not wake it: the per-frame queues (moves, fades, node transforms and controllers, LOD fades,
	 * emittance, shading), object LOD's segment writes (the terrain manager refreshes them as the camera moves), node and geometry value
	 * updates, an attach out of the world (a loader's subtree: its world attach wakes), the material writers' captures (controllers
	 * write every frame), and the render thread's mid-frame posts.
	 */
	enum class SceneWake : std::uint8_t
	{
		FrameInput,      // the frame inputs and the ahead context (once a frame)
		Attach,          // the tracker: an attach in the world
		Detach,          // the tracker: a detach
		Hidden,          // the tracker: a hidden bit store
		Leaf,            // the tracker: a geometry's property or alpha swapped
		PropertyUpdate,  // the tracker: a property's values
		AlphaUpdate,     // the tracker: an alpha property's values
		PropertyEvent,   // a property's flags, material or controller (SceneEvents)
		SwitchEvent,     // a switch's selection (the render thread's)
		MaterialAnswer,  // a requested material captured and answered (ServeMaterialRequests)
		TextureReply,    // a binding's texture answered (GpuTextures)
		Catalog,         // the pipeline lane's new catalog
		LoadDrain,       // a Present or frame start under a load screen (the queues drained once a call)
		Capture,         // the render thread's captures for the pass (the category capture, the mirror probe)
		Channel,         // an engine channel's wake (Api::WakeSource::Pass: Extensions::WakeChannel, its SceneStack events; the LOD gates')
		Count
	};
	inline constexpr std::array<const char*, static_cast<std::size_t>(SceneWake::Count)> kSceneWakeNames{ "frame input", "attach", "detach",
		"hidden", "leaf swap", "property values", "alpha values", "property event", "switch", "material answer", "texture reply",
		"catalog", "load drain", "captures", "LOD gate" };

	/**
	 * @brief A producer's wake (any thread, after the push it announces). Lock-free and cheap when its source is already pending (one
	 * relaxed load); level-triggered: a wake while a pass runs queues one more. Nothing before Coordinator::StartScenePump, nothing while
	 * the passes run inline (the parities: Coordinator::SetScenePassMode), and no engine event's wake while a load screen is up
	 * (SetScenePassLoading: the passes then drain once a Present).
	 */
	void WakeScenePass(SceneWake a_source);
	/** @brief EventQueue's on-push hooks (a function each, by source). */
	inline void WakeScenePropertyEvent() { WakeScenePass(SceneWake::PropertyEvent); }
	inline void WakeSceneSwitchEvent() { WakeScenePass(SceneWake::SwitchEvent); }
	inline void WakeSceneMaterialAnswer() { WakeScenePass(SceneWake::MaterialAnswer); }
	inline void WakeSceneTextureReply() { WakeScenePass(SceneWake::TextureReply); }
	/** @brief Render thread (FrameView::NoteLoadingScreen): a load screen is up (the engine events' wakes held), or not. */
	void SetScenePassLoading(bool a_loading);
	/** @brief Since the last call: the pushes each source made inside the frame's render (no wake: the frame's next pass took them). */
	std::array<std::uint64_t, static_cast<std::size_t>(SceneWake::Count)> TakeScenePassDeferred();

	/**
	 * @brief The wakes this thread makes while one lives are folded into one, made when the outermost goes (the frame's start posts the
	 * frame inputs, the ahead context and the captures: one pass takes them all).
	 */
	class ScenePassWakeBatch
	{
	public:
		ScenePassWakeBatch();
		~ScenePassWakeBatch();
		ScenePassWakeBatch(const ScenePassWakeBatch&) = delete;
		ScenePassWakeBatch& operator=(const ScenePassWakeBatch&) = delete;
	};
}
