#pragma once

// The vertex stream contract's Core side (Api/VertexStreams.h): the stream's entry, resolved once at InstallFeatures (none registered:
// no stream, and every draw's second stream repeats its first); what its select and upload are handed; and the calls Core makes where
// the scene's tables are made and where the commits upload. The rows are the scene's tables' (SceneTables::streamRows), read as data by
// the builds (Payload/Geometry.h: StreamGeometry, AppendStreamRows).

#include "Features/DrawcallLimitFix/Api/VertexStreams.h"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace org
{
	class VersionedBuffer;
	namespace services
	{
		class NamedVersions;
	}
}

namespace DCLF::Draws
{
	struct SceneBuffers;
	class CommitUploads;
}

namespace DCLF::Api
{
	/** @brief The stream's select (render thread, EnsureSceneBuffers): its resources' state; select answers with buffer. */
	class VertexStreamSelect
	{
	public:
		const std::shared_ptr<void>& state;
		const std::shared_ptr<org::VersionedBuffer>* buffer = nullptr;  // the answer: the state's buffer of the scene's set (null: none)

		template <class T>
		const T& State() const
		{
			return *static_cast<const T*>(state.get());
		}
	};

	/**
	 * @brief The stream's upload (render thread, a commit): what the frame adopted, the payload's rows (as its build copied them from its
	 * tables), the stream's buffer, its resources' state (the commits' own: render thread) and the commit's staged uploads.
	 */
	class VertexStreamUpload
	{
	public:
		const org::services::NamedVersions& adopted;
		std::span<const VertexStreamRow> rows;
		const std::shared_ptr<org::VersionedBuffer>& buffer;
		const std::shared_ptr<void>& state;
		Draws::CommitUploads& uploads;

		template <class T>
		T& State() const
		{
			return *static_cast<T*>(state.get());
		}
	};
}

namespace DCLF::Extensions
{
	/** @brief The stream's entry: its registration (copied, its name owned), and whether there is one. */
	struct VertexStreamEntry
	{
		bool registered = false;
		std::string name;
		Api::VertexStreamContribution contribution;
	};

	/** @brief Main thread, the end of InstallFeatures: the entry from the registration (entryClass Count: none). */
	void ResolveVertexStream(const Api::VertexStreamContribution& a_registered);
	/** @brief Any thread, after InstallFeatures (none before): the stream's entry. */
	const VertexStreamEntry& VertexStream();
	/**
	 * @brief Any thread, after InstallFeatures: whether its objects are DCLF's (registered, its entry class taken): a build embeds the
	 * buffer's address only then (Draws::ResourceAddresses::streamVertices), and the named-address parity checks it.
	 */
	bool VertexStreamTaken();
	/**
	 * @brief The scene resources' call order (SceneResources.cpp): every registration's index, the stream's resources first, the others in
	 * registration order.
	 */
	std::span<const std::uint32_t> SceneResourcesOrder();
	/** @brief Whether a_resources is the stream's (its setup giving no state leaves the scene's tables unmade). */
	bool VertexStreamResources(std::uint32_t a_resources);

	/** @brief Render thread (EnsureSceneBuffers, after the scene resources' setup): the stream's buffer (select), or null without one. */
	std::shared_ptr<org::VersionedBuffer> SelectVertexStream(const std::vector<std::shared_ptr<void>>& a_states);
	/**
	 * @brief Render thread (a commit, first, before the commit hooks): the stream's upload of a_rows into the version a_adopted names of
	 * a_scene's stream buffer. The rows it sent; 0 without a stream.
	 */
	std::uint32_t UploadVertexStream(const org::services::NamedVersions& a_adopted, std::span<const Api::VertexStreamRow> a_rows, const Draws::SceneBuffers& a_scene,
		Draws::CommitUploads& a_uploads);
}
