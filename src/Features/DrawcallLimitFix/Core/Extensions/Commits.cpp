// The registered commit hooks' calls (see the header).

#include "Features/DrawcallLimitFix/Core/Extensions/Commits.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Registry.h"

namespace DCLF::Extensions
{
	void RunCommitHooks(const Api::CommitContext& a_commit)
	{
		for (const auto& hook : RegisteredCommitHooks())
			hook.commit(a_commit);
	}
}
