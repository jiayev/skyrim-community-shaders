#include "GrassMeshLibrary.h"

namespace GrassShared
{
	namespace
	{
		// Written by the loader threads' LoadGrassType and the dtor's caller, read by either consumer.
		std::mutex meshMutex;
		std::unordered_map<const RE::BSMultiStreamInstanceTriShape*, ::GrassMeshLibrary::MeshRecord*> meshByShape;
	}

	::GrassMeshLibrary& MeshLibrary()
	{
		static ::GrassMeshLibrary library;
		return library;
	}

	::GrassMeshLibrary::MeshRecord* MeshOf(const RE::BSMultiStreamInstanceTriShape* shape)
	{
		std::scoped_lock lock(meshMutex);
		const auto it = meshByShape.find(shape);
		return it == meshByShape.end() ? nullptr : it->second;
	}

	bool RecordMesh(const RE::BSMultiStreamInstanceTriShape* shape, ::GrassMeshLibrary::MeshRecord* mesh)
	{
		std::scoped_lock lock(meshMutex);
		auto& entry = meshByShape[shape];
		return std::exchange(entry, mesh) != mesh;
	}

	void ForgetMesh(const RE::BSMultiStreamInstanceTriShape* shape)
	{
		std::scoped_lock lock(meshMutex);
		meshByShape.erase(shape);
	}
}
