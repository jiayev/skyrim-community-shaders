#pragma once

#include <array>
#include <cstdint>

namespace DCLF
{
	/**
	 * @brief T6b3d: what woke the coordinator's pump (SceneStore's scene passes, on the scene lane), by source - the report counts the
	 * passes each source woke and those of them that changed nothing. A pass runs only for an input whose effect is wanted before the
	 * frame's own pass (the FrameInput one, once a frame): an attach in the world, a detach, a hidden bit, a leaf's property or alpha
	 * swapped, a property's or alpha's values, a property event (flags, material, controller), a switch, the answers a waiting join or
	 * binding needs, a new catalog, a load. The hidden, leaf, value and property sources wake only outside the frame's render (the
	 * EngineReadWindow, Main::Draw to Present): there the engine sets and undoes them for its own views (GetRenderPasses leaves each
	 * camera's alpha on the property, the reflection faces hide the water, Main::Draw hides the first-person skeleton), and the frame's
	 * next pass takes them (w130: 70-80% of the passes they woke changed nothing). What the frame's pass takes anyway does not wake it
	 * (w127: 7539 of 9697 passes changed nothing): the per-frame queues (moves, fades, fade snaps and amounts, node transforms and
	 * controllers, LOD fades, emittance, shading), object LOD's segment writes (the terrain manager refreshes them as the camera moves,
	 * mostly to the ranges held), the node and geometry value updates, an attach out of the world (a loader's subtree: its world attach
	 * wakes), the material writers' captures (controllers write every frame), the render thread's mid-frame posts (the registration
	 * drain, the room map, the pipeline frame and blocks, the fade posts).
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
		LoadMarker,      // a load screen's marker
		LoadDrain,       // a Present or frame start under a load screen (the queues drained once a call)
		Capture,         // the render thread's captures for the pass (the category capture, the mirror probe)
		Gate,            // T6b5: a LOD gate captured (the tracker's Gate event), or its outcome posted (LodGates: a flip, a forced release)
		Count
	};
	inline constexpr std::array<const char*, static_cast<std::size_t>(SceneWake::Count)> kSceneWakeNames{ "frame input", "attach", "detach",
		"hidden", "leaf swap", "property values", "alpha values", "property event", "switch", "material answer", "texture reply",
		"catalog", "load marker", "load drain", "captures", "LOD gate" };

	/**
	 * @brief A producer's wake (any thread, after the push it announces). Lock-free and cheap when its source is already pending (one
	 * relaxed load); level-triggered: a wake while a pass runs queues one more. Nothing before SceneStore::StartScenePump, nothing while
	 * the passes run inline (the parities: SceneStore::SetScenePassMode), and no engine event's wake while a load screen is up
	 * (SetScenePassLoading: the passes then drain once a Present, as the old ingestion did).
	 */
	void WakeScenePass(SceneWake a_source);
	/** @brief EventQueue's on-push hooks (a function each, by source). */
	inline void WakeScenePropertyEvent() { WakeScenePass(SceneWake::PropertyEvent); }
	inline void WakeSceneSwitchEvent() { WakeScenePass(SceneWake::SwitchEvent); }
	inline void WakeSceneMaterialAnswer() { WakeScenePass(SceneWake::MaterialAnswer); }
	inline void WakeSceneTextureReply() { WakeScenePass(SceneWake::TextureReply); }
	inline void WakeSceneLoadMarker() { WakeScenePass(SceneWake::LoadMarker); }
	/**
	 * @brief T6b5: a LOD gate's outcome posted (LodGates' forced releases call it after the post; the render thread's flips are woken by
	 * SceneStore::ReleaseGates). Not an engine event for the load screen's hold: the first pass after a load takes the outcomes.
	 */
	inline void WakeSceneGate() { WakeScenePass(SceneWake::Gate); }
	/** @brief Render thread (SceneStore::NoteLoadingScreen): a load screen is up (the engine events' wakes held), or not. */
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
