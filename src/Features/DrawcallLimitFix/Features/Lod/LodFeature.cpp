// LOD: object and terrain LOD under TES::lodLandRoot, DCLF's while their toggles are on. Their switches and toggles, the two object
// classes' contributions, the LOD gates (their engine hooks, channel, show veto, adoption guard, component, report lines, observers and
// parity: LodGates.h, LodGateComponent.h), and object LOD's segment events (their writers' hooks at the early hooks, their engine channel
// and its component, report line and parity: LodSegmentEvents.h).

#include "Features/DrawcallLimitFix/Features/Lod/Lod.h"
#include "Features/DrawcallLimitFix/Features/Lod/LodGateComponent.h"
#include "Features/DrawcallLimitFix/Features/Lod/LodSegmentEvents.h"
#include "Features/DrawcallLimitFix/Features/Lod/LodSegments.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ObjectContributors.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Registry.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Views.h"
#include "Features/DrawcallLimitFix/Diagnostics/Observation.h"

#include "ShaderCache.h"

#include <cstdint>
#include <vector>

namespace DCLF::Features::Lod
{
	namespace
	{
		using Technique = SIE::ShaderCache::LightingShaderTechniques;
		using Flag = RE::BSShaderProperty::EShaderPropertyFlag;

		// The Lighting property flags that mark object LOD (a BSSubIndexTriShape: dclf-architecture.md, "Object LOD") and terrain LOD (a land block's BSTriShape).
		constexpr std::uint64_t kObjectLodFlags = static_cast<std::uint64_t>(Flag::kLODObjects) | static_cast<std::uint64_t>(Flag::kHDLODObjects);
		constexpr std::uint64_t kTerrainLodFlags = static_cast<std::uint64_t>(Flag::kLODLandscape);
		constexpr auto kSubIndexTriShape = static_cast<std::uint8_t>(RE::BSGeometry::Type::kSubIndexTriShape);
		constexpr auto kTriShape = static_cast<std::uint8_t>(RE::BSGeometry::Type::kTriShape);

		/**
		 * @brief Both classes' category root (the category capture, render thread): TES::lodLandRoot holds the terrain manager's LOD blocks
		 * (a BSMultiBoundNode each: object LOD's BSSubIndexTriShapes, one segment per cell, and the land's BSTriShapes), attached and detached
		 * as the camera moves (dclf-architecture.md, "LOD"). It is the category node; its content is tracked by the usual structural events. A class not
		 * taken, or whose toggle is off, classifies as Lod.
		 */
		RE::NiNode* LodLandRoot()
		{
			const auto* tes = RE::TES::GetSingleton();
			return tes ? tes->lodLandRoot : nullptr;
		}

		/**
		 * @brief An object LOD shape's drawn ranges (its geometry type contribution's ranges: the capturing thread, inside the engine-read
		 * window, per record capture): what the engine's sub-index draw draws now (LodSegments::DrawnRanges).
		 */
		void CaptureDrawnRanges(const void* a_geometry, std::vector<Api::EntryRange>& a_out)
		{
			LodSegments::DrawnRanges(a_geometry, a_out);
		}

		/**
		 * @brief A covered view's LOD land and objects roots are its faces' (the view root contribution) while the engine's LOD swaps are
		 * gated (a show veto registered: the LOD gates') and both classes are DCLF's: a swap's incoming blocks are shown only by the frame
		 * whose claims hold them, so the faces never need the engine's registrations of them. Ungated, the LOD chunks and objects the engine
		 * shows or attaches in motion take DCLF a few frames to bind (ClassifyResidue) and their roots stay, the members withheld
		 * (PassCapture).
		 */
		bool LodRootsOwned(const Api::ViewRootFrame& a_frame)
		{
			return Extensions::ShowVetoRegistered() && Extensions::ObjectClassOn(a_frame.toggles, Api::ObjectClass::LodObjects) &&
			       Extensions::ObjectClassOn(a_frame.toggles, Api::ObjectClass::LodTerrain);
		}

		void Install(Api::InstallContext& a_context)
		{
			// Where Core's were listed: the switches before the LOD gates' (Features/TreeLod's follow them there), the toggles after the
			// refraction's, behind Features/Particles', Features/Billboards' and Features/SwitchNodes' (which register before this feature).
			const auto objectsSwitch = a_context.RegisterSwitch({ .name = "CS_DCLF_LOD_OBJECTS", .kind = Api::SwitchKind::Feature, .reduced = Api::SwitchReduced::UnlessOne,
				.description = "0: object LOD stays native and untracked (live toggle's seed)", .before = "CS_DCLF_LOD_GATES" });
			const auto terrainSwitch = a_context.RegisterSwitch({ .name = "CS_DCLF_LOD_TERRAIN", .kind = Api::SwitchKind::Feature, .reduced = Api::SwitchReduced::UnlessOne,
				.description = "0: terrain LOD stays native (live toggle's seed)", .before = "CS_DCLF_LOD_GATES" });
			// The LOD gates', where Core's was listed: before the shadows' (after the switch nodes', listed after the projected UVs'), behind
			// this feature's two and Features/TreeLod's, which name it.
			const auto gatesSwitch = a_context.RegisterSwitch({ .name = "CS_DCLF_LOD_GATES", .kind = Api::SwitchKind::Feature, .reduced = Api::SwitchReduced::Zero,
				.description = "0: the engine's LOD swaps ungated (T6b5: by default an object or terrain LOD swap keeps the outgoing level shown, and the incoming hidden, until DCLF's claims cover the incoming)",
				.before = "CS_DCLF_SHADOWS" });
			const auto objects = a_context.RegisterToggle({ .name = "object LOD", .label = "Object LOD: distant objects", .seed = objectsSwitch,
				.entersClassification = true, .after = "refraction" });
			const auto terrain = a_context.RegisterToggle({ .name = "terrain LOD", .label = "Terrain LOD: distant land", .seed = terrainSwitch,
				.entersClassification = true, .after = "refraction" });
			// Object LOD (the sub-index shapes' eligibility, the LOD object techniques and flags) and terrain LOD (the LOD land techniques and
			// flags), each an object class; either lists the LOD root as a category node, and both (with the swaps gated) let a view's faces
			// draw the LOD land and objects roots in the engine's place.
			a_context.RegisterObjectClass({ .objectClass = Api::ObjectClass::LodObjects, .toggle = objects, .categoryRoot = &LodLandRoot });
			a_context.RegisterObjectClass({ .objectClass = Api::ObjectClass::LodTerrain, .toggle = terrain, .categoryRoot = &LodLandRoot });
			a_context.RegisterViewRoot({ .root = kOwnedRoot, .owned = &LodRootsOwned });
			// Object LOD, drawn by visible segment ranges: its techniques, only for a BSSubIndexTriShape with the LOD object flags; and the
			// sub-index shapes with those flags, drawn as tri-shapes over the ranges LodSegments mirrors (Ineligible::Lod while the toggle is
			// off; the ranges captured whatever it). Any other sub-index shape is not one DCLF draws.
			for (const auto technique : { Technique::LODObjects, Technique::LODObjectHD })
				a_context.RegisterTechnique({ .technique = static_cast<std::uint32_t>(technique), .toggle = objects, .propertyFlags = kObjectLodFlags,
					.geometryType = kSubIndexTriShape });
			a_context.RegisterGeometryType({ .geometryType = kSubIndexTriShape, .toggle = objects, .propertyFlags = kObjectLodFlags,
				.ranges = &CaptureDrawnRanges });
			// Terrain LOD: its techniques, only for a land block's shape under the LOD root (a BSTriShape with the LOD landscape flag).
			for (const auto technique : { Technique::LODLand, Technique::LODLandNoise })
				a_context.RegisterTechnique({ .technique = static_cast<std::uint32_t>(technique), .toggle = terrain, .propertyFlags = kTerrainLodFlags,
					.geometryType = kTriShape });
			// The LOD gates (CS_DCLF_LOD_GATES): their engine hooks, then, while installed, their registrations, first of the feature's (where
			// Core registered them, before every feature: their channel and adoption guard the first, their hidden site the first reserved,
			// their component before the segments' and every later feature's; no earlier feature's component shares a hook with theirs).
			Gates::RegisterGates(a_context, gatesSwitch);
			// Object LOD's segment events: the channel its writers' hooks feed, its component (the shapes' drawn ranges kept up to date)
			// and its report line; CS_DCLF_PERSISTENT_PARITY: the kept ranges against the live ones.
			RegisterSegmentChannel(a_context);
			// The gates' observers (CS_DCLF_SET_PARITY or CS_DCLF_PERSISTENT_PARITY: the flips against the claims) and parity
			// (CS_DCLF_PERSISTENT_PARITY: their state after each commit, after the commit parity), whether the gates are installed or not.
			if (Diagnostics::Observe().claimsOrScene)
				Gates::SubscribeGateObservers();
			if (Diagnostics::Observe().scene) {
				Gates::SubscribeGateParity();
				SubscribeSegmentParity();
			}
		}
	}

	Api::Feature Feature()
	{
		// The early hooks: the segment writers, the channel's source (where Core's scene events installed them: after the billboards'
		// early hooks, before tree LOD's and the hidden nodes').
		return { "lod", &Install, &InstallSegmentHooks };
	}
}
