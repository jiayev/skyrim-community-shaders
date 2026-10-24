#include "GrassCapture.h"

#include "GrassMeshLibrary.h"
#include "TruePBR/BSLightingShaderMaterialPBR.h"
#include "Utils/VersionedRelocation.h"

#include <thread>

namespace GrassShared
{
	namespace
	{
		constexpr uint32_t kMaxSinks = 2;

		// Filled before InstallCapture, during PostPostLoad, then only read; the count publishes each entry.
		std::array<Sink, kMaxSinks> sinks{};
		std::atomic<uint32_t> sinkCount{ 0 };

		// Every hook hands its event to the sinks under this lock, so each sink sees one order of a shape's lifecycle: a group's
		// removal is handed on before a later group can reuse its index. Recursive, in case an engine call made under it reaches
		// another hook.
		std::recursive_mutex lifecycleMutex;

		// The grass shapes LoadGrassType named, with the groups handed on of each (under lifecycleMutex): the attach scan's, which
		// finds the groups the engine attached from its queue without AddGroup.
		std::unordered_map<RE::BSMultiStreamInstanceTriShape*, std::unordered_set<uint32_t>> namedShapes;
		// Set by the queued producers (DoneAddingInstances' queued chunks, the queued GID buffer and file adds), taken by the attach.
		std::atomic<bool> pendingAttach{ false };

		std::atomic<uint64_t> nextSeq{ 1 };
		std::atomic<RE::BSShader*> grassShader{ nullptr };

		uint64_t TakeSeq() { return nextSeq.fetch_add(1, std::memory_order_relaxed); }

		template <class F>
		void ForEachSink(F&& f)
		{
			const uint32_t n = sinkCount.load(std::memory_order_acquire);
			for (uint32_t i = 0; i < n; ++i)
				f(sinks[i]);
		}

		bool IsGrassProperty(const RE::BSShaderProperty* prop)
		{
			return prop && prop->GetRTTI() == globals::rtti::BSGrassShaderPropertyRTTI.get();
		}

		/** @brief The engine's lock over a shape's instance groups, as Grass Optimizations' store takes it. */
		class EngineGroupLock
		{
		public:
			EngineGroupLock()
			{
				while (_InterlockedCompareExchange(word.get(), 1, 0))
					std::this_thread::yield();
			}
			~EngineGroupLock() { _InterlockedExchange(word.get(), 0); }
			EngineGroupLock(const EngineGroupLock&) = delete;
			EngineGroupLock& operator=(const EngineGroupLock&) = delete;

		private:
			inline static REL::Relocation<volatile long*> word{ REL::RelocationID(524610, 411250) };
		};

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

		/**
		 * @brief Reads one group's records and the shape's state now, under the engine's group lock, and hands it to the add sinks
		 * (or the rejected ones, when the engine draws a group this cannot read).
		 */
		void CaptureGroup(RE::BSMultiStreamInstanceTriShape* shape, uint32_t index, AddEvent::Source source)
		{
			bool wanted = false;
			ForEachSink([&](const Sink& sink) { wanted |= sink.add || sink.rejected; });
			if (!wanted)
				return;

			auto& geometry = shape->GetGeometryRuntimeData();
			auto* prop = static_cast<RE::BSGrassShaderProperty*>(geometry.shaderProperty.get());
			const uint64_t descVal = *reinterpret_cast<const uint64_t*>(&geometry.vertexDesc);

			EngineGroupLock engineLock;
			const auto& groups = shape->GetMultiStreamTrishapeRuntimeData().instanceGroups;
			const auto* group = index < groups.size() ? groups[index] : nullptr;
			if (!group || !group->instanceCount)
				return;
			const auto* vertexBuffer = group->vertexBuffer;
			const uint32_t count = group->instanceCount;
			// The engine writes the width's low 32 bits only (the field is 64): the rest can be anything.
			const uint32_t stride = vertexBuffer ? static_cast<uint32_t>(vertexBuffer->byteWidth) / count : 0;

			auto* tex = prop->GetBaseTexture();
			auto* material = prop->material;
			auto* rendererData = geometry.rendererData;
			if (!vertexBuffer || !vertexBuffer->m_data || stride != kGrassStride || !tex || !material ||
				material->GetType() != RE::BSShaderMaterial::Type::kLighting || !rendererData || !rendererData->vertexBuffer || !rendererData->indexBuffer) {
				logger::debug("[GRASS SHARED] group rejected: count={} stride={} desc={:016X} shape={:p} group={}", count, stride, descVal, (void*)shape, index);
				const uint64_t seq = TakeSeq();
				ForEachSink([&](const Sink& sink) {
					if (sink.rejected)
						sink.rejected(shape, index, seq);
				});
				return;
			}

			AddEvent e;
			e.seq = TakeSeq();
			e.shape = shape;
			e.group = index;
			e.records = static_cast<const uint8_t*>(vertexBuffer->m_data);
			e.count = count;
			e.vertexDesc = descVal;
			e.origin = shape->world.translate;
			e.property = prop;
			e.material = material;
			e.diffuse = tex;
			e.meshKey = rendererData;
			e.vertexBuffer = reinterpret_cast<ID3D11Buffer*>(rendererData->vertexBuffer);
			e.indexBuffer = reinterpret_cast<ID3D11Buffer*>(rendererData->indexBuffer);
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

			ForEachSink([&](const Sink& sink) {
				if (sink.add)
					sink.add(e);
			});
		}

		/** @brief Hands a group the engine added to every sink: by index, then read. */
		void AddedGroup(RE::BSMultiStreamInstanceTriShape* shape, uint32_t index, AddEvent::Source source)
		{
			if (!shape || index == UINT32_MAX || !IsGrassProperty(shape->GetGeometryRuntimeData().shaderProperty.get()))
				return;

			std::scoped_lock lock(lifecycleMutex);
			if (const auto it = namedShapes.find(shape); it != namedShapes.end())
				it->second.insert(index);
			ForEachSink([&](const Sink& sink) {
				if (sink.group)
					sink.group(shape, index);
			});
			CaptureGroup(shape, index, source);
		}

		/**
		 * @brief After the engine attached its queued groups: the index sinks scan for theirs; the add sinks get every named shape's
		 * group not handed on yet, read.
		 */
		void AttachedGroups()
		{
			std::scoped_lock lock(lifecycleMutex);
			ForEachSink([&](const Sink& sink) {
				if (sink.attached)
					sink.attached();
			});

			std::vector<std::pair<RE::BSMultiStreamInstanceTriShape*, uint32_t>> attached;
			{
				EngineGroupLock engineLock;
				for (auto& [shape, known] : namedShapes) {
					const auto& data = shape->GetMultiStreamTrishapeRuntimeData();
					if (known.size() == data.activeGroupCount)
						continue;
					for (uint32_t index = 0; index < data.instanceGroups.size(); ++index)
						if (data.instanceGroups[index] && known.insert(index).second)
							attached.emplace_back(shape, index);
				}
			}
			for (const auto& [shape, index] : attached)
				CaptureGroup(shape, index, AddEvent::Source::Queued);
		}

		struct Hooks
		{
			struct BSMultiStreamInstanceTriShape_dtor
			{
				static void thunk(RE::BSMultiStreamInstanceTriShape* This)
				{
					{
						std::scoped_lock lock(lifecycleMutex);
						const uint64_t seq = TakeSeq();
						ForEachSink([&](const Sink& sink) {
							if (sink.remove)
								sink.remove(This, seq);
						});
						// Forgotten here, before the address can be reused, so a later shape at it resolves its own mesh.
						ForgetMesh(This);
						namedShapes.erase(This);
					}
					func(This);
				}
				static inline REL::Relocation<decltype(thunk)> func;
			};

			// DoneAddingInstances adds each chunk of generated instances through this virtual, which returns the group index.
			struct AddGroup
			{
				static std::uint32_t thunk(RE::BSMultiStreamInstanceTriShape* shape, std::uint32_t count, std::uint16_t* instanceData, std::uint32_t instanceShorts, float boundPadding)
				{
					const auto index = func(shape, count, instanceData, instanceShorts, boundPadding);
					AddedGroup(shape, index, AddEvent::Source::Runtime);
					return index;
				}
				static inline REL::Relocation<decltype(thunk)> func;
			};

			// Generated grass: each chunk through AddGroup, or (while the engine queues, outside grass cache generation) onto the attach
			// queue, which AttachQueuedGroups drains.
			struct DoneAddingInstances
			{
				static void thunk(RE::BSMultiStreamInstanceTriShape* shape, RE::BSTArray<std::uint32_t>& groupIndices)
				{
					pendingAttach.store(true, std::memory_order_release);
					func(shape, groupIndices);
					pendingAttach.store(true, std::memory_order_release);
				}
				static inline REL::Relocation<decltype(thunk)> func;
			};

			// Vanilla grass shapes are shared by every cell in a 12x12 block, so a cell unload removes its groups here.
			struct RemoveGroup
			{
				static void thunk(RE::BSMultiStreamInstanceTriShape* shape, std::uint32_t index)
				{
					std::scoped_lock lock(lifecycleMutex);
					func(shape, index);
					if (index == UINT32_MAX)
						return;
					if (const auto it = namedShapes.find(shape); it != namedShapes.end())
						it->second.erase(index);
					const uint64_t seq = TakeSeq();
					ForEachSink([&](const Sink& sink) {
						if (sink.removeGroup)
							sink.removeGroup(shape, index, seq);
					});
				}
				static inline REL::Relocation<decltype(thunk)> func;
			};

			struct AddGroupGIDBuffer
			{
				static std::uint32_t thunk(RE::BSMultiStreamInstanceTriShape* a1, RE::BSMultiStreamInstanceTriShape::GroupHeader* a2, std::uint16_t* a3)
				{
					const auto index = func(a1, a2, a3);
					AddedGroup(a1, index, AddEvent::Source::GidBuffer);
					return index;
				}
				static inline REL::Relocation<decltype(thunk)> func;
			};

			struct AddQueuedGroupGIDBuffer
			{
				static void thunk(RE::BSMultiStreamInstanceTriShape* shape, RE::BSMultiStreamInstanceTriShape::GroupHeader* header, std::uint16_t* instanceData, RE::BSTArray<std::uint32_t>& groupIndices)
				{
					pendingAttach.store(true, std::memory_order_release);
					func(shape, header, instanceData, groupIndices);
					pendingAttach.store(true, std::memory_order_release);
				}
				static inline REL::Relocation<decltype(thunk)> func;
			};

			struct AddQueuedGroupGIDFile
			{
				static void thunk(RE::BSMultiStreamInstanceTriShape* shape, RE::BSStream* stream, RE::BSTArray<std::uint32_t>& groupIndices)
				{
					pendingAttach.store(true, std::memory_order_release);
					func(shape, stream, groupIndices);
					pendingAttach.store(true, std::memory_order_release);
				}
				static inline REL::Relocation<decltype(thunk)> func;
			};

			// The engine's drain of its attach queue: each queued group into a free slot of its shape's groups, without AddGroup.
			struct AttachQueuedGroups
			{
				static void thunk()
				{
					const bool pending = pendingAttach.exchange(false, std::memory_order_acq_rel);
					func();
					// Work queued during the attach stays pending for the next one as well.
					if (pending || pendingAttach.load(std::memory_order_acquire))
						AttachedGroups();
				}
				static inline REL::Relocation<decltype(thunk)> func;
			};

			struct AddGroupGIDFile
			{
				static void thunk(RE::BSMultiStreamInstanceTriShape* a1, RE::BSStream* a2)
				{
					// The file loader does not return a group index, so identify the first changed slot.
					static thread_local std::vector<void*> groupsBefore;
					const auto& groups = a1->GetMultiStreamTrishapeRuntimeData().instanceGroups;
					groupsBefore.assign(groups.begin(), groups.end());

					func(a1, a2);

					uint32_t index = 0;
					while (index < groups.size() && (!groups[index] || (index < groupsBefore.size() && groups[index] == groupsBefore[index])))
						++index;

					if (index < groups.size())
						AddedGroup(a1, index, AddEvent::Source::GidFile);
				}
				static inline REL::Relocation<decltype(thunk)> func;
			};

			struct LoadGrassType
			{
				static RE::BSMultiStreamInstanceTriShape* thunk(RE::BGSGrassManager* grassManager, RE::GrassParam* a_param, uint32_t CellXDivided, uint32_t CellYDivided, uint64_t* typeKey, RE::BSFixedString* modelPath)
				{
					auto* shape = func(grassManager, a_param, CellXDivided, CellYDivided, typeKey, modelPath);

					if (shape && modelPath && modelPath->c_str() && *modelPath->c_str()) {
						std::scoped_lock lock(lifecycleMutex);
						// The engine hands every cell of a 12x12 block the same shape: only its first naming is news.
						const bool named = RecordMesh(shape, &MeshLibrary().RegisterMesh(modelPath->c_str()));
						ForEachSink([&](const Sink& sink) {
							if (sink.model)
								sink.model(shape, modelPath->c_str());
						});
						// The groups it holds at its first naming, read again for the add sinks (as the store's RegisterModel does for
						// its own), in case one was added before the shape was a grass shape; from here on the attach scan knows it.
						if (named && IsGrassProperty(shape->GetGeometryRuntimeData().shaderProperty.get())) {
							std::vector<uint32_t> indices;
							{
								EngineGroupLock engineLock;
								const auto& groups = shape->GetMultiStreamTrishapeRuntimeData().instanceGroups;
								for (uint32_t i = 0; i < groups.size(); ++i)
									if (groups[i])
										indices.push_back(i);
							}
							namedShapes[shape].insert(indices.begin(), indices.end());
							for (const uint32_t index : indices)
								CaptureGroup(shape, index, AddEvent::Source::Runtime);
						}
					}

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
				stl::write_vfunc<0x3C, AddGroup>(RE::VTABLE_BSMultiStreamInstanceTriShape[0]);
				stl::write_vfunc<0x3D, RemoveGroup>(RE::VTABLE_BSMultiStreamInstanceTriShape[0]);
				stl::write_vfunc<0x2A, BSGrassShaderProperty_GetRenderPasses>(RE::VTABLE_BSGrassShaderProperty[0]);

				// Queued groups (generated grass, queued cache groups) get their slots when the engine drains its attach queue.
				stl::write_thunk_call<AddQueuedGroupGIDBuffer>(REL::RelocationID(15205, 15373).address() + Util::VersionedRelocation::Select(0x7FF, 0x756, 0x768));
				stl::write_thunk_call<AddQueuedGroupGIDFile>(REL::RelocationID(15206, 15374).address() + REL::Relocate(0x394, 0x384));
				stl::detour_thunk<AttachQueuedGroups>(REL::RelocationID(74603, 76335));

				// Loaded cache groups.
				stl::write_thunk_call<AddGroupGIDBuffer>(REL::RelocationID(15205, 15373).address() + Util::VersionedRelocation::Select(0x806, 0x75D, 0x76F));
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
