#pragma once

// DCLF's extension API: a feature is a name and an install function that registers the extension points it fills. Api/ names only
// Api/, standard and ORG types; the contexts its callbacks receive are Core's (Core/Extensions/), which the features inside DCLF include.

#include "Features/DrawcallLimitFix/Api/AncestorValues.h"
#include "Features/DrawcallLimitFix/Api/ClassifyInputs.h"
#include "Features/DrawcallLimitFix/Api/Commits.h"
#include "Features/DrawcallLimitFix/Api/CoordinatorComponents.h"
#include "Features/DrawcallLimitFix/Api/CullInputs.h"
#include "Features/DrawcallLimitFix/Api/DepthPyramid.h"
#include "Features/DrawcallLimitFix/Api/DrawFamilies.h"
#include "Features/DrawcallLimitFix/Api/EngineEvents.h"
#include "Features/DrawcallLimitFix/Api/Epochs.h"
#include "Features/DrawcallLimitFix/Api/FrameBindings.h"
#include "Features/DrawcallLimitFix/Api/FrameValues.h"
#include "Features/DrawcallLimitFix/Api/LightVolumes.h"
#include "Features/DrawcallLimitFix/Api/MainRanges.h"
#include "Features/DrawcallLimitFix/Api/Materials.h"
#include "Features/DrawcallLimitFix/Api/ObjectContributors.h"
#include "Features/DrawcallLimitFix/Api/Passes.h"
#include "Features/DrawcallLimitFix/Api/Payload.h"
#include "Features/DrawcallLimitFix/Api/PrimaryView.h"
#include "Features/DrawcallLimitFix/Api/Reports.h"
#include "Features/DrawcallLimitFix/Api/RootStates.h"
#include "Features/DrawcallLimitFix/Api/SceneResources.h"
#include "Features/DrawcallLimitFix/Api/ShadingFields.h"
#include "Features/DrawcallLimitFix/Api/Settings.h"
#include "Features/DrawcallLimitFix/Api/ShadowEpochs.h"
#include "Features/DrawcallLimitFix/Api/ShadowViews.h"
#include "Features/DrawcallLimitFix/Api/SunVolume.h"
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
		/**
		 * @brief A pass the frame's schedule names by its key (Schedule/Frame.cpp). Fails the install for no key, a key another pass has,
		 * neither or both of make and emit, or an anchor with a setup or latch word; at the end, a key the schedule names that nobody
		 * registered, a second pass with a latch word, or one scheduled anywhere but in main.depth before core.z.build-draws.
		 */
		virtual void RegisterPass(const PassDef& a_pass) = 0;
		/**
		 * @brief The schedule's trigger a_id (Schedule/Frame.cpp's TriggerDef), for the feature's hook to mark active around its DCLF work
		 * (Extensions::TriggerScope, Core/Extensions/Triggers.h). Fails the install for an id the schedule has not.
		 */
		virtual TriggerId FindTrigger(std::string_view a_id) = 0;
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
		/** @brief Fails the install for no name or sample, or past Extensions::kClassifyInputSlots inputs. */
		virtual ClassifyInputId RegisterClassifyInput(const ClassifyInputContribution& a_input) = 0;
		/**
		 * @brief The input registered (by any feature or integration) under a_name, bound once every install ran (as a toggle's needs by
		 * name are); until then, and for good when none has the name, it reads false. Fails the install for no name or past
		 * Extensions::kClassifyInputNeedSlots needs. Not for a technique gate or a held group, which take a registered input.
		 */
		virtual ClassifyInputId NeedClassifyInput(std::string_view a_name) = 0;
		virtual void RegisterReportLines(const ReportLines& a_lines) = 0;
		/** @brief Whether the primary cull is installed: false, and the observer is never called, without it (its engine hooks failed). */
		virtual bool RegisterStandInObserver(const StandInObserver& a_observer) = 0;
		/**
		 * @brief Fails the install for a name another channel has, no name or drain (a Keys channel), or past
		 * Extensions::kEngineChannelSlots channels.
		 */
		virtual EngineChannelId RegisterEngineChannel(const EngineChannel& a_channel) = 0;
		/** @brief The channel registered (by this feature or an earlier one) under a_name; none when no channel has it. */
		virtual EngineChannelId FindEngineChannel(std::string_view a_name) const = 0;
		/**
		 * @brief The channel registered (by any feature, a later one too) under a_name, for the feature's own hooks to push to
		 * (Extensions::PushChannelKey), bound once every install ran (as NeedClassifyInput's needs are); until then, and for good when
		 * none has the name or its channel takes no pushes (Api::EngineChannel::push), a push is dropped. Not a channel the feature's
		 * keys hooks are handed (those name the registered id). Fails the install for no name or past Extensions::kEngineChannelNeedSlots
		 * needs.
		 */
		virtual EngineChannelId NeedEngineChannel(std::string_view a_name) = 0;
		/** @brief Fails the install for a second show veto, or one without open or vetoes. */
		virtual void RegisterShowVeto(const ShowVeto& a_veto) = 0;
		/**
		 * @brief A hidden-event site of the feature's own, past the hidden-node source's sites (its stores' events carry it:
		 * Extensions::PushReservedHidden), in reservation order.
		 */
		virtual std::uint32_t ReserveHiddenSite() = 0;
		/** @brief A node class whose ordered children the mirror captures for the feature (fails: incomplete, or its RTTI taken). */
		virtual void RegisterNodeCapture(const NodeCaptureContribution& a_capture) = 0;
		/**
		 * @brief A move writer of the feature's own named a_name, before the writer's hooks go in (Extensions::ReserveMoveWriter, which
		 * early hooks call directly: they have no context). Fails for no name or past Extensions::kMoveWriterSlots.
		 */
		virtual MoveWriterId ReserveMoveWriter(std::string_view a_name) = 0;
		/**
		 * @brief The feature writes every material row's frame VS variable (the ports' MaterialPortRules::frameVSVariable, the texture
		 * transforms' TexcoordOffset) and the ports' declared frame variables (MaterialPortContribution::frameVariables: the Effect port's
		 * falloff and base colour) with the frame's value every frame. A capture whose texture-transform buffers or declared frame
		 * variables' bytes alone moved then rewrites no record, and a write that moved only them captures nothing; without it they are the
		 * records' (rewritten from the capture). Once or more; read as Extensions::FrameVSVariablePatched.
		 */
		virtual void RegisterFrameVSVariablePatch() = 0;
		/** @brief Fails the install past Extensions::kTableFamilySlots families. */
		virtual TableFamilyId RegisterTableFamily(const TableFamily& a_family) = 0;
		virtual CoordinatorComponentId RegisterCoordinatorComponent(const CoordinatorComponent& a_component) = 0;
		/** @brief Fails the install for a technique another feature has, past Extensions::kTechniqueSlots, or without a registered toggle. */
		virtual void RegisterTechnique(const TechniqueContribution& a_technique) = 0;
		/**
		 * @brief Fails the install for a technique no contribution has (registered before) or another gate has, or without a registered
		 * input or toggle.
		 */
		virtual void RegisterTechniqueGate(const TechniqueGate& a_gate) = 0;
		/** @brief Fails the install for a type another feature has, past Extensions::kGeometryTypeSlots, or without a registered toggle. */
		virtual void RegisterGeometryType(const GeometryTypeContribution& a_type) = 0;
		/** @brief Fails the install for a parent and leaves another feature has, or without a registered toggle. */
		virtual void RegisterParent(const ParentContribution& a_parent) = 0;
		/** @brief Fails the install for a class another feature has, or without a registered toggle. */
		virtual void RegisterObjectClass(const ObjectClassContribution& a_class) = 0;
		/**
		 * @brief Fails the install for a class or form type another contribution has, a class a geometry type's entry rule names (or None),
		 * no name, or ownMotion without everyFrame.
		 */
		virtual void RegisterEntryClass(const EntryClassContribution& a_class) = 0;
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
		/** @brief Fails the install for no root or no owned, or a root another contribution or a draw family's faces have. */
		virtual void RegisterViewRoot(const ViewRootContribution& a_root) = 0;
		/** @brief Fails the install for no nodes or an unknown bit. */
		virtual void RegisterCullHiddenSource(const CullHiddenSource& a_source) = 0;
		/**
		 * @brief Fails the install for a map another feature has, without a registered toggle or its available flag, with a phase
		 * or view bit Core or another view has (or none, checked once all are in), or an ImportedTexture map without importTarget.
		 */
		virtual void RegisterOcclusionMap(const OcclusionMapContribution& a_map) = 0;
		/**
		 * @brief Fails the install for a source another feature has, one without camera, a Spot or Point source without lightKept, or a
		 * Sun source without fullFrustumWindowEnd (another source with one).
		 */
		virtual void RegisterShadowSource(const ShadowSourceContribution& a_source) = 0;
		/** @brief Fails the install for a second contribution, or one without a name, build, publish, frameStart or listExclusion. */
		virtual void RegisterShadowExclusion(const ShadowExclusionContribution& a_exclusion) = 0;
		/**
		 * @brief Fails the install for a family another registration has, a class with no registered toggle, or without its scene
		 * resources, its frame and main callbacks (decide, programSource, main's three), its program (Lighting's, no or another draw or
		 * program family's shader type; no name, label, key name or source path) or its engine shapes' vtable, or with only some of its
		 * faces' callbacks, sizes, names and root, or a root another family's faces or a view root contribution have.
		 */
		virtual void RegisterDrawFamily(const DrawFamilyContribution& a_family) = 0;
		/**
		 * @brief Fails the install for a slot another registration has (a held group's too), an order another range has, no name, a depth
		 * prepass on an appended slot, or a class with no registered toggle.
		 */
		virtual void RegisterMainRange(const MainRangeContribution& a_range) = 0;
		/**
		 * @brief Registers the group's main range too (RegisterMainRange's refusals, its class Count). Fails the install for a slot not
		 * appended, no name or property flags, or without a registered input.
		 */
		virtual void RegisterHeldGroup(const HeldGroupContribution& a_group) = 0;
		/** @brief Fails the install for a second near twin, or one without a name, debug name, resource, target or near test. */
		virtual void RegisterNearTwin(const NearTwinContribution& a_twin) = 0;
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
		/**
		 * @brief Fails the install for no name, resources or indices, a non-empty resource after an empty one, a register below the frame
		 * textures (t16) or past t127, Core's, a frame binding's, a frame texture's or other graph buffers', or past
		 * Extensions::kFrameGraphBufferSlots registrations.
		 */
		virtual void RegisterFrameGraphBuffers(const FrameGraphBuffers& a_buffers) = 0;
		/** @brief Fails the install for a second contribution, or one without a name or sample. */
		virtual void RegisterLightVolumes(const LightVolumesContribution& a_volumes) = 0;
		/** @brief Fails the install for a second contribution, or one without a name, cascades or entryPlanes. */
		virtual void RegisterSunVolume(const SunVolumeContribution& a_volume) = 0;
		/**
		 * @brief Fails the install for a second contribution, or one without a name, an ancestor RTTI, generation or fill, or a registered
		 * whileOn input.
		 */
		virtual void RegisterAncestorValue(const AncestorValueContribution& a_value) = 0;
		/**
		 * @brief Fails the install for a field another registration has (or Count), no name, membership or capture, or members None or
		 * past the entry classes.
		 */
		virtual ShadingFieldId RegisterShadingField(const ShadingFieldContribution& a_field) = 0;
		/**
		 * @brief Fails the install for a second input (Core's main cull reads one walk), or one without a name or any of its callbacks.
		 */
		virtual void RegisterMainCullInput(const MainCullInputContribution& a_input) = 0;
		/** @brief Fails the install for a slot another binding has (Core's cull reads each at one word), Count, or no name or select. */
		virtual void RegisterCullBinding(const CullBinding& a_binding) = 0;
		/**
		 * @brief Fails the install for a second contribution (Core's cull tests one pyramid), or one without a resource, scratch resource or
		 * setup.
		 */
		virtual void RegisterDepthPyramid(const DepthPyramidContribution& a_pyramid) = 0;
		/** @brief Fails the install for a second contribution (FrameDriver's main pass points call one main view), or one without a handler. */
		virtual void RegisterPrimaryView(const PrimaryViewContribution& a_view) = 0;
		/**
		 * @brief Fails the install for a second contribution (FrameDriver's shadow and occlusion points call one shadow engine's epochs), or
		 * one without its handlers.
		 */
		virtual void RegisterShadowEpochs(const ShadowEpochsContribution& a_epochs) = 0;
		/**
		 * @brief Fails the install for a second contribution (Core's readers take one producer's states), or one without a name, a table
		 * family registered before, view or test.
		 */
		virtual void RegisterRootStates(const RootStatesContribution& a_states) = 0;
		/**
		 * @brief Fails the install for no name, sample or evaluate, a Replace contributor without takesPass (or with covers), an After one
		 * without covers (or with takesPass), only some of a material class's callbacks, registers not strictly ascending, below the frame
		 * textures (t16) or past t127, Core's, a frame binding's, a frame texture's or graph buffers', or whose range overlaps another
		 * contributor's, registers past Extensions::kFeatureTextureSlots in all, witness words past Extensions::kMaterialContributorWitnessWords
		 * in all, or one past Extensions::kMaterialContributorSlots.
		 */
		virtual void RegisterMaterialContributor(const MaterialContributor& a_contributor) = 0;
		/** @brief Fails the install for no name or sample, or past Extensions::kReferenceFactSlots facts. */
		virtual ReferenceFactId RegisterReferenceFact(const ReferenceFact& a_fact) = 0;
		/** @brief Fails the install for no name, or a second declaration (the geometry record has one extra value). */
		virtual void DeclareGeometryExtra(std::string_view a_extraName) = 0;
		/** @brief Fails the install for no name, bits or capture, bits another registration has, or past Extensions::kPropertyBitsSlots. */
		virtual void RegisterPropertyBits(const PropertyBits& a_bits) = 0;
		/**
		 * @brief Fails the install for an object or record flag not one bit, or another registration's, no applies, or past
		 * Extensions::kLightingObjectFlagSlots.
		 */
		virtual void RegisterLightingObjectFlag(const LightingObjectFlag& a_flag) = 0;
		/** @brief Fails the install for a second claimant, or one without keyBits or permutation. */
		virtual void RegisterLightingKeyBits(const LightingKeyBits& a_bits) = 0;
		/**
		 * @brief Fails the install for nothing to apply, a pixelKeep outside Extensions::kLightingPixelSplitBits, a raw rule with rawWhen
		 * when another registration has one (or rawSet or rawFlip without rawWhen), or derivePass past Extensions::kLightingPassDeriveSlots.
		 */
		virtual void RegisterLightingPassRules(const LightingPassRules& a_rules) = 0;
		/**
		 * @brief Fails the install for no name, a register past the stage's constant buffer slots (b13), Core's
		 * (Extensions::CorePerDrawMask) or another declaration's of the stage.
		 */
		virtual void DeclarePerDrawBuffer(const PerDrawBuffer& a_buffer) = 0;

	protected:
		~InstallContext() = default;
	};

	/**
	 * @brief A feature DCLF installs (Features/FeatureList.cpp).
	 *
	 * - install: see InstallContext.
	 * - earlyHooks (optional): the main thread at PostPostLoad, before DCLF's own engine side and observers install (the shell's
	 *   Extensions::InstallEarlyHooks, in the list's order): engine hooks that must be in before Core's engine side reads what they
	 *   change (a vtable slot the primary cull takes at its install; a call site the primary cull requires, the main walk's, noted with
	 *   Extensions::NoteMainWalkInstalled; the source of an engine event channel Core follows, after Core's
	 *   own scene events are in; a writer completing Core's movers, on a slot it reserves first: Extensions::ReserveMoveWriter). Registers
	 *   nothing; Core's switches and Diagnostics::Observe are readable.
	 * - sourceHooks (optional): the main thread at PostPostLoad, where the shadow sources' hooks install (the shell's
	 *   Extensions::InstallSourceHooks, before the primary cull's): the engine hooks of a shadow source, its culls' (calling
	 *   ShadowSourceHooks::CullsInstalled once they are in) and those that write its cameras into its boards (with Core's camera math,
	 *   Core/Render/Shadow/ShadowCameras.h; Core reads them through the source's camera and the sun volume). Called once per source, in ShadowSource order (Sun, Spot, Point, Focus; at each source the
	 *   features in the list's order; Features/LocalLights' light selection at the Spot's): a feature installs the hooks of
	 *   a_hooks.source (none for a source it does not take), so the hooks' install order (and the install log's) is the sources',
	 *   whatever the features' places in the list. Registers nothing.
	 * - preflight (optional): the main thread at PostPostLoad, before any of DCLF's hooks is written (the shell's
	 *   Extensions::PreflightFeatures): every engine site the feature's hooks (early, source and install's) will write, under the same
	 *   conditions its installs check, through Core/Engine/HookSites.h. A chained hook requires its site to be a CALL (RequireCall);
	 *   one that skips or replaces the engine's code requires the engine's own there (RequireCallTo, RequireSlot, RequireEntry,
	 *   RequireBytes). A conflict leaves DCLF off, with the site named, rather than half installed.
	 */
	struct Feature
	{
		std::string_view name;
		void (*install)(InstallContext& a_context) = nullptr;
		void (*earlyHooks)() = nullptr;
		void (*sourceHooks)(ShadowSourceHooks& a_hooks) = nullptr;
		void (*preflight)() = nullptr;
	};
}
