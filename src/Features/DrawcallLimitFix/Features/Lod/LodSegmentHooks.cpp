// Object LOD's segment writers (Core/Engine/LodSegments.h): each patched call makes the write, then names the shape on the feature's engine
// channel (its queue, drained by the coordinator's passes: LodSegmentChannel.cpp) and pushes the mirror's update (Core's channel:
// Scene::PushGeometryUpdate). Installed on the render thread, at the feature's early hooks.

#include "Features/DrawcallLimitFix/Features/Lod/LodSegmentEvents.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Extensions/EngineEvents.h"
#include "Features/DrawcallLimitFix/Core/Runtime/EventQueue.h"
#include "Features/DrawcallLimitFix/Engine/Capture/SceneEvents.h"

namespace DCLF::Features::Lod
{
	namespace
	{
		// BSSubIndexTriShapes a segment write touched, as keys, pushed after the write. No wake: written as the camera moves, mostly
		// unchanged ranges; the next pass takes them.
		EventQueue<const void*> segmentEvents;
		bool segmentHooksInstalled = false;

		struct LodSegmentShow
		{
			static void thunk(RE::BSGeometry* a_shape, std::uint64_t a_segment)
			{
				func(a_shape, a_segment);
				segmentEvents.Push(a_shape);
				Scene::PushGeometryUpdate(a_shape, SceneCapture::GeometryRecord::kSegments);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
		struct LodSegmentHide
		{
			static void thunk(RE::BSGeometry* a_shape, std::uint64_t a_segment)
			{
				func(a_shape, a_segment);
				segmentEvents.Push(a_shape);
				Scene::PushGeometryUpdate(a_shape, SceneCapture::GeometryRecord::kSegments);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
		struct LodSegmentShowAll
		{
			static void thunk(RE::BSGeometry* a_shape)
			{
				func(a_shape);
				segmentEvents.Push(a_shape);
				Scene::PushGeometryUpdate(a_shape, SceneCapture::GeometryRecord::kSegments);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		bool PatchSegmentWriters()
		{
			// Every xref of the three writers (AE 1.6.1170): the active and large-reference grids' segment updates (FUN_1405110b0,
			// FUN_1405112f0), two show-all callers (FUN_1404fdcf0, FUN_140509550), the LOD block builder (FUN_140e55790). All or none.
			constexpr std::uintptr_t kShow = 0xe31130, kHide = 0xe31160, kShowAll = 0xe310b0;
			constexpr std::uintptr_t kShowSites[] = { 0x511550, 0x511269 };
			constexpr std::uintptr_t kHideSites[] = { 0x511544, 0x51125c };
			constexpr std::uintptr_t kShowAllSites[] = { 0x4fde80, 0x509691, 0x511402, 0x5112b4, 0xe55ba3 };
			auto at = [](std::uintptr_t a_offset) { return REL::Offset(a_offset).address(); };
			bool ok = true;
			for (const auto site : kShowSites)
				ok = ok && Engine::CallsTo(at(site), at(kShow));
			for (const auto site : kHideSites)
				ok = ok && Engine::CallsTo(at(site), at(kHide));
			for (const auto site : kShowAllSites)
				ok = ok && Engine::CallsTo(at(site), at(kShowAll));
			if (!ok) {
				logger::warn("[DCLF] object LOD: a segment writer's call site differs; object LOD stays native");
				return false;
			}
			for (const auto site : kShowSites)
				stl::write_thunk_call<LodSegmentShow>(at(site));
			for (const auto site : kHideSites)
				stl::write_thunk_call<LodSegmentHide>(at(site));
			for (const auto site : kShowAllSites)
				stl::write_thunk_call<LodSegmentShowAll>(at(site));
			return true;
		}
	}

	void InstallSegmentHooks()
	{
		segmentHooksInstalled = PatchSegmentWriters();
	}

	bool SegmentHooksInstalled()
	{
		return segmentHooksInstalled;
	}

	std::size_t DrainSegmentEvents(Api::KeySink& a_sink)
	{
		return segmentEvents.Drain(a_sink);
	}
}
