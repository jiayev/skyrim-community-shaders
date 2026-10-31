// Tree wind's scene resources (the tree rows, clocks, frame row and wind rows, made with the scene's tables), the commits' uploads into
// them (the commit hook), and the wind rows the frame's draws read at t124 (the frame binding).

#include "Features/DrawcallLimitFix/Features/TreeWind/TreeWindScene.h"
#include "Features/DrawcallLimitFix/Features/TreeWind/TreeWindTables.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Commits.h"
#include "Features/DrawcallLimitFix/Core/Extensions/FrameBindings.h"
#include "Features/DrawcallLimitFix/Core/Extensions/PassContext.h"
#include "Features/DrawcallLimitFix/Core/Extensions/SceneResources.h"
#include "Features/DrawcallLimitFix/Core/Render/Payload/Uploads.h"
#include "Features/DrawcallLimitFix/Core/Render/Payload/Versioned.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/SceneBufferSet.h"
#include "Features/DrawcallLimitFix/Diagnostics/Policy.h"

#include <algorithm>
#include <cstdint>
#include <memory>

namespace DCLF::Features::TreeWind
{
	namespace
	{
		Api::SceneResourcesId sceneId;  // install

		// Data globals SetupGeometry's TreeAnim case reads, resolved as addresses and read through.
		REL::Relocation<std::uintptr_t> treeWindFadeStart{ REL::Offset(Address::kTreeWindFadeStart) };
		REL::Relocation<std::uintptr_t> treeWindFadeEnd{ REL::Offset(Address::kTreeWindFadeEnd) };
		REL::Relocation<std::uintptr_t> treeWindTimerScale{ REL::Offset(Address::kTreeWindTimerScale) };
		// A pointer to the object whose +0x304 holds the global wind magnitude.
		REL::Relocation<std::uintptr_t> treeWindSource{ REL::Offset(Engine::Address::kShadowSceneNode) };
		// BSTreeManager* (+0x78 the gust's speed, +0x80 the squared range of its update), and the frame's seconds it advances the
		// clocks by (FUN_140437d40's argument; GetSecondsPassed reads it).
		REL::Relocation<std::uintptr_t> treeManager{ REL::Offset(Address::kTreeManager) };
		REL::Relocation<std::uintptr_t> frameSeconds{ REL::Offset(Address::kFrameSeconds) };

		float GlobalFloat(const REL::Relocation<std::uintptr_t>& a_at)
		{
			return *reinterpret_cast<const float*>(a_at.address());
		}

		/** @brief The frame's tree clock inputs (render thread, any time after Main::Update). */
		TreeWindFrame SampleTreeWindFrame()
		{
			TreeWindFrame frame;
			frame.deltaTime = GlobalFloat(frameSeconds);
			if (auto* camera = RE::Main::WorldRootCamera()) {
				frame.camera[0] = camera->world.translate.x;
				frame.camera[1] = camera->world.translate.y;
				frame.camera[2] = camera->world.translate.z;
			}
			if (const auto manager = *reinterpret_cast<const std::uintptr_t*>(treeManager.address())) {
				frame.windSpeed = *reinterpret_cast<const float*>(manager + 0x78);
				frame.maxDistance2 = *reinterpret_cast<const float*>(manager + 0x80);
			}
			frame.windMagnitude = *reinterpret_cast<const float*>(*reinterpret_cast<const std::uintptr_t*>(treeWindSource.address()) + 0x304);
			frame.fadeStart = GlobalFloat(treeWindFadeStart);
			frame.fadeEnd = GlobalFloat(treeWindFadeEnd);
			frame.timerScale = GlobalFloat(treeWindTimerScale);
			return frame;
		}

		// The scene's tables are made (the first frame with a device): the program and the buffers, after the face positions.
		std::shared_ptr<void> Setup(const Api::SceneResourcesSetup& a_setup)
		{
			using org::services::GrowableContents;
			auto scene = std::make_shared<TreeWindScene>();
			// Without the tree wind program, trees keep the wind they joined with (the wind buffers, t124, exist and stay zero).
			scene->treeWind = ComputeProgram::Load(a_setup.device, { .source = kShader, .constantWords = kTreeWindConstantWords });
			std::uint32_t trees = 0;
			if (scene->treeWind) {
				trees = a_setup.smallStart ? 4u : kInitialTrees;
				scene->trees = Draws::MakeVersioned(a_setup.StructuredBuffer(trees, sizeof(TreeStatic), "cs.dclf.trees"), { .extent = { 0, sizeof(TreeStatic), sizeof(TreeStatic) } });
				// New clocks restart from each tree's static row.
				scene->treeClocks = Draws::MakeVersioned(a_setup.StructuredBuffer(trees, sizeof(TreeClock), "cs.dclf.tree-clocks", true),
					{ .extent = { 0, sizeof(TreeClock), sizeof(TreeClock) } });
				scene->treeFrameBuffer = a_setup.StructuredBuffer(1, sizeof(TreeWindFrameRow), "cs.dclf.tree-frame");
			} else {
				logger::warn("[DCLF] The tree wind program could not be created; trees keep the wind they joined with");
			}
			// The nodeless trees' entry, then a unit per tree slot; every version zero-filled (a zero generation is never a listing's).
			for (std::uint32_t h = 0; h < 2; ++h)
				scene->treeWindRows[h] = Draws::MakeVersioned(a_setup.StructuredBuffer((trees + 1) * kTreeWindEntryRows, 16, h ? "cs.dclf.tree-wind1" : "cs.dclf.tree-wind0", true),
					{ .extent = { kTreeWindEntryRows * 16, kTreeWindEntryRows * 16, 16 }, .contents = GrowableContents::Zero });
			return scene;
		}

		void RegisterGraph(const Api::SceneResourcesGraph& a_graph)
		{
			const auto& scene = a_graph.State<TreeWindScene>();
			if (!scene.trees)
				return;
			Draws::Register(a_graph.graph, org::ResourceIdentifier("cs.dclf.trees"), scene.trees);
			Draws::Register(a_graph.graph, org::ResourceIdentifier("cs.dclf.tree-clocks"), scene.treeClocks);
			Draws::Register(a_graph.graph, org::ResourceIdentifier("cs.dclf.tree-wind0"), scene.treeWindRows[0]);
			Draws::Register(a_graph.graph, org::ResourceIdentifier("cs.dclf.tree-wind1"), scene.treeWindRows[1]);
			Draws::Register(a_graph.graph, org::ResourceIdentifier("cs.dclf.tree-frame"), scene.treeFrameBuffer);
		}

		// Tree rows, clocks and wind by tree slot.
		void Require(const Api::SceneResourcesRequirements& a_requirements)
		{
			const auto& s = a_requirements.State<TreeWindScene>();
			if (s.trees)
				Draws::UnitGroup{}.Add(s.trees).Add(s.treeClocks).Add(s.treeWindRows[0]).Add(s.treeWindRows[1]).Require(a_requirements.build, TreesOf(a_requirements.tables).trees.size());
		}

		// Without the tree wind program no draw reads the wind rows by its tree slot (the records keep their own wind).
		void Fit(Api::SceneResourcesFit& a_fit)
		{
			const auto& scene = a_fit.State<TreeWindScene>();
			const auto units = [&](const Draws::Versioned& a_buffer) {
				return static_cast<std::uint32_t>(std::min<std::uint64_t>(Draws::NamedUnits(a_fit.sealed, a_buffer), UINT32_MAX));
			};
			if (scene.treeWind && scene.trees)
				a_fit.fit.familyRows[TreeFamilyId().index] = std::min({ units(scene.trees), units(scene.treeWindRows[0]), units(scene.treeWindRows[1]) });
		}

		/**
		 * @brief A commit's tree uploads into the versions a_sealed (the adopted set) names. a_frameRow: also the frame row (only the
		 * Z-prepass's TreeWindPass reads it).
		 */
		void UploadTrees(const Draws::NamedVersions& a_sealed, const TreeTables& a_tables, std::uint32_t a_frame, TreeWindScene& a_scene, Draws::CommitUploads& a_uploads,
			bool a_frameRow)
		{
			if (!a_scene.treeWind || !a_scene.trees)
				return;
			const auto& trees = a_sealed.Of(a_scene.trees);
			if (a_scene.treesFilled.Since(trees) != a_tables.treesContentEpoch && a_tables.trees.size() <= trees.units) {
				if (!a_tables.trees.empty())
					a_uploads(a_scene.trees, a_tables.trees.data(), a_tables.trees.size() * sizeof(TreeStatic), 0);
				a_scene.treesFilled.Filled(trees, a_tables.treesContentEpoch);
			}
			a_scene.frameTreesContentEpoch = a_scene.TreesContentEpoch(a_sealed);
			a_scene.treeCount = a_scene.frameTreesContentEpoch == a_tables.treesContentEpoch ? static_cast<std::uint32_t>(a_tables.trees.size()) : 0u;
			if (a_scene.treeFrame != a_frame) {
				a_scene.previousTreeFrame = a_scene.treeFrame;
				a_scene.previousTreeInputs = a_scene.treeInputs;
				a_scene.previousTreesContentEpoch = a_scene.frameTreesContentEpoch;
				a_scene.treeFrame = a_frame;
				a_scene.treeInputs = SampleTreeWindFrame();
				if (Policy::FreezeTreeWind())
					a_scene.treeInputs.deltaTime = 0.0f;
			}
			// Every Z-prepass commit: the pass's invocation is prepared ahead of it.
			if (a_frameRow && a_scene.treeFrameBuffer) {
				const TreeWindFrameRow row{ a_scene.treeCount, 0, a_scene.treeFrame, SeedsIndex(), a_scene.treeInputs, {} };
				a_uploads(a_scene.treeFrameBuffer, &row, sizeof(row), 0);
			}
		}

		// Every commit (depth, colour, shadow) with the scene's streams; only the depth commit's graph runs the pass that reads the frame row.
		void Commit(const Api::CommitContext& a_commit)
		{
			if (auto* scene = SceneOf(a_commit.scene))
				UploadTrees(a_commit.adopted, TreesOf(a_commit.tables), a_commit.frame, *scene, a_commit.uploads, a_commit.kind == Api::CommitKind::Depth);
		}

		// t124: the wind buffer a frame's draws read (the one the previous frame wrote).
		void SelectWind(Api::FrameBindingSelect& a_select)
		{
			if (const auto* scene = SceneOf(a_select.scene))
				a_select.buffer = &scene->WindReadBuffer(a_select.frame);
		}
	}

	TreeWindScene* SceneOf(const Draws::SceneBuffers& a_scene)
	{
		return Extensions::SceneResourcesOf<TreeWindScene>(a_scene, sceneId);
	}

	std::shared_ptr<TreeWindScene> SharedSceneOf(const Draws::SceneBuffers& a_scene)
	{
		return Extensions::SharedSceneResourcesOf<TreeWindScene>(a_scene, sceneId);
	}

	void RegisterScene(Api::InstallContext& a_context)
	{
		sceneId = a_context.RegisterSceneResources({ .name = "tree-wind", .setup = &Setup, .registerGraph = &RegisterGraph, .require = &Require, .fit = &Fit });
		a_context.RegisterCommitHook({ .name = "tree-wind", .commit = &Commit });
		// Named as the named-address parity names it.
		a_context.RegisterFrameBinding({ .name = "tree wind SRV", .textureRegister = kWindRegister, .select = &SelectWind });
	}
}
