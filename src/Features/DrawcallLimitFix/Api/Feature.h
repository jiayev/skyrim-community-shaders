#pragma once

// DCLF's extension API: a feature is a name and an install function that registers the extension points it fills. Api/ names only
// Api/, standard and ORG types; the contexts its callbacks receive are Core's (Core/Extensions/), which the features inside DCLF include.

#include "Features/DrawcallLimitFix/Api/Commits.h"
#include "Features/DrawcallLimitFix/Api/CoordinatorComponents.h"
#include "Features/DrawcallLimitFix/Api/EngineEvents.h"
#include "Features/DrawcallLimitFix/Api/FrameBindings.h"
#include "Features/DrawcallLimitFix/Api/FrameValues.h"
#include "Features/DrawcallLimitFix/Api/GpuStages.h"
#include "Features/DrawcallLimitFix/Api/ObjectContributors.h"
#include "Features/DrawcallLimitFix/Api/Payload.h"
#include "Features/DrawcallLimitFix/Api/Reports.h"
#include "Features/DrawcallLimitFix/Api/SceneResources.h"
#include "Features/DrawcallLimitFix/Api/Settings.h"
#include "Features/DrawcallLimitFix/Api/ShadowViews.h"
#include "Features/DrawcallLimitFix/Api/TableFamilies.h"
#include "Features/DrawcallLimitFix/Api/Views.h"

#include <string_view>

namespace DCLF::Api
{
	/**
	 * @brief What a feature registers with (Feature::install), implemented by Core (Extensions::InstallFeatures). Valid only during the
	 * install call: once, on the main thread at PostPostLoad, before DCLF's threads start. What is registered is read-only after, and
	 * runs in registration order (the features in FeatureList's order, a feature's registrations in its call order). The descriptors are
	 * copied (their strings too); their callbacks are plain functions, called per phase or per frame, never per object.
	 *
	 * Install also installs the feature's engine hooks (the engine is not running DCLF's frames yet; the hooks DCLF's own engine side
	 * installed are in) and subscribes its observers (Diagnostics::Subscribe, under the same Observe() flags as Core's), after Core's.
	 */
	class InstallContext
	{
	public:
		virtual FrameValuePoolId RegisterFrameValuePool(const FrameValuePool& a_pool) = 0;
		virtual void RegisterGpuStage(const GpuStage& a_stage) = 0;
		virtual void RegisterPayloadProduct(const PayloadProduct& a_product) = 0;
		virtual SceneResourcesId RegisterSceneResources(const SceneResources& a_resources) = 0;
		virtual void RegisterCommitHook(const CommitHook& a_hook) = 0;
		/** @brief Fails the install for a register Core or another binding has, or past Extensions::kFrameBindingSlots bindings. */
		virtual void RegisterFrameBinding(const FrameBinding& a_binding) = 0;
		virtual SwitchId RegisterSwitch(const FeatureSwitch& a_switch) = 0;
		/**
		 * @brief Fails the install past the toggle word's bits, for a toggle after one Core does not have, or needing one not listed
		 * before it.
		 */
		virtual ToggleId RegisterToggle(const FeatureToggle& a_toggle) = 0;
		virtual void RegisterReportLines(const ReportLines& a_lines) = 0;
		/** @brief Whether the primary cull is installed: false, and the observer is never called, without it (its engine hooks failed). */
		virtual bool RegisterStandInObserver(const StandInObserver& a_observer) = 0;
		/** @brief Fails the install past Extensions::kTableFamilySlots families. */
		virtual TableFamilyId RegisterTableFamily(const TableFamily& a_family) = 0;
		virtual CoordinatorComponentId RegisterCoordinatorComponent(const CoordinatorComponent& a_component) = 0;
		/** @brief Fails the install for a technique another feature has, past Extensions::kTechniqueSlots, or without a registered toggle. */
		virtual void RegisterTechnique(const TechniqueContribution& a_technique) = 0;
		/** @brief Fails the install for a type another feature has, past Extensions::kGeometryTypeSlots, or without a registered toggle. */
		virtual void RegisterGeometryType(const GeometryTypeContribution& a_type) = 0;
		/** @brief Fails the install for a parent and leaves another feature has, or without a registered toggle. */
		virtual void RegisterParent(const ParentContribution& a_parent) = 0;
		/** @brief Fails the install for a class another feature has, or without a registered toggle. */
		virtual void RegisterObjectClass(const ObjectClassContribution& a_class) = 0;
		virtual void RegisterShading(const ShadingContribution& a_shading) = 0;
		/** @brief Fails the install past Extensions::kFrameGlobalsFloats floats in all. */
		virtual FrameGlobalsSlot RegisterFrameGlobals(const FrameGlobalsContribution& a_globals) = 0;
		/**
		 * @brief Fails the install for a view another feature has, without a registered toggle, with a phase or view bit Core or another
		 * view has (or none), without covered, a CategoryRoot view without captureRoot, or an InMainRenderers view without accumulators.
		 */
		virtual void RegisterView(const ViewContribution& a_view) = 0;
		/** @brief Fails the install for no view, no point or no function, or a view no feature contributes. */
		virtual void RegisterViewWork(const ViewWork& a_work) = 0;
		/**
		 * @brief Fails the install for a map another feature (or Core) has, without a registered toggle or its available flag, with a phase
		 * or view bit Core or another view has (or none, checked once all are in), or an ImportedTexture map without importTarget.
		 */
		virtual void RegisterOcclusionMap(const OcclusionMapContribution& a_map) = 0;
		/** @brief Fails the install for a source another feature (or Core) has, or a Spot or Point source without lightKept. */
		virtual void RegisterShadowSource(const ShadowSourceContribution& a_source) = 0;

	protected:
		~InstallContext() = default;
	};

	/**
	 * @brief A feature DCLF installs (Features/FeatureList.cpp).
	 *
	 * - install: see InstallContext.
	 * - earlyHooks (optional): the main thread at PostPostLoad, before DCLF's own engine side and observers install (the shell's
	 *   Extensions::InstallEarlyHooks, in the list's order): engine hooks that must be in before Core's engine side reads what they
	 *   change (a vtable slot the primary cull takes at its install; the source of an engine event channel Core follows, after Core's
	 *   own scene events are in). Registers nothing; Core's switches and Diagnostics::Observe are readable.
	 * - cameraHooks (optional): the main thread at PostPostLoad, where the shadow cameras' hooks install (the shell's
	 *   Extensions::InstallCameraHooks, after SunAccumulation's, in the list's order after Core's own): the engine hooks that write a
	 *   shadow source's cameras into Core's boards (Core/Render/Shadow/ShadowCameras.h). Registers nothing.
	 */
	struct Feature
	{
		std::string_view name;
		void (*install)(InstallContext& a_context) = nullptr;
		void (*earlyHooks)() = nullptr;
		void (*cameraHooks)() = nullptr;
	};
}
