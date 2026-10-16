// Texture transforms: the materials' TexcoordOffset as the frame's shaders read it, patched into the installed payload's material rows
// every frame (the rows hold their records' captured value). The list of rows is a payload product, the frame's values a frame-value
// pool, the patch a pass first in the depth segment.

#include "Features/DrawcallLimitFix/Features/TextureTransforms/TextureTransforms.h"

namespace DCLF::Features::TextureTransforms
{
	namespace
	{
		void Install(Api::InstallContext& a_context)
		{
			RegisterPool(a_context);
			RegisterPass(a_context);
			RegisterProduct(a_context);
			// Together they write every material row's TexcoordOffset (the ports' frame VS variable) every frame: a capture or controller
			// write that moved only the texture transforms leaves the records as they are.
			a_context.RegisterFrameVSVariablePatch();
		}
	}

	Api::Feature Feature()
	{
		return { "texture-transforms", &Install };
	}
}
