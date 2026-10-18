#pragma once

// Faces (dclf-architecture.md, "NPC faces: positions published by the engine's own writer"): the
// NPC face shapes (a BSDynamicTriShape under a BSFaceGenNiNode: head, mouth, hair, brows), whose positions DCLF never reads from the
// shape. The feature installs the face morphing's thunks (FaceSnapshots: the heads' snapshots, published by the engine's own writer) and
// registers the coordinator component that follows the face shapes' writes (FaceShapes.cpp; Api::EntryClass::FaceShape, taken while
// the thunks are in): their heads' lists and publications, each written shape's snapshot, its region of the positions buffer and its
// vertex stream row in the tables, and the regions freed as their rows go. And it registers the vertex stream (FacePositions.cpp:
// Api::VertexStreamContribution): the positions buffer as scene resources (its graph registration, requirement and fit) and the commits'
// uploads into it.
//
// Core keeps what every draw of the stream reads, generic: the stream rows in the scene's tables (SceneTables::streamRows, the objects'
// streamRow, kRetiredStreamRow) and the payloads' copies of them, the builds' second-stream geometry draws (StreamGeometry,
// AppendStreamRows) and the passes' bindings of the buffer (SceneBuffers::vertexStream); and the walk's entry classes, which the feature's
// geometry type contribution names a face shape's (Api::GeometryTypeContribution::entry: a dynamic shape under a BSFaceGenNiNode, classified
// every frame with the per-frame entry-class trait, its head held from the attach's pins). Included by the feature's sources only.

#include "Features/DrawcallLimitFix/Api/Feature.h"

namespace DCLF::Features::Faces
{
	// FacesFeature.cpp: the feature's entry (Features/FeatureList.cpp).
	Api::Feature Feature();
	// FaceShapes.cpp: the coordinator component that follows the face shapes' writes (a_taken: whether their objects are DCLF's).
	void RegisterShapes(Api::InstallContext& a_context, bool a_taken);
	// FacePositions.cpp: the vertex stream (the positions buffer's scene resources and uploads), after the component.
	void RegisterPositions(Api::InstallContext& a_context);
}
