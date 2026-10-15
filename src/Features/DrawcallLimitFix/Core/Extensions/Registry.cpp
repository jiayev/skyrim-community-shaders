// The extension registry (see the header): the install context the features register with, and the lists it fills.

#include "Features/DrawcallLimitFix/Core/Extensions/Registry.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Commits.h"
#include "Features/DrawcallLimitFix/Core/Extensions/CoordinatorComponents.h"
#include "Features/DrawcallLimitFix/Core/Extensions/DrawFamilies.h"
#include "Features/DrawcallLimitFix/Core/Extensions/EngineEvents.h"
#include "Features/DrawcallLimitFix/Core/Extensions/FrameBindings.h"
#include "Features/DrawcallLimitFix/Core/Extensions/MainRanges.h"
#include "Features/DrawcallLimitFix/Core/Extensions/MaterialPorts.h"
#include "Features/DrawcallLimitFix/Core/Extensions/NodeCaptures.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ObjectContributors.h"
#include "Features/DrawcallLimitFix/Core/Extensions/PaletteRows.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ProgramFamilies.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ShadowViews.h"
#include "Features/DrawcallLimitFix/Core/Extensions/TableFamilies.h"
#include "Features/DrawcallLimitFix/Core/Extensions/VertexStreams.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ViewCommon.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Views.h"
#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"

#include <algorithm>
#include <array>
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
			std::vector<GpuStageEntry> stages;
			std::vector<PayloadProductEntry> products;
			std::vector<SceneResourcesEntry> sceneResources;
			std::uint32_t sceneResourceInputs = 0;
			std::vector<CommitHookEntry> commitHooks;
			std::vector<FrameBindingEntry> frameBindings;
			std::vector<ReportLinesEntry> reportLines;
			std::vector<Api::StandInObserver> standInObservers;
			std::vector<EngineChannelEntry> engineChannels;
			std::vector<AdoptionGuardEntry> adoptionGuards;
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
			ParentToggles parents{};
			ObjectClassToggles objectClasses{};
			CategoryRoots categoryRoots{};
			std::vector<ShadingContributionEntry> shadingContributions;
			std::vector<FrameGlobalsEntry> frameGlobals;
			std::uint32_t frameGlobalsFloats = 0;
			// By view: its contribution (EngineView::Count: none); the views' work, in order. Resolved at the end (ResolveViews).
			std::array<Api::ViewContribution, kEngineViews> views{};
			std::vector<Api::ViewWork> viewWork;
			// By map and by source: its contribution (Count: none). Resolved at the end (ResolveShadowViews).
			std::array<Api::OcclusionMapContribution, kOcclusionMaps> occlusionMaps{};
			std::array<Api::ShadowSourceContribution, kShadowSources> shadowSources{};
			// By family: its contribution (Count: none). Resolved at the end (ResolveDrawFamilies).
			std::array<Api::DrawFamilyContribution, kDrawFamilies> drawFamilies{};
			// By slot: its range (Count: none), Core's built-ins first. Resolved at the end (ResolveMainRanges).
			std::array<Api::MainRangeContribution, kMainRangeSlots> mainRanges{};
			// The vertex stream (entry class Count: none). Resolved at the end (ResolveVertexStream).
			Api::VertexStreamContribution vertexStream{};
			// By palette rows class (its index - 1): its registration (itemClass Count: none). Resolved at the end (ResolvePaletteRows).
			std::array<Api::PaletteRowsContribution, Api::kPaletteRowsClasses> paletteRows{};
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
			bool installed = false;
		};

		/**
		 * @brief Adds a_range by its slot, refused (a_who's) for a slot or order already registered, no name, a depth prepass on an
		 * appended slot, or a feature's class with no registered toggle (Core's built-ins have none: Count).
		 */
		void AddMainRange(Lists& a_lists, std::string_view a_who, const Api::MainRangeContribution& a_range, bool a_builtIn)
		{
			const bool classValid = a_builtIn ? a_range.objectClass == Api::ObjectClass::Count :
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
		 * @brief Adds a_port at the next kind, which it returns, refused (a_who's) for no property RTTI, material vtable or callbacks,
		 * Lighting's or no shader type, a space past kMaxStageVariables or with names not one a variable, a class with no registered toggle,
		 * a shader type another port has, or a kind past kMaterialKinds.
		 */
		std::uint8_t AddMaterialPort(Lists& a_lists, std::string_view a_who, const Api::MaterialPortContribution& a_port)
		{
			const auto spaceValid = [](const Api::MaterialStageSpace& a_stage) {
				return a_stage.variables <= kMaxStageVariables && (a_stage.names.empty() || a_stage.names.size() == a_stage.variables);
			};
			const bool classValid = a_port.objectClass < Api::ObjectClass::Count && a_lists.objectClasses[static_cast<std::size_t>(a_port.objectClass)];
			if (a_port.shaderType == RE::BSShader::Type::Lighting || a_port.shaderType >= RE::BSShader::Type::Total || !classValid || !a_port.propertyRtti ||
				!a_port.materialVtable || !a_port.capture || !a_port.derive || !a_port.evaluate || !a_port.shading || !spaceValid(a_port.vs) || !spaceValid(a_port.ps))
				stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} registered an incomplete material port", a_who));
			for (std::size_t kind = 1; kind < a_lists.materialPortCount; ++kind)
				if (a_lists.materialPorts[kind].shaderType == a_port.shaderType)
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {}'s material port takes shader type {}, which another port has", a_who,
						static_cast<std::uint32_t>(a_port.shaderType)));
			if (a_lists.materialPortCount >= kMaterialKinds)
				stl::report_and_fail(fmt::format("Drawcall Limit Fix: {}'s material port is one past kMaterialKinds: grow it first", a_who));
			a_lists.materialPorts[a_lists.materialPortCount] = a_port;
			return static_cast<std::uint8_t>(a_lists.materialPortCount++);
		}

		/**
		 * @brief Adds a_family by its family, refused (a_who's) for a family already registered, Lighting's or no shader type, no name, key
		 * name or source path, or no program source.
		 */
		void AddProgramFamily(Lists& a_lists, std::string_view a_who, const Api::ProgramFamilyContribution& a_family)
		{
			if (a_family.family >= Api::ProgramFamily::Count || a_family.shaderType == RE::BSShader::Type::Lighting || a_family.shaderType >= RE::BSShader::Type::Total ||
				a_family.name.empty() || a_family.keyName.empty() || a_family.sourcePath.empty() || !a_family.programSource)
				stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} registered an incomplete program family", a_who));
			auto& entry = a_lists.programFamilies[static_cast<std::size_t>(a_family.family)];
			if (entry.family != Api::ProgramFamily::Count)
				stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} contributes program family {}, which {} has", a_who, a_family.name, entry.name));
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
			// A colour commit samples every view into a fixed array (ResolveFrameTextures).
			if (a_lists.frameTextures.size() >= kFrameTextureSlots)
				stl::report_and_fail(fmt::format("Drawcall Limit Fix: {}'s frame texture {} is one past kFrameTextureSlots: grow it first", a_who, a_texture.name));
			a_lists.frameTextureIndices[r] = static_cast<std::uint8_t>(a_lists.frameTextures.size());
			a_lists.frameTextures.push_back({ std::string(a_texture.name), r, a_texture.view });
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

			void RegisterGpuStage(const Api::GpuStage& a_stage) override
			{
				if (a_stage.passName.empty() || !a_stage.setup || !a_stage.makePass)
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} registered an incomplete GPU stage", feature));
				// BuildDrawsLatch has one stage word, the depth segment's (Api/GpuStages.h).
				if (a_stage.latchWord) {
					if (a_stage.slot != Api::GpuStageSlot::DepthSegmentBeforeBuild)
						stl::report_and_fail(fmt::format("Drawcall Limit Fix: {}'s stage {} asks for a latch word outside the depth segment", feature, a_stage.passName));
					for (const auto& stage : lists.stages)
						if (stage.latchWord && stage.slot == a_stage.slot)
							stl::report_and_fail(fmt::format("Drawcall Limit Fix: {}'s stage {} asks for the latch's stage word, which {} has: grow BuildDrawsLatch first",
								feature, a_stage.passName, stage.passName));
				}
				lists.stages.push_back({ std::string(a_stage.passName), a_stage.slot, a_stage.queue, a_stage.setup, a_stage.makePass, a_stage.latchWord, a_stage.kind });
			}

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
					a_resources.postInputs, lists.sceneResourceInputs, a_resources.inputWords, a_resources.latched });
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
					a_channel.drain });
				return { static_cast<std::uint32_t>(lists.engineChannels.size() - 1) };
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

			void RegisterAdoptionGuard(const Api::AdoptionGuard& a_guard) override
			{
				if (a_guard.name.empty() || !a_guard.generation)
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} registered an incomplete adoption guard", feature));
				// The frame inputs and a publication carry each guard's generation in a slot of their own (AdoptionGenerations).
				if (lists.adoptionGuards.size() >= kAdoptionGuardSlots)
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {}'s adoption guard {} is one past kAdoptionGuardSlots: grow it first", feature, a_guard.name));
				lists.adoptionGuards.push_back({ std::string(a_guard.name), a_guard.generation });
			}

			std::uint32_t ReserveHiddenSite() override { return HiddenSourceSites() + lists.reservedHiddenSites++; }
			void RegisterNodeCapture(const Api::NodeCaptureContribution& a_capture) override { AddNodeCapture(feature, a_capture); }

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
				lists.tableFamilies.push_back({ std::string(a_family.name), a_family.make, a_family.assign, a_family.same, a_family.clear, a_family.recycle, a_family.rowsEnd,
					a_family.recordWord });
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
				lists.coordinatorComponents.push_back({ std::string(a_component.name), a_component.make, a_component.residency, a_component.accumulate, a_component.walk,
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
				entry = { a_technique.toggle, a_technique.objectFlags };
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

			void RegisterGeometryType(const Api::GeometryTypeContribution& a_type) override
			{
				// A toggle, but for a contribution that names an entry class alone; an entry class with the parent kinds it is under.
				const auto& entry = a_type.entry;
				const bool entryValid = entry.entryClass < Api::EntryClass::Count &&
				                        (entry.entryClass == Api::EntryClass::None ? !entry.parentKind && !entry.perFrame && !entry.holdsParent : entry.parentKind != 0);
				const bool classValid = a_type.toggle ||
				                        (!a_type.propertyFlags && a_type.itemClass == Api::FrameValueItemClass::None && !a_type.moves && entry.entryClass != Api::EntryClass::None);
				if (a_type.geometryType >= kGeometryTypeSlots || a_type.itemClass >= Api::FrameValueItemClass::Count || !entryValid || !classValid)
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} registered an incomplete geometry type contribution", feature));
				auto& contribution = lists.geometryTypeContributions[a_type.geometryType];
				if (contribution.geometryType != 0xFF)
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} contributes geometry type {}, which another feature has", feature, static_cast<std::uint32_t>(a_type.geometryType)));
				contribution = a_type;
				lists.geometryTypes[a_type.geometryType] = a_type.toggle;
				// One rule an entry class: a second type naming it names it alike.
				if (entry.entryClass != Api::EntryClass::None) {
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

			void RegisterShading(const Api::ShadingContribution& a_shading) override
			{
				if (a_shading.name.empty() || !a_shading.propertyFlags || !a_shading.derive)
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} registered an incomplete shading contribution", feature));
				lists.shadingContributions.push_back({ std::string(a_shading.name), a_shading.propertyFlags, a_shading.derive });
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
				// Its callbacks: the coverage always, the root for a CategoryRoot view, the accumulators for an InMainRenderers one. Its phase
				// and view bit are checked against Core's and the other views' once all are in (ResolveViews).
				if (a_view.view >= Api::EngineView::Count || !a_view.toggle || !a_view.phase || !a_view.shape.viewBits || !a_view.covered ||
					(a_view.membership == Api::ViewMembership::CategoryRoot && !a_view.captureRoot) ||
					(a_view.withholding == Api::ViewWithholding::InMainRenderers && !a_view.accumulators))
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
				// A local light source is told of each light the selection keeps.
				const bool local = a_source.source == Api::ShadowSource::Spot || a_source.source == Api::ShadowSource::Point;
				if (a_source.source >= Api::ShadowSource::Count || (local && !a_source.lightKept))
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} registered an incomplete shadow source contribution", feature));
				auto& entry = lists.shadowSources[static_cast<std::size_t>(a_source.source)];
				if (entry.source != Api::ShadowSource::Count)
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} contributes shadow source {}, which another feature has", feature,
						static_cast<std::uint32_t>(a_source.source)));
				entry = a_source;
			}

			void RegisterDrawFamily(const Api::DrawFamilyContribution& a_family) override
			{
				// Its class's toggle gates it, its scene resources (registered before) are its tables; its frame and main callbacks always,
				// its faces' all or none.
				const auto& main = a_family.main;
				const auto& faces = a_family.faces;
				const int facesSet = (faces.makeCull ? 1 : 0) + (faces.declare ? 1 : 0) + (faces.record ? 1 : 0) + (faces.row ? 1 : 0) + (faces.lists ? 1 : 0);
				if (a_family.family >= Api::DrawFamily::Count || a_family.objectClass >= Api::ObjectClass::Count ||
					!lists.objectClasses[static_cast<std::size_t>(a_family.objectClass)] || a_family.resources.index >= lists.sceneResources.size() ||
					!a_family.frame.decide || !a_family.frame.programSource || !main.declare || !main.revision || !main.record || (facesSet != 0 && facesSet != 5))
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} registered an incomplete draw family contribution", feature));
				auto& entry = lists.drawFamilies[static_cast<std::size_t>(a_family.family)];
				if (entry.family != Api::DrawFamily::Count)
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} contributes draw family {}, which another registration has", feature,
						static_cast<std::uint32_t>(a_family.family)));
				entry = a_family;
			}

			void RegisterMainRange(const Api::MainRangeContribution& a_range) override { AddMainRange(lists, feature, a_range, false); }

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

			std::string_view feature;  // the feature installing

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

	void InstallFeatures(std::span<const Api::Feature> a_features)
	{
		auto& lists = Registered();
		if (std::exchange(lists.installed, true))
			stl::report_and_fail("Drawcall Limit Fix: the features were installed twice");
		// Core's own main ranges first: a feature's on their slots or orders is refused.
		for (const auto& range : CoreMainRanges())
			AddMainRange(lists, "Core", range, true);
		// Lighting's space and rules as kind 0; the features' material ports take kinds from 1 in their registration order, which the records
		// they write name (RegisterMaterialPort returns it).
		lists.materialPorts[lists.materialPortCount++] = CoreLightingPort();
		Context context(lists);
		for (const auto& feature : a_features) {
			context.feature = feature.name;
			if (feature.install)
				feature.install(context);
		}
		// The features' switches' anchors, which may name a switch a later feature registered.
		CheckSwitchAnchors();
		// The views' entries, from Core's slots and the features' contributions and work; the occlusion maps' and shadow sources', from
		// the features' contributions; then both families' phases and view bits; the draw families' from their registrations; the main
		// ranges' draw order; the vertex stream's (and the scene resources' order: its first); the palette rows classes'; the material ports'
		// (their engine addresses and spaces); the program families' (their ports' kinds); the frame constant rows, in row order.
		ResolveViews(lists.views, lists.viewWork);
		ResolveShadowViews(lists.occlusionMaps, lists.shadowSources);
		ResolveViewBits();
		ResolveDrawFamilies(lists.drawFamilies);
		ResolveMainRanges(lists.mainRanges);
		ResolveVertexStream(lists.vertexStream);
		ResolvePaletteRows(lists.paletteRows);
		ResolveMaterialPorts(lists.materialPorts, lists.materialPortCount);
		ResolveProgramFamilies(lists.programFamilies);
		std::ranges::sort(lists.frameConstantRows, {}, &FrameConstantRowEntry::row);
		// The features' toggles seeded from their switches, into the requested and active sets.
		Toggles::Get().SeedRegistered();
	}

	std::span<const FrameValuePoolEntry> RegisteredPools() { return Registered().pools; }
	std::span<const GpuStageEntry> RegisteredStages() { return Registered().stages; }
	std::span<const PayloadProductEntry> RegisteredProducts() { return Registered().products; }
	std::span<const SceneResourcesEntry> RegisteredSceneResources() { return Registered().sceneResources; }
	std::span<const CommitHookEntry> RegisteredCommitHooks() { return Registered().commitHooks; }
	std::span<const FrameBindingEntry> RegisteredFrameBindings() { return Registered().frameBindings; }
	std::span<const FrameConstantRowEntry> RegisteredFrameConstantRows() { return Registered().frameConstantRows; }
	std::span<const FrameTextureEntry> RegisteredFrameTextures() { return Registered().frameTextures; }
	std::span<const std::uint8_t> FrameTextureIndices() { return Registered().frameTextureIndices; }
	std::span<const ReportLinesEntry> RegisteredReportLines() { return Registered().reportLines; }
	std::span<const Api::StandInObserver> RegisteredStandInObservers() { return Registered().standInObservers; }
	std::span<const EngineChannelEntry> RegisteredEngineChannels() { return Registered().engineChannels; }
	std::span<const AdoptionGuardEntry> RegisteredAdoptionGuards() { return Registered().adoptionGuards; }
	const Api::ShowVeto& RegisteredShowVeto() { return Registered().showVeto; }
	bool ShowVetoRegistered() { return Registered().showVetoRegistered; }
	std::uint32_t ReservedHiddenSites() { return Registered().reservedHiddenSites; }
	std::span<const TableFamilyEntry> RegisteredTableFamilies() { return Registered().tableFamilies; }
	std::span<const CoordinatorComponentEntry> RegisteredCoordinatorComponents() { return Registered().coordinatorComponents; }
	const TechniqueEntries& RegisteredTechniques() { return Registered().techniques; }
	const GeometryTypeToggles& RegisteredGeometryTypes() { return Registered().geometryTypes; }
	const GeometryTypeContributions& RegisteredGeometryTypeContributions() { return Registered().geometryTypeContributions; }
	const EntryClassRules& RegisteredEntryClassRules() { return Registered().entryClassRules; }
	std::span<const PropertyFlagRule> PropertyFlagRules() { return std::span(Registered().propertyFlagRules).first(Registered().propertyFlagRuleCount); }
	const CategoryRoots& RegisteredCategoryRoots() { return Registered().categoryRoots; }
	const ParentToggles& RegisteredParents() { return Registered().parents; }
	const ObjectClassToggles& RegisteredObjectClasses() { return Registered().objectClasses; }
	std::span<const ShadingContributionEntry> RegisteredShadingContributions() { return Registered().shadingContributions; }
	std::span<const FrameGlobalsEntry> RegisteredFrameGlobals() { return Registered().frameGlobals; }

	bool EntryClassTaken(Api::EntryClass a_class)
	{
		return a_class < Api::EntryClass::Count && Registered().entryClassesTaken[static_cast<std::size_t>(a_class)];
	}
}
