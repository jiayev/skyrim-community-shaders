#include "SceneCapture.h"

#include "Features/DrawcallLimitFix/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Scene/FadeState.h"
#include "TruePBR/BSLightingShaderMaterialPBR.h"

#include <atomic>
#include <chrono>
#include <cstring>

namespace DCLF::SceneCapture
{
	namespace
	{
		using Engine::At;

		constexpr std::uint32_t kMaxDepth = 64;

		std::atomic<std::uint64_t> attaches{ 0 }, records{ 0 }, ns{ 0 }, mainThreadCaptures{ 0 }, mainThreadNs{ 0 }, outOfWorld{ 0 };
		std::atomic<std::uint32_t> mainThread{ 0 };

		bool NonFixedBody(const RE::NiAVObject& a_object)
		{
			auto* collision = a_object.collisionObject.get();
			auto* ni = collision ? collision->AsBhkNiCollisionObject() : nullptr;
			if (!ni || !ni->body || !ni->body->GetRTTI() || !std::strstr(ni->body->GetRTTI()->GetName(), "RigidBody"))
				return false;
			auto* entity = static_cast<RE::hkpEntity*>(static_cast<RE::hkReferencedObject*>(ni->body->referencedObject.get()));
			return entity && entity->motion.type.get() != RE::hkpMotion::MotionType::kFixed;
		}

		bool TriShapeType(std::uint8_t a_type)
		{
			using Type = RE::BSGeometry::Type;
			return a_type >= static_cast<std::uint8_t>(Type::kTriShape) && a_type <= static_cast<std::uint8_t>(Type::kParticleShaderDynamicTriShape);
		}

		void Visit(const RE::NiAVObject& a_root, Records& a_out)
		{
			std::vector<const RE::NiAVObject*> stack{ &a_root };
			while (!stack.empty()) {
				const auto* object = stack.back();
				stack.pop_back();
				if (const auto* geometry = const_cast<RE::NiAVObject*>(object)->AsGeometry()) {
					CaptureLeaf(*geometry, a_out);
					continue;
				}
				a_out.nodes.push_back(CaptureNode(*object));
				if (const auto* node = const_cast<RE::NiAVObject*>(object)->AsNode())
					for (const auto& child : node->GetChildren())
						if (child)
							stack.push_back(child.get());
			}
		}
	}

	std::uint32_t NodeRecord::Differ(const NodeRecord& a_o) const
	{
		std::uint32_t d = 0;
		d |= parent != a_o.parent ? kParent : 0u;
		d |= rtti != a_o.rtti ? kRtti : 0u;
		d |= kind != a_o.kind ? kKind : 0u;
		d |= ((flags ^ a_o.flags) & 1u) ? kHidden : 0u;
		d |= ((flags ^ a_o.flags) & ~(1u | kFrameFlags)) ? kFlags : 0u;
		d |= (userData != a_o.userData || formType != a_o.formType || actor != a_o.actor) ? kUserData : 0u;
		d |= controllers != a_o.controllers ? kControllers : 0u;
		d |= body != a_o.body ? kBody : 0u;
		d |= children != a_o.children ? kChildren : 0u;
		d |= (switchIndex != a_o.switchIndex || switchFlags != a_o.switchFlags || switchCurrent != a_o.switchCurrent) ? kSwitch : 0u;
		d |= std::bit_cast<std::uint32_t>(fadeNear) != std::bit_cast<std::uint32_t>(a_o.fadeNear) ? kFadeNear : 0u;
		d |= std::bit_cast<std::uint32_t>(fadeFar) != std::bit_cast<std::uint32_t>(a_o.fadeFar) ? kFadeFar : 0u;
		d |= fade109 != a_o.fade109 ? kFade109 : 0u;
		d |= fadeType != a_o.fadeType ? kFadeType : 0u;
		d |= treeLodSwitch != a_o.treeLodSwitch ? kTreeLodSwitch : 0u;
		d |= name != a_o.name ? kName : 0u;
		return d;
	}

	std::uint32_t GeometryRecord::Differ(const GeometryRecord& a_o) const
	{
		std::uint32_t d = 0;
		d |= type != a_o.type ? kType : 0u;
		d |= (rendererData != a_o.rendererData || vertexBuffer != a_o.vertexBuffer || indexBuffer != a_o.indexBuffer || vertexDesc != a_o.vertexDesc ||
				 vertexCount != a_o.vertexCount || triangleCount != a_o.triangleCount) ?
		         kRenderer :
		         0u;
		d |= (skin != a_o.skin || skinRtti != a_o.skinRtti || skinPartition != a_o.skinPartition || skinData != a_o.skinData || boneCount != a_o.boneCount ||
				 partitions != a_o.partitions) ?
		         kSkin :
		         0u;
		d |= shown != a_o.shown ? kDismember : 0u;
		d |= property != a_o.property ? kProperty : 0u;
		d |= alpha != a_o.alpha ? kAlpha : 0u;
		d |= (layerProperty != a_o.layerProperty || altIndexBuffer != a_o.altIndexBuffer || altPrimCount != a_o.altPrimCount) ? kLayer : 0u;
		return d;
	}

	std::uint32_t PropertyRecord::Differ(const PropertyRecord& a_o) const
	{
		std::uint32_t d = 0;
		d |= rtti != a_o.rtti ? kRtti : 0u;
		d |= flags != a_o.flags ? kFlags : 0u;
		d |= material != a_o.material ? kMaterial : 0u;
		d |= std::bit_cast<std::uint32_t>(materialAlpha) != std::bit_cast<std::uint32_t>(a_o.materialAlpha) ? kMaterialAlpha : 0u;
		d |= (feature != a_o.feature || glints != a_o.glints || diffuseView != a_o.diffuseView) ? kMaterialOther : 0u;
		d |= fadeNode != a_o.fadeNode ? kFadeNode : 0u;
		d |= emissive != a_o.emissive ? kEmissive : 0u;
		d |= controllers != a_o.controllers ? kControllers : 0u;
		return d;
	}

	std::uint32_t AlphaRecord::Differ(const AlphaRecord& a_o) const
	{
		return (flags != a_o.flags ? kFlags : 0u) | (threshold != a_o.threshold ? kThreshold : 0u);
	}

	NodeRecord CaptureNode(const RE::NiAVObject& a_object)
	{
		auto& object = const_cast<RE::NiAVObject&>(a_object);
		NodeRecord r;
		r.key = &a_object;
		r.parent = a_object.parent;
		r.rtti = object.GetRTTI();
		r.flags = a_object.GetFlags().underlying();
		r.name = a_object.name.c_str();
		r.controllers = object.GetControllers() != nullptr;
		r.body = NonFixedBody(a_object);
		if (const auto* reference = a_object.GetUserData()) {
			r.userData = reference;
			r.formType = static_cast<std::uint8_t>(reference->GetFormType());
			r.actor = const_cast<RE::TESObjectREFR*>(reference)->IsActor();
		}
		if (object.AsGeometry()) {
			r.kind |= kKindGeometry;
			return r;
		}
		auto* node = object.AsNode();
		if (!node)
			return r;
		r.kind |= kKindNode;
		for (const auto& child : node->GetChildren())
			if (child)
				r.children.push_back(child.get());
		if (auto* switchNode = node->AsSwitchNode()) {
			r.kind |= kKindSwitch;
			// NiSwitchNode (AE 1.6.1170): flags +0x128, index +0x12C, revID +0x134, childRevID's data +0x140 and capacity +0x148.
			r.switchFlags = At<std::uint16_t>(switchNode, 0x128);
			r.switchIndex = At<std::int32_t>(switchNode, 0x12C);
			const auto revID = At<std::uint32_t>(switchNode, 0x134);
			const auto* childRevID = At<const std::uint32_t*>(switchNode, 0x140);
			const auto capacity = At<std::uint16_t>(switchNode, 0x148);
			r.switchCurrent = r.switchIndex >= 0 && childRevID && static_cast<std::uint32_t>(r.switchIndex) < capacity &&
			                  static_cast<std::uint32_t>(r.switchIndex) < node->GetChildren().capacity() && childRevID[r.switchIndex] == revID;
		}
		r.kind |= netimmerse_cast<RE::NiBillboardNode*>(node) ? kKindBillboard : 0u;
		r.kind |= netimmerse_cast<RE::BSOrderedNode*>(node) ? kKindOrdered : 0u;
		r.kind |= netimmerse_cast<RE::BSMultiBoundNode*>(node) ? kKindMultiBound : 0u;
		r.kind |= netimmerse_cast<RE::BSFaceGenNiNode*>(node) ? kKindFaceGen : 0u;
		if (netimmerse_cast<RE::BSFadeNode*>(node)) {
			r.kind |= kKindFadeNode;
			r.kind |= netimmerse_cast<RE::BSTreeNode*>(node) ? kKindTree : 0u;
			r.fadeNear = At<float>(node, 0x128);
			r.fadeFar = At<float>(node, 0x12C);
			r.fade109 = At<std::uint8_t>(node, 0x109);
			r.fadeType = At<std::uint8_t>(node, 0x153) & 0xF;
			r.treeLodSwitch = FadeState::TreeLodSwitch(*node);
		}
		return r;
	}

	GeometryRecord CaptureGeometry(const RE::BSGeometry& a_geometry)
	{
		auto& geometry = const_cast<RE::BSGeometry&>(a_geometry);
		const auto& data = a_geometry.GetGeometryRuntimeData();
		GeometryRecord r;
		r.key = &a_geometry;
		r.type = static_cast<std::uint8_t>(geometry.GetType().get());
		if (const auto* triShape = data.rendererData) {
			r.rendererData = triShape;
			r.vertexBuffer = reinterpret_cast<ID3D11Buffer*>(triShape->vertexBuffer);
			r.indexBuffer = reinterpret_cast<ID3D11Buffer*>(triShape->indexBuffer);
			r.vertexDesc = std::bit_cast<std::uint64_t>(triShape->vertexDesc);
		}
		if (TriShapeType(r.type)) {
			const auto& shape = static_cast<const RE::BSTriShape&>(a_geometry).GetTrishapeRuntimeData();
			r.vertexCount = shape.vertexCount;
			r.triangleCount = shape.triangleCount;
		}
		if (const auto* skin = data.skinInstance.get()) {
			r.skin = skin;
			r.skinRtti = const_cast<RE::NiSkinInstance*>(skin)->GetRTTI();
			const auto* partition = skin->skinPartition.get();
			r.skinPartition = partition;
			r.skinData = skin->skinData.get();
			r.boneCount = skin->skinData ? skin->skinData->GetBoneCount() : 0u;
			if (partition) {
				r.partitions.reserve(partition->numPartitions);
				for (std::uint32_t i = 0; i < partition->numPartitions; ++i) {
					const auto& p = partition->partitions[i];
					GeometryRecord::Partition out;
					if (p.buffData) {
						out.vertexBuffer = reinterpret_cast<ID3D11Buffer*>(p.buffData->vertexBuffer);
						out.indexBuffer = reinterpret_cast<ID3D11Buffer*>(p.buffData->indexBuffer);
						out.vertexDesc = std::bit_cast<std::uint64_t>(p.buffData->vertexDesc);
					}
					out.vertices = p.vertices;
					out.triangles = p.triangles;
					out.lodByte = static_cast<std::uint8_t>(p.pad42 & 0xFF);
					r.partitions.push_back(out);
				}
			}
			static const REL::Relocation<const RE::NiRTTI*> dismember{ RE::BSDismemberSkinInstance::Ni_RTTI };
			if (r.skinRtti == dismember.get()) {
				const auto& flags = static_cast<const RE::BSDismemberSkinInstance*>(skin)->GetRuntimeData();
				if (flags.partitions)
					for (std::int32_t i = 0; i < flags.numPartitions; ++i)
						r.shown.push_back(flags.partitions[i].editorVisible ? 1 : 0);
			}
		}
		r.property = data.shaderProperty.get();
		r.alpha = data.alphaProperty.get();
		if (r.type == static_cast<std::uint8_t>(RE::BSGeometry::Type::kMultiIndexTriShape)) {
			const auto& multi = static_cast<const RE::BSMultiIndexTriShape&>(a_geometry).GetMultiIndexTrishapeRuntimeData();
			r.altIndexBuffer = multi.altIndexBuffer ? *reinterpret_cast<ID3D11Buffer* const*>(multi.altIndexBuffer) : nullptr;
			r.altPrimCount = multi.altPrimCount;
			r.layerProperty = multi.additionalShaderProperty.get();
		}
		return r;
	}

	PropertyRecord CaptureProperty(const RE::BSShaderProperty& a_property)
	{
		auto& property = const_cast<RE::BSShaderProperty&>(a_property);
		PropertyRecord r;
		r.key = &a_property;
		r.rtti = property.GetRTTI();
		r.flags = a_property.flags.underlying();
		r.material = a_property.material;
		r.fadeNode = a_property.fadeNode;
		r.controllers = property.GetControllers() != nullptr;
		if (const auto* lighting = netimmerse_cast<const RE::BSLightingShaderProperty*>(&a_property)) {
			r.lighting = true;
			r.emissive = lighting->emissiveColor;
			if (const auto* material = static_cast<const RE::BSLightingShaderMaterialBase*>(a_property.material)) {
				r.materialAlpha = material->materialAlpha;
				const auto* texture = material->diffuseTexture ? material->diffuseTexture->rendererTexture : nullptr;
				r.diffuseView = texture ? texture->resourceView : nullptr;
				const auto feature = const_cast<RE::BSLightingShaderMaterialBase*>(material)->GetFeature();
				r.feature = static_cast<std::uint32_t>(feature);
				// LightingDescriptors' PBR test (Community Shaders' GetRenderPasses: TruePBR.cpp).
				const bool pbr = a_property.flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kVertexLighting) &&
				                 (feature == RE::BSShaderMaterial::Feature::kDefault || feature == RE::BSShaderMaterial::Feature::kMultiTexLandLODBlend);
				r.glints = pbr && static_cast<const BSLightingShaderMaterialPBR*>(a_property.material)->glintParameters.enabled;
			}
		}
		return r;
	}

	AlphaRecord CaptureAlpha(const RE::NiAlphaProperty& a_alpha)
	{
		AlphaRecord r;
		r.key = &a_alpha;
		r.flags = a_alpha.alphaFlags;
		r.threshold = a_alpha.alphaThreshold;
		return r;
	}

	void CaptureLeaf(const RE::BSGeometry& a_geometry, Records& a_out)
	{
		a_out.nodes.push_back(CaptureNode(a_geometry));
		a_out.geometries.push_back(CaptureGeometry(a_geometry));
		const auto& g = a_out.geometries.back();
		if (g.property)
			a_out.properties.push_back(CaptureProperty(*static_cast<const RE::BSShaderProperty*>(g.property)));
		if (g.layerProperty)
			a_out.properties.push_back(CaptureProperty(*static_cast<const RE::BSShaderProperty*>(g.layerProperty)));
		if (g.alpha)
			a_out.alphas.push_back(CaptureAlpha(*static_cast<const RE::NiAlphaProperty*>(g.alpha)));
	}

	bool InWorld(const RE::NiAVObject* a_object)
	{
		const auto* root = static_cast<const RE::NiAVObject*>(RE::Main::WorldRootNode());
		for (std::uint32_t depth = 0; root && a_object && depth <= kMaxDepth; ++depth, a_object = a_object->parent)
			if (a_object == root)
				return true;
		return false;
	}

	std::unique_ptr<Records> CaptureAttached(const RE::NiAVObject& a_root)
	{
		if (!InWorld(&a_root)) {
			outOfWorld.fetch_add(1, std::memory_order_relaxed);
			return nullptr;
		}
		const auto start = std::chrono::steady_clock::now();
		auto out = std::make_unique<Records>();
		Visit(a_root, *out);
		// The ancestors: their children lists changed (the parent's), and a record is wanted for every node a chain walks.
		for (const auto* ancestor = a_root.parent; ancestor; ancestor = ancestor->parent)
			out->nodes.push_back(CaptureNode(*ancestor));
		NoteCapture(out->nodes.size() + out->geometries.size() + out->properties.size() + out->alphas.size(),
			static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count()));
		return out;
	}

	void NoteCapture(std::uint64_t a_records, std::uint64_t a_ns)
	{
		attaches.fetch_add(1, std::memory_order_relaxed);
		records.fetch_add(a_records, std::memory_order_relaxed);
		ns.fetch_add(a_ns, std::memory_order_relaxed);
		if (::GetCurrentThreadId() == mainThread.load(std::memory_order_relaxed)) {
			mainThreadCaptures.fetch_add(1, std::memory_order_relaxed);
			mainThreadNs.fetch_add(a_ns, std::memory_order_relaxed);
		}
	}

	void NoteOutOfWorld()
	{
		outOfWorld.fetch_add(1, std::memory_order_relaxed);
	}

	void SetMainThread(std::uint32_t a_thread)
	{
		mainThread.store(a_thread, std::memory_order_relaxed);
	}

	Counters TakeCounters()
	{
		Counters c;
		c.attaches = attaches.exchange(0, std::memory_order_relaxed);
		c.records = records.exchange(0, std::memory_order_relaxed);
		c.ns = ns.exchange(0, std::memory_order_relaxed);
		c.mainThread = mainThreadCaptures.exchange(0, std::memory_order_relaxed);
		c.mainThreadNs = mainThreadNs.exchange(0, std::memory_order_relaxed);
		c.outOfWorld = outOfWorld.exchange(0, std::memory_order_relaxed);
		return c;
	}
}
