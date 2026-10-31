#pragma once

#include "Features/GrassOptimizations/GrassMeshLibrary.h"

/**
 * The grass mesh library both consumers share: Grass Optimizations' GrassMeshLibrary (mesh records by source .nif stem, their
 * LOD meshes loaded off-thread), one instance, and each grass shape's mesh record as LoadGrassType named it. The library's LOD
 * state is the render thread's: either consumer ensures, commits and reads LODs there, and holds a record's activeBuckets
 * while it draws from its LODs (GrassMeshLibrary::ReleaseMesh evicts them at zero).
 */
namespace GrassShared
{
	/** @brief The one library. */
	::GrassMeshLibrary& MeshLibrary();

	/** @brief A grass shape's mesh record, from LoadGrassType; null before it, for a shape it never named, or once the shape is destroyed. Any thread. */
	::GrassMeshLibrary::MeshRecord* MeshOf(const RE::BSMultiStreamInstanceTriShape* shape);

	/** @brief The capture's: a shape's mesh record (LoadGrassType; true when it is new for the shape), and its end (the shape's destructor, before the address can be reused). */
	bool RecordMesh(const RE::BSMultiStreamInstanceTriShape* shape, ::GrassMeshLibrary::MeshRecord* mesh);
	void ForgetMesh(const RE::BSMultiStreamInstanceTriShape* shape);
}
