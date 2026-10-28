// The CPU-driven pipeline's draws as graph passes (Sites.h): each draw site's epoch ends with a late pass (Api::PassDef::recordAtExecution,
// ORG's RecordAtExecution), which the site's execution records on DXVK's CS thread into DXVK's command buffer, once the site's culls are in
// (SiteFrame.h, RecordExecutingSite). It declares the targets its site draws, so the graph orders, times and names the draws with the
// epoch's other passes; its revision is constant (what it draws is its run's, not the recording's), so a kept recording records it again
// in each execution (PreparedPass::RecordAgain).

// Main/Resources.h first: volk must precede every Vulkan header of the translation unit.
#include "Features/DrawcallLimitFix/Core/Render/Main/Resources.h"
#include "Features/DrawcallLimitFix/Core/Extensions/PassContext.h"
#include "Features/DrawcallLimitFix/Core/Render/Direct/SiteFrame.h"
#include "Features/DrawcallLimitFix/Core/Render/Epochs/ViewEpoch.h"
#include "Features/DrawcallLimitFix/Core/Render/Shadow/ShadowFrame.h"

#include <RenderPasses/Base/TypedRenderGraphPass.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace DCLF::Direct
{
	namespace
	{
		struct SiteBindings
		{
			Site site = Site::Depth;
		};

		/** @brief A draw site's late pass: its targets declared, its recording the executing run's draws. */
		class SitePass final : public org::TypedRenderGraphPass<SitePass, org::EmptyPassFrameData, SiteBindings>
		{
		public:
			using Textures = std::vector<std::shared_ptr<org::ExternalTextureResource>>;

			SitePass(Site a_site, Textures a_colours, Textures a_depths) :
				site(a_site), colours(std::move(a_colours)), depths(std::move(a_depths)) {}

			SiteBindings Declare(org::PassBuilder& a_builder)
			{
				a_builder.PreferQueue(org::QueueKind::Graphics);
				for (const auto& colour : colours)
					(void)a_builder.RenderTarget(colour);
				for (const auto& depth : depths)
					(void)a_builder.DepthReadWrite(depth);
				return { site };
			}

			void InvocationRevision(const org::PassPrepareContext&, std::vector<std::uint64_t>& a_out) const
			{
				a_out.push_back(static_cast<std::uint64_t>(site));
			}

			static void Record(const SiteBindings& a_bindings, org::PassRecordContext& a_recording)
			{
				(void)detail::RecordExecutingSite(a_bindings.site, a_recording.Commands());
			}

		private:
			Site site;
			Textures colours, depths;
		};

		/** @brief A site's pass, or null when it would declare no target (a pass that writes nothing would be culled). */
		std::shared_ptr<org::RenderPass> MakeSitePass(Site a_site, SitePass::Textures a_colours, SitePass::Textures a_depths)
		{
			std::erase(a_colours, nullptr);
			std::erase(a_depths, nullptr);
			if (a_colours.empty() && a_depths.empty())
				return nullptr;
			return std::make_shared<SitePass>(a_site, std::move(a_colours), std::move(a_depths));
		}

		SitePass::Textures MainColours(const Draws::Resources& a_resources)
		{
			return { a_resources.native.begin(), a_resources.native.begin() + a_resources.targetCount };
		}

		// main.depth: main depth, the families' depth and the hosted views' (the reflection's faces into their own targets, first person's
		// depth into the main depth).
		std::shared_ptr<org::RenderPass> DepthSite(const Api::PassContext& a_context)
		{
			const auto& main = *a_context.Main();
			return MakeSitePass(Site::Depth, {}, { main.nativeDepth });
		}

		// main.colour: main colour, the decals, the families' colour, the second pass.
		std::shared_ptr<org::RenderPass> ColourSite(const Api::PassContext& a_context)
		{
			const auto& main = *a_context.Main();
			return MakeSitePass(Site::Colour, MainColours(main), { main.nativeDepth });
		}

		// forward: the forward range's draws and the refraction normals.
		std::shared_ptr<org::RenderPass> ForwardSite(const Api::PassContext& a_context)
		{
			const auto& main = *a_context.Main();
			auto colours = MainColours(main);
			colours.push_back(main.refractionNormals);
			return MakeSitePass(Site::Forward, std::move(colours), { main.nativeDepth });
		}

		// shadow: every shadow view and the occlusion maps, into the imported maps.
		std::shared_ptr<org::RenderPass> ShadowSite(const Api::PassContext& a_context)
		{
			const auto& shadow = *a_context.Shadow();
			return MakeSitePass(Site::Shadow, {}, { shadow.depth.begin(), shadow.depth.end() });
		}

		// fp.colour: first person's colour and forward part, into the main targets against its depth (kPOST_ZPREPASS_COPY).
		std::shared_ptr<org::RenderPass> FirstPersonSite(const Api::PassContext& a_context)
		{
			const auto& view = *a_context.View();
			if (!view.main)
				return nullptr;
			return MakeSitePass(Site::FirstPerson, MainColours(*view.main), { view.target });
		}

		constexpr Api::PassDef kSitePasses[] = {
			{ .key = "core.z.direct", .passName = "cs.dclf.z.direct", .kind = Api::PassKind::Render, .make = &DepthSite, .cpuPipeline = true, .recordAtExecution = true },
			{ .key = "core.direct", .passName = "cs.dclf.direct", .kind = Api::PassKind::Render, .make = &ColourSite, .cpuPipeline = true, .recordAtExecution = true },
			{ .key = "core.forward-direct", .passName = "cs.dclf.forward.direct", .kind = Api::PassKind::Render, .make = &ForwardSite, .cpuPipeline = true, .recordAtExecution = true },
			{ .key = "core.shadow-direct", .passName = "cs.dclf.shadow.direct", .kind = Api::PassKind::Render, .make = &ShadowSite, .cpuPipeline = true, .recordAtExecution = true },
			{ .key = "view.direct", .passName = "{view}.direct", .kind = Api::PassKind::Render, .make = &FirstPersonSite, .cpuPipeline = true, .recordAtExecution = true },
		};
	}

	std::span<const Api::PassDef> SitePassDefs()
	{
		return kSitePasses;
	}
}
