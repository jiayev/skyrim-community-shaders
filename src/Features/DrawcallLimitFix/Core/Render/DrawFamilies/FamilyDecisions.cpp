// The draw families' frame decisions (IndirectDraws::DecideDrawFamilies): each taken family's decide, published as the frame's ownership
// (the depth commit's, a trees view's, PassCapture's), and the pipeline lane its decide asks for its slot's pipelines
// (Api::DrawFamilyFrame::Pipelines). Render thread.

#include "Features/DrawcallLimitFix/Core/Render/Internal.h"

namespace DCLF::Api
{
	bool DrawFamilyFrame::Pipelines(bool a_drawable) const
	{
		// The lane builds the slot's pipelines from the family's program source (TreeInstances': the DistantTree shader); until the
		// frame's catalog has them the engine draws the family.
		auto& drawPipelines = DrawPipelines::Get();
		if (auto* shader = static_cast<RE::BSShader*>(const_cast<void*>(Extensions::DrawFamilyOf(family).contribution.frame.programSource()))) {
			drawPipelines.SetFamilyInputs(family, *shader);  // the lane only reads it
			drawPipelines.RequestFamily(family);
		}
		const auto* catalog = FrameCatalog();
		FamilyPipelines pipelines;
		const bool built = CatalogFamilyPipelines(catalog, family, pipelines);
		const bool owned = built && a_drawable;
		if (owned) {
			auto& published = scene->familyPipelines[static_cast<std::size_t>(family)];
			const auto current = published.load(std::memory_order_acquire);
			if (!current || !Draws::SameHandle(current->depth, pipelines.depth) || !Draws::SameHandle(current->colour, pipelines.colour) ||
				!Draws::SameHandle(current->colour2, pipelines.colour2))
				published.store(std::make_shared<const FamilyPipelines>(pipelines), std::memory_order_release);
		}
		drawPipelines.NoteFamilyWait(family, !built && !(catalog && catalog->families[static_cast<std::size_t>(family)].failed));
		return owned;
	}
}

namespace DCLF
{
	void IndirectDraws::DecideDrawFamilies(std::uint32_t a_frame)
	{
		auto* scene = impl->scene.get();
		const auto toggles = ActiveToggles();
		for (std::size_t f = 0; f < Extensions::kDrawFamilies; ++f) {
			const auto& entry = Extensions::DrawFamilies()[f];
			if (!entry.taken)
				continue;
			const auto family = static_cast<Api::DrawFamily>(f);
			// A frame without claims is the engine's whole, the family's draws included; so is one whose family is not set up or off.
			bool owned = false;
			if (scene && Draws::FamilySetUp(*scene, family) && Extensions::DrawFamilyOn(toggles, family) && !Scene::FrameView::Get().NotDrawingYet())
				owned = entry.contribution.frame.decide({ a_frame, scene, family });
			impl->familyOwned[f] = owned;
			PassCapture::Get().SetFamilyOwned(family, owned);
		}
	}

	bool IndirectDraws::DrawFamilyOwned(Api::DrawFamily a_family) const
	{
		return impl && impl->familyOwned[static_cast<std::size_t>(a_family)];
	}
}
