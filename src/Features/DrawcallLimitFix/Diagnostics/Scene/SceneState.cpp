// The scene area's diagnostics state, one instance (SceneState.h).

#include "SceneState.h"

namespace DCLF::Diagnostics
{
	SceneState& SceneDiagnostics()
	{
		static SceneState state;
		return state;
	}
}
