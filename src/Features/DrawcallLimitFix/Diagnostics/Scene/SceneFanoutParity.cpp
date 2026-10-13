#include "SceneObservers.h"
#include "SceneState.h"

#include "Features/DrawcallLimitFix/Scene/Coordinator/Internal.h"

// CS_DCLF_FANOUT_PARITY: every 60th pass, each round the pass fans out (the walk's rounds, the subtrees' prepares, the joins and the bind
// queue) evaluated again on the pass's thread, one element after another (the round's reference), and compared field by field.

namespace DCLF::Diagnostics
{
	namespace
	{
		using V = SceneView;

		/** @brief The first field two evaluations of one walk entry differ in, or null when they agree. */
		const char* Difference(const V::EvalResult& a_left, const V::EvalResult& a_right)
		{
			const auto& l = a_left;
			const auto& r = a_right;
			const auto& ld = l.delta;
			const auto& rd = r.delta;
			const auto leafSame = [](const SceneCapture::LeafView& a_a, const SceneCapture::LeafView& a_b) {
				return a_a.node == a_b.node && a_a.parent == a_b.parent && a_a.geometry == a_b.geometry && a_a.property == a_b.property && a_a.layer == a_b.layer &&
				       a_a.alpha == a_b.alpha && a_a.fadeNode == a_b.fadeNode && a_a.layerFadeNode == a_b.layerFadeNode;
			};
			const auto verdictSame = [](const V::Tracked::StaticVerdict& a_a, const V::Tracked::StaticVerdict& a_b) {
				return a_a.cached == a_b.cached && a_a.reason == a_b.reason && a_a.rendererData == a_b.rendererData && a_a.property == a_b.property &&
				       a_a.material == a_b.material && a_a.fadeState == a_b.fadeState;
			};
			const std::pair<const char*, bool> fields[] = {
				{ "kind", l.kind == r.kind },
				{ "light", l.light == r.light },
				{ "lightRecorded", l.lightRecorded == r.lightRecorded },
				{ "lightSkin", l.lightSkin == r.lightSkin },
				{ "skinRows", l.skinRows == r.skinRows },
				{ "skinPartitions", l.skinPartitions == r.skinPartitions },
				{ "skinLodPartitions", l.skinLodPartitions == r.skinLodPartitions },
				{ "verdictUnannounced", l.verdictUnannounced == r.verdictUnannounced },
				{ "write", l.write == r.write },
				{ "delta.faceShape", ld.faceShapeSet == rd.faceShapeSet && ld.faceShape == rd.faceShape },
				{ "delta.actorOwned", ld.actorOwnedSet == rd.actorOwnedSet && ld.actorOwned == rd.actorOwned },
				{ "delta.verdict", ld.verdictSet == rd.verdictSet && verdictSame(ld.verdict, rd.verdict) },
				{ "delta.candidate", ld.candidateSet == rd.candidateSet && ld.candidateFrame == rd.candidateFrame && ld.candidateReason == rd.candidateReason },
				{ "delta.classifyInputs", ld.classifyInputsSet == rd.classifyInputsSet && ld.classifyInputs == rd.classifyInputs },
				{ "delta.inputComponents", ld.inputComponentsSet == rd.inputComponentsSet && ld.inputComponents == rd.inputComponents },
				{ "leaf", leafSame(l.leaf, r.leaf) },
				{ "reason", l.reason == r.reason },
				{ "shadowOnly", l.shadowOnly == r.shadowOnly },
				{ "faceShape", l.faceShape == r.faceShape },
				{ "referenceReason", l.referenceReason == r.referenceReason },
				{ "partitionMask", l.partitionMask == r.partitionMask },
				{ "lodChain", l.lodChain == r.lodChain },
				{ "skinCandidate", l.skinCandidate == r.skinCandidate },
				{ "boneRows", l.boneRows == r.boneRows },
				{ "shadowReject", l.shadowReject == r.shadowReject },
				{ "shadowTechnique", l.shadowTechnique == r.shadowTechnique },
				{ "shadowMaterial", l.shadowMaterial == r.shadowMaterial },
				{ "shadowDiffuse", l.shadowDiffuse == r.shadowDiffuse },
				{ "occlusion", l.occlusion == r.occlusion },
				{ "skinLodWord", l.skinLodWord == r.skinLodWord },
				{ "classified", l.classified == r.classified },
				{ "traits", l.traits == r.traits },
				{ "mayRecord", l.mayRecord == r.mayRecord },
				{ "rootCandidate", l.rootCandidate == r.rootCandidate },
				{ "rootMovesNow", l.rootMovesNow == r.rootMovesNow },
				{ "moveKey", l.moveKey == r.moveKey },
				{ "shadingInputs", l.shadingInputsSet == r.shadingInputsSet && l.shadingInputs == r.shadingInputs },
			};
			for (const auto& [name, same] : fields)
				if (!same)
					return name;
			return nullptr;
		}

		/** @brief The first field two prepares of one subtree differ in, or null. */
		const char* Difference(const V::SubtreePrep& a_left, const V::SubtreePrep& a_right)
		{
			const auto& l = a_left;
			const auto& r = a_right;
			if (l.root != r.root || l.source != r.source)
				return "root";
			if (l.category != r.category)
				return "category";
			if (l.unmirrored != r.unmirrored)
				return "unmirrored";
			if (l.geometries.size() != r.geometries.size())
				return "geometries";
			for (std::size_t i = 0; i < l.geometries.size(); ++i) {
				const auto& a = l.geometries[i];
				const auto& b = r.geometries[i];
				const std::pair<const char*, bool> fields[] = {
					{ "geometry", a.geometry == b.geometry },
					{ "reason", a.reason == b.reason },
					{ "actorOwner", a.actorOwner == b.actorOwner },
					{ "roomNode", a.roomNode == b.roomNode },
					{ "faceHead", a.faceHead == b.faceHead },
					{ "mirrored", a.mirrored == b.mirrored },
					{ "subIndex", a.subIndex == b.subIndex },
					{ "resolved", a.resolved == b.resolved },
					{ "sunEntry", a.sunEntry == b.sunEntry },
					{ "lightActorRoot", a.lightActorRoot == b.lightActorRoot },
				};
				for (const auto& [name, same] : fields)
					if (!same)
						return name;
			}
			return nullptr;
		}

		/** @brief The first field two bind queue evaluations differ in, or null. */
		const char* Difference(const V::JoinPre& a_left, const V::JoinPre& a_right)
		{
			const auto& l = a_left;
			const auto& r = a_right;
			const std::pair<const char*, bool> fields[] = {
				{ "kind", l.kind == r.kind },
				{ "geometry", l.geometry == r.geometry },
				{ "layer", l.layer == r.layer },
				{ "leaf", l.leaf.node == r.leaf.node && l.leaf.geometry == r.leaf.geometry && l.leaf.property == r.leaf.property && l.leaf.layer == r.leaf.layer &&
				              l.leaf.alpha == r.leaf.alpha && l.leaf.fadeNode == r.leaf.fadeNode },
				{ "derived", l.derived == r.derived },
				{ "pass", l.pass.technique == r.pass.technique && l.pass.subPass == r.pass.subPass && l.pass.passEnum == r.pass.passEnum && l.pass.hint == r.pass.hint &&
				              l.pass.lodRow == r.pass.lodRow && l.pass.sunTest == r.pass.sunTest && l.pass.resident == r.pass.resident &&
				              l.pass.fadeDistance == r.pass.fadeDistance && l.pass.heightTest == r.pass.heightTest },
				{ "fail", l.fail == r.fail },
			};
			for (const auto& [name, same] : fields)
				if (!same)
					return name;
			return nullptr;
		}

		/** @brief The first field two join evaluations differ in, or null. */
		const char* Difference(const V::JoinResult& a_left, const V::JoinResult& a_right)
		{
			const auto& l = a_left;
			const auto& r = a_right;
			const auto& ld = l.descriptors;
			const auto& rd = r.descriptors;
			const std::pair<const char*, bool> fields[] = {
				{ "kind", l.kind == r.kind },
				{ "leaf", l.leaf.node == r.leaf.node && l.leaf.geometry == r.leaf.geometry && l.leaf.property == r.leaf.property && l.leaf.layer == r.leaf.layer &&
				              l.leaf.alpha == r.leaf.alpha && l.leaf.fadeNode == r.leaf.fadeNode },
				{ "property", l.property == r.property },
				{ "material", l.material == r.material },
				{ "fadeState", l.fadeState == r.fadeState },
				{ "alphaBelowOne", l.alphaBelowOne == r.alphaBelowOne },
				{ "geometrySlot", l.geometrySlot == r.geometrySlot },
				{ "derivedWitness", l.derivedWitness == r.derivedWitness },
				{ "classified", l.classified == r.classified },
				{ "reason", l.reason == r.reason },
				{ "descriptors", ld.technique == rd.technique && ld.pass == rd.pass && ld.vertex == rd.vertex && ld.pixel == rd.pixel && ld.rawVertex == rd.rawVertex &&
				                     ld.rawPixel == rd.rawPixel && ld.derivedPass == rd.derivedPass && ld.rejectedTechnique == rd.rejectedTechnique &&
				                     ld.decalGroup == rd.decalGroup && ld.decalBlendMode == rd.decalBlendMode && ld.decalWriteMode == rd.decalWriteMode && ld.opaqueEffect == rd.opaqueEffect &&
				                     ld.terrainGroup == rd.terrainGroup &&
				                     ld.forward == rd.forward && ld.forwardBlendMode == rd.forwardBlendMode && ld.forwardDepthTest == rd.forwardDepthTest &&
				                     ld.forwardDepthWrite == rd.forwardDepthWrite && ld.forwardDecalGroup == rd.forwardDecalGroup && ld.refraction == rd.refraction &&
				                     ld.refractionFalloff == rd.refractionFalloff && ld.refractionClamp == rd.refractionClamp &&
				                     ld.projectedUV == rd.projectedUV && ld.effect == rd.effect },
				{ "key", l.key == r.key },
				{ "staticFlags", l.staticFlags == r.staticFlags },
				{ "room", l.roomSet == r.roomSet && l.roomIndex == r.roomIndex },
			};
			for (const auto& [name, same] : fields)
				if (!same)
					return name;
			return nullptr;
		}

		void OnFanoutDue(const FanoutDue& a_event)
		{
			a_event.checked = a_event.checked || ParityDue(V::passSerial(a_event.store), 11);
		}

		/** @brief a_results against a_serial's evaluation, the first difference described by a_describe(element, field). */
		template <class Result, class Describe>
		void Compare(std::span<const Result> a_results, const std::function<void(std::vector<Result>&)>& a_serial, Describe&& a_describe)
		{
			std::vector<Result> serial;
			a_serial(serial);
			auto& f = SceneDiagnostics().fanout;
			++f.parityRounds;
			f.parityEntries += a_results.size();
			for (std::size_t i = 0; i < a_results.size(); ++i)
				if (const char* field = Difference(a_results[i], serial[i]); field && f.parityDiffers++ == 0)
					f.parityFirst = a_describe(i, field);
		}

		void OnWalkRound(const WalkRoundEvaluated& a_event)
		{
			const auto& order = V::order(a_event.store);
			Compare(a_event.results, a_event.serial, [&](std::size_t a_index, const char* a_field) {
				const auto* geometry = order[a_event.first + a_index].geometry;
				return fmt::format("walk round from {}, entry {} '{}': {}", a_event.first, a_event.first + a_index, geometry && geometry->name.c_str() ? geometry->name.c_str() : "?",
					a_field);
			});
		}

		void OnSubtrees(const SubtreesPrepared& a_event)
		{
			Compare(a_event.results, a_event.serial, [&](std::size_t a_index, const char* a_field) {
				const auto source = a_event.roots[a_index].second;
				return fmt::format("subtrees ({}), root {} {}: {}",
					source == V::SubtreeSource::AttachEvent ? "attach events" : source == V::SubtreeSource::CategoryWalk ? "category walk" : "retry", a_index,
					static_cast<const void*>(a_event.roots[a_index].first), a_field);
			});
		}

		template <class Result>
		void OnJoins(const JoinsEvaluated<Result>& a_event)
		{
			Compare(a_event.results, a_event.serial, [&](std::size_t a_index, const char* a_field) { return fmt::format("{}, element {}: {}", a_event.what, a_index, a_field); });
		}
	}

	void InstallFanoutParity()
	{
		Subscribe<FanoutDue>(OnFanoutDue);
		Subscribe<WalkRoundEvaluated>(OnWalkRound);
		Subscribe<SubtreesPrepared>(OnSubtrees);
		Subscribe<JoinsEvaluated<V::JoinPre>>(OnJoins<V::JoinPre>);
		Subscribe<JoinsEvaluated<V::JoinResult>>(OnJoins<V::JoinResult>);
	}
}
