#include "SceneCapture.h"

#include "Features/DrawcallLimitFix/Diagnostics/MirrorWatch.h"
#include "Features/DrawcallLimitFix/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Engine/EngineReadWindow.h"
#include "Features/DrawcallLimitFix/Scene/MaterialPort.h"
#include "Features/DrawcallLimitFix/Scene/FadeState.h"
#include "Features/SubsurfaceScattering.h"
#include "Globals.h"
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
		// +0x109's bits written every frame, not the record's: 0x40 the cull's (FUN_140d1c5f0), 0x20 the strip particles' update (set) and the cell's update pass (clear, FUN_1402b41a0).
		constexpr std::uint8_t kFade109FrameBits = 0x60;

		std::atomic<std::uint64_t> attaches{ 0 }, records{ 0 }, ns{ 0 }, mainThreadCaptures{ 0 }, mainThreadNs{ 0 }, outOfWorld{ 0 };
		std::atomic<std::uint32_t> mainThread{ 0 };
		std::atomic<std::uint32_t> currentFrame{ 0 };
		std::atomic<std::uint64_t> sequence{ 0 };

		bool NonFixedBody(const RE::NiAVObject& a_object)
		{
			auto* collision = a_object.collisionObject.get();
			auto* ni = collision ? collision->AsBhkNiCollisionObject() : nullptr;
			if (!ni || !ni->body || !ni->body->GetRTTI() || !std::strstr(ni->body->GetRTTI()->GetName(), "RigidBody"))
				return false;
			auto* entity = static_cast<RE::hkpEntity*>(static_cast<RE::hkReferencedObject*>(ni->body->referencedObject.get()));
			return entity && entity->motion.type.get() != RE::hkpMotion::MotionType::kFixed;
		}

		// T6b1a's extras, read on the capturing thread (a hook's or the probe's), never the scene work's.
		std::int32_t BsxOf(const RE::NiNode& a_node)
		{
			static const RE::BSFixedString name{ "BSX" };
			const auto* extra = const_cast<RE::NiNode&>(a_node).GetExtraData(name);
			return extra ? static_cast<std::int32_t>(static_cast<const RE::BSXFlags*>(extra)->value) : -1;
		}

		std::int64_t AnisotropicOf(const RE::BSGeometry& a_geometry)
		{
			static const RE::BSFixedString name{ "AnisotropicAlphaMaterial" };
			const auto* extra = const_cast<RE::BSGeometry&>(a_geometry).GetExtraData(name);
			if (!extra)
				return GeometryRecord::kNoExtra;
			if (const_cast<RE::NiExtraData*>(extra)->GetRTTI() != globals::rtti::NiIntegerExtraDataRTTI.get())
				return GeometryRecord::kWrongExtra;
			return static_cast<std::uint32_t>(static_cast<const RE::NiIntegerExtraData*>(extra)->value);
		}

		bool EmittanceOf(const RE::TESObjectREFR& a_reference)
		{
			const auto* source = a_reference.extraList.GetByType<RE::ExtraEmittanceSource>();
			return source && source->source;
		}

		bool BeastOf(const RE::TESObjectREFR& a_reference)
		{
			const auto& sss = globals::features::subsurfaceScattering;
			if (!sss.loaded || !sss.isBeastRaceKeyword)
				return true;
			if (auto* actor = const_cast<RE::TESObjectREFR&>(a_reference).As<RE::Actor>())
				if (auto* race = actor->GetRace())
					return race->HasKeyword(sss.isBeastRaceKeyword);
			return true;
		}

		// A fade node's per-frame state: currentFade (+0x130), the LOD level (+0x152 & 0xF), the screen-door byte (+0x154).
		void CaptureFadeState(const RE::NiAVObject& a_node, NodeRecord& a_out)
		{
			a_out.currentFade = At<float>(&a_node, 0x130);
			a_out.fadeLevel = At<std::uint8_t>(&a_node, 0x152) & 0xF;
			a_out.fadeDoor = At<std::uint8_t>(&a_node, 0x154);
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
				if (a_out.pinning)
					a_out.pins.emplace_back(const_cast<RE::NiAVObject*>(object));
				if (const auto* geometry = const_cast<RE::NiAVObject*>(object)->AsGeometry()) {
					CaptureLeaf(*geometry, a_out, a_out.pinning ? &a_out.pins : nullptr);
					MirrorWatch::ArmGeometry(geometry);
					continue;
				}
				a_out.nodes.push_back(CaptureNode(*object));
				MirrorWatch::ArmNode(object);
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
		d |= ((flags ^ a_o.flags) & RecordFlags(kind)) ? kFlags : 0u;
		d |= (userData != a_o.userData || formType != a_o.formType || actor != a_o.actor) ? kUserData : 0u;
		d |= controllers != a_o.controllers ? kControllers : 0u;
		d |= body != a_o.body ? kBody : 0u;
		d |= children != a_o.children ? kChildren : 0u;
		d |= (switchIndex != a_o.switchIndex || switchFlags != a_o.switchFlags || switchCurrent != a_o.switchCurrent || switchChild != a_o.switchChild) ? kSwitch : 0u;
		d |= (bsx != a_o.bsx || emittance != a_o.emittance || beast != a_o.beast) ? kExtra : 0u;
		d |= std::bit_cast<std::uint32_t>(currentFade) != std::bit_cast<std::uint32_t>(a_o.currentFade) ? kFadeCurrent : 0u;
		d |= fadeLevel != a_o.fadeLevel ? kFadeLevel : 0u;
		d |= fadeDoor != a_o.fadeDoor ? kFadeDoor : 0u;
		d |= std::bit_cast<std::uint32_t>(fadeAmount) != std::bit_cast<std::uint32_t>(a_o.fadeAmount) ? kFadeAmount : 0u;
		d |= std::bit_cast<std::uint32_t>(fadeNear) != std::bit_cast<std::uint32_t>(a_o.fadeNear) ? kFadeNear : 0u;
		d |= std::bit_cast<std::uint32_t>(fadeFar) != std::bit_cast<std::uint32_t>(a_o.fadeFar) ? kFadeFar : 0u;
		d |= fade109 != a_o.fade109 ? kFade109 : 0u;
		d |= fadeType != a_o.fadeType ? kFadeType : 0u;
		d |= treeLodSwitch != a_o.treeLodSwitch ? kTreeLodSwitch : 0u;
		d |= decals != a_o.decals ? kDecals : 0u;
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
		d |= (layerProperty != a_o.layerProperty || altIndexList != a_o.altIndexList || altIndexBuffer != a_o.altIndexBuffer || altPrimCount != a_o.altPrimCount) ? kLayer : 0u;
		d |= anisotropic != a_o.anisotropic ? kExtra : 0u;
		d |= std::memcmp(multiParams.data(), a_o.multiParams.data(), sizeof(multiParams)) != 0 ? kMultiParams : 0u;
		d |= segments != a_o.segments ? kSegments : 0u;
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
		d |= std::bit_cast<std::uint32_t>(alpha) != std::bit_cast<std::uint32_t>(a_o.alpha) ? kAlphaValue : 0u;
		d |= std::memcmp(projected.data(), a_o.projected.data(), sizeof(projected)) != 0 ? kProjected : 0u;
		d |= std::memcmp(landBlend.data(), a_o.landBlend.data(), sizeof(landBlend)) != 0 ? kLandBlend : 0u;
		d |= shadowPasses != a_o.shadowPasses ? kShadowPasses : 0u;
		return d;
	}

	std::uint32_t AlphaRecord::Differ(const AlphaRecord& a_o) const
	{
		return (flags != a_o.flags ? kFlags : 0u) | (threshold != a_o.threshold ? kThreshold : 0u) | (rtti != a_o.rtti ? kRtti : 0u) |
		       (controllers != a_o.controllers ? kControllers : 0u);
	}

	void NodeRecord::Assign(const NodeRecord& a_o, std::uint32_t a_f)
	{
		if (a_f & kParent)
			parent = a_o.parent;
		if (a_f & kRtti)
			rtti = a_o.rtti;
		if (a_f & kKind)
			kind = a_o.kind;
		if (a_f & kHidden)
			flags = (flags & ~1u) | (a_o.flags & 1u);
		if (a_f & kFlags)
			flags = (flags & ~RecordFlags(kind)) | (a_o.flags & RecordFlags(kind));
		if (a_f & kUserData) {
			userData = a_o.userData;
			formType = a_o.formType;
			actor = a_o.actor;
		}
		if (a_f & kControllers)
			controllers = a_o.controllers;
		if (a_f & kBody)
			body = a_o.body;
		if (a_f & kChildren)
			children = a_o.children;
		if (a_f & kSwitch) {
			switchIndex = a_o.switchIndex;
			switchFlags = a_o.switchFlags;
			switchCurrent = a_o.switchCurrent;
			switchChild = a_o.switchChild;
		}
		if (a_f & kExtra) {
			bsx = a_o.bsx;
			emittance = a_o.emittance;
			beast = a_o.beast;
		}
		if (a_f & kFadeCurrent)
			currentFade = a_o.currentFade;
		if (a_f & kFadeLevel)
			fadeLevel = a_o.fadeLevel;
		if (a_f & kFadeDoor)
			fadeDoor = a_o.fadeDoor;
		if (a_f & kFadeAmount)
			fadeAmount = a_o.fadeAmount;
		if (a_f & kFadeNear)
			fadeNear = a_o.fadeNear;
		if (a_f & kFadeFar)
			fadeFar = a_o.fadeFar;
		if (a_f & kFade109)
			fade109 = a_o.fade109;
		if (a_f & kFadeType)
			fadeType = a_o.fadeType;
		if (a_f & kTreeLodSwitch)
			treeLodSwitch = a_o.treeLodSwitch;
		if (a_f & kDecals)
			decals = a_o.decals;
		if (a_f & kName)
			name = a_o.name;
		updatedFrame = Frame();
	}

	void GeometryRecord::Assign(const GeometryRecord& a_o, std::uint32_t a_f)
	{
		if (a_f & kType)
			type = a_o.type;
		if (a_f & kRenderer) {
			rendererData = a_o.rendererData;
			vertexBuffer = a_o.vertexBuffer;
			indexBuffer = a_o.indexBuffer;
			vertexDesc = a_o.vertexDesc;
			vertexCount = a_o.vertexCount;
			triangleCount = a_o.triangleCount;
		}
		if (a_f & kSkin) {
			skin = a_o.skin;
			skinRtti = a_o.skinRtti;
			skinPartition = a_o.skinPartition;
			skinData = a_o.skinData;
			boneCount = a_o.boneCount;
			partitions = a_o.partitions;
		}
		if (a_f & kDismember)
			shown = a_o.shown;
		if (a_f & kProperty)
			property = a_o.property;
		if (a_f & kAlpha)
			alpha = a_o.alpha;
		if (a_f & kLayer) {
			layerProperty = a_o.layerProperty;
			altIndexList = a_o.altIndexList;
			altIndexBuffer = a_o.altIndexBuffer;
			altPrimCount = a_o.altPrimCount;
		}
		if (a_f & kExtra)
			anisotropic = a_o.anisotropic;
		if (a_f & kMultiParams)
			multiParams = a_o.multiParams;
		if (a_f & kSegments)
			segments = a_o.segments;
	}

	void PropertyRecord::Assign(const PropertyRecord& a_o, std::uint32_t a_f)
	{
		if (a_f & kRtti) {
			rtti = a_o.rtti;
			lighting = a_o.lighting;
		}
		if (a_f & kFlags)
			flags = a_o.flags;
		if (a_f & kMaterial)
			material = a_o.material;
		if (a_f & kMaterialAlpha)
			materialAlpha = a_o.materialAlpha;
		if (a_f & kMaterialOther) {
			feature = a_o.feature;
			glints = a_o.glints;
			diffuseView = a_o.diffuseView;
			diffuseHeld = a_o.diffuseHeld;
		}
		if (a_f & kFadeNode)
			fadeNode = a_o.fadeNode;
		if (a_f & kEmissive)
			emissive = a_o.emissive;
		if (a_f & kControllers)
			controllers = a_o.controllers;
		if (a_f & kAlphaValue)
			alpha = a_o.alpha;
		if (a_f & kProjected)
			projected = a_o.projected;
		if (a_f & kLandBlend)
			landBlend = a_o.landBlend;
		if (a_f & kShadowPasses)
			shadowPasses = a_o.shadowPasses;
		writer = 2;
		writtenFrame = Frame();
	}

	void AlphaRecord::Assign(const AlphaRecord& a_o, std::uint32_t a_f)
	{
		if (a_f & kFlags)
			flags = a_o.flags;
		if (a_f & kThreshold)
			threshold = a_o.threshold;
		if (a_f & kRtti)
			rtti = a_o.rtti;
		if (a_f & kControllers)
			controllers = a_o.controllers;
	}

	namespace
	{
		bool IsDecalNode(RE::NiAVObject& a_object)
		{
			static const REL::Relocation<const RE::NiRTTI*> decalNode{ RE::BGSDecalNode::Ni_RTTI };
			return a_object.GetRTTI() == decalNode.get();
		}

		void CaptureDecals(const RE::NiAVObject& a_node, NodeRecord& r)
		{
			r.decals.clear();
			for (const auto& decal : static_cast<const RE::BGSDecalNode&>(a_node).GetRuntimeData().decals)
				r.decals.push_back(decal ? decal->Get3D() : nullptr);
		}

		void CaptureSwitch(const RE::NiNode& a_node, NodeRecord& r)
		{
			// NiSwitchNode (AE 1.6.1170): flags +0x128, index +0x12C, revID +0x134, childRevID's data +0x140 and capacity +0x148.
			r.switchFlags = At<std::uint16_t>(&a_node, 0x128);
			r.switchIndex = At<std::int32_t>(&a_node, 0x12C);
			const auto revID = At<std::uint32_t>(&a_node, 0x134);
			const auto* childRevID = At<const std::uint32_t*>(&a_node, 0x140);
			const auto capacity = At<std::uint16_t>(&a_node, 0x148);
			r.switchCurrent = r.switchIndex >= 0 && childRevID && static_cast<std::uint32_t>(r.switchIndex) < capacity &&
			                  static_cast<std::uint32_t>(r.switchIndex) < a_node.GetChildren().capacity() && childRevID[r.switchIndex] == revID;
			r.switchChild = nullptr;
			if (r.switchIndex >= 0 && static_cast<std::uint32_t>(r.switchIndex) < a_node.GetChildren().capacity())
				r.switchChild = a_node.GetChildren()[static_cast<std::uint16_t>(r.switchIndex)].get();
		}
	}

	NodeRecord CaptureNodeFields(const RE::NiAVObject& a_object, std::uint32_t a_fields)
	{
		EngineReadWindow::Touch("SceneCapture::CaptureNodeFields");
		using F = NodeRecord::Field;
		auto& object = const_cast<RE::NiAVObject&>(a_object);
		NodeRecord r;
		r.key = &a_object;
		if (a_fields & (F::kHidden | F::kFlags))
			r.flags = a_object.GetFlags().underlying();
		if (a_fields & F::kControllers)
			r.controllers = object.GetControllers() != nullptr;
		if (a_fields & F::kBody)
			r.body = NonFixedBody(a_object);
		if (a_fields & (F::kFadeNear | F::kFadeFar | F::kFade109 | F::kFadeType)) {
			r.fadeNear = At<float>(&a_object, 0x128);
			r.fadeFar = At<float>(&a_object, 0x12C);
			r.fade109 = At<std::uint8_t>(&a_object, 0x109) & ~kFade109FrameBits;
			r.fadeType = At<std::uint8_t>(&a_object, 0x153) & 0xF;
		}
		if (a_fields & (F::kFadeCurrent | F::kFadeLevel | F::kFadeDoor))
			CaptureFadeState(a_object, r);
		if (a_fields & F::kFadeAmount)
			r.fadeAmount = At<float>(&a_object, 0x100);
		// T6b1a: a switch's index store or child edit (the caller's node is a switch).
		if (a_fields & F::kSwitch)
			if (const auto* node = object.AsNode())
				CaptureSwitch(*node, r);
		// T6b1a: a decal node's array edit.
		if ((a_fields & F::kDecals) && IsDecalNode(object))
			CaptureDecals(a_object, r);
		return r;
	}

	NodeRecord CaptureNode(const RE::NiAVObject& a_object)
	{
		EngineReadWindow::Touch("SceneCapture::CaptureNode");
		auto& object = const_cast<RE::NiAVObject&>(a_object);
		NodeRecord r;
		r.key = &a_object;
		r.capturedFrame = Frame();
		r.thread = ::GetCurrentThreadId();
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
			r.emittance = EmittanceOf(*reference);
			r.beast = BeastOf(*reference);
		}
		if (object.AsGeometry()) {
			r.kind |= kKindGeometry;
			return r;
		}
		auto* node = object.AsNode();
		if (!node)
			return r;
		r.kind |= kKindNode;
		const auto& children = node->GetChildren();
		for (std::uint16_t i = 0; i < children.free_idx(); ++i)
			r.children.push_back(children[i].get());
		while (!r.children.empty() && !r.children.back())
			r.children.pop_back();
		if (IsDecalNode(object)) {
			r.kind |= kKindDecalNode;
			CaptureDecals(a_object, r);
		}
		if (node->AsSwitchNode()) {
			r.kind |= kKindSwitch;
			CaptureSwitch(*node, r);
		}
		r.kind |= netimmerse_cast<RE::NiBillboardNode*>(node) ? kKindBillboard : 0u;
		r.kind |= netimmerse_cast<RE::BSOrderedNode*>(node) ? kKindOrdered : 0u;
		r.kind |= netimmerse_cast<RE::BSMultiBoundNode*>(node) ? kKindMultiBound : 0u;
		r.kind |= netimmerse_cast<RE::BSFaceGenNiNode*>(node) ? kKindFaceGen : 0u;
		// The engine's own test (the vtable's AsFadeNode, which every fade writer reaches the node by), not the RTTI chain.
		if (node->AsFadeNode()) {
			r.kind |= kKindFadeNode;
			r.kind |= netimmerse_cast<RE::BSTreeNode*>(node) ? kKindTree : 0u;
			r.fadeNear = At<float>(node, 0x128);
			r.fadeFar = At<float>(node, 0x12C);
			r.fade109 = At<std::uint8_t>(node, 0x109) & ~kFade109FrameBits;
			r.fadeType = At<std::uint8_t>(node, 0x153) & 0xF;
			r.treeLodSwitch = FadeState::TreeLodSwitch(*node);
			r.bsx = BsxOf(*node);
			CaptureFadeState(*node, r);
			r.fadeAmount = At<float>(node, 0x100);
		}
		return r;
	}

	GeometryRecord CaptureGeometry(const RE::BSGeometry& a_geometry)
	{
		EngineReadWindow::Touch("SceneCapture::CaptureGeometry");
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
						out.rendererData = p.buffData;
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
			r.altIndexList = multi.altIndexBuffer;
			r.altIndexBuffer = multi.altIndexBuffer ? *reinterpret_cast<ID3D11Buffer* const*>(multi.altIndexBuffer) : nullptr;
			r.altPrimCount = multi.altPrimCount;
			r.layerProperty = multi.additionalShaderProperty.get();
			std::memcpy(r.multiParams.data(), &multi.materialProjection, 16 * sizeof(float));
			r.multiParams[16] = multi.materialParams.red;
			r.multiParams[17] = multi.materialParams.green;
			r.multiParams[18] = multi.materialParams.blue;
			r.multiParams[19] = multi.materialParams.alpha;
			r.multiParams[20] = multi.materialScale;
			r.multiParams[21] = multi.normalDampener;
		}
		r.anisotropic = AnisotropicOf(a_geometry);
		if (r.type == static_cast<std::uint8_t>(RE::BSGeometry::Type::kSubIndexTriShape))
			LodSegments::DrawnRanges(&a_geometry, r.segments);
		return r;
	}

	PropertyRecord CaptureProperty(const RE::BSShaderProperty& a_property)
	{
		EngineReadWindow::Touch("SceneCapture::CaptureProperty");
		auto& property = const_cast<RE::BSShaderProperty&>(a_property);
		PropertyRecord r;
		r.key = &a_property;
		r.capturedFrame = Frame();
		r.thread = ::GetCurrentThreadId();
		r.rtti = property.GetRTTI();
		r.flags = a_property.flags.underlying();
		r.material = a_property.material;
		r.fadeNode = a_property.fadeNode;
		r.controllers = property.GetControllers() != nullptr;
		if (const auto* lighting = netimmerse_cast<const RE::BSLightingShaderProperty*>(&a_property)) {
			r.lighting = true;
			r.shadowPasses = lighting->shadowMapOrMaskPasses.head != nullptr;
			r.alpha = a_property.alpha;  // a Lighting property's (MaterialModelOf): GetRenderPasses' update carries it
			r.emissive = lighting->emissiveColor;
			const auto& params = lighting->projectedUVParams;
			const auto& colour = lighting->projectedUVColor;
			r.projected = { params.red, params.green, params.blue, params.alpha, colour.red, colour.green, colour.blue, colour.alpha };
			if (const auto* material = static_cast<const RE::BSLightingShaderMaterialBase*>(a_property.material)) {
				r.materialAlpha = material->materialAlpha;
				const auto* texture = material->diffuseTexture ? material->diffuseTexture->rendererTexture : nullptr;
				r.diffuseView = texture ? texture->resourceView : nullptr;
				r.diffuseHeld.copy_from(static_cast<ID3D11ShaderResourceView*>(const_cast<void*>(r.diffuseView)));
				const auto feature = const_cast<RE::BSLightingShaderMaterialBase*>(material)->GetFeature();
				r.feature = static_cast<std::uint32_t>(feature);
				// LightingDescriptors' PBR test (Community Shaders' GetRenderPasses: TruePBR.cpp).
				const bool pbr = a_property.flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kVertexLighting) &&
				                 (feature == RE::BSShaderMaterial::Feature::kDefault || feature == RE::BSShaderMaterial::Feature::kMultiTexLandLODBlend);
				r.glints = pbr && static_cast<const BSLightingShaderMaterialPBR*>(a_property.material)->glintParameters.enabled;
				// The land blend's material (WriteObjectExtras: techniques 8 and 19, the landscape materials).
				if (feature == RE::BSShaderMaterial::Feature::kMultiTexLand || feature == RE::BSShaderMaterial::Feature::kMultiTexLandLODBlend) {
					const auto& blend = static_cast<const RE::BSLightingShaderMaterialLandscape*>(material)->landBlendParams;
					r.landBlend = { blend.red, blend.green, blend.blue, blend.alpha };
				}
			}
		}
		return r;
	}

	AlphaRecord CaptureAlpha(const RE::NiAlphaProperty& a_alpha)
	{
		EngineReadWindow::Touch("SceneCapture::CaptureAlpha");
		AlphaRecord r;
		r.key = &a_alpha;
		r.flags = a_alpha.alphaFlags;
		r.threshold = a_alpha.alphaThreshold;
		r.rtti = const_cast<RE::NiAlphaProperty&>(a_alpha).GetRTTI();
		r.controllers = const_cast<RE::NiAlphaProperty&>(a_alpha).GetControllers() != nullptr;
		return r;
	}

	void CaptureLeaf(const RE::BSGeometry& a_geometry, Records& a_out, std::vector<RE::NiPointer<RE::NiRefObject>>* a_pins)
	{
		a_out.nodes.push_back(CaptureNode(a_geometry));
		a_out.geometries.push_back(CaptureGeometry(a_geometry));
		const auto& g = a_out.geometries.back();
		if (a_pins)
			for (const void* property : { g.property, g.layerProperty, g.alpha })
				if (property)
					a_pins->emplace_back(static_cast<RE::NiRefObject*>(const_cast<void*>(property)));
		// T6b2a: a Lighting property's material, for the scene work's records (the property holds it here): an attach's or a swap's
		// (a_pins). A material shared by several leaves is pushed by each; the scene work keeps the newest.
		auto pushMaterial = [&](const PropertyRecord& a_property) {
			if (a_pins && a_property.lighting && a_property.material)
				MaterialPort::PushCapture(static_cast<const RE::BSShaderMaterial*>(a_property.material));
		};
		if (g.property) {
			a_out.properties.push_back(CaptureProperty(*static_cast<const RE::BSShaderProperty*>(g.property)));
			pushMaterial(a_out.properties.back());
		}
		if (g.layerProperty) {
			a_out.properties.push_back(CaptureProperty(*static_cast<const RE::BSShaderProperty*>(g.layerProperty)));
			pushMaterial(a_out.properties.back());
		}
		if (g.alpha)
			a_out.alphas.push_back(CaptureAlpha(*static_cast<const RE::NiAlphaProperty*>(g.alpha)));
	}

	LiveLeaf::LiveLeaf(const RE::BSGeometry& a_geometry)
	{
		EngineReadWindow::Touch("SceneCapture::LiveLeaf");
		CaptureLeaf(a_geometry, records);
		const auto& g = records.geometries.back();
		const RE::NiAVObject* parent = a_geometry.parent;
		const auto* property = static_cast<const RE::BSShaderProperty*>(g.property);
		const RE::NiAVObject* fadeNode = property ? property->fadeNode : nullptr;
		const auto* layer = static_cast<const RE::BSShaderProperty*>(g.Layer());
		const RE::NiAVObject* layerFadeNode = layer ? layer->fadeNode : nullptr;
		if (parent)
			records.nodes.push_back(CaptureNode(*parent));
		if (fadeNode)
			records.nodes.push_back(CaptureNode(*fadeNode));
		if (layerFadeNode)
			records.nodes.push_back(CaptureNode(*layerFadeNode));
		// The view, once the records hold still.
		view.node = &records.nodes[0];
		view.parent = parent ? &records.nodes[1] : nullptr;
		view.fadeNode = fadeNode ? &records.nodes[parent ? 2 : 1] : nullptr;
		view.layerFadeNode = layerFadeNode ? &records.nodes.back() : nullptr;
		view.geometry = &g;
		for (const auto& p : records.properties) {
			view.property = !view.property && p.key == g.property ? &p : view.property;
			view.layer = p.key == g.Layer() ? &p : view.layer;
		}
		view.alpha = records.alphas.empty() ? nullptr : &records.alphas.front();
	}

	namespace
	{
		std::atomic<const std::vector<const RE::NiAVObject*>*> drawnRoots{ nullptr };
		std::vector<std::unique_ptr<const std::vector<const RE::NiAVObject*>>> drawnRootSets;  // render thread: every set published
	}

	void SetDrawnRoots(std::vector<const RE::NiAVObject*> a_roots)
	{
		std::ranges::sort(a_roots);
		if (const auto* current = drawnRoots.load(std::memory_order_acquire); current ? *current == a_roots : a_roots.empty())
			return;
		auto set = std::make_unique<const std::vector<const RE::NiAVObject*>>(std::move(a_roots));
		drawnRoots.store(set.get(), std::memory_order_release);
		drawnRootSets.push_back(std::move(set));
	}

	bool InWorld(const RE::NiAVObject* a_object)
	{
		EngineReadWindow::Touch("SceneCapture::InWorld");
		const auto* root = static_cast<const RE::NiAVObject*>(RE::Main::WorldRootNode());
		const RE::NiAVObject* top = nullptr;
		for (std::uint32_t depth = 0; root && a_object && depth <= kMaxDepth; ++depth, a_object = a_object->parent) {
			if (a_object == root)
				return true;
			top = a_object;
		}
		// A portal graph's parentless root (SetDrawnRoots).
		if (const auto* roots = drawnRoots.load(std::memory_order_acquire); roots && top && !top->parent)
			return std::ranges::binary_search(*roots, top);
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
		out->sequence = NextSequence();
		out->pinning = true;
		Visit(a_root, *out);
		// The ancestors: their children lists changed (the parent's), and a record is wanted for every node a chain walks.
		for (const auto* ancestor = a_root.parent; ancestor; ancestor = ancestor->parent) {
			out->nodes.push_back(CaptureNode(*ancestor));
			out->pins.emplace_back(const_cast<RE::NiNode*>(ancestor));
		}
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

	std::uint64_t NextSequence()
	{
		return sequence.fetch_add(1, std::memory_order_acq_rel) + 1;
	}

	void SetFrame(std::uint32_t a_frame)
	{
		currentFrame.store(a_frame, std::memory_order_relaxed);
	}

	std::uint32_t Frame()
	{
		return currentFrame.load(std::memory_order_relaxed);
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
