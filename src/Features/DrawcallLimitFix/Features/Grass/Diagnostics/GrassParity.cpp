// CS_DCLF_PERSISTENT_PARITY: grass's mirror against the capture's live set on parity frames. The sink's observer keeps every captured
// shape the capture still holds (any thread, under a lock: parity only), with its record count and seq; a check compares it with the
// mirror's shapes - a live shape the mirror lacks or holds with another count, a mirrored shape no longer live. An event pushed but not
// drained yet makes a shape differ at the check: a shape that differs counts as late when its event arrives at the next drain, and as
// differing only when it does not.

#include "Features/DrawcallLimitFix/Features/Grass/Grass.h"
#include "Features/DrawcallLimitFix/Features/Grass/GrassMirror.h"
#include "Features/DrawcallLimitFix/Features/Grass/Diagnostics/GrassEvents.h"
#include "Features/DrawcallLimitFix/Features/Grass/Diagnostics/GrassParity.h"
#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"
#include "Features/DrawcallLimitFix/Diagnostics/Observation.h"

#include <mutex>

namespace DCLF::Features::Grass
{
	namespace
	{
		struct Live
		{
			std::uint32_t count = 0;
			std::uint64_t seq = 0;
		};

		// The capture's live grass shapes (the sink's threads write, the render thread's check reads).
		std::mutex liveLock;
		ankerl::unordered_dense::map<const void*, Live> live;

		void OnCaptured(const GrassCaptured& a_event)
		{
			std::scoped_lock lock(liveLock);
			const auto it = live.find(a_event.shape);
			if (it != live.end() && a_event.seq < it->second.seq)
				return;
			// Each add is one more group of the shape (the mirror's too); its removal takes them all.
			if (a_event.kind == GrassCaptured::Kind::Add) {
				auto& entry = live[a_event.shape];
				entry = { entry.count + a_event.count, a_event.seq };
			}
			else if (it != live.end())
				live.erase(it);
		}

		void CheckParity(const Mirror& a_mirror)
		{
			auto& stats = ParityState();
			auto& suspects = stats.suspects;
			const auto& shapes = a_mirror.MirroredShapes();
			++stats.checks;
			std::uint64_t liveInstances = 0;
			std::scoped_lock lock(liveLock);
			for (const auto& [shape, entry] : live) {
				++stats.shapes;
				liveInstances += entry.count;
				const auto it = shapes.find(shape);
				if (it == shapes.end())
					suspects[shape] = fmt::format("a captured shape ({} records) not mirrored", entry.count);
				else if (it->second.records != entry.count)
					suspects[shape] = fmt::format("a shape mirrored with {} records, captured with {}", it->second.records, entry.count);
			}
			// A mirrored shape the capture no longer holds: its removal was missed.
			for (const auto& [shape, mirrored] : shapes)
				if (!live.contains(shape))
					suspects[shape] = fmt::format("a mirrored shape ({} records) no longer captured", mirrored.records);
			stats.liveInstances = liveInstances;
			stats.mirroredInstances = a_mirror.InstanceCount();
		}

		void OnEventDrained(const GrassEventDrained& a_event)
		{
			auto& stats = ParityState();
			if (!stats.suspects.empty() && stats.suspects.erase(a_event.shape))
				++stats.late;
		}

		void OnDrained(const GrassDrained& a_event)
		{
			// Suspects with no event since the check: the mirror missed or misapplied one.
			auto& stats = ParityState();
			for (auto& [shape, text] : stats.suspects) {
				if (text.starts_with("a captured"))
					++stats.missing;
				else if (text.starts_with("a mirrored"))
					++stats.stale;
				else
					++stats.differ;
				if (stats.first.empty())
					stats.first = std::move(text);
			}
			stats.suspects.clear();
			if (Diagnostics::ParityDue(a_event.frame) && std::exchange(stats.frame, a_event.frame) != a_event.frame)
				CheckParity(a_event.mirror);
		}
	}

	GrassParityState& ParityState()
	{
		static GrassParityState state;
		return state;
	}

	void SubscribeParity()
	{
		Diagnostics::Subscribe<GrassCaptured>(OnCaptured);
		Diagnostics::Subscribe<GrassEventDrained>(OnEventDrained);
		Diagnostics::Subscribe<GrassDrained>(OnDrained);
	}
}
