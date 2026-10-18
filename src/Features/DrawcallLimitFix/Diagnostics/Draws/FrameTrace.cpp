// The frame trace (CS_DCLF_FRAME_TRACE), the corrupt-frame tool: every frame, one line per point of what it drew with - the snapshot it
// adopted (its set, its payload's parts), its epoch coverage, each main epoch's outcome, the colour epoch's culling counts and the
// retirement point it bound against the clock - each "[DCLF] frame trace: frame N ...", with F10 logged as the user's marker. A line
// that is not the steady state ("submitted", every epoch covered) carries its reason; a binding the clock had already passed, or a point
// below the last, is flagged with `<-`. tools/dclf-corrupt-frames.py maps the corrupt captures of tools/dclf-capture-burst.ps1 to these
// lines by their timestamps.

#include "DrawsView.h"
#include "Events.h"
#include "Observers.h"

#include <string>

namespace DCLF::Diagnostics
{
	namespace
	{
		/** @brief Render thread: the last frame's bound point, and whether F10 was down at it. */
		struct FrameTraceState
		{
			std::uint64_t lastPoint = 0;
			bool markDown = false;
		};
		FrameTraceState& State()
		{
			static FrameTraceState state;
			return state;
		}
	}

	void TraceSnapshotAdopted(const SnapshotAdopted& a_adopted)
	{
		std::string parts;
		if (const auto* p = a_adopted.payload)
			for (std::size_t k = 0; k < Draws::kPayloadParts; ++k)
				parts += fmt::format(" p{}[srv {} units {} bytes {}{}{}]", k, p->parts[k].srvIndex, p->parts[k].units, p->bytes[k], p->allocated[k] ? " NEW" : "",
					p->shared[k] ? " shared" : "");
		logger::info("[DCLF] frame trace: frame {} adopts snapshot {} (versions {}, set {:p}{}), payload {} ready {}{}", a_adopted.frame, a_adopted.snapshot,
			a_adopted.set ? "yes" : "none", static_cast<const void*>(a_adopted.set), a_adopted.growth ? " GROWTH" : "", a_adopted.payload ? a_adopted.payload->serial : 0,
			a_adopted.payload ? a_adopted.payload->Ready() : false, parts);
	}

	void TraceEpochCoverage(const EpochCoverageDecided& a_decided)
	{
		std::string bits;
		bits.reserve(a_decided.covered.size());
		for (const bool covered : a_decided.covered)
			bits += covered ? '1' : '0';
		logger::info("[DCLF] frame trace: frame {} coverage {} (snapshot {} drawsComplete {} stale {} payload {} ready {} versions {} current {})", a_decided.frame, bits,
			a_decided.snapshot, a_decided.drawsComplete, a_decided.stale, a_decided.payload ? a_decided.payload->serial : 0, a_decided.payload && a_decided.payload->Ready(),
			a_decided.versions ? "yes" : "none", a_decided.current);
	}

	void TraceMainEpoch(const MainEpochOutcome& a_outcome)
	{
		const char* epoch = a_outcome.depthOnly ? "Z-prepass" : "colour";
		if (!a_outcome.executed) {
			logger::info("[DCLF] frame trace: frame {} {} epoch: {} (set {})", a_outcome.frame, epoch, a_outcome.outcome, a_outcome.notDrawingYet ? "not drawing yet" : "CLAIMS INSTALLED");
			return;
		}
		const auto* payload = a_outcome.payload;
		logger::info("[DCLF] frame trace: frame {} {} epoch: {} (payload {}, inputs {}, snapshot {})", a_outcome.frame, epoch, a_outcome.outcome, payload ? payload->serial : 0,
			payload ? (*payload)[Draws::PayloadPart::Inputs].units : 0, a_outcome.snapshot);
	}

	void TraceAdoptedBound(const AdoptedBound& a_bound)
	{
		auto& state = State();
		logger::info("[DCLF] frame trace: frame {} binds point {} (clock {}){}{}", a_bound.frame, a_bound.point, a_bound.completed,
			a_bound.point <= a_bound.completed ? " <- BOUND AT A RETIRED POINT" : "", a_bound.point < state.lastPoint ? " <- POINT WENT BACK" : "");
		state.lastPoint = a_bound.point;
		// F10: the user saw a corrupt frame (its edge, once per press).
		const bool down = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
		if (down && !state.markDown)
			logger::info("[DCLF] frame trace: ===== USER MARK (F10) at frame {} =====", a_bound.frame);
		state.markDown = down;
	}

	void TraceColourCounts(const Draws::CounterReadback& a_readback, std::uint32_t a_slot)
	{
		// The slot's last colour execution's words (the host has waited for it), its first source's: whatever execution they are of,
		// named by the stamp they carry.
		if (a_slot >= a_readback.buffers.size())
			return;
		auto resource = a_readback.buffers[a_slot]->GetAPIResource();
		void* mapped = nullptr;
		resource.Map(&mapped);
		if (!mapped)
			return;
		const auto* w = static_cast<const std::uint32_t*>(mapped);
		logger::info("[DCLF] frame trace: colour exec stamp {} (slot {}): drawn {} rejected {} tested {} occluded {} phase2 drawn {} rescued {}", w[Draws::kCountWords],
			a_slot, w[0], w[1], w[2], w[6], w[17], w[18]);
		resource.Unmap(0, 0);
	}
}
