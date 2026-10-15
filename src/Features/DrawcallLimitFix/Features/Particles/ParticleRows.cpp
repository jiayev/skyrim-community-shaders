// The particle systems' palette rows class (Api::PaletteRowsContribution, FrameValueItemClass::ParticleSystem): their blocks in
// FrameValues' palette part, five rows a particle of their capacity bucket, expanded from the snapshots each frame, and their placement
// rows the snapshots' world and bound; the quads' geometry slot; the frame's cameras.

#include "Features/DrawcallLimitFix/Features/Particles/Particles.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Extensions/PaletteRows.h"

#include <cmath>
#include <cstring>

namespace DCLF::Features::Particles
{
	namespace
	{
		// The frame's input (FrameValues' job, from the kick): the main camera the quads face, and the camera first-person quads face: the
		// main camera's rotation (the first-person camera's, FUN_1414cb020) at the first-person eye (DrawWorld +0x00).
		struct FrameCameras
		{
			ParticleSnapshots::Camera main, firstPerson;
		};

		// A particle system draws the renderer's own quads (FUN_1414f2ad0 type 1: FUN_140e4d280's quad index list and texcoord stream, one
		// quad a particle), vertices pulled from its palette block: one slot per capacity bucket, keyed by a DCLF address.
		bool GeometrySource(Api::PaletteGeometrySource& a_source)
		{
			const auto quads = a_source.units;
			const auto engineQuads = ParticleSnapshots::Get().EngineQuads();
			if (!engineQuads.vertices || !engineQuads.indices)
				return false;
			a_source.key = ParticleSnapshots::QuadSlotKey(quads);
			a_source.vertexBuffer = engineQuads.vertices;
			a_source.indexBuffer = engineQuads.indices;
			a_source.vertexDesc = ParticleSnapshots::kSlotVertexDesc;
			a_source.vertexCount = quads * 4;
			a_source.indexCount = quads * 6;
			return true;
		}

		// The main camera the frame's particle quads face, and the renderer's quads once.
		void FrameStart()
		{
			ParticleSnapshots::Get().BeginFrame();
		}

		void Kick(const Api::PaletteRowsKick& a_kick)
		{
			auto& cameras = a_kick.input.As<FrameCameras>();
			cameras.main = ParticleSnapshots::Get().FrameCamera();
			// The first-person eye (DrawWorld +0x00, filled by FUN_1406452c0 before FrameDriver::BeginFrame): where FUN_1414cb020 places
			// the first-person camera, with the world camera's rotation.
			cameras.firstPerson = cameras.main;
			if (const auto* eye = REL::Relocation<const float*>{ REL::Offset(Engine::Address::kDrawWorldEye) }.get(); eye && cameras.main.valid)
				std::memcpy(cameras.firstPerson.eye, eye, sizeof(cameras.firstPerson.eye));
		}

		// The members' records, followed from now on (ParticleSnapshots::Follow is the producer's alone).
		void BeginRun(const Api::PaletteRowsRun& a_run)
		{
			auto& snapshots = ParticleSnapshots::Get();
			for (auto& item : a_run.items)
				item.state = snapshots.Follow(item.geometry);
		}

		// Particle quads, transforms and bound from the newest snapshot (particle data is never read here).
		void SampleChunk(const Api::PaletteRowsChunk& a_chunk)
		{
			const auto& cameras = a_chunk.input.As<FrameCameras>();
			auto& snapshots = ParticleSnapshots::Get();
			for (auto& write : a_chunk.writes) {
				ParticleSnapshots::Expanded expanded;
				snapshots.Expand(*static_cast<ParticleSnapshots::Record*>(write.state), write.firstPerson ? cameras.firstPerson : cameras.main,
					write.rows / ParticleSnapshots::kRowsPerParticle, write.palette, expanded);
				if (!expanded.snapshot)
					continue;
				std::memcpy(write.world, expanded.world, sizeof(write.world));
				std::memcpy(write.bound, expanded.bound, sizeof(write.bound));
				write.placed = true;
				if (write.firstPerson) {
					const auto& eye = cameras.firstPerson.eye;
					const float dx = expanded.bound[0] - eye[0], dy = expanded.bound[1] - eye[1], dz = expanded.bound[2] - eye[2];
					a_chunk.NoteFirstPerson(expanded.particles, std::sqrt(dx * dx + dy * dy + dz * dz));
				}
			}
		}

		void EndRun(const Api::PaletteRowsRun&)
		{
			ParticleSnapshots::Get().EndRun();
		}
	}

	void RegisterRows(Api::InstallContext& a_context)
	{
		a_context.RegisterPaletteRows({ .name = "particles",
			.itemClass = Api::FrameValueItemClass::ParticleSystem,
			.rule = { .rowsPerUnit = ParticleSnapshots::kRowsPerParticle, .minUnits = ParticleSnapshots::kMinQuads, .maxUnits = ParticleSnapshots::kMaxQuads, .pow2 = true },
			.geometrySource = &GeometrySource,
			.frameStart = &FrameStart,
			.kick = &Kick,
			.beginRun = &BeginRun,
			.sampleChunk = &SampleChunk,
			.endRun = &EndRun });
	}
}
