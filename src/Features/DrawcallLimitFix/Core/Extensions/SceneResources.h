#pragma once

// The scene resources contract's Core side (Api/SceneResources.h): what a feature's setup, graph registration, requirement and fit are
// handed, the calls Core makes where the scene's tables are made, registered with a graph, required and fitted, and the lookup of a
// feature's state in the scene's tables.

#include "Features/DrawcallLimitFix/Api/SceneResources.h"

#include <ORGModuleServices/GrowableBufferSet.h>
#include <rhi.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace org
{
	class Buffer;
	class RenderGraph;
}

namespace DCLF
{
	struct SceneTables;
	namespace Draws
	{
		struct SceneBuffers;
		struct SceneFit;
	}
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

	/** @brief Scene resources' requirement (snapshot builder, RequireSceneTables): the revision's build and the publication's tables. */
	class SceneResourcesRequirements
	{
	public:
		org::services::GrowableBufferSet::Build& build;
		const SceneTables& tables;
		const std::shared_ptr<void>& state;

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
}

namespace DCLF::Extensions
{
	/** @brief The features' scene resources' states, by registration (Extensions::RegisteredSceneResources); a slot may be null. */
	using SceneResourceStates = std::vector<std::shared_ptr<void>>;

	/** @brief Render thread (EnsureSceneBuffers): every registration's setup, in order. */
	SceneResourceStates SetUpSceneResources(rhi::Device a_device, bool a_smallStart);
	/** @brief Render thread (RegisterSceneBuffers): every state's buffers registered with a_graph, in order. */
	void RegisterSceneResources(org::RenderGraph& a_graph, const SceneResourceStates& a_states);
	/** @brief Snapshot builder (RequireSceneTables): every state's requirements for a_tables, in order. */
	void RequireSceneResources(org::services::GrowableBufferSet::Build& a_build, const SceneTables& a_tables, const SceneResourceStates& a_states);
	/** @brief SceneFitOf: what every state's buffers hold, as a_sealed names them, into a_fit. */
	void FitSceneResources(const org::services::NamedVersions& a_sealed, const SceneResourceStates& a_states, Draws::SceneFit& a_fit);

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
