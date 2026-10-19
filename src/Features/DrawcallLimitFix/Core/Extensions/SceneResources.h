#pragma once

// The scene resources contract's Core side (Api/SceneResources.h): what a feature's setup, graph registration, requirement, fit,
// latched buffers and sealed set are handed, the calls Core makes where the scene's tables are made, registered with a graph, required,
// fitted, latched and sealed and where the revision's inputs are posted, and the lookup of a feature's state in the scene's tables.

#include "Features/DrawcallLimitFix/Api/SceneResources.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Registry.h"
#include "Features/DrawcallLimitFix/Core/Extensions/TableFamilies.h"

#include <ORGModuleServices/GrowableBufferSet.h>
#include <rhi.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace org
{
	class Buffer;
	class RenderGraph;
	class VersionedBuffer;
}

namespace DCLF
{
	struct SceneTables;
	namespace Draws
	{
		struct SceneBuffers;
	}
}

namespace DCLF::Draws
{
	/** @brief Every table family's rows unbounded (SceneFit::familyRows before a fit bounds them). */
	inline constexpr std::array<std::uint32_t, Extensions::kTableFamilySlots> kUnboundedFamilyRows = [] {
		std::array<std::uint32_t, Extensions::kTableFamilySlots> rows{};
		rows.fill(~0u);
		return rows;
	}();

	/**
	 * @brief What the named versions of the scene's buffers hold (the per-object buffers' objects): an object past them - its per-object
	 * rows, its table families' rows (the fade roots' among them), its vertex stream row's vertices - waits for the requirement that grows them, drawing nothing and
	 * culling nothing (ObjectFits). The payload's parts are not among them: it holds every row of its publication.
	 */
	struct SceneFit
	{
		std::uint32_t objects = ~0u;
		// The vertex stream's vertices (its scene resources' fit: Features/Faces' face positions), unbounded without one.
		std::uint32_t streamVertices = ~0u;
		// The table families' rows a draw reads by its reference, by registration (Extensions::TableFamilyColumns::objectRow): what the
		// features' scene resources hold of them (their fit: Features/TreeWind's wind entries), unbounded without one.
		std::array<std::uint32_t, Extensions::kTableFamilySlots> familyRows = kUnboundedFamilyRows;
		bool operator==(const SceneFit&) const = default;
	};
}

namespace DCLF::Api
{
	/**
	 * @brief Scene resources' setup (render thread, EnsureSceneBuffers): the device, and how the tables start. A versioned buffer is made
	 * with Draws::MakeVersioned (registered with the scene's set as it is made).
	 */
	class SceneResourcesSetup
	{
	public:
		rhi::Device device;
		bool smallStart = false;  // CS_DCLF_TABLE_START=small: the growable tables start with a few rows, so growth runs early and often

		/** @brief A structured buffer of a_elements a_stride-byte elements, named, materialized: its SRV is what shaders read it by. */
		std::shared_ptr<org::Buffer> StructuredBuffer(std::uint32_t a_elements, std::uint32_t a_stride, const char* a_name, bool a_unorderedAccess = false) const;
	};

	/** @brief Scene resources' graph registration (render thread, RegisterSceneBuffers): the graph, and the state setup made. */
	class SceneResourcesGraph
	{
	public:
		org::RenderGraph& graph;
		const std::shared_ptr<void>& state;

		template <class T>
		const T& State() const
		{
			return *static_cast<const T*>(state.get());
		}
	};

	/**
	 * @brief Scene resources' requirement (snapshot builder, RequireSceneTables): the revision's build, the publication's tables, and the
	 * words the registration's postInputs posted with the revision's inputs (SceneResources::inputWords; empty without).
	 */
	class SceneResourcesRequirements
	{
	public:
		org::services::GrowableBufferSet::Build& build;
		const SceneTables& tables;
		const std::shared_ptr<void>& state;
		std::span<const std::uint32_t> inputs;
		std::uint32_t objectRows = 0;  // already witnessed by RequireSceneTables; stable object-slot high-water mark

		template <class T>
		const T& State() const
		{
			return *static_cast<const T*>(state.get());
		}
	};

	/** @brief Scene resources' fit (SceneFitOf): the versions a set names, and the fit the feature's slots go into. */
	class SceneResourcesFit
	{
	public:
		const org::services::NamedVersions& sealed;
		const std::shared_ptr<void>& state;
		Draws::SceneFit& fit;

		template <class T>
		const T& State() const
		{
			return *static_cast<const T*>(state.get());
		}
	};

	/**
	 * @brief Scene resources' latched buffers (render thread: MainLatchedTargets, MainLatchedLayout): the state, and where each buffer
	 * the depth commit latches goes (Latch: a_bytes of it from offset 0; a versioned buffer by its key, copied into the adopted version).
	 */
	class SceneResourcesLatch
	{
	public:
		const std::shared_ptr<void>& state;
		// Core's: where Latch puts a target (the listed targets, or the layout's add), and its context.
		void (*sink)(void* a_context, const std::shared_ptr<org::Buffer>& a_buffer, const std::shared_ptr<org::VersionedBuffer>& a_versioned, std::size_t a_bytes);
		void* context;

		template <class T>
		const T& State() const
		{
			return *static_cast<const T*>(state.get());
		}
		void Latch(const std::shared_ptr<org::Buffer>& a_buffer, std::size_t a_bytes) const { sink(context, a_buffer, nullptr, a_bytes); }
		void Latch(const std::shared_ptr<org::VersionedBuffer>& a_buffer, std::size_t a_bytes) const { sink(context, nullptr, a_buffer, a_bytes); }
	};

	/** @brief Scene resources' sealed set (snapshot builder, MakeRevision): the versions a revision's sealed set names, and the state. */
	class SceneResourcesSealed
	{
	public:
		const org::services::NamedVersions& sealed;
		const std::shared_ptr<void>& state;

		template <class T>
		const T& State() const
		{
			return *static_cast<const T*>(state.get());
		}
	};
}

namespace DCLF::Extensions
{
	/** @brief The features' scene resources' states, by registration (Extensions::RegisteredSceneResources); a slot may be null. */
	using SceneResourceStates = std::vector<std::shared_ptr<void>>;
	/** @brief The words the registrations' postInputs post with the revision's inputs (Registry.h's kSceneResourceInputWords). */
	using SceneResourceInputs = std::array<std::uint32_t, kSceneResourceInputWords>;

	// Every call below but PostSceneResourceInputs runs the registrations in SceneResourcesOrder (Extensions/VertexStreams.h): the vertex
	// stream's first, the others in registration order.

	/** @brief Render thread (EnsureSceneBuffers): every registration's setup, in order (none after the vertex stream's giving no state). */
	SceneResourceStates SetUpSceneResources(rhi::Device a_device, bool a_smallStart);
	/** @brief Render thread (RegisterSceneBuffers): every state's buffers registered with a_graph, in order. */
	void RegisterSceneResources(org::RenderGraph& a_graph, const SceneResourceStates& a_states);
	/** @brief Snapshot builder (RequireSceneTables): every state's requirements for a_tables and the posted a_inputs, in order. */
	void RequireSceneResources(org::services::GrowableBufferSet::Build& a_build, const SceneTables& a_tables, const SceneResourceStates& a_states,
		const SceneResourceInputs& a_inputs);
	/** @brief Render thread (PostRevisionInputs): every registration's postInputs into its words of a_out (zeroed first), in order. */
	void PostSceneResourceInputs(SceneResourceInputs& a_out);
	/** @brief a_id's words of a_inputs (what its postInputs posted; empty: none, or no such registration). */
	std::span<const std::uint32_t> SceneResourceInputsOf(const SceneResourceInputs& a_inputs, Api::SceneResourcesId a_id);
	/** @brief Render thread (MainLatchedTargets, MainLatchedLayout): every state's latched buffers into a_sink, in order. */
	void LatchSceneResources(const SceneResourceStates& a_states,
		void (*a_sink)(void*, const std::shared_ptr<org::Buffer>&, const std::shared_ptr<org::VersionedBuffer>&, std::size_t), void* a_context);
	/** @brief SceneFitOf: what every state's buffers hold, as a_sealed names them, into a_fit. */
	void FitSceneResources(const org::services::NamedVersions& a_sealed, const SceneResourceStates& a_states, Draws::SceneFit& a_fit);
	/** @brief Snapshot builder (MakeRevision, once a revision's set is sealed, before any shape of it): every state's sealed, in order. */
	void SealSceneResources(const org::services::NamedVersions& a_sealed, const SceneResourceStates& a_states);

	/** @brief a_id's state in a_scene's tables (empty: none made, or no such registration). */
	const std::shared_ptr<void>& SceneResourcesStateOf(const Draws::SceneBuffers& a_scene, Api::SceneResourcesId a_id);
	/** @brief SceneResourcesStateOf as the feature's type. Its thread rules are the feature's (Api/SceneResources.h). */
	template <class T>
	T* SceneResourcesOf(const Draws::SceneBuffers& a_scene, Api::SceneResourcesId a_id)
	{
		return static_cast<T*>(SceneResourcesStateOf(a_scene, a_id).get());
	}
	/** @brief SceneResourcesOf, sharing the state's ownership (a pass or a stage state that holds it). */
	template <class T>
	std::shared_ptr<T> SharedSceneResourcesOf(const Draws::SceneBuffers& a_scene, Api::SceneResourcesId a_id)
	{
		return std::static_pointer_cast<T>(SceneResourcesStateOf(a_scene, a_id));
	}
}
