#pragma once

// DCLF's extension API: a feature is a name and an install function that registers the extension points it fills. Api/ names only
// Api/, standard and ORG types; the contexts its callbacks receive are Core's (Core/Extensions/), which the features inside DCLF include.

#include "Features/DrawcallLimitFix/Api/Commits.h"
#include "Features/DrawcallLimitFix/Api/CoordinatorComponents.h"
#include "Features/DrawcallLimitFix/Api/DrawFamilies.h"
#include "Features/DrawcallLimitFix/Api/EngineEvents.h"
#include "Features/DrawcallLimitFix/Api/FrameBindings.h"
#include "Features/DrawcallLimitFix/Api/FrameValues.h"
#include "Features/DrawcallLimitFix/Api/GpuStages.h"
#include "Features/DrawcallLimitFix/Api/MainRanges.h"
#include "Features/DrawcallLimitFix/Api/ObjectContributors.h"
#include "Features/DrawcallLimitFix/Api/Payload.h"
#include "Features/DrawcallLimitFix/Api/Reports.h"
#include "Features/DrawcallLimitFix/Api/SceneResources.h"
#include "Features/DrawcallLimitFix/Api/Settings.h"
#include "Features/DrawcallLimitFix/Api/ShadowViews.h"
#include "Features/DrawcallLimitFix/Api/TableFamilies.h"
#include "Features/DrawcallLimitFix/Api/VertexStreams.h"
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
		/**
		 * @brief Fails the install for a register Core, another binding or a frame texture has, or past Extensions::kFrameBindingSlots
		 * bindings.
		 */
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
		/** @brief Fails the install for a name another channel has, no name or drain, or past Extensions::kEngineChannelSlots channels. */
		virtual EngineChannelId RegisterEngineChannel(const EngineChannel& a_channel) = 0;
		/** @brief The channel registered (by this feature or an earlier one) under a_name; none when no channel has it. */
		virtual EngineChannelId FindEngineChannel(std::string_view a_name) const = 0;
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
		 * @brief Fails the install for a map another feature has, without a registered toggle or its available flag, with a phase
		 * or view bit Core or another view has (or none, checked once all are in), or an ImportedTexture map without importTarget.
		 */
		virtual void RegisterOcclusionMap(const OcclusionMapContribution& a_map) = 0;
		/** @brief Fails the install for a source another feature has, or a Spot or Point source without lightKept. */
		virtual void RegisterShadowSource(const ShadowSourceContribution& a_source) = 0;
		/**
		 * @brief Fails the install for a family another registration has, a class with no registered toggle, or without its scene
		 * resources, its frame and main callbacks (decide, programSource, main's three) or with only some of its faces' five.
		 */
		virtual void RegisterDrawFamily(const DrawFamilyContribution& a_family) = 0;
		/**
		 * @brief Fails the install for a slot another registration has (Core's built-ins are in first), an order another range has, no
		 * name, a depth prepass on an appended slot, or a class with no registered toggle.
		 */
		virtual void RegisterMainRange(const MainRangeContribution& a_range) = 0;
		/**
		 * @brief Fails the install for a second stream (Core's draws have one second-stream slot), no name, select or upload, scene resources
		 * not registered before, or an entry class no component follows.
		 */
		virtual void RegisterVertexStream(const VertexStreamContribution& a_stream) = 0;
		/**
		 * @brief Fails the install for a class another registration has (or None), no name, geometrySource, beginRun, sampleChunk or endRun,
		 * or a rule without rows or units (minUnits past maxUnits; maxUnits not a power of two for a pow2 rule).
		 */
		virtual void RegisterPaletteRows(const PaletteRowsContribution& a_rows) = 0;
		/**
		 * @brief The material kind the port takes (Core's Lighting is 0; the ports from 1, in registration order), which its records carry.
		 * Fails the install for a shader type another port has or Lighting's, a class with no registered toggle, no property RTTI,
		 * material vtable, capture, derive, evaluate or shading, more than 64 variables a stage, or names not one a variable, or one port
		 * past Extensions::kMaterialKinds.
		 */
		virtual std::uint8_t RegisterMaterialPort(const MaterialPortContribution& a_port) = 0;
		/**
		 * @brief Fails the install for a family another registration has, Lighting's or no shader type, no name, key name or source path,
		 * or no program source; and at its end for a shader type no material port has.
		 */
		virtual void RegisterProgramFamily(const ProgramFamilyContribution& a_family) = 0;
		/**
		 * @brief Fails the install for a row outside Core's feature rows (Extensions::kFrameConstantRowFirst up to kFrameConstantRowsEnd),
		 * one another registration has, no name or no sample.
		 */
		virtual void RegisterFrameConstantRow(const FrameConstantRow& a_row) = 0;
		/**
		 * @brief Fails the install for a register below the frame textures (t16), past t127, Core's, a frame binding's or another frame
		 * texture's, no name or no view, or past Extensions::kFrameTextureSlots textures.
		 */
		virtual void RegisterFrameTexture(const FrameTexture& a_texture) = 0;

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
	 *   Extensions::InstallCameraHooks, after SunAccumulation's, before the primary cull's): the engine hooks that write a shadow
	 *   source's cameras into Core's boards (Core/Render/Shadow/ShadowCameras.h). Called once per source, in ShadowSource order (Sun,
	 *   Spot, Point, Focus; at each source the features in the list's order), with Core's light selection installed right after the
	 *   Sun's: a feature installs the hooks of a_source's cameras (none for a source it does not take), so the hooks' install order
	 *   (and the install log's) is the sources', whatever the features' places in the list. Registers nothing.
	 */
	struct Feature
	{
		std::string_view name;
		void (*install)(InstallContext& a_context) = nullptr;
		void (*earlyHooks)() = nullptr;
		void (*cameraHooks)(ShadowSource a_source) = nullptr;
	};
}
