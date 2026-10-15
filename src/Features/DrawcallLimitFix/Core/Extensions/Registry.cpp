// The extension registry (see the header): the install context the features register with, and the lists it fills.

#include "Features/DrawcallLimitFix/Core/Extensions/Registry.h"
#include "Features/DrawcallLimitFix/Core/Extensions/CoordinatorComponents.h"
#include "Features/DrawcallLimitFix/Core/Extensions/EngineEvents.h"
#include "Features/DrawcallLimitFix/Core/Extensions/FrameBindings.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ObjectContributors.h"
#include "Features/DrawcallLimitFix/Core/Extensions/TableFamilies.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Views.h"
#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"

#include <array>
#include <vector>

namespace DCLF::Extensions
{
	namespace
	{
		// Written only during InstallFeatures (the main thread, before DCLF's threads exist: their start orders these writes before
		// every read), read-only after.
		struct Lists
		{
			std::vector<FrameValuePoolEntry> pools;
			std::vector<GpuStageEntry> stages;
			std::vector<PayloadProductEntry> products;
			std::vector<SceneResourcesEntry> sceneResources;
			std::vector<CommitHookEntry> commitHooks;
			std::vector<FrameBindingEntry> frameBindings;
			std::vector<ReportLinesEntry> reportLines;
			std::vector<Api::StandInObserver> standInObservers;
			std::vector<TableFamilyEntry> tableFamilies;
			std::vector<CoordinatorComponentEntry> coordinatorComponents;
			// By entry class: whether the component that follows it takes it (EntryClassTaken).
			std::array<bool, static_cast<std::size_t>(Api::EntryClass::Count)> entryClassesTaken{};
			TechniqueEntries techniques{};
			GeometryTypeToggles geometryTypes{};
			ParentToggles parents{};
			ObjectClassToggles objectClasses{};
			std::vector<ShadingContributionEntry> shadingContributions;
			std::vector<FrameGlobalsEntry> frameGlobals;
			std::uint32_t frameGlobalsFloats = 0;
			// By view: its contribution (EngineView::Count: none); the views' work, in order. Resolved at the end (ResolveViews).
			std::array<Api::ViewContribution, kEngineViews> views{};
			std::vector<Api::ViewWork> viewWork;
			bool installed = false;
		};

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
				lists.stages.push_back({ std::string(a_stage.passName), a_stage.slot, a_stage.queue, a_stage.setup, a_stage.makePass, a_stage.latchWord });
			}

			void RegisterPayloadProduct(const Api::PayloadProduct& a_product) override
			{
				if (a_product.name.empty() || !a_product.build || !a_product.install)
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} registered an incomplete payload product", feature));
				lists.products.push_back({ std::string(a_product.name), a_product.build, a_product.carried, a_product.install });
			}

			Api::SceneResourcesId RegisterSceneResources(const Api::SceneResources& a_resources) override
			{
				if (a_resources.name.empty() || !a_resources.setup)
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} registered incomplete scene resources", feature));
				lists.sceneResources.push_back({ std::string(a_resources.name), a_resources.setup, a_resources.registerGraph, a_resources.require, a_resources.fit });
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
				const bool text = a_lines.point == Api::ReportPoint::AsyncWorkViews || a_lines.point == Api::ReportPoint::AsyncWork;
				if (a_lines.name.empty() || (text ? !a_lines.text || a_lines.lines : !a_lines.lines || a_lines.text))
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} registered incomplete report lines", feature));
				lists.reportLines.push_back({ std::string(a_lines.name), a_lines.point, a_lines.lines, a_lines.text });
			}

			bool RegisterStandInObserver(const Api::StandInObserver& a_observer) override
			{
				// Without the primary cull nothing stands in, and nothing would call it.
				if (!StandInInstalled())
					return false;
				lists.standInObservers.push_back(a_observer);
				return true;
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
					a_component.dense, a_component.retirement, followed });
				if (followed.follows != Api::EntryClass::None)
					lists.entryClassesTaken[static_cast<std::size_t>(followed.follows)] = followed.takes;
				return { static_cast<std::uint32_t>(lists.coordinatorComponents.size() - 1) };
			}

			void RegisterTechnique(const Api::TechniqueContribution& a_technique) override
			{
				if (a_technique.technique >= kTechniqueSlots || !a_technique.toggle)
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} registered an incomplete technique contribution", feature));
				auto& entry = lists.techniques[a_technique.technique];
				if (entry.toggle)
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} contributes technique {}, which another feature has", feature, a_technique.technique));
				entry = { a_technique.toggle, a_technique.objectFlags };
			}

			void RegisterGeometryType(const Api::GeometryTypeContribution& a_type) override
			{
				if (a_type.geometryType >= kGeometryTypeSlots || !a_type.toggle)
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} registered an incomplete geometry type contribution", feature));
				auto& toggle = lists.geometryTypes[a_type.geometryType];
				if (toggle)
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} contributes geometry type {}, which another feature has", feature, static_cast<std::uint32_t>(a_type.geometryType)));
				toggle = a_type.toggle;
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
				if (CoreKeepsView(a_view.view))
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} contributes view {}, which Core keeps", feature, static_cast<std::uint32_t>(a_view.view)));
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
		Context context(lists);
		for (const auto& feature : a_features) {
			context.feature = feature.name;
			if (feature.install)
				feature.install(context);
		}
		// The views' entries, from Core's slots and the features' contributions and work.
		ResolveViews(lists.views, lists.viewWork);
		// The features' toggles seeded from their switches, into the requested and active sets.
		Toggles::Get().SeedRegistered();
	}

	std::span<const FrameValuePoolEntry> RegisteredPools() { return Registered().pools; }
	std::span<const GpuStageEntry> RegisteredStages() { return Registered().stages; }
	std::span<const PayloadProductEntry> RegisteredProducts() { return Registered().products; }
	std::span<const SceneResourcesEntry> RegisteredSceneResources() { return Registered().sceneResources; }
	std::span<const CommitHookEntry> RegisteredCommitHooks() { return Registered().commitHooks; }
	std::span<const FrameBindingEntry> RegisteredFrameBindings() { return Registered().frameBindings; }
	std::span<const ReportLinesEntry> RegisteredReportLines() { return Registered().reportLines; }
	std::span<const Api::StandInObserver> RegisteredStandInObservers() { return Registered().standInObservers; }
	std::span<const TableFamilyEntry> RegisteredTableFamilies() { return Registered().tableFamilies; }
	std::span<const CoordinatorComponentEntry> RegisteredCoordinatorComponents() { return Registered().coordinatorComponents; }
	const TechniqueEntries& RegisteredTechniques() { return Registered().techniques; }
	const GeometryTypeToggles& RegisteredGeometryTypes() { return Registered().geometryTypes; }
	const ParentToggles& RegisteredParents() { return Registered().parents; }
	const ObjectClassToggles& RegisteredObjectClasses() { return Registered().objectClasses; }
	std::span<const ShadingContributionEntry> RegisteredShadingContributions() { return Registered().shadingContributions; }
	std::span<const FrameGlobalsEntry> RegisteredFrameGlobals() { return Registered().frameGlobals; }

	bool EntryClassTaken(Api::EntryClass a_class)
	{
		return a_class < Api::EntryClass::Count && Registered().entryClassesTaken[static_cast<std::size_t>(a_class)];
	}
}
