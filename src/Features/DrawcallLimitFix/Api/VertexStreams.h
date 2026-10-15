#pragma once

// The vertex stream contract (Api::InstallContext::RegisterVertexStream): the second vertex stream of an object's draws, where the
// engine's own draws take a shape's per-frame positions (BSDynamicTriShape::dynamicData) - a float4 per vertex of each stream object, in
// its range of a versioned buffer the feature's scene resources hold. Core keeps the stream's rows in the scene's tables
// (SceneTables::streamRows, the objects' streamRow: a row per stream object, written by the coordinator component that follows the entry
// class), and reads them as data: its builds give each row a geometry draw after the geometry slots and name it as the object's second
// stream (an object of the class without one waits), its passes bind the buffer as a vertex buffer, and its commits hand the payload's
// rows to the feature's upload. One stream: Core's draws have one second-stream slot (Features/Faces' face positions).

#include "Features/DrawcallLimitFix/Api/ObjectContributors.h"
#include "Features/DrawcallLimitFix/Api/SceneResources.h"

#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace DCLF::Api
{
	// Defined by Core (Core/Extensions/VertexStreams.h), which the features inside DCLF include for them.
	class VertexStreamSelect;
	class VertexStreamUpload;

	/**
	 * @brief A row of the stream (SceneTables::streamRows; a payload's copy for its commit's uploads): an object's vertices, as its
	 * entry's component resolved them, and their range of the stream's buffer.
	 */
	struct VertexStreamRow
	{
		static constexpr std::uint32_t kNoObject = ~0u;
		static constexpr std::uint32_t kNoFirst = ~0u;
		std::uint32_t object = kNoObject;  // the object slot whose draws read it; kNoObject: a free row (a geometry draw nothing names)
		std::uint32_t first = kNoFirst;    // its first vertex in the stream's buffer
		std::uint32_t count = 0;           // its vertices
		std::uint64_t generation = 0;      // its vertices' version: uploaded on a new one
		const float* vertices = nullptr;   // float4 per vertex, valid while owner is held
		std::shared_ptr<const std::vector<float>> owner;
		// The feature's, carried with the row (Features/Faces: its head's complete capture, for a future consistency-group publication);
		// Core reads none.
		std::shared_ptr<const void> group;
		// The entry the row is of (its BSGeometry), as a key: the row's release names it (SceneTables::streamRowsReleased).
		const void* source = nullptr;
	};

	/**
	 * @brief The vertex stream. Read as data where Core decides (the builds, the frame's addresses, the declarations); its callbacks run at
	 * the scene's setup and in the commits, never per object.
	 *
	 * - name: the named-address parity's name for its buffer's address (a build embeds it while the entry class is taken).
	 * - entryClass: the class whose objects draw from it (registered: a component follows it). Their records wait for their row; the
	 *   builds embed the buffer's address only while the class is taken (Extensions::EntryClassTaken).
	 * - resources: the scene resources whose state holds the buffer. Core sets them up, registers them with a graph and requires them
	 *   first, ahead of every other registration's (the buffer is what every draw of the stream binds); their setup giving no state leaves
	 *   the scene's tables unmade (EnsureSceneBuffers fails, and tries again next frame).
	 * - select: render thread, once, right after the resources' setup: the versioned buffer of the state the stream's rows are in
	 *   (VertexStreamSelect::buffer), which Core keeps with the scene's tables (SceneBuffers::vertexStream) for every pass that binds it.
	 * - upload: render thread, in each commit (depth, colour, shadow), first, before the commit hooks: the payload's rows into the version
	 *   the adopted set names. Returns the rows it sent (the shadow commit's: the report's "face regions uploaded").
	 */
	struct VertexStreamContribution
	{
		std::string_view name;
		EntryClass entryClass = EntryClass::Count;
		SceneResourcesId resources;
		void (*select)(VertexStreamSelect& a_select) = nullptr;
		std::uint32_t (*upload)(const VertexStreamUpload& a_upload) = nullptr;
	};
}
