#include "GrassCapture.h"

#include "GrassMeshLibrary.h"
#include "TruePBR/BSLightingShaderMaterialPBR.h"
#include "Utils/VersionedRelocation.h"

namespace GrassShared
{
	namespace
	{
		constexpr uint32_t kMaxSinks = 2;

		// Filled before InstallCapture, during PostPostLoad, then only read; the count publishes each entry.
		std::array<Sink, kMaxSinks> sinks{};
		std::atomic<uint32_t> sinkCount{ 0 };

		std::atomic<uint64_t> nextSeq{ 1 };
		std::atomic<RE::BSShader*> grassShader{ nullptr };

		uint64_t TakeSeq() { return nextSeq.fetch_add(1, std::memory_order_relaxed); }

		bool IsGrassProperty(const RE::BSShaderProperty* prop)
		{
			return prop && prop->GetRTTI() == globals::rtti::BSGrassShaderPropertyRTTI.get();
		}

		void Reject(RE::BSMultiStreamInstanceTriShape* shape)
		{
			const uint64_t seq = TakeSeq();
			const uint32_t n = sinkCount.load(std::memory_order_acquire);
			for (uint32_t i = 0; i < n; ++i)
				if (sinks[i].rejected)
					sinks[i].rejected(shape, seq);
		}

		/** @brief Reads TruePBR's grass material, which SetupGrassMaterial interned when it set kMenuScreen. */
		void ReadPbr(AddEvent::Pbr& out, RE::BSShaderMaterial* material)
		{
			const auto* pbr = static_cast<const BSLightingShaderMaterialPBR*>(material);
			const RE::NiSourceTexture* white = globals::game::graphicsState->GetRuntimeData().defaultTextureWhite.get();

			out.normal = pbr->normalTexture.get();
			out.rmaos = pbr->rmaosTexture.get();
			out.features0 = (pbr->featuresTexture0 && pbr->featuresTexture0.get() != white) ? pbr->featuresTexture0.get() : nullptr;

			stl::enumeration<PBRShaderFlags> shaderFlags;
			if (pbr->pbrFlags.any(PBRFlags::Subsurface))
				shaderFlags.set(PBRShaderFlags::Subsurface);
			if (out.features0)
				shaderFlags.set(PBRShaderFlags::HasFeaturesTexture0);
			out.flags = shaderFlags.underlying();

			out.roughnessScale = pbr->GetRoughnessScale();
			out.specularLevel = pbr->GetSpecularLevel();
			const auto& subsurface = pbr->GetSubsurfaceColor();
			out.subsurface[0] = subsurface.red;
			out.subsurface[1] = subsurface.green;
			out.subsurface[2] = subsurface.blue;
			out.subsurface[3] = pbr->GetSubsurfaceOpacity();
		}

		/** @brief Builds one shape's event from its records and the shape's state now, and hands it to every sink. */
		void Capture(RE::BSMultiStreamInstanceTriShape* shape, const void* src, uint32_t count, uint32_t stride, AddEvent::Source source)
		{
			auto& geometry = shape->GetGeometryRuntimeData();
			auto* prop = static_cast<RE::BSGrassShaderProperty*>(geometry.shaderProperty.get());
			const uint64_t descVal = *reinterpret_cast<const uint64_t*>(&geometry.vertexDesc);

			// Without a texture the engine's own path draws the shape; nothing to capture.
			RE::NiSourceTexture* tex = prop->GetBaseTexture();
			if (!tex) {
				Reject(shape);
				return;
			}

			if (!src || !count || stride != kGrassStride) {
				logger::debug("[GRASS OPTIMIZATIONS] capture rejected: count={} stride={} desc={:016X} shape={:p}", count, stride, descVal, (void*)shape);
				// An empty shape draws nothing either way; only a record layout this cannot read is a rejection.
				if (count && stride != kGrassStride)
					Reject(shape);
				return;
			}

			auto* material = prop->material;
			if (!material) {
				Reject(shape);
				return;
			}

			AddEvent e;
			e.seq = TakeSeq();
			e.shape = shape;
			e.records = static_cast<const uint8_t*>(src);
			e.count = count;
			e.vertexDesc = descVal;
			e.origin = shape->world.translate;
			e.property = prop;
			e.material = material;
			e.diffuse = tex;
			if (auto* rendererData = geometry.rendererData) {
				e.meshKey = rendererData;
				e.vertexBuffer = reinterpret_cast<ID3D11Buffer*>(rendererData->vertexBuffer);
				e.indexBuffer = reinterpret_cast<ID3D11Buffer*>(rendererData->indexBuffer);
			}
			e.triangleCount = (uint32_t)shape->GetTrishapeRuntimeData().triangleCount;
			e.modelBound = shape->GetModelData().modelBound;
			e.wavePeriod = prop->wavePeriod;
			e.clampMode = (uint32_t)static_cast<RE::BSLightingShaderMaterialBase*>(material)->textureClampMode;
			e.pbr = prop->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kMenuScreen);
			e.sphereNormal = prop->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kEffectLighting);
			e.twoSided = prop->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kTwoSided);
			e.uniformScale = prop->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kUniformScale);
			if (const auto* alpha = geometry.alphaProperty.get())
				e.alphaRef = static_cast<float>(static_cast<int>(static_cast<float>(alpha->alphaThreshold) * prop->alpha)) / 255.0f;
			if (e.pbr)
				ReadPbr(e.pbrData, material);
			e.source = source;

			const uint32_t n = sinkCount.load(std::memory_order_acquire);
			for (uint32_t i = 0; i < n; ++i)
				if (sinks[i].add)
					sinks[i].add(e);
		}

		size_t GIDGroupBytes(const RE::BSMultiStreamInstanceTriShape::GroupHeader* header)
		{
			if (!header || !header->numShortsPerInstance)
				return 0;
			return (size_t)header->groupInstanceCount * header->numShortsPerInstance * sizeof(std::uint16_t);
		}

		/** @brief Captures one GID group's instance records from the cell-load hooks. */
		void CaptureGIDGroup(RE::BSMultiStreamInstanceTriShape* shape, RE::BSMultiStreamInstanceTriShape::GroupHeader* header, const uint16_t* instanceData, size_t dataBytes, AddEvent::Source source)
		{
			if (!shape || !header || !instanceData)
				return;

			if (!IsGrassProperty(shape->GetGeometryRuntimeData().shaderProperty.get()))
				return;

			uint32_t count = header->groupInstanceCount;
			if (dataBytes / kGrassStride < count)
				count = (uint32_t)(dataBytes / kGrassStride);

			Capture(shape, instanceData, count, kGrassStride, source);
		}

		// The .cgid readers fill these while AddGroup(Queued)GIDFile runs; the file thunks capture them after it returns.
		thread_local RE::BSMultiStreamInstanceTriShape::GroupHeader tl_lastFileGroupHeader{};
		thread_local std::vector<uint16_t> tl_lastFileInstanceData;
		thread_local bool tl_haveFileGroup = false;

		struct Hooks
		{
			struct BSMultiStreamInstanceTriShape_dtor
			{
				static void thunk(RE::BSMultiStreamInstanceTriShape* This)
				{
					const uint64_t seq = TakeSeq();
					const uint32_t n = sinkCount.load(std::memory_order_acquire);
					for (uint32_t i = 0; i < n; ++i)
						if (sinks[i].remove)
							sinks[i].remove(This, seq);

					// Forgotten here, before the address can be reused, so a later shape at it resolves its own mesh.
					MeshLibrary().ForgetShape(This);
					func(This);
				}
				static inline REL::Relocation<decltype(thunk)> func;
			};

			struct DoneAddingInstances
			{
				static void thunk(RE::BSMultiStreamInstanceTriShape* shape, RE::BSTArray<std::uint32_t>& a_instances)
				{
					auto& rt = shape->GetMultiStreamTrishapeRuntimeData();
					if (rt.groupAlloc && IsGrassProperty(shape->GetGeometryRuntimeData().shaderProperty.get()))
						Capture(shape, rt.groupAlloc, rt.instanceCount, 2u * rt.instanceSize, AddEvent::Source::Runtime);
					func(shape, a_instances);
				}
				static inline REL::Relocation<decltype(thunk)> func;
			};

			struct AddQueuedGroupGIDBuffer
			{
				static std::uint32_t thunk(RE::BSMultiStreamInstanceTriShape* a1, RE::BSMultiStreamInstanceTriShape::GroupHeader* a2, std::uint16_t* a3, RE::BSTArray<std::uint32_t>& a4)
				{
					CaptureGIDGroup(a1, a2, a3, GIDGroupBytes(a2), AddEvent::Source::GidBuffer);
					return func(a1, a2, a3, a4);
				}
				static inline REL::Relocation<decltype(thunk)> func;
			};

			struct AddGroupGIDBuffer
			{
				static std::uint32_t thunk(RE::BSMultiStreamInstanceTriShape* a1, RE::BSMultiStreamInstanceTriShape::GroupHeader* a2, std::uint16_t* a3)
				{
					CaptureGIDGroup(a1, a2, a3, GIDGroupBytes(a2), AddEvent::Source::GidBuffer);
					return func(a1, a2, a3);
				}
				static inline REL::Relocation<decltype(thunk)> func;
			};

			struct ReadGroupHeaderStreamTraits
			{
				static void thunk(RE::BSStreamHeader* streamHeader, RE::BSMultiStreamInstanceTriShape::GroupHeader* groupHeader, uint32_t size)
				{
					func(streamHeader, groupHeader, size);
					std::memcpy(&tl_lastFileGroupHeader, groupHeader, std::min<uint32_t>(size, sizeof(tl_lastFileGroupHeader)));
				}
				static inline REL::Relocation<decltype(thunk)> func;
			};

			struct ReadInstanceGroupStreamTraits
			{
				static void thunk(RE::BSStreamHeader* streamHeader, uint16_t* instanceData, uint32_t size)
				{
					func(streamHeader, instanceData, size);
					tl_lastFileInstanceData.resize((size + sizeof(uint16_t) - 1) / sizeof(uint16_t));
					std::memcpy(tl_lastFileInstanceData.data(), instanceData, size);
					tl_haveFileGroup = true;
				}
				static inline REL::Relocation<decltype(thunk)> func;
			};

			/** @brief Captures the group the .cgid readers stashed while a file thunk's call ran. */
			static void CaptureFileGroup(RE::BSMultiStreamInstanceTriShape* shape)
			{
				if (tl_haveFileGroup) {
					CaptureGIDGroup(shape, &tl_lastFileGroupHeader,
						tl_lastFileInstanceData.data(), tl_lastFileInstanceData.size() * sizeof(uint16_t), AddEvent::Source::GidFile);
					tl_haveFileGroup = false;
				}
			}

			struct AddGroupQueuedGIDFile
			{
				static void thunk(RE::BSMultiStreamInstanceTriShape* a1, RE::BSStream* a2, RE::BSTArray<std::uint32_t>& a3)
				{
					tl_haveFileGroup = false;
					func(a1, a2, a3);
					CaptureFileGroup(a1);
				}
				static inline REL::Relocation<decltype(thunk)> func;
			};

			struct AddGroupGIDFile
			{
				static void thunk(RE::BSMultiStreamInstanceTriShape* a1, RE::BSStream* a2)
				{
					tl_haveFileGroup = false;
					func(a1, a2);
					CaptureFileGroup(a1);
				}
				static inline REL::Relocation<decltype(thunk)> func;
			};

			struct LoadGrassType
			{
				static RE::BSMultiStreamInstanceTriShape* thunk(RE::BGSGrassManager* grassManager, RE::GrassParam* a_param, uint32_t CellXDivided, uint32_t CellYDivided, uint64_t* typeKey, RE::BSFixedString* modelPath)
				{
					auto* shape = func(grassManager, a_param, CellXDivided, CellYDivided, typeKey, modelPath);

					if (shape && modelPath)
						MeshLibrary().RecordModelPath(shape, modelPath->c_str());

					return shape;
				}
				static inline REL::Relocation<decltype(thunk)> func;
			};

			// An observer only: records the grass shader once, and leaves the passes as TruePBR's hook of the same slot set them.
			struct BSGrassShaderProperty_GetRenderPasses
			{
				static RE::BSShaderProperty::RenderPassArray* thunk(RE::BSLightingShaderProperty* property, RE::BSGeometry* geometry, std::uint32_t renderFlags, RE::BSShaderAccumulator* accumulator)
				{
					auto* renderPasses = func(property, geometry, renderFlags, accumulator);
					if (renderPasses && !grassShader.load(std::memory_order_relaxed)) {
						for (auto* pass = renderPasses->head; pass != nullptr; pass = pass->next) {
							if (pass->shader && pass->shader->shaderType == RE::BSShader::Type::Grass) {
								grassShader.store(pass->shader, std::memory_order_relaxed);
								break;
							}
						}
					}
					return renderPasses;
				}
				static inline REL::Relocation<decltype(thunk)> func;
			};

			static void Install()
			{
				stl::write_vfunc<0x0, BSMultiStreamInstanceTriShape_dtor>(RE::VTABLE_BSMultiStreamInstanceTriShape[0]);
				stl::write_vfunc<0x3A, DoneAddingInstances>(RE::VTABLE_BSMultiStreamInstanceTriShape[0]);
				stl::write_vfunc<0x2A, BSGrassShaderProperty_GetRenderPasses>(RE::VTABLE_BSGrassShaderProperty[0]);

				// Capture raw instance data for cached grass.
				stl::write_thunk_call<AddQueuedGroupGIDBuffer>(REL::RelocationID(15205, 15373).address() + Util::VersionedRelocation::Select(0x7FF, 0x756, 0x768));
				stl::write_thunk_call<AddGroupGIDBuffer>(REL::RelocationID(15205, 15373).address() + Util::VersionedRelocation::Select(0x806, 0x75D, 0x76F));
				stl::write_thunk_call<ReadGroupHeaderStreamTraits>(REL::RelocationID(74599, 76327).address() + Util::VersionedRelocation::Select(0x36, 0x36, 0x45));
				stl::write_thunk_call<ReadGroupHeaderStreamTraits>(REL::RelocationID(74596, 76324).address() + Util::VersionedRelocation::Select(0x2F, 0x33, 0x42));
				stl::write_thunk_call<ReadInstanceGroupStreamTraits>(REL::RelocationID(74607, 76339).address() + REL::Relocate(0xCF, 0xCF));
				stl::write_thunk_call<AddGroupQueuedGIDFile>(REL::RelocationID(15206, 15374).address() + REL::Relocate(0x394, 0x384));
				stl::write_thunk_call<AddGroupGIDFile>(REL::RelocationID(15206, 15374).address() + REL::Relocate(0x39B, 0x38B));

				// Record each grass type's source .nif path alongside its shape.
				stl::write_thunk_call<LoadGrassType>(REL::RelocationID(15204, 15372).address() + Util::VersionedRelocation::Select(0x2F5, 0x2F5, 0x305));
				stl::write_thunk_call<LoadGrassType>(REL::RelocationID(15205, 15373).address() + Util::VersionedRelocation::Select(0x62B, 0x597, 0x590));
				stl::write_thunk_call<LoadGrassType>(REL::RelocationID(15206, 15374).address() + REL::Relocate(0x25C, 0x25C));

				logger::info("[GRASS SHARED] Installed capture hooks");
			}
		};
	}

	void AddSink(const Sink& sink)
	{
		const uint32_t n = sinkCount.load(std::memory_order_relaxed);
		if (n >= kMaxSinks) {
			logger::error("[GRASS SHARED] sink rejected: all {} slots are taken", kMaxSinks);
			return;
		}
		sinks[n] = sink;
		sinkCount.store(n + 1, std::memory_order_release);
	}

	void InstallCapture()
	{
		static std::once_flag once;
		std::call_once(once, [] { Hooks::Install(); });
	}

	RE::BSShader* GrassShader()
	{
		return grassShader.load(std::memory_order_relaxed);
	}
}
