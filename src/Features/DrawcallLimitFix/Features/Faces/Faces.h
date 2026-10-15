#pragma once

// Faces (drawcall-limit-fix.md, "NPC faces: positions published by the engine's own writer" and "NPC face parts in the main pass"): the
// NPC face shapes (a BSDynamicTriShape under a BSFaceGenNiNode: head, mouth, hair, brows), whose positions DCLF never reads from the
// shape. The feature installs the face morphing's thunks (FaceSnapshots: the heads' snapshots, published by the engine's own writer) and
// registers the coordinator component that follows the face shapes' writes (FaceShapes.cpp; Api::EntryClass::FaceShape, taken while
// the thunks are in): their heads' lists and publications, each written shape's snapshot, its region of the positions buffer and its
// stream in the tables, and the regions freed as their streams go.
//
// Core keeps what every draw reads: the face streams in the scene's tables (SceneTables::faceStreams, the objects' faceStream), the
// positions buffer (SceneBuffers::facePositions) and the commits' uploads into it, the builds' second-stream geometry draws and the
// passes' bindings; and the walk's classification of a face shape (Tracked::faceShape, its head held from the attach's pins, the per-frame
// face trait). Included by the feature's sources only.

#include "Features/DrawcallLimitFix/Api/Feature.h"

namespace DCLF::Features::Faces
{
	// FacesFeature.cpp: the feature's entry (Features/FeatureList.cpp).
	Api::Feature Feature();
	// FaceShapes.cpp: the coordinator component that follows the face shapes' writes (a_taken: whether their objects are DCLF's).
	void RegisterShapes(Api::InstallContext& a_context, bool a_taken);
}
