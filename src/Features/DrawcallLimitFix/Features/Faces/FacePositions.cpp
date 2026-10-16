// Faces' vertex stream (Api::VertexStreamContribution): the face positions buffer as scene resources (made with the scene's tables, the
// vertex stream's first; its graph registration, each revision's requirement from the stream rows and its fit), and the commits' uploads
// of the face shapes' rows into it.

#include "Features/DrawcallLimitFix/Features/Faces/Faces.h"
#include "Features/DrawcallLimitFix/Core/Extensions/PassContext.h"
#include "Features/DrawcallLimitFix/Core/Extensions/SceneResources.h"
#include "Features/DrawcallLimitFix/Core/Extensions/VertexStreams.h"
#include "Features/DrawcallLimitFix/Core/Render/Payload/Uploads.h"
#include "Features/DrawcallLimitFix/Core/Render/Payload/Versioned.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/GpuLayouts.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/SceneBufferSet.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Tables.h"

#include <Resources/Buffers/Buffer.h>
#include <rhi.h>

#include <algorithm>
#include <cstdint>
#include <memory>

namespace DCLF::Features::Faces
{
	namespace
	{
		// The positions buffer's first capacity in vertices, doubled by the scene's set to what the stream rows need (Require).
		constexpr std::uint32_t kInitialFaceVertices = 1u << 18;

		/** @brief The face regions a positions version holds (render thread): region -> snapshot generation, for the version filled. */
		struct FaceUploads
		{
			org::services::FilledVersion version;
			ankerl::unordered_dense::map<std::uint32_t, std::uint64_t> regions;
		};

		/**
		 * @brief The positions buffer (Api::SceneResources' state, render thread but for the handle, made once at setup): a float4 per
		 * vertex of each drawn face shape, in its region (its stream row's range), and what the commits uploaded into its versions.
		 */
		struct FacePositions
		{
			Draws::Versioned positions;
			FaceUploads uploaded;
		};

		/** @brief A device-local buffer the shaders read by device address. */
		std::shared_ptr<org::Buffer> DeviceBuffer(std::uint64_t a_bytes, const char* a_name)
		{
			auto created = org::Buffer::CreateShared(rhi::HeapType::DeviceLocal, a_bytes, false);
			created->SetName(a_name);
			return created;
		}

		std::uint64_t AddressOf(rhi::Device a_device, org::Buffer& a_buffer)
		{
			return a_device.GetBufferDeviceAddress({ a_buffer.GetAPIResource().GetHandle(), 0 });
		}

		// The scene's tables are made (the first frame with a device), the vertex stream's first. Face positions are a vertex buffer; its
		// address and capacity are the sealed set's. Without its device address there is none, and the scene's tables are not made.
		std::shared_ptr<void> Setup(const Api::SceneResourcesSetup& a_setup)
		{
			auto state = std::make_shared<FacePositions>();
			state->positions = Draws::MakeVersioned(DeviceBuffer(std::uint64_t(a_setup.smallStart ? 1024u : kInitialFaceVertices) * Draws::kStreamVertexBytes,
														"cs.dclf.face-positions"),
				{ .extent = { 0, Draws::kStreamVertexBytes, 0 } });
			if (!AddressOf(a_setup.device, *state->positions->Get()))
				return nullptr;
			return state;
		}

		void RegisterGraph(const Api::SceneResourcesGraph& a_graph)
		{
			Draws::Register(a_graph.graph, org::ResourceIdentifier("cs.dclf.face-positions"), a_graph.State<FacePositions>().positions);
		}

		// Every face shape's region of the publication's tables.
		void Require(const Api::SceneResourcesRequirements& a_requirements)
		{
			std::uint64_t faceVertices = 0;
			for (const auto& stream : a_requirements.tables.streamRows)
				if (stream.object != Api::VertexStreamRow::kNoObject)
					faceVertices = std::max<std::uint64_t>(faceVertices, std::uint64_t(stream.first) + stream.count);
			Draws::SceneBufferSet::Get().Require(a_requirements.build, a_requirements.State<FacePositions>().positions, faceVertices);
		}

		// A face shape whose region is past the version's vertices waits (ObjectFits).
		void Fit(Api::SceneResourcesFit& a_fit)
		{
			a_fit.fit.streamVertices = static_cast<std::uint32_t>(std::min<std::uint64_t>(Draws::NamedUnits(a_fit.sealed, a_fit.State<FacePositions>().positions), UINT32_MAX));
		}

		void Select(Api::VertexStreamSelect& a_select)
		{
			a_select.buffer = &a_select.State<FacePositions>().positions;
		}

		// A commit's face uploads (render thread): a region at a time, and only when the head's snapshot is not the one the version the
		// adopted set names holds (a version it did not fill holds none). The snapshot stays the walk's until its next walk, which is after
		// the commit. A region past that version's vertices waits for the requirement that grows it.
		std::uint32_t Upload(const Api::VertexStreamUpload& a_upload)
		{
			auto& uploaded = a_upload.State<FacePositions>().uploaded;
			const auto& version = a_upload.adopted.Of(a_upload.buffer);
			if (!uploaded.version.Holds(version)) {
				uploaded.regions.clear();
				uploaded.version.Filled(version);
			}
			std::uint32_t count = 0;
			for (const auto& stream : a_upload.rows) {
				if (stream.object == Api::VertexStreamRow::kNoObject || std::uint64_t(stream.first) + stream.count > version.units)
					continue;
				auto& region = uploaded.regions[stream.first];
				if (region == stream.generation)
					continue;
				a_upload.uploads(a_upload.buffer, stream.vertices, std::size_t(stream.count) * Draws::kStreamVertexBytes, std::uint64_t(stream.first) * Draws::kStreamVertexBytes);
				region = stream.generation;
				++count;
			}
			return count;
		}
	}

	void RegisterPositions(Api::InstallContext& a_context)
	{
		const auto resources = a_context.RegisterSceneResources({ .name = "face-positions", .setup = &Setup, .registerGraph = &RegisterGraph, .require = &Require, .fit = &Fit });
		// Named as the named-address parity names it.
		a_context.RegisterVertexStream({ .name = "face positions", .entryClass = Api::EntryClass::FaceShape, .resources = resources, .select = &Select, .upload = &Upload });
	}
}
