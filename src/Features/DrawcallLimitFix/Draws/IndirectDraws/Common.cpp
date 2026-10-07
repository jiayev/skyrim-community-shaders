#if defined(CS_HAS_RENDER_GRAPH) && defined(CS_HAS_ORG_MODULE_SERVICES)
#	include "Internal.h"

namespace DCLF::Draws
{
	std::string FrameConstantsPart(std::uint64_t a_offset)
	{
		const auto slot = static_cast<std::uint32_t>(a_offset / kFrameSlotBytes);
		const std::uint64_t within = a_offset % kFrameSlotBytes;
		if (slot == kFrameFogRegister)
			return within < sizeof(FrameFog) ? "frame constants VS b13 (fog)" : "frame constants VS b13 (extras frame)";
		if (slot < kConstantBufferRegisters)
			return fmt::format("frame constants VS b{}", slot);
		if (slot < 2 * kConstantBufferRegisters)
			return fmt::format("frame constants PS b{}", slot - kConstantBufferRegisters);
		if (slot == kFrameSlotSharedLight)
			return "frame constants: zeroed light block";
		if (slot == kFrameSlotLighting)
			return within < sizeof(FrameLighting)                                   ? "frame constants: frame lighting" :
			       within < sizeof(FrameLighting) + sizeof(LodFadeFrame)           ? "frame constants: LOD fade frame" :
			       within < kExtrasPixelFrameOffset % kFrameSlotBytes               ? "frame constants: foliage parity" :
			                                                                         "frame constants PS b13 (extras frame)";
		if (slot == kFrameSlotRecord)
			return "frame constants: frame record";
		return "frame constants: ?";
	}

	void CheckBindlessRecord(const SceneStore::Tables& a_tables, std::uint32_t a_objectIndex, const BindlessObject& a_record,
		const BindlessPlacement& a_placement, const BindlessShading& a_shading, const RE::NiPoint3& a_eye,
		const RE::NiPoint3& a_previousEye, const GeometryPatchOffsets& a_offsets, std::span<const std::byte> a_vs, std::span<const std::byte> a_ps,
		IndirectDraws::Stats& a_stats)
	{
		auto compare = [&](std::span<const std::byte> a_group, std::uint32_t a_offset, std::uint32_t a_count, const float* a_expected, const char* a_name) {
			if (a_offset == ~0u)
				return;  // the pipeline's shaders do not declare it, so neither form carries a value
			for (std::uint32_t c = 0; c < a_count; ++c) {
				const std::size_t at = (std::size_t(a_offset) + c) * 4;
				if (at + 4 > a_group.size())
					return;
				float packed = 0.0f;
				std::memcpy(&packed, a_group.data() + at, 4);
				++a_stats.bindlessParityChecks;
				if (std::bit_cast<std::uint32_t>(packed) == std::bit_cast<std::uint32_t>(a_expected[c]))
					continue;
				if (a_stats.bindlessParityMismatches++ == 0)
					logger::warn("[DCLF] bindless record parity: {}[{}] is {} in the record and {} in the constant group", a_name, c, a_expected[c], packed);
			}
		};
		// The placement is absolute; the shader makes it relative with the same single subtraction as this.
		float relative[16] = {};
		StoreRelativeTo(relative, a_placement.world, a_eye);
		compare(a_vs, a_offsets.vsWorld, std::min(a_offsets.vsWorldSize, 12u), relative, "World");
		StoreRelativeTo(relative, a_placement.previousWorld, a_previousEye);
		compare(a_vs, a_offsets.vsPreviousWorld, std::min(a_offsets.vsPreviousWorldSize, 12u), relative, "PreviousWorld");
		compare(a_ps, a_offsets.psMaterialData, std::min(a_offsets.psMaterialDataSize, 4u), a_shading.shading.materialData, "MaterialData");
		compare(a_ps, a_offsets.psEmitColor, std::min(a_offsets.psEmitColorSize, 3u), a_shading.shading.emitColor, "EmitColor");
		if (a_offsets.psSSRParams != ~0u && a_offsets.psSSRParamsSize > 3)
			compare(a_ps, a_offsets.psSSRParams + 3, 1, &a_shading.shading.ssrSpecular, "SSRParams.w");

		// The tail has no constant group of its own to compare against (it is the record's alone), so it is checked
		// against the values the engine's buffers would hold, derived from the tables.
		const auto& object = a_tables.objects[a_objectIndex];
		const auto& lights = a_tables.lights[a_objectIndex];
		auto expect = [&](bool a_equal, const char* a_name) {
			++a_stats.bindlessParityChecks;
			if (!a_equal && a_stats.bindlessParityMismatches++ == 0)
				logger::warn("[DCLF] bindless record parity: {} differs for object {}", a_name, a_objectIndex);
		};
		expect(a_record.roomIndex == lights.roomIndex, "RoomIndex");
		expect(a_record.recordFlags == (((object.flags & kObjectBeastRace) ? kRecordBeastRace : 0u) | ((object.flags & kObjectAlphaBlended) ? kRecordAlphaBlended : 0u)),
			"RecordFlags");
		const float threshold = (object.flags & kObjectAlphaTest) ? ((object.flags >> kObjectAlphaThresholdShift) & 0xFF) / 255.0f : 0.0f;
		expect(std::bit_cast<std::uint32_t>(a_record.alphaTestRef) == std::bit_cast<std::uint32_t>(threshold), "AlphaTestRef");
		const bool skinned = (object.flags & kObjectSkinned) && a_objectIndex < a_tables.boneOffset.size();
		const auto palette = skinned ? PaletteRowsOf(a_tables.boneOffset[a_objectIndex], a_tables.boneRows[a_objectIndex]) : PaletteRows{};
		expect(a_record.boneOffset == palette.current, "BoneOffset");
		expect(a_record.boneRows == (skinned ? a_tables.boneRows[a_objectIndex] : 0u), "BoneRows");
		expect(a_record.previousBoneOffset == palette.previous, "PreviousBoneOffset");
		const bool extras = a_objectIndex < a_tables.extraOffset.size() && a_tables.extraOffset[a_objectIndex] != kNoExtraRows;
		expect(a_record.extraOffset == (extras ? a_tables.extraOffset[a_objectIndex] : 0u), "ExtraOffset");
	}

	Capture CaptureBindings()
	{
		RenderThreadBudget::Part budget(RenderThreadBudget::Bucket::Capture);
		auto* context = globals::d3d::context;
		Capture capture;
		context->VSGetConstantBuffers(0, kConstantBufferRegisters, capture.vsBuffers.data());
		context->PSGetConstantBuffers(0, kConstantBufferRegisters, capture.psBuffers.data());
		context->PSGetShaderResources(0, kTextureRegisters, capture.psViews.data());
		ID3D11RenderTargetView* views[kColorTargets] = {};
		ID3D11DepthStencilView* depth = nullptr;
		context->OMGetRenderTargets(kColorTargets, views, &depth);
		for (std::uint32_t i = 0; i < kColorTargets; ++i) {
			if (!views[i])
				continue;
			winrt::com_ptr<ID3D11Resource> resource;
			views[i]->GetResource(resource.put());
			capture.targets[i] = resource.try_as<ID3D11Texture2D>();
			capture.targetCount = i + 1;
			views[i]->Release();
		}
		if (depth) {
			winrt::com_ptr<ID3D11Resource> resource;
			depth->GetResource(resource.put());
			capture.depth = resource.try_as<ID3D11Texture2D>();
			depth->Release();
		}
		// Community Shaders binds its own per-frame pixel buffers - b5 SharedData and b6 FeatureData -
		// from Renderer_ResetState, so whether they happen to be bound when this capture is taken
		// depends on where the engine last reset its state. Skipping DCLF's objects' native draws changes
		// that, and the objects whose shaders read b5 were then dropped for
		// missing constants and rendered untextured. They are CS's own buffers with known identities,
		// so take them from CS instead of from whatever is bound.
		if (auto* state = globals::state) {
			auto adopt = [&](std::uint32_t a_slot, ConstantBuffer* a_buffer) {
				if (capture.psBuffers[a_slot] || !a_buffer || !a_buffer->CB())
					return;
				capture.psBuffers[a_slot] = a_buffer->CB();
				capture.psBuffers[a_slot]->AddRef();  // Capture::Release owns what it holds
			};
			adopt(kSharedDataRegister, state->sharedDataCB);
			adopt(kFeatureDataRegister, state->featureDataCB);
		}
		auto& shadowState = globals::game::shadowState->GetRuntimeData();
		capture.eye = shadowState.posAdjust.getEye();
		capture.previousEye = shadowState.previousPosAdjust.getEye();
		D3D11_VIEWPORT viewport{};
		UINT count = 1;
		context->RSGetViewports(&count, &viewport);
		capture.viewportWidth = static_cast<std::uint32_t>(viewport.Width);
		capture.viewportHeight = static_cast<std::uint32_t>(viewport.Height);
		capture.minDepth = viewport.MinDepth;
		capture.maxDepth = viewport.MaxDepth;
		return capture;
	}

	std::vector<FrameBuffer> FrameBuffersOf(const Capture& a_capture, bool a_lightLimitFix)
	{
		std::vector<FrameBuffer> result;
		for (std::uint32_t t = kPixelTextureSlots; t < kTextureRegisters; ++t) {
			auto* view = a_capture.psViews[t];
			if (!view || (a_lightLimitFix && t >= kLightsRegister && t < kLightsRegister + 3))
				continue;
			D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{};
			view->GetDesc(&viewDesc);
			if (viewDesc.ViewDimension != D3D11_SRV_DIMENSION_BUFFER && viewDesc.ViewDimension != D3D11_SRV_DIMENSION_BUFFEREX)
				continue;
			winrt::com_ptr<ID3D11Resource> resource;
			view->GetResource(resource.put());
			auto buffer = resource.try_as<ID3D11Buffer>();
			D3D11_BUFFER_DESC bufferDesc{};
			buffer->GetDesc(&bufferDesc);
			if (!(bufferDesc.CPUAccessFlags & D3D11_CPU_ACCESS_WRITE) || !bufferDesc.StructureByteStride)
				continue;  // only CPU-written structured buffers are copied
			FrameBuffer frameBuffer;
			frameBuffer.textureRegister = t;
			frameBuffer.stride = bufferDesc.StructureByteStride;
			frameBuffer.firstElement = viewDesc.ViewDimension == D3D11_SRV_DIMENSION_BUFFER ? viewDesc.Buffer.FirstElement : viewDesc.BufferEx.FirstElement;
			frameBuffer.elements = viewDesc.ViewDimension == D3D11_SRV_DIMENSION_BUFFER ? viewDesc.Buffer.NumElements : viewDesc.BufferEx.NumElements;
			result.push_back(frameBuffer);
		}
		return result;
	}
}

#endif
