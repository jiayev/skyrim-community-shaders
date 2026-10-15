#pragma once

// The scene's channels: every message between the render thread (Scene::FrameView) and the coordinator's passes (Scene::Coordinator).
// Each is lock-free and has one producer side and one consumer side; neither thread reads the other's state, only what was sent here.

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "Features/DrawcallLimitFix/Core/Extensions/Views.h"
#include "Features/DrawcallLimitFix/Core/Runtime/EventQueue.h"
#include "Features/DrawcallLimitFix/Core/Runtime/LatestSlot.h"
#include "Features/DrawcallLimitFix/Core/Runtime/PublicationLog.h"
#include "Features/DrawcallLimitFix/Core/Runtime/Retirement.h"
#include "Features/DrawcallLimitFix/Core/Scene/SceneWake.h"
#include "Features/DrawcallLimitFix/Engine/Capture/SceneEvents.h"
#include "Features/DrawcallLimitFix/Scene/Ports/ConstantEvaluator.h"
#include "Features/DrawcallLimitFix/Scene/Ports/GeometryPort.h"
#include "Features/DrawcallLimitFix/Scene/Model/Publication.h"
#include "Features/DrawcallLimitFix/Scene/Coordinator/SharedBindings.h"

namespace DCLF
{
	struct FrameGlobals;
}

namespace DCLF::Scene
{
	/**
	 * @brief What the coordinator's passes take of the frame, posted latest-wins by the render thread after BeginFrame's capture and taken
	 * at the start of each scene pass (TakeFrameInputs): the engine globals the passes read (FrameGlobals::Scope), the frame number
	 * (sceneFrame), the membership witness the binds read, the toggles generation and the verdicts generation (a toggle that enters the
	 * classification: the coordinator drops its cached verdicts). The post wakes the pump; a pass reads only what it took, and keeps the
	 * last it took while none newer is posted.
	 */
	struct FrameInputs
	{
		std::shared_ptr<const FrameGlobals> globals;
		std::uint32_t frame = 0;
		std::uint32_t membershipWitness = 0;
		std::uint32_t togglesGeneration = 0;
		std::uint32_t verdictsGeneration = 0;
		std::uint64_t lodForcedGeneration = 0;  // LodGates::ForcedGeneration at the frame's start (ScenePublication's)
		Extensions::AdoptionGenerations adoptionGuards{};  // the adoption guards' generations at the frame's start (ScenePublication's)
	};

	/** @brief Light Limit Fix's room map (its room nodes' indices), copied by the render thread when its generation moves. */
	struct RoomMapInput
	{
		std::shared_ptr<const ankerl::unordered_dense::map<const RE::NiNode*, int>> map;
		std::uint64_t generation = 0;
	};

	/**
	 * @brief The category nodes and the portal graphs' parentless roots, as the render thread found them (the scene work reads no TES,
	 * cell or portal graph). Immutable; it holds every node it names. Made again only when the category signature moved, a pass saw a
	 * detach (a detach can take a category node with it before the signature sees the cell go) or a load ended; RefreshCategoryNodes
	 * diffs its own set against the newest.
	 */
	struct CategoryCapture
	{
		std::uint64_t generation = 0;
		std::uint64_t signature = 0;
		ankerl::unordered_dense::set<RE::NiNode*> nodes;
		std::vector<RE::NiPointer<RE::NiNode>> held;
		struct Root
		{
			RE::NiPointer<RE::NiAVObject> root;
			RE::NiNode* category = nullptr;
		};
		ankerl::unordered_dense::map<const RE::NiAVObject*, Root> roots;
		// The category nodes new to the coordinator's set (all of them before a rescan): their children's keys.
		ankerl::unordered_dense::map<const RE::NiNode*, std::vector<const RE::NiAVObject*>> children;
		// Every node and geometry under its new category nodes and new roots, held for the coordinator's references (copies of these) until
		// it has applied a newer capture. "New" is against the capture the coordinator applied last, so each capture pins all it names.
		std::vector<RE::NiPointer<RE::NiRefObject>> pins;
		// By view, a CategoryRoot view's root (Api::ViewContribution::captureRoot: the player's first-person root while its toggle is on),
		// both a category node and a root (filed under itself), or null.
		std::array<const RE::NiNode*, Extensions::kEngineViews> viewRoots{};
	};

	/**
	 * @brief A fade node the coordinator holds no reference to (no pin, no root owner: a property whose fade node is not on its geometry's
	 * chain, set by another instance's clone of a shared model), asked of the render thread. It reads the geometry's property live where
	 * the engine reads it (the frame's start), checks the property still names that node and the geometry is in the world, takes a
	 * reference and posts it back. The request holds the geometry.
	 */
	struct FadeNodeRequest
	{
		RE::NiPointer<RE::BSGeometry> geometry;
		const void* fadeNode = nullptr;
	};
	struct FadeNodeAnswer
	{
		const RE::BSGeometry* geometry = nullptr;  // a key
		const void* fadeNode = nullptr;            // the requested key
		RE::NiPointer<RE::NiAVObject> node;        // null: the property names another node now, or the geometry left the world
	};

	/** @brief A pipeline's PerGeometry block the render thread made (RefreshFrameConstants), applied where the slot still holds its key. */
	struct PipelineConstantsPost
	{
		std::uint32_t slot = 0, binding = 0, generation = 0;
		PipelineKey key{};
		GeometryConstants constants{};
	};

	/**
	 * @brief What the coordinator tells PrimaryCull (render-thread state), in the order it made them: a hidden store's key (the kept scene
	 * lists built again), a member the joins dropped, every residency ended.
	 */
	struct PrimaryNote
	{
		enum class Kind : std::uint8_t
		{
			HiddenKey,
			MemberLost,
			AllMembersLost
		};
		Kind kind = Kind::HiddenKey;
		const void* key = nullptr;  // the hidden store's key, or the member's geometry (keys: nothing is dereferenced)
	};

	/**
	 * @brief A join's material with no capture held: the property the join read it from (its entry's, held), for the render thread to
	 * capture at EarlyPrepass when the property still has it.
	 */
	struct MaterialRequest
	{
		RE::NiPointer<RE::BSShaderProperty> property;
		const RE::BSShaderMaterial* material = nullptr;
		std::uint32_t pass = 0;
	};
	struct MaterialAnswer
	{
		const RE::BSShaderMaterial* material = nullptr;
		bool captured = false;  // false: the property held another material by then (the request stale)
	};

	/** @brief A fade root whose OnVisible DCLF services (PrimaryCull's admitted, stood-in entries' roots; kFadeRootOwned). */
	struct OwnedFadeRoot
	{
		const RE::NiAVObject* node = nullptr;
		bool standIn = false;  // kFadeRootStoodIn: no engine-drawn part
	};

	/**
	 * @brief A pass's request for the switch catch-ups (CollectEvents -> FrameView::ServeFrameRequests): its attached roots and its switch
	 * events, each a reference of its own, dropped on the render thread after the catch-ups.
	 */
	struct SwitchCatchUp
	{
		std::vector<RE::NiPointer<RE::NiAVObject>> attached;
		std::vector<SwitchEvent> switches;
	};

	/** @brief The joins' frame inputs since the last report: posted (render thread) and taken (coordinator), the blocks made from them. */
	struct AccumulateInputCounters
	{
		std::atomic<std::uint64_t> roomMapsPosted{ 0 }, roomMapsTaken{ 0 }, pipelineFramesPosted{ 0 }, pipelineFramesTaken{ 0 };
		std::atomic<std::uint64_t> pipelineBlocksMade{ 0 }, pipelineBlocksWaited{ 0 };
	};

	struct Channels
	{
		static Channels& Get()
		{
			static Channels channels;
			return channels;
		}

		// Render thread -> coordinator.
		LatestSlot<FrameInputs> frameInputs;
		LatestSlot<std::shared_ptr<const CategoryCapture>> categoryPosted;
		std::atomic<std::shared_ptr<const GeometryPort::PipelineFrame>> pipelineFramePosted;
		std::atomic<std::shared_ptr<const RoomMapInput>> roomMapPosted;
		std::atomic<std::shared_ptr<const SharedBindings::ProjectedCapture>> projectedPosted;
		// The fade root rows the depth commit holds (latest wins), PrimaryCull's fade ownership (a whole set: latest wins).
		static constexpr std::uint64_t kNoFadeRootsSent = ~0ull;
		std::atomic<std::uint64_t> fadeRootsSentPosted{ kNoFadeRootsSent };
		LatestSlot<std::vector<OwnedFadeRoot>> fadeOwnedPosted;
		EventQueue<std::unique_ptr<PipelineConstantsPost>, 256> pipelineConstantsPosted;
		// A load screen's marker, posted once a load after loadingSeen is raised: the coordinator drops what the load invalidates
		// (ApplyLoading); while loadingSeen holds, a pass drains the engine's queues into the mirror's carry.
		EventQueue<bool, 64> loadMarkers{ &WakeSceneLoadMarker };
		std::atomic<bool> loadingSeen{ false };
		// The answers to the coordinator's requests.
		EventQueue<MaterialAnswer> materialRequestsAnswered{ &WakeSceneMaterialAnswer };
		EventQueue<FadeNodeAnswer, 256> fadeNodeAnswers;
		// The publications the frame replaced or skipped, dropped by the coordinator at its next pass.
		Retired<ScenePublication> retiredPublications;

		// Coordinator -> render thread.
		PublicationLog<PublicationDeltas> publicationLog;  // every publication's deltas, walked by the frame's start
		EventQueue<PrimaryNote> primaryNotes;
		EventQueue<MaterialRequest> materialRequests;
		EventQueue<FadeNodeRequest, 256> fadeNodeRequests;
		// Tracked geometries the mirror held no record of, captured by the render thread at the next frame's start.
		EventQueue<RE::NiPointer<RE::NiAVObject>, 256> mirrorCaptureRequests;
		EventQueue<std::shared_ptr<SwitchCatchUp>, 64> switchCatchUps;
		// The capture RefreshCategoryNodes last diffed against: which captures the render thread may let go of, and what is new to the
		// coordinator's set.
		std::atomic<std::uint64_t> categoryAppliedGeneration{ 0 };
		std::atomic<bool> categoryDetachSeen{ false };  // a pass drained a detach: the next frame start's category capture is forced

		// Counted on both sides, taken by the reports.
		AccumulateInputCounters accumulateInputs;
		std::atomic<std::uint64_t> catchUpsBySwitch{ 0 }, catchUpsByAttach{ 0 }, catchUpNs{ 0 };
	};
}
