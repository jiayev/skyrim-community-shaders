// Faces: the NPC face shapes, drawn from their heads' snapshots. The face morphing's thunks, the coordinator component that follows
// the face shapes' writes, and the vertex stream their positions are drawn from.

#include "Features/DrawcallLimitFix/Features/Faces/Faces.h"
#include "Features/DrawcallLimitFix/Features/Faces/FaceSnapshots.h"

namespace DCLF::Features::Faces
{
	namespace
	{
		void Install(Api::InstallContext& a_context)
		{
			// The face morphing's thunks (AE only, checked first): the face shapes are DCLF's only once they are in. The component follows
			// their writes either way (their heads' lists).
			FaceSnapshots::Get().Install();
			RegisterShapes(a_context, FaceSnapshots::Enabled());
			// The positions buffer, set up first among the scene resources whatever the list's order (Api/VertexStreams.h).
			RegisterPositions(a_context);
		}
	}

	Api::Feature Feature()
	{
		return { "faces", &Install };
	}
}
