#if defined(CS_HAS_RENDER_GRAPH) && defined(CS_HAS_ORG_MODULE_SERVICES)
#	include "DrawsState.h"
#	include "Events.h"
#	include "Observers.h"
#	include "Features/DrawcallLimitFix/Engine/Views/ReflectionFaces.h"

// The first-person skeleton's claims (CS_DCLF_SET_PARITY), and the water reflection's face cameras (CS_DCLF_SET_PARITY or
// CS_DCLF_PERSISTENT_PARITY): the face the engine has just drawn, as its accumulator render left it - the bound colour target's face and
// VS_PerFrame (b12) from the mirror - against DCLF's (ReflectionFaceCamera). Render thread; the reflection report takes the counts.

namespace DCLF::Diagnostics
{
	void NoteFirstPersonSkeleton(const FirstPersonSkeletonWalked& a_walked)
	{
		// Which first-person geometry the first-person view does not take, and what it takes instead.
		const auto& tables = a_walked.tables;
		const auto& skeleton = a_walked.skeleton;
		const auto& claims = a_walked.claims;
		std::string members;
		for (std::size_t o = 0; o < tables.objects.size() && !skeleton.empty(); ++o)
			if (o < tables.objectGeometry.size() && tables.objectGeometry[o] && skeleton.contains(tables.objectGeometry[o]) && !(tables.objects[o].flags & kObjectFree))
				members += fmt::format("{}'{}' {} slot {} flags {:#x} phases {:#x}", members.empty() ? "" : "; ", tables.objectGeometry[o]->name.c_str() ? tables.objectGeometry[o]->name.c_str() : "?", static_cast<const void*>(tables.objectGeometry[o]),
					o, tables.objects[o].flags, claims.PhasesOf(tables.objectGeometry[o]));
		auto& logged = DrawsState::Get().loggedSkeleton;
		if (members != logged) {
			logged = members;
			logger::info("[DCLF] first-person skeleton's slots (T7e, CS_DCLF_SET_PARITY): {} geometries walked; {}", a_walked.walked, members);
		}
	}

	void CheckReflectionFace(const ReflectionFaceDrawn&)
	{
		const auto& reflection = DrawsView::Get().reflection;
		const std::uint32_t f = reflection.orientedFace;
		if (!ReflectionFaces::Plain() || f >= kReflectionFaces || !reflection.faces[f].captured)
			return;
		const auto& face = reflection.faces[f];
		auto& parity = DrawsState::Get().faceCameras;
		++parity.faces;
		// The bound target's face (the array slice it views) and texture.
		ID3D11RenderTargetView* bound = nullptr;
		globals::d3d::context->OMGetRenderTargets(1, &bound, nullptr);
		std::uint32_t targetFace = ~0u;
		winrt::com_ptr<ID3D11Texture2D> texture;
		if (bound) {
			D3D11_RENDER_TARGET_VIEW_DESC desc{};
			bound->GetDesc(&desc);
			if (desc.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2DARRAY && desc.Texture2DArray.ArraySize == 1)
				targetFace = desc.Texture2DArray.FirstArraySlice;
			winrt::com_ptr<ID3D11Resource> resource;
			bound->GetResource(resource.put());
			if (resource)
				resource->QueryInterface(IID_PPV_ARGS(texture.put()));
			bound->Release();
		}
		if (targetFace != f || texture.get() != reflection.cube.get()) {
			if (parity.slices++ == 0 && parity.first.empty())
				parity.first = fmt::format("face {}: the engine drew into face {}", f, targetFace);
		}
		auto* perFrame = *globals::game::perFrame.get();
		const auto contents = perFrame ? ConstantMirror::Get().Contents(perFrame) : std::span<const std::byte>{};
		if (contents.size() < SunViews::kBlockBytes)
			return;
		const auto* theirs = reinterpret_cast<const float*>(contents.data());
		const auto* mine = reinterpret_cast<const float*>(face.perFrame.data());
		// Every register within rounding but the inverses DCLF computes with arithmetic of its own (c20-c23, c32-c39: within 1e-3).
		float largest = 0.0f;
		std::int32_t first = -1;
		for (std::uint32_t i = 0; i < SunViews::kBlockBytes / sizeof(float); ++i) {
			const std::uint32_t c = i / 4;
			const bool loose = (c >= 20 && c < 24) || (c >= 32 && c < 40);
			const float d = std::abs(mine[i] - theirs[i]);
			const bool same = loose ? d <= 1e-3f * std::max(1.0f, std::abs(theirs[i])) : SunViews::Close(mine[i], theirs[i]);
			if (!same) {
				largest = std::max(largest, d);
				parity.registers |= 1ull << c;
				if (first < 0)
					first = static_cast<std::int32_t>(i);
			}
		}
		if (first >= 0) {
			parity.largest = std::max(parity.largest, largest);
			if (parity.blocks++ == 0 && parity.first.empty()) {
				const std::uint32_t row = static_cast<std::uint32_t>(first) / 4 * 4;
				parity.first = fmt::format("face {}: c{} ({} {} {} {}), the engine's ({} {} {} {})", f, row / 4, mine[row], mine[row + 1], mine[row + 2], mine[row + 3], theirs[row],
					theirs[row + 1], theirs[row + 2], theirs[row + 3]);
			}
		}
	}
}

#endif
