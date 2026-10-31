// Object LOD's segment writers (LodSegments.h, SceneWriterFramework's LOD segment calls): after each write, the shape is named on the
// feature's engine channel (its queue, drained by the coordinator's passes: LodSegmentChannel.cpp) and the mirror's update pushed (Core's
// channel: Extensions::PushRangesUpdate). Subscribed on the render thread, at the feature's early hooks.

#include "Features/DrawcallLimitFix/Core/Engine/Capture/SceneWriter.h"
#include "Features/DrawcallLimitFix/Core/Extensions/EngineEvents.h"
#include "Features/DrawcallLimitFix/Core/Runtime/EventQueue.h"
#include "Features/DrawcallLimitFix/Features/Lod/LodSegmentEvents.h"

namespace DCLF::Features::Lod
{
	namespace
	{
		// BSSubIndexTriShapes a segment write touched, as keys, pushed after the write. No wake: written as the camera moves, mostly
		// unchanged ranges; the next pass takes them.
		EventQueue<const void*> segmentEvents;
		bool segmentHooksInstalled = false;

		// Show, hide and show-all (SWF_HOOK_LOD_SEGMENT_*): the shape is the first argument.
		void OnSegmentWrite(const SWF_EventHeader* a_event, std::uintptr_t*, void*)
		{
			auto* shape = SceneWriter::Arg<RE::BSGeometry*>(a_event, 0);
			segmentEvents.Push(shape);
			Extensions::PushRangesUpdate(shape);
		}

		bool SubscribeSegmentWriters()
		{
			// SceneWriterFramework's LOD segment calls: every call of the three writers (AE 1.6.1170), at its call sites. All or none.
			if (!SceneWriter::Has(SWF_CAP_LOD_SEGMENT_CALLS)) {
				logger::warn("[DCLF] object LOD: the scene writer framework has no LOD segment calls (a call site differs); object LOD stays native");
				return false;
			}
			for (const std::uint32_t hook : { SWF_HOOK_LOD_SEGMENT_SHOW, SWF_HOOK_LOD_SEGMENT_HIDE, SWF_HOOK_LOD_SEGMENT_SHOW_ALL })
				if (!SceneWriter::SubscribeCall(hook, &OnSegmentWrite, SWF_PHASE_POST))
					stl::report_and_fail("Drawcall Limit Fix: the scene writer framework refused a LOD segment writer after reporting it installed");
			return true;
		}
	}

	void InstallSegmentHooks()
	{
		segmentHooksInstalled = SubscribeSegmentWriters();
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
