// Texture transforms: the materials' TexcoordOffset as the frame's shaders read it, patched into the installed payload's material rows
// every frame (the rows hold their records' captured value). The list of rows is a payload product, the frame's values a frame-value
// pool, the patch a GPU stage first in the depth segment.

#include "Features/DrawcallLimitFix/Features/TextureTransforms/TextureTransforms.h"

namespace DCLF::Features::TextureTransforms
{
	namespace
	{
		void Install(Api::InstallContext& a_context)
		{
			RegisterPool(a_context);
			RegisterStage(a_context);
			RegisterProduct(a_context);
		}
	}

	Api::Feature Feature()
	{
		return { "texture-transforms", &Install };
	}
}
