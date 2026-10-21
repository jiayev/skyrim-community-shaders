// The extension registry (see the header): the install context the features register with, and the lists it fills.

#include "Features/DrawcallLimitFix/Core/Extensions/Registry.h"
#include "Features/DrawcallLimitFix/Core/Extensions/AncestorValues.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ClassifyInputs.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Commits.h"
#include "Features/DrawcallLimitFix/Core/Extensions/CoordinatorComponents.h"
#include "Features/DrawcallLimitFix/Core/Extensions/CullInputs.h"
#include "Features/DrawcallLimitFix/Core/Extensions/DepthPyramid.h"
#include "Features/DrawcallLimitFix/Core/Extensions/DrawFamilies.h"
#include "Features/DrawcallLimitFix/Core/Extensions/EngineEvents.h"
#include "Features/DrawcallLimitFix/Core/Extensions/FrameBindings.h"
#include "Features/DrawcallLimitFix/Core/Extensions/LightVolumes.h"
#include "Features/DrawcallLimitFix/Core/Extensions/MainRanges.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Materials.h"
#include "Features/DrawcallLimitFix/Core/Extensions/MaterialWrites.h"
#include "Features/DrawcallLimitFix/Core/Extensions/MaterialPorts.h"
#include "Features/DrawcallLimitFix/Core/Extensions/NodeCaptures.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ObjectContributors.h"
#include "Features/DrawcallLimitFix/Core/Extensions/PaletteRows.h"
#include "Features/DrawcallLimitFix/Core/Extensions/PrimaryView.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ProgramFamilies.h"
#include "Features/DrawcallLimitFix/Core/Extensions/RootStates.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ShadingFields.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ShadowEpochs.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ShadowViews.h"
#include "Features/DrawcallLimitFix/Core/Extensions/SunVolume.h"
#include "Features/DrawcallLimitFix/Core/Extensions/TableFamilies.h"
#include "Features/DrawcallLimitFix/Core/Extensions/VertexStreams.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ViewCommon.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Views.h"
#include "Features/DrawcallLimitFix/Core/Render/Epochs/ScheduleExecutor.h"
#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"

#include <algorithm>
#include <array>
#include <bit>
#include <functional>
#include <vector>

namespace DCLF::Extensions
{
	namespace
	{
		// The show veto's cells while none is registered: never open, never vetoing (the stubs load the cells whatever is registered).
		const std::atomic<std::uint32_t> kShowVetoClosed{ 0 };
		bool NoShowVeto(const void*) { return false; }

		// Written only during InstallFeatures (the main thread, before DCLF's threads exist: their start orders these writes before
		// every read), read-only after.
		struct Lists
		{
			std::vector<FrameValuePoolEntry> pools;
			std::vector<PayloadProductEntry> products;
			std::vector<SceneResourcesEntry> sceneResources;
			std::uint32_t sceneResourceInputs = 0;
			std::vector<CommitHookEntry> commitHooks;
			std::vector<FrameBindingEntry> frameBindings;
			std::vector<ReportLinesEntry> reportLines;
			std::vector<Api::StandInObserver> standInObservers;
			std::vector<EngineChannelEntry> engineChannels;
			// The engine channels needed by name (by need slot), bound at the end (BindEngineChannelNeeds).
			std::vector<std::string> engineChannelNeeds;
			// The show veto's cells (RegisteredShowVeto: their address fixed for the session), its hide sites owned.
			Api::ShowVeto showVeto{ &kShowVetoClosed, &NoShowVeto, nullptr, {} };
			std::vector<std::uint32_t> showVetoHideSites;
			bool showVetoRegistered = false;
			std::uint32_t reservedHiddenSites = 0;
			std::vector<TableFamilyEntry> tableFamilies;
			std::vector<CoordinatorComponentEntry> coordinatorComponents;
			// By entry class: whether the component that follows it takes it (EntryClassTaken).
			std::array<bool, static_cast<std::size_t>(Api::EntryClass::Count)> entryClassesTaken{};
			TechniqueEntries techniques{};
			// The techniques' property flag rules, once each (PropertyFlagRules): at most one a technique.
			std::array<PropertyFlagRule, kTechniqueSlots> propertyFlagRules{};
			std::size_t propertyFlagRuleCount = 0;
			GeometryTypeToggles geometryTypes{};
			// By type: its contribution (geometryType 0xFF: none); by entry class: the rule a contribution names it with.
			GeometryTypeContributions geometryTypeContributions{};
			EntryClassRules entryClassRules{};
			// The owner classes by form type (OwnerClassOf), whether a class is one, and their entries' timing labels (OwnedEntriesLabel).
			OwnerClasses ownerClasses{};
			std::array<bool, static_cast<std::size_t>(Api::EntryClass::Count)> ownerClassRegistered{};
			std::string ownedLabel = "owned", ownedWithoutRecordLabel = "owned without a record";
			ParentToggles parents{};
			ObjectClassToggles objectClasses{};
			CategoryRoots categoryRoots{};
			std::vector<ShadingContributionEntry> shadingContributions;
			std::vector<FrameGlobalsEntry> frameGlobals;
			std::uint32_t frameGlobalsFloats = 0;
			// By view: its contribution (EngineView::Count: none); the views' work, in order. Resolved at the end (ResolveViews).
			std::array<Api::ViewContribution, kEngineViews> views{};
			std::vector<Api::ViewWork> viewWork;
			// By root: its view root contribution (ViewRoot::Count: none). Resolved with the views (ResolveViews).
			std::array<Api::ViewRootContribution, kViewRoots> viewRoots{};
			// The cull hidden sources, in registration order. Resolved with the views (ResolveViews).
			std::vector<Api::CullHiddenSource> cullHiddenSources;
			// By map and by source: its contribution (Count: none). Resolved at the end (ResolveShadowViews).
			std::array<Api::OcclusionMapContribution, kOcclusionMaps> occlusionMaps{};
			std::array<Api::ShadowSourceContribution, kShadowSources> shadowSources{};
			// The shadow exclusion (no name: none). Resolved with them.
			Api::ShadowExclusionContribution shadowExclusion{};
			// By family: its contribution (Count: none). Resolved at the end (ResolveDrawFamilies).
			std::array<Api::DrawFamilyContribution, kDrawFamilies> drawFamilies{};
			// By slot: its range (Count: none), the held groups' among them. Resolved at the end (ResolveMainRanges).
			std::array<Api::MainRangeContribution, kMainRangeSlots> mainRanges{};
			// The held groups, in registration order (HeldGroups); the near twin (target null: none).
			std::vector<HeldGroupEntry> heldGroups;
			NearTwinEntry nearTwin;
			// The classify inputs, in registration order (an index is the input's id); the names needed (by need slot), bound at the end
			// (BindClassifyInputNeeds).
			std::vector<ClassifyInputEntry> classifyInputs;
			std::vector<std::string> classifyInputNeeds;
			// The frame graph buffers, in registration order.
			std::vector<FrameGraphBuffersEntry> frameGraphBuffers;
			// The light volumes, the sun volume and the ancestor value (no name: none). Resolved at the end (ResolveLightVolumes,
			// ResolveSunVolume, ResolveAncestorValue).
			Api::LightVolumesContribution lightVolumes{};
			Api::SunVolumeContribution sunVolume{};
			Api::AncestorValueContribution ancestorValue{};
			// The vertex stream (entry class Count: none). Resolved at the end (ResolveVertexStream).
			Api::VertexStreamContribution vertexStream{};
			// The main cull input (no name: none). Resolved at the end (ResolveMainCullInput).
			Api::MainCullInputContribution mainCullInput{};
			// By slot: its cull binding (no select: none). Resolved at the end (ResolveCullBindings).
			std::array<Api::CullBinding, kCullBindingSlots> cullBindings{};
			// The depth pyramid (no setup: none). Resolved at the end (ResolveDepthPyramid).
			Api::DepthPyramidContribution depthPyramid{};
			// The primary view and its feature (no handlers: none). Resolved at the end (ResolvePrimaryView).
			Api::PrimaryViewContribution primaryView{};
			std::string primaryViewFeature;
			// The shadow epochs and their feature (no handlers: none). Resolved at the end (ResolveShadowEpochs).
			Api::ShadowEpochsContribution shadowEpochs{};
			std::string shadowEpochsFeature;
			// The root states (no name: none). Resolved at the end (ResolveRootStates).
			Api::RootStatesContribution rootStates{};
			// By palette rows class (its index - 1): its registration (itemClass Count: none). Resolved at the end (ResolvePaletteRows).
			std::array<Api::PaletteRowsContribution, Api::kPaletteRowsClasses> paletteRows{};
			// The shading fields, in registration order; whether a field is taken. Resolved at the end (ResolveShadingFields).
			std::vector<Api::ShadingFieldContribution> shadingFields;
			std::array<bool, kShadingRowFields> shadingFieldTaken{};
			// By kind: Lighting's (0), then the features' ports, in order (materialPorts first unset). Resolved at the end
			// (ResolveMaterialPorts).
			std::array<Api::MaterialPortContribution, kMaterialKinds> materialPorts{};
			std::size_t materialPortCount = 0;
			// By family: its contribution (Count: none). Resolved at the end (ResolveProgramFamilies).
			std::array<Api::ProgramFamilyContribution, kProgramFamilies> programFamilies{};
			// In registration order; the rows sorted by row at the end. By texture register: its frame texture's index.
			std::vector<FrameConstantRowEntry> frameConstantRows;
			std::vector<FrameTextureEntry> frameTextures;
			std::array<std::uint8_t, kTextureRegisters> frameTextureIndices = [] {
				std::array<std::uint8_t, kTextureRegisters> indices{};
				indices.fill(kNoFrameTexture);
				return indices;
			}();
			// The material contributors, in registration order, and their registers and witness words so far. Resolved at the end
			// (ResolveMaterialContributors).
			std::vector<MaterialContributorEntry> materialContributors;
			std::uint32_t materialTextureRegisters = 0, materialWitnessWords = 0;
			// The Lighting join rules, each registration in place (read as registered: RegisteredLightingJoinRules).
			LightingJoinRules lightingJoinRules;
			// By stage (Api::ShaderStage): the per-draw constant buffer registers, Core's and the declared ones (PerDrawMask).
			std::array<std::uint32_t, 2> perDrawMasks{ kCorePerDrawVS, kCorePerDrawPS };
			bool installed = false;
		};

		/** @brief The material contributor registered so far that binds a_register (null: none). */
		const MaterialContributorEntry* MaterialRegisterAt(const Lists& a_lists, std::uint32_t a_register)
		{
			for (const auto& contributor : a_lists.materialContributors)
				if (std::ranges::find(contributor.registers, a_register) != contributor.registers.end())
					return &contributor;
			return nullptr;
		}

		/**
		 * @brief Adds a_range by its slot, refused (a_who's) for a slot or order already registered, no name, a depth prepass on an
		 * appended slot, or a feature's class with no registered toggle (a held group's range has none: Count).
		 */
		void AddMainRange(Lists& a_lists, std::string_view a_who, const Api::MainRangeContribution& a_range, bool a_heldGroup)
		{
			const bool classValid = a_heldGroup ? a_range.objectClass == Api::ObjectClass::Count :
			                                      a_range.objectClass < Api::ObjectClass::Count && a_lists.objectClasses[static_cast<std::size_t>(a_range.objectClass)];
			if (a_range.slot >= Api::MainRangeSlot::Count || a_range.name.empty() || !classValid ||
				(a_range.depthPrepass && a_range.slot >= Api::MainRangeSlot::Appended0))
				stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} registered an incomplete main range", a_who));
			for (const auto& other : a_lists.mainRanges)
				if (other.slot != Api::MainRangeSlot::Count && other.order == a_range.order)
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {}'s main range {} takes order {}, which {} has", a_who, a_range.name, static_cast<std::uint32_t>(a_range.order), other.name));
			auto& entry = a_lists.mainRanges[static_cast<std::size_t>(a_range.slot)];
			if (entry.slot != Api::MainRangeSlot::Count)
				stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} contributes main range slot {}, which another registration has", a_who,
					static_cast<std::uint32_t>(a_range.slot)));
			entry = a_range;
		}

		/**
		 * @brief Adds a_group and its main range (AddMainRange's refusals), refused (a_who's) for a slot not appended, no name or property
		 * flags, or an input not registered.
		 */
		void AddHeldGroup(Lists& a_lists, std::string_view a_who, const Api::HeldGroupContribution& a_group)
		{
			if (a_group.slot < Api::MainRangeSlot::Appended0 || a_group.slot >= Api::MainRangeSlot::Count || a_group.name.empty() || !a_group.propertyFlags ||
				a_group.heldWhile.index >= a_lists.classifyInputs.size())
				stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} registered an incomplete held group", a_who));
			AddMainRange(a_lists, a_who, { .slot = a_group.slot, .name = a_group.name, .order = a_group.order }, true);
			const auto group = static_cast<std::uint32_t>(a_group.slot) - static_cast<std::uint32_t>(Api::MainRangeSlot::Appended0) + 1;
			a_lists.heldGroups.push_back({ a_group.heldWhile, group, a_group.propertyFlags, a_group.toggle });
		}

		/** @brief Sets a_twin, refused (a_who's) for a second one, or one without a name, debug name, resource, target or near test. */
		void SetNearTwin(Lists& a_lists, std::string_view a_who, const Api::NearTwinContribution& a_twin)
		{
			if (a_twin.name.empty() || a_twin.debugName.empty() || a_twin.resource.empty() || !a_twin.target || !a_twin.nearTest)
				stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} registered an incomplete near twin", a_who));
			// Appended0's twin buckets draw into one target.
			if (a_lists.nearTwin.target)
				stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} registered near twin {}, which {} has", a_who, a_twin.name, a_lists.nearTwin.name));
			const std::string name(a_twin.name);
			a_lists.nearTwin = { name, std::string(a_twin.debugName), std::string(a_twin.resource), name + " cannot be described", name + " could not be imported",
				a_twin.target, a_twin.nearTest };
		}

		/**
		 * @brief Adds a_port at the next kind, which it returns, refused (a_who's) for no name, property RTTI, material vtable or callbacks,
		 * Lighting's or no shader type, a space past kMaxStageVariables or with names not one a variable, a class with no registered toggle,
		 * frame variables past kMaxMaterialFrameVariables or outside its space or material, a shader type another port has, or a kind past
		 * kMaterialKinds.
		 */
		std::uint8_t AddMaterialPort(Lists& a_lists, std::string_view a_who, const Api::MaterialPortContribution& a_port)
		{
			const auto spaceValid = [](const Api::MaterialStageSpace& a_stage) {
				return a_stage.variables <= kMaxStageVariables && (a_stage.names.empty() || a_stage.names.size() == a_stage.variables);
			};
			const bool classValid = a_port.objectClass < Api::ObjectClass::Count && a_lists.objectClasses[static_cast<std::size_t>(a_port.objectClass)];
			if (a_port.shaderType == RE::BSShader::Type::Lighting || a_port.shaderType >= RE::BSShader::Type::Total || !classValid || a_port.name.empty() || !a_port.propertyRtti ||
				!a_port.materialVtable || !a_port.capture || !a_port.derive || !a_port.evaluate || !a_port.shading || !spaceValid(a_port.vs) || !spaceValid(a_port.ps))
				stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} registered an incomplete material port", a_who));
			// Its frame variables: a material row's places, each a variable of its stage read from bytes its material has.
			const auto readable = [&](const std::array<Api::MaterialFrameComponent, 4>& a_components) {
				return std::all_of(a_components.begin(), a_components.end(), [&](const Api::MaterialFrameComponent& a_component) {
					return a_component.source + 4u <= a_port.materialBytes &&
					       (a_component.scale == Api::MaterialFrameComponent::kNoScale || a_component.scale + 4u <= a_port.materialBytes);
				});
			};
			if (a_port.frameVariables.size() > Api::kMaxMaterialFrameVariables)
				stl::report_and_fail(fmt::format("Drawcall Limit Fix: {}'s material port declares more than kMaxMaterialFrameVariables frame variables: grow it first", a_who));
			for (const auto& variable : a_port.frameVariables)
				if (variable.variable >= (variable.pixel ? a_port.ps.variables : a_port.vs.variables) || variable.first >= variable.end ||
					variable.end > a_port.materialBytes || !readable(variable.components) || !readable(variable.alternate))
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {}'s material port declares a frame variable outside its space or material", a_who));
			for (std::size_t kind = 1; kind < a_lists.materialPortCount; ++kind)
				if (a_lists.materialPorts[kind].shaderType == a_port.shaderType)
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {}'s material port takes shader type {}, which another port has", a_who,
						static_cast<std::uint32_t>(a_port.shaderType)));
			if (a_lists.materialPortCount >= kMaterialKinds)
				stl::report_and_fail(fmt::format("Drawcall Limit Fix: {}'s material port is one past kMaterialKinds: grow it first", a_who));
			a_lists.materialPorts[a_lists.materialPortCount] = a_port;
			return static_cast<std::uint8_t>(a_lists.materialPortCount++);
		}

		/** @brief Whether a draw family's program or a program family registered so far is built from a_shaderType. */
		bool ProgramShaderTaken(const Lists& a_lists, std::uint8_t a_shaderType)
		{
			for (const auto& family : a_lists.drawFamilies)
				if (family.family != Api::DrawFamily::Count && family.program.shaderType == a_shaderType)
					return true;
			for (const auto& family : a_lists.programFamilies)
				if (family.family != Api::ProgramFamily::Count && family.shaderType == a_shaderType)
					return true;
			return false;
		}

		/** @brief Whether a_root has an owner registered so far: a view root contribution or a draw family's faces. */
		bool ViewRootTaken(const Lists& a_lists, Api::ViewRoot a_root)
		{
			if (a_lists.viewRoots[static_cast<std::size_t>(a_root)].root != Api::ViewRoot::Count)
				return true;
			for (const auto& family : a_lists.drawFamilies)
				if (family.family != Api::DrawFamily::Count && family.faces.root == a_root)
					return true;
			return false;
		}

		/**
		 * @brief Whether a_faces are all or none: the callbacks, sizes, names and root all set (a trees view's faces draw the family) or none
		 * (they do not).
		 */
		bool DrawFamilyFacesComplete(const Api::DrawFamilyContribution::Faces& a_faces)
		{
			const int callbacks = (a_faces.declare ? 1 : 0) + (a_faces.record ? 1 : 0) + (a_faces.row ? 1 : 0) + (a_faces.lists ? 1 : 0) + (a_faces.face ? 1 : 0);
			const int data = (a_faces.rowBytes ? 1 : 0) + (a_faces.headerBytes ? 1 : 0) + (a_faces.unitWords ? 1 : 0) + (a_faces.cullGroup ? 1 : 0) +
			                 (a_faces.rowName.empty() ? 0 : 1) + (a_faces.listName.empty() ? 0 : 1) + (a_faces.root < Api::ViewRoot::Count ? 1 : 0);
			return callbacks == 0 ? data == 0 : callbacks == 5 && data == 7;
		}

		/**
		 * @brief Adds a_family by its family, refused (a_who's) for a family already registered, Lighting's or no shader type, no name, key
		 * name or source path, or no program source; or a shader type a draw family's program or another program family has.
		 */
		void AddProgramFamily(Lists& a_lists, std::string_view a_who, const Api::ProgramFamilyContribution& a_family)
		{
			if (a_family.family >= Api::ProgramFamily::Count || a_family.shaderType == RE::BSShader::Type::Lighting || a_family.shaderType >= RE::BSShader::Type::Total ||
				a_family.name.empty() || a_family.keyName.empty() || a_family.sourcePath.empty() || !a_family.programSource)
				stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} registered an incomplete program family", a_who));
			auto& entry = a_lists.programFamilies[static_cast<std::size_t>(a_family.family)];
			if (entry.family != Api::ProgramFamily::Count)
				stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} contributes program family {}, which {} has", a_who, a_family.name, entry.name));
			if (ProgramShaderTaken(a_lists, a_family.shaderType))
				stl::report_and_fail(fmt::format("Drawcall Limit Fix: {}'s program family {} takes shader type {}, which another family has", a_who, a_family.name,
					static_cast<std::uint32_t>(a_family.shaderType)));
			entry = a_family;
		}

		/** @brief Adds a_row, refused (a_who's) for a row outside Core's feature rows or another registration's, no name or no sample. */
		void AddFrameConstantRow(Lists& a_lists, std::string_view a_who, const Api::FrameConstantRow& a_row)
		{
			if (a_row.name.empty() || !a_row.sample || a_row.row < kFrameConstantRowFirst || a_row.row >= kFrameConstantRowsEnd)
				stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} registered an incomplete frame constant row", a_who));
			for (const auto& row : a_lists.frameConstantRows)
				if (row.row == a_row.row)
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {}'s frame constant row {} takes c{}, which {} has", a_who, a_row.name, a_row.row, row.name));
			a_lists.frameConstantRows.push_back({ std::string(a_row.name), a_row.row, a_row.sample });
		}

		/** @brief The frame graph buffers registered so far that read a_register (null: none). */
		const FrameGraphBuffersEntry* GraphBuffersAt(const Lists& a_lists, std::uint32_t a_register)
		{
			for (const auto& buffers : a_lists.frameGraphBuffers)
				if (a_register >= buffers.firstRegister && a_register - buffers.firstRegister < buffers.count)
					return &buffers;
			return nullptr;
		}

		/**
		 * @brief Adds a_buffers, refused (a_who's) for no name, resources or indices, a non-empty resource after an empty one, a register
		 * below the frame textures or past t127, Core's, a frame binding's, a frame texture's or other graph buffers', or one past
		 * kFrameGraphBufferSlots.
		 */
		void AddFrameGraphBuffers(Lists& a_lists, std::string_view a_who, const Api::FrameGraphBuffers& a_buffers)
		{
			std::uint32_t count = 0;
			while (count < a_buffers.resources.size() && !a_buffers.resources[count].empty())
				++count;
			bool gap = false;
			for (std::size_t r = count; r < a_buffers.resources.size(); ++r)
				gap = gap || !a_buffers.resources[r].empty();
			const auto first = a_buffers.firstRegister;
			if (a_buffers.name.empty() || !a_buffers.indices || !count || gap || first < kPixelTextureSlots || first >= kTextureRegisters ||
				count > kTextureRegisters - first)
				stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} registered incomplete frame graph buffers", a_who));
			for (std::uint32_t r = first; r < first + count; ++r) {
				if (CoreFrameRegister(r))
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {}'s frame graph buffers {} take t{}, which Core keeps", a_who, a_buffers.name, r));
				for (const auto& binding : a_lists.frameBindings)
					if (binding.textureRegister == r)
						stl::report_and_fail(fmt::format("Drawcall Limit Fix: {}'s frame graph buffers {} take t{}, which {} has", a_who, a_buffers.name, r, binding.name));
				if (const auto texture = a_lists.frameTextureIndices[r]; texture != kNoFrameTexture)
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {}'s frame graph buffers {} take t{}, which {} has", a_who, a_buffers.name, r,
						a_lists.frameTextures[texture].name));
				if (const auto* other = GraphBuffersAt(a_lists, r))
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {}'s frame graph buffers {} take t{}, which {} has", a_who, a_buffers.name, r, other->name));
				if (const auto* contributor = MaterialRegisterAt(a_lists, r))
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {}'s frame graph buffers {} take t{}, which {} has", a_who, a_buffers.name, r, contributor->name));
			}
			// A commit keeps each registration's indices in a fixed array (FrameGraphBufferValues).
			if (a_lists.frameGraphBuffers.size() >= kFrameGraphBufferSlots)
				stl::report_and_fail(fmt::format("Drawcall Limit Fix: {}'s frame graph buffers {} are one past kFrameGraphBufferSlots: grow it first", a_who, a_buffers.name));
			auto& entry = a_lists.frameGraphBuffers.emplace_back();
			entry.name = std::string(a_buffers.name);
			entry.firstRegister = first;
			entry.count = count;
			for (std::uint32_t r = 0; r < count; ++r)
				entry.resources[r] = std::string(a_buffers.resources[r]);
			entry.indices = a_buffers.indices;
		}

		/**
		 * @brief Adds a_texture, refused (a_who's) for a register below the frame textures or past t127, Core's, a frame binding's or another
		 * frame texture's, no name or no view, or one past kFrameTextureSlots.
		 */
		void AddFrameTexture(Lists& a_lists, std::string_view a_who, const Api::FrameTexture& a_texture)
		{
			const auto r = a_texture.textureRegister;
			if (a_texture.name.empty() || !a_texture.view || r < kPixelTextureSlots || r >= kTextureRegisters)
				stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} registered an incomplete frame texture", a_who));
			if (CoreFrameRegister(r))
				stl::report_and_fail(fmt::format("Drawcall Limit Fix: {}'s frame texture {} takes t{}, which Core keeps", a_who, a_texture.name, r));
			for (const auto& binding : a_lists.frameBindings)
				if (binding.textureRegister == r)
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {}'s frame texture {} takes t{}, which {} has", a_who, a_texture.name, r, binding.name));
			if (const auto other = a_lists.frameTextureIndices[r]; other != kNoFrameTexture)
				stl::report_and_fail(fmt::format("Drawcall Limit Fix: {}'s frame texture {} takes t{}, which {} has", a_who, a_texture.name, r, a_lists.frameTextures[other].name));
			if (const auto* buffers = GraphBuffersAt(a_lists, r))
				stl::report_and_fail(fmt::format("Drawcall Limit Fix: {}'s frame texture {} takes t{}, which {} has", a_who, a_texture.name, r, buffers->name));
			if (const auto* contributor = MaterialRegisterAt(a_lists, r))
				stl::report_and_fail(fmt::format("Drawcall Limit Fix: {}'s frame texture {} takes t{}, which {} has", a_who, a_texture.name, r, contributor->name));
			// A colour commit samples every view into a fixed array (ResolveFrameTextures).
			if (a_lists.frameTextures.size() >= kFrameTextureSlots)
				stl::report_and_fail(fmt::format("Drawcall Limit Fix: {}'s frame texture {} is one past kFrameTextureSlots: grow it first", a_who, a_texture.name));
			a_lists.frameTextureIndices[r] = static_cast<std::uint8_t>(a_lists.frameTextures.size());
			a_lists.frameTextures.push_back({ std::string(a_texture.name), r, a_texture.view, a_texture.capturedWhenNull });
		}

		/**
		 * @brief Whether a_contributor is complete: a name, sample and evaluate; its stage's callback alone (Replace: takesPass, After:
		 * covers); a material class's callbacks all or none; its registers strictly ascending within the frame textures' (t16-t127).
		 */
		bool MaterialContributorComplete(const Api::MaterialContributor& a_contributor)
		{
			const bool stageValid = a_contributor.stage == Api::MaterialStage::Replace ? a_contributor.takesPass && !a_contributor.covers :
			                        a_contributor.stage == Api::MaterialStage::After  ? a_contributor.covers && !a_contributor.takesPass :
			                                                                            false;
			const auto& materialClass = a_contributor.materialClass;
			const int classCallbacks = (materialClass.owns ? 1 : 0) + (materialClass.bytes ? 1 : 0) + (materialClass.textureFields ? 1 : 0);
			const auto registers = a_contributor.textureRegisters;
			bool registersValid = true;
			for (std::size_t i = 0; i < registers.size(); ++i)
				registersValid = registersValid && registers[i] >= kPixelTextureSlots && registers[i] < kTextureRegisters && (i == 0 || registers[i] > registers[i - 1]);
			return !a_contributor.name.empty() && a_contributor.sample && a_contributor.evaluate && stageValid && (classCallbacks == 0 || classCallbacks == 3) &&
			       registersValid;
		}

		/**
		 * @brief Adds a_contributor, refused (a_who's) when incomplete (MaterialContributorComplete), for a register Core's, a frame
		 * binding's, a frame texture's or graph buffers', a register range overlapping another contributor's (each contributor's registers
		 * take consecutive slots), or one past kFeatureTextureSlots registers, kMaterialContributorWitnessWords words or
		 * kMaterialContributorSlots contributors.
		 */
		void AddMaterialContributor(Lists& a_lists, std::string_view a_who, const Api::MaterialContributor& a_contributor)
		{
			if (!MaterialContributorComplete(a_contributor))
				stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} registered an incomplete material contributor", a_who));
			const auto registers = a_contributor.textureRegisters;
			for (const std::uint32_t r : registers) {
				if (CoreFrameRegister(r))
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {}'s material contributor {} takes t{}, which Core keeps", a_who, a_contributor.name, r));
				for (const auto& binding : a_lists.frameBindings)
					if (binding.textureRegister == r)
						stl::report_and_fail(fmt::format("Drawcall Limit Fix: {}'s material contributor {} takes t{}, which {} has", a_who, a_contributor.name, r, binding.name));
				if (const auto texture = a_lists.frameTextureIndices[r]; texture != kNoFrameTexture)
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {}'s material contributor {} takes t{}, which {} has", a_who, a_contributor.name, r,
						a_lists.frameTextures[texture].name));
				if (const auto* buffers = GraphBuffersAt(a_lists, r))
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {}'s material contributor {} takes t{}, which {} has", a_who, a_contributor.name, r, buffers->name));
			}
			if (!registers.empty())
				for (const auto& other : a_lists.materialContributors)
					if (!other.registers.empty() && registers.front() <= other.registers.back() && other.registers.front() <= registers.back())
						stl::report_and_fail(fmt::format("Drawcall Limit Fix: {}'s material contributor {} takes t{}-t{}, which overlap {}'s", a_who, a_contributor.name,
							registers.front(), registers.back(), other.name));
			// MaterialRecord::featureTextures' slots are the row header's (frozen with the HLSL decode).
			if (a_lists.materialTextureRegisters + registers.size() > kFeatureTextureSlots)
				stl::report_and_fail(fmt::format("Drawcall Limit Fix: {}'s material contributor {} takes registers past kFeatureTextureSlots", a_who, a_contributor.name));
			// The frame witness holds every contributor's words (MaterialContributorSamples::words).
			if (a_lists.materialWitnessWords + a_contributor.witnessWords > kMaterialContributorWitnessWords)
				stl::report_and_fail(fmt::format("Drawcall Limit Fix: {}'s material contributor {} writes witness words past kMaterialContributorWitnessWords: grow it first",
					a_who, a_contributor.name));
			// A frame's samples hold one a contributor (MaterialContributorSamples::contributors).
			if (a_lists.materialContributors.size() >= kMaterialContributorSlots)
				stl::report_and_fail(fmt::format("Drawcall Limit Fix: {}'s material contributor {} is one past kMaterialContributorSlots: grow it first", a_who,
					a_contributor.name));
			a_lists.materialTextureRegisters += static_cast<std::uint32_t>(registers.size());
			a_lists.materialWitnessWords += a_contributor.witnessWords;
			auto& entry = a_lists.materialContributors.emplace_back();
			entry.name = std::string(a_contributor.name);
			entry.stage = a_contributor.stage;
			entry.order = a_contributor.order;
			entry.registers = registers;
			entry.witnessWords = a_contributor.witnessWords;
			entry.signatureBits = a_contributor.signatureBits;
			entry.materialClass = a_contributor.materialClass;
			entry.sample = a_contributor.sample;
			entry.takesPass = a_contributor.takesPass;
			entry.covers = a_contributor.covers;
			entry.evaluate = a_contributor.evaluate;
		}

		/** @brief Adds a_fact as the next facts bit, which it returns, refused (a_who's) for no name or sample, or past kReferenceFactSlots. */
		Api::ReferenceFactId AddReferenceFact(Lists& a_lists, std::string_view a_who, const Api::ReferenceFact& a_fact)
		{
			auto& rules = a_lists.lightingJoinRules;
			if (a_fact.name.empty() || !a_fact.sample)
				stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} registered an incomplete reference fact", a_who));
			// The node record holds each fact in a bit (NodeRecord's facts).
			if (rules.factCount >= kReferenceFactSlots)
				stl::report_and_fail(fmt::format("Drawcall Limit Fix: {}'s reference fact {} is one past kReferenceFactSlots: grow it first", a_who, a_fact.name));
			const std::uint32_t index = rules.factCount++;
			rules.facts[index] = { std::string(a_fact.name), a_fact.none, a_fact.sample };
			rules.factsNone |= a_fact.none ? 1u << index : 0u;
			return { index };
		}

		/** @brief Adds a_bits, refused (a_who's) for no name, bits or capture, bits another registration has, or past kPropertyBitsSlots. */
		void AddPropertyBits(Lists& a_lists, std::string_view a_who, const Api::PropertyBits& a_bits)
		{
			auto& rules = a_lists.lightingJoinRules;
			if (a_bits.name.empty() || !a_bits.bits || !a_bits.capture)
				stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} registered incomplete property bits", a_who));
			if (a_bits.bits & rules.propertyBitsTaken)
				stl::report_and_fail(fmt::format("Drawcall Limit Fix: {}'s property bits {} ({:#x}) take bits another registration has", a_who, a_bits.name, a_bits.bits));
			if (rules.propertyBitsCount >= kPropertyBitsSlots)
				stl::report_and_fail(fmt::format("Drawcall Limit Fix: {}'s property bits {} are one past kPropertyBitsSlots: grow it first", a_who, a_bits.name));
			rules.propertyBits[rules.propertyBitsCount++] = { std::string(a_bits.name), a_bits.bits, a_bits.capture };
			rules.propertyBitsTaken = static_cast<std::uint8_t>(rules.propertyBitsTaken | a_bits.bits);
		}

		/**
		 * @brief Adds a_flag, refused (a_who's) for an object or record flag not one bit or another registration's, no applies, or past
		 * kLightingObjectFlagSlots.
		 */
		void AddLightingObjectFlag(Lists& a_lists, std::string_view a_who, const Api::LightingObjectFlag& a_flag)
		{
			auto& rules = a_lists.lightingJoinRules;
			if (!std::has_single_bit(a_flag.objectFlag) || !std::has_single_bit(a_flag.recordFlag) || !a_flag.applies)
				stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} registered an incomplete Lighting object flag", a_who));
			for (std::uint32_t i = 0; i < rules.objectFlagCount; ++i)
				if (rules.objectFlags[i].objectFlag == a_flag.objectFlag || rules.objectFlags[i].recordFlag == a_flag.recordFlag)
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {}'s Lighting object flag {:#x} (record flag {:#x}) is another registration's", a_who,
						a_flag.objectFlag, a_flag.recordFlag));
			// The record flags' pairs are read per record (LightingRecordFlagsOf).
			if (rules.objectFlagCount >= kLightingObjectFlagSlots)
				stl::report_and_fail(fmt::format("Drawcall Limit Fix: {}'s Lighting object flag {:#x} is one past kLightingObjectFlagSlots: grow it first", a_who,
					a_flag.objectFlag));
			rules.objectFlags[rules.objectFlagCount++] = a_flag;
		}

		/**
		 * @brief Adds a_rules' masks and derivePass, refused (a_who's) for nothing to apply, a pixelKeep outside kLightingPixelSplitBits, a
		 * raw rule without rawWhen or when another registration has one, or derivePass past kLightingPassDeriveSlots.
		 */
		void AddLightingPassRules(Lists& a_lists, std::string_view a_who, const Api::LightingPassRules& a_rules)
		{
			auto& rules = a_lists.lightingJoinRules;
			auto& masks = rules.passMasks;
			const bool empty = !a_rules.pixelKeep && !a_rules.rawWhen && !a_rules.rawSet && !a_rules.rawFlip && !a_rules.derivePass;
			if (empty || (a_rules.pixelKeep & ~kLightingPixelSplitBits) || ((a_rules.rawSet || a_rules.rawFlip) && !a_rules.rawWhen))
				stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} registered incomplete Lighting pass rules", a_who));
			// SetupGeometry's raw technique takes one rule (LightingRawTechnique).
			if (a_rules.rawWhen && masks.rawWhen)
				stl::report_and_fail(fmt::format("Drawcall Limit Fix: {}'s Lighting pass rules give a raw technique rule, which another registration has", a_who));
			if (a_rules.derivePass && rules.deriveCount >= kLightingPassDeriveSlots)
				stl::report_and_fail(fmt::format("Drawcall Limit Fix: {}'s Lighting pass rules' derivePass is one past kLightingPassDeriveSlots: grow it first", a_who));
			masks.pixelKeep |= a_rules.pixelKeep;
			if (a_rules.rawWhen)
				masks = { masks.pixelKeep, a_rules.rawWhen, a_rules.rawSet, a_rules.rawFlip };
			if (a_rules.derivePass)
				rules.derives[rules.deriveCount++] = a_rules.derivePass;
		}

		/**
		 * @brief Adds a_buffer's register to its stage's per-draw mask, refused (a_who's) for no name, a stage or register out of range,
		 * Core's or another declaration's.
		 */
		void AddPerDrawBuffer(Lists& a_lists, std::string_view a_who, const Api::PerDrawBuffer& a_buffer)
		{
			const auto stage = static_cast<std::size_t>(a_buffer.stage);
			if (a_buffer.name.empty() || stage >= a_lists.perDrawMasks.size() || a_buffer.constantRegister >= kConstantBufferRegisters)
				stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} declared an incomplete per-draw buffer", a_who));
			const std::uint32_t bit = 1u << a_buffer.constantRegister;
			if (CorePerDrawMask(a_buffer.stage) & bit)
				stl::report_and_fail(fmt::format("Drawcall Limit Fix: {}'s per-draw buffer {} takes b{}, which Core binds per draw", a_who, a_buffer.name, a_buffer.constantRegister));
			if (a_lists.perDrawMasks[stage] & bit)
				stl::report_and_fail(fmt::format("Drawcall Limit Fix: {}'s per-draw buffer {} takes b{}, which another declaration has", a_who, a_buffer.name,
					a_buffer.constantRegister));
			a_lists.perDrawMasks[stage] |= bit;
		}

		Lists& Registered()
		{
			static Lists lists;
			return lists;
		}

		class Context final : public Api::InstallContext
		{
		public:
			explicit Context(Lists& a_lists) :
				lists(a_lists) {}

			Api::FrameValuePoolId RegisterFrameValuePool(const Api::FrameValuePool& a_pool) override
			{
				if (a_pool.name.empty() || !a_pool.stride || !a_pool.kick || !a_pool.sample || !a_pool.send)
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} registered an incomplete frame-value pool", feature));
				lists.pools.push_back({ std::string(a_pool.name), a_pool.stride, a_pool.frameStart, a_pool.kick, a_pool.sample, a_pool.send });
				return { static_cast<std::uint32_t>(lists.pools.size() - 1) };
			}

			void RegisterPass(const Api::PassDef& a_pass) override { Draws::RegisterPassDef(a_pass, feature); }
			Api::TriggerId FindTrigger(std::string_view a_id) override { return Draws::FindTrigger(*schedule, a_id, feature); }

			void RegisterPayloadProduct(const Api::PayloadProduct& a_product) override
			{
				if (a_product.name.empty() || !a_product.build || !a_product.install)
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} registered an incomplete payload product", feature));
				lists.products.push_back({ std::string(a_product.name), a_product.build, a_product.carried, a_product.install });
			}

			Api::SceneResourcesId RegisterSceneResources(const Api::SceneResources& a_resources) override
			{
				if (a_resources.name.empty() || !a_resources.setup || !a_resources.postInputs != !a_resources.inputWords)
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} registered incomplete scene resources", feature));
				// RevisionInputs holds every registration's posted words (SceneResourceInputs).
				if (lists.sceneResourceInputs + a_resources.inputWords > kSceneResourceInputWords)
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {}'s scene resources {} post words past kSceneResourceInputWords: grow it first", feature, a_resources.name));
				lists.sceneResources.push_back({ std::string(a_resources.name), a_resources.setup, a_resources.registerGraph, a_resources.require, a_resources.fit,
					a_resources.postInputs, lists.sceneResourceInputs, a_resources.inputWords, a_resources.latched, a_resources.sealed });
				lists.sceneResourceInputs += a_resources.inputWords;
				return { static_cast<std::uint32_t>(lists.sceneResources.size() - 1) };
			}

			void RegisterCommitHook(const Api::CommitHook& a_hook) override
			{
				if (a_hook.name.empty() || !a_hook.commit)
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} registered an incomplete commit hook", feature));
				lists.commitHooks.push_back({ std::string(a_hook.name), a_hook.commit });
			}

			void RegisterFrameBinding(const Api::FrameBinding& a_binding) override
			{
				if (a_binding.name.empty() || !a_binding.select || a_binding.textureRegister >= kTextureRegisters)
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} registered an incomplete frame binding", feature));
				if (CoreFrameRegister(a_binding.textureRegister))
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {}'s frame binding {} takes t{}, which Core keeps", feature, a_binding.name, a_binding.textureRegister));
				for (const auto& binding : lists.frameBindings)
					if (binding.textureRegister == a_binding.textureRegister)
						stl::report_and_fail(fmt::format("Drawcall Limit Fix: {}'s frame binding {} takes t{}, which {} has", feature, a_binding.name, a_binding.textureRegister,
							binding.name));
				if (const auto texture = lists.frameTextureIndices[a_binding.textureRegister]; texture != kNoFrameTexture)
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {}'s frame binding {} takes t{}, which {} has", feature, a_binding.name, a_binding.textureRegister,
						lists.frameTextures[texture].name));
				if (const auto* buffers = GraphBuffersAt(lists, a_binding.textureRegister))
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {}'s frame binding {} takes t{}, which {} has", feature, a_binding.name, a_binding.textureRegister,
						buffers->name));
				if (const auto* contributor = MaterialRegisterAt(lists, a_binding.textureRegister))
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {}'s frame binding {} takes t{}, which {} has", feature, a_binding.name, a_binding.textureRegister,
						contributor->name));
				// A build embeds every binding's value (ResourceAddresses::frameBindings).
				if (lists.frameBindings.size() >= kFrameBindingSlots)
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {}'s frame binding {} is one past kFrameBindingSlots: grow it first", feature, a_binding.name));
				lists.frameBindings.push_back({ std::string(a_binding.name), a_binding.textureRegister, a_binding.select });
			}

			Api::SwitchId RegisterSwitch(const Api::FeatureSwitch& a_switch) override
			{
				return DCLF::RegisterSwitch(a_switch, feature);
			}

			Api::ToggleId RegisterToggle(const Api::FeatureToggle& a_toggle) override
			{
				return DCLF::RegisterToggle(a_toggle, feature);
			}

			Api::ClassifyInputId RegisterClassifyInput(const Api::ClassifyInputContribution& a_input) override
			{
				if (a_input.name.empty() || !a_input.sample)
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} registered an incomplete classify input", feature));
				// A set holds each input in a bit (ClassifyInputSet).
				if (lists.classifyInputs.size() >= kClassifyInputSlots)
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {}'s classify input {} is one past kClassifyInputSlots: grow it first", feature, a_input.name));
				// The needs bind by name (NeedClassifyInput).
				for (const auto& input : lists.classifyInputs)
					if (input.name == a_input.name)
						stl::report_and_fail(fmt::format("Drawcall Limit Fix: {}'s classify input {} has a name another input has", feature, a_input.name));
				lists.classifyInputs.push_back({ std::string(a_input.name), a_input.sample });
				return { static_cast<std::uint32_t>(lists.classifyInputs.size() - 1) };
			}

			Api::ClassifyInputId NeedClassifyInput(std::string_view a_name) override
			{
				if (a_name.empty())
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} needs a classify input without a name", feature));
				// A name needed twice is one need.
				std::size_t slot = 0;
				while (slot < lists.classifyInputNeeds.size() && lists.classifyInputNeeds[slot] != a_name)
					++slot;
				if (slot == lists.classifyInputNeeds.size()) {
					// The bound table holds each need in a slot (ClassifyInputs.h).
					if (slot >= kClassifyInputNeedSlots)
						stl::report_and_fail(fmt::format("Drawcall Limit Fix: {}'s need of classify input {} is one past kClassifyInputNeedSlots: grow it first", feature, a_name));
					lists.classifyInputNeeds.emplace_back(a_name);
				}
				return { kClassifyInputNeedBit | static_cast<std::uint32_t>(slot) };
			}

			void RegisterFrameGraphBuffers(const Api::FrameGraphBuffers& a_buffers) override { AddFrameGraphBuffers(lists, feature, a_buffers); }

			void RegisterLightVolumes(const Api::LightVolumesContribution& a_volumes) override
			{
				if (a_volumes.name.empty() || !a_volumes.sample)
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} registered an incomplete light volumes contribution", feature));
				// BuildDraws tests one list of volumes.
				if (!lists.lightVolumes.name.empty())
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} contributes light volumes {}, which {} has", feature, a_volumes.name, lists.lightVolumes.name));
				lists.lightVolumes = a_volumes;
			}

			void RegisterSunVolume(const Api::SunVolumeContribution& a_volume) override
			{
				if (a_volume.name.empty() || !a_volume.cascades || !a_volume.entryPlanes)
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} registered an incomplete sun volume contribution", feature));
				// One sun: its cascades and its full-frustum planes are one feature's.
				if (!lists.sunVolume.name.empty())
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} contributes sun volume {}, which {} has", feature, a_volume.name, lists.sunVolume.name));
				lists.sunVolume = a_volume;
			}

			void RegisterAncestorValue(const Api::AncestorValueContribution& a_value) override
			{
				// Its input registered before (an integration registers its own first).
				if (a_value.name.empty() || (!a_value.ancestorRtti[0] && !a_value.ancestorRtti[1]) || !a_value.generation || !a_value.fill ||
					a_value.whileOn.index >= lists.classifyInputs.size())
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} registered an incomplete ancestor value", feature));
				// The record has one ancestor value column.
				if (!lists.ancestorValue.name.empty())
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} contributes ancestor value {}, which {} has", feature, a_value.name, lists.ancestorValue.name));
				lists.ancestorValue = a_value;
			}

			void RegisterReportLines(const Api::ReportLines& a_lines) override
			{
				// A point composing its text takes text lines; the others, logged lines.
				const auto point = a_lines.point;
				const bool text = Api::ReportPointComposesText(point);
				// A coordinator component's lines in text's (or lines') place, at the coordinator's points, after the component.
				const bool component = Api::ReportPointTakesComponents(point) && a_lines.componentText;
				if (component && (!a_lines.component || a_lines.component.index >= lists.coordinatorComponents.size()))
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {}'s report lines {} name no registered component", feature, a_lines.name));
				if (a_lines.name.empty() || (component ? a_lines.text || a_lines.lines :
				                                        text ? !a_lines.text || a_lines.lines || a_lines.componentText : !a_lines.lines || a_lines.text || a_lines.componentText))
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} registered incomplete report lines", feature));
				lists.reportLines.push_back({ std::string(a_lines.name), a_lines.point, a_lines.lines, a_lines.text, a_lines.component, a_lines.componentText });
			}

			bool RegisterStandInObserver(const Api::StandInObserver& a_observer) override
			{
				// Without the primary cull nothing stands in, and nothing would call it.
				if (!StandInInstalled())
					return false;
				lists.standInObservers.push_back(a_observer);
				return true;
			}

			Api::EngineChannelId RegisterEngineChannel(const Api::EngineChannel& a_channel) override
			{
				// A SceneStack channel's events are on the scene's stack: no queue to drain.
				if (a_channel.name.empty() || (a_channel.kind != Api::ChannelKind::SceneStack && !a_channel.drain))
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} registered an incomplete engine channel", feature));
				if (FindEngineChannel(a_channel.name))
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {}'s engine channel {} has a name another channel has", feature, a_channel.name));
				// A batch holds each channel's keys in a slot of its own (Coordinator::EventBatch::channelKeys).
				if (lists.engineChannels.size() >= kEngineChannelSlots)
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {}'s engine channel {} is one past kEngineChannelSlots: grow it first", feature, a_channel.name));
				lists.engineChannels.push_back({ std::string(a_channel.name), a_channel.kind, a_channel.apply, a_channel.loading, a_channel.wake, a_channel.namesKeys,
					a_channel.drain, a_channel.push });
				return { static_cast<std::uint32_t>(lists.engineChannels.size() - 1) };
			}

			Api::EngineChannelId NeedEngineChannel(std::string_view a_name) override
			{
				if (a_name.empty())
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} needs an engine channel without a name", feature));
				// A name needed twice is one need.
				std::size_t slot = 0;
				while (slot < lists.engineChannelNeeds.size() && lists.engineChannelNeeds[slot] != a_name)
					++slot;
				if (slot == lists.engineChannelNeeds.size()) {
					// The bound table holds each need in a slot (EngineEvents.h).
					if (slot >= kEngineChannelNeedSlots)
						stl::report_and_fail(fmt::format("Drawcall Limit Fix: {}'s need of engine channel {} is one past kEngineChannelNeedSlots: grow it first", feature, a_name));
					lists.engineChannelNeeds.emplace_back(a_name);
				}
				return { kEngineChannelNeedBit | static_cast<std::uint32_t>(slot) };
			}

			void RegisterShowVeto(const Api::ShowVeto& a_veto) override
			{
				if (!a_veto.open || !a_veto.vetoes)
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} registered an incomplete show veto", feature));
				// The stubs load one veto's cells.
				if (std::exchange(lists.showVetoRegistered, true))
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} registered a show veto, which another registration has", feature));
				lists.showVetoHideSites.assign(a_veto.hideSites.begin(), a_veto.hideSites.end());
				lists.showVeto = { a_veto.open, a_veto.vetoes, a_veto.engineHid, lists.showVetoHideSites };
			}

			std::uint32_t ReserveHiddenSite() override { return HiddenSourceSites() + lists.reservedHiddenSites++; }
			void RegisterNodeCapture(const Api::NodeCaptureContribution& a_capture) override { AddNodeCapture(feature, a_capture); }
			Api::MoveWriterId ReserveMoveWriter(std::string_view a_name) override { return Extensions::ReserveMoveWriter(a_name); }
			void RegisterFrameVSVariablePatch() override { NoteFrameVSVariablePatch(); }

			Api::EngineChannelId FindEngineChannel(std::string_view a_name) const override
			{
				for (std::uint32_t c = 0; c < lists.engineChannels.size(); ++c)
					if (lists.engineChannels[c].name == a_name)
						return { c };
				return {};
			}

			Api::TableFamilyId RegisterTableFamily(const Api::TableFamily& a_family) override
			{
				if (a_family.name.empty() || !a_family.make || !a_family.assign || !a_family.clear || !a_family.recycle)
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} registered an incomplete table family", feature));
				// A SceneFit bounds each family's rows (Draws::SceneFit::familyRows).
				if (lists.tableFamilies.size() >= kTableFamilySlots)
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {}'s table family {} is one past kTableFamilySlots: grow it first", feature, a_family.name));
				// The reference must leave a bit above its shift, and a shifted or biased reference needs a word to ride in.
				if (a_family.recordShift >= 32 || ((a_family.recordShift || a_family.recordBias || !a_family.recordTag) && a_family.recordWord == ~0u))
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {}'s table family {} packs its record reference past the word, or into none", feature, a_family.name));
				lists.tableFamilies.push_back({ std::string(a_family.name), a_family.make, a_family.assign, a_family.assignRow, a_family.same, a_family.clear, a_family.recycle, a_family.rowsEnd,
					a_family.recordWord, a_family.recordShift, a_family.recordBias, a_family.recordTag, a_family.journaled, a_family.rows });
				return { static_cast<std::uint32_t>(lists.tableFamilies.size() - 1) };
			}

			Api::CoordinatorComponentId RegisterCoordinatorComponent(const Api::CoordinatorComponent& a_component) override
			{
				const auto& followed = a_component.entryClass;
				if (a_component.name.empty() || !a_component.make || followed.follows >= Api::EntryClass::Count)
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} registered an incomplete coordinator component", feature));
				// One component follows an entry class: it supplies what the class's writes wait on.
				if (followed.follows != Api::EntryClass::None)
					for (const auto& component : lists.coordinatorComponents)
						if (component.entryClass.follows == followed.follows)
							stl::report_and_fail(fmt::format("Drawcall Limit Fix: {}'s component {} follows entry class {}, which {} follows", feature, a_component.name,
								static_cast<std::uint32_t>(followed.follows), component.name));
				lists.coordinatorComponents.push_back({ std::string(a_component.name), a_component.make, a_component.slots, a_component.residency, a_component.accumulate, a_component.walk,
					a_component.dense, a_component.retirement, followed, a_component.events, a_component.commit, a_component.adoption });
				if (followed.follows != Api::EntryClass::None)
					lists.entryClassesTaken[static_cast<std::size_t>(followed.follows)] = followed.takes;
				return { static_cast<std::uint32_t>(lists.coordinatorComponents.size() - 1) };
			}

			void RegisterTechnique(const Api::TechniqueContribution& a_technique) override
			{
				// A property flag rule names the geometry type its flagged objects must be.
				if (a_technique.technique >= kTechniqueSlots || !a_technique.toggle || (a_technique.propertyFlags && a_technique.geometryType >= kGeometryTypeSlots))
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} registered an incomplete technique contribution", feature));
				auto& entry = lists.techniques[a_technique.technique];
				if (entry.toggle)
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} contributes technique {}, which another feature has", feature, a_technique.technique));
				entry = { a_technique.toggle, a_technique.objectFlags, a_technique.extras };
				// Its property flag rule, once for the techniques that share it (LODLand and LODLandNoise: one rule).
				if (a_technique.propertyFlags) {
					const PropertyFlagRule rule{ a_technique.propertyFlags, a_technique.geometryType, a_technique.toggle };
					const auto rules = std::span(lists.propertyFlagRules).first(lists.propertyFlagRuleCount);
					const bool known = std::ranges::any_of(rules, [&rule](const PropertyFlagRule& a_rule) {
						return a_rule.propertyFlags == rule.propertyFlags && a_rule.geometryType == rule.geometryType && a_rule.toggle.bit == rule.toggle.bit;
					});
					if (!known)
						lists.propertyFlagRules[lists.propertyFlagRuleCount++] = rule;
				}
			}

			void RegisterTechniqueGate(const Api::TechniqueGate& a_gate) override
			{
				// It gates a contribution registered before (an integration's gates a feature's technique).
				if (a_gate.technique >= kTechniqueSlots || !lists.techniques[a_gate.technique].toggle || a_gate.whileOn.index >= lists.classifyInputs.size() || !a_gate.needs)
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} registered an incomplete technique gate", feature));
				auto& entry = lists.techniques[a_gate.technique];
				if (entry.gateNeeds)
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} gates technique {}, which another gate has", feature, a_gate.technique));
				entry.gateWhile = a_gate.whileOn;
				entry.gateNeeds = a_gate.needs;
			}

			void RegisterGeometryType(const Api::GeometryTypeContribution& a_type) override
			{
				// A toggle, but for a contribution that names an entry class alone; an entry class with the parent kinds it is under.
				const auto& entry = a_type.entry;
				const bool entryValid = entry.entryClass < Api::EntryClass::Count &&
				                        (entry.entryClass == Api::EntryClass::None ? !entry.parentKind && !entry.perFrame && !entry.holdsParent : entry.parentKind != 0);
				const bool classValid = a_type.toggle ||
				                        (!a_type.propertyFlags && a_type.itemClass == Api::FrameValueItemClass::None && !a_type.moves && !a_type.ranges &&
				                            entry.entryClass != Api::EntryClass::None);
				if (a_type.geometryType >= kGeometryTypeSlots || a_type.itemClass >= Api::FrameValueItemClass::Count || !entryValid || !classValid)
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} registered an incomplete geometry type contribution", feature));
				auto& contribution = lists.geometryTypeContributions[a_type.geometryType];
				if (contribution.geometryType != 0xFF)
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} contributes geometry type {}, which another feature has", feature, static_cast<std::uint32_t>(a_type.geometryType)));
				contribution = a_type;
				lists.geometryTypes[a_type.geometryType] = a_type.toggle;
				// One rule an entry class: a second type naming it names it alike.
				if (entry.entryClass != Api::EntryClass::None) {
					if (lists.ownerClassRegistered[static_cast<std::size_t>(entry.entryClass)])
						stl::report_and_fail(fmt::format("Drawcall Limit Fix: {}'s geometry type {} names entry class {}, an owner class", feature,
							static_cast<std::uint32_t>(a_type.geometryType), static_cast<std::uint32_t>(entry.entryClass)));
					auto& rule = lists.entryClassRules[static_cast<std::size_t>(entry.entryClass)];
					if (rule.entryClass != Api::EntryClass::None && (rule.perFrame != entry.perFrame || rule.holdsParent != entry.holdsParent))
						stl::report_and_fail(fmt::format("Drawcall Limit Fix: {}'s geometry type {} names entry class {} unlike another contribution", feature,
							static_cast<std::uint32_t>(a_type.geometryType), static_cast<std::uint32_t>(entry.entryClass)));
					rule = entry;
				}
			}

			void RegisterParent(const Api::ParentContribution& a_parent) override
			{
				if (a_parent.parent >= Api::EngineParent::Count || a_parent.leaves >= Api::ParentLeaves::Count || !a_parent.toggle)
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} registered an incomplete parent contribution", feature));
				auto& toggle = lists.parents[static_cast<std::size_t>(a_parent.parent)][static_cast<std::size_t>(a_parent.leaves)];
				if (toggle)
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} contributes parent {} leaves {}, which another feature has", feature,
						static_cast<std::uint32_t>(a_parent.parent), static_cast<std::uint32_t>(a_parent.leaves)));
				toggle = a_parent.toggle;
			}

			void RegisterObjectClass(const Api::ObjectClassContribution& a_class) override
			{
				if (a_class.objectClass >= Api::ObjectClass::Count || !a_class.toggle)
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} registered an incomplete object class contribution", feature));
				auto& toggle = lists.objectClasses[static_cast<std::size_t>(a_class.objectClass)];
				if (toggle)
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} contributes object class {}, which another feature has", feature,
						static_cast<std::uint32_t>(a_class.objectClass)));
				toggle = a_class.toggle;
				lists.categoryRoots[static_cast<std::size_t>(a_class.objectClass)] = a_class.categoryRoot;
			}

			void RegisterEntryClass(const Api::EntryClassContribution& a_class) override
			{
				// Its own motion (every frame, no event) makes its entries per frame.
				if (a_class.entryClass == Api::EntryClass::None || a_class.entryClass >= Api::EntryClass::Count || a_class.name.empty() ||
					(a_class.ownMotion && !a_class.everyFrame))
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} registered an incomplete entry class contribution", feature));
				const auto index = static_cast<std::size_t>(a_class.entryClass);
				auto& owner = lists.ownerClasses[a_class.ownerFormType];
				if (lists.ownerClassRegistered[index] || owner.ownerClass != Api::EntryClass::None ||
					lists.entryClassRules[index].entryClass != Api::EntryClass::None)
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} contributes entry class {} (form type {}), which another contribution has", feature,
						static_cast<std::uint32_t>(a_class.entryClass), static_cast<std::uint32_t>(a_class.ownerFormType)));
				// The timing report's labels: the registered names, in registration order.
				const bool first = std::ranges::none_of(lists.ownerClassRegistered, std::identity{});
				lists.ownerClassRegistered[index] = true;
				owner = { a_class.entryClass, a_class.everyFrame, a_class.ownMotion, a_class.published };
				lists.ownedLabel = first ? std::string(a_class.name) : lists.ownedLabel + "/" + std::string(a_class.name);
				lists.ownedWithoutRecordLabel = lists.ownedLabel + " without a record";
			}

			void RegisterShading(const Api::ShadingContribution& a_shading) override
			{
				// A block's variables lie in its words, each one at most a word (a variable mask bit each: Extensions::ShadingBlock::vsMask).
				const auto blockValid = [&] {
					if (a_shading.blockWords > Api::kShadingBlockWords || (a_shading.blockWords && !a_shading.objectFlags) ||
						a_shading.vsVariables.size() > a_shading.blockWords)
						return false;
					for (const auto& variable : a_shading.vsVariables)
						if (variable.variable >= 64 || !variable.floats || variable.compared > variable.floats ||
							std::uint32_t(variable.firstWord) + variable.floats > a_shading.blockWords)
							return false;
					return true;
				};
				if (a_shading.name.empty() || !a_shading.propertyFlags || !a_shading.derive || !blockValid())
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} registered an incomplete shading contribution", feature));
				// One block slot.
				if (a_shading.blockWords)
					for (const auto& other : lists.shadingContributions)
						if (other.blockWords)
							stl::report_and_fail(fmt::format("Drawcall Limit Fix: {}'s shading contribution {} claims the feature block, which {} has", feature,
								a_shading.name, other.name));
				lists.shadingContributions.push_back({ std::string(a_shading.name), a_shading.propertyFlags, a_shading.derive, a_shading.objectFlags,
					a_shading.blockWords, a_shading.vsVariables });
			}

			Api::FrameGlobalsSlot RegisterFrameGlobals(const Api::FrameGlobalsContribution& a_globals) override
			{
				if (a_globals.name.empty() || !a_globals.floats || !a_globals.capture)
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} registered an incomplete frame-globals contribution", feature));
				if (lists.frameGlobalsFloats + a_globals.floats > kFrameGlobalsFloats)
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {}'s frame globals {} are past kFrameGlobalsFloats: grow it first", feature, a_globals.name));
				const std::uint32_t first = lists.frameGlobalsFloats;
				lists.frameGlobals.push_back({ std::string(a_globals.name), first, a_globals.floats, a_globals.capture });
				lists.frameGlobalsFloats += a_globals.floats;
				return { first };
			}

			void RegisterView(const Api::ViewContribution& a_view) override
			{
				// Its callbacks: the coverage always, the root for a CategoryRoot view, the accumulators for an InMainRenderers one, the residue
				// question only for an OwnRenderers one (the watch is the pass capture's OwnRenderers test). Its phase and view bit are checked
				// against Core's and the other views' once all are in (ResolveViews).
				if (a_view.view >= Api::EngineView::Count || !a_view.toggle || !a_view.phase || !a_view.shape.viewBits || !a_view.covered ||
					(a_view.membership == Api::ViewMembership::CategoryRoot && !a_view.captureRoot) ||
					(a_view.withholding == Api::ViewWithholding::InMainRenderers && !a_view.accumulators) ||
					(a_view.withholding != Api::ViewWithholding::OwnRenderers && a_view.residueDue))
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} registered an incomplete view contribution", feature));
				auto& entry = lists.views[static_cast<std::size_t>(a_view.view)];
				if (entry.view != Api::EngineView::Count)
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} contributes view {}, which another feature has", feature, static_cast<std::uint32_t>(a_view.view)));
				entry = a_view;
			}

			void RegisterViewWork(const Api::ViewWork& a_work) override
			{
				if (a_work.view >= Api::EngineView::Count || a_work.point >= Api::ViewPoint::Count || !a_work.run)
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} registered incomplete view work", feature));
				lists.viewWork.push_back(a_work);
			}

			void RegisterViewRoot(const Api::ViewRootContribution& a_root) override
			{
				if (a_root.root >= Api::ViewRoot::Count || !a_root.owned)
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} registered an incomplete view root contribution", feature));
				// One owner a root: a view's owned roots are its owners' answers.
				if (ViewRootTaken(lists, a_root.root))
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} contributes view root {}, which another registration has", feature,
						static_cast<std::uint32_t>(a_root.root)));
				lists.viewRoots[static_cast<std::size_t>(a_root.root)] = a_root;
			}

			void RegisterCullHiddenSource(const Api::CullHiddenSource& a_source) override
			{
				if (!a_source.nodes || a_source.bit > Api::CullHiddenBit::Hidden)
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} registered an incomplete cull hidden source", feature));
				lists.cullHiddenSources.push_back(a_source);
			}

			void RegisterOcclusionMap(const Api::OcclusionMapContribution& a_map) override
			{
				// Its toggle, flag, bits, rule, camera and accumulator test always, the import for an ImportedTexture map. Its bits are checked against Core's and the other
				// views' once all are in (ResolveViewBits).
				if (a_map.map >= Api::OcclusionMap::Count || !a_map.toggle || !a_map.available || !a_map.bits.phase || !a_map.bits.viewBit ||
					!a_map.technique || !a_map.camera || !a_map.ownsAccumulator || (a_map.target == Api::OcclusionTarget::ImportedTexture && !a_map.importTarget))
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} registered an incomplete occlusion map contribution", feature));
				auto& entry = lists.occlusionMaps[static_cast<std::size_t>(a_map.map)];
				if (entry.map != Api::OcclusionMap::Count)
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} contributes occlusion map {}, which another feature has", feature, static_cast<std::uint32_t>(a_map.map)));
				entry = a_map;
			}

			void RegisterShadowSource(const Api::ShadowSourceContribution& a_source) override
			{
				// A local light source is told of each light the selection keeps; the Sun alone ends the full-frustum window.
				const bool local = a_source.source == Api::ShadowSource::Spot || a_source.source == Api::ShadowSource::Point;
				const bool sun = a_source.source == Api::ShadowSource::Sun;
				if (a_source.source >= Api::ShadowSource::Count || !a_source.camera || (local && !a_source.lightKept) || sun != !!a_source.fullFrustumWindowEnd)
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} registered an incomplete shadow source contribution", feature));
				auto& entry = lists.shadowSources[static_cast<std::size_t>(a_source.source)];
				if (entry.source != Api::ShadowSource::Count)
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} contributes shadow source {}, which another feature has", feature,
						static_cast<std::uint32_t>(a_source.source)));
				entry = a_source;
			}

			void RegisterShadowExclusion(const Api::ShadowExclusionContribution& a_exclusion) override
			{
				if (a_exclusion.name.empty() || !a_exclusion.build || !a_exclusion.publish || !a_exclusion.frameStart || !a_exclusion.listExclusion)
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} registered an incomplete shadow exclusion contribution", feature));
				// The payload carries one exclusion per mode.
				if (!lists.shadowExclusion.name.empty())
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} contributes shadow exclusion {}, which {} has", feature, a_exclusion.name,
						lists.shadowExclusion.name));
				lists.shadowExclusion = a_exclusion;
			}

			void RegisterDrawFamily(const Api::DrawFamilyContribution& a_family) override
			{
				// Its class's toggle gates it, its scene resources (registered before) are its tables; its frame and main callbacks, its
				// program and its engine shapes always, its faces' all or none.
				const auto& main = a_family.main;
				const auto& program = a_family.program;
				// One or two colour programs (the second named), an engine write mode (0-12), and a view's forward program only with faces.
				const bool programValid = program.shaderType != RE::BSShader::Type::Lighting && program.shaderType < RE::BSShader::Type::Total && !program.name.empty() &&
				                          !program.label.empty() && !program.keyName.empty() && !program.sourcePath.empty() && a_family.engineShapes.propertyVtable &&
				                          (program.colours == 1 || (program.colours == 2 && program.descriptors.colour2)) && program.writeMode < 13 &&
				                          (a_family.faces.face || !program.descriptors.forward);
				if (a_family.family >= Api::DrawFamily::Count || a_family.objectClass >= Api::ObjectClass::Count ||
					!lists.objectClasses[static_cast<std::size_t>(a_family.objectClass)] || a_family.resources.index >= lists.sceneResources.size() ||
					!a_family.frame.decide || !a_family.frame.programSource || !main.declare || !main.revision || !main.record || !programValid ||
					!DrawFamilyFacesComplete(a_family.faces))
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} registered an incomplete draw family contribution", feature));
				auto& entry = lists.drawFamilies[static_cast<std::size_t>(a_family.family)];
				if (entry.family != Api::DrawFamily::Count)
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} contributes draw family {}, which another registration has", feature,
						static_cast<std::uint32_t>(a_family.family)));
				// Its programs are found by their shader type (Extensions::DrawFamilyOfShader); its faces' root has one owner.
				if (ProgramShaderTaken(lists, program.shaderType))
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {}'s draw family {} takes shader type {}, which another family has", feature, program.name,
						static_cast<std::uint32_t>(program.shaderType)));
				if (a_family.faces.root < Api::ViewRoot::Count && ViewRootTaken(lists, a_family.faces.root))
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {}'s draw family {} draws view root {}, which another registration has", feature, program.name,
						static_cast<std::uint32_t>(a_family.faces.root)));
				entry = a_family;
			}

			void RegisterMainRange(const Api::MainRangeContribution& a_range) override { AddMainRange(lists, feature, a_range, false); }
			void RegisterHeldGroup(const Api::HeldGroupContribution& a_group) override { AddHeldGroup(lists, feature, a_group); }
			void RegisterNearTwin(const Api::NearTwinContribution& a_twin) override { SetNearTwin(lists, feature, a_twin); }

			void RegisterVertexStream(const Api::VertexStreamContribution& a_stream) override
			{
				// Its scene resources (registered before) hold its buffer; a component follows its entry class (registered before).
				bool followed = false;
				for (const auto& component : lists.coordinatorComponents)
					followed = followed || (a_stream.entryClass != Api::EntryClass::None && component.entryClass.follows == a_stream.entryClass);
				if (a_stream.name.empty() || !a_stream.select || !a_stream.upload || a_stream.resources.index >= lists.sceneResources.size() || !followed)
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} registered an incomplete vertex stream", feature));
				if (lists.vertexStream.entryClass != Api::EntryClass::Count)
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} contributes vertex stream {}, which {} has: Core's draws have one second stream", feature,
						a_stream.name, lists.vertexStream.name));
				lists.vertexStream = a_stream;
			}

			void RegisterPaletteRows(const Api::PaletteRowsContribution& a_rows) override
			{
				const auto& rule = a_rows.rule;
				const bool ruleValid = rule.rowsPerUnit && rule.minUnits && rule.minUnits <= rule.maxUnits && (!rule.pow2 || std::has_single_bit(rule.maxUnits));
				if (a_rows.itemClass == Api::FrameValueItemClass::None || a_rows.itemClass >= Api::FrameValueItemClass::Count || a_rows.name.empty() || !ruleValid ||
					!a_rows.geometrySource || !a_rows.beginRun || !a_rows.sampleChunk || !a_rows.endRun)
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} registered an incomplete palette rows class", feature));
				auto& entry = lists.paletteRows[static_cast<std::size_t>(a_rows.itemClass) - 1];
				if (entry.itemClass != Api::FrameValueItemClass::Count)
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} contributes palette rows class {}, which {} has", feature,
						static_cast<std::uint32_t>(a_rows.itemClass), entry.name));
				entry = a_rows;
			}

			std::uint8_t RegisterMaterialPort(const Api::MaterialPortContribution& a_port) override { return AddMaterialPort(lists, feature, a_port); }
			void RegisterProgramFamily(const Api::ProgramFamilyContribution& a_family) override { AddProgramFamily(lists, feature, a_family); }
			void RegisterFrameConstantRow(const Api::FrameConstantRow& a_row) override { AddFrameConstantRow(lists, feature, a_row); }
			void RegisterFrameTexture(const Api::FrameTexture& a_texture) override { AddFrameTexture(lists, feature, a_texture); }

			Api::ShadingFieldId RegisterShadingField(const Api::ShadingFieldContribution& a_field) override
			{
				if (a_field.field >= Api::ShadingRowField::Count || a_field.name.empty() || !a_field.membership || !a_field.capture ||
					a_field.members == Api::EntryClass::None || a_field.members >= Api::EntryClass::Count)
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} registered an incomplete shading field", feature));
				if (std::exchange(lists.shadingFieldTaken[static_cast<std::size_t>(a_field.field)], true))
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} contributes shading field {} ({}), which another registration has", feature,
						a_field.name, static_cast<std::uint32_t>(a_field.field)));
				lists.shadingFields.push_back(a_field);
				return { static_cast<std::uint32_t>(lists.shadingFields.size() - 1) };
			}

			void RegisterMainCullInput(const Api::MainCullInputContribution& a_input) override
			{
				if (a_input.name.empty() || !a_input.unboundVisible || !a_input.structure || !a_input.programsVersion || !a_input.judge || !a_input.test)
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} registered an incomplete main cull input", feature));
				// The scene lists' keep check and the depth commits' programs read one walk.
				if (!lists.mainCullInput.name.empty())
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} contributes main cull input {}, which {} has", feature, a_input.name, lists.mainCullInput.name));
				lists.mainCullInput = a_input;
			}

			void RegisterCullBinding(const Api::CullBinding& a_binding) override
			{
				if (a_binding.slot >= Api::CullBindingSlot::Count || a_binding.name.empty() || !a_binding.select)
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} registered an incomplete cull binding", feature));
				// Core's cull reads each slot at one descriptor word.
				auto& taken = lists.cullBindings[static_cast<std::size_t>(a_binding.slot)];
				if (taken.select)
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {}'s cull binding {} takes slot {}, which {} has", feature, a_binding.name,
						static_cast<std::uint32_t>(a_binding.slot), taken.name));
				taken = a_binding;
			}

			void RegisterDepthPyramid(const Api::DepthPyramidContribution& a_pyramid) override
			{
				if (a_pyramid.resource.empty() || a_pyramid.scratchResource.empty() || !a_pyramid.setup)
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} registered an incomplete depth pyramid", feature));
				// The cull tests one pyramid.
				if (lists.depthPyramid.setup)
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} contributes depth pyramid {}, which another feature has", feature, a_pyramid.resource));
				lists.depthPyramid = a_pyramid;
			}

			void RegisterPrimaryView(const Api::PrimaryViewContribution& a_view) override
			{
				if (!a_view.beforeOpaque || !a_view.afterOpaque)
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} registered an incomplete primary view", feature));
				// FrameDriver's main pass points call one main view.
				if (lists.primaryView.beforeOpaque)
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} contributes a primary view, which {} has", feature, lists.primaryViewFeature));
				lists.primaryView = a_view;
				lists.primaryViewFeature = std::string(feature);
			}

			void RegisterShadowEpochs(const Api::ShadowEpochsContribution& a_epochs) override
			{
				if (!a_epochs.beforeShadowCasters || !a_epochs.beforeShadowMaps || !a_epochs.afterShadowMaps || !a_epochs.occlusionReady)
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} registered incomplete shadow epochs", feature));
				// FrameDriver's shadow and occlusion points call one shadow engine's epoch.
				if (lists.shadowEpochs.afterShadowMaps)
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} contributes the shadow epochs, which {} has", feature, lists.shadowEpochsFeature));
				lists.shadowEpochs = a_epochs;
				lists.shadowEpochsFeature = std::string(feature);
			}

			void RegisterRootStates(const Api::RootStatesContribution& a_states) override
			{
				// Its family registered before (its objects' references are the roots Core reads), and the callbacks Core calls per decode and join.
				if (a_states.name.empty() || !a_states.family || a_states.family.index >= lists.tableFamilies.size() || !a_states.view || !a_states.test)
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} registered incomplete root states", feature));
				// The cull, the shadows and the draws read one producer's states.
				if (!lists.rootStates.name.empty())
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} contributes root states {}, which {} has", feature, a_states.name, lists.rootStates.name));
				lists.rootStates = a_states;
			}

			void RegisterMaterialContributor(const Api::MaterialContributor& a_contributor) override { AddMaterialContributor(lists, feature, a_contributor); }
			Api::ReferenceFactId RegisterReferenceFact(const Api::ReferenceFact& a_fact) override { return AddReferenceFact(lists, feature, a_fact); }

			void DeclareGeometryExtra(std::string_view a_extraName) override
			{
				if (a_extraName.empty())
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} declared a geometry extra without a name", feature));
				// The geometry record keeps one extra value.
				auto& declared = lists.lightingJoinRules.geometryExtra;
				if (!declared.empty())
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} declared geometry extra {}, but {} is declared", feature, a_extraName, declared));
				declared = std::string(a_extraName);
			}

			void RegisterPropertyBits(const Api::PropertyBits& a_bits) override { AddPropertyBits(lists, feature, a_bits); }
			void RegisterLightingObjectFlag(const Api::LightingObjectFlag& a_flag) override { AddLightingObjectFlag(lists, feature, a_flag); }

			void RegisterLightingKeyBits(const Api::LightingKeyBits& a_bits) override
			{
				if (!a_bits.keyBits || !a_bits.permutation)
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} registered incomplete Lighting key bits", feature));
				// The Lighting key has one set of spare bits.
				auto& claimed = lists.lightingJoinRules.keyBits;
				if (claimed.keyBits)
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} registered Lighting key bits, which another registration has", feature));
				claimed = a_bits;
			}

			void RegisterLightingPassRules(const Api::LightingPassRules& a_rules) override { AddLightingPassRules(lists, feature, a_rules); }
			void DeclarePerDrawBuffer(const Api::PerDrawBuffer& a_buffer) override { AddPerDrawBuffer(lists, feature, a_buffer); }

			std::string_view feature;                       // the feature installing
			const Api::FrameSchedule* schedule = nullptr;  // the schedule InstallFeatures resolves (FindTrigger)

		private:
			Lists& lists;
		};
	}

	void InstallEarlyHooks(std::span<const Api::Feature> a_features)
	{
		static bool installed = false;
		if (std::exchange(installed, true) || Registered().installed)
			stl::report_and_fail("Drawcall Limit Fix: the features' early hooks were installed twice, or after the features");
		for (const auto& feature : a_features)
			if (feature.earlyHooks)
				feature.earlyHooks();
	}

	void InstallFeatures(std::span<const Api::Feature> a_features, std::span<const Api::Feature> a_integrations, const Api::FrameSchedule& a_schedule)
	{
		auto& lists = Registered();
		if (std::exchange(lists.installed, true))
			stl::report_and_fail("Drawcall Limit Fix: the features were installed twice");
		// Core's passes (Core/Render/Passes/CorePasses.cpp) before the features', so a feature's key that is Core's fails its install.
		Draws::RegisterCorePasses();
		// Lighting's space and rules as kind 0; the features' material ports take kinds from 1 in their registration order, which the records
		// they write name (RegisterMaterialPort returns it).
		lists.materialPorts[lists.materialPortCount++] = CoreLightingPort();
		Context context(lists);
		context.schedule = &a_schedule;
		for (const auto& feature : a_features) {
			context.feature = feature.name;
			if (feature.install)
				feature.install(context);
		}
		// The integrations after every feature (Integrations/IntegrationList.cpp): what they register may name what a feature registered
		// (Integrations/AdvancedSkin's wetness: Features/Actors' published actor-owned class). They have no early or source hooks (the
		// shell installs those for the features alone).
		for (const auto& integration : a_integrations) {
			if (integration.earlyHooks || integration.sourceHooks)
				stl::report_and_fail(fmt::format("Drawcall Limit Fix: integration {} has early or source hooks", integration.name));
			context.feature = integration.name;
			if (integration.install)
				integration.install(context);
		}
		// The features' switches' anchors, which may name a switch a later feature registered.
		CheckSwitchAnchors();
		// The classify inputs needed by name, which a later feature or an integration may register; then the light volumes, the sun volume
		// and the ancestor value (its key classes' RTTI).
		BindClassifyInputNeeds(lists.classifyInputNeeds);
		// The engine channels needed by name (Features/Actors' fade amounts, a later feature's channel).
		BindEngineChannelNeeds(lists.engineChannelNeeds);
		ResolveLightVolumes(lists.lightVolumes);
		ResolveSunVolume(lists.sunVolume);
		ResolveAncestorValue(lists.ancestorValue);
		// The views' entries, from Core's slots and the features' contributions, work and view roots; the occlusion maps', shadow sources' and shadow exclusion's, from
		// the features' contributions; then both families' phases and view bits; the draw families' from their registrations; the main
		// ranges' draw order; the vertex stream's (and the scene resources' order: its first); the main cull input's; the cull bindings'; the depth pyramid's; the primary view's; the shadow epochs'; the root states'; the palette rows classes'; the shading fields'
		// (and the published entry classes); the material ports'
		// (their engine addresses and spaces); the program families' (their ports' kinds); the frame constant rows, in row order.
		ResolveViews(lists.views, lists.viewWork, lists.viewRoots, lists.cullHiddenSources);
		ResolveShadowViews(lists.occlusionMaps, lists.shadowSources, lists.shadowExclusion);
		ResolveViewBits();
		ResolveDrawFamilies(lists.drawFamilies);
		ResolveMainRanges(lists.mainRanges);
		ResolveVertexStream(lists.vertexStream);
		ResolveMainCullInput(lists.mainCullInput);
		ResolveCullBindings(lists.cullBindings);
		ResolveDepthPyramid(lists.depthPyramid);
		ResolvePrimaryView(lists.primaryView);
		ResolveShadowEpochs(lists.shadowEpochs);
		ResolveRootStates(lists.rootStates);
		ResolvePaletteRows(lists.paletteRows);
		ResolveShadingFields(lists.shadingFields);
		ResolveMaterialPorts(lists.materialPorts, lists.materialPortCount);
		// The material contributors' order (stage, order, registration), feature texture slots, witness words and classes.
		ResolveMaterialContributors(lists.materialContributors);
		ResolveProgramFamilies(lists.programFamilies);
		std::ranges::sort(lists.frameConstantRows, {}, &FrameConstantRowEntry::row);
		// The Core flags' needs by name resolved, then the features' toggles placed in the menu's table by their anchors (which may
		// name a toggle a later feature registered), then seeded from their switches, into the requested and active sets.
		Toggles::Get().SeedRegistered();
		// The frame's schedule against every registered pass: a key nobody registered fails the install.
		Draws::ResolveSchedule(a_schedule);
	}

	std::span<const FrameValuePoolEntry> RegisteredPools() { return Registered().pools; }
	std::span<const PayloadProductEntry> RegisteredProducts() { return Registered().products; }
	std::span<const SceneResourcesEntry> RegisteredSceneResources() { return Registered().sceneResources; }
	std::span<const CommitHookEntry> RegisteredCommitHooks() { return Registered().commitHooks; }
	std::span<const FrameBindingEntry> RegisteredFrameBindings() { return Registered().frameBindings; }
	std::span<const FrameConstantRowEntry> RegisteredFrameConstantRows() { return Registered().frameConstantRows; }
	std::span<const FrameTextureEntry> RegisteredFrameTextures() { return Registered().frameTextures; }
	std::span<const FrameGraphBuffersEntry> RegisteredFrameGraphBuffers() { return Registered().frameGraphBuffers; }
	std::span<const std::uint8_t> FrameTextureIndices() { return Registered().frameTextureIndices; }
	std::span<const ReportLinesEntry> RegisteredReportLines() { return Registered().reportLines; }
	std::span<const Api::StandInObserver> RegisteredStandInObservers() { return Registered().standInObservers; }
	std::span<const EngineChannelEntry> RegisteredEngineChannels() { return Registered().engineChannels; }
	const Api::ShowVeto& RegisteredShowVeto() { return Registered().showVeto; }
	bool ShowVetoRegistered() { return Registered().showVetoRegistered; }
	std::uint32_t ReservedHiddenSites() { return Registered().reservedHiddenSites; }
	std::span<const TableFamilyEntry> RegisteredTableFamilies() { return Registered().tableFamilies; }
	std::span<const CoordinatorComponentEntry> RegisteredCoordinatorComponents() { return Registered().coordinatorComponents; }
	const TechniqueEntries& RegisteredTechniques() { return Registered().techniques; }
	const GeometryTypeToggles& RegisteredGeometryTypes() { return Registered().geometryTypes; }
	const GeometryTypeContributions& RegisteredGeometryTypeContributions() { return Registered().geometryTypeContributions; }
	const EntryClassRules& RegisteredEntryClassRules() { return Registered().entryClassRules; }
	const OwnerClasses& RegisteredOwnerClasses() { return Registered().ownerClasses; }
	std::string_view OwnedEntriesLabel(bool a_withoutRecord) { return a_withoutRecord ? Registered().ownedWithoutRecordLabel : Registered().ownedLabel; }
	std::span<const PropertyFlagRule> PropertyFlagRules() { return std::span(Registered().propertyFlagRules).first(Registered().propertyFlagRuleCount); }
	const CategoryRoots& RegisteredCategoryRoots() { return Registered().categoryRoots; }
	const ParentToggles& RegisteredParents() { return Registered().parents; }
	const ObjectClassToggles& RegisteredObjectClasses() { return Registered().objectClasses; }
	std::span<const ShadingContributionEntry> RegisteredShadingContributions() { return Registered().shadingContributions; }
	std::span<const FrameGlobalsEntry> RegisteredFrameGlobals() { return Registered().frameGlobals; }
	std::span<const ClassifyInputEntry> RegisteredClassifyInputs() { return Registered().classifyInputs; }
	std::span<const HeldGroupEntry> HeldGroups() { return Registered().heldGroups; }
	const NearTwinEntry& NearTwin() { return Registered().nearTwin; }
	const LightingJoinRules& RegisteredLightingJoinRules() { return Registered().lightingJoinRules; }

	std::uint32_t PerDrawMask(Api::ShaderStage a_stage)
	{
		const auto stage = static_cast<std::size_t>(a_stage);
		return stage < Registered().perDrawMasks.size() ? Registered().perDrawMasks[stage] : 0u;
	}

	bool EntryClassTaken(Api::EntryClass a_class)
	{
		return a_class < Api::EntryClass::Count && Registered().entryClassesTaken[static_cast<std::size_t>(a_class)];
	}
}
