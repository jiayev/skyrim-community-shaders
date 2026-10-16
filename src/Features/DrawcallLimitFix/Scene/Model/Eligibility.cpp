// The eligibility rules that need more than a header: the cacheable verdicts, a geometry's and a layer's static verdict, and the
// primary-cull rule. Pure functions of the records passed in; called on the scene-work thread and on the render thread.

#include "Features/DrawcallLimitFix/Scene/Model/Eligibility.h"

#include "Features/DrawcallLimitFix/Core/Extensions/CoordinatorComponents.h"
#include "Features/DrawcallLimitFix/Core/Extensions/MaterialPorts.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ObjectContributors.h"
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"

namespace DCLF::Eligibility
{
	bool Cacheable(Ineligible a_reason)
	{
		// Only the verdicts that follow from the geometry's own shape - its type, its skin instance, its
		// renderer data and the *type* of its shader property. Those are exactly what the pointer
		// witnesses cover.
		//
		// Everything decided later is deliberately left out, because its inputs can change without any
		// witness noticing. AlphaBlend reads `materialAlpha`, which is animated and lives behind an
		// unchanged material pointer; the reasons DeriveLightingDescriptors produces (Decal, Technique,
		// ProjectedUV, Lod, Fading) read property *flags*, which Community Shaders' own features set in
		// place behind an unchanged property pointer. Caching those would trade a real correctness
		// surface for the cheapest third of the population.
		switch (a_reason) {
		case Ineligible::NotTriShape:
		case Ineligible::Skinned:
		case Ineligible::NoRendererData:
		case Ineligible::NotLightingShader:
			return true;
		default:
			return false;
		}
	}

	Ineligible Static(const SceneCapture::LeafView& a_leaf, LightingDescriptors* a_descriptors, const AccumulatedPass* a_accumulated)
	{
		if (!a_leaf)
			return Ineligible::NotTriShape;
		const auto& g = *a_leaf.geometry;
		// An entry of a taken entry class (its geometry type's rule, Api::GeometryTypeContribution::entry: Features/Faces' face shape, a
		// dynamic shape under a BSFaceGenNiNode) takes the checks below like any shape. Its positions are not in its buffers but its
		// component's (a face shape's head's snapshots): the walk gives every record of one its stream (Coordinator::Tracked::entryClass),
		// whatever the verdict, and the draws bind it as the second stream.
		const auto type = static_cast<RE::BSGeometry::Type>(g.type);
		const auto entryClass = a_leaf.parent ? Extensions::EntryClassOf(g.type, a_leaf.parent->kind) : Api::EntryClass::None;
		const bool entryShape = entryClass != Api::EntryClass::None && Extensions::EntryClassTaken(entryClass);
		// A BSMultiIndexTriShape (cave walls, rocks with a snow layer) draws every pass but one as a tri-shape: its renderer data and
		// its triangle count (FUN_1414f2ad0, type 7). The exception is the additional property's main-pass layer (accumulation
		// hint 12), drawn from its second index list: DCLF's layer object (Tracked::layerSlot, Layer). The shadow modes
		// register the main property's passes alone (FUN_1414b2a60), so it casts as a tri-shape. One whose layer DCLF cannot
		// draw is MultiIndex, a shadow-only caster (ShadowOnlyReason): its two main passes stay the engine's, together.
		const bool multiIndex = type == RE::BSGeometry::Type::kMultiIndexTriShape;
		// A geometry type a contribution takes by its Lighting property's flags (Api::GeometryTypeContribution::propertyFlags: Features/Lod's
		// object LOD, a BSSubIndexTriShape with the LOD object flags, drawn by the segment runs the engine leaves visible, FUN_1414f2ad0
		// type 8): a tri-shape over the ranges LodSegments mirrors, Lod while its toggle is off. Any other shape of the type is not one DCLF
		// draws.
		bool flaggedShape = false;
		if (const auto& flagged = Extensions::GeometryTypeContributionOf(g.type); flagged.propertyFlags) {
			const auto* flaggedProperty = SceneCapture::LeafView::Lighting(a_leaf.property);
			if (flaggedProperty && (flaggedProperty->flags & flagged.propertyFlags)) {
				if (!ActiveToggles().On(flagged.toggle))
					return Ineligible::Lod;
				flaggedShape = true;
			}
		}
		if (type != RE::BSGeometry::Type::kTriShape && !multiIndex && !flaggedShape) {
			// Its geometry type's contribution (Features/Particles): a particle geometry with a material port's property is that port's to
			// classify when it takes the particle geometries (the Effect port's DeriveEffectDescriptors: a particle system DCLF's particle path
			// draws, or the particle variant). It has no renderer data of its own: its draw is the renderer's quads, its vertices
			// ParticleSnapshots' (Coordinator::ResolveGeometrySlot).
			if (ParticleGeometryType(g.type) && Extensions::GeometryTypeOn(ActiveToggles(), g.type)) {
				const auto* port = SceneCapture::LeafView::Port(a_leaf.property);
				if (!port || !Extensions::PortTakesParticles(ActiveToggles(), port->port))
					return Ineligible::NotLightingShader;
				LightingDescriptors descriptors;
				const Ineligible reason = Extensions::DerivePort(port->port, *port, a_leaf, descriptors);
				if (a_descriptors && reason == Ineligible::None)
					*a_descriptors = descriptors;
				return reason;
			}
			if (!entryShape)
				return Ineligible::NotTriShape;
		}

		if (g.skin) {
			if (!Extensions::ObjectClassOn(ActiveToggles(), Api::ObjectClass::Skinned))
				return Ineligible::Skinned;
			// The skinned path (engine notes: skinning): a NiSkinInstance, or with CS_DCLF_SKIN_PARTITIONS a
			// BSDismemberSkinInstance or several partitions, and a palette the native shader could index (240
			// rows). Each partition the engine draws is one draw of that PARTITION's buffer, not the geometry's
			// rendererData, which for a skinned shape is a different TriShape; which partitions it draws is
			// SkinPartitionMask's, per frame.
			static const REL::Relocation<const RE::NiRTTI*> niSkinInstance{ RE::NiSkinInstance::Ni_RTTI };
			static const REL::Relocation<const RE::NiRTTI*> dismemberSkinInstance{ RE::BSDismemberSkinInstance::Ni_RTTI };
			const bool dismember = g.skinRtti == dismemberSkinInstance.get();
			if (g.skinRtti != niSkinInstance.get() && !dismember)
				return Ineligible::SkinShape;
			const auto partitions = static_cast<std::uint32_t>(g.partitions.size());
			if (!g.skinPartition || !g.skinData || partitions == 0 || g.boneCount == 0 || g.boneCount * 3 > 240)
				return Ineligible::SkinShape;
			if ((partitions > 1 || dismember) && !Extensions::ObjectClassOn(ActiveToggles(), Api::ObjectClass::SkinPartitions))
				return Ineligible::SkinShape;
			if (partitions > kMaxSkinPartitions)
				return Ineligible::SkinShape;
			// The engine indexes the dismember flags by partition; a flag array of another length is not one it
			// could have drawn from either.
			if (dismember && !g.shown.empty() && g.shown.size() != partitions)
				return Ineligible::SkinShape;
			// Every partition is drawn with the first one's pipeline, so they share its vertex layout; a LOD
			// byte past 2 would index the next row of the engine's table.
			const auto& first = g.partitions[0];
			for (const auto& p : g.partitions) {
				if (!p.vertexBuffer || !p.indexBuffer)
					return Ineligible::NoRendererData;
				if (p.vertexDesc != first.vertexDesc || p.lodByte > 2)
					return Ineligible::SkinShape;
			}
		} else if (!g.rendererData || !g.vertexBuffer || !g.indexBuffer) {
			return Ineligible::NoRendererData;
		}

		const auto* property = SceneCapture::LeafView::Lighting(a_leaf.property);
		if (!property) {
			// A material port while its class is on (the Effect port, Api::ObjectClass::Effect: a BSEffectShaderProperty on a BSTriShape, drawn
			// by the forward epoch when its pass is the static blended mesh's: DeriveEffectDescriptors). Any other shape of one keeps the port's
			// own variant reason.
			const auto* port = SceneCapture::LeafView::Port(a_leaf.property);
			if (!port || !Extensions::PortOn(ActiveToggles(), port->port))
				return Ineligible::NotLightingShader;
			if (type != RE::BSGeometry::Type::kTriShape)
				return Ineligible::EffectVariant;
			LightingDescriptors descriptors;
			const Ineligible reason = Extensions::DerivePort(port->port, *port, a_leaf, descriptors);
			if (a_descriptors && reason == Ineligible::None)
				*a_descriptors = descriptors;
			return reason;
		}

		// GetRenderPasses treats material alpha below one as transparent (the record's: 1 without a material): blended, the forward
		// epoch's with CS_DCLF_FORWARD (DeriveLightingDescriptors), else native.
		if (property->materialAlpha < 1.0f && !ActiveToggles().forward)
			return Ineligible::AlphaBlend;

		LightingDescriptors descriptors;
		const Ineligible reason = DeriveLightingDescriptors(*property, a_leaf, a_accumulated, descriptors);
		if (a_descriptors) {
			if (reason == Ineligible::None) {
				*a_descriptors = descriptors;
			} else {
				// The rejection diagnostics have to survive the rejection: rejectedTechnique's default 0 is `none`, a
				// supported technique, so it is copied on rejection too.
				a_descriptors->rejectedTechnique = descriptors.rejectedTechnique;
			}
		}
		if (!multiIndex || reason != Ineligible::None)
			return reason;
		if (!g.layerProperty)
			return Ineligible::None;  // nothing but its own passes: a tri-shape
		return Extensions::ObjectClassOn(ActiveToggles(), Api::ObjectClass::Layer) && Layer(a_leaf, nullptr, nullptr) == Ineligible::None ? Ineligible::None : Ineligible::MultiIndex;
	}

	Ineligible Layer(const SceneCapture::LeafView& a_leaf, LightingDescriptors* a_descriptors, const AccumulatedPass* a_accumulated)
	{
		// The layer's passes are GetRenderPasses of the additional property for this geometry (FUN_1414b2330): the geometry's
		// alpha property and skin, the layer's flags and material.
		const auto* layer = SceneCapture::LeafView::Lighting(a_leaf.layer);
		if (!layer)
			return Ineligible::NotLightingShader;
		if (a_leaf.Skinned())
			return Ineligible::Skinned;  // a skinned layer is not modelled
		if (layer->materialAlpha < 1.0f)
			return Ineligible::AlphaBlend;
		LightingDescriptors descriptors;
		const Ineligible reason = DeriveLightingDescriptors(*layer, a_leaf, a_accumulated, descriptors, true);
		if (a_descriptors) {
			if (reason == Ineligible::None)
				*a_descriptors = descriptors;
			else
				a_descriptors->rejectedTechnique = descriptors.rejectedTechnique;
		}
		return reason;
	}

	bool Primary(const EntryVerdict& a_entry, const SceneCapture::LeafView& a_leaf)
	{
		if (!a_entry.record || !a_entry.classified || a_entry.reason != Ineligible::None || !a_leaf.property)
			return false;
		// A decal is a member like any other (its group from the settled state); the blended decal group is DCLF's too.
		const bool decal = (a_leaf.property->flags & 0xc000000ull) != 0;
		if (decal && !Extensions::ObjectClassOn(ActiveToggles(), Api::ObjectClass::Decal))
			return false;
		// A blended member is DCLF's too (the forward epoch: blended Lighting, the Effect port, its billboards), so its entry can be whole;
		// its eligibility (the entry's reason) already says whether the forward toggles take it.
		if (ActiveToggles().forward)
			return true;
		return decal || !a_leaf.AlphaBlending();
	}
}
