// Grass's engine side: the sink on GrassShared's capture (any thread: the cell loaders, the shapes' destructors' callers), the source of
// the feature's grass channel (events onto events through Push, which the render thread's mirror drains: GrassMirror.h). The capture's
// hooks are GrassShared's, shared with Grass Optimizations (src/GrassShared/GrassCapture.h): either registers its sink and installs
// them, and they are installed once.

#include "Features/DrawcallLimitFix/Features/Grass/Grass.h"
#include "Features/DrawcallLimitFix/Features/Grass/GrassMirror.h"
#include "Features/DrawcallLimitFix/Features/Grass/Diagnostics/GrassEvents.h"
#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"

#include "GrassShared/GrassCapture.h"

#include <cstring>
#include <utility>

namespace DCLF::Features::Grass
{
	namespace
	{
		/** @brief What the bucket a shape draws in reads of its material and mesh (the event's fields, which live only during the call). */
		BucketSource SourceOf(const GrassShared::AddEvent& a_add)
		{
			BucketSource source;
			source.diffuse.reset(a_add.diffuse);
			const auto& bound = a_add.modelBound;
			source.boundCenter[0] = bound.center.x;
			source.boundCenter[1] = bound.center.y;
			source.boundCenter[2] = bound.center.z;
			source.modelRadius = bound.radius;
			source.wavePeriod = a_add.wavePeriod;
			source.triangleCount = a_add.triangleCount;
			if (a_add.pbr) {
				const auto& pbr = a_add.pbrData;
				source.normal.reset(pbr.normal);
				source.rmaos.reset(pbr.rmaos);
				source.features0.reset(pbr.features0);
				source.pbrFlags = pbr.flags;
				source.roughnessScale = pbr.roughnessScale;
				source.specularLevel = pbr.specularLevel;
				std::memcpy(source.subsurface, pbr.subsurface, sizeof(source.subsurface));
			}
			return source;
		}

		// A shape's records: copied, with its bucket's key and material and its mesh's buffers (held until the render thread leases them).
		void OnAdd(const GrassShared::AddEvent& a_add)
		{
			Event event{ .kind = Event::Kind::Add, .seq = a_add.seq, .shape = a_add.shape, .instances = std::vector<GrassInstances::Instance>(a_add.count) };
			if (a_add.count)
				std::memcpy(event.instances.data(), a_add.records, a_add.count * sizeof(GrassInstances::Instance));
			event.origin = { a_add.origin.x, a_add.origin.y, a_add.origin.z };
			event.key = { .mesh = a_add.meshKey, .material = a_add.material, .diffuse = a_add.diffuse, .vertexDesc = a_add.vertexDesc, .clampMode = a_add.clampMode,
				.alphaRef = a_add.alphaRef, .pbr = a_add.pbr, .sphereNormal = a_add.sphereNormal, .uniformScale = a_add.uniformScale };
			event.source = SourceOf(a_add);
			event.mesh.key = a_add.meshKey;
			event.mesh.vertexBuffer.copy_from(a_add.vertexBuffer);
			event.mesh.indexBuffer.copy_from(a_add.indexBuffer);
			event.mesh.vertexDesc = a_add.vertexDesc;
			event.mesh.indexCount = a_add.triangleCount * 3;
			Diagnostics::Notify(GrassCaptured{ a_add.shape, a_add.count, a_add.seq, GrassCaptured::Kind::Add });
			Push(std::move(event));
		}

		// Every BSMultiStreamInstanceTriShape's destruction: the drain ignores a shape it does not hold.
		void OnRemove(RE::BSMultiStreamInstanceTriShape* a_shape, std::uint64_t a_seq)
		{
			Diagnostics::Notify(GrassCaptured{ a_shape, 0, a_seq, GrassCaptured::Kind::Remove });
			Push(Event{ .kind = Event::Kind::Remove, .seq = a_seq, .shape = a_shape });
		}

		// A grass shape whose records the capture could not read: the engine draws it, so the family must not withhold grass while it lives.
		void OnRejected(RE::BSMultiStreamInstanceTriShape* a_shape, std::uint64_t a_seq)
		{
			Diagnostics::Notify(GrassCaptured{ a_shape, 0, a_seq, GrassCaptured::Kind::Rejected });
			Push(Event{ .kind = Event::Kind::Rejected, .seq = a_seq, .shape = a_shape });
		}
	}

	void Push(Event&& a_event)
	{
		events.Push(std::move(a_event));
	}

	void InstallHooks()
	{
		// GrassShared holds two sinks (Grass Optimizations' and this), added before its hooks install; installing them again is nothing.
		GrassShared::AddSink({ .add = &OnAdd, .remove = &OnRemove, .rejected = &OnRejected });
		GrassShared::InstallCapture();
		installed = true;
	}
}
